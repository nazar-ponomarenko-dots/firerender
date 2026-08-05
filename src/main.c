// ============================================================================
//  firerender — 3D-проект на чистом C (SDL3 + OpenGL)
// ----------------------------------------------------------------------------
//  Точка входа: инициализация SDL/GL, меню, загрузка мира и главный цикл.
//  Логика разнесена по модулям:
//    game.c      — состояние мира в структуре Game + жизненный цикл/обновление;
//    commands.c  — консольные команды (регистрация — здесь, в main);
//    renderer.c  — оркестрация рендера (сцена, bloom, туман, оверлеи);
//    input.c     — события главного цикла и режим мыши;
//    world.c     — данные мира (WorldState): высоты, размеры, поле.
//
//  Состояние приложения — ЯВНЫЕ ФАЗЫ (см. app.h): APP_BOOT (поднять системы,
//  один раз в главном цикле) -> APP_MENU/APP_PLAY/APP_PAUSE.
//
//  Мир (карта, рельеф, растительность) грузится ЛЕНИВО — только после
//  нажатия "НОВАЯ ИГРА"/"ПРОДОЛЖИТЬ". В меню работает только шрифт,
//  фон-картинка (img/menu.png) и консоль — 3D не грузится и не рисуется.
//  При выходе в главное меню мир выгружается (память освобождается).
// ============================================================================

#include <SDL3/SDL.h>        // SDL3: окна, события, таймеры, GL-контекст
#include <SDL3/SDL_main.h>   // Переносимый main(): на Windows закрывает
                             // собой WinMain, на Android — точку входа
                             // из Java-обёртки SDLActivity.

#include "gl_loader.h"
#include "world.h"           // g_vw / g_vh — размер окна в пикселях
#include "game.h"
#include "commands.h"
#include "renderer.h"
#include "input.h"
#include "lua_engine.h"
#include "shader.h"   // shader_hot_reload (см. reload_shaders)
#include "app.h"
#include "console.h"
#include "cvars.h"
#include "hud.h"
#include "resources.h"
#include "audio.h"
#include "binds.h"
#include "actions.h"
#include "shadow.h"          // FR_TEST: shadow_toggle, g_shadow_*
#include "vegetation.h"      // FR_TEST: veg_* тумблеры
#include "voxfog.h"          // FR_TEST: voxfog_set
#include "terrain.h"         // sync_cvars: terrain_shadows_bake
#include "sun.h"             // (SunParams*)g->sun_node->params

#include <stdio.h>           // printf (FPS)
#include <stdlib.h>          // getenv
#ifdef _WIN32
#  include <direct.h>        // _chdir
#else
#  include <unistd.h>        // chdir
#endif

// --- Константы окна -----------------------------------------------------------
#define WIN_W 1280          // Ширина окна в пикселях
#define WIN_H 720           // Высота окна в пикселях

static SDL_Window *g_win = NULL;

// --- Cvar'ы -> системы ---------------------------------------------------------
static float s_sync_shadows = -1.0f, s_sync_fog = -1.0f;
static float s_sync_fs = -1.0f, s_sync_vsync = -1.0f, s_sync_wire = -1.0f;
static void sync_cvars(Game *g)
{
    float sh = cvar_get("r_shadows_mode");
    if (sh != s_sync_shadows) {
        s_sync_shadows = sh;
        // CSM рендерится в режимах 1 (только динамика) и 2 (динамика вблизи
        // игрока, статика — по всему миру)
        shadow_set(sh == 1.0f || sh == 2.0f);
        if (sh == 2.0f && g->world_loaded)  // статические: пересчитать сейчас
            terrain_shadows_bake(&g->world, (SunParams *)g->sun_node->params);
        SDL_Log("Shadows mode: %d", (int)sh);
    }
    float fo = cvar_get("r_fog_mode");
    if (fo != s_sync_fog) {
        s_sync_fog = fo;
        voxfog_set_mode((int)fo);
    }
    float fs = cvar_get("r_fullscreen");
    if (fs != s_sync_fs) {
        s_sync_fs = fs;
        if (g_win) SDL_SetWindowFullscreen(g_win, fs > 0.5f);
    }
    float vs = cvar_get("r_vsync");
    if (vs != s_sync_vsync) {
        s_sync_vsync = vs;
        SDL_GL_SetSwapInterval(vs > 0.5f ? 1 : 0);
    }
    float wr = cvar_get("r_wireframe");
    if (wr != s_sync_wire) {
        s_sync_wire = wr;   // применяется в renderer_frame (3D-проход)
        renderer_set_wireframe(wr > 0.5f);
    }
}

// --- Фаза APP_BOOT: инициализация систем (выполняется один раз) ---------------
// Поднимает системы в любом порядке регистраций (console_add_command работает
// и до console_init — см. console.c), грузит config.cfg и переводит
// состояние в APP_MENU (или APP_PLAY при FR_TEST).
static void boot_init(Game *g)
{
    resources_init();            // кеш ресурсов — до любых загрузок
    console_init();              // cvar'ы, команды, вывод SDL_Log в консоль
    audio_init();                // звук: устройство + каналы
    binds_init();                // дефолтные привязки клавиш (HL-стиль)
    actions_register_commands(); // +forward/-forward и прочие +/-команды
    // Команды-действия (см. commands.c) — доступны СРАЗУ, в т.ч. в меню
    console_add_command("toggle_map", "показать/скрыть мини-карту", cmd_toggle_map);
    console_add_command("toggle_hud", "показать/скрыть HUD", cmd_toggle_hud);
    console_add_command("toggle_shadows", "вкл/выкл тени", cmd_toggle_shadows);
    console_add_command("toggle_fog", "вкл/выкл объёмный туман", cmd_toggle_fog);
    console_add_command("demo", "демо-сцена (автополёт, замер FPS)", cmd_demo);
    console_add_command("pause", "пауза / назад в меню", cmd_pause);
    console_add_command("map", "map <имя> — выбрать и запустить карту", cmd_map_select);
    console_add_command("maps", "список карт", cmd_maps_list);
    console_add_command("noclip", "полёт сквозь мир (чит)", cmd_noclip);
    console_add_command("restart", "перезапустить текущую карту", cmd_restart);
    console_add_command("changelevel", "changelevel <карта>", cmd_changelevel);
    console_add_command("r_drawhud", "r_drawhud [0/1] — HUD", cmd_r_drawhud);
    console_add_command("mat_wireframe", "mat_wireframe [0/1]", cmd_mat_wireframe);
    console_add_command("thirdperson", "вид от третьего лица", cmd_thirdperson);
    console_add_command("firstperson", "вид от первого лица", cmd_firstperson);
    console_add_command("ent_create", "ent_create <класс> [x y z]", cmd_ent_create);
    console_add_command("ent_list", "список сущностей", cmd_ent_list);
    console_add_command("snd_test", "проиграть все звуки подряд", cmd_snd_test);
    console_add_command("screenshot", "сохранить кадр в PNG (F12)", cmd_screenshot);
    console_add_command("save", "сохранить позицию игрока (F5)", cmd_save);
    console_add_command("load", "загрузить сохранение (F9)", cmd_load);
    console_add_command("setpos", "setpos x y z [yaw] [pitch] — телепорт", cmd_setpos);
    console_add_command("reload_shaders", "перекомпилировать изменённые шейдеры", cmd_reload_shaders);
    hud_shared_init();           // шрифт/атласы + рендер текста для меню и консоли
    app_init();                  // фон img/menu.png
    console_autoload();          // config.cfg: cvar'ы и бинды пользователя
    game_map_config_load(g);     // + выбранная карта
    lua_engine_init();           // скрипты Lua (после консоли и cvar'ов)
    app_set_state(APP_MENU);     // фаза закончена: меню
}

// --- FR_TEST — тест производительности / самопроверка (после boot_init) --------
// Требует мир: грузим его сразу и входим в игру. 0 — успех, -1 — мир не загрузился.
static int fr_test_apply(Game *g)
{
    const char *fr_test = getenv("FR_TEST");
    if (!fr_test) return 0;
    if (strchr(fr_test, 'x')) console_selftest();
    if (strchr(fr_test, 's')) shadow_toggle();
    if (strchr(fr_test, 'v')) veg_set_visible_test(0);
    if (strchr(fr_test, 'p')) g_shadow_terrain = 0;
    if (strchr(fr_test, 'q')) g_shadow_veg = 0;
    if (strchr(fr_test, 'c')) g_shadow_cull = 0;
    if (strchr(fr_test, 'e')) veg_set_static_main(0);
    if (strchr(fr_test, 'f')) veg_set_dyn_main(0);
    if (strchr(fr_test, 'o')) voxfog_set(1);
    if (game_load_world(g) != 0) {
        SDL_Log("FR_TEST: world load failed");
        return -1;
    }
    // t/k — отключить рельеф/небо (нужны узлы, поэтому после load_world)
    if (strchr(fr_test, 't')) g->terr_node->render = NULL;
    if (strchr(fr_test, 'k')) g->sky_node->render = NULL;
    app_set_state(APP_PLAY);
    SDL_Log("FR_TEST=[%s]", fr_test);
    return 0;
}

int main(int argc, char *argv[])
{
    (void)argc; (void)argv;

    // Если в текущей папке нет данных (map/map.json) — переходим в папку
    // исполняемого файла: двойной клик запускает из любого места.
    FILE *probe = fopen("map/map.json", "rb");
    if (!probe) {
        const char *base = SDL_GetBasePath();
        if (base && base[0]) {
#ifdef _WIN32
            _chdir(base);
#else
            chdir(base);
#endif
        }
        SDL_free((void *)base);
    } else {
        fclose(probe);
    }

    // 1) Инициализация SDL -----------------------------------------------------
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    // 2) Атрибуты GL-контекста -------------------------------------------------
#ifdef USE_GLES_PATH
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
#endif
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

    // 3) Окно и контекст -------------------------------------------------------
    g_win = SDL_CreateWindow("firerender", WIN_W, WIN_H,
                             SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (!g_win) { SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError()); SDL_Quit(); return 1; }
    SDL_GLContext ctx = SDL_GL_CreateContext(g_win);
    if (!ctx) { SDL_Log("SDL_GL_CreateContext failed: %s", SDL_GetError());
                SDL_DestroyWindow(g_win); SDL_Quit(); return 1; }
    SDL_GL_MakeCurrent(g_win, ctx);

    // 4) Загрузка функций GL и vsync -------------------------------------------
    if (load_gl_funcs() != 0) {
        SDL_GL_DestroyContext(ctx); SDL_DestroyWindow(g_win); SDL_Quit(); return 1;
    }
    SDL_GL_SetSwapInterval(0);

    // 5) Состояние мира: системы (консоль/звук/шрифт/команды) поднимает
    //    фаза APP_BOOT — boot_init() в главном цикле ---------------------------
    Game game;
    game_init(&game);            // единственный экземпляр состояния мира

    SDL_Log("Video driver: %s", SDL_GetCurrentVideoDriver());
    SDL_Log("OpenGL: %s", glGetString(GL_VERSION));

    // 6) Вьюпорт = реальный размер окна в пикселях -----------------------------
    int vw = WIN_W, vh = WIN_H;
    SDL_GetWindowSizeInPixels(g_win, &vw, &vh);
    glViewport(0, 0, vw, vh);
    g_vw = vw; g_vh = vh;

    // 7) Отсечение задних граней, блендинг -------------------------------------
    glEnable(GL_CULL_FACE);
    glFrontFace(GL_CCW);
    glCullFace(GL_BACK);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // 8) Ввод: привязка мыши/клавиш к окну. Режим мыши и cvar-синки
    //    применяет главный цикл (после boot_init). ------------------------------
    input_init(g_win, &game);

    // 9) Главный цикл: фаза APP_BOOT (boot_init) -> MENU/PLAY/PAUSE -------------
    Uint64 last = SDL_GetPerformanceCounter();
    float fps_accum = 0;
    int fps_frames = 0;
    bool running = true;
    while (running) {
        Uint64 now = SDL_GetPerformanceCounter();
        float dt = (float)(now - last) / SDL_GetPerformanceFrequency();
        last = now;
        if (dt > 0.1f) dt = 0.1f;
        dt *= cvar_get("sv_timescale");     // sv_timescale — скорость времени

        float inst_fps = 1.0f / (dt > 0.0001f ? dt : 0.0001f);
        if (game.hud) game.hud->fps = game.hud->fps * 0.95f + inst_fps * 0.05f;

        input_process();                    // события: консоль, бинды, ресайз
        if (input_quit()) running = false;

        // Фаза APP_BOOT: поднять системы один раз (в конце — APP_MENU/PLAY)
        if (app_state() == APP_BOOT) {
            boot_init(&game);
            if (fr_test_apply(&game) != 0) goto fail;   // FR_TEST: мир

            if (getenv("FR_CONSOLE")) console_toggle();   // консоль при старте
            // cvar-синки не должны перетирать FR_TEST на первом кадре —
            // примайниваем только то, что трогает FR_TEST (тени/туман).
            // Полноэкран/VSync/каркас применит первый sync_cvars из config.cfg.
            s_sync_shadows = cvar_get("r_shadows_mode");
            s_sync_fog = cvar_get("r_fog_mode");
        }

        // Hot-reload шейдеров: раз в ~30 кадров проверяем mtime файлов
        {
            static int s_reload = 0;
            if (++s_reload >= 30) { s_reload = 0; shader_hot_reload(); }
        }

        game_demo_tick(&game, dt, inst_fps); // демо-сцена (Q): автополёт + FPS
        game_update(&game, dt);             // мир: узлы, триггеры/use, туман
        audio_update();                     // фоновые петли и 3D-источники
        lua_engine_update(dt);              // per-frame колбэк скрипта

        sync_cvars(&game);
        input_sync_mode();

        // Рендер: сцена -> bloom/туман -> оверлеи (см. renderer.c)
        renderer_begin();
        renderer_frame(&game, dt, game.world_loaded &&
                                   (app_state() != APP_MENU || !app_bg_has()));
        renderer_end(&game, dt);

        // Меню (главное/пауза/настройки) — поверх замороженной сцены
        if (app_state() != APP_PLAY) {
            int act = app_menu(g_vw, g_vh);
            if (act == APP_ACT_START || act == APP_ACT_RESUME) {
                if (!game.world_loaded && game_load_world(&game) != 0) {
                    SDL_Log("World load failed");
                    goto fail;
                }
                app_set_state(APP_PLAY);
                if (act == APP_ACT_START) {
                    // Новая игра: камера на спавн, сброс демо.
                    // (если это load из save — телепорт вместо спавна)
                    if (game.pending_spawn) {
                        game_apply_spawn(&game, game.save_pos, game.save_yaw,
                                         game.save_pitch);
                        game.pending_spawn = 0;
                        console_print("load: загружено");
                    } else {
                        game.cam->cam.pos[0] = game.spawn_pos[0];
                        game.cam->cam.pos[1] = game.spawn_pos[1];
                        game.cam->cam.pos[2] = game.spawn_pos[2];
                        game.cam->cam.yaw = game.spawn_yaw;
                        game.cam->cam.pitch = game.spawn_pitch;
                        game.cam->cam.vy = 0;
                        game.cam->cam.vx = game.cam->cam.vz = 0;
                        game_demo_stop(&game);
                        SDL_Log("New game");
                    }
                } else {
                    SDL_Log("Game resumed");
                }
            } else if (act == APP_ACT_MAIN) {
                if (game.world_loaded) game_unload_world(&game);  // в меню
                app_set_state(APP_MENU);
            } else if (act == APP_ACT_QUIT) {
                g_quit_request = 1;
            }
        }
        commands_shot_take(g_vw, g_vh);     // скриншот (F12)
        console_render(g_vw, g_vh);         // консоль — поверх всего (`)

        // FR_SHOT — сохранить кадр в PPM (отладка рендера без Wayland)
        {
            static int shot_frame = 0;
            if (getenv("FR_SHOT") && shot_frame != -1) {
                if (shot_frame == 0) shot_frame = 1;
                else if (shot_frame < 60) shot_frame++;
                if (shot_frame == 60) {
                    unsigned char *px = malloc((size_t)g_vw * g_vh * 3);
                    glReadPixels(0, 0, g_vw, g_vh, GL_RGB, GL_UNSIGNED_BYTE, px);
                    FILE *f = fopen("/tmp/fr_shot.ppm", "wb");
                    if (f) {
                        fprintf(f, "P6\n%d %d\n255\n", g_vw, g_vh);
                        for (int y = 0; y < g_vh; y++)
                            fwrite(px + (size_t)(g_vh - 1 - y) * g_vw * 3, 1,
                                   (size_t)g_vw * 3, f);
                        fclose(f);
                        SDL_Log("FR_SHOT: /tmp/fr_shot.ppm (%dx%d) console=%d",
                                g_vw, g_vh, console_active());
                    }
                    free(px);
                    shot_frame = -1;
                }
            }
        }

        SDL_GL_SwapWindow(g_win);

        // host_framerate — лимит FPS (задержка до следующего кадра)
        {
            float hf = cvar_get("host_framerate");
            if (hf > 0.0f) {
                Uint64 target = (Uint64)(SDL_GetPerformanceFrequency() / hf);
                Uint64 elapsed = SDL_GetPerformanceCounter() - now;
                if (elapsed < target) {
                    Uint32 ms = (Uint32)((target - elapsed) * 1000 /
                                         SDL_GetPerformanceFrequency());
                    if (ms > 0 && ms < 100) SDL_Delay(ms);
                }
            }
        }

        if (g_quit_request) {
            g_quit_request = 0;
            running = false;
        }

        fps_accum += dt;
        if (++fps_frames >= 60) {
            printf("fps: %.0f\n", fps_frames / fps_accum);
            fps_frames = 0;
            fps_accum = 0;
        }
    }

    // 11) Очистка --------------------------------------------------------------
    console_save_config();       // config.cfg: cvar'ы на диск
    game_map_config_save(&game); // + выбранная карта
    game_unload_world(&game);    // выгрузить мир (если загружен)
    audio_cleanup();             // звук: каналы и кеш
    hud_shared_cleanup();        // шрифт/атласы/рендер текста
    resources_shutdown();        // кеш ресурсов
    SDL_GL_DestroyContext(ctx);
    SDL_DestroyWindow(g_win);
    SDL_Quit();
    return 0;

fail:
    SDL_GL_DestroyContext(ctx);
    SDL_DestroyWindow(g_win);
    SDL_Quit();
    return 1;
}
