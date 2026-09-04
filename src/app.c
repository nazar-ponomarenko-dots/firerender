// ============================================================================
//  app.c — менеджер состояний и экраны меню (см. app.h)
// ----------------------------------------------------------------------------
//  Главное меню: логотип слева по центру, пункты внизу слева (стиль HL1).
//  Всё, что открывается из меню (настройки, пауза) — ОКНА с рамкой,
//  шапкой-заголовком и крестиком закрытия. Настройки пишут в cvar'ы сразу,
//  config.cfg сохраняется при уходе с экранов настроек и при выходе.
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

// Экраны меню
enum {
    SCR_MAIN = 0,
    SCR_PAUSE,
    SCR_SETTINGS,
    SCR_GRAPHICS,
    SCR_AUDIO,
    SCR_CONTROLS,
    SCR_KEYS,
    SCR_MAPS,
    // Коды действий (возвращаются экран-функциями)
    SCR_ACT_START = 100,   // новая игра (загрузка мира)
    SCR_ACT_RESUME,        // продолжить
    SCR_ACT_QUIT,          // выход
    SCR_ACT_BACK,          // назад по экранам
    SCR_ACT_MAIN,          // в главное меню (из паузы)
};

// Действия для экрана "Клавиши" (команда = бинд). Только важные;
// всё остальное, что привязано в config.cfg, показывается отдельно
// (панель "ДРУГИЕ БИНДЫ" справа).
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

// Бинд из меню (стандартный)? Нет — попадёт в "ДРУГИЕ БИНДЫ" справа.
static int bind_is_standard(const char *cmd)
{
    for (int i = 0; i < (int)BIND_ACTIONS_N; i++)
        if (strcmp(s_bind_actions[i].cmd, cmd) == 0) return 1;
    return 0;
}

static AppState s_state = APP_BOOT;
static int s_screen = SCR_MAIN;
static int s_rec = -1;      // индекс действия в записи (экран "Клавиши"), -1 нет
static Ui s_ui;

// --- Список карт: только maps/*.zip (карты редактируются через распаковку) ------
#define MAPS_MAX 32
static char s_maps[MAPS_MAX][64];
static int s_maps_n = 0;

static void maps_scan(void)
{
    s_maps_n = 0;
    // Ищем maps/ в рабочей папке и на уровень выше (запуск из build/ и т.п.)
    const char *dirs[] = { "maps", "../maps" };
    for (int d = 0; d < 2 && s_maps_n < MAPS_MAX; d++) {
#ifdef _WIN32
        (void)dirs;   // Windows: задел (пока только папка map/)
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

// Список карт в консоль (команда maps)
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

// Фон меню: картинка img/menu.png (как в HL1 — меню поверх фотки).
static GLuint s_bg_tex = 0;
static GLuint s_bg_prog = 0, s_bg_vao = 0, s_bg_vbo = 0;
static GLint s_bg_u_basemap, s_bg_u_color, s_bg_u_usetex, s_bg_u_usemarker;

// --- Фон-фотка -----------------------------------------------------------------
void app_init(void)
{
    s_state = APP_BOOT;   // стартовая фаза: системы поднимает boot_init (main.c)
    s_screen = SCR_MAIN;
    memset(&s_ui, 0, sizeof s_ui);
    s_ui.active = -1;

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
            // Квад во весь экран. V перевёрнут: PNG-строки грузятся сверху
            // вниз (v=0 — ВЕРХ изображения), а в OpenGL v=0 — низ экрана.
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
    // В паузе фон — ЗАТЕМНЁННАЯ ИГРА (сцена отрисована главным циклом).
    // В главном меню (мир не загружен) — фотка img/menu.png.
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
    // Затемнение под меню — текст читается на любом фоне
    float a = (s_state == APP_PAUSE) ? 0.55f
            : (app_bg_has() ? 0.30f : 0.55f);
    const float dim[4] = { 0, 0, 0, a };
    hud_rect_draw(vw, vh, 0, 0, (float)vw, (float)vh, dim);
}

// Панель: полупрозрачная подложка с рамкой
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

// Начало экрана: крупный заголовок сверху по центру, панель внизу слева.
// items — число строк в панели. Возвращает высоту пункта (для позиций).
static float menu_start(const char *title, int items, float pw)
{
    ui_title_big(&s_ui, title);
    float ih = s_ui.font_h + 14.0f;   // высота одного пункта
    float px = 90.0f;
    float py = s_ui.vh - 26.0f - items * ih;
    if (py < 12.0f) py = 12.0f;       // маленькое окно: панель не за экран
    menu_panel(px - 16.0f, py - 12.0f, pw + 32.0f, items * ih + 24.0f);
    ui_col(&s_ui, px, py, pw);
    return ih;
}

// Вспомогательные: виджет-обёртки над cvar'ами
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

// --- Экраны --------------------------------------------------------------------
static int menu_main(void)
{
    // Подпись движка справа снизу (мелкий шрифт, независимо от big)
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

static int menu_graphics(void)
{
    menu_start("ГРАФИКА", 16, 460.0f);
    cvar_checkbox(&s_ui, "r_fullscreen", "Полноэкранный режим");
    cvar_checkbox(&s_ui, "r_vsync", "Вертикальная синхронизация");
    cvar_slider(&s_ui, "fov", "Угол обзора", 5.0f, 120.0f, "%.0f");
    // Тени: 4 режима (клик — переключение)
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
    // Туман: 3 режима (клик — переключение)
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
    cvar_checkbox(&s_ui, "r_bloom", "Свечение (bloom)");
    cvar_slider(&s_ui, "r_bloom_strength", "Сила свечения", 0.0f, 1.5f, "%.2f");
    cvar_slider(&s_ui, "r_gamma", "Гамма", 0.5f, 2.5f, "%.2f");
    // SSR-отражения на мокром рельефе (сила — не в меню, cvar r_ssr_strength).
    cvar_checkbox(&s_ui, "r_ssr", "Отражения луж (SSR)");
    // Глобальное освещение (GI, аналог Lumen): вкл/выкл. Тонкие настройки —
    // в консоли: r_gi_strength, r_gi_ao, r_gi_skylight и пр.
    cvar_checkbox(&s_ui, "r_gi", "Глобальное освещение (GI)");
    // MSAA-сглаживание: клик циклит 0/2/4/8/16 (применяется сразу)
    {
        static const int vals[] = { 0, 2, 4, 8, 16 };
        static const char *names[] = {
            "Выкл", "2x MSAA", "4x MSAA", "8x MSAA", "16x MSAA"
        };
        int m = (int)(cvar_get("r_msaa") + 0.5f);
        int idx = 0;
        for (int i = 0; i < 5; i++)
            if (vals[i] <= m) idx = i;
        char lbl[96];
        snprintf(lbl, sizeof lbl, "Сглаживание: %s", names[idx]);
        if (ui_button(&s_ui, lbl))
            cvar_set("r_msaa", (float)vals[(idx + 1) % 5]);
    }
    cvar_slider(&s_ui, "r_wheat_density", "Плотность пшеницы", 0.0f, 200.0f, "%.0f");
    cvar_slider(&s_ui, "r_grass_density", "Плотность травы", 0.0f, 10.0f, "%.1f");
    ui_sep(&s_ui);
    if (ui_button(&s_ui, "СБРОСИТЬ НАСТРОЙКИ ГРАФИКИ")) {
        int n = cvar_reset_graphics();
        console_save_config();          // сразу в config.cfg (дефолты не пишутся)
        console_print("графика: сброшено к дефолтам (%d cvar)", n);
    }
    if (ui_button(&s_ui, "НАЗАД")) return SCR_ACT_BACK;
    return s_screen;
}

static int menu_controls(void)
{
    menu_start("УПРАВЛЕНИЕ", 9, 460.0f);
    // Чувствительность мыши в удобных единицах: 1.0 = дефолт (0.0025
    // рад/пиксель, cvar cl_sens), диапазон x0.1..x10.
    {
        float v = cvar_get("cl_sens") / 0.0025f;   // в единицах «дефолта»
        if (ui_slider(&s_ui, "Чувствительность мыши", &v, 0.1f, 10.0f, "%.2f"))
            cvar_set("cl_sens", v * 0.0025f);
    }
    // Интерполяция взгляда (cvar m_smooth): 0 — обычный ввод, 1 — сильное
    // сглаживание (плавный «плывущий» взгляд).
    cvar_slider(&s_ui, "m_smooth", "Сглаживание мыши (интерполяция)", 0.0f, 1.0f, "%.2f");
    cvar_checkbox(&s_ui, "m_invert", "Инверсия мыши (Y)");
    ui_sep(&s_ui);
    if (ui_button(&s_ui, "КЛАВИШИ (переназначить)")) return SCR_KEYS;
    if (ui_button(&s_ui, "СБРОС УПРАВЛЕНИЯ")) {
        binds_reset();
        console_save_config();      // сразу в config.cfg
        console_print("binds reset to defaults");
    }
    ui_sep(&s_ui);
    if (ui_button(&s_ui, "НАЗАД")) return SCR_ACT_BACK;
    return s_screen;
}

static int menu_keys(void)
{
    // Считаем нестандартные бинды (показываются справа)
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

    // Панель справа: бинды из config.cfg, которых нет в меню
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

// Запись бинда: главный цикл отдаёт сюда KEYDOWN
int app_bind_key(SDL_Scancode sc)
{
    if (s_screen != SCR_KEYS || s_rec < 0) return 0;
    if (sc == SDL_SCANCODE_ESCAPE) { s_rec = -1; return 1; }
    if (sc != SDL_SCANCODE_UNKNOWN) {
        // Переназначение: снять эту команду с ПРЕЖНИХ клавиш — иначе
        // старый бинд останется висеть (напр. demo на Q).
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
        console_save_config();          // сразу в config.cfg
    }
    s_rec = -1;
    return 1;
}

// Экран "Выбор карты": папка map/ + maps/*.zip
static int menu_maps(void)
{
    maps_scan();
    menu_start("ВЫБОР КАРТЫ", s_maps_n + 2, 420.0f);
    const char *cur = maps_current();
    for (int i = 0; i < s_maps_n; i++) {
        char label[96];
        snprintf(label, sizeof label, "%s%s", s_maps[i],
                 strcmp(s_maps[i], cur) == 0 ? "  *" : "");
        if (ui_button(&s_ui, label)) {
            snprintf(game_instance()->map_name, 64, "%s", s_maps[i]);
            console_save_config();      // выбор карты в config.cfg
            SDL_Log("Map selected: %s", s_maps[i]);
            return SCR_ACT_BACK;
        }
    }
    ui_sep(&s_ui);
    if (ui_button(&s_ui, "НАЗАД")) return SCR_ACT_BACK;
    return s_screen;
}

// Куда ведёт "НАЗАД" с текущего экрана
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

// --- Состояние -----------------------------------------------------------------
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
        s_state = APP_PAUSE;             // игра -> пауза
        s_screen = SCR_PAUSE;
    } else if (s_state == APP_PAUSE) {
        s_state = APP_PLAY;              // пауза -> игра
    } else if (s_screen != SCR_MAIN) {
        int back = app_back_target();    // меню: назад по экранам
        if (back == SCR_ACT_RESUME) { s_state = APP_PLAY; return; }
        s_screen = back;
    }
    ui_focus_first(&s_ui);
}

// --- Рендер --------------------------------------------------------------------
int app_menu(int vw, int vh)
{
    int big = (s_screen == SCR_MAIN || s_screen == SCR_PAUSE ||
               s_screen == SCR_SETTINGS);
    // Ввод меню выключается при открытой консоли — она забирает клавиатуру
    ui_begin(&s_ui, vw, vh, big, !console_active());
    app_draw_bg(vw, vh);        // фотка img/menu.png (или затемнение) — фон

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

    // Обработка перехода: простой и предсказуемый
    int act = APP_ACT_NONE;
    if (next != s_screen) {
        // Уходим с экранов настроек — настройки на диск
        if (s_screen == SCR_SETTINGS || s_screen == SCR_GRAPHICS ||
            s_screen == SCR_AUDIO || s_screen == SCR_CONTROLS ||
            s_screen == SCR_KEYS || s_screen == SCR_MAPS)
            console_save_config();
        if (next != SCR_KEYS) s_rec = -1;   // вышли из записи бинда

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
            act = APP_ACT_MAIN;          // в главное меню: мир выгрузится
            s_state = APP_MENU;
            next = SCR_MAIN;
            break;
        case SCR_ACT_BACK:
            next = app_back_target();
            if (next == SCR_ACT_RESUME) {   // назад из паузы = продолжить
                act = APP_ACT_RESUME;
                next = SCR_PAUSE;
            }
            break;
        default:
            break;                       // обычный переход экран -> экран
        }
        s_screen = next;
        ui_focus_first(&s_ui);           // фокус на первый пункт нового экрана
    }
    return act;
}
