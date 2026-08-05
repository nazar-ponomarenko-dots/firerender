// ============================================================================
//  renderer.c — оркестрация рендера кадра (см. renderer.h)
// ----------------------------------------------------------------------------
//  Вынесено из main.c: обход дерева сцены с профилированием, каскад
//  постобработки (сцена -> bloom -> воксельный туман) и 2D-оверлеи
//  (миникарта, HUD, прицел, сообщение карты, панели отладки).
// ============================================================================
#include "renderer.h"

#include "app.h"
#include "audio.h"
#include "bloom.h"
#include "camera.h"
#include "console.h"
#include "cvars.h"
#include "game.h"
#include "gl_loader.h"
#include "hud.h"
#include "linalg.h"
#include "mapdata.h"
#include "profiler.h"
#include "sky.h"
#include "voxfog.h"
#include "world.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// FBO сцены и тумана: создаются при загрузке мира (renderer_setup)
static BloomParams s_bloom;
static int s_active = 0;       // мир загружен: FBO существуют
static int s_wire = 0;         // каркасный режим (cvar r_wireframe)

// --- Рендер-контекст: единое кадровое состояние сцены --------------------------
static RendererContext s_rc;

const RendererContext *renderer_context(void)
{
    return &s_rc;
}

// Собирает кадровое состояние сцены (камера, свет, туман, тени) — ОДИН раз
// за кадр, до рендера узлов. Матрицы каскадов обновляет shadow.c в конце
// shadow_render (свежие для vegetation/objects в том же кадре).
static void renderer_ctx_fill(Game *g)
{
    RendererContext *rc = &s_rc;
    memset(rc, 0, sizeof *rc);

    // Камера
    if (g->cam) {
        const Camera *cam = &g->cam->cam;
        memcpy(rc->viewproj, g->cam->viewproj, sizeof rc->viewproj);
        memcpy(rc->cam_pos, cam->pos, sizeof rc->cam_pos);
        rc->cam_yaw = cam->yaw;
        rc->cam_pitch = cam->pitch;
        rc->cam_fov_deg = g->cam->fov_deg;
        rc->cam_aspect = g->cam->aspect;
        rc->cam_vy = cam->vy;
        rc->cam_fly = cam->fly ? 1 : 0;
        rc->znear = g->cam->znear;
        rc->zfar = g->cam->zfar;
        camera_dirs(cam, rc->cam_fwd, rc->cam_right);
        vec3_cross(rc->cam_right, rc->cam_fwd, rc->cam_up);
        vec3_normalize(rc->cam_up);
    }
    // Свет: направленные источники (0 = солнце)
    if (g->sun_node) {
        const SunParams *sun = g->sun_node->params;
        int n = sun->light_count > SUN_MAX_LIGHTS ? SUN_MAX_LIGHTS : sun->light_count;
        for (int i = 0; i < n; i++) {
            memcpy(rc->light_dir[i], sun->light_dir[i], 3 * sizeof(float));
            memcpy(rc->light_color[i], sun->light_color[i], 3 * sizeof(float));
            rc->light_intensity[i] = sun->light_intensity[i];
        }
        rc->light_count = n;
    }
    // Окружение и туман — унифицированы для всех материалов сцены
    rc->ambient[0] = 0.75f; rc->ambient[1] = 0.80f; rc->ambient[2] = 0.90f;
    rc->ground_color[0] = 0.22f; rc->ground_color[1] = 0.30f; rc->ground_color[2] = 0.18f;
    sky_fog_color(rc->sky_color);
    memcpy(rc->fog_color, rc->sky_color, sizeof rc->fog_color);
    {
        MapTerrain tc;
        mapdata_terrain(&tc);
        rc->fog_start = g->world.world_w * tc.fog_start;
        rc->fog_end = g->world.world_w * tc.fog_end;
    }
    rc->fog_on = cvar_get("r_fog_mode") > 0.5f;
    // Каскадные тени: текстуры и производные (матрицы свежит shadow.c)
    {
        const float boxes[SHADOW_CASCADES] = {
            SHADOW_BOX_0, SHADOW_BOX_1, SHADOW_BOX_2, SHADOW_BOX_3
        };
        int sun_shadow = 0;
        float bias = 4.0f, bias_veg = 2.0f;
        if (g->sun_node) {
            const SunParams *sun = g->sun_node->params;
            sun_shadow = sun->shadow_on;
            bias = sun->shadow_bias;
            bias_veg = sun->shadow_bias_veg;
        }
        for (int c = 0; c < SHADOW_CASCADES; c++) {
            rc->shadow_tex[c] = shadow_texture(c);
            memcpy(rc->shadow_matrix[c], shadow_matrix(c),
                   sizeof rc->shadow_matrix[c]);
            rc->shadow_box[c] = boxes[c];
            rc->shadow_depth[c] = boxes[c] * SHADOW_DEPTH_MUL + 400.0f;
            // bias рельефа: shadow_bias латеральных текселей (из map.json),
            // на дальнем каскаде — 1/4 (иначе тень отрывается от гор)
            float bias_world = (c == 3 ? 0.5f : 1.0f) * bias * boxes[c] / g_shadow_size[c];
            rc->shadow_bias[c] = bias_world / rc->shadow_depth[c];
            rc->shadow_grad[c] = 10.0f / rc->shadow_depth[c];
            // bias растительности: shadow_bias_veg текселей (в 2 раза меньше)
            float bias_world_v = bias_veg * boxes[c] / g_shadow_size[c];
            rc->shadow_bias_veg[c] = bias_world_v / rc->shadow_depth[c];
            rc->shadow_grad_veg[c] = 4.5f / rc->shadow_depth[c];
        }
        rc->shadow_on = shadow_is_on() && sun_shadow;
    }
}

// --- Общий uniform-набор сцены --------------------------------------------------
// Локация по имени; теневые матрицы/bias/grad объявлены по-разному:
// рельеф — uShadowMatrix0..3, растительность/объекты — массив uShadowMatrix[4].
static GLint su_loc2(GLuint prog, const char *a, const char *b)
{
    GLint l = glGetUniformLocation(prog, a);
    if (l < 0) l = glGetUniformLocation(prog, b);
    return l;
}

void scene_uniforms_init(SceneUniforms *su, GLuint prog)
{
    memset(su, 0, sizeof *su);
    su->u_viewproj = glGetUniformLocation(prog, "uViewProj");
    su->u_campos   = glGetUniformLocation(prog, "uCamPos");
    su->u_lightdir = glGetUniformLocation(prog, "uLightDir");
    su->u_lightcolor = glGetUniformLocation(prog, "uLightColor");
    su->u_lightint = glGetUniformLocation(prog, "uLightIntensity");
    su->u_lightcount = glGetUniformLocation(prog, "uLightCount");
    su->u_ambient  = glGetUniformLocation(prog, "uAmbient");
    su->u_skycolor = glGetUniformLocation(prog, "uSkyColor");
    su->u_groundcolor = glGetUniformLocation(prog, "uGroundColor");
    su->u_fogcolor = glGetUniformLocation(prog, "uFogColor");
    su->u_fogstart = glGetUniformLocation(prog, "uFogStart");
    su->u_fogend   = glGetUniformLocation(prog, "uFogEnd");
    su->u_fogon    = glGetUniformLocation(prog, "uFogOn");
    su->u_shadow_on = glGetUniformLocation(prog, "uShadowOn");
    for (int c = 0; c < SHADOW_CASCADES; c++) {
        char a[64], b[64];
        snprintf(a, sizeof a, "uShadowMap%d", c);
        su->u_shadowmap[c] = glGetUniformLocation(prog, a);
        snprintf(a, sizeof a, "uShadowMatrix%d", c);
        snprintf(b, sizeof b, "uShadowMatrix[%d]", c);
        su->u_shadowmatrix[c] = su_loc2(prog, a, b);
        snprintf(a, sizeof a, "uShadowBias%d", c);
        snprintf(b, sizeof b, "uShadowBias[%d]", c);
        su->u_shadowbias[c] = su_loc2(prog, a, b);
        snprintf(a, sizeof a, "uShadowDepth%d", c);
        su->u_shadowdepth[c] = glGetUniformLocation(prog, a);
        snprintf(a, sizeof a, "uShadowGrad%d", c);
        snprintf(b, sizeof b, "uShadowGrad[%d]", c);
        su->u_shadowgrad[c] = su_loc2(prog, a, b);
    }
}

// Шлёт общий набор из контекста. bias/градиент — базовые (рельефные);
// растительность/объекты переопределяют их своими значениями после вызова.
void scene_uniforms_bind(const SceneUniforms *su, const RendererContext *rc)
{
    if (su->u_viewproj >= 0)
        glUniformMatrix4fv(su->u_viewproj, 1, GL_FALSE, rc->viewproj);
    if (su->u_campos >= 0)
        glUniform3f(su->u_campos, rc->cam_pos[0], rc->cam_pos[1], rc->cam_pos[2]);
    if (su->u_lightdir >= 0)
        glUniform3fv(su->u_lightdir, SUN_MAX_LIGHTS, &rc->light_dir[0][0]);
    if (su->u_lightcolor >= 0)
        glUniform3fv(su->u_lightcolor, SUN_MAX_LIGHTS, &rc->light_color[0][0]);
    if (su->u_lightint >= 0)
        glUniform1fv(su->u_lightint, SUN_MAX_LIGHTS, rc->light_intensity);
    if (su->u_lightcount >= 0) glUniform1i(su->u_lightcount, rc->light_count);
    if (su->u_ambient >= 0)
        glUniform3f(su->u_ambient, rc->ambient[0], rc->ambient[1], rc->ambient[2]);
    if (su->u_skycolor >= 0)
        glUniform3f(su->u_skycolor, rc->sky_color[0], rc->sky_color[1], rc->sky_color[2]);
    if (su->u_groundcolor >= 0)
        glUniform3f(su->u_groundcolor, rc->ground_color[0], rc->ground_color[1], rc->ground_color[2]);
    if (su->u_fogcolor >= 0)
        glUniform3f(su->u_fogcolor, rc->fog_color[0], rc->fog_color[1], rc->fog_color[2]);
    if (su->u_fogstart >= 0) glUniform1f(su->u_fogstart, rc->fog_start);
    if (su->u_fogend >= 0)   glUniform1f(su->u_fogend, rc->fog_end);
    if (su->u_fogon >= 0)    glUniform1f(su->u_fogon, rc->fog_on ? 1.0f : 0.0f);
    if (su->u_shadow_on >= 0)
        glUniform1f(su->u_shadow_on, rc->shadow_on ? 1.0f : 0.0f);
    for (int c = 0; c < SHADOW_CASCADES; c++) {
        // Каскады на текстурных юнитах 3..6 (семплеры назначены при init)
        if (su->u_shadowmap[c] >= 0 && rc->shadow_tex[c]) {
            glActiveTexture(GL_TEXTURE3 + c);
            glBindTexture(GL_TEXTURE_2D, rc->shadow_tex[c]);
        }
        if (su->u_shadowmatrix[c] >= 0)
            glUniformMatrix4fv(su->u_shadowmatrix[c], 1, GL_FALSE,
                               rc->shadow_matrix[c]);
        if (su->u_shadowbias[c] >= 0)
            glUniform1f(su->u_shadowbias[c], rc->shadow_bias[c]);
        if (su->u_shadowgrad[c] >= 0)
            glUniform1f(su->u_shadowgrad[c], rc->shadow_grad[c]);
    }
    glActiveTexture(GL_TEXTURE0);
}

void renderer_setup(const WorldState *w, int vw, int vh)
{
    memset(&s_bloom, 0, sizeof s_bloom);
    bloom_init(&s_bloom, vw, vh);
    voxfog_init(w, vw, vh);
    s_active = 1;
    // Каркасный режим из config.cfg: sync_cvars срабатывает на изменение.
    s_wire = cvar_get("r_wireframe") > 0.5f;
}

void renderer_resize(int vw, int vh)
{
    if (!s_active) return;
    bloom_resize(&s_bloom, vw, vh);
    voxfog_resize(vw, vh);
}

void renderer_teardown(void)
{
    if (!s_active) return;
    bloom_cleanup(&s_bloom);
    voxfog_cleanup();
    s_active = 0;
}

void renderer_set_wireframe(int on)
{
    s_wire = on ? 1 : 0;
}

// --- Обход дерева сцены --------------------------------------------------------
// Рендер части дерева: 0 = 3D-сцена (в FBO bloom'а), 1 = UI (миникарта и
// HUD — поверх композита, в bloom не попадают). Каждый узел — с таймером
// профилирования (profiler).
static ProfSlot node_slot(const char *name)
{
    if (strcmp(name, "Sky") == 0) return PROF_SKY;
    if (strcmp(name, "Terrain") == 0) return PROF_TERRAIN;
    if (strcmp(name, "Vegetation") == 0) return PROF_VEG;
    if (strcmp(name, "Objects") == 0) return PROF_OBJECTS;
    if (strcmp(name, "Fauna") == 0) return PROF_FAUNA;
    if (strcmp(name, "FieldArea") == 0) return PROF_FIELD;
    if (strcmp(name, "Selection") == 0) return PROF_SEL;
    if (strcmp(name, "Minimap") == 0) return PROF_MM;
    if (strcmp(name, "DebugHUD") == 0) return PROF_HUD;
    return PROF_TERRAIN;   // неизвестный узел — в общий бюджет рельефа
}

static void render_part(SceneNode *world, float dt, int ui)
{
    for (SceneNode *c = world->first_child; c; c = c->next) {
        if (!c->render) continue;
        int is_ui = (strcmp(c->name, "Minimap") == 0) ||
                    (strcmp(c->name, "DebugHUD") == 0);
        if (is_ui != ui) continue;
        ProfSlot slot = node_slot(c->name);
        Uint64 t0 = SDL_GetPerformanceCounter();
        prof_gpu_begin(slot);
        c->render(c, dt);
        prof_gpu_end(slot);
        Uint64 t1 = SDL_GetPerformanceCounter();
        float ms = (float)(t1 - t0) * 1000.0f /
                   (float)SDL_GetPerformanceFrequency();
        prof_add_ms(slot, ms);
    }
}

static void render_3d(SceneNode *world, float dt)
{
    render_part(world, dt, 0);
}

static void render_ui(SceneNode *world, float dt)
{
    render_part(world, dt, 1);
}

// --- Кадр -----------------------------------------------------------------------
void renderer_begin(void)
{
    // профайлер: обнулить тайминги и счётчики на кадр
    prof_reset();
    float clear_rgb[3] = { 0.6f, 0.65f, 0.7f };
    if (s_active) sky_fog_color(clear_rgb);
    glBindFramebuffer(GL_FRAMEBUFFER, s_active ? bloom_scene_fbo(&s_bloom) : 0);
    glViewport(0, 0, g_vw, g_vh);
    glClearColor(clear_rgb[0], clear_rgb[1], clear_rgb[2], 1.0f);
    glDepthMask(GL_TRUE);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void renderer_frame(Game *g, float dt, int draw_3d)
{
    renderer_ctx_fill(g);        // единый контекст сцены на этот кадр
    SceneNode *world = g->root;
    int did_3d = 0;
    // В игре и в паузе — рисуем сцену (пауза затемняет её меню).
    // В главном меню с фоткой-фоном сцену не рисуем вовсе (draw_3d = 0).
    if (world && draw_3d) {
#ifndef USE_GLES_PATH
        glPolygonMode(GL_FRONT_AND_BACK, s_wire ? GL_LINE : GL_FILL);
#endif
        render_3d(world, dt);
#ifndef USE_GLES_PATH
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
#endif
        did_3d = 1;
    }
    if (did_3d) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, g_vw, g_vh);
        Uint64 pf = SDL_GetPerformanceFrequency();
        if (voxfog_is_on()) {
            Uint64 t0 = SDL_GetPerformanceCounter();
            prof_gpu_begin(PROF_BLOOM);
            bloom_render(&s_bloom, bloom_scene_tex(&s_bloom), voxfog_fbo());
            prof_gpu_end(PROF_BLOOM);
            Uint64 t1 = SDL_GetPerformanceCounter();
            prof_add_ms(PROF_BLOOM, (float)(t1 - t0) * 1000.0f / (float)pf);
            t0 = SDL_GetPerformanceCounter();
            prof_gpu_begin(PROF_FOG);
            voxfog_render(g_vw, g_vh, &s_rc,
                          voxfog_texture(), bloom_scene_depth(&s_bloom));
            prof_gpu_end(PROF_FOG);
            t1 = SDL_GetPerformanceCounter();
            prof_add_ms(PROF_FOG, (float)(t1 - t0) * 1000.0f / (float)pf);
        } else {
            Uint64 t0 = SDL_GetPerformanceCounter();
            prof_gpu_begin(PROF_BLOOM);
            bloom_render(&s_bloom, bloom_scene_tex(&s_bloom), 0);
            prof_gpu_end(PROF_BLOOM);
            Uint64 t1 = SDL_GetPerformanceCounter();
            prof_add_ms(PROF_BLOOM, (float)(t1 - t0) * 1000.0f / (float)pf);
        }
    }

    // HUD/миникарта — только в игре; меню — поверх
    if (world && app_state() == APP_PLAY)
        render_ui(world, dt);
}

void renderer_end(Game *g, float dt)
{
    // Прицел-крест в центре (HL-стиль) — в игре, без консоли
    if (g->world_loaded && app_state() == APP_PLAY && !console_active()) {
        const float cx = g_vw * 0.5f, cy = g_vh * 0.5f;
        const float cw4[4] = { 0.92f, 0.96f, 1.0f, 0.9f };
        const float sh[4] = { 0.0f, 0.0f, 0.0f, 0.5f };
        glDisable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        // тень креста
        hud_rect_draw(g_vw, g_vh, cx - 1.5f + 1.0f, cy - 1.0f + 1.0f, 3.0f, 2.0f, sh);
        // горизонталь: два луча
        hud_rect_draw(g_vw, g_vh, cx - 10.0f, cy - 1.0f, 7.0f, 2.0f, cw4);
        hud_rect_draw(g_vw, g_vh, cx + 3.0f, cy - 1.0f, 7.0f, 2.0f, cw4);
        // вертикаль: два луча
        hud_rect_draw(g_vw, g_vh, cx - 1.0f, cy - 10.0f, 2.0f, 7.0f, cw4);
        hud_rect_draw(g_vw, g_vh, cx - 1.0f, cy + 3.0f, 2.0f, 7.0f, cw4);
    }

    // Сообщение карты (message из триггеров/use) — по центру экрана.
    // Таймер (g_ui_msg_t) считает game.c в game_update.
    if (g->ui_msg_t > 0.0f && g->world_loaded && app_state() == APP_PLAY) {
        float mw = hud_text_width(g->ui_msg);
        const float bg[4] = { 0.05f, 0.06f, 0.10f, 0.55f };
        const float fg[4] = { 0.9f, 0.95f, 1.0f, 1.0f };
        float my = g_vh * 0.62f;
        hud_rect_draw(g_vw, g_vh, (g_vw - mw) * 0.5f - 16.0f, my - 2.0f,
                      mw + 32.0f, hud_text_line_height() + 8.0f, bg);
        hud_text_draw(g_vw, g_vh, (g_vw - mw) * 0.5f,
                      my + hud_text_ascent(), g->ui_msg, fg);
    }

    // snd_debug: панелька справа сверху — какие звуки играют сейчас
    // и счётчик шагов (для отладки звука). Работает в меню и в игре.
    if (cvar_get("snd_debug") > 0.5f) {
        extern int g_step_calls;    // счётчик шагов (camera.c)
        char sndbuf[128], line[192];
        audio_active_names(sndbuf, sizeof sndbuf);
        snprintf(line, sizeof line, "SND: %s | steps: %d",
                 sndbuf[0] ? sndbuf : "-", g_step_calls);
        glDisable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        const float bg[4] = { 0.05f, 0.06f, 0.10f, 0.75f };
        hud_rect_draw(g_vw, g_vh, (float)g_vw - 470.0f, 8.0f, 462.0f, 26.0f, bg);
        // Текст по центру окошка: базовая линия = top + ascent
        const float fg[4] = { 0.85f, 0.95f, 1.0f, 1.0f };
        hud_text_draw_small(g_vw, g_vh, (float)g_vw - 458.0f,
                            8.0f + hud_text_ascent_small(), line, fg);
    }
    // profiler: панель справа снизу — полигоны, вершины, draw calls и
    // время по модулям (команда profiler). Что жрёт больше всего.
    if (cvar_get("profiler") > 0.5f && g->world_loaded) {
        char line[256];
        float total = prof_total_ms();
        float frame = dt * 1000.0f;
        float fps = frame > 0.001f ? 1000.0f / frame : 0.0f;
        glDisable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        const float bg[4] = { 0.05f, 0.06f, 0.10f, 0.75f };
        const float fg[4] = { 0.85f, 0.95f, 1.0f, 1.0f };
        const float dim[4] = { 0.62f, 0.66f, 0.74f, 1.0f };
        const float hot[4] = { 1.0f, 0.72f, 0.3f, 1.0f };
        float fh = hud_text_line_height_small();
        float y = (float)g_vh - 8.0f - fh * 8.0f;
        hud_rect_draw(g_vw, g_vh, (float)g_vw - 330.0f, y - 8.0f, 322.0f,
                      fh * 8.0f + 16.0f, bg);
        float x = (float)g_vw - 318.0f;
        snprintf(line, sizeof line, "Frame %.1f ms  (%.0f fps)", frame, fps);
        hud_text_draw_small(g_vw, g_vh, x, y + hud_text_ascent_small(), line, fg);
        y += fh;
        snprintf(line, sizeof line, "Tris %d | Verts %d | Draws %d",
                 g_prof_tris, g_prof_verts, g_prof_draws);
        hud_text_draw_small(g_vw, g_vh, x, y + hud_text_ascent_small(), line, fg);
        y += fh;
        // строки по модулям (только ненулевые, в порядке убывания GPU-времени)
        int order[PROF_N];
        for (int i = 0; i < PROF_N; i++) order[i] = i;
        for (int a = 0; a < PROF_N; a++)
            for (int b = a + 1; b < PROF_N; b++)
                if (prof_gpu_ms(order[b]) > prof_gpu_ms(order[a])) {
                    int t = order[a]; order[a] = order[b]; order[b] = t;
                }
        for (int k = 0; k < PROF_N && y < (float)g_vh - 8.0f - fh; k++) {
            int i = order[k];
            float cpu = prof_ms(i);
            float gpu = prof_gpu_ms(i);
            if (cpu <= 0.01f && gpu <= 0.01f) continue;
            float ref = gpu > cpu ? gpu : cpu;
            float pct = total > 0.01f ? 100.0f * ref / total : 0.0f;
            snprintf(line, sizeof line, "%-10s CPU %5.2f | GPU %5.2f ms",
                     prof_name(i), cpu, gpu);
            const float *c = (pct >= 25.0f) ? hot : (pct >= 10.0f ? fg : dim);
            hud_text_draw_small(g_vw, g_vh, x, y + hud_text_ascent_small(), line, c);
            y += fh;
        }
    }
}
