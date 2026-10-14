// ============================================================================
//  app.c — state manager and menu screens (see app.h)
// ----------------------------------------------------------------------------
//  Main menu: logo on the left, vertically centered; items at the bottom left
//  (HL1 style). Everything opened from the menu (settings, pause) is a WINDOW
//  with a frame, a title header and a close cross. Settings write into cvars
//  immediately; config.cfg is saved when leaving the settings screens and on exit.
// ============================================================================
#include "app.h"

#include "binds.h"
#include "console.h"
#include "cvars.h"
#include "game.h"
#include "gl_loader.h"
#include "hud.h"
#include "resources.h"
#include "shader.h"
#include "ui.h"

#include <stdlib.h>
#include <SDL3/SDL.h>
#include <string.h>
#ifdef _WIN32
#  include <io.h>
#else
#  include <dirent.h>
#endif

// Menu screens
enum {
    SCR_MAIN = 0,
    SCR_PAUSE,
    SCR_SETTINGS,
    SCR_GRAPHICS,
    SCR_AUDIO,
    SCR_CONTROLS,
    SCR_KEYS,
    SCR_MAPS,
    // Action codes (returned by the screen functions)
    SCR_ACT_START = 100,   // new game (load the world)
    SCR_ACT_RESUME,        // resume
    SCR_ACT_QUIT,          // quit
    SCR_ACT_BACK,          // back across screens
    SCR_ACT_MAIN,          // to the main menu (from pause)
};

// Actions for the "Keys" screen (command = bind). Only the important ones;
// everything else bound in config.cfg is shown separately
// (the "OTHER BINDS" panel on the right).
typedef struct { const char *cmd; const char *desc; } BindAction;
static const BindAction s_bind_actions[] = {
    { "+forward",       "Вперёд" },
    { "+back",          "Назад" },
    { "+left",          "Влево" },
    { "+right",         "Вправо" },
    { "+jump",          "Прыжок / вверх" },
    { "+duck",          "Присесть" },
    { "+down",          "Вниз (полёт)" },
    { "+speed",         "Бег (ускорение)" },
    { "+use",           "Взаимодействие (E)" },
    { "noclip",         "Полёт / ходьба" },
    { "save",           "Сохранить игру" },
    { "load",           "Загрузить игру" },
    { "screenshot",     "Скриншот" },
    { "demo",           "Демо-сцена" },
};
#define BIND_ACTIONS_N (sizeof s_bind_actions / sizeof s_bind_actions[0])

// Is this a menu bind (standard)? If not — it goes to "OTHER BINDS" on the right.
static int bind_is_standard(const char *cmd)
{
    for (int i = 0; i < (int)BIND_ACTIONS_N; i++)
        if (strcmp(s_bind_actions[i].cmd, cmd) == 0) return 1;
    return 0;
}

static AppState s_state = APP_BOOT;
static int s_screen = SCR_MAIN;
static int s_rec = -1;      // index of the action being recorded ("Keys" screen), -1 = none
static Ui s_ui;

// --- Map list: only maps/*.zip (maps are edited by unpacking them) -------------
#define MAPS_MAX 32
static char s_maps[MAPS_MAX][64];
static int s_maps_n = 0;
static int s_maps_page = 0;

static void maps_scan(void)
{
    s_maps_n = 0;
    // Look for maps/ in the working folder and one level up (running from build/ etc.)
    const char *dirs[] = { "maps", "../maps" };
    for (int d = 0; d < 2 && s_maps_n < MAPS_MAX; d++) {
#ifdef _WIN32
        (void)dirs;   // Windows: placeholder (for now only the map/ folder)
#else
        DIR *dir = opendir(dirs[d]);
        if (!dir) continue;
        struct dirent *e;
        while ((e = readdir(dir)) && s_maps_n < MAPS_MAX) {
            const char *nm = e->d_name;
            size_t l = strlen(nm);
            if (l > 4 && strcmp(nm + l - 4, ".zip") == 0) {
                char base[64];
                snprintf(base, sizeof base, "%.*s", (int)(l - 4), nm);
                int dup = 0;
                for (int i = 0; i < s_maps_n; i++)
                    if (strcmp(s_maps[i], base) == 0) dup = 1;
                if (!dup)
                    snprintf(s_maps[s_maps_n++], 64, "%s", base);
            }
        }
        closedir(dir);
#endif
    }
}

static const char *maps_current(void)
{
    const char *cur = game_instance()->map_name;
    for (int i = 0; i < s_maps_n; i++)
        if (strcmp(s_maps[i], cur) == 0) return s_maps[i];
    return s_maps_n > 0 ? s_maps[0] : "map1";
}

// Print the map list to the console (maps command)
void app_maps_print(void)
{
    maps_scan();
    const char *cur = game_instance()->map_name;
    for (int i = 0; i < s_maps_n; i++)
        console_print("  %s%s", s_maps[i],
                      strcmp(s_maps[i], cur) == 0 ? "  *" : "");
}

#define C_WHITE  { 0.92f, 0.92f, 0.95f, 1.0f }
#define C_HILITE { 1.0f, 0.78f, 0.25f, 1.0f }
#define C_DIM    { 0.62f, 0.66f, 0.72f, 1.0f }

// Menu background: img/menu.png picture (like HL1 — menu over a photo).
static GLuint s_bg_tex = 0;
static GLuint s_bg_prog = 0, s_bg_vao = 0, s_bg_vbo = 0;
static GLint s_bg_u_basemap, s_bg_u_color, s_bg_u_usetex, s_bg_u_usemarker;

// --- Photo background -----------------------------------------------------------
void app_init(void)
{
    s_state = APP_BOOT;   // startup phase: systems are brought up by boot_init (main.c)
    s_screen = SCR_MAIN;
    memset(&s_ui, 0, sizeof s_ui);
    s_ui.active = -1;

    // FR_MENU=graphics|settings|... — dev hook: start on the given menu
    // screen (headless UI screenshots: FR_MENU=graphics FR_SHOT=120)
    {
        const char *m = getenv("FR_MENU");
        if (m && m[0]) {
            if (!strcmp(m, "graphics")) s_screen = SCR_GRAPHICS;
            else if (!strcmp(m, "settings")) s_screen = SCR_SETTINGS;
            else if (!strcmp(m, "audio")) s_screen = SCR_AUDIO;
            else if (!strcmp(m, "controls")) s_screen = SCR_CONTROLS;
            else if (!strcmp(m, "pause")) s_screen = SCR_PAUSE;
            s_state = APP_PAUSE;
        }
    }

    s_bg_tex = resources_texture("img/menu.png", 3, GL_CLAMP_TO_EDGE, 1);
    if (s_bg_tex) {
        s_bg_prog = make_program("shaders/minimap.vs", "shaders/minimap.fs");
        if (s_bg_prog) {
            s_bg_u_basemap = glGetUniformLocation(s_bg_prog, "uBasemap");
            s_bg_u_color = glGetUniformLocation(s_bg_prog, "uColor");
            s_bg_u_usetex = glGetUniformLocation(s_bg_prog, "uUseTex");
            s_bg_u_usemarker = glGetUniformLocation(s_bg_prog, "uUseMarker");
            glGenVertexArrays(1, &s_bg_vao);
            glBindVertexArray(s_bg_vao);
            glGenBuffers(1, &s_bg_vbo);
            glBindBuffer(GL_ARRAY_BUFFER, s_bg_vbo);
            // Full-screen quad. V is flipped: PNG rows load top-down
            // (v=0 is the TOP of the image), while in OpenGL v=0 is the bottom of the screen.
            const float quad[6][4] = {
                {-1, -1, 0, 1}, { 1, -1, 1, 1}, { 1,  1, 1, 0},
                {-1, -1, 0, 1}, { 1,  1, 1, 0}, {-1,  1, 0, 0},
            };
            glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), NULL);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                                  (const void *)(2 * sizeof(float)));
            glEnableVertexAttribArray(1);
            glBindVertexArray(0);
            glUseProgram(s_bg_prog);
            glUniform1i(s_bg_u_basemap, 0);
            glUniform1i(s_bg_u_usemarker, 0);
            glUseProgram(0);
        }
    }
}

int app_bg_has(void)
{
    return s_bg_tex && s_bg_prog;
}

static void app_draw_bg(int vw, int vh)
{
    // In pause the background is the DARKENED GAME (scene drawn by the main loop).
    // In the main menu (world not loaded) — the photo img/menu.png.
    if (s_state != APP_PAUSE && app_bg_has()) {
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glUseProgram(s_bg_prog);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, s_bg_tex);
        glUniform1f(s_bg_u_usetex, 1.0f);
        glUniform1f(s_bg_u_usemarker, 0.0f);
        glUniform3f(s_bg_u_color, 1.0f, 1.0f, 1.0f);
        glBindVertexArray(s_bg_vao);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glEnable(GL_CULL_FACE);
    }
    // Darkening under the menu — text stays readable over any background
    float a = (s_state == APP_PAUSE) ? 0.55f
            : (app_bg_has() ? 0.30f : 0.55f);
    const float dim[4] = { 0, 0, 0, a };
    hud_rect_draw(vw, vh, 0, 0, (float)vw, (float)vh, dim);
}

// Panel: semi-transparent backing with a frame
static void menu_panel(float x, float y, float w, float h)
{
    const float bg[4] = { 0.06f, 0.07f, 0.10f, 0.80f };
    hud_rect_draw(s_ui.vw, s_ui.vh, x, y, w, h, bg);
    const float br[4] = { 0.42f, 0.47f, 0.58f, 0.85f };
    hud_rect_draw(s_ui.vw, s_ui.vh, x, y, w, 2.0f, br);
    hud_rect_draw(s_ui.vw, s_ui.vh, x, y + h - 2.0f, w, 2.0f, br);
    hud_rect_draw(s_ui.vw, s_ui.vh, x, y, 2.0f, h, br);
    hud_rect_draw(s_ui.vw, s_ui.vh, x + w - 2.0f, y, 2.0f, h, br);
}

// Screen start: big title at the top center, panel at the bottom left.
// items — number of rows in the panel. Returns the item height (for positions).
// The panel accounts for inter-row GAPS (4px between rows): previously only
// the rows were counted, and the last button ("BACK") stuck out of the panel,
// and on low windows — off the screen edge (it could not be clicked).
static float menu_start(const char *title, int items, float pw)
{
    ui_title_big(&s_ui, title);
#ifdef __ANDROID__
    // On a phone the panel is wider and lower — easier for a finger, larger text (scale in hud.c 1.3-2.2)
    pw *= 1.6f;
    if (pw > s_ui.vw * 0.85f) pw = s_ui.vw * 0.85f;
#endif
    float scale = ui_layout_scale(&s_ui);
    float ih = ui_layout_item_h(&s_ui);
    float gap = 4.0f * scale;
    float px = 90.0f;
#ifdef __ANDROID__
    px = s_ui.vw * 0.05f; // 5% of width; on a narrow screen not pressed to the edge
#endif
    float gaps = (float)(items + 1) * gap;
    float panel_h = items * ih + gaps + 24.0f;
    float max_h = s_ui.vh - 24.0f;
    if (panel_h > max_h) panel_h = max_h;
    float py = s_ui.vh - 14.0f - panel_h + 12.0f;
    if (py < 12.0f) py = 12.0f;
    menu_panel(px - 16.0f, py - 12.0f, pw + 32.0f, panel_h);
    ui_col(&s_ui, px, py, pw);
    return ih;
}

// Helpers: widget wrappers over cvars
static void cvar_checkbox(Ui *u, const char *name, const char *label)
{
    int v = cvar_get(name) > 0.5f;
    if (ui_checkbox(u, label, &v))
        cvar_set(name, v ? 1.0f : 0.0f);
}

static void cvar_slider(Ui *u, const char *name, const char *label,
                        float min, float max, const char *fmt)
{
    float v = cvar_get(name);
    if (ui_slider(u, label, &v, min, max, fmt))
        cvar_set(name, v);
}

// --- Screens --------------------------------------------------------------------
static int menu_main(void)
{
    // Engine signature at the bottom right (small font, independent of big)
    const char *credits = "firerender | by Zawor Group";
    float cw = hud_text_width_small(credits);
    const float dim[4] = { 0.62f, 0.66f, 0.74f, 1.0f };
    hud_text_draw_small(s_ui.vw, s_ui.vh,
                        (float)s_ui.vw - cw - 20.0f,
                        (float)s_ui.vh - 18.0f, credits, dim);

    menu_start("FIRERENDER", 4, 320.0f);
    if (ui_button(&s_ui, "НОВАЯ ИГРА")) return SCR_ACT_START;
    if (ui_button(&s_ui, "ВЫБОР КАРТЫ")) return SCR_MAPS;
    if (ui_button(&s_ui, "НАСТРОЙКИ")) return SCR_SETTINGS;
    if (ui_button(&s_ui, "ВЫХОД")) return SCR_ACT_QUIT;
    return s_screen;
}

static int menu_pause(void)
{
    menu_start("ПАУЗА", 4, 320.0f);
    if (ui_button(&s_ui, "ПРОДОЛЖИТЬ")) return SCR_ACT_RESUME;
    if (ui_button(&s_ui, "НАСТРОЙКИ")) return SCR_SETTINGS;
    if (ui_button(&s_ui, "В ГЛАВНОЕ МЕНЮ")) return SCR_ACT_MAIN;
    if (ui_button(&s_ui, "ВЫХОД")) return SCR_ACT_QUIT;
    return s_screen;
}

static int menu_settings(void)
{
    menu_start("НАСТРОЙКИ", 5, 320.0f);
    if (ui_button(&s_ui, "ГРАФИКА")) return SCR_GRAPHICS;
    if (ui_button(&s_ui, "АУДИО")) return SCR_AUDIO;
    if (ui_button(&s_ui, "УПРАВЛЕНИЕ")) return SCR_CONTROLS;
    ui_sep(&s_ui);
    if (ui_button(&s_ui, "НАЗАД")) return SCR_ACT_BACK;
    return s_screen;
}

static int menu_audio(void)
{
    menu_start("АУДИО", 7, 460.0f);
    cvar_slider(&s_ui, "snd_master", "Общая громкость", 0.0f, 1.0f, "%.2f");
    cvar_slider(&s_ui, "snd_sfx", "Звуки (шаги, эффекты)", 0.0f, 1.0f, "%.2f");
    cvar_slider(&s_ui, "snd_dialog", "Диалоги", 0.0f, 1.0f, "%.2f");
    cvar_slider(&s_ui, "snd_music", "Музыка", 0.0f, 1.0f, "%.2f");
    ui_sep(&s_ui);
    if (ui_button(&s_ui, "НАЗАД")) return SCR_ACT_BACK;
    return s_screen;
}

// Quality presets: one click applies a coherent graphics set. Tuned for
// weak laptops (iGPU): grass is the main hog (fill-bound), so a preset cuts
// its density first; MSAA/bloom/clouds are also expensive on iGPU
// (all passes are fullscreen — the cost grows with pixels).
// Quality presets: one click applies a coherent graphics set. Tuned for
// weak laptops (iGPU): grass is the main hog (fill-bound), so a preset cuts
// its density first; MSAA/bloom/clouds are also expensive on iGPU
// (all passes are fullscreen — the cost grows with pixels).
//                 msaa gi  shadow wheat grass dand  ssr  bloom clouds fog  gi_sc gi_rays gi_steps rscale vegvis minpix terrLOD simple falloff cheapfog ldr
static const float GFX_PRESETS[][21] = {
    { 0,    0,   3,     15,   1.0f, 0.0f, 0,   0,    0,     2,   0.25f, 4,  6,  1.0f,  0.45f, 4,     60,   1, 0.5f,  1, 1 }, // TATER (potato PC)
    { 4,    1,   1,     80,   5.0f, 0.05f, 0,  1,    0.15f, 1,   0.5f,  6,  10, 1.0f,  1.0f,  1,     0,    0, 1.0f,  0, 0 }, // MEDIUM
    { 8,    1,   1,     140,  10.0f, 0.10f, 1,  1,    0.30f, 1,   0.5f,  8,  12, 1.0f,  1.0f,  1,     0,    0, 1.0f,  0, 0 }, // ULTRA
};
static const char *const GFX_PRESET_NAMES[] = { "КАРТОШКА", "СРЕДНИЕ", "УЛЬТРА" };

// The fps-reduction package "Simplified render" (checkbox in the menu + TATER):
// scene scale, wheat draw distance, min-pixel culler, terrain detail LOD.
// Does NOT touch densities and effects — those are separate settings.
static void simple_render_apply(int on)
{
    cvar_set("r_simple_render", on ? 1.0f : 0.0f);
#ifdef __ANDROID__
    cvar_set("r_render_scale", on ? 0.60f : 1.0f); // aggressive — 60% (saves 64% of pixels), yet not blurry
#else
    cvar_set("r_render_scale", 1.0f);
#endif
    cvar_set("r_scene_ldr", on ? 1.0f : 0.0f);
    // Grass Z-prepass cuts overdraw in the most expensive shader of the frame —
    // the picture is identical, so it goes into the "Simplified render" package
    // (TATER, MSAA off). On MEDIUM/ULTRA (MSAA on) the pass itself does not activate.
    cvar_set("r_veg_zprepass", on ? 1.0f : 0.0f);
    if (on) {
#ifdef __ANDROID__
        // Wildly simplified but not empty: the field is sparse, but bushes start earlier and wider (lod_shift)
        cvar_set("r_veg_vis_mult", 0.28f);
        cvar_set("r_veg_minpix", 6.0f);
        cvar_set("r_terr_detail_dist", 30.0f);
        cvar_set("r_veg_falloff_mult", 0.32f);
        cvar_set("r_veg_regen_budget", 3.0f);
        cvar_set("r_veg_dyn_mult", 0.6f);   // load grass over 60% of the radius
        cvar_set("r_veg_lod_shift", 1.5f);
        cvar_set("r_wheat_density", 6.0f);
        cvar_set("r_grass_density", 0.6f);
#else
        cvar_set("r_veg_vis_mult", 0.45f);
        cvar_set("r_veg_minpix", 4.0f);     // cull subpixel sprites harder
        cvar_set("r_terr_detail_dist", 60.0f);
        cvar_set("r_veg_falloff_mult", 0.5f);
        cvar_set("r_veg_regen_budget", 6.0f);
        cvar_set("r_veg_dyn_mult", 0.7f);   // dynamic grass over 70% of the radius (48→34 m)
        cvar_set("r_veg_lod_shift", 0.0f);
#endif
    } else {
        cvar_set("r_veg_vis_mult", 1.0f);
        cvar_set("r_veg_minpix", 1.0f);
        cvar_set("r_terr_detail_dist", 0.0f);
        cvar_set("r_veg_falloff_mult", 1.0f);
        cvar_set("r_veg_dyn_mult", 1.0f);
        // Dense Ultra chunks are generated progressively: the previous 4096 allowed
        // the whole queue in one frame and caused regular frame hitches during flight.
        cvar_set("r_veg_regen_budget", 2.0f);
        cvar_set("r_veg_lod_shift", 0.0f);
    }
}

static void gfx_preset_apply_values(int level)
{
    if (level < 0) level = 0;
    if (level > 2) level = 2;
    static const char *const order[] = {
        "r_msaa", "r_gi", "r_shadows_mode", "r_wheat_density",
        "r_grass_density", "r_dandelion_density", "r_ssr",
        "r_bloom", "r_clouds", "r_fog_mode",
        "r_gi_scale", "r_gi_rays", "r_gi_steps", "r_render_scale",
        "r_veg_vis_mult", "r_veg_minpix", "r_terr_detail_dist",
        "r_simple_render", "r_veg_falloff_mult", "r_cheap_fog",
        "r_scene_ldr"
    };
    for (int k = 0; k < 21; k++)
        cvar_set(order[k], GFX_PRESETS[level][k]);
    simple_render_apply(GFX_PRESETS[level][17] > 0.5f);
#ifdef __ANDROID__
    if (level == 0) {
        // Wildly simplified but not bare: keep a sparse field (6/0.6) with early LOD — wider bushes fill the emptiness
        cvar_set("r_wheat_density", 6.0f);
        cvar_set("r_grass_density", 0.6f);
        cvar_set("r_dandelion_density", 0.0f);
        cvar_set("r_render_scale", 0.60f);
        cvar_set("r_veg_vis_mult", 0.28f);
        cvar_set("r_veg_minpix", 6.0f);
        cvar_set("r_veg_lod_shift", 1.5f);
        cvar_set("r_shadows_mode", 3.0f); // blob spots — cheap and not bare
        cvar_set("r_fog_mode", 2.0f); // simple fog — 1 mix, no 3D
        cvar_set("r_bloom", 0.0f);
        cvar_set("r_gi", 0.0f);
        cvar_set("r_ssr", 0.0f);
        cvar_set("r_clouds", 0.0f);
    }
#endif
}

void app_apply_benchmark_preset(int level)
{
    if (level < 0) level = 0;
    if (level > 2) level = 2;
    gfx_preset_apply_values(level);
    cvar_set("r_render_scale", 1.0f);
    cvar_set("r_dynamic_resolution", 0.0f);
    cvar_set("r_lowend_profile", 0.0f);
    cvar_set("r_vsync", 0.0f);
    cvar_set("host_framerate", 0.0f);
    cvar_set("profiler", 0.0f);
    // FR_MSAA — bench-only override of anti-aliasing for A/B measurements
    // (applied before the map loads; the map may override it with its own cvars).
    const char *m = getenv("FR_MSAA");
    if (m) cvar_set("r_msaa", (float)atoi(m));
    SDL_Log("BENCH preset: %s, native scale", GFX_PRESET_NAMES[level]);
}

static void gfx_preset_apply(int level)
{
    if (level < 0) level = 0;
    if (level > 2) level = 2;
    gfx_preset_apply_values(level);
    console_save_config();
    console_print("пресет графики: %s", GFX_PRESET_NAMES[level]);
}

// Current preset from cvars (exact match on all 17 values);
// 0..2 — matched, -1 — the user tweaked manually ("custom" settings)
static int gfx_preset_current(void)
{
    static const char *const cvars[] = {
        "r_msaa", "r_gi", "r_shadows_mode", "r_wheat_density",
        "r_grass_density", "r_dandelion_density", "r_ssr",
        "r_bloom", "r_clouds", "r_fog_mode",
        "r_gi_scale", "r_gi_rays", "r_gi_steps", "r_render_scale",
        "r_veg_vis_mult", "r_veg_minpix", "r_terr_detail_dist",
        "r_simple_render", "r_veg_falloff_mult", "r_cheap_fog",
        "r_scene_ldr"
    };
    for (int p = 0; p < 3; p++) {
        int match = 1;
        for (int k = 0; k < 21 && match; k++)
            if (cvar_get(cvars[k]) < GFX_PRESETS[p][k] - 0.01f ||
                cvar_get(cvars[k]) > GFX_PRESETS[p][k] + 0.01f) match = 0;
        if (match) return p;
    }
    return -1;
}

// A row of three preset buttons: the active one is underlined in yellow below.
// Clicking a non-active one applies the preset; clicking the active one — nothing.
static void menu_preset_row(void)
{
    const float ih = s_ui.font_h + 10.0f;
    const float gap = 8.0f;
    float total = s_ui.col_w < 520.0f ? s_ui.col_w : 520.0f;
    float x0 = s_ui.cx + (s_ui.col_w - total) * 0.5f;
    const float bw = (total - 2.0f * gap) / 3.0f;
    int cur = gfx_preset_current();
    for (int i = 0; i < 3; i++) {
        float x = x0 + i * (bw + gap);
        if (ui_button_at(&s_ui, x, s_ui.cy, bw, ih, GFX_PRESET_NAMES[i])
            && i != cur)
            gfx_preset_apply(i);
        if (i == cur) {   // yellow line under the active preset
            const float hl[4] = { 1.0f, 0.78f, 0.20f, 1.0f };
            hud_rect_draw(s_ui.vw, s_ui.vh, x + 14.0f, s_ui.cy + ih - 3.0f,
                          bw - 28.0f, 2.0f, hl);
        }
    }
    s_ui.cy += ih + 2.0f;
}

static int menu_graphics(void)
{
    // Compact two-column panel: presets on top, left — screen and detail,
    // right — effects, bottom — reset and back in a single row.
    const float pw = 780.0f;
    menu_start("ГРАФИКА", cvar_get("r_bloom") > 0.5f ? 11 : 10, pw);
    const float px = 90.0f;
    const float half = pw * 0.5f;
    const float ih = s_ui.font_h + 10.0f;

    menu_preset_row();

    const float top = s_ui.cy;
    // LEFT column: screen + detail
    ui_col(&s_ui, px, top, half - 14.0f);
    cvar_checkbox(&s_ui, "r_fullscreen", "Полноэкранный режим");
    cvar_checkbox(&s_ui, "r_vsync", "Вертикальная синхронизация");
    cvar_slider(&s_ui, "fov", "Угол обзора", 5.0f, 120.0f, "%.0f");
    cvar_slider(&s_ui, "r_wheat_density", "Плотность пшеницы", 0.0f, 200.0f, "%.0f");
    cvar_slider(&s_ui, "r_grass_density", "Плотность травы", 0.0f, 10.0f, "%.1f");
    cvar_slider(&s_ui, "r_render_scale", "Масштаб рендера", 0.4f, 1.0f, "%.2f");
    cvar_slider(&s_ui, "r_gamma", "Гамма", 0.5f, 2.5f, "%.2f");
    float bottom = s_ui.cy;

    // RIGHT column: quality and effects
    ui_col(&s_ui, px + half, top, half - 14.0f);
    {
        static const int vals[] = { 0, 4, 16 };
        static const char *names[] = { "Выкл", "4x MSAA", "16x MSAA" };
        int m = (int)(cvar_get("r_msaa") + 0.5f);
        int idx = 0;
        for (int i = 0; i < 3; i++)
            if (vals[i] <= m) idx = i;
        char lbl[96];
        snprintf(lbl, sizeof lbl, "Сглаживание: %s", names[idx]);
        if (ui_button(&s_ui, lbl))
            cvar_set("r_msaa", (float)vals[(idx + 1) % 3]);
    }
    {
        static const char *names[] = {
            "Выкл", "Динамические (CSM)", "Статические", "Пятна (blob)"
        };
        int m = (int)cvar_get("r_shadows_mode");
        if (m < 0) m = 0;
        if (m > 3) m = 3;
        char lbl[96];
        snprintf(lbl, sizeof lbl, "Тени: %s", names[m]);
        if (ui_button(&s_ui, lbl))
            cvar_set("r_shadows_mode", (float)((m + 1) % 4));
    }
    {
        static const char *names[] = { "Выкл", "Объёмный", "Простой" };
        int m = (int)cvar_get("r_fog_mode");
        if (m < 0) m = 0;
        if (m > 2) m = 2;
        char lbl[96];
        snprintf(lbl, sizeof lbl, "Туман: %s", names[m]);
        if (ui_button(&s_ui, lbl))
            cvar_set("r_fog_mode", (float)((m + 1) % 3));
    }
    cvar_checkbox(&s_ui, "r_gi", "Глобальное освещение (GI)");
    cvar_checkbox(&s_ui, "r_ssr", "Отражения луж (SSR)");
    cvar_checkbox(&s_ui, "r_bloom", "Свечение (bloom)");
    if (cvar_get("r_bloom") > 0.5f)   // strength is not needed with bloom off
        cvar_slider(&s_ui, "r_bloom_strength", "Сила свечения", 0.0f, 1.5f, "%.2f");
    // Simplified render: the fps-reduction package behind one flag (scene scale,
    // wheat distance, min-pixel, terrain detail LOD) — placed at the very end
    {
        int sr = cvar_get("r_simple_render") > 0.5f;
        if (ui_checkbox(&s_ui, "Упрощённый рендер (слабый GPU)", &sr))
            simple_render_apply(sr);
    }
    if (s_ui.cy > bottom) bottom = s_ui.cy;

    // Bottom: reset and back — in one row
    s_ui.cy = bottom + 8.0f;
    if (ui_button_at(&s_ui, px, s_ui.cy, half - 14.0f, ih, "НАЗАД"))
        return SCR_ACT_BACK;
    if (ui_button_at(&s_ui, px + half, s_ui.cy, half - 14.0f, ih,
                     "СБРОС ГРАФИКИ")) {
        int n = cvar_reset_graphics();
        console_save_config();          // straight to config.cfg (defaults are not written)
        console_print("графика: сброшено к дефолтам (%d cvar)", n);
    }
    return s_screen;
}

static int menu_controls(void)
{
    menu_start("УПРАВЛЕНИЕ", 9, 460.0f);
    // Mouse sensitivity in convenient units: 1.0 = default (0.0025
    // rad/pixel, cvar cl_sens), range x0.1..x10.
    {
        float v = cvar_get("cl_sens") / 0.0025f;   // in "default" units
        if (ui_slider(&s_ui, "Чувствительность мыши", &v, 0.1f, 10.0f, "%.2f"))
            cvar_set("cl_sens", v * 0.0025f);
    }
    // View interpolation (cvar m_smooth): 0 — raw input, 1 — strong
    // smoothing (a smooth "floating" look).
    cvar_slider(&s_ui, "m_smooth", "Сглаживание мыши (интерполяция)", 0.0f, 1.0f, "%.2f");
    cvar_checkbox(&s_ui, "m_invert", "Инверсия мыши (Y)");
    ui_sep(&s_ui);
    if (ui_button(&s_ui, "КЛАВИШИ (переназначить)")) return SCR_KEYS;
    if (ui_button(&s_ui, "СБРОС УПРАВЛЕНИЯ")) {
        binds_reset();
        console_save_config();      // straight to config.cfg
        console_print("binds reset to defaults");
    }
    ui_sep(&s_ui);
    if (ui_button(&s_ui, "НАЗАД")) return SCR_ACT_BACK;
    return s_screen;
}

static int menu_keys(void)
{
    // Count non-standard binds (they are shown on the right)
    int extras = 0;
    for (int i = 0; i < binds_count(); i++)
        if (!bind_is_standard(binds_at(i)->cmd)) extras++;
    menu_start("КЛАВИШИ", (int)BIND_ACTIONS_N + 2, 460.0f);
    if (s_rec >= 0)
        ui_text(&s_ui, "Нажмите клавишу... (Esc — отмена)");
    for (int i = 0; i < (int)BIND_ACTIONS_N; i++) {
        SDL_Scancode sc = binds_key_for_cmd(s_bind_actions[i].cmd);
        const char *kn = (sc != SDL_SCANCODE_UNKNOWN)
                             ? binds_key_name(sc) : NULL;
        if (ui_key_slot(&s_ui, s_bind_actions[i].desc, kn))
            s_rec = i;
    }
    ui_sep(&s_ui);
    if (ui_button(&s_ui, "НАЗАД")) return SCR_ACT_BACK;

    // Panel on the right: binds from config.cfg that are not in the menu
    if (extras > 0) {
        const float ih = s_ui.font_h + 9.0f;
        const float px = 90.0f + 460.0f + 56.0f;
        float py = s_ui.vh - 26.0f - (float)(extras + 2) * ih;
        if (py < 12.0f) py = 12.0f;
        menu_panel(px - 14.0f, py - 14.0f, 470.0f,
                   (float)(extras + 2) * ih + 28.0f);
        const float wc[4] = C_WHITE;
        const float dc[4] = C_DIM;
        hud_text_draw(s_ui.vw, s_ui.vh, px, py + hud_text_ascent(),
                      "ДРУГИЕ БИНДЫ", wc);
        py += ih * 1.4f;
        for (int i = 0; i < binds_count(); i++) {
            const Bind *b = binds_at(i);
            if (bind_is_standard(b->cmd)) continue;
            char line[256];
            snprintf(line, sizeof line, "%-16s  %s", binds_key_name(b->sc),
                     b->cmd);
            hud_text_draw(s_ui.vw, s_ui.vh, px, py + hud_text_ascent(),
                          line, dc);
            py += ih;
        }
    }
    return s_screen;
}

// Bind recording: the main loop routes KEYDOWN here
int app_bind_key(SDL_Scancode sc)
{
    if (s_screen != SCR_KEYS || s_rec < 0) return 0;
    if (sc == SDL_SCANCODE_ESCAPE) { s_rec = -1; return 1; }
    if (sc != SDL_SCANCODE_UNKNOWN) {
        // Reassignment: unbind this command from its PREVIOUS keys — otherwise
        // the old bind would linger (e.g., demo on Q).
        const char *cmd = s_bind_actions[s_rec].cmd;
        SDL_Scancode olds[BINDS_MAX];
        int nold = 0;
        for (int i = 0; i < binds_count(); i++) {
            const Bind *b = binds_at(i);
            if (b->sc != sc && strcmp(b->cmd, cmd) == 0 && nold < BINDS_MAX)
                olds[nold++] = b->sc;
        }
        for (int i = 0; i < nold; i++) binds_clear(olds[i]);
        binds_set(sc, cmd);
        console_save_config();          // straight to config.cfg
    }
    s_rec = -1;
    return 1;
}

// Map selection screen: the map/ folder + maps/*.zip
static int menu_maps(void)
{
    maps_scan();
    float row = ui_layout_item_h(&s_ui) + 4.0f * ui_layout_scale(&s_ui);
    int per_page = (int)(((float)s_ui.vh - 150.0f) / row) - 3;
    if (per_page < 3) per_page = 3;
    if (per_page > 12) per_page = 12;
    int pages = (s_maps_n + per_page - 1) / per_page;
    if (pages < 1) pages = 1;
    if (s_maps_page >= pages) s_maps_page = pages - 1;
    int first = s_maps_page * per_page;
    int last = first + per_page;
    if (last > s_maps_n) last = s_maps_n;
    int nav = pages > 1 ? 2 : 0;
    menu_start("ВЫБОР КАРТЫ", (last - first) + nav + 2, 420.0f);
    const char *cur = maps_current();
    for (int i = first; i < last; i++) {
        char label[96];
        snprintf(label, sizeof label, "%s%s", s_maps[i],
                 strcmp(s_maps[i], cur) == 0 ? "  *" : "");
        if (ui_button(&s_ui, label)) {
            snprintf(game_instance()->map_name, 64, "%s", s_maps[i]);
            console_save_config();
            SDL_Log("Map selected: %s", s_maps[i]);
            return SCR_ACT_BACK;
        }
    }
    if (pages > 1) {
        char page[64];
        snprintf(page, sizeof page, "<  СТРАНИЦА %d/%d  >", s_maps_page + 1, pages);
        if (ui_button(&s_ui, page)) s_maps_page = (s_maps_page + 1) % pages;
        if (ui_button(&s_ui, "ПРЕДЫДУЩАЯ СТРАНИЦА"))
            s_maps_page = (s_maps_page + pages - 1) % pages;
    }
    ui_sep(&s_ui);
    if (ui_button(&s_ui, "НАЗАД")) return SCR_ACT_BACK;
    return s_screen;
}

// Where "BACK" leads from the current screen
static int app_back_target(void)
{
    if (s_screen == SCR_GRAPHICS || s_screen == SCR_AUDIO ||
        s_screen == SCR_CONTROLS || s_screen == SCR_KEYS)
        return SCR_SETTINGS;
    if (s_screen == SCR_MAPS)
        return SCR_MAIN;
    if (s_screen == SCR_SETTINGS)
        return (s_state == APP_PAUSE) ? SCR_PAUSE : SCR_MAIN;
    if (s_screen == SCR_PAUSE)
        return SCR_ACT_RESUME;
    return SCR_MAIN;
}

// --- State -----------------------------------------------------------------
AppState app_state(void)
{
    return s_state;
}

void app_set_state(AppState s)
{
    s_state = s;
}

void app_escape(void)
{
    if (s_state == APP_PLAY) {
        s_state = APP_PAUSE;             // game -> pause
        s_screen = SCR_PAUSE;
    } else if (s_state == APP_PAUSE) {
        s_state = APP_PLAY;              // pause -> game
    } else if (s_screen != SCR_MAIN) {
        int back = app_back_target();    // menus: back across screens
        if (back == SCR_ACT_RESUME) { s_state = APP_PLAY; return; }
        s_screen = back;
    }
    ui_focus_first(&s_ui);
}

// --- Render --------------------------------------------------------------------
int app_menu(int vw, int vh)
{
    int big = (s_screen == SCR_MAIN || s_screen == SCR_PAUSE ||
               s_screen == SCR_SETTINGS);
    // Menu input is disabled while the console is open — it takes the keyboard
    ui_begin(&s_ui, vw, vh, big, !console_active());
    app_draw_bg(vw, vh);        // img/menu.png photo (or darkening) — background

    int next = s_screen;
    switch (s_screen) {
    case SCR_MAIN:     next = menu_main(); break;
    case SCR_PAUSE:    next = menu_pause(); break;
    case SCR_SETTINGS: next = menu_settings(); break;
    case SCR_GRAPHICS: next = menu_graphics(); break;
    case SCR_AUDIO:    next = menu_audio(); break;
    case SCR_CONTROLS: next = menu_controls(); break;
    case SCR_KEYS:     next = menu_keys(); break;
    case SCR_MAPS:     next = menu_maps(); break;
    default: break;
    }
    ui_end(&s_ui);

    // Transition handling: simple and predictable
    int act = APP_ACT_NONE;
    if (next != s_screen) {
        // Leaving the settings screens — settings to disk
        if (s_screen == SCR_SETTINGS || s_screen == SCR_GRAPHICS ||
            s_screen == SCR_AUDIO || s_screen == SCR_CONTROLS ||
            s_screen == SCR_KEYS || s_screen == SCR_MAPS)
            console_save_config();
        if (next != SCR_KEYS) s_rec = -1;   // left bind recording

        switch (next) {
        case SCR_ACT_START:
            act = APP_ACT_START;
            next = (s_state == APP_PAUSE) ? SCR_PAUSE : SCR_MAIN;
            break;
        case SCR_ACT_RESUME:
            act = APP_ACT_RESUME;
            next = (s_state == APP_PAUSE) ? SCR_PAUSE : SCR_MAIN;
            break;
        case SCR_ACT_QUIT:
            act = APP_ACT_QUIT;
            next = s_screen;
            break;
        case SCR_ACT_MAIN:
            act = APP_ACT_MAIN;          // to the main menu: the world gets unloaded
            s_state = APP_MENU;
            next = SCR_MAIN;
            break;
        case SCR_ACT_BACK:
            next = app_back_target();
            if (next == SCR_ACT_RESUME) {   // back from pause = resume
                act = APP_ACT_RESUME;
                next = SCR_PAUSE;
            }
            break;
        default:
            break;                       // regular screen -> screen transition
        }
        s_screen = next;
        ui_focus_first(&s_ui);           // focus the first item of the new screen
    }
    return act;
}
