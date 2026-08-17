// ============================================================================
//  renderer.c — оркестрация рендера кадра (см. renderer.h)
// ----------------------------------------------------------------------------
//  Вынесено из main.c: обход дерева сцены с профилированием, каскад
//  постобработки (сцена -> bloom -> воксельный туман) и 2D-оверлеи
//  (миникарта, HUD, прицел, сообщение карты, панели отладки).
// ============================================================================
#include "renderer.h"

#include "app.h"
#include "actions.h"
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
#include "resources.h"
#include "sky.h"
#include "ssr.h"
#include "voxfog.h"
#include "water.h"
#include "world.h"
#include "fx.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#include "png.h"
#include "stb_image.h"

// FBO сцены и тумана: создаются при загрузке мира (renderer_setup)
static BloomParams s_bloom;
// SSR — отражения экранного пространства (рельеф в дождь): сцена+отражение.
static SsrParams s_ssr;
// Кубомап отражений воды (6 граней, рендер от игрока — см. renderer_cubemap_pass)
static GLuint s_refl_cubemap = 0;
static GLuint s_refl_cubemap_fbo[6] = {0};
static GLuint s_refl_cubemap_depth = 0;
static int s_refl_cubemap_size = 256; // размер каждой грани
static float s_cubemap_pos[3] = { 0, 0, 0 };  // центр последнего кубомапа
static int s_cubemap_valid = 0;               // кубомап ещё не рендерился
// Кубомап рендерится КАЖДЫЙ КАдр (реалтайм): отражения всегда свежие,
// без запаздывания при движении. 6 рендеров сцены 256x256 за кадр.
static int s_active = 0;       // мир загружен: FBO существуют
static int s_wire = 0;         // каркасный режим (cvar r_wireframe)
static GLuint s_noise_tex = 0; // img/noise.png: неравномерность мокрости/дымки
static int s_vw = 1280, s_vh = 720;   // размер окна в пикселях

int renderer_vw(void) { return s_vw; }
int renderer_vh(void) { return s_vh; }

void renderer_set_viewport(int vw, int vh)
{
    if (vw > 0 && vh > 0) { s_vw = vw; s_vh = vh; }
}

// --- Cubemap reflection FBO ----------------------------------------------------
static GLuint make_cubemap_color_tex(int size)
{
    GLuint id;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_CUBE_MAP, id);
    for (int face = 0; face < 6; face++) {
        glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, 0, GL_RGBA8,
                     size, size, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    }
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    return id;
}

static GLuint load_cubemap_from_files(const char *dir, int *out_size)
{
    static const char *face_names[6] = { "px", "nx", "py", "ny", "pz", "nz" };
    GLuint id = 0;
    int size = 0;
    int first = 1;

    // PNG хранит строки сверху вниз (в capture флип по Y), а glTexImage2D
    // считает строку 0 низом — без обратного флипа отражения были бы
    // перевёрнуты и ложились "криво". Флипаем при загрузке.
    stbi_set_flip_vertically_on_load(1);
    for (int face = 0; face < 6; face++) {
        char pattern[256];
        snprintf(pattern, sizeof pattern, "cubemap_*_%s.png", face_names[face]);
        int count = 0;
        char **files = SDL_GlobDirectory(dir, pattern, 0, &count);
        if (!files || count == 0) {
            if (files) SDL_free(files);
            continue;
        }
        // Take the most recent (last in list)
        char *latest = files[0];
        for (int i = 1; i < count; i++) {
            if (strcmp(files[i], latest) > 0) latest = files[i];
        }
        SDL_free(files);

        char fullpath[512];
        snprintf(fullpath, sizeof fullpath, "%s/%s", dir, latest);
        int w, h, ch;
        unsigned char *px = stbi_load(fullpath, &w, &h, &ch, 3);
        if (!px) {
            SDL_Log("Failed to load cubemap face %s: %s", latest, stbi_failure_reason());
            continue;
        }
        if (first) {
            glGenTextures(1, &id);
            glBindTexture(GL_TEXTURE_CUBE_MAP, id);
            size = w;
            first = 0;
        }
        glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, 0, GL_RGB8,
                     w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, px);
        stbi_image_free(px);
    }

    if (id) {
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
        if (out_size) *out_size = size;
        SDL_Log("Loaded static cubemap from %s (%dx%d)", dir, size, size);
    }
    return id;
}

static void make_cubemap_fbo(int size)
{
    if (size <= 0) return;
    if (s_refl_cubemap) {
        glDeleteTextures(1, &s_refl_cubemap);
        glDeleteFramebuffers(6, s_refl_cubemap_fbo);
        glDeleteTextures(1, &s_refl_cubemap_depth);
    }
    s_refl_cubemap_size = size;
    // Try to load static cubemap from cubemaps/ directory
    s_refl_cubemap = load_cubemap_from_files("cubemaps", &size);
    if (!s_refl_cubemap) {
        // Fallback: create empty cubemap texture
        s_refl_cubemap = make_cubemap_color_tex(size);
        SDL_Log("Cubemap reflection FBO created (empty): %dx%d", size, size);
    } else {
        SDL_Log("Using static cubemap from cubemaps/ (%dx%d)", size, size);
    }
    // Глубина (общая для всех граней) - still needed for capture
    glGenTextures(1, &s_refl_cubemap_depth);
    glBindTexture(GL_TEXTURE_CUBE_MAP, s_refl_cubemap_depth);
    for (int face = 0; face < 6; face++) {
        glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, 0, GL_DEPTH_COMPONENT24,
                     size, size, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, NULL);
    }
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    // FBO для каждой грани (still needed for capture command)
    for (int face = 0; face < 6; face++) {
        glGenFramebuffers(1, &s_refl_cubemap_fbo[face]);
        glBindFramebuffer(GL_FRAMEBUFFER, s_refl_cubemap_fbo[face]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, s_refl_cubemap, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                               GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, s_refl_cubemap_depth, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            SDL_Log("Cubemap FBO face %d incomplete!", face);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// --- Рендер-контекст: единое кадровое состояние сцены --------------------------
static RendererContext s_rc;

static void cubemap_center_pos(Game *g, float out[3]);   // fwd decl (ниже)

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
    rc->vw = s_vw;
    rc->vh = s_vh;

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
        // Видовой базис — откуда РЕАЛЬНО рендерится кадр. Узел Camera
        // пересчитывает его каждый кадр (в 3-м лице камера стоит сзади
        // персонажа); скайбокс/биллборды/SSR/воксельный туман берут его.
        if (g->cam->view_fwd[0] || g->cam->view_fwd[1] || g->cam->view_fwd[2]) {
            memcpy(rc->view_pos, g->cam->view_pos, sizeof rc->view_pos);
            memcpy(rc->view_fwd, g->cam->view_fwd, sizeof rc->view_fwd);
            memcpy(rc->view_right, g->cam->view_right, sizeof rc->view_right);
            memcpy(rc->view_up, g->cam->view_up, sizeof rc->view_up);
        } else {                        // до первого camera_node_update
            memcpy(rc->view_pos, rc->cam_pos, sizeof rc->view_pos);
            memcpy(rc->view_fwd, rc->cam_fwd, sizeof rc->view_fwd);
            memcpy(rc->view_right, rc->cam_right, sizeof rc->view_right);
            memcpy(rc->view_up, rc->cam_up, sizeof rc->view_up);
        }
        rc->cam_duck = g_act[A_DUCK] ? 1 : 0;
        frustum_build(&rc->frustum, rc->viewproj);   // culling цветного прохода
    }
    // Мокрость (дождь) — плавный 0..1 из fx.c (нарастает/высыхает секундами).
    // В дождь оригинальные источники света не выключаются, а гаснут до 20%
    // яркости; сверху добавляется яркий рассеянный «свет от неба», который
    // компенсирует потерянные 80% освещённости.
    rc->wetness = fx_wetness();
    float light_dim = 1.0f - 0.8f * rc->wetness;   // 1.0 -> 0.20
    // Свет: направленные источники (0 = солнце)
    float dir_sum = 0.0f;
    if (g->sun_node) {
        const SunParams *sun = g->sun_node->params;
        int n = sun->light_count > SUN_MAX_LIGHTS ? SUN_MAX_LIGHTS : sun->light_count;
        for (int i = 0; i < n; i++) {
            memcpy(rc->light_dir[i], sun->light_dir[i], 3 * sizeof(float));
            memcpy(rc->light_color[i], sun->light_color[i], 3 * sizeof(float));
            rc->light_intensity[i] = sun->light_intensity[i] * light_dim;
            dir_sum += sun->light_intensity[i];
        }
        rc->light_count = n;
    }
    // Свет от неба (только в дождь): светит ровно вниз, цвет пасмурного
    // неба; яркость растёт с мокротой до 80% суммарной яркости солнца.
    if (rc->wetness > 0.001f && dir_sum > 0.0f && rc->light_count < SUN_MAX_LIGHTS) {
        int k = rc->light_count++;
        rc->light_dir[k][0] = 0.0f;
        rc->light_dir[k][1] = 1.0f;
        rc->light_dir[k][2] = 0.0f;
        rc->light_color[k][0] = 0.62f;
        rc->light_color[k][1] = 0.68f;
        rc->light_color[k][2] = 0.80f;
        rc->light_intensity[k] = 0.8f * dir_sum * rc->wetness;
    }
    // Точечные и прожекторные источники (map.json lights[]: point/spot)
    rc->point_count = rc->spot_count = 0;
    for (int i = 0; i < mapdata_light_count(); i++) {
        const MapLight *l = mapdata_light(i);
        if (!l) continue;
        if (l->type == LIGHT_POINT && rc->point_count < SCENE_MAX_POINT) {
            int k = rc->point_count++;
            memcpy(rc->point_pos[k], l->pos, 3 * sizeof(float));
            memcpy(rc->point_color[k], l->color, 3 * sizeof(float));
            rc->point_intensity[k] = l->intensity;
            rc->point_range[k] = l->range > 0.1f ? l->range : 20.0f;
        } else if (l->type == LIGHT_SPOT && rc->spot_count < SCENE_MAX_SPOT) {
            int k = rc->spot_count++;
            memcpy(rc->spot_pos[k], l->pos, 3 * sizeof(float));
            memcpy(rc->spot_dir[k], l->dir, 3 * sizeof(float));
            memcpy(rc->spot_color[k], l->color, 3 * sizeof(float));
            rc->spot_intensity[k] = l->intensity;
            rc->spot_range[k] = l->range > 0.1f ? l->range : 20.0f;
            rc->spot_angle[k] = l->angle > 0.01f ? l->angle : 0.6f;
        }
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
    // Видимость солнца (в дождь диск скрыт облаками — гаснет вместе с мокротой).
    // Мокрость уже посчитана выше (rc->wetness) — она же гасит источник света.
    rc->sun_glow = 1.0f - rc->wetness;
    if (rc->sun_glow < 0.0f) rc->sun_glow = 0.0f;
    if (rc->sun_glow > 1.0f) rc->sun_glow = 1.0f;
    // Вьюпорт текущего прохода (проходы кубомапа переопределяют его)
    rc->view_vp[0] = 0;
    rc->view_vp[1] = 0;
    rc->view_vp[2] = s_vw;
    rc->view_vp[3] = s_vh;
    // Кубомап отражений — статический: загружается из cubemaps/ при старте.
    // refl_cubemap_dirty оставлен для совместимости, но не используется.
    if (g->cam && g->world_loaded)
        rc->refl_cubemap_dirty = 0;
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
            // на дальнем каскаде — вдвое меньше (иначе тень отрывается от гор)
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
    su->u_pointpos = glGetUniformLocation(prog, "uPointPos");
    su->u_pointcolor = glGetUniformLocation(prog, "uPointColor");
    su->u_pointint = glGetUniformLocation(prog, "uPointIntensity");
    su->u_pointrange = glGetUniformLocation(prog, "uPointRange");
    su->u_pointcount = glGetUniformLocation(prog, "uPointCount");
    su->u_spotpos = glGetUniformLocation(prog, "uSpotPos");
    su->u_spotdir = glGetUniformLocation(prog, "uSpotDir");
    su->u_spotcolor = glGetUniformLocation(prog, "uSpotColor");
    su->u_spotint = glGetUniformLocation(prog, "uSpotIntensity");
    su->u_spotrange = glGetUniformLocation(prog, "uSpotRange");
    su->u_spotangle = glGetUniformLocation(prog, "uSpotAngle");
    su->u_spotcount = glGetUniformLocation(prog, "uSpotCount");
    su->u_ambient  = glGetUniformLocation(prog, "uAmbient");
    su->u_skycolor = glGetUniformLocation(prog, "uSkyColor");
    su->u_groundcolor = glGetUniformLocation(prog, "uGroundColor");
    su->u_fogcolor = glGetUniformLocation(prog, "uFogColor");
    su->u_fogstart = glGetUniformLocation(prog, "uFogStart");
    su->u_fogend   = glGetUniformLocation(prog, "uFogEnd");
    su->u_fogon    = glGetUniformLocation(prog, "uFogOn");
    su->u_shadow_on = glGetUniformLocation(prog, "uShadowOn");
    su->u_wetness = glGetUniformLocation(prog, "uWetness");
    su->u_reflection = glGetUniformLocation(prog, "uReflection");
    su->u_noise = glGetUniformLocation(prog, "uNoise");
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
    // Точечные и прожекторные источники
    if (su->u_pointpos >= 0) glUniform3fv(su->u_pointpos, SCENE_MAX_POINT, &rc->point_pos[0][0]);
    if (su->u_pointcolor >= 0) glUniform3fv(su->u_pointcolor, SCENE_MAX_POINT, &rc->point_color[0][0]);
    if (su->u_pointint >= 0) glUniform1fv(su->u_pointint, SCENE_MAX_POINT, rc->point_intensity);
    if (su->u_pointrange >= 0) glUniform1fv(su->u_pointrange, SCENE_MAX_POINT, rc->point_range);
    if (su->u_pointcount >= 0) glUniform1i(su->u_pointcount, rc->point_count);
    if (su->u_spotpos >= 0) glUniform3fv(su->u_spotpos, SCENE_MAX_SPOT, &rc->spot_pos[0][0]);
    if (su->u_spotdir >= 0) glUniform3fv(su->u_spotdir, SCENE_MAX_SPOT, &rc->spot_dir[0][0]);
    if (su->u_spotcolor >= 0) glUniform3fv(su->u_spotcolor, SCENE_MAX_SPOT, &rc->spot_color[0][0]);
    if (su->u_spotint >= 0) glUniform1fv(su->u_spotint, SCENE_MAX_SPOT, rc->spot_intensity);
    if (su->u_spotrange >= 0) glUniform1fv(su->u_spotrange, SCENE_MAX_SPOT, rc->spot_range);
    if (su->u_spotangle >= 0) glUniform1fv(su->u_spotangle, SCENE_MAX_SPOT, rc->spot_angle);
    if (su->u_spotcount >= 0) glUniform1i(su->u_spotcount, rc->spot_count);
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
    if (su->u_wetness >= 0)
        glUniform1f(su->u_wetness, rc->wetness);
    // Кубомап отражений воды на юните 9 (для мокрой земли/деревьев; вода
    // держит его на юните 7 отдельно — не конфликтует). Внутри прохода
    // граней кубомапа его НЕ биндим: текстура прикреплена к текущему FBO —
    // сэмплинг был бы фидбек-лупом (UB).
    if (su->u_reflection >= 0 && s_refl_cubemap && rc->pass != RP_CUBEMAP_FACE) {
        glActiveTexture(GL_TEXTURE9);
        glBindTexture(GL_TEXTURE_CUBE_MAP, s_refl_cubemap);
        glUniform1i(su->u_reflection, 9);
    }
    // Шум мокрости/дымки (img/noise.png, REPEAT) — юнит 11
    if (su->u_noise >= 0 && s_noise_tex) {
        glActiveTexture(GL_TEXTURE11);
        glBindTexture(GL_TEXTURE_2D, s_noise_tex);
        glUniform1i(su->u_noise, 11);
    }
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
    ssr_init(&s_ssr, vw, vh);
    voxfog_init(w, vw, vh);
    make_cubemap_fbo(256); // кубомап фиксированного размера
    s_cubemap_valid = 0;
    s_noise_tex = resources_texture("img/noise.png", 3, GL_REPEAT, 1);
    s_active = 1;
    // Каркасный режим из config.cfg: sync_cvars срабатывает на изменение.
    s_wire = cvar_get("r_wireframe") > 0.5f;
}

void renderer_resize(int vw, int vh)
{
    if (!s_active) return;
    bloom_resize(&s_bloom, vw, vh);
    ssr_resize(&s_ssr, vw, vh);
    voxfog_resize(vw, vh);
}

void renderer_teardown(void)
{
    if (!s_active) return;
    bloom_cleanup(&s_bloom);
    ssr_cleanup(&s_ssr);
    voxfog_cleanup();
    if (s_refl_cubemap) {
        glDeleteTextures(1, &s_refl_cubemap);
        glDeleteFramebuffers(6, s_refl_cubemap_fbo);
        glDeleteTextures(1, &s_refl_cubemap_depth);
        s_refl_cubemap = 0;
    }
    if (s_noise_tex) {
        resources_texture_release(s_noise_tex);
        s_noise_tex = 0;
    }
    s_cubemap_valid = 0;
    s_active = 0;
}

void renderer_set_wireframe(int on)
{
    s_wire = on ? 1 : 0;
}

GLuint renderer_refl_cubemap(void)
{
    return s_refl_cubemap;
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
    glViewport(0, 0, s_vw, s_vh);
    glClearColor(clear_rgb[0], clear_rgb[1], clear_rgb[2], 1.0f);
    glDepthMask(GL_TRUE);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

// --- Cubemap reflection pass ---------------------------------------------------
// 6 направлений/вверх-векторов граней кубомапа (стандартная ориентация GL:
// рендер каждой грани развёрнут, чтобы samplerCube читал её по спеке).
static const float s_face_dirs[6][3] = {
    { 1,  0,  0},   // +X
    {-1,  0,  0},   // -X
    { 0,  1,  0},   // +Y
    { 0, -1,  0},   // -Y
    { 0,  0,  1},   // +Z
    { 0,  0, -1},   // -Z
};
static const float s_face_ups[6][3] = {
    { 0, -1,  0},   // +X
    { 0, -1,  0},   // -X
    { 0,  0,  1},   // +Y
    { 0,  0, -1},   // -Y
    { 0, -1,  0},   // +Z
    { 0, -1,  0},   // -Z
};

// Центр кубомапа отражений: x/z игрока, y — уровень воды под игроком
// (если воды нет — высота камеры: куб описывает окружение игрока).
static void cubemap_center_pos(Game *g, float out[3])
{
    const Camera *cam = &g->cam->cam;
    out[0] = cam->pos[0];
    out[2] = cam->pos[2];
    float ws = water_surface_at(&g->world, cam->pos[0], cam->pos[2]);
    out[1] = (ws > -1e8f) ? ws : cam->pos[1];
}

// Рендер всех 6 граней кубомапа отражений из точки pos. Сохраняет/восстанавливает
// камеру контекста: узлы сцены (небо, биллборды, фрустум) должны видеть
// текущую грань, а не основную камеру. dt = 0 — время анимаций не крутим.
static void renderer_cubemap_render_at(Game *g, SceneNode *world, const float *pos)
{
    if (!s_refl_cubemap || !g || !g->cam || !world) return;

    RendererContext *rc = &s_rc;
    // Сохраняем текущее состояние контекста
    float orig_viewproj[16];
    memcpy(orig_viewproj, rc->viewproj, sizeof orig_viewproj);
    int orig_pass = rc->pass;
    float orig_pos[3], orig_fwd[3], orig_right[3], orig_up[3];
    memcpy(orig_pos, rc->cam_pos, sizeof orig_pos);
    memcpy(orig_fwd, rc->cam_fwd, sizeof orig_fwd);
    memcpy(orig_right, rc->cam_right, sizeof orig_right);
    memcpy(orig_up, rc->cam_up, sizeof orig_up);
    float orig_fov = rc->cam_fov_deg, orig_aspect = rc->cam_aspect;
    int orig_vp[4];
    memcpy(orig_vp, rc->view_vp, sizeof orig_vp);
    Frustum orig_frustum;
    memcpy(&orig_frustum, &rc->frustum, sizeof orig_frustum);
    GLint orig_fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &orig_fbo);

    float proj[16];
    mat4_perspective(proj, 90.0f * 3.14159f / 180.0f, 1.0f, rc->znear, rc->zfar);

    // Проход грани: камера в центре куба; клип отрезает всё ниже центра
    // (подводный/подземный мусор в отражениях не нужен).
    rc->pass = RP_CUBEMAP_FACE;
    memcpy(rc->cam_pos, pos, sizeof rc->cam_pos);
    rc->cam_fov_deg = 90.0f;
    rc->cam_aspect = 1.0f;
    rc->view_vp[0] = 0;
    rc->view_vp[1] = 0;
    rc->view_vp[2] = s_refl_cubemap_size;
    rc->view_vp[3] = s_refl_cubemap_size;

    for (int face = 0; face < 6; face++) {
        float view[16], viewproj[16], target[3];
        target[0] = pos[0] + s_face_dirs[face][0];
        target[1] = pos[1] + s_face_dirs[face][1];
        target[2] = pos[2] + s_face_dirs[face][2];
        mat4_lookat(view, pos, target, s_face_ups[face]);
        mat4_mul(viewproj, proj, view);

        memcpy(rc->viewproj, viewproj, sizeof rc->viewproj);
        // Базис взгляда грани: небо (sky.fs) и биллборды (particle.vs)
        // восстанавливают направление луча из cam_fwd/right/up.
        memcpy(rc->cam_fwd, s_face_dirs[face], sizeof rc->cam_fwd);
        memcpy(rc->cam_up, s_face_ups[face], sizeof rc->cam_up);
        vec3_cross(rc->cam_fwd, rc->cam_up, rc->cam_right);
        vec3_normalize(rc->cam_right);
        vec3_cross(rc->cam_right, rc->cam_fwd, rc->cam_up);
        vec3_normalize(rc->cam_up);
        frustum_build(&rc->frustum, viewproj);

        glBindFramebuffer(GL_FRAMEBUFFER, s_refl_cubemap_fbo[face]);
        glViewport(0, 0, s_refl_cubemap_size, s_refl_cubemap_size);
        glClearColor(0.10f, 0.10f, 0.15f, 1.0f);
        glDepthMask(GL_TRUE);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        render_part(world, 0.0f, 0);   // dt = 0: время сцены не двигать
    }

    // Восстанавливаем контекст
    memcpy(rc->viewproj, orig_viewproj, sizeof rc->viewproj);
    rc->pass = orig_pass;
    memcpy(rc->cam_pos, orig_pos, sizeof rc->cam_pos);
    memcpy(rc->cam_fwd, orig_fwd, sizeof rc->cam_fwd);
    memcpy(rc->cam_right, orig_right, sizeof rc->cam_right);
    memcpy(rc->cam_up, orig_up, sizeof rc->cam_up);
    rc->cam_fov_deg = orig_fov;
    rc->cam_aspect = orig_aspect;
    memcpy(rc->view_vp, orig_vp, sizeof rc->view_vp);
    memcpy(&rc->frustum, &orig_frustum, sizeof rc->frustum);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)orig_fbo);
    glViewport(orig_vp[0], orig_vp[1], orig_vp[2], orig_vp[3]);

    memcpy(s_cubemap_pos, pos, sizeof s_cubemap_pos);
    s_cubemap_valid = 1;
}

void renderer_frame(Game *g, float dt, int draw_3d)
{
    renderer_ctx_fill(g);        // единый контекст сцены на этот кадр
    SceneNode *world = g->root;
    int did_3d = 0;

    // Подводные эффекты: следим за тем, под водой ли глаза игрока, и плавно
    // вводим/выводим постобработку (тонирование, искажение, виньетка). Тон
    // берём из водоёма (water_underwater); время копится для анимации волн.
    if (world && draw_3d) {
        static float s_under = 0.0f;
        static float s_undtime = 0.0f;
        float tint[3] = { 0.08f, 0.22f, 0.30f };
        int inw = g->world_loaded &&
                  water_underwater(&g->world, s_rc.cam_pos, tint);
        float target = inw ? 1.0f : 0.0f;
        float rate = 1.6f;                 // ~0.6 с до полного входа/выхода
        if (s_under < target) {
            s_under += rate * dt; if (s_under > target) s_under = target;
        } else if (s_under > target) {
            s_under -= rate * dt; if (s_under < 0.0f) s_under = 0.0f;
        }
        if (s_under > 0.0f) s_undtime += dt;   // волны идут, пока эффект виден
        bloom_set_underwater(s_under, tint, s_undtime);
    }

    // Кубомап отражений — статический (загружен из cubemaps/ при старте).
    // Рендер в реальном времени отключён для производительности.
    // Для обновления используйте консольную команду cubemap_capture.

    // В игре и в паузе — рисуем сцену (пауза затемняет её меню).
    // В главном меню с фоткой-фоном сцену не рисуем вовсе (draw_3d = 0).
    if (world && draw_3d) {
        // Каскадные тени: узлы рисуют глубину (pass = RP_SHADOW). Всегда
        // заполненными — до включения каркасного режима.
        if (s_rc.shadow_on) {
            Uint64 t0 = SDL_GetPerformanceCounter();
            prof_gpu_begin(PROF_SHADOW);
            shadow_render(world, &s_rc);
            prof_gpu_end(PROF_SHADOW);
            Uint64 t1 = SDL_GetPerformanceCounter();
            prof_add_ms(PROF_SHADOW,
                        (float)(t1 - t0) * 1000.0f /
                        (float)SDL_GetPerformanceFrequency());
        }
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
        glViewport(0, 0, s_vw, s_vh);
        Uint64 pf = SDL_GetPerformanceFrequency();
        // SSR: сцена+отражение (мокрый рельеф в дождь). SSR выключен/сухо
        // или сила 0 — проходим мимо (FBO SSR мог устареть с прошлых кадров).
        GLuint scene_for_post = bloom_scene_tex(&s_bloom);
        if (cvar_get("r_ssr") >= 0.5f &&
            (s_rc.wetness > 0.001f || cvar_get("r_ssr_wet") > 0.001f) &&
            cvar_get("r_ssr_strength") > 0.0f) {
            Uint64 ts = SDL_GetPerformanceCounter();
            prof_gpu_begin(PROF_SSR);
            ssr_render(&s_ssr, bloom_scene_tex(&s_bloom),
                       bloom_scene_depth(&s_bloom), &s_rc);
            prof_gpu_end(PROF_SSR);
            Uint64 te = SDL_GetPerformanceCounter();
            prof_add_ms(PROF_SSR, (float)(te - ts) * 1000.0f / (float)pf);
            scene_for_post = ssr_tex(&s_ssr);
        }
        if (voxfog_is_on()) {
            Uint64 t0 = SDL_GetPerformanceCounter();
            prof_gpu_begin(PROF_BLOOM);
            bloom_render(&s_bloom, scene_for_post, voxfog_fbo());
            prof_gpu_end(PROF_BLOOM);
            Uint64 t1 = SDL_GetPerformanceCounter();
            prof_add_ms(PROF_BLOOM, (float)(t1 - t0) * 1000.0f / (float)pf);
            t0 = SDL_GetPerformanceCounter();
            prof_gpu_begin(PROF_FOG);
            voxfog_render(s_vw, s_vh, &s_rc,
                          voxfog_texture(), bloom_scene_depth(&s_bloom));
            prof_gpu_end(PROF_FOG);
            t1 = SDL_GetPerformanceCounter();
            prof_add_ms(PROF_FOG, (float)(t1 - t0) * 1000.0f / (float)pf);
        } else {
            Uint64 t0 = SDL_GetPerformanceCounter();
            prof_gpu_begin(PROF_BLOOM);
            bloom_render(&s_bloom, scene_for_post, 0);
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
    // Прицел — простая белая точка в центре (в игре, без консоли,
    // cvar r_crosshair; консоль: toggle_crosshair)
    if (g->world_loaded && app_state() == APP_PLAY && !console_active()
        && cvar_get("r_crosshair") > 0.5f) {
        const float cx = s_vw * 0.5f, cy = s_vh * 0.5f;
        const float white[4] = { 1.0f, 1.0f, 1.0f, 0.9f };
        glDisable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        // точка 3x3 px в центре
        hud_rect_draw(s_vw, s_vh, cx - 1.5f, cy - 1.5f, 3.0f, 3.0f, white);
    }

    // Сообщение карты (message из триггеров/use) — по центру экрана.
    // Таймер (g_ui_msg_t) считает game.c в game_update.
    if (g->ui_msg_t > 0.0f && g->world_loaded && app_state() == APP_PLAY) {
        float mw = hud_text_width(g->ui_msg);
        const float bg[4] = { 0.05f, 0.06f, 0.10f, 0.55f };
        const float fg[4] = { 0.9f, 0.95f, 1.0f, 1.0f };
        float my = s_vh * 0.62f;
        hud_rect_draw(s_vw, s_vh, (s_vw - mw) * 0.5f - 16.0f, my - 2.0f,
                      mw + 32.0f, hud_text_line_height() + 8.0f, bg);
        hud_text_draw(s_vw, s_vh, (s_vw - mw) * 0.5f,
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
        hud_rect_draw(s_vw, s_vh, (float)s_vw - 470.0f, 8.0f, 462.0f, 26.0f, bg);
        // Текст по центру окошка: базовая линия = top + ascent
        const float fg[4] = { 0.85f, 0.95f, 1.0f, 1.0f };
        hud_text_draw_small(s_vw, s_vh, (float)s_vw - 458.0f,
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
        float y = (float)s_vh - 8.0f - fh * 8.0f;
        hud_rect_draw(s_vw, s_vh, (float)s_vw - 330.0f, y - 8.0f, 322.0f,
                      fh * 8.0f + 16.0f, bg);
        float x = (float)s_vw - 318.0f;
        snprintf(line, sizeof line, "Frame %.1f ms  (%.0f fps)", frame, fps);
        hud_text_draw_small(s_vw, s_vh, x, y + hud_text_ascent_small(), line, fg);
        y += fh;
        snprintf(line, sizeof line, "Tris %d | Verts %d | Draws %d",
                 g_prof_tris, g_prof_verts, g_prof_draws);
        hud_text_draw_small(s_vw, s_vh, x, y + hud_text_ascent_small(), line, fg);
        y += fh;
        // строки по модулям (только ненулевые, в порядке убывания GPU-времени)
        int order[PROF_N];
        for (int i = 0; i < PROF_N; i++) order[i] = i;
        for (int a = 0; a < PROF_N; a++)
            for (int b = a + 1; b < PROF_N; b++)
                if (prof_gpu_ms(order[b]) > prof_gpu_ms(order[a])) {
                    int t = order[a]; order[a] = order[b]; order[b] = t;
                }
        for (int k = 0; k < PROF_N && y < (float)s_vh - 8.0f - fh; k++) {
            int i = order[k];
            float cpu = prof_ms(i);
            float gpu = prof_gpu_ms(i);
            if (cpu <= 0.01f && gpu <= 0.01f) continue;
            float ref = gpu > cpu ? gpu : cpu;
            float pct = total > 0.01f ? 100.0f * ref / total : 0.0f;
            snprintf(line, sizeof line, "%-10s CPU %5.2f | GPU %5.2f ms",
                     prof_name(i), cpu, gpu);
            const float *c = (pct >= 25.0f) ? hot : (pct >= 10.0f ? fg : dim);
            hud_text_draw_small(s_vw, s_vh, x, y + hud_text_ascent_small(), line, c);
            y += fh;
        }
    }
}

// --- Cubemap capture: рендер 6 граней в позиции и сохранение PNG --------------
// Рендерит кубомап отражений из точки pos (размер size, лимит 512) той же
// корректной логикой, что и живой прохождение отражений (renderer_cubemap_render_at),
// и тут же читает каждую грань обратно, сохраняя 6 PNG в cubemaps/ (px..nz).
// Заодно этот кубомап становится живым (вода начнёт отражать его).
void renderer_cubemap_capture_at(Game *g, const float *pos, int size)
{
    if (!s_refl_cubemap || size <= 0 || !g || !pos) return;
    if (size > 512) size = 512;   // лимит
    if (size < 16) size = 16;

    // Если размер отличается от текущего живого кубомапа — пересоздать под него
    if (size != s_refl_cubemap_size)
        make_cubemap_fbo(size);

    // Рендер 6 граней из точки pos (корректная ориентация/клип/без солнца-в-дождь)
    renderer_cubemap_render_at(g, g->root, pos);

    static const char *face_names[6] = { "px", "nx", "py", "ny", "pz", "nz" };

    // Читаем каждую грань обратно из её FBO и сохраняем PNG
    GLint orig_fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &orig_fbo);
    for (int face = 0; face < 6; face++) {
        glBindFramebuffer(GL_FRAMEBUFFER, s_refl_cubemap_fbo[face]);
        glViewport(0, 0, size, size);
        unsigned char *px = malloc((size_t)size * size * 3);
        unsigned char *flip = malloc((size_t)size * size * 3);
        if (px && flip) {
            glReadPixels(0, 0, size, size, GL_RGB, GL_UNSIGNED_BYTE, px);
            // PNG хранит строки сверху вниз, GL возвращает снизу вверх — флип по Y
            size_t row = (size_t)size * 3;
            for (int y = 0; y < size; y++)
                memcpy(flip + (size_t)y * row, px + (size_t)(size - 1 - y) * row, row);

            char path[256];
            time_t now = time(NULL);
            struct tm *tm = localtime(&now);
            snprintf(path, sizeof path, "cubemaps/cubemap_%04d%02d%02d_%02d%02d%02d_%s.png",
                     tm ? tm->tm_year + 1900 : 2026, tm ? tm->tm_mon + 1 : 1,
                     tm ? tm->tm_mday : 1, tm ? tm->tm_hour : 0,
                     tm ? tm->tm_min : 0, tm ? tm->tm_sec : 0,
                     face_names[face]);
#ifdef _WIN32
            _mkdir("cubemaps");
#else
            mkdir("cubemaps", 0755);
#endif
            if (png_save(path, size, size, flip) == 0)
                SDL_Log("cubemap_capture: saved %s (%dx%d)", path, size, size);
            else
                SDL_Log("cubemap_capture: failed to save %s", path);
        }
        free(px);
        free(flip);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)orig_fbo);
    glViewport(s_rc.view_vp[0], s_rc.view_vp[1], s_rc.view_vp[2], s_rc.view_vp[3]);

    SDL_Log("cubemap_capture: done at (%.0f, %.0f, %.0f) size %d", pos[0], pos[1], pos[2], size);
}
