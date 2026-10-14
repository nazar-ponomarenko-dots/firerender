// ============================================================================
//  firerender — a 3D project in pure C (SDL3 + OpenGL)
// ----------------------------------------------------------------------------
//  Entry point: SDL/GL init, menu, world loading and the main loop.
//  Logic is split across modules:
//    game.c      — world state in the Game struct + lifecycle/update;
//    commands.c  — console commands (registration happens here, in main);
//    renderer.c  — render orchestration (scene, bloom, fog, overlays);
//    input.c     — main-loop events and mouse mode;
//    world.c     — world data (WorldState): heights, sizes, field.
//
//  Application state — EXPLICIT PHASES (see app.h): APP_BOOT (bring systems up,
//  once in the main loop) -> APP_MENU/APP_PLAY/APP_PAUSE.
//
//  The world (map, terrain, vegetation) is loaded LAZILY — only after
//  pressing "NEW GAME"/"CONTINUE". In the menu only the font, the background
//  image (img/menu.png) and the console work — 3D is neither loaded nor drawn.
//  On return to the main menu the world is unloaded (memory freed).
// ============================================================================

#include <SDL3/SDL.h>
#include <math.h>        // SDL3: windows, events, timers, GL context
#include <SDL3/SDL_main.h>   // Portable main(): on Windows it replaces
                             // WinMain; on Android it is the entry point
                             // called from the SDLActivity Java wrapper.

#include "gl_loader.h"
#include "world.h"
#include "game.h"
#include "commands.h"
#include "events.h"
#include "renderer.h"
#include "srender.h"
#include "buildnum.h"
#include "input.h"
#include "lua_engine.h"
#include "shader.h"   // shader_hot_reload (see reload_shaders)
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
#include "vegetation.h"      // FR_TEST: veg_* toggles
#include "ecs.h"             // ECS core: ecs_enable cvar, commands, selftest
#include "ecs_scene.h"       // scene<->ECS bridge: init/enable at boot
#include "phys_body.h"       // rigid bodies: step, commands, selftest
#include "voxfog.h"          // FR_TEST: voxfog_set
#include "terrain.h"         // sync_cvars: terrain_shadows_bake
#include "sun.h"             // (SunParams*)g->sun_node->params
#include "material.h"        // material: registry by name
#include "physics.h"         // mini physics (Verlet): step in the fixed tick
#include "glb_anim.h"        // ragdoll: skeleton physics step

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

// --- Window constants -------------------------------------------------------
#define WIN_W 1280          // Window width in pixels
#define WIN_H 720           // Window height in pixels

static SDL_Window *g_win = NULL;

// --- Cvar's -> systems --------------------------------------------------------
static float s_sync_shadows = -1.0f, s_sync_fog = -1.0f;
static float s_sync_fs = -1.0f, s_sync_vsync = -1.0f, s_sync_wire = -1.0f;
static void sync_cvars(Game *g)
{
    float sh = cvar_get("r_shadows_mode");
    if (sh != s_sync_shadows) {
        s_sync_shadows = sh;
        // CSM renders in modes 1 (dynamic only) and 2 (dynamic near the
        // player, static across the whole world)
        shadow_set(sh == 1.0f || sh == 2.0f);
        // Static: recompute the shadow map ONLY if there is no baked
        // lightmap. If a ready one exists (map/baked) — mode 2 reads it, and a
        // CPU bake would waste 2+ s (the shader ignores its result).
        if (sh == 2.0f && g->world_loaded && !terrain_baked_light_have())
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
        s_sync_wire = wr;   // applied in renderer_frame (3D pass)
        renderer_set_wireframe(wr > 0.5f);
    }
}

// --- Phase APP_BOOT: system initialization (runs once) ----------------------
// Brings systems up in any order (console_add_command works even before
// console_init — see console.c), registers ALL static commands through
// the SINGLE registry cmdreg.c, loads config.cfg and switches the state to
// APP_MENU (or APP_PLAY under FR_TEST).
static void boot_init(Game *g)
{
    resources_init();            // resource cache — before any loads
    console_init();              // cvar's, commands, SDL_Log mirrored to console
    audio_init();                // audio: device + channels
    binds_init();                // default key binds (HL style)
    fx_init_console();           // effect presets (fx_* commands — in cmdreg)
    // ECS: the bridge registers its components; the cvar is the master
    // switch for the layer.
    ecs_scene_init();
    cvar_register("ecs_enable", 0.0f,
                  "ECS-слой: синхронизация сцены и системы (0=выключен)");
    phys_body_init();                // rigid bodies: cvar's + empty registry
    // All static commands — from one central registry (cmdreg.c).
    cmdreg_register_all();
    hud_shared_init();           // font/atlas + text rendering for menu and console
    app_init();                  // background img/menu.png
    console_autoload();          // config.cfg: user cvar's and binds
    game_map_config_load(g);     // + selected map
    lua_engine_init();           // Lua scripts (after console and cvar's)
    app_set_state(APP_MENU);     // phase done: menu
}

// --- FR_TEST — performance test / self-check (after boot_init) ---------------
// Flags: x — console selftest; s — shadows; v/p/q/c/e/f — disable subsystems;
// o — voxel fog; t/k — no terrain/sky; m — particle burst at the player
// (preset "fire"); d — decal; a — rain emitter; n — snow emitter;
// y — explosion; E — enable the ECS layer and run ecs_test at startup;
// V — ECS bridge + connectivity verify on the first tick;
// u — rigid-body selftest; U — enable bodies and spawn a crate stack;
// O — disable positional-sound occlusion (A/B pair for snd_occlusion 1);
// Z — sound occlusion selftest on a synthetic wall;
// Needs the world: load it immediately and enter the game. 0 — success,
// -1 — world failed to load.
static int fr_test_apply(Game *g)
{
    const char *fr_test = getenv("FR_TEST");
    if (!fr_test) return 0;
    if (strchr(fr_test, 'x')) console_selftest();
    if (strchr(fr_test, 'r')) cvar_set("r_ssr", 0.0f);   // SSR off (A/B frames)
    if (strchr(fr_test, 'O')) cvar_set("snd_occlusion", 0.0f);  // sound muting off (A/B)
    if (strchr(fr_test, 'Z')) {
        int fails = audio_selftest();
        SDL_Log("FR_TEST Z: audio_selftest %s (провалов %d)",
                fails ? "FAIL" : "OK", fails);
    }
    if (strchr(fr_test, 'C')) g_shadow_cache = 0;        // static cascade cache off
    if (strchr(fr_test, 'v')) veg_set_visible_test(0);
    if (strchr(fr_test, 'p')) g_shadow_terrain = 0;
    if (strchr(fr_test, 'q')) g_shadow_veg = 0;
    if (strchr(fr_test, 'c')) g_shadow_cull = 0;
    if (strchr(fr_test, 'e')) veg_set_static_main(0);
    if (strchr(fr_test, 'f')) veg_set_dyn_main(0);
    if (strchr(fr_test, 'E')) {
        cvar_set("ecs_enable", 1.0f);   // ECS layer active from the first tick
        int fails = ecs_selftest();
        SDL_Log("FR_TEST E: ecs_selftest %s (провалов %d)",
                fails ? "FAIL" : "OK", fails);
        ecs_scene_new_generation();     // clean generation after the test
    }
    if (strchr(fr_test, 'V')) {
        cvar_set("ecs_enable", 1.0f);
        ecs_scene_verify_once();        // bridge verify on the very first tick
    }
    if (strchr(fr_test, 'u')) {
        cvar_set("phys_bodies", 1.0f);
        int fails = phys_body_selftest();
        SDL_Log("FR_TEST u: phys_body_selftest %s (провалов %d)",
                fails ? "FAIL" : "OK", fails);
        phys_body_kill_all(physics_world());   // the test leaves nothing behind
    }
    if (game_load_world(g) != 0) {
        SDL_Log("FR_TEST: world load failed");
        return -1;
    }
    // FR_POS="x,z[,yaw_deg]" — teleport the player after world load
    // (graphics screenshots at different map points: FR_POS="300,500" FR_TEST=S)
    {
        const char *pos = getenv("FR_POS");
        if (pos && *pos) {
            float fx, fz, fyaw = 0.0f, fpitch = 0.0f;
            if (sscanf(pos, "%f,%f,%f,%f", &fx, &fz, &fyaw, &fpitch) >= 2) {
                g->cam->cam.pos[0] = fx;
                g->cam->cam.pos[2] = fz;
                // Eye at 2.4 m: slightly above the wheat — view points for tests
                g->cam->cam.pos[1] = sample_height(&g->world, fx, fz) + 2.4f;
                g->cam->cam.vy = 0;
                // noclip: the camera does not fall and pitch is not limited
                // by walking (convenient to look at the sky/down for screenshots)
                g->cam->cam.fly = 1;
                if (fyaw != 0.0f) g->cam->cam.yaw = fyaw * 3.14159f / 180.0f;
                g->cam->cam.pitch = fpitch * 3.14159f / 180.0f;
                // Mouse smoothing targets — snap to the new look (otherwise
                // m_smooth drags the camera back to spawn)
                camera_sync_look(&g->cam->cam);
                SDL_Log("FR_POS: телепорт %.0f,%.0f yaw=%.2f pitch=%.2f",
                        fx, fz, g->cam->cam.yaw, g->cam->cam.pitch);
            }
        }
    }
    // Z — after load_world/FR_POS: listener and map are ready, there is something to measure.
    if (strchr(fr_test, 'Z')) audio_selftest_world(&g->world, g->cam->cam.pos);
    // U — a crate stack in front of the camera. Must come after load_world
    // and FR_POS: the body is placed relative to the camera, and the camera
    // spawns (and teleports) only there; otherwise the "stack" hangs in the
    // void until the player spawns.
    if (strchr(fr_test, 'U')) {
        cvar_set("phys_bodies", 1.0f);  // bodies active from the first tick
        cmd_phys_props("8");
    }
    // t/k — disable terrain/sky (needs nodes, hence after load_world)
    if (strchr(fr_test, 't')) g->terr_node->render = NULL;
    if (strchr(fr_test, 'k')) g->sky_node->render = NULL;
    // m — "fire" burst at the player; d — decal under the player
    if (strchr(fr_test, 'm')) {
        const float *cp = g->cam->cam.pos;
        fx_burst("fire", cp[0], cp[1] + 1.0f, cp[2], 0);
    }
    if (strchr(fr_test, 'd')) {
        const float *cp = g->cam->cam.pos;
        decal_place(cp[0], cp[2], 3.0f, 3.0f, 0.0f, "img/particle.png",
                    0.15f, 0.4f, 0.8f, 0.85f);
    }
    // a/n — weather (rain/snow follows the player); y — explosion
    if (strchr(fr_test, 'a')) {
        float ext[3] = { 20.0f, 10.0f, 20.0f };
        fx_weather_start("rain", 12.0f, ext, 150.0f, 0.0f);
    }
    // w — wetness=1 WITHOUT rain particles (clean wet-SSR test, A/B frames)
    if (strchr(fr_test, 'w')) fx_force_wetness(1.0f);
    // D — demo flight (headless screenshots of a nice viewpoint, FR_TEST=SD)
    if (strchr(fr_test, 'D')) game_demo_toggle(g);
    // g — glb car 6 m in front of the camera ON THE GROUND (tests auto-
    // placement of cmd_glb without coordinates and model shadows): FR_TEST=Sg
    if (strchr(fr_test, 'g')) {
        cmd_glb("models/vaz-2105.glb");
    }
    // l — animated model 3 m in front of the camera (tests face UV and
    // skinning shadows): FR_TEST=Sl. y=0 → auto-placement onto the terrain.
    if (strchr(fr_test, 'l')) {
        const float *cp = g->cam->cam.pos;
        float fwd[3] = { 0, 0, 1 };
        camera_dirs(&g->cam->cam, fwd, NULL);
        char args[300];
        snprintf(args, sizeof args,
                 "models/handpaint_-_girl_01.glb %.2f 0 %.2f 1",
                 cp[0] + fwd[0] * 3.0f, cp[2] + fwd[2] * 3.0f);
        cmd_glbanim(args);
        g->cam->cam.pitch = -0.26f;      // down — face in close-up
        camera_sync_look(&g->cam->cam);
    }
    // R — Verlet rope in front of the camera (mini-physics test): FR_TEST=SR
    if (strchr(fr_test, 'R')) cmd_phys_rope("16");
    // H — woman + ragdoll with a push (skeleton physics test): FR_TEST=SH
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
        cmd_phys_ragdoll("0 0");   // without a push: go limp and fall
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

// Terrain height callback for the mini physics (physics.c): Verlet particles
// collide with the same ground the player walks on.
static float phys_ground(float x, float z)
{
    Game *g = game_instance();
    return (g && g->world_loaded) ? sample_height(&g->world, x, z) : 0.0f;
}

// Determine the base resources directory (where map/, shaders/, img/ live).
// Priority: CWD (if map/ exists) -> exe folder -> parent folder of the exe.
// Calls resources_set_base_dir() and chdirs into it — config.cfg and logs are
// written next to the data, not in an arbitrary CWD.
static void boot_resolve_base_dir(void)
{
    static char base_dir[512];
    FILE *probe = fopen("map/map.json", "rb");
    if (probe) {
        fclose(probe);
        snprintf(base_dir, sizeof base_dir, ".");   // CWD is the project root
    } else {
        const char *base = SDL_GetBasePath();
        if (base && base[0]) {
            // Check the exe folder
            char test_path[512];
            snprintf(test_path, sizeof test_path, "%smap/map.json", base);
            probe = fopen(test_path, "rb");
            if (probe) {
                fclose(probe);
                snprintf(base_dir, sizeof base_dir, "%s", base);
            } else {
                // Check the parent folder (project launched from build/)
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

    // Set the base directory BEFORE resources_init()
    resources_set_base_dir(base_dir);

    // If the current folder has no data — chdir to the executable's folder
    // (for config.cfg, logs, etc.)
    if (strcmp(base_dir, ".") != 0) {
#ifdef _WIN32
        _chdir(base_dir);
#else
        chdir(base_dir);
#endif
    }
}

// Cascade of GL context versions. The window is already created at call time,
// DOUBLEBUFFER/DEPTH attributes are set.
static SDL_GLContext boot_gl_context(void)
{
#ifdef USE_GLES_PATH
    // GLES 3.0; if the driver refuses — try whatever version is available.
    static const int ctx_try[][3] = {
        { 3, 0, SDL_GL_CONTEXT_PROFILE_ES }, { 0, 0, 0 },
    };
#else
    // GL 4.5 CORE: needed for GPU-driven grass rendering (compute-cull +
    // indirect draw, GL 4.3+). A superset of 3.3, old shaders work as
    // before. If the driver won't give 4.5, vegetation.c falls back to the
    // instanced path on its own (g_gl_gpu_driven == 0).
    // Old GLX drivers (Mesa <= 3.1, NVIDIA legacy 340/470) do NOT downgrade
    // the request, they fail with GLXBadFBConfig — hence we iterate versions
    // downward and recreate the context: SDL re-reads the attributes on each call.
    static const int ctx_try[][3] = {
        { 4, 5, SDL_GL_CONTEXT_PROFILE_CORE },
        { 4, 3, SDL_GL_CONTEXT_PROFILE_CORE },
        { 3, 3, SDL_GL_CONTEXT_PROFILE_CORE },
        { 3, 1, SDL_GL_CONTEXT_PROFILE_CORE },
        { 0, 0, 0 },
    };
#endif
    SDL_GLContext ctx = NULL;
    for (unsigned ci = 0; ci < sizeof ctx_try / sizeof ctx_try[0] && !ctx; ci++) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, ctx_try[ci][0]);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, ctx_try[ci][1]);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, ctx_try[ci][2]);
        ctx = SDL_GL_CreateContext(g_win);
        if (!ctx && ctx_try[ci][0] > 0)
            SDL_Log("Контекст GL %d.%d не доступен (%s); пробую ниже",
                    ctx_try[ci][0], ctx_try[ci][1], SDL_GetError());
    }
    return ctx;
}

// FR_SHOT — save the frame to PPM (render debugging without Wayland).
// The value is the frame number to shoot (FR_SHOT=60), empty = 60.
static void loop_fr_shot(void)
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
            renderer_read_pixels(px, renderer_vw(), renderer_vh());
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

// host_framerate — FPS cap (delay before the next frame)
static void loop_framerate_limit(Uint64 frame_start)
{
    float hf = cvar_get("host_framerate");
    if (hf <= 0.0f) return;
    Uint64 target = (Uint64)(SDL_GetPerformanceFrequency() / hf);
    Uint64 elapsed = SDL_GetPerformanceCounter() - frame_start;
    if (elapsed < target) {
        Uint32 ms = (Uint32)((target - elapsed) * 1000 /
                             SDL_GetPerformanceFrequency());
        if (ms > 0 && ms < 100) SDL_Delay(ms);
    }
}

int main(int argc, char *argv[])
{
    (void)argc; (void)argv;

    // 1) SDL initialization — must happen BEFORE any SDL_* calls
    //    (including SDL_GetPrefPath/SDL_LoadFile in fr_android_prepare on Android).
    //    It used to sit later — on the device it crashed with SIGSEGV before the
    //    window was shown.
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        // SDL is not up yet — write to stderr
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    srand((unsigned)SDL_GetTicks());

    // Android: unpack APK assets into storage and chdir there (before base_dir).
    // Called AFTER SDL_Init — on Android JNI/env and AAssetManager exist by then.
    if (!fr_android_prepare()) {
#ifdef __ANDROID__
        SDL_Log("android: подготовка ресурсов не удалась");
        SDL_Quit();
        return 1;
#endif
    }

    // Base resources directory + chdir into it (map/, shaders/, img/).
    boot_resolve_base_dir();

    // 2) Window and context ----------------------------------------------------
    // Backend selection at compile time: make vulkan / make metal create windows
    // WITHOUT a GL context (the Vulkan/Metal device is brought up by sr_create()).
    // Until the render pipeline is ported to these APIs — the device is created,
    // the status is logged, the engine exits cleanly.
    int ww = WIN_W, wh = WIN_H;
    {
        const char *ew = getenv("FR_W"), *eh = getenv("FR_H");
        if (ew && eh) { ww = atoi(ew); wh = atoi(eh); }
    }
#if defined(FR_BACKEND_VULKAN) || defined(FR_BACKEND_METAL)
    {
        unsigned win_flags = SDL_WINDOW_RESIZABLE
#ifdef FR_BACKEND_VULKAN
            | SDL_WINDOW_VULKAN;
#else
            | SDL_WINDOW_METAL;
#endif
        g_win = SDL_CreateWindow("firerender", ww, wh, win_flags);
        if (!g_win) { SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError()); SDL_Quit(); return 1; }
        sr_api_report();
        SDL_Log("firerender build #%d", ENGINE_BUILD_NUM);
        SrDevice *dev = sr_create();
        if (dev) {
            const SrCaps *c = sr_caps(dev);
            SDL_Log("Backend %s: device OK (%s %s), но рендер-пайплайн на этот "
                    "API ещё не портирован (sr_* заглушки) — завершение.",
                    sr_api_name(
#ifdef FR_BACKEND_VULKAN
                        SR_API_VULKAN
#else
                        SR_API_METAL
#endif
                    ),
                    c->vendor, c->version);
            sr_destroy(dev);
        } else {
            SDL_Log("Backend %s: sr_create() не удался",
                    sr_api_name(
#ifdef FR_BACKEND_VULKAN
                        SR_API_VULKAN
#else
                        SR_API_METAL
#endif
                    ));
        }
        SDL_DestroyWindow(g_win);
        SDL_Quit();
        return 0;
    }
#endif
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

    g_win = SDL_CreateWindow("firerender", ww, wh,
                             SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (!g_win) { SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError()); SDL_Quit(); return 1; }
    SDL_GLContext ctx = boot_gl_context();
    if (!ctx) { SDL_Log("SDL_GL_CreateContext failed: %s", SDL_GetError());
                SDL_DestroyWindow(g_win); SDL_Quit(); return 1; }
    SDL_GL_MakeCurrent(g_win, ctx);

    // 4) Loading GL functions and vsync ----------------------------------------
    if (load_gl_funcs() != 0) {
        SDL_GL_DestroyContext(ctx); SDL_DestroyWindow(g_win); SDL_Quit(); return 1;
    }
    SDL_GL_SetSwapInterval(0);

    // 5) World state: systems (console/audio/font/commands) are brought up by
    //    the APP_BOOT phase — boot_init() in the main loop ---------------------
    Game game;
    game_init(&game);            // the single instance of the world state

    SDL_Log("Video driver: %s", SDL_GetCurrentVideoDriver());
#ifdef USE_GLES_PATH
    SDL_Log("Renderer profile: GLES LOW-END (LDR, cheap fog, instanced culling)");
#else
    SDL_Log("Renderer profile: DESKTOP GL");
#endif
    sr_api_report();   // render API status (GL/GLES + Vulkan/Metal stubs)
    SDL_Log("Graphics API: %s", glGetString(GL_VERSION));
    SDL_Log("GPU-driven veg (compute+indirect): %s",
            g_gl_gpu_driven ? "ENABLED" : "disabled (instanced fallback)");
    SDL_Log("firerender build #%d", ENGINE_BUILD_NUM);

    // 6) Viewport = actual window size in pixels -------------------------------
    int vw = ww, vh = wh;
    SDL_GetWindowSizeInPixels(g_win, &vw, &vh);
    SDL_Log("Drawable: %dx%d (requested %dx%d)", vw, vh, ww, wh);
    if (getenv("FR_BENCH") && (vw != ww || vh != wh))
        SDL_Log("BENCH warning: drawable differs from requested size");
    glViewport(0, 0, vw, vh);
    renderer_set_viewport(vw, vh);

    // 7) Back-face culling, blending --------------------------------------------
    glEnable(GL_CULL_FACE);
    glFrontFace(GL_CCW);
    glCullFace(GL_BACK);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // 8) Input: mouse/keyboard grab to the window. Mouse mode and cvar syncs
    //    are applied by the main loop (after boot_init). ------------------------
    input_init(g_win, &game);

    // 9) Main loop: phase APP_BOOT (boot_init) -> MENU/PLAY/PAUSE --------------
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

        // FR_STAGES — temporary frame instrumentation: per-stage averages are
        // printed to the log at exit (hunting engine CPU hotspots).
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

        input_process();                    // events: console, binds, resize
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

        // APP_BOOT phase: bring systems up once (ends with APP_MENU/PLAY)
        if (app_state() == APP_BOOT) {
            boot_init(&game);
            if (bench.active) {
                const char *preset = getenv("FR_BENCH_PRESET");
                app_apply_benchmark_preset(preset ? atoi(preset) : 0);
            }
            if (fr_test_apply(&game) != 0) goto fail;   // FR_TEST: the world

            if (getenv("FR_CONSOLE")) console_toggle();   // console at startup
            // Resync subsystems with cvars AFTER the world loads. shadow_init/
            // voxfog_init read the mode at the start of game_load_world, but
            // map_apply, graphics presets and the map script may change
            // r_shadows_mode/r_fog_mode already AFTER initialization. We used
            // to only reseed the trackers with the current value — and sync_cvars,
            // seeing no "change", NEVER brought g_shadow_on/fog in line with the
            // mode: CSM stayed enabled in blob/off mode (the shadow map was
            // rendered for nothing), and fog got stuck in the mode from
            // initialization. Now we forcibly sync the state with the final mode.
            s_sync_shadows = cvar_get("r_shadows_mode");
            shadow_set(s_sync_shadows == 1.0f || s_sync_shadows == 2.0f);
            s_sync_fog = cvar_get("r_fog_mode");
            voxfog_set_mode((int)s_sync_fog);
            // FR_TEST 's'/'o' toggle the subsystem ON TOP of the mode (A/B frames) —
            // apply after the resync, otherwise shadow_init/voxfog_init wiped them.
            {
                const char *ft = getenv("FR_TEST");
                if (ft && strchr(ft, 's')) shadow_toggle();
                if (ft && strchr(ft, 'o')) voxfog_set(1);
            }
        }

        // In the APK assets are immutable, and periodic stat() calls only wake up storage.
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

        game_demo_tick(&game, dt, inst_fps); // demo scene (Q): auto-fly + FPS
        ST_ACC(s_st_demo);

        // Fixed logic/physics tick: 100 Hz on desktop, 60 Hz on the phone (CPU saving)
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
                    game_update(&game, step);   // world: nodes, triggers/use, fog
                    ST_ACC(s_st_gu);
                    physics_set_ground(phys_ground);
                    phys_body_enable(cvar_get("phys_bodies") != 0.0f);
                    phys_body_step(physics_world(), step);  // bodies+ropes,
                                        // physics layered over physics_step (phys_body.c)
                    glb_anim_ragdoll_step_all(step);       // ragdolls of animated models
                    ST_ACC(s_st_phys);
                    audio_update();             // background loops and 3D sources
                    lua_engine_update(step);    // per-frame script callback
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

        // FR_CVAR="name value;name value" — override cvar's for
        // headless tests AFTER config.cfg (otherwise archived values would
        // overwrite them).
        {
            static int s_fr_cvar_done = 0;
            const char *fc = getenv("FR_CVAR");
            if (fc && !s_fr_cvar_done) {
                s_fr_cvar_done = 1;
                char buf[512];
                snprintf(buf, sizeof buf, "%s", fc);
                for (char *tok = strtok(buf, ";"); tok; tok = strtok(NULL, ";")) {
                    char name[64]; float val;
                    if (sscanf(tok, "%63s %f", name, &val) == 2) {
                        cvar_set(name, val);
                        SDL_Log("FR_CVAR: %s = %g", name, val);
                    }
                }
            }
        }

        // FR_TEST 'H': auto-aim the camera at the ragdoll every frame — the
        // ragdoll physics test must not depend on where the model crawled to
        // (otherwise it leaves the frame and the bug is invisible).
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

        // FR_TEST 'K': kill the player ~1.2 s before the shot (headless FR_TEST=SK) —
        // checks the death screen, camera "fall" and input freeze.
        if (strchr(getenv("FR_TEST") ? getenv("FR_TEST") : "", 'K') &&
            game.world_loaded && app_state() == APP_PLAY &&
            !health_is_dead(&game.player.hp)) {
            static float k_t = 0.0f;
            k_t += dt;
            if (k_t > 0.1f) { health_kill(&game.player.hp);
                              SDL_Log("FR_TEST K: player killed"); }
        }

        // Deferred events (events_emit_deferred) — a safe point after all
        // ticks, before render: callbacks must not reuse listener lists
        // mid-iteration.
        events_flush_deferred();
        ST_ACC(s_st_sync);

        // Render: scene -> bloom/fog -> overlays (see renderer.c)
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
        // FR_TEST 'S': auto-screenshot at frame 450 and exit at frame 480 —
        // headless picture check + stable warmup for the profiler.
        // The request is set before renderer_end so the readback happens in the
        // same frame: some EGL backbuffers are already discarded after overlay draw.
        if (strchr(getenv("FR_TEST") ? getenv("FR_TEST") : "", 'S')) {
            static int s_frames = 0, s_shot_at = -1;
            if (s_shot_at < 0) {
                const char *fs = getenv("FR_SHOT");
                s_shot_at = fs ? atoi(fs) : 450;
            }
            if (++s_frames == s_shot_at) cmd_screenshot(NULL);
            // FR_BENCH lives tens of seconds (warmup + measurement) — exit only
            // on bench.done; the shot still happens at s_shot_at.
            if (s_frames >= s_shot_at + 30 && !bench.active) running = false;
        }
        // FR_BAKE: offline light baking of the map (make mapbake). Warm up a few
        // frames (sun/CSM/rc settle), bake the terrain RGB texel + probe grid
        // into map/baked/ and exit. The engine afterwards only READS them (after
        // make mapzip puts baked/ into the map zip).
        if (getenv("FR_BAKE") && game.world_loaded &&
            app_state() == APP_PLAY && game.sun_node) {
            static int s_bake_warm = 0;
            if (++s_bake_warm == 30) {
                SDL_Log("FR_BAKE: запекаю свет карты...");
                terrain_light_bake(&game.world,
                                   (SunParams *)game.sun_node->params,
                                   renderer_context());
                SDL_Log("FR_BAKE: готово. Свет в map/baked/ — запусти make mapzip.");
                running = false;
            }
        }
        renderer_end(&game, dt);
        ST_ACC(s_st_end);

        // Menu (main/pause/settings) — over the frozen scene
        if (app_state() != APP_PLAY) {
            int act = app_menu(renderer_vw(), renderer_vh());
            if (act == APP_ACT_START || act == APP_ACT_RESUME) {
                if (!game.world_loaded && game_load_world(&game) != 0) {
                    SDL_Log("World load failed — остаёмся в меню");
                    console_print("ошибка загрузки карты");
                    // do not exit the app, give a chance to pick another map
                    continue;
                }
                app_set_state(APP_PLAY);
                if (act == APP_ACT_START) {
                    // New game: camera to spawn, reset demo.
                    // (if this is a load from save — teleport instead of spawn)
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
                if (game.world_loaded) game_unload_world(&game);  // to menu
                app_set_state(APP_MENU);
            } else if (act == APP_ACT_QUIT) {
                g_quit_request = 1;
            }
        }
        commands_shot_take(renderer_vw(), renderer_vh());     // screenshot (F12)
        if (app_state() == APP_PLAY)
            touch_hud_draw(renderer_vw(), renderer_vh());     // virtual joystick
        console_drain();                                      // deferred wait-commands
        console_render(renderer_vw(), renderer_vh());         // console — over everything (`)
        bench_gpu_end(&bench);

        loop_fr_shot();

        SDL_GL_SwapWindow(g_win);
        ST_ACC(s_st_swap);
        s_st_n++;
        {
            double fr = (SDL_GetPerformanceCounter() - now) / st_f;
            if (fr > s_st_frame_max) s_st_frame_max = fr;
        }

        loop_framerate_limit(now);

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

    // 11) Cleanup --------------------------------------------------------------
    bench_gpu_cleanup(&bench);
    console_save_config();       // config.cfg: cvar's to disk
    game_map_config_save(&game); // + selected map
    game_unload_world(&game);    // unload the world (if loaded)
    audio_cleanup();             // audio: channels and cache
    hud_shared_cleanup();        // font/atlas/text render
    resources_shutdown();        // resource cache
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
