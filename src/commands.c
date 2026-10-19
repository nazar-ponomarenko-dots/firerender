// ============================================================================
//  commands.c — console action commands (see commands.h)
// ----------------------------------------------------------------------------
//  Extracted from main.c (lines 215-660): HUD/map/shadow/fog toggles,
//  demo, pause, map selection, save/load/setpos, entities, screenshot, sound.
//  Registration of all static commands lives in the SINGLE registry (cmdreg.c).
// ============================================================================
#include "commands.h"
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "attach.h"
#include "audio.h"
#include "camera.h"
#include "console.h"
#include "cvars.h"
#include "fx.h"
#include "entity.h"
#include "physics.h"
#include "events.h"
#include "game.h"
#include "gl_loader.h"
#include "glb.h"
#include "glb_anim.h"
#include "mapdata.h"
#include "material.h"
#include "mesh.h"
#include "png.h"
#include "renderer.h"
#include "resources.h"
#include "selection.h"
#include "shader.h"
#include "splat.h"
#include "world.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifdef _WIN32
#  include <direct.h>
#endif

// --- Toggles and modes ------------------------------------------------------------
void cmd_noclip(const char *args)
{
    (void)args;
    Game *g = game_instance();
    if (cvar_get("sv_cheats") < 0.5f) {
        console_print("noclip: требуется sv_cheats 1");
        return;
    }
    if (!g->cam) return;
    // Noclip — toggles character collision WITHOUT teleporting: the character
    // stays in place, it just passes through terrain and objects (and stops
    // doing so once noclip is turned off). Flying (fly) is a separate mode.
    g->cam->cam.noclip = !g->cam->cam.noclip;
    SDL_Log("Noclip: %s", g->cam->cam.noclip ? "ON" : "OFF");
}

void cmd_toggle_map(const char *args)
{
    (void)args;
    Game *g = game_instance();
    if (!g->mm) return;
    g->mm->visible = !g->mm->visible;
    SDL_Log("Map: %s", g->mm->visible ? "ON" : "OFF");
}

void cmd_toggle_debug(const char *args)
{
    (void)args;
    Game *g = game_instance();
    if (!g->hud) return;
    g->hud->visible = !g->hud->visible;
    selection_set_on(g->hud->visible);
    SDL_Log("Debug: %s", g->hud->visible ? "ON" : "OFF");
}

void cmd_toggle_crosshair(const char *args)
{
    (void)args;
    float on = cvar_get("r_crosshair") > 0.5f ? 0.0f : 1.0f;
    cvar_set("r_crosshair", on);
    SDL_Log("Crosshair: %s", on > 0.5f ? "ON" : "OFF");
}

void cmd_toggle_shadows(const char *args)
{
    (void)args;
    // quick toggle: dynamic <-> off
    float m = cvar_get("r_shadows_mode");
    cvar_set("r_shadows_mode", m == 1.0f ? 0.0f : 1.0f);
    SDL_Log("Shadows: %s",
            cvar_get("r_shadows_mode") == 1.0f ? "CSM" : "OFF");
}

void cmd_toggle_fog(const char *args)
{
    (void)args;
    // quick toggle: volumetric <-> off (simple mode is menu-only)
    float m = cvar_get("r_fog_mode");
    cvar_set("r_fog_mode", m == 1.0f ? 0.0f : 1.0f);
    SDL_Log("Fog mode: %d", (int)cvar_get("r_fog_mode"));
}

void cmd_demo(const char *args)
{
    (void)args;
    game_demo_toggle(game_instance());
}

void cmd_pause(const char *args)
{
    (void)args;
    app_escape();   // game <-> pause, in menus goes back through screens
}

// --- Maps -------------------------------------------------------------------------
// Map selection: map <name> — "map" (folder) or a name from maps/*.zip.
// If a world is loaded — restarts the game on the new map right away.
static int s_map_switching = 0;   // recursion: a script/command loads a map inside game_load_world
static int map_reload(Game *g, const char *name, int force)
{
    if (!g || !name || g->world_loading || s_map_switching) {
        console_print("map: вложенный вызов игнорируется (карта уже грузится)");
        return -1;
    }
    char target_name[sizeof g->map_name];
    char old_name[sizeof g->map_name];
    snprintf(target_name, sizeof target_name, "%s", name);
    snprintf(old_name, sizeof old_name, "%s", g->map_name);
    if (!force && g->world_loaded && strcmp(target_name, old_name) == 0) {
        console_print("map \"%s\" уже загружена", target_name);
        return 0;
    }
    // Check the package before tearing down the current world. A full load may
    // still fail later, but a plain typo no longer kicks the player out.
    if (strcmp(target_name, "map") != 0) {
        char path[160], resolved[512];
        snprintf(path, sizeof path, "maps/%s.zip", target_name);
        if (!resources_resolve_path(path, resolved, sizeof resolved)) {
            console_print("map: нет %s", path);
            return -1;
        }
    }
    snprintf(g->map_name, sizeof g->map_name, "%s", target_name);
    if (!g->world_loaded) {
        SDL_Log("Map selected: %s (will load on NEW GAME)", g->map_name);
        return 0;
    }
    s_map_switching = 1;
    game_unload_world(g);
    int rc = game_load_world(g);
    if (rc != 0) {
        SDL_Log("Map %s load failed; restoring %s", target_name, old_name);
        if (g->world_loaded) game_unload_world(g);
        snprintf(g->map_name, sizeof g->map_name, "%s", old_name);
        int rollback_rc = game_load_world(g);
        s_map_switching = 0;
        if (rollback_rc == 0) {
            app_set_state(APP_PLAY);
            console_print("map: загрузка %s не удалась, восстановлена %s",
                          target_name, old_name);
        } else {
            SDL_Log("Map rollback %s failed", old_name);
            app_set_state(APP_MENU);
        }
        return -1;
    }
    s_map_switching = 0;
    game_demo_stop(g);
    bool was_load = g->player.pending_spawn != 0;
    if (g->player.pending_spawn) {
        game_apply_spawn(g, g->player.save_pos, g->player.save_yaw,
                         g->player.save_pitch);
        g->player.pending_spawn = 0;
    }
    if (was_load) commands_apply_saved_world_state();
    app_set_state(APP_PLAY);
    SDL_Log("Map loaded: %s", g->map_name);
    return 0;
}

void cmd_map_select(const char *args)
{
    Game *g = game_instance();
    char name[64];
    if (sscanf(args, "%63s", name) != 1 || !name[0]) {
        console_print("current map: %s", g->map_name);
        console_print("usage: map <name>  (\"map\" — папка map/, иначе maps/<name>.zip)");
        console_print("maps — список карт");
        return;
    }
    map_reload(g, name, 0);
}

// List of available maps (maps command)
void cmd_maps_list(const char *args)
{
    (void)args;
    console_print("maps:");
    app_maps_print();
}

// HL-style: commands adapted to our systems
void cmd_restart(const char *args)
{
    (void)args;
    Game *g = game_instance();
    map_reload(g, g->map_name, 1);
}

void cmd_changelevel(const char *args)
{
    cmd_map_select(args);   // map change (we do not preserve state)
}

void cmd_r_drawdebug(const char *args)
{
    Game *g = game_instance();
    float v;
    if (sscanf(args, "%f", &v) == 1) {
        if (!g->hud) return;
        g->hud->visible = v > 0.5f;
        selection_set_on(g->hud->visible);
        SDL_Log("Debug: %s", g->hud->visible ? "ON" : "OFF");
        return;
    }
    console_print("r_drawdebug = %d", g->hud && g->hud->visible);
}

void cmd_mat_wireframe(const char *args)
{
    float v;
    if (sscanf(args, "%f", &v) == 1)
        cvar_set("r_wireframe", v > 0.5f ? 1.0f : 0.0f);
    console_print("mat_wireframe = %d", cvar_get("r_wireframe") > 0.5f);
}

void cmd_thirdperson(const char *args)
{
    (void)args;
    cvar_set("cam_thirdperson", 1.0f);
    console_print("camera: thirdperson");
}

void cmd_firstperson(const char *args)
{
    (void)args;
    cvar_set("cam_thirdperson", 0.0f);
    console_print("camera: firstperson");
}

// --- Entities ------------------------------------------------------------------
// ent_create <class> [x y z] — spawns an entity (classes come from the registry)
void cmd_ent_create(const char *args)
{
    Game *g = game_instance();
    if (cvar_get("sv_cheats") < 0.5f) {
        console_print("ent_create: требуется sv_cheats 1");
        return;
    }
    char cls[64];
    float x = 0, y = 0, z = 0;
    int n = sscanf(args, "%63s %f %f %f", cls, &x, &y, &z);
    if (n < 1) {
        console_print("usage: ent_create <класс> [x y z]");
        return;
    }
    if (!g->cam) {
        console_print("ent_create: мир не загружен");
        return;
    }
    float pos[3] = { x, y, z };
    if (n == 1) {   // in front of the camera, at eye height
        float fwd[3];
        camera_dirs(&g->cam->cam, fwd, NULL);
        pos[0] = g->cam->cam.pos[0] + fwd[0] * 5.0f;
        pos[1] = g->cam->cam.pos[1];
        pos[2] = g->cam->cam.pos[2] + fwd[2] * 5.0f;
    }
    Entity *e = entity_spawn(cls, pos, NULL, NULL);
    if (e)
        console_print("entity \"%s\" спавнена в (%.0f, %.0f, %.0f)",
                      cls, pos[0], pos[1], pos[2]);
}

// ent_list — entity list
void cmd_ent_list(const char *args)
{
    (void)args;
    console_print("entities: %d", entity_count());
    for (int i = 0; i < entity_count(); i++) {
        Entity *e = entity_at(i);
        console_print("  %-12s (%.0f, %.0f, %.0f)", e->classname,
                      e->node->pos[0], e->node->pos[1], e->node->pos[2]);
    }
}

// --- Screenshot: save the frame as a PNG into the screenshots/ folder --------------
static int g_shot_req = 0;

void cmd_screenshot(const char *args)
{
    (void)args;
    g_shot_req = 1;
    console_print("screenshot: кадр будет сохранён в screenshots/");
}

// Creates a directory (utility, cross-platform)
static void make_dir(const char *path)
{
#ifdef _WIN32
    _mkdir(path);
#else
    mkdir(path, 0755);
#endif
}

// Reads the frame and writes a PNG (flipped in Y, as the player sees it)
static void take_shot_png(const char *path, int vw, int vh)
{
    unsigned char *px = malloc((size_t)vw * vh * 3);
    if (!px) return;
    renderer_read_pixels(px, vw, vh);
    // GL rows go bottom-up — flip them for PNG
    size_t row = (size_t)vw * 3;
    unsigned char *flip = malloc(row * (size_t)vh);
    if (flip) {
        for (int y = 0; y < vh; y++)
            memcpy(flip + (size_t)y * row, px + (size_t)(vh - 1 - y) * row, row);
        if (png_save(path, vw, vh, flip) == 0)
            SDL_Log("screenshot: %s (%dx%d)", path, vw, vh);
        else
            SDL_Log("screenshot: failed to write %s", path);
        free(flip);
    }
    free(px);
}

// Called from the main loop every frame (screenshot command / F12)
void commands_shot_take(int vw, int vh)
{
    if (!g_shot_req) return;
    g_shot_req = 0;
    char path[320];
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    if (!tm) {   // localtime may return NULL — do not dereference tm->*
        SDL_Log("screenshot: localtime failed");
        return;
    }
    snprintf(path, sizeof path, "screenshots/shot_%04d%02d%02d_%02d%02d%02d.png",
             tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
             tm->tm_hour, tm->tm_min, tm->tm_sec);
    make_dir("screenshots");
    take_shot_png(path, vw, vh);
}

// --- Sound --------------------------------------------------------------------------
// Sound check: plays all engine sounds one after another
void cmd_snd_test(const char *args)
{
    (void)args;
    const char *paths[] = {
        "audio/click.wav", "audio/step1.wav", "audio/step2.wav",
        "audio/step3.wav", "audio/jump.wav", "audio/land.wav",
        "audio/wind.wav", "audio/blip.wav",
    };
    for (size_t i = 0; i < sizeof paths / sizeof paths[0]; i++) {
        Sound *s = audio_load(paths[i]);
        if (s) {
            audio_play(s, 0.45f, 1.0f);
            console_print("snd_test: %s", paths[i]);
        } else {
            console_print("snd_test: НЕТ звука %s", paths[i]);
        }
        SDL_Delay(200);
    }
}

// --- Shader hot-reload ----------------------------------------------------------------
void cmd_reload_shaders(const char *args)
{
    (void)args;
    int n = shader_hot_reload();
    n += resources_texture_reload();
    console_print("reload_shaders: %d", n);
}

// --- Save/Load (F5/F9): player position + map + world state -------------------
// Besides the position it stores which triggers have already fired and which
// use-objects were used, so that on load the world "continues" instead of resetting.
#define SAVE_MAX_TRIGGER 64
#define SAVE_MAX_OBJUSED 128
static int s_save_triggers[SAVE_MAX_TRIGGER];    // trigger: 0/1
static int s_save_objects_used_n = 0;            // objects: name already used
static char s_save_objects_used[SAVE_MAX_OBJUSED][64];

void cmd_save(const char *args)
{
    (void)args;
    Game *g = game_instance();
    if (!g->cam) { console_print("save: мир не загружен"); return; }
    player_save(&g->player, &g->cam->cam, g->map_name);
    FILE *f = fopen("save.cfg", "w");
    if (!f) { console_print("save: не могу записать save.cfg"); return; }
    fprintf(f, "map_name %s\n", g->player.save_map);
    fprintf(f, "setpos %.2f %.2f %.2f %.4f %.4f\n",
            g->player.save_pos[0], g->player.save_pos[1], g->player.save_pos[2],
            g->player.save_yaw, g->player.save_pitch);
    // World state: fired triggers + used use-objects
    int tn = mapdata_trigger_count();
    if (tn > SAVE_MAX_TRIGGER) tn = SAVE_MAX_TRIGGER;
    for (int i = 0; i < tn; i++)
        fprintf(f, "trigger_fired %d %d\n", i, mapdata_trigger_fired(i));
    for (int i = 0; i < mapdata_object_count(); i++) {
        const MapObject *o = mapdata_object(i);
        if (o && o->use.set && o->use.used)
            fprintf(f, "object_used %s\n", o->name);
    }
    fclose(f);
    console_print("save: %s (%.0f,%.0f,%.0f)", g->player.save_map,
                  g->player.save_pos[0], g->player.save_pos[1],
                  g->player.save_pos[2]);
}

// Applies the saved world state to the current map (if it matches).
void commands_apply_saved_world_state(void)
{
    Game *g = game_instance();
    if (!g) return;
    if (!g->world_loaded || !g->map_name[0]) return;
    // only if the save belongs to the current map
    if (strcmp(g->player.save_map, g->map_name) != 0) return;
    for (int i = 0; i < SAVE_MAX_TRIGGER; i++)
        mapdata_trigger_set_fired(i, s_save_triggers[i]);
    for (int i = 0; i < s_save_objects_used_n; i++)
        mapdata_object_set_used(s_save_objects_used[i], 1);
}

// --- Effects at the gaze raycast hit point ------------------------------------
// m - sparks (add), comma - smoke (alpha), period - blood (bounce)
static void cmd_fx_raycast(const char *args, const char *preset, int count)
{
    (void)args;
    Game *g = game_instance();
    if (!g || !g->cam || !g->world_loaded) {
        console_print("fx: мир не загружен");
        return;
    }
    float x, y, z;
    if (!camera_raycast_terrain(&g->cam->cam, &g->world, 50.0f, &x, &y, &z)) {
        console_print("fx: луч не попал в рельеф");
        return;
    }
    fx_burst(preset, x, y, z, count);
}

void cmd_fx_sparks(const char *args)  { cmd_fx_raycast(args, "sparks", 30); }
void cmd_fx_smoke(const char *args)   { cmd_fx_raycast(args, "smoke", 16); }
void cmd_fx_blood(const char *args)   { cmd_fx_raycast(args, "blood", 24); }

// --- Save/Load (F5/F9) -----------------------------------------------------------
void cmd_load(const char *args)
{
    (void)args;
    Game *g = game_instance();
    FILE *f = fopen("save.cfg", "r");
    if (!f) { console_print("load: нет save.cfg (F5 — сохранить)"); return; }
    char line[256], map[64] = "";
    float x = 0, y = 0, z = 0, ya = 0, pi = 0;
    // Reset the world-state buffer
    for (int i = 0; i < SAVE_MAX_TRIGGER; i++) s_save_triggers[i] = 0;
    s_save_objects_used_n = 0;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "map_name %63s", map) == 1) continue;
        if (sscanf(line, "setpos %f %f %f %f %f", &x, &y, &z, &ya, &pi) >= 3)
            continue;
        int ti = -1, tv = 0;
        if (sscanf(line, "trigger_fired %d %d", &ti, &tv) == 2) {
            if (ti >= 0 && ti < SAVE_MAX_TRIGGER) s_save_triggers[ti] = tv;
            continue;
        }
        char name[64] = "";
        if (sscanf(line, "object_used %63s", name) == 1) {
            if (name[0] && s_save_objects_used_n < SAVE_MAX_OBJUSED) {
                snprintf(s_save_objects_used[s_save_objects_used_n], 64, "%s", name);
                s_save_objects_used_n++;
            }
            continue;
        }
    }
    fclose(f);
    if (!map[0]) { console_print("load: save.cfg повреждён"); return; }
    player_load(&g->player, x, y, z, ya, pi, map);
    if (g->world_loaded && strcmp(g->player.save_map, g->map_name) == 0) {
        game_apply_spawn(g, g->player.save_pos, g->player.save_yaw,
                         g->player.save_pitch);
        commands_apply_saved_world_state();   // restore triggers/use
        console_print("load: загружено (%.0f,%.0f,%.0f)", x, y, z);
        return;
    }
    // Different map (or world not loaded yet): load it and teleport.
    // The state gets applied automatically inside game_load_world.
    g->player.pending_spawn = 1;
    if (g->world_loaded) {
        cmd_map_select(g->player.save_map);
    } else {
        snprintf(g->map_name, sizeof g->map_name, "%s", g->player.save_map);
        SDL_Log("Map selected: %s (from save)", g->map_name);
    }
    console_print("load: карта %s, телепорт при загрузке", g->player.save_map);
}

// Player teleport: setpos x y z [yaw] [pitch]
void cmd_setpos(const char *args)
{
    Game *g = game_instance();
    float x, y, z, ya = 0, pi = 0;
    int n = sscanf(args, "%f %f %f %f %f", &x, &y, &z, &ya, &pi);
    if (n < 3) {
        console_print("usage: setpos x y z [yaw] [pitch]");
        return;
    }
    if (!g->cam) { console_print("setpos: мир не загружен"); return; }
    if (n < 4) ya = g->cam->cam.yaw;
    if (n < 5) pi = g->cam->cam.pitch;
    float p[3] = { x, y, z };
    game_apply_spawn(g, p, ya, pi);
}

// Place a static mesh (.obj): mesh <path> [x y z] [scale] [tex]
void cmd_mesh(const char *args)
{
    Game *g = game_instance();
    if (!g || !g->root) { console_print("mesh: мир не загружен"); return; }
    char path[256] = "", tex[256] = "";
    float x = 0, y = 0, z = 0, scale = 1.0f;
    int n = sscanf(args, "%255s %f %f %f %f %255s", path, &x, &y, &z, &scale, tex);
    g = game_instance();
    if (n < 1) { console_print("usage: mesh <obj> [x y z] [scale] [tex]"); return; }
    if (!g || !g->root) return;
    float pos[3] = { x, y, z };
    SceneNode *nd = mesh_make_node(path, pos, scale,
                                   (n >= 6 && tex[0]) ? tex : NULL,
                                   renderer_context());
    if (!nd) { console_print("mesh: не удалось загрузить %s", path); return; }
    node_add(g->root, nd);
    // Occupancy: grass does not grow inside a placed mesh (splat.c)
    splat_occupy_circle(x, z, 1.0f * (scale > 0.01f ? scale : 1.0f), 1.0f);
    console_print("mesh: %s в (%.0f,%.0f,%.0f)", path, x, y, z);
}

// Place a binary glTF (.glb): glb <path> [x y z] [scale]
static SceneNode *s_last_anim;   // last placed AnimInstance3D

// The anim node pointed to by s_last_anim may be killed/removed
// (entity_kill, world unload) — the pointer would become dangling. Clear it on
// EV_NODE_EXIT while node_free_tree has not yet freed the node itself.
static void anim_last_exit(const Event *e, void *user)
{
    (void)user;
    if (e && e->a == (const void *)s_last_anim) s_last_anim = NULL;
}

static void anim_last_hook(void)
{
    static int s_hooked = 0;
    if (!s_hooked) { events_listen(EV_NODE_EXIT, anim_last_exit, NULL); s_hooked = 1; }
}

// Anim node: by name (if given) or the last placed one.
static SceneNode *anim_find(const char *name)
{
    Game *g = game_instance();
    SceneNode *nd = NULL;
    if (name && name[0] && g && g->root) nd = node_find(g->root, name);
    if (!nd) nd = s_last_anim;
    return nd;
}

void cmd_glb(const char *args)
{
    Game *g = game_instance();
    if (!g || !g->root) { console_print("glb: мир не загружен"); return; }
    char path[256] = "";
    float x = 0, y = 0, z = 0, scale = 1.0f;
    int n = sscanf(args, "%255s %f %f %f %f", path, &x, &y, &z, &scale);
    if (n < 1) { console_print("usage: glb <glb> [x y z] [scale]"); return; }
    // Auto-placement ON THE GROUND: without coordinates — 6 m in front of the
    // player along the view ("glb model 12" — 12 m); with y <= 0 — at the XZ point
    // with terrain height. Spawning cars/props without manual height tuning.
    if (n < 4 && g->cam) {
        const float *cp = g->cam->cam.pos;
        float fwd[3] = { 0, 0, 1 };
        camera_dirs(&g->cam->cam, fwd, NULL);
        float d = (n >= 2) ? x : 6.0f;         // "glb model 12" — 12 m away
        x = cp[0] + fwd[0] * d;
        z = cp[2] + fwd[2] * d;
        y = sample_height(&g->world, x, z) + 0.3f;
    } else if (y <= 0.0f) {
        y = sample_height(&g->world, x, z) + 0.3f;
    }
    float pos[3] = { x, y, z };
    SceneNode *nd = NULL;
    // auto-select: skin + animation present -> skeletal path (skinning)
    int is_anim = glb_anim_has_anim(path);
    if (is_anim)
        nd = glb_anim_make_node(path, pos, scale, renderer_context());
    else
        nd = glb_make_node(path, pos, scale, renderer_context());
    if (!nd) { console_print("glb: не удалось загрузить %s", path); return; }
    anim_last_hook();
    node_add(g->root, nd);
    // Occupancy: grass does not grow inside a placed model (splat.c).
    // The radius is derived from the scale — no need to compute an exact
    // sphere, what matters is that blades do not stick through the body/hull.
    splat_occupy_circle(x, z, 1.2f * (scale > 0.01f ? scale : 1.0f), 1.0f);
    // glb_anim_clip_count/... work ONLY with AnimInstance3D (AnimParams).
    // A static GlbInstance has no data field — reading it would SEGV.
    if (is_anim && glb_anim_clip_count(nd) > 0) s_last_anim = nd;
    console_print("glb: %s в (%.0f,%.0f,%.0f)", path, x, y, z);
}

// Place an animated glTF (.glb): glbanim <path> [x y z] [scale]
void cmd_glbanim(const char *args)
{
    Game *g = game_instance();
    if (!g || !g->root) { console_print("glbanim: мир не загружен"); return; }
    char path[256] = "";
    float x = 0, y = 0, z = 0, scale = 1.0f;
    int n = sscanf(args, "%255s %f %f %f %f", path, &x, &y, &z, &scale);
    if (n < 1) { console_print("usage: glbanim <glb> [x y z] [scale]"); return; }
    // Auto-placement ON THE GROUND — same as cmd_glb: without coordinates —
    // 6 m in front of the player along the view; with y <= 0 — terrain height at XZ.
    if (n < 4 && g->cam) {
        const float *cp = g->cam->cam.pos;
        float fwd[3] = { 0, 0, 1 };
        camera_dirs(&g->cam->cam, fwd, NULL);
        float d = (n >= 2) ? x : 6.0f;
        x = cp[0] + fwd[0] * d;
        z = cp[2] + fwd[2] * d;
        y = sample_height(&g->world, x, z) + 0.3f;
    } else if (y <= 0.0f) {
        y = sample_height(&g->world, x, z) + 0.3f;
    }
    float pos[3] = { x, y, z };
    SceneNode *nd = glb_anim_make_node(path, pos, scale, renderer_context());
    if (!nd) { console_print("glbanim: не удалось загрузить %s", path); return; }
    anim_last_hook();
    node_add(g->root, nd);
    splat_occupy_circle(x, z, 1.0f * (scale > 0.01f ? scale : 1.0f), 1.0f);
    s_last_anim = nd;
    console_print("glbanim: %s в (%.0f,%.0f,%.0f)", path, x, y, z);
    if (glb_anim_clip_count(nd) > 0) {
        console_print("  клипы: %d, активный: %s",
                      glb_anim_clip_count(nd),
                      glb_anim_current_clip(nd) ? glb_anim_current_clip(nd) : "?");
        for (int i = 0; i < glb_anim_clip_count(nd); i++)
            console_print("  [%d] %s", i, glb_anim_clip_name(nd, i) ? glb_anim_clip_name(nd, i) : "?");
    }
}

// --- Mini-physics (Verlet): demo rope in front of the camera ------------------
void cmd_phys_rope(const char *args)
{
    Game *g = game_instance();
    if (!g || !g->cam || !g->world_loaded) {
        console_print("phys: мир не загружен");
        return;
    }
    int n = 12;
    sscanf(args, "%d", &n);
    if (n < 2) n = 2;
    if (n > 48) n = 48;
    PhysWorld *w = physics_world();
    physics_reset(w);
    const float *cp = g->cam->cam.pos;
    float fwd[3] = { 0, 0, 1 };
    camera_dirs(&g->cam->cam, fwd, NULL);
    float ax = cp[0] + fwd[0] * 5.0f;
    float az = cp[2] + fwd[2] * 5.0f;
    float ay = sample_height(&g->world, ax, az) + 3.0f;
    float seg = 0.35f;
    for (int i = 0; i < n; i++) {
        float p[3] = { ax, ay - i * seg, az };
        physics_add_part(w, p, 1.0f, 0.08f);
        if (i > 0) physics_add_link(w, i - 1, i);
    }
    physics_pin(w, 0, 1);                    // first node pinned in mid-air
    console_print("phys: верёвка из %d звеньев в (%.0f,%.0f)", n, ax, az);
}

void cmd_phys_push(const char *args)
{
    Game *g = game_instance();
    PhysWorld *w = physics_world();
    if (!g || !g->cam || w->np <= 0) { console_print("phys: верёвки нет (phys_rope)"); return; }
    float force = 10.0f;
    sscanf(args, "%f", &force);
    float fwd[3] = { 0, 0, 1 };
    camera_dirs(&g->cam->cam, fwd, NULL);
    float vel[3] = { fwd[0] * force, force * 0.3f, fwd[2] * force };
    physics_impulse(w, w->np - 1, vel);
    console_print("phys: толчок %.0f м/с", force);
}

void cmd_phys_clear(const char *args)
{
    (void)args;
    physics_reset(physics_world());
    console_print("phys: очищено");
}

// phys_ragdoll [hang] [kick] — ragdoll on the last animated model:
// bones become Verlet particles, animation gives way to physics.
// hang=1 (default) — origin pinned, the model sags in mid-air;
// kick — impulse to all particles (m/s) — a shove, the model falls over.
void cmd_phys_ragdoll(const char *args)
{
    int hang = 1, kick = 0;
    sscanf(args, "%d %d", &hang, &kick);
    if (!s_last_anim) {
        console_print("phys_ragdoll: нет аним-модели (сначала glbanim ...)");
        return;
    }
    if (glb_anim_ragdoll_enable(s_last_anim, hang) != 0) {
        console_print("phys_ragdoll: не удалось (слишком много костей?)");
        return;
    }
    Game *g = game_instance();
    if (kick > 0 && g && g->cam) {
        float fwd[3] = { 0, 0, 1 };
        camera_dirs(&g->cam->cam, fwd, NULL);
        float vel[3] = { fwd[0] * kick, kick * 0.4f, fwd[2] * kick };
        glb_anim_ragdoll_kick(s_last_anim, vel);
    }
    console_print("phys_ragdoll: включён (%s%s)", hang ? "висит" : "падает",
                  kick > 0 ? " + толчок" : "");
}

// List of the node's animation clips: glbclips [node-name]
void cmd_glbclips(const char *args)
{
    char name[128] = "";
    sscanf(args, "%127s", name);
    SceneNode *nd = anim_find(name);
    if (!nd) { console_print("glbclips: нет анимик-узла (спавн через glbanim/glb)"); return; }
    int nc = glb_anim_clip_count(nd);
    console_print("glbclips: узел %s, клипов %d, активный: %s",
                  nd->name ? nd->name : "?", nc,
                  glb_anim_current_clip(nd) ? glb_anim_current_clip(nd) : "?");
    for (int i = 0; i < nc; i++)
        console_print("  [%d] %s", i, glb_anim_clip_name(nd, i) ? glb_anim_clip_name(nd, i) : "?");
}

// Switch clip: glbclip <index|name> [node-name]
void cmd_glbclip(const char *args)
{
    char sel[64] = "", name[128] = "";
    int n = sscanf(args, "%63s %127s", sel, name);
    if (n < 1) { console_print("usage: glbclip <idx|имя> [имя-узла]"); return; }
    SceneNode *nd = anim_find(name);
    if (!nd) { console_print("glbclip: нет анимик-узла (спавн через glbanim/glb)"); return; }
    int idx = atoi(sel);
    if (!(sel[0] >= '0' && sel[0] <= '9') || idx > 99999) idx = glb_anim_clip_find(nd, sel);
    if (idx < 0) { console_print("glbclip: клип '%s' не найден", sel); return; }
    glb_anim_set_clip(nd, idx);
    console_print("glbclip: %s -> [%d] %s", nd->name ? nd->name : "?", idx,
                  glb_anim_clip_name(nd, idx) ? glb_anim_clip_name(nd, idx) : "?");
}

// Pause/resume: glbanim_pause [node-name]
void cmd_glbanim_pause(const char *args)
{
    char name[128] = "";
    sscanf(args, "%127s", name);
    SceneNode *nd = anim_find(name);
    if (!nd) { console_print("glbanim_pause: нет анимик-узла"); return; }
    glb_anim_set_paused(nd, 1);
    console_print("glbanim_pause: %s", nd->name ? nd->name : "?");
}
// Resume: glbanim_play [node-name]
void cmd_glbanim_play(const char *args)
{
    char name[128] = "";
    sscanf(args, "%127s", name);
    SceneNode *nd = anim_find(name);
    if (!nd) { console_print("glbanim_play: нет анимик-узла"); return; }
    glb_anim_set_paused(nd, 0);
    console_print("glbanim_play: %s", nd->name ? nd->name : "?");
}
// Time speed: glbanim_speed <x> [node-name]
void cmd_glbanim_speed(const char *args)
{
    float s = 1.0f; char name[128] = "";
    if (sscanf(args, "%f %127s", &s, name) < 1) { console_print("usage: glbanim_speed <x> [имя-узла]"); return; }
    SceneNode *nd = anim_find(name);
    if (!nd) { console_print("glbanim_speed: нет анимик-узла"); return; }
    glb_anim_set_speed(nd, s);
    console_print("glbanim_speed: %s = %g", nd->name ? nd->name : "?", s);
}
// Looping: glbanim_loop <0|1> [node-name]
void cmd_glbanim_loop(const char *args)
{
    int v = 1; char name[128] = "";
    if (sscanf(args, "%d %127s", &v, name) < 1) { console_print("usage: glbanim_loop <0|1> [имя-узла]"); return; }
    SceneNode *nd = anim_find(name);
    if (!nd) { console_print("glbanim_loop: нет анимик-узла"); return; }
    glb_anim_set_loop(nd, v);
    console_print("glbanim_loop: %s = %d", nd->name ? nd->name : "?", v?1:0);
}
// Set clip time in seconds: glbanim_seek <sec> [node-name]
void cmd_glbanim_seek(const char *args)
{
    float t = 0; char name[128] = "";
    if (sscanf(args, "%f %127s", &t, name) < 1) { console_print("usage: glbanim_seek <сек> [имя-узла]"); return; }
    SceneNode *nd = anim_find(name);
    if (!nd) { console_print("glbanim_seek: нет анимик-узла"); return; }
    glb_anim_set_time(nd, t);
    console_print("glbanim_seek: %s = %.3f с", nd->name ? nd->name : "?", t);
}
// Set a frame: glbanim_frame <frame> [fps] [node-name] (fps defaults to 24)
void cmd_glbanim_frame(const char *args)
{
    int frame = 0; float fps = 24.0f; char name[128] = "";
    int n = sscanf(args, "%d %f %127s", &frame, &fps, name);
    if (n < 1) { console_print("usage: glbanim_frame <кадр> [fps] [имя-узла]"); return; }
    SceneNode *nd = anim_find(name);
    if (!nd) { console_print("glbanim_frame: нет анимик-узла"); return; }
    glb_anim_set_time(nd, frame / (fps > 0 ? fps : 24.0f));
    console_print("glbanim_frame: %s = кадр %d @%.0f fps (%.3f с)",
                  nd->name ? nd->name : "?", frame, fps, glb_anim_time(nd));
}

// ---- anim tool: registry of all anim nodes, addressed by index ----------------
//   anim                    — list of all loaded models
//   anim <N>                — info about the model with index N
//   anim <N> pos x y z      — teleport
//   anim <N> move x y z [spd] — smooth move toward a point (spd=units/s, default 5)
//   anim <N> stop           — stop the smooth movement
//   anim <N> clip <name|#>  — switch animation
//   anim <N> play | pause   — play / pause
//   anim <N> loop <0|1>     — loop on / loop off
//   anim <N> speed <x>      — playback speed
//   anim <N> seek <sec>     — set clip time
//   anim <N> frame <n> [fps]— set a frame
//   anim <N> on | off       — show / hide (freeze)
//   anim <N> scale <s>      — model scale
//   anim <N> clips          — list of the model's clips
//   anim <N> remove         — unload the model
static void anim_show_state(SceneNode *nd)
{
    console_print("  pos(%.2f,%.2f,%.2f) scale=%.2g enabled=%d paused=%d loop=%d speed=%.2g time=%.3f клип=%s",
                  nd->pos[0], nd->pos[1], nd->pos[2],
                  glb_anim_scale_get(nd), glb_anim_enabled(nd),
                  glb_anim_paused_get(nd), glb_anim_loop_get(nd),
                  glb_anim_speed_get(nd), glb_anim_time(nd),
                  glb_anim_current_clip(nd) ? glb_anim_current_clip(nd) : "?");
}
void cmd_anim(const char *args)
{
    char a0[64] = "";
    if (!args) args = "";
    sscanf(args, "%63s", a0);
    if (a0[0] == 0 || strcmp(a0, "list") == 0 || strcmp(a0, "help") == 0 || strcmp(a0, "?") == 0 ||
        !(a0[0] >= '0' && a0[0] <= '9')) {
        int n = glb_anim_count();
        console_print("anim: моделей %d (спавн: glb/glbanim <файл> …); usage подсказка: anim help", n);
        for (int i = 0; i < n; i++) {
            SceneNode *nd = glb_anim_by_index(i);
            if (!nd) continue;
            console_print(" [%d] %s  pos(%.2f,%.2f,%.2f) %s%s клип=%s",
                          i, glb_anim_path(nd) ? glb_anim_path(nd) : "?",
                          nd->pos[0], nd->pos[1], nd->pos[2],
                          glb_anim_enabled(nd) ? "" : " [off] ",
                          glb_anim_loop_get(nd) ? "loop" : "",
                          glb_anim_current_clip(nd) ? glb_anim_current_clip(nd) : "?");
        }
        console_print("anim: usage: anim | anim <N> | anim <N> pos|move x y z [spd] | clip <имя|№> | play|pause | loop <0|1> | speed <x> | seek <сек> | frame <n> [fps] | on|off | scale <s> | clips | stop | remove");
        return;
    }
    int idx = atoi(a0);
    SceneNode *nd = glb_anim_by_index(idx);
    if (!nd) { console_print("anim: нет модели с индексом %d (см. anim)", idx); return; }
    char sub[24];
    int n = sscanf(args, "%*s %23s", sub);
    if (n < 1 || sub[0] == 0) { anim_show_state(nd); return; }

    float p[3], spd;
    if (strcmp(sub, "pos") == 0) {
        if (sscanf(args, "%*s %*s %f %f %f", &p[0], &p[1], &p[2]) == 3) {
            glb_anim_set_pos(nd, p);
            console_print("anim %d: позиция (%.2f,%.2f,%.2f)", idx, p[0], p[1], p[2]);
        } else console_print("usage: anim %d pos x y z", idx);
        return;
    }
    if (strcmp(sub, "move") == 0) {
        float tail[5]; int nr = sscanf(args, "%*s %*s %f %f %f %f", &p[0], &p[1], &p[2], &spd);
        if (nr < 3) { console_print("usage: anim %d move x y z [spd] (spd = ед/с)", idx); return; }
        if (nr < 4) spd = 5.0f;
        glb_anim_move_to(nd, p, spd);
        console_print("anim %d: движение к (%.2f,%.2f,%.2f) со скоростью %.1f", idx, p[0], p[1], p[2], spd);
        (void)tail;
        return;
    }
    if (strcmp(sub, "stop") == 0) { glb_anim_stop_move(nd); console_print("anim %d: движение остановлено", idx); return; }
    if (strcmp(sub, "clip") == 0) {
        char sel[64] = "";
        // Without a third token sscanf leaves sel untouched, and sel is then
        // used as a string ("%s" and clip_find) — an uninitialized read.
        if (sscanf(args, "%*s %*s %63s", sel) != 1) {
            console_print("usage: anim %d clip <имя|№>", idx);
            return;
        }
        int cidx = atoi(sel);
        if (!(sel[0] >= '0' && sel[0] <= '9')) cidx = glb_anim_clip_find(nd, sel);
        if (cidx < 0) { console_print("anim %d: клип '%s' не найден", idx, sel); return; }
        glb_anim_set_clip(nd, cidx);
        console_print("anim %d: клип -> %s", idx, glb_anim_clip_name(nd, cidx) ? glb_anim_clip_name(nd, cidx) : "?");
        return;
    }
    if (strcmp(sub, "play") == 0) { glb_anim_set_paused(nd, 0); console_print("anim %d: играет", idx); return; }
    if (strcmp(sub, "pause") == 0) { glb_anim_set_paused(nd, 1); console_print("anim %d: пауза", idx); return; }
    if (strcmp(sub, "loop") == 0) {
        int v; if (sscanf(args, "%*s %*s %d", &v) != 1) { console_print("usage: anim %d loop <0|1>", idx); return; }
        glb_anim_set_loop(nd, v); console_print("anim %d: loop=%d", idx, v ? 1 : 0); return;
    }
    if (strcmp(sub, "speed") == 0) {
        float v; if (sscanf(args, "%*s %*s %f", &v) != 1) { console_print("usage: anim %d speed <x>", idx); return; }
        glb_anim_set_speed(nd, v); console_print("anim %d: speed=%.2g", idx, v); return;
    }
    if (strcmp(sub, "seek") == 0) {
        float v; if (sscanf(args, "%*s %*s %f", &v) != 1) { console_print("usage: anim %d seek <сек>", idx); return; }
        glb_anim_set_time(nd, v); console_print("anim %d: время %.3f с", idx, v); return;
    }
    if (strcmp(sub, "frame") == 0) {
        int fr = 0; float fps = 24;
        // "anim 0 frame" with no number left fr uninitialized and the model was
        // seeked to a garbage time.
        if (sscanf(args, "%*s %*s %d %f", &fr, &fps) < 1) {
            console_print("usage: anim %d frame <n> [fps]", idx);
            return;
        }
        glb_anim_set_time(nd, fr / (fps > 0 ? fps : 24.0f));
        console_print("anim %d: кадр %d @%.0f fps (%.3f с)", idx, fr, fps, glb_anim_time(nd)); return;
    }
    if (strcmp(sub, "on") == 0 || strcmp(sub, "off") == 0) {
        int on = sub[0] == 'o' && sub[1] == 'n';
        glb_anim_set_enabled(nd, on);
        console_print("anim %d: %s", idx, on ? "включён" : "выключен"); return;
    }
    if (strcmp(sub, "scale") == 0) {
        float v; if (sscanf(args, "%*s %*s %f", &v) != 1) { console_print("usage: anim %d scale <s>", idx); return; }
        glb_anim_set_scale(nd, v); console_print("anim %d: scale=%.2g", idx, v); return;
    }
    if (strcmp(sub, "clips") == 0) {
        int nc = glb_anim_clip_count(nd);
        console_print("anim %d: клипов %d, активный: %s", idx, nc,
                      glb_anim_current_clip(nd) ? glb_anim_current_clip(nd) : "?");
        for (int i = 0; i < nc; i++)
            console_print("  [%d] %s", i, glb_anim_clip_name(nd, i) ? glb_anim_clip_name(nd, i) : "?");
        return;
    }
    if (strcmp(sub, "remove") == 0) {
        console_print("anim %d: выгружаю %s", idx, glb_anim_path(nd) ? glb_anim_path(nd) : "?");
        glb_anim_remove_node(nd);
        return;
    }
    if (strcmp(sub, "rot") == 0) {
        // Rotation in DEGREES (yaw pitch roll) — the node stores radians.
        if (sscanf(args, "%*s %*s %f %f %f", &p[0], &p[1], &p[2]) >= 1) {
            nd->rot[1] = p[0] * 0.0174532925f;
            nd->rot[0] = p[1] * 0.0174532925f;
            nd->rot[2] = p[2] * 0.0174532925f;
            console_print("anim %d: поворот yaw=%.1f pitch=%.1f roll=%.1f",
                          idx, p[0], p[1], p[2]);
        } else console_print("usage: anim %d rot <yaw> [pitch] [roll] (градусы)", idx);
        return;
    }
    if (strcmp(sub, "bones") == 0) {
        int bn = glb_anim_bone_count(nd);
        console_print("anim %d: узлов/костей %d (attach <имя> — прицепить)", idx, bn);
        for (int i = 0; i < bn; i++) {
            const char *bname = glb_anim_bone_name(nd, i);
            if (bname && bname[0]) console_print("  [%d] %s", i, bname);
        }
        return;
    }
    console_print("anim %d: неизвестная команда '%s' (см. anim help)", idx, sub);
}

// ---- attach: hook a node to a model/bone --------------------------------------
//   attach sprite <N> <bone> <png> [w h] [up]  — sprite on a bone of model N
//   attach glb    <N> <bone> <file.glb> [scale] — model on a bone of model N
//   attach mesh   <N> <bone> <file.obj> [scale] [tex]
//   attach list   <N>                            — what is attached
//   attach bones  <N>                            — bone list
void cmd_attach(const char *args)
{
    Game *g = game_instance();
    if (!g || !g->root) { console_print("attach: мир не загружен"); return; }
    char what[24] = "", bone[64] = "", file[256] = "";
    int idx = -1;
    if (!args) args = "";
    if (sscanf(args, "%23s", what) != 1 || !what[0]) {
        console_print("usage: attach sprite|glb|mesh <N> <кость> <файл> [...]");
        console_print("       attach list <N> | attach bones <N>");
        console_print("  <N> — индекс модели из `anim`, <кость> — имя (или - для origin)");
        return;
    }
    if (sscanf(args, "%*s %d", &idx) != 1) {
        console_print("attach: укажи индекс модели (см. anim)");
        return;
    }
    SceneNode *parent = glb_anim_by_index(idx);
    if (!parent) { console_print("attach: нет модели с индексом %d", idx); return; }

    if (strcmp(what, "bones") == 0) {
        int bn = glb_anim_bone_count(parent);
        console_print("attach: у модели %d узлов/костей %d", idx, bn);
        for (int i = 0; i < bn; i++) {
            const char *bname = glb_anim_bone_name(parent, i);
            if (bname && bname[0]) console_print("  %s", bname);
        }
        return;
    }
    if (strcmp(what, "list") == 0) {
        int n = 0;
        for (SceneNode *c = parent->first_child; c; c = c->next, n++) {
            const char *bname = c->attach_slot >= 0
                              ? glb_anim_bone_name(parent, c->attach_slot) : NULL;
            console_print("  [%d] %s (%s) -> %s  off(%.2f,%.2f,%.2f)", n,
                          c->name ? c->name : "?", c->type ? c->type : "?",
                          bname ? bname : "origin",
                          c->pos[0], c->pos[1], c->pos[2]);
        }
        if (!n) console_print("attach: у модели %d ничего не прицеплено", idx);
        return;
    }

    if (sscanf(args, "%*s %*d %63s %255s", bone, file) != 2) {
        console_print("usage: attach %s <N> <кость|-> <файл> [...]", what);
        return;
    }
    const char *bone_arg = (bone[0] == '-' && bone[1] == 0) ? NULL : bone;

    SceneNode *child = NULL;
    int upright = 0;
    if (strcmp(what, "sprite") == 0) {
        float w = 0.5f, h = 0.5f;
        int up = 0;
        sscanf(args, "%*s %*d %*s %*s %f %f %d", &w, &h, &up);
        upright = up;
        child = sprite_make_node(file, w, h, 1, renderer_context());
        if (child) child->name = "Attached Sprite";
    } else if (strcmp(what, "glb") == 0) {
        float scale = 1.0f;
        sscanf(args, "%*s %*d %*s %*s %f", &scale);
        child = glb_anim_has_anim(file)
              ? glb_anim_make_node(file, NULL, scale, renderer_context())
              : glb_make_node(file, NULL, scale, renderer_context());
    } else if (strcmp(what, "mesh") == 0) {
        float scale = 1.0f;
        char tex[256] = "";
        int nn = sscanf(args, "%*s %*d %*s %*s %f %255s", &scale, tex);
        child = mesh_make_node(file, NULL, scale, nn >= 2 && tex[0] ? tex : NULL,
                               renderer_context());
    } else {
        console_print("attach: неизвестный тип '%s' (sprite|glb|mesh)", what);
        return;
    }
    if (!child) { console_print("attach: не удалось создать %s из %s", what, file); return; }
    int rc = attach_node(parent, child, bone_arg, NULL, upright);
    console_print("attach: %s '%s' -> модель %d, кость %s%s", what, file, idx,
                  bone_arg ? bone_arg : "origin",
                  rc == 0 ? "" : " (кость не найдена, прицеплено к origin)");
}

// ---- sprite: place a free sprite into the world --------------------------------
//   sprite <png> [x y z] [w h] [flat]
void cmd_sprite(const char *args)
{
    Game *g = game_instance();
    if (!g || !g->root) { console_print("sprite: мир не загружен"); return; }
    char path[256] = "";
    float x = 0, y = 0, z = 0, w = 1.0f, h = 1.0f;
    char flag[16] = "";
    int n = sscanf(args, "%255s %f %f %f %f %f %15s", path, &x, &y, &z, &w, &h, flag);
    if (n < 1) {
        console_print("usage: sprite <png> [x y z] [w h] [flat]");
        return;
    }
    int billboard = strcmp(flag, "flat") != 0;
    SceneNode *nd = sprite_make_node(path, w, h, billboard, renderer_context());
    if (!nd) { console_print("sprite: не удалось загрузить %s", path); return; }
    nd->pos[0] = x; nd->pos[1] = y; nd->pos[2] = z;
    node_add(g->root, nd);
    console_print("sprite: %s в (%.1f,%.1f,%.1f) %.2fx%.2f м%s", path, x, y, z,
                  w, h, billboard ? " (биллборд)" : " (плоский)");
}

// cubemap_capture [x y z] [size] — capture the reflection cubemap at the
// player position (or at point x y z) and save 6 PNGs into cubemaps/. Default
// size is 256; the captured cubemap is immediately used by the water.
void cmd_cubemap_capture(const char *args)
{
    Game *g = game_instance();
    if (!g || !g->cam || !g->world_loaded) {
        console_print("cubemap_capture: мир не загружен");
        return;
    }
    if (!renderer_refl_cubemap()) {
        console_print("cubemap_capture: кубомап недоступен");
        return;
    }
    // Position: the player by default, or from the arguments "x y z [size]"
    float pos[3] = {
        g->cam->cam.pos[0], g->cam->cam.pos[1], g->cam->cam.pos[2]
    };
    int size = 256;
    if (args && args[0]) {
        float x, y, z;
        int n = sscanf(args, "%f %f %f %d", &x, &y, &z, &size);
        if (n == 1) size = (int)x;       // "cubemap_capture 128" — size only
        else if (n >= 3) { pos[0] = x; pos[1] = y; pos[2] = z; }
    }
    renderer_cubemap_capture_at(g, pos, size);
    console_print("cubemap_capture: (%.0f, %.0f, %.0f) size=%d сохранён в cubemaps/",
                  pos[0], pos[1], pos[2], size);
}

// List of registered materials by name (matlist).
void cmd_matlist(const char *args)
{
    (void)args;
    if (material_count() == 0) {
        SDL_Log("matlist: no materials registered");
        return;
    }
    for (int i = 0; i < material_count(); i++) {
        const Material *m = material_find(material_name(i));
        SDL_Log("material %-24s tint=(%.2f,%.2f,%.2f) alpha=%.2f blend=%d tile=(%.2f,%.2f)",
                material_name(i),
                m->tint[0], m->tint[1], m->tint[2], m->alpha, m->blend,
                m->tile[0], m->tile[1]);
    }
}

// --- Health (HP) ----------------------------------------------------------------
void cmd_hp(const char *args)
{
    (void)args;
    Game *g = game_instance();
    if (!g) return;
    const Health *h = &g->player.hp;
    console_print("HP %.0f/%.0f  реген %.0f/с  god %s%s",
                  h->hp, h->max, h->regen, h->god ? "ON" : "OFF",
                  h->dead ? "  [МЁРТВ]" : "");
}

void cmd_hurt(const char *args)
{
    Game *g = game_instance();
    if (!g) return;
    float amt = 25.0f;
    if (args && args[0]) sscanf(args, "%f", &amt);
    float took = health_damage(&g->player.hp, amt);
    console_print("hurt %.0f -> HP %.0f", took, g->player.hp.hp);
}

void cmd_heal(const char *args)
{
    Game *g = game_instance();
    if (!g) return;
    float amt = g->player.hp.max;   // no argument — heal to full
    if (args && args[0]) sscanf(args, "%f", &amt);
    health_heal(&g->player.hp, amt);
    console_print("heal -> HP %.0f", g->player.hp.hp);
}

void cmd_god(const char *args)
{
    Game *g = game_instance();
    if (!g) return;
    if (cvar_get("sv_cheats") < 0.5f) {
        console_print("god: требуется sv_cheats 1");
        return;
    }
    int on = -1;
    if (args && args[0]) { int v; if (sscanf(args, "%d", &v) == 1) on = v ? 1 : 0; }
    g->player.hp.god = (on < 0) ? !g->player.hp.god : on;
    console_print("god: %s", g->player.hp.god ? "ON" : "OFF");
}

void cmd_kill(const char *args)
{
    (void)args;
    Game *g = game_instance();
    if (!g) return;
    health_kill(&g->player.hp);
}

void cmd_sethp(const char *args)
{
    Game *g = game_instance();
    if (!g) return;
    if (cvar_get("sv_cheats") < 0.5f) {
        console_print("sethp: требуется sv_cheats 1");
        return;
    }
    float v = 0;
    if (!args || sscanf(args, "%f", &v) != 1) {
        console_print("usage: sethp <значение>");
        return;
    }
    Health *h = &g->player.hp;
    if (h->dead && v > 0.0f) health_revive(h);
    h->hp = v < 0.0f ? 0.0f : (v > h->max ? h->max : v);
    console_print("HP = %.0f", h->hp);
}

void cmd_maxhp(const char *args)
{
    Game *g = game_instance();
    if (!g) return;
    float v = 0;
    if (!args || sscanf(args, "%f", &v) != 1 || v <= 0.0f) {
        console_print("usage: maxhp <значение>");
        return;
    }
    health_set_max(&g->player.hp, v, 1);
    console_print("max HP = %.0f (долит до полного)", g->player.hp.max);
}
