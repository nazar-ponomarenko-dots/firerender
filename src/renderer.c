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
#include "console.h"
#include "cvars.h"
#include "game.h"
#include "gl_loader.h"
#include "hud.h"
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
            voxfog_render(g_vw, g_vh, g->cam, (SunParams *)g->sun_node->params,
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
