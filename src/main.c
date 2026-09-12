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

#include <SDL3/SDL.h>
#include <math.h>        // SDL3: окна, события, таймеры, GL-контекст
#include <SDL3/SDL_main.h>   // Переносимый main(): на Windows закрывает
                             // собой WinMain, на Android — точку входа
                             // из Java-обёртки SDLActivity.

#include "gl_loader.h"
#include "world.h"
#include "game.h"
#include "commands.h"
#include "events.h"
#include "renderer.h"
#include "buildnum.h"
#include "input.h"
#include "lua_engine.h"
#include "shader.h"   // shader_hot_reload (см. reload_shaders)
#include "android.h"
#include "app.h"
#include "console.h"
#include "touch.h"
#include "cvars.h"
#include "hud.h"
#include "resources.h"
#include "audio.h"
#include "binds.h"
#include "decals.h"
#include "particles.h"
#include "fx.h"
#include "actions.h"
#include "cmdreg.h"
#include "shadow.h"          // FR_TEST: shadow_toggle, g_shadow_*
#include "vegetation.h"      // FR_TEST: veg_* тумблеры
#include "voxfog.h"          // FR_TEST: voxfog_set
#include "terrain.h"         // sync_cvars: terrain_shadows_bake
#include "sun.h"             // (SunParams*)g->sun_node->params
#include "material.h"        // материал: реестр по имени
#include "physics.h"         // мини-физика (Verlet): шаг в фикс-тике
#include "glb_anim.h"        // рэгдолл: шаг физики скелетов

#include <stdio.h>           // printf (FPS)
#include <stdlib.h>          // getenv
#include <string.h>

#define BENCH_MAX_SAMPLES 65536

typedef struct {
    double samples[BENCH_MAX_SAMPLES];
    double warm_time;
    double sample_time;
    double gpu_ms_sum;
    unsigned int gpu_q[8];
    unsigned char gpu_pending[8];
    int warm_frames;
    int count;
    int gpu_count;
    int gpu_slot;
    int active;
    int done;
} FrameBench;

static int bench_cmp(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static void bench_gpu_init(FrameBench *b)
{
    if (!b->active || !p_glGenQueries || !p_glBeginQuery ||
        !p_glEndQuery || !p_glGetQueryObjectiv || !p_glGetQueryObjectui64v) return;
    glGenQueries(8, b->gpu_q);
}

static void bench_gpu_begin(FrameBench *b)
{
    if (!b->active || !b->gpu_q[0]) return;
    int slot = b->gpu_slot;
    if (b->gpu_pending[slot]) {
        GLint ready = 0;
        glGetQueryObjectiv(b->gpu_q[slot], GL_QUERY_RESULT_AVAILABLE, &ready);
        if (!ready) return;
        GLuint64 ns = 0;
        glGetQueryObjectui64v(b->gpu_q[slot], GL_QUERY_RESULT, &ns);
        if (b->warm_frames >= 600 && b->warm_time >= 10.0) {
            b->gpu_ms_sum += (double)ns / 1000000.0;
            b->gpu_count++;
        }
        b->gpu_pending[slot] = 0;
    }
    glBeginQuery(GL_TIME_ELAPSED, b->gpu_q[slot]);
}

static void bench_gpu_end(FrameBench *b)
{
    if (!b->active || !b->gpu_q[0]) return;
    glEndQuery(GL_TIME_ELAPSED);
    b->gpu_pending[b->gpu_slot] = 1;
    b->gpu_slot = (b->gpu_slot + 1) & 7;
}

static void bench_gpu_cleanup(FrameBench *b)
{
    if (b->gpu_q[0]) glDeleteQueries(8, b->gpu_q);
}

static void bench_frame(FrameBench *b, double frame_s)
{
    if (!b->active || b->done || frame_s <= 0.0) return;
    if (b->warm_frames < 600 || b->warm_time < 10.0) {
        b->warm_frames++;
        b->warm_time += frame_s;
        return;
    }
    if (b->count < BENCH_MAX_SAMPLES) b->samples[b->count++] = frame_s * 1000.0;
    b->sample_time += frame_s;
    if (b->sample_time < 30.0 && b->count < 60000) return;
    qsort(b->samples, (size_t)b->count, sizeof b->samples[0], bench_cmp);
    double sum = 0.0;
    for (int i = 0; i < b->count; i++) sum += b->samples[i];
    int p50 = (int)((b->count - 1) * 0.50);
    int p95 = (int)((b->count - 1) * 0.95);
    int p99 = (int)((b->count - 1) * 0.99);
    SDL_Log("BENCH native wall-clock: %d frames %.2f s | %.1f fps | "
            "mean %.3f ms p50 %.3f p95 %.3f p99 %.3f | GPU %.3f ms",
            b->count, b->sample_time, b->count / b->sample_time,
            sum / b->count, b->samples[p50], b->samples[p95], b->samples[p99],
            b->gpu_count ? b->gpu_ms_sum / b->gpu_count : 0.0);
    b->done = 1;
}
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
        SDL_Log("VSync request %d: %s", vs > 0.5f ? 1 : 0,
                SDL_GL_SetSwapInterval(vs > 0.5f ? 1 : 0) ? "ok" : "failed");
    }
    float wr = cvar_get("r_wireframe");
    if (wr != s_sync_wire) {
        s_sync_wire = wr;   // применяется в renderer_frame (3D-проход)
        renderer_set_wireframe(wr > 0.5f);
    }
}

// --- Фаза APP_BOOT: инициализация систем (выполняется один раз) ---------------
// Поднимает системы в любом порядке (console_add_command работает и до
// console_init — см. console.c), регистрирует ВСЕ статические команды через
// ЕДИНЫЙ реестр cmdreg.c, грузит config.cfg и переводит состояние в APP_MENU
// (или APP_PLAY при FR_TEST).
static void boot_init(Game *g)
{
    resources_init();            // кеш ресурсов — до любых загрузок
    console_init();              // cvar'ы, команды, вывод SDL_Log в консоль
    audio_init();                // звук: устройство + каналы
    binds_init();                // дефолтные привязки клавиш (HL-стиль)
    fx_init_console();           // пресеты эффектов (команды fx_* — у cmdreg)
    // Все статические команды — из одного центрального реестра (cmdreg.c).
    cmdreg_register_all();
    hud_shared_init();           // шрифт/атласы + рендер текста для меню и консоли
    app_init();                  // фон img/menu.png
    console_autoload();          // config.cfg: cvar'ы и бинды пользователя
    game_map_config_load(g);     // + выбранная карта
    lua_engine_init();           // скрипты Lua (после консоли и cvar'ов)
    app_set_state(APP_MENU);     // фаза закончена: меню
}

// --- FR_TEST — тест производительности / самопроверка (после boot_init) --------
// Флаги: x — селфтест консоли; s — тени; v/p/q/c/e/f — выкл. подсистем;
// o — воксельный туман; t/k — без рельефа/неба; m — burst частиц у игрока
// (пресет "fire"); d — декаль; a — дождь-эмиттер; n — снег-эмиттер;
// y — взрыв. Требует мир: грузим сразу и входим в игру. 0 — успех,
// -1 — мир не загрузился.
static int fr_test_apply(Game *g)
{
    const char *fr_test = getenv("FR_TEST");
    if (!fr_test) return 0;
    if (strchr(fr_test, 'x')) console_selftest();
    if (strchr(fr_test, 's')) shadow_toggle();
    if (strchr(fr_test, 'r')) cvar_set("r_ssr", 0.0f);   // SSR off (A/B-кадры)
    if (strchr(fr_test, 'C')) g_shadow_cache = 0;        // кэш статических каскадов off
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
    // FR_POS="x,z[,yaw_deg]" — телепорт игрока после загрузки мира
    // (скриншоты графики в разных точках карты: FR_POS="300,500" FR_TEST=S)
    {
        const char *pos = getenv("FR_POS");
        if (pos && *pos) {
            float fx, fz, fyaw = 0.0f, fpitch = 0.0f;
            if (sscanf(pos, "%f,%f,%f,%f", &fx, &fz, &fyaw, &fpitch) >= 2) {
                g->cam->cam.pos[0] = fx;
                g->cam->cam.pos[2] = fz;
                // Глаз 2.4 м: чуть выше пшеницы — точки обзора для тестов
                g->cam->cam.pos[1] = sample_height(&g->world, fx, fz) + 2.4f;
                g->cam->cam.vy = 0;
                // noclip: камера не падает и pitch не ограничен ходьбой
                // (удобно смотреть на небо/вниз для скриншотов)
                g->cam->cam.fly = 1;
                if (fyaw != 0.0f) g->cam->cam.yaw = fyaw * 3.14159f / 180.0f;
                g->cam->cam.pitch = fpitch * 3.14159f / 180.0f;
                // Цели сглаживания мыши — к новому взгляду (иначе m_smooth
                // потащит камеру обратно к спавну)
                camera_sync_look(&g->cam->cam);
                SDL_Log("FR_POS: телепорт %.0f,%.0f yaw=%.2f pitch=%.2f",
                        fx, fz, g->cam->cam.yaw, g->cam->cam.pitch);
            }
        }
    }
    // t/k — отключить рельеф/небо (нужны узлы, поэтому после load_world)
    if (strchr(fr_test, 't')) g->terr_node->render = NULL;
    if (strchr(fr_test, 'k')) g->sky_node->render = NULL;
    // m — burst "fire" у игрока; d — декаль под игроком
    if (strchr(fr_test, 'm')) {
        const float *cp = g->cam->cam.pos;
        fx_burst("fire", cp[0], cp[1] + 1.0f, cp[2], 0);
    }
    if (strchr(fr_test, 'd')) {
        const float *cp = g->cam->cam.pos;
        decal_place(cp[0], cp[2], 3.0f, 3.0f, 0.0f, "img/particle.png",
                    0.15f, 0.4f, 0.8f, 0.85f);
    }
    // a/n — погода (дождь/снег следует за игроком); y — взрыв
    if (strchr(fr_test, 'a')) {
        float ext[3] = { 20.0f, 10.0f, 20.0f };
        fx_weather_start("rain", 12.0f, ext, 150.0f, 0.0f);
    }
    // w — мокрость=1 БЕЗ частиц дождя (чистый тест мокрого SSR, A/B-кадры)
    if (strchr(fr_test, 'w')) fx_force_wetness(1.0f);
    // D — демо-полёт (headless-скриншоты красивой точки обзора, FR_TEST=SD)
    if (strchr(fr_test, 'D')) game_demo_toggle(g);
    // g — glb-машина в 6 м перед камерой НА ЗЕМЛЕ (тест автопостановки
    // cmd_glb без координат и теней моделей): FR_TEST=Sg
    if (strchr(fr_test, 'g')) {
        cmd_glb("models/vaz-2105.glb");
    }
    // l — анимированная модель в 3 м перед камерой (тест UV лица и теней
    // скиннинга): FR_TEST=Sl. y=0 → автопостановка на рельеф.
    if (strchr(fr_test, 'l')) {
        const float *cp = g->cam->cam.pos;
        float fwd[3] = { 0, 0, 1 };
        camera_dirs(&g->cam->cam, fwd, NULL);
        char args[300];
        snprintf(args, sizeof args,
                 "models/handpaint_-_girl_01.glb %.2f 0 %.2f 1",
                 cp[0] + fwd[0] * 3.0f, cp[2] + fwd[2] * 3.0f);
        cmd_glbanim(args);
        g->cam->cam.pitch = -0.26f;      // вниз — лицо крупным планом
        camera_sync_look(&g->cam->cam);
    }
    // R — Verlet-верёвка перед камерой (тест мини-физики): FR_TEST=SR
    if (strchr(fr_test, 'R')) cmd_phys_rope("16");
    // H — женщина + рэгдолл с толчком (тест физики скелета): FR_TEST=SH
    if (strchr(fr_test, 'H')) {
        const float *cp = g->cam->cam.pos;
        float fwd[3] = { 0, 0, 1 };
        camera_dirs(&g->cam->cam, fwd, NULL);
        char args[300];
        snprintf(args, sizeof args,
                 "models/handpaint_-_girl_01.glb %.2f 0 %.2f 1",
                 cp[0] + fwd[0] * 5.0f, cp[2] + fwd[2] * 5.0f);
        cmd_glbanim(args);
        g->cam->cam.pitch = -0.25f;
        camera_sync_look(&g->cam->cam);
        cmd_phys_ragdoll("0 0");   // без толчка: обмякнуть и упасть
    }
    if (strchr(fr_test, 'n')) {
        float ext[3] = { 20.0f, 8.0f, 20.0f };
        fx_weather_start("snow", 12.0f, ext, 40.0f, 0.0f);
    }
    if (strchr(fr_test, 'y')) {
        const float *cp = g->cam->cam.pos;
        fx_burst("explosion", cp[0], cp[1] + 1.0f, cp[2], 0);
        fx_burst("sparks", cp[0], cp[1] + 1.0f, cp[2], 0);
        fx_burst("smoke", cp[0], cp[1] + 1.0f, cp[2], 0);
    }
    app_set_state(APP_PLAY);
    SDL_Log("FR_TEST=[%s]", fr_test);
    return 0;
}

// Колбэк высоты рельефа для мини-физики (physics.c):Verlet-частицы
// сталкиваются с той же землёй, по которой ходит игрок.
static float phys_ground(float x, float z)
{
    Game *g = game_instance();
    return (g && g->world_loaded) ? sample_height(&g->world, x, z) : 0.0f;
}

int main(int argc, char *argv[])
{
    (void)argc; (void)argv;

    // 1) Инициализация SDL — должна быть ДО любых SDL_* вызовов
    //    (включая SDL_GetPrefPath/SDL_LoadFile в fr_android_prepare на Android)
    //    Раньше стояла после — на девайсе падал с SIGSEGV до показа окна.
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        // SDL ещё не поднялась — пишем в stderr
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    srand((unsigned)SDL_GetTicks());

    // Android: распаковать APK-assets в хранилище и перейти туда (до base_dir).
    // Вызываем ПОСЛЕ SDL_Init — на Android здесь уже есть JNI/env и AAssetManager.
    if (!fr_android_prepare()) {
#ifdef __ANDROID__
        SDL_Log("android: подготовка ресурсов не удалась");
        SDL_Quit();
        return 1;
#endif
    }

    // Определяем базовую директорию ресурсов (где лежат map/, shaders/, img/).
    // Приоритет: CWD (если есть map/) -> папка exe -> родительская папка exe.
    static char base_dir[512];
    FILE *probe = fopen("map/map.json", "rb");
    if (probe) {
        fclose(probe);
        snprintf(base_dir, sizeof base_dir, ".");   // CWD — это корень проекта
    } else {
        const char *base = SDL_GetBasePath();
        if (base && base[0]) {
            // Проверяем папку exe
            char test_path[512];
            snprintf(test_path, sizeof test_path, "%smap/map.json", base);
            probe = fopen(test_path, "rb");
            if (probe) {
                fclose(probe);
                snprintf(base_dir, sizeof base_dir, "%s", base);
            } else {
                // Проверяем родительскую папку (проект при запуске из build/)
                snprintf(test_path, sizeof test_path, "%s../map/map.json", base);
                probe = fopen(test_path, "rb");
                if (probe) {
                    fclose(probe);
                    snprintf(base_dir, sizeof base_dir, "%s..", base);
                } else {
                    snprintf(base_dir, sizeof base_dir, "%s", base);  // fallback
                }
            }
            SDL_free((void *)base);
        } else {
            snprintf(base_dir, sizeof base_dir, ".");
        }
    }

    // Устанавливаем базовую директорию ДО resources_init()
    resources_set_base_dir(base_dir);

    // Если в текущей папке нет данных — переходим в папку исполняемого файла
    // (для config.cfg, логов и т.п.)
    if (strcmp(base_dir, ".") != 0) {
#ifdef _WIN32
        _chdir(base_dir);
#else
        chdir(base_dir);
#endif
    }

    // 2) Атрибуты GL-контекста (SDL уже инициализирована выше) -----------------
#ifdef USE_GLES_PATH
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#else
    // GL 4.5 CORE: нужен для GPU-driven рендера травы (compute-cull +
    // indirect draw, GL 4.3+). Суперсет 3.3, старые шейдеры работают как
    // раньше. Если драйвер не даст 4.5, SDL вернёт ближайший доступный, и
    // vegetation.c сам откатится на инстансный путь (g_gl_gpu_driven == 0).
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 5);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
#endif
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

    // 3) Окно и контекст -------------------------------------------------------
    // FR_W/FR_H — headless-тест высоких разрешений (4К и т.п.)
    int ww = WIN_W, wh = WIN_H;
    {
        const char *ew = getenv("FR_W"), *eh = getenv("FR_H");
        if (ew && eh) { ww = atoi(ew); wh = atoi(eh); }
    }
    g_win = SDL_CreateWindow("firerender", ww, wh,
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
#ifdef USE_GLES_PATH
    SDL_Log("Renderer profile: GLES LOW-END (LDR, cheap fog, instanced culling)");
#else
    SDL_Log("Renderer profile: DESKTOP GL");
#endif
    SDL_Log("Graphics API: %s", glGetString(GL_VERSION));
    SDL_Log("GPU-driven veg (compute+indirect): %s",
            g_gl_gpu_driven ? "ENABLED" : "disabled (instanced fallback)");
    SDL_Log("firerender build #%d", ENGINE_BUILD_NUM);

    // 6) Вьюпорт = реальный размер окна в пикселях -----------------------------
    int vw = ww, vh = wh;
    SDL_GetWindowSizeInPixels(g_win, &vw, &vh);
    SDL_Log("Drawable: %dx%d (requested %dx%d)", vw, vh, ww, wh);
    if (getenv("FR_BENCH") && (vw != ww || vh != wh))
        SDL_Log("BENCH warning: drawable differs from requested size");
    glViewport(0, 0, vw, vh);
    renderer_set_viewport(vw, vh);

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
    FrameBench bench = {0};
    bench.active = getenv("FR_BENCH") != NULL;
    if (getenv("FR_BENCH_GPU")) bench_gpu_init(&bench);
    Uint64 last = SDL_GetPerformanceCounter();
    float fps_accum = 0;
    int fps_frames = 0;
    float sim_acc = 0.0f;
    bool running = true;
    while (running) {
        Uint64 now = SDL_GetPerformanceCounter();
        float frame_dt = (float)(now - last) / SDL_GetPerformanceFrequency();
        last = now;
        if (frame_dt > 0.1f) frame_dt = 0.1f;
        float dt = frame_dt * cvar_get("sv_timescale");

        float inst_fps = 1.0f / (frame_dt > 0.000001f ? frame_dt : 0.000001f);
        if (game.hud) game.hud->fps = game.hud->fps * 0.95f + inst_fps * 0.05f;

        // FR_STAGES — временная инструметация кадра: средние по стадиям
        // печатаются в лог при выходе (поиск CPU-хотспотов движка).
        static int s_stages = -1;
        static double s_st_in = 0, s_st_logic = 0, s_st_sync = 0,
                      s_st_render = 0, s_st_end = 0, s_st_swap = 0,
                      s_st_gu = 0, s_st_phys = 0, s_st_aux = 0,
                      s_st_reload = 0, s_st_demo = 0;
        static double s_st_frame_max = 0, s_st_gu_max = 0, s_st_render_max = 0;
        static int s_st_n = 0;
        Uint64 st_t = SDL_GetPerformanceCounter(), st_p;
        double st_f = (double)SDL_GetPerformanceFrequency();
        if (s_stages < 0) s_stages = getenv("FR_STAGES") ? 1 : 0;
#define ST_ACC(v) do { st_p = SDL_GetPerformanceCounter(); v += (st_p - st_t) / st_f; st_t = st_p; } while (0)

        input_process();                    // события: консоль, бинды, ресайз
        if (input_quit()) running = false;
        if (input_take_resumed()) {
            last = SDL_GetPerformanceCounter();
            sim_acc = 0.0f;
            frame_dt = dt = 0.0f;
        }
        if (input_background()) {
            SDL_Delay(50);
            last = SDL_GetPerformanceCounter();
            sim_acc = 0.0f;
            continue;
        }
        ST_ACC(s_st_in);

        // Фаза APP_BOOT: поднять системы один раз (в конце — APP_MENU/PLAY)
        if (app_state() == APP_BOOT) {
            boot_init(&game);
            if (bench.active) {
                const char *preset = getenv("FR_BENCH_PRESET");
                app_apply_benchmark_preset(preset ? atoi(preset) : 0);
            }
            if (fr_test_apply(&game) != 0) goto fail;   // FR_TEST: мир

            if (getenv("FR_CONSOLE")) console_toggle();   // консоль при старте
            // cvar-синки не должны перетирать FR_TEST на первом кадре —
            // примайниваем только то, что трогает FR_TEST (тени/туман).
            // Полноэкран/VSync/каркас применит первый sync_cvars из config.cfg.
            s_sync_shadows = cvar_get("r_shadows_mode");
            s_sync_fog = cvar_get("r_fog_mode");
        }

        // В APK assets неизменяемы, а периодические stat() только будят storage.
#ifndef __ANDROID__
        {
            static double s_reload_at = 0.0;
            double t = (double)SDL_GetTicks() * 0.001;
            if (!bench.active && t >= s_reload_at) {
                s_reload_at = t + 1.0;
                shader_hot_reload();
                resources_texture_reload();
            }
        }
#endif
        ST_ACC(s_st_reload);

        game_demo_tick(&game, dt, inst_fps); // демо-сцена (Q): автополёт + FPS
        ST_ACC(s_st_demo);

        // Фиксированный тик логики/физики: 100 Гц десктоп, 60 Гц на телефоне (экономия CPU)
        {
#ifdef __ANDROID__
            const float step = 1.0f / 60.0f;
            const int max_ticks = 4;
#else
            const float step = 1.0f / 100.0f;
            const int max_ticks = 8;
#endif
            sim_acc += dt;
            if (sim_acc >= 0.25f) sim_acc = 0.25f;
            for (int ticks = 0; sim_acc >= step && ticks < max_ticks; ticks++) {
                if (app_state() == APP_PLAY) {
                    game_update(&game, step);   // мир: узлы, триггеры/use, туман
                    ST_ACC(s_st_gu);
                    physics_set_ground(phys_ground);
                    physics_step(physics_world(), step);   // мини-физика (Verlet)
                    glb_anim_ragdoll_step_all(step);       // рэгдоллы аним-моделей
                    ST_ACC(s_st_phys);
                    audio_update();             // фоновые петли и 3D-источники
                    lua_engine_update(step);    // per-frame колбэк скрипта
                    ST_ACC(s_st_aux);
                }
                sim_acc -= step;
            }
        }
        ST_ACC(s_st_logic);
        {
            static double s_gu_prev = 0;
            double gu_frame = s_st_gu - s_gu_prev;
            s_gu_prev = s_st_gu;
            if (gu_frame > s_st_gu_max) s_st_gu_max = gu_frame;
        }

        sync_cvars(&game);
        input_sync_mode();

        // FR_TEST 'H': автонаведение камеры на рэгдолл каждый кадр —
        // головной тест физики не должен зависеть от того, куда уползла
        // модель (без этого она уезжает из кадра и баг не видно).
        if (strchr(getenv("FR_TEST") ? getenv("FR_TEST") : "", 'H') &&
            game.world_loaded && app_state() == APP_PLAY) {
            float aim[3];
            if (glb_anim_ragdoll_aim(aim)) {
                Camera *cam = &game.cam->cam;
                float d[3] = { aim[0] - cam->pos[0], aim[1] - cam->pos[1],
                               aim[2] - cam->pos[2] };
                float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                if (len > 0.5f) {
                    cam->yaw = atan2f(d[0], d[2]);
                    cam->pitch = asinf(d[1] / len < -1.0f ? -1.0f :
                                       (d[1] / len > 1.0f ? 1.0f :
                                        d[1] / len));
                    camera_sync_look(cam);
                }
            }
        }

        // Отложенные события (events_emit_deferred) — безопасная точка после
        // всех тиков, до рендера: колбэки не могут переиспользовать списки
        // слушателей посреди итерации.
        events_flush_deferred();

        ST_ACC(s_st_sync);

        // Рендер: сцена -> bloom/туман -> оверлеи (см. renderer.c)
        bench_gpu_begin(&bench);
        renderer_begin();
        renderer_frame(&game, dt, game.world_loaded &&
                                   (app_state() != APP_MENU || !app_bg_has()));
        ST_ACC(s_st_render);
        {
            static double s_rd_prev = 0;
            double rd_frame = s_st_render - s_rd_prev;
            s_rd_prev = s_st_render;
            if (rd_frame > s_st_render_max) s_st_render_max = rd_frame;
        }
        // FR_TEST 'S': автоскриншот на 450-м кадре и выход на 480-м —
        // headless-проверка картинки + стабильный прогрев для профайлера.
        // Запрос ставится до renderer_end, чтобы чтение было в том же кадре:
        // некоторые EGL backbuffer'ы после overlay draw уже discard-нуты.
        if (strchr(getenv("FR_TEST") ? getenv("FR_TEST") : "", 'S')) {
            static int s_frames = 0, s_shot_at = -1;
            if (s_shot_at < 0) {
                const char *fs = getenv("FR_SHOT");
                s_shot_at = fs ? atoi(fs) : 450;
            }
            if (++s_frames == s_shot_at) cmd_screenshot(NULL);
            if (s_frames >= s_shot_at + 30) running = false;
        }
        renderer_end(&game, dt);
        ST_ACC(s_st_end);

        // Меню (главное/пауза/настройки) — поверх замороженной сцены
        if (app_state() != APP_PLAY) {
            int act = app_menu(renderer_vw(), renderer_vh());
            if (act == APP_ACT_START || act == APP_ACT_RESUME) {
                if (!game.world_loaded && game_load_world(&game) != 0) {
                    SDL_Log("World load failed — остаёмся в меню");
                    console_print("ошибка загрузки карты");
                    // не выходим из приложения, даём шанс выбрать другую карту
                    continue;
                }
                app_set_state(APP_PLAY);
                if (act == APP_ACT_START) {
                    // Новая игра: камера на спавн, сброс демо.
                    // (если это load из save — телепорт вместо спавна)
                    if (game.player.pending_spawn) {
                        game_apply_spawn(&game, game.player.save_pos,
                                         game.player.save_yaw,
                                         game.player.save_pitch);
                        game.player.pending_spawn = 0;
                        console_print("load: загружено");
                    } else {
                        game.cam->cam.pos[0] = game.player.spawn_pos[0];
                        game.cam->cam.pos[1] = game.player.spawn_pos[1];
                        game.cam->cam.pos[2] = game.player.spawn_pos[2];
                        game.cam->cam.yaw = game.player.spawn_yaw;
                        game.cam->cam.pitch = game.player.spawn_pitch;
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
        commands_shot_take(renderer_vw(), renderer_vh());     // скриншот (F12)
        if (app_state() == APP_PLAY)
            touch_hud_draw(renderer_vw(), renderer_vh());     // виртуальный джойстик
        console_drain();                                      // отложенные wait-команды
        console_render(renderer_vw(), renderer_vh());         // консоль — поверх всего (`)
        bench_gpu_end(&bench);

        // FR_SHOT — сохранить кадр в PPM (отладка рендера без Wayland).
        // Значение — номер кадра для снимка (FR_SHOT=60), пустое = 60.
        {
            static int shot_frame = 0;
            const char *fs = getenv("FR_SHOT");
            if (fs && shot_frame != -1) {
                int target = atoi(fs);
                if (target < 1) target = 60;
                if (shot_frame == 0) shot_frame = 1;
                else if (shot_frame < target) shot_frame++;
                if (shot_frame == target) {
                    unsigned char *px = malloc((size_t)renderer_vw() * renderer_vh() * 3);
                    glReadPixels(0, 0, renderer_vw(), renderer_vh(), GL_RGB, GL_UNSIGNED_BYTE, px);
                    FILE *f = fopen("/tmp/fr_shot.ppm", "wb");
                    if (f) {
                        fprintf(f, "P6\n%d %d\n255\n", renderer_vw(), renderer_vh());
                        for (int y = 0; y < renderer_vh(); y++)
                            fwrite(px + (size_t)(renderer_vh() - 1 - y) * renderer_vw() * 3, 1,
                                   (size_t)renderer_vw() * 3, f);
                        fclose(f);
                        SDL_Log("FR_SHOT: /tmp/fr_shot.ppm (%dx%d) console=%d",
                                renderer_vw(), renderer_vh(), console_active());
                    }
                    free(px);
                    shot_frame = -1;
                }
            }
        }

        SDL_GL_SwapWindow(g_win);
        ST_ACC(s_st_swap);
        s_st_n++;
        {
            double fr = (SDL_GetPerformanceCounter() - now) / st_f;
            if (fr > s_st_frame_max) s_st_frame_max = fr;
        }

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

        if (!running && s_stages && s_st_n > 0) {
            SDL_Log("FR_STAGES (среднее по %d кадрам, мс): input %.3f | "
                    "logic %.3f [reload %.3f | demo %.3f | game_update %.3f | "
                    "phys %.3f | audio+lua %.3f]"
                    " | sync %.3f | render %.3f | end %.3f | "
                    "swap %.3f | СУММА %.3f",
                    s_st_n, s_st_in * 1000.0 / s_st_n,
                    s_st_logic * 1000.0 / s_st_n,
                    s_st_reload * 1000.0 / s_st_n,
                    s_st_demo * 1000.0 / s_st_n,
                    s_st_gu * 1000.0 / s_st_n, s_st_phys * 1000.0 / s_st_n,
                    s_st_aux * 1000.0 / s_st_n,
                    s_st_sync * 1000.0 / s_st_n,
                    s_st_render * 1000.0 / s_st_n, s_st_end * 1000.0 / s_st_n,
                    s_st_swap * 1000.0 / s_st_n,
                    (s_st_in + s_st_logic + s_st_sync + s_st_render +
                     s_st_end + s_st_swap) * 1000.0 / s_st_n);
            SDL_Log("FR_STAGES max (мс): кадр %.1f | game_update %.1f | "
                    "render %.1f  (boot-кадр портит reload-среднее — не смотри туда)",
                    s_st_frame_max * 1000.0, s_st_gu_max * 1000.0,
                    s_st_render_max * 1000.0);
        }

        fps_accum += frame_dt;
        bench_frame(&bench, frame_dt);
        if (bench.done) running = false;
        if (++fps_frames >= 60) {
            if (!bench.active) {
                float fps = fps_frames / fps_accum;
#ifdef __ANDROID__
                SDL_Log("fps: %.0f (dt %.1f ms) vw %d vh %d", fps,
                        dt * 1000.0f, renderer_vw(), renderer_vh());
#else
                printf("fps: %.0f\n", fps);
#endif
            }
            fps_frames = 0;
            fps_accum = 0;
        }
    }

    // 11) Очистка --------------------------------------------------------------
    bench_gpu_cleanup(&bench);
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
