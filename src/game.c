// ============================================================================
//  game.c — game state, world load/unload and update (see game.h)
// ----------------------------------------------------------------------------
//  All world state lives in a single Game struct (scene nodes, camera/HUD,
//  map, spawn/save, demo, WorldState data). Modules receive Game* as a
//  parameter; game_instance() serves functions that take no parameter
//  (console commands).
//
//  World loading is split into EXPLICIT PHASES (the order is fixed):
//     1. world_load_geometry — map pak, map.json, heights/normals, field;
//     2. scene_build          — shadows, node tree, entities, FBO, first frame;
//     3. map_apply            — the map's cvars and commands;
//     4. audio_load_map       — music, ambient/wind, positional sounds.
//  Unloading is a STRICT INVERSE: audio_stop_map -> scene_teardown ->
//  world_unload_geometry (each step frees what was created last).
// ============================================================================
#include "game.h"

#include "actions.h"
#include "app.h"
#include "audio.h"
#include "console.h"
#include "cvars.h"
#include "decals.h"
#include "ecs.h"
#include "ecs_scene.h"
#include "entity.h"
#include "fauna.h"
#include "field.h"
#include "fx.h"
#include "lua_engine.h"
#include "loading.h"
#include "mapdata.h"
#include "material.h"
#include "movement.h"
#include "objects.h"
#include "particles.h"
#include "playerbox.h"
#include "renderer.h"
#include "resources.h"
#include "selection.h"
#include "shadow.h"
#include "sky.h"
#include "splat.h"
#include "sun.h"
#include "terrain.h"
#include "vegetation.h"
#include "voxfog.h"
#include "water.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Single instance: main.c creates the Game on the stack and registers it via
// game_init; game_instance() hands it out to commands and the like.
static Game *s_game = NULL;

// Phase functions are defined further down in this same file; forward
// declarations for calls from scene_build (partial rollback) and game_load_world.
static void scene_teardown(Game *g);
static void world_unload_geometry(Game *g);

void game_init(Game *g)
{
    s_game = g;
    memset(g, 0, sizeof *g);
    snprintf(g->map_name, sizeof g->map_name, "%s", "map1");
    player_init(&g->player);
}

Game *game_instance(void)
{
    return s_game;
}

static int map_is_zip(const Game *g)
{
    return strcmp(g->map_name, "map") != 0;
}

// --- Demo scene (key Q): automatic flythrough with FPS measurement ------------
#define DEMO_DUR 116.0f

// The tree for the orbit (tree1 from map.json) and the ellipse around the field
static const float DEMO_TREE[2] = { 880.0f, 380.0f };
#define DEMO_FCX 1000.0f
#define DEMO_FCZ 562.0f
#define DEMO_A 280.0f     // semi-axes of the ellipse around the field (outside its edge)
#define DEMO_B 190.0f

#define DEMO_H(w, x, z) sample_height(w, (x), (z))

// Demo step: time t from the start, drive the camera through phases at player height
static void demo_step(const WorldState *w, float t, CameraParams *cam)
{
    float hx = 0, hz = 0;                    // position
    float gx, gy, gz;                        // gaze point (at ground height)

    if (t < 8.0f) {
        hx = 1000.0f;
        hz = 408.0f + 2.5f * t;
        gx = 1000.0f; gz = hz + 30.0f;
        gy = DEMO_H(w, gx, gz);
    } else if (t < 12.0f) {
        float k = (t - 8.0f) / 4.0f;
        k = k * k * (3.0f - 2.0f * k);
        float d = 20.0f + (2.5f + (12.0f - 2.5f) * k) * (t - 8.0f);
        hx = 1000.0f;
        hz = 408.0f + d;
        gx = 1000.0f; gz = hz + 40.0f;
        gy = DEMO_H(w, gx, gz);
    } else if (t < 20.0f) {
        float k = (t - 12.0f) / 8.0f;
        k = k * k * (3.0f - 2.0f * k);
        float d = 58.0f + (12.0f + (22.0f - 12.0f) * k) * (t - 12.0f);
        hx = 1000.0f;
        hz = 408.0f + d;
        gx = 1000.0f; gz = hz + 50.0f;
        gy = DEMO_H(w, gx, gz);
    } else if (t < 35.0f) {
        float k = (t - 20.0f) / 15.0f;
        float p0x = 1000.0f, p0z = 408.0f + 58.0f + 176.0f;
        float p2x = DEMO_TREE[0], p2z = DEMO_TREE[1];
        float c0x = 930.0f, c0z = 480.0f;
        float ik = 1.0f - k;
        hx = ik * ik * p0x + 2.0f * k * ik * c0x + k * k * p2x;
        hz = ik * ik * p0z + 2.0f * k * ik * c0z + k * k * p2z;
        gx = DEMO_TREE[0]; gz = DEMO_TREE[1];
        gy = DEMO_H(w, DEMO_TREE[0], DEMO_TREE[1]) + 6.0f;
    } else if (t < 55.0f) {
        float th = 1.5708f + 0.44f * (t - 35.0f);
        hx = DEMO_TREE[0] + cosf(th) * 12.0f;
        hz = DEMO_TREE[1] + sinf(th) * 12.0f;
        gx = DEMO_TREE[0]; gz = DEMO_TREE[1];
        gy = DEMO_H(w, DEMO_TREE[0], DEMO_TREE[1]) + 6.0f;
    } else if (t < 63.0f) {
        float k = (t - 55.0f) / 8.0f;
        if (k > 1.0f) k = 1.0f;
        k = k * k * (3.0f - 2.0f * k);
        float th = 1.5708f + 0.44f * 20.0f;
        float ox = DEMO_TREE[0] + cosf(th) * 12.0f;
        float oz = DEMO_TREE[1] + sinf(th) * 12.0f;
        float ea = atan2f(DEMO_TREE[0] - DEMO_FCX, DEMO_TREE[1] - DEMO_FCZ);
        float ex = DEMO_FCX + sinf(ea) * DEMO_A;
        float ez = DEMO_FCZ + cosf(ea) * DEMO_B;
        hx = ox + (ex - ox) * k;
        hz = oz + (ez - oz) * k;
        gx = DEMO_FCX; gz = DEMO_FCZ;
        gy = DEMO_H(w, gx, gz);
    } else if (t < 110.0f) {
        float k = (t - 63.0f) / 47.0f;
        float ea = atan2f(DEMO_TREE[0] - DEMO_FCX, DEMO_TREE[1] - DEMO_FCZ);
        float th = ea + 6.28318f * k;
        hx = DEMO_FCX + sinf(th) * DEMO_A;
        hz = DEMO_FCZ + cosf(th) * DEMO_B;
        gx = DEMO_FCX; gz = DEMO_FCZ;
        gy = DEMO_H(w, gx, gz);
    } else {
        float k = (t - 110.0f) / 6.0f;
        if (k > 1.0f) k = 1.0f;
        k = k * k * (3.0f - 2.0f * k);
        float ea = atan2f(DEMO_TREE[0] - DEMO_FCX, DEMO_TREE[1] - DEMO_FCZ);
        float ox = DEMO_FCX + sinf(ea + 6.28318f) * DEMO_A;
        float oz = DEMO_FCZ + cosf(ea + 6.28318f) * DEMO_B;
        hx = ox + (1000.0f - ox) * k;
        hz = oz + (562.0f - oz) * k;
        gx = 1000.0f; gz = 562.0f;
        gy = DEMO_H(w, gx, gz);
    }

    cam->cam.pos[0] = hx;
    cam->cam.pos[1] = DEMO_H(w, hx, hz) + EYE_H;
    cam->cam.pos[2] = hz;
    float dx = gx - hx, dy = gy - cam->cam.pos[1], dz = gz - hz;
    float hl = sqrtf(dx * dx + dz * dz);
    cam->cam.yaw = atan2f(dx, dz);
    cam->cam.pitch = atan2f(dy, hl);
}

// --- Player teleport (spawn, setpos, load, teleport triggers) ------------------
void game_apply_spawn(Game *g, const float pos[3], float yaw, float pitch)
{
    if (!g->cam) return;
    player_teleport(&g->player, &g->cam->cam, pos, yaw, pitch);
}

// --- Demo scene (key Q) --------------------------------------------------------
void game_demo_toggle(Game *g)
{
    if (!g->world_loaded || app_state() != APP_PLAY || !g->cam || !g->hud) return;
    g->demo_on = !g->demo_on;
    camera_demo_set(g->demo_on);
    if (g->demo_on) {
        g->demo_t = 0.0f;
        g->demo_frames = 0;
        g->demo_elapsed = 0;
        g->demo_min_dt = 1e9;
        g->demo_max_dt = 0;
        hud_demo_start(g->hud);
        SDL_Log("DEMO: start (autofly, FPS measurement)");
    } else {
        hud_demo_stop(g->hud);
        SDL_Log("DEMO: stopped");
    }
}

void game_demo_stop(Game *g)
{
    if (!g->demo_on) return;
    g->demo_on = 0;
    camera_demo_set(0);
    if (g->hud) hud_demo_stop(g->hud);
}

// Demo scene step: drive the camera along the route and measure FPS.
void game_demo_tick(Game *g, float dt, float inst_fps)
{
    if (!g->demo_on || !g->world_loaded || app_state() != APP_PLAY) return;
    g->demo_t += dt;
    if (g->demo_t >= DEMO_DUR) {
        double avg = g->demo_elapsed > 0.0 ? g->demo_frames / g->demo_elapsed : 0.0;
        SDL_Log("DEMO: end | frames=%.0f time=%.1fs | avg=%.0f min=%.0f max=%.0f fps",
                g->demo_frames, g->demo_elapsed, avg,
                g->demo_max_dt > 0.0 ? 1.0 / g->demo_max_dt : 0.0,
                g->demo_min_dt < 1e9 ? 1.0 / g->demo_min_dt : 0.0);
        g->demo_on = 0;
        camera_demo_set(0);
        hud_demo_stop(g->hud);
    } else {
        demo_step(&g->world, g->demo_t, g->cam);
        g->demo_frames += 1;
        g->demo_elapsed += dt;
        if (dt > 0.0005f && dt < 0.1f) {
            if (dt < g->demo_min_dt) g->demo_min_dt = dt;
            if (dt > g->demo_max_dt) g->demo_max_dt = dt;
        }
        hud_demo_frame(g->hud, inst_fps, dt);
    }
}

// --- Map interaction and triggers ---------------------------------------------
// Runs a map action: commands, message, sound (positional from pos), teleport.
// IMPORTANT: console_exec may unload the world (the "map X" command) — after it
// you MUST NOT touch g->cam (it was freed in scene_teardown). Therefore the
// position is passed by copy, and camera access happens only while the world is alive.
static void map_action_run(Game *g, const char commands[MAP_ACTIONS_MAX][MAP_CMD_LEN],
                           int cmd_n, const char *message, const char *sound,
                           const float pos[3], const float *tp, int has_tp)
{
    for (int i = 0; i < cmd_n; i++) {
        console_exec(commands[i]);
        if (!g->world_loaded) return;   // the command unloaded the world — action aborted
    }
    if (message && message[0]) {
        snprintf(g->ui_msg, sizeof g->ui_msg, "%s", message);
        g->ui_msg_t = 4.0f;
    }
    if (sound && sound[0] && pos) {
        Sound *s = audio_load(sound);
        if (s)
            audio_3d_play_once(s, 0.4f * cvar_get("snd_dialog"), pos, 30.0f);
    }
    if (has_tp && tp && g->cam)
        game_apply_spawn(g, tp, g->cam->cam.yaw, g->cam->cam.pitch);
}

// Trigger and interaction (E) checks — every frame while in game
static void map_actions_update(Game *g)
{
    static int s_use_prev = 0;
    const int use_now = g_act[A_USE];
    const int use_press = use_now && !s_use_prev;
    s_use_prev = use_now;

    if (!g->cam) return;
    // Position is passed BY COPY: a trigger's console command may unload the
    // world (the "map ..." command), leaving the camera pointer dangling
    // (use-after-free).
    float px = g->cam->cam.pos[0];
    float pz = g->cam->cam.pos[2];

    // --- Triggers: entered the area -> action ---
    for (int i = 0; i < mapdata_trigger_count(); i++) {
        const MapTrigger *t = mapdata_trigger(i);
        if (!t) continue;
        int inside = fabsf(px - t->pos[0]) <= t->size[0] * 0.5f &&
                     fabsf(pz - t->pos[2]) <= t->size[1] * 0.5f;
        int edge = inside && !t->entered;
        MapTrigger *mt = (MapTrigger *)t;
        mt->entered = inside;
        if (!edge) continue;
        if (t->once && t->fired) continue;
        if (t->once) mt->fired = 1;
        SDL_Log("Trigger: %s", t->name[0] ? t->name : "(unnamed)");
        map_action_run(g, t->commands, t->cmd_n, t->message, t->sound,
                       t->pos, t->teleport, t->has_tp);
        if (!g->world_loaded) return;   // the world changed — the new map's
                                        // triggers are handled next frame
    }

    // --- E interaction: nearest object with use within a 4.5 m radius ---
    if (!use_press) return;
    const MapObject *best = NULL;
    float best_d = 4.5f * 4.5f;
    for (int i = 0; i < mapdata_object_count(); i++) {
        const MapObject *o = mapdata_object(i);
        if (!o || !o->use.set || o->use.used) continue;
        float dx = px - o->pos[0];
        float dz = pz - o->pos[2];
        float d = dx * dx + dz * dz;
        if (d < best_d) { best_d = d; best = o; }
    }
    if (!best) return;
    MapObject *mo = (MapObject *)best;
    if (best->use.once) mo->use.used = 1;
    SDL_Log("Use: %s", best->name[0] ? best->name : "(object)");
    map_action_run(g, best->use.commands, best->use.cmd_n,
                   best->use.message, best->use.sound, best->pos, NULL, 0);
}

// --- World load phases ---------------------------------------------------------
// Fixed order: world_load_geometry -> scene_build -> map_apply ->
// audio_load_map. Unloading is the strict inverse (see game_unload_world).

//  Phase 1. World geometry: the map pak, map.json, heights/normals, wheat field.
static int world_load_geometry(Game *g, WorldTextures *wt)
{
    WorldState *w = &g->world;

    // Map: a zip from maps/ (working folder or one level up) or the map/ folder
    if (map_is_zip(g)) {
        char path[128];
        snprintf(path, sizeof path, "maps/%s.zip", g->map_name);
        if (!resources_pak_open(path)) {
            snprintf(path, sizeof path, "../maps/%s.zip", g->map_name);
            if (!resources_pak_open(path)) return -1;   // pak not opened — nothing to clean
        }
    }
    // Everything about the map sits at its root: map/map.json or map.json (inside the zip)
    if (mapdata_load(map_is_zip(g) ? "map.json" : "map/map.json") != 0) {
        mapdata_free();            // partially loaded json — remove it
        resources_pak_close();     // ~ resources_pak_open
        return -1;
    }
    if (world_load(w, wt) != 0) {
        mapdata_free();            // phase 1 rolls back entirely
        world_free(w);             // partial heights/textures
        resources_pak_close();
        return -1;
    }

    // Wheat field: a world rectangle shared by terrain, vegetation
    // and the FieldArea node (frame). Coordinates come from map/map.json.
    g->field_pos[0]  = w->world_w / 2;
    g->field_pos[1]  = 0;
    g->field_pos[2]  = w->world_d / 2;
    g->field_size[0] = 500;
    g->field_size[1] = 0;
    g->field_size[2] = 300;
    {
        float c[2], s[2];
        if (mapdata_field(c, s)) {
            g->field_pos[0] = c[0];
            g->field_pos[2] = c[1];
            g->field_size[0] = s[0];
            g->field_size[2] = s[1];
        }
    }
    w->field_x0 = g->field_pos[0] - g->field_size[0] * 0.5f;
    w->field_z0 = g->field_pos[2] - g->field_size[2] * 0.5f;
    w->field_w = g->field_size[0];
    w->field_d = g->field_size[2];
    if (mapdata_field_attached()) {
        w->field_x0 = 0.0f;
        w->field_z0 = 0.0f;
        w->field_w = w->world_w;
        w->field_d = w->world_d;
    }
    // Terrain surfaces: splat mask of materials (soil/sand/gravel/asphalt),
    // roads and the occupancy map (where grass does not grow). Heights under
    // roads are already flattened inside world_load (splat_flatten_roads).
    splat_build(w);
    return 0;
}

// Attaches all created scene nodes to the root (skips the not-yet-created ones).
static void scene_attach_nodes(Game *g)
{
    if (!g->root) return;
#define FR_ATTACH(n) if (g->n) node_add(g->root, g->n)
    FR_ATTACH(sky_node);
    FR_ATTACH(sun_node);
    FR_ATTACH(terr_node);
    FR_ATTACH(wheat_node);
    FR_ATTACH(obj_node);
    FR_ATTACH(fauna_node);
    FR_ATTACH(particles_node);
    FR_ATTACH(decals_node);
    FR_ATTACH(water_node);
    FR_ATTACH(field_node);
    FR_ATTACH(sel_node);
    FR_ATTACH(cam_node);
    FR_ATTACH(playerbox_node);
    FR_ATTACH(mm_node);
    FR_ATTACH(hud_node);
#undef FR_ATTACH
}

// Phase 2. Scene: shadows, node tree, entities, render FBO, first frame.
static int scene_build(Game *g, const WorldTextures *wt)
{
    WorldState *w = &g->world;

    shadow_init();                     // cascaded shadow maps

    g->root = node_new("World", "Node3D", NULL, NULL, NULL, NULL, NULL);

    g->sun_node = sun_make_node();
    SceneNode *sun_node = g->sun_node;

    // Camera: player start from map.json (player/pos, yaw, pitch, fov)
    float start_pos[3] = {
        w->world_w / 2,
        height_curve(w, 1.0f - world_avg_lum()) + 6.0f,
        w->world_d / 2
    };
    float start_yaw = 3.14159f, start_pitch = -0.5f, start_fov = 60.0f;
    {
        float pp[3], pyaw, ppitch, pfov = 60.0f;
        if (mapdata_player(pp, &pyaw, &ppitch, &pfov)) {
            start_pos[0] = pp[0];
            start_pos[1] = pp[1];
            start_pos[2] = pp[2];
            start_yaw = pyaw;
            start_pitch = ppitch;
            start_fov = pfov;
            if (start_pos[1] < 1.0f)
                start_pos[1] = sample_height(w, start_pos[0], start_pos[2]) + EYE_H;
        }
    }
    g->cam_node = camera_make_node(w, start_pos, start_yaw, start_pitch,
                                   start_fov, 0.1f, w->world_w * 1.5f);
    if (!sun_node || !g->cam_node) {
        // Clean up after ourselves: created nodes + root + shadows (scene not built).
        scene_attach_nodes(g);
        scene_teardown(g);
        return -1;
    }
    g->cam = g->cam_node->params;

    // fov from map.json acts as the default value, then config.cfg overrides it
    cvar_set("fov", g->cam->fov_deg);
    console_autoload();

        // Spawn: the player's start point/gaze (for "New game")
    player_set_spawn(&g->player, g->cam->cam.pos, g->cam->cam.yaw,
                     g->cam->cam.pitch);
    health_revive(&g->player.hp);   // new/reloaded world — full HP

    // Scene nodes receive the unified render context (renderer.c) instead of cam/sun
    const RendererContext *rc = renderer_context();
    g->sky_node = sky_make_node(rc);
    g->terr_node = terrain_make_node(w, wt, rc);
    g->wheat_node = veg_make_node(w, rc);
    g->obj_node = objects_make_node(w, rc);
    g->field_node = field_make_node(w, g->field_pos, g->field_size, rc);
    g->sel_node = selection_make_node(g->root, rc);
    g->playerbox_node = playerbox_make_node(g->cam, rc);
    g->mm_node = minimap_make_node(w, wt->tex_base, rc);
    g->hud_node = hud_make_node(w, rc);
    g->fauna_node = fauna_make_node(rc);
    g->particles_node = particles_make_node(w, rc);
    g->decals_node = decals_make_node(w, rc);
    g->water_node = water_make_node(w, wt, rc);
    if (!g->sky_node || !g->terr_node || !g->wheat_node || !g->field_node ||
        !g->sel_node || !g->mm_node || !g->hud_node) {
        SDL_Log("Failed to create a scene node (missing texture/shader?)");
        // The scene was not fully built: free the created nodes and shadows.
        scene_attach_nodes(g);
        scene_teardown(g);
        return -1;
    }
    g->mm = g->mm_node->params;
    g->hud = g->hud_node->params;
    selection_set_on(0);             // selection outlines — only via H (debug)

    scene_attach_nodes(g);

    // Entities: classes, world, player, map objects
    {
        static const EntityClass cls_world = { "world", NULL, NULL, NULL };
        static const EntityClass cls_player = { "player", NULL, NULL, NULL };
        static const EntityClass cls_prop = { "prop_static", NULL, NULL, NULL };
        entity_set_root(g->root);
        entity_register_class(&cls_world);
        entity_register_class(&cls_player);
        entity_register_class(&cls_prop);
        entity_bind("world", g->root, NULL);
        entity_bind("player", g->cam_node, NULL);
        for (int i = 0; i < mapdata_object_count(); i++) {
            const MapObject *o = mapdata_object(i);
            entity_spawn(o->class[0] ? o->class : "prop_static",
                         o->pos, NULL, NULL);
        }
    }

    // Window size in pixels — what OpenGL sees (glViewport)
    int vw = renderer_vw(), vh = renderer_vh();
    g->cam->aspect = (float)vw / vh;
    g->mm->vw = vw;
    g->mm->vh = vh;
    g->hud->vw = vw;
    g->hud->vh = vh;

    // Bloom and voxel fog — window-sized FBOs (renderer)
    renderer_setup(w, vw, vh);

    // First scene frame (background ready, camera computed)
    node_update_tree(g->root, 1.0f / 60.0f);
    node_update_transforms(g->root);   // world matrices before the first render
    entity_update_all(1.0f / 60.0f);
    voxfog_update(g->cam->cam.pos);

    g->world_loaded = 1;
    return 0;
}

// Phase 3. Map application: cvars and commands (after the scene is built).
static void map_apply(void)
{
    // map cvars (cvars: { name: value }) — override config.cfg
    for (int i = 0; i < mapdata_cvar_count(); i++) {
        const char *name = mapdata_cvar_name(i);
        if (!name) continue;
        cvar_set(name, mapdata_cvar_value(i));
        SDL_Log("Map cvar: %s = %g", name, mapdata_cvar_value(i));
    }
    // map commands (commands: [ ... ]) — console commands
    for (int i = 0; i < mapdata_command_count(); i++) {
        const char *cmd = mapdata_command(i);
        if (!cmd) continue;
        SDL_Log("Map command: %s", cmd);
        console_exec(cmd);
    }
}

// Phase 4. Map audio: music, ambient/wind, positional sources.
// (a map may ship its own wav files in the zip — the resource manager pulls them in)
static void audio_load_map(Game *g)
{
    MapSound ms;
    audio_loop_stop();
    audio_music_stop();
    if (mapdata_music(&ms)) {
        g->music = audio_load(ms.sound);
        if (g->music) audio_music_start(g->music, ms.volume);
        else SDL_Log("Map music: missing %s", ms.sound);
    }
    if (mapdata_ambient(&ms)) {
        g->ambient = audio_load(ms.sound);
        if (g->ambient) audio_loop_start(g->ambient, ms.volume);
        else SDL_Log("Map ambient: missing %s", ms.sound);
    } else {
        if (!g->wind) g->wind = audio_load("audio/wind.wav");
        if (g->wind) audio_loop_start(g->wind, 0.22f);
    }
    // Map positional sounds (sounds[]): volume depends on distance to the player
    for (int i = 0; i < mapdata_sound_count(); i++) {
        const MapSound3D *m3 = mapdata_sound(i);
        if (!m3) continue;
        Sound *s = audio_load(m3->sound);
        if (s) {
            audio_3d_play(s, m3->volume, m3->pos, m3->radius);
            SDL_Log("Map sound: %s at %.0f,%.0f (r=%.0fm)", m3->sound,
                    m3->pos[0], m3->pos[2], m3->radius);
        } else {
            SDL_Log("Map sound: missing %s", m3->sound);
        }
    }
}

// Loads the map and builds the scene. Called from "New game"/"Continue".
// While the world loads there is a loading screen: img/loading.png background
// plus a progress bar at the bottom right labeled with the phase (loading.c
// presents the frames).
int game_load_world(Game *g)
{
    if (!g || g->world_loading) {
        SDL_Log("World load rejected: nested load");
        return -1;
    }
    g->world_loading = 1;
    // FR_STAGES: timing of world-load phases (hunting heavy spots in loading)
    const int st = getenv("FR_STAGES") != NULL;
    double st_f = (double)SDL_GetPerformanceFrequency();
    Uint64 st_t = SDL_GetPerformanceCounter(), st_p = st_t;
#define STLOG(msg) do { if (st) { st_p = SDL_GetPerformanceCounter(); \
        SDL_Log("LOAD %s: %.0f ms", msg, (st_p - st_t) * 1000.0 / st_f); \
        st_t = st_p; } } while (0)
    loading_begin();
    loading_progress(0.05f, "Загрузка уровня");
    WorldTextures wt;
    if (world_load_geometry(g, &wt) != 0) {
        loading_end();
        g->world_loading = 0;
        return -1;
    } // phase 1
    STLOG("geometry+textures");
    loading_progress(0.30f, "Собираем сцену");
    if (scene_build(g, &wt) != 0) {
        // scene_build already freed the partial scene (scene_teardown inside);
        // here — roll back the phase 1 data, otherwise it leaks.
        world_unload_geometry(g);
        SDL_Log("World load failed: scene_build");
        loading_end();
        g->world_loading = 0;
        return -1;                                       // phase 2: scene
    }
    STLOG("scene_build");
    loading_progress(0.60f, "Настройки карты");
    map_apply();                                       // phase 3: cvars/commands
    loading_progress(0.75f, "Звук карты");
    audio_load_map(g);                                 // phase 4: map audio

    loading_progress(0.85f, "Тени и маски");
    // The map's baked light (map/baked): the engine only READS it. No file —
    // terrain_baked_light_have() = 0, terrain keeps using the old computed path.
    // Loaded FIRST — whether the CPU static rebake is needed depends on it.
    terrain_baked_light_load(&g->world);
    // Static shadows (mode 2): the offline lightmap already contains static
    // shadows (+AO+bounces), and in mode 2 the shader uses exactly that. The
    // CPU rebake (terrain_shadows_bake) is needed ONLY as a fallback for maps
    // WITHOUT baked light — otherwise it is seconds wasted on a result nobody reads.
    if (cvar_get("r_shadows_mode") == 2.0f && !terrain_baked_light_have())
        terrain_shadows_bake(&g->world, (SunParams *)g->sun_node->params);
    // Wetness: baked "under roof" mask (always, useful for wetness)
    terrain_wetness_mask_bake(&g->world);
    STLOG("bakes");
    SDL_Log("World loaded (%.0fx%.0f m)", g->world.world_w, g->world.world_d);

    // Map script (lua/maps/<map>.lua): after the scene and entities
    loading_progress(0.95f, "Скрипт карты");
    lua_engine_map(g->map_name);
    loading_progress(1.0f, "Готово");
    loading_end();
    g->world_loading = 0;
    return 0;
}

// --- World unload phases (STRICT INVERSE of the creation order) ----------------
//  Inverse of phase 4 (audio_load_map)
static void audio_stop_map(void)
{
    audio_loop_stop();           // wind/ambient — in game only
    audio_music_stop();          // map music — likewise
    audio_3d_stop_all();         // and positional sources
}

//  Inverse of phase 2 (scene_build): render FBO -> entities -> nodes in the
//  reverse creation order -> root -> shadows.
static void scene_teardown(Game *g)
{
    renderer_teardown();         // ~ renderer_setup (created last)
    ecs_scene_shutdown();        // ~ node ecs pairs (before freeing the tree)
    entity_shutdown();           // ~ entity_* (kill frees its own nodes)
    hud_cleanup(g->hud_node);    // ~ hud_make_node (last of the nodes)
    minimap_cleanup(g->mm_node); // ~ minimap_make_node
    playerbox_cleanup(g->playerbox_node); // ~ playerbox_make_node
    selection_cleanup(g->sel_node);
    field_cleanup(g->field_node);
    if (g->fauna_node) fauna_cleanup(g->fauna_node);
    if (g->particles_node) particles_cleanup(g->particles_node);
    if (g->decals_node) decals_cleanup(g->decals_node);
    if (g->water_node) water_cleanup(g->water_node);
    if (g->obj_node) objects_cleanup(g->obj_node);
    veg_cleanup(g->wheat_node);
    terrain_cleanup(g->terr_node);
    sky_cleanup(g->sky_node);    // ~ sky_make_node (first of the nodes)
    node_free_tree(g->root);     // ~ root and all nodes (camera, sun)
    shadow_cleanup();            // ~ shadow_init (created first)
    g->root = NULL;
    g->cam_node = g->sun_node = g->sky_node = g->terr_node = g->wheat_node = NULL;
    g->obj_node = g->field_node = g->sel_node = g->mm_node = g->hud_node = NULL;
    g->playerbox_node = NULL;
    g->fauna_node = g->particles_node = g->decals_node = g->water_node = NULL;
    g->cam = NULL;
    g->mm = NULL;
    g->hud = NULL;
    g->world_loaded = 0;
}

//  Inverse of phase 1 (world_load_geometry)
static void world_unload_geometry(Game *g)
{
    splat_free();                // ~ splat_build (material/occupancy mask)
    mapdata_free();              // ~ mapdata_load
    world_free(&g->world);       // ~ world_load
    resources_pak_close();       // ~ resources_pak_open
}

// Unloads the world when returning to the main menu
void game_unload_world(Game *g)
{
    if (!g->world_loaded) return;
    lua_engine_clear();          // reset the map script's timers/update
    audio_stop_map();            // phase 4: map audio
    scene_teardown(g);           // phase 2: scene
    world_unload_geometry(g);    // phase 1: map data
    material_clear_all();        // map materials do not carry over to the next one
    SDL_Log("World unloaded");
}

// --- Map choice: saved into config.cfg (map_name <name> line) ------------------
void game_map_config_load(Game *g)
{
    FILE *f = fopen("config.cfg", "r");
    if (f) {
        char line[128];
        while (fgets(line, sizeof line, f)) {
            if (sscanf(line, "map_name %63s", g->map_name) == 1) { }
        }
        fclose(f);
    }
    // FR_MAP — forced map selection (for baking all maps: make bake-maps).
    // Overrides both the default and config.cfg without touching the user config.
    const char *em = getenv("FR_MAP");
    if (em && em[0]) snprintf(g->map_name, sizeof g->map_name, "%s", em);
}

void game_map_config_save(Game *g)
{
    // map_name used to be appended at the end — each exit added another line
    // and the file grew. Now: read config.cfg (console_save_config just
    // updated it beforehand), drop all old map_name lines and write exactly one.
    char keep[4096] = { 0 };
    int n = 0;
    FILE *r = fopen("config.cfg", "r");
    if (r) {
        char line[128];
        while (fgets(line, sizeof line, r)) {
            char mname[64];
            if (sscanf(line, "map_name %63s", mname) == 1) continue;  // toss the old one
            size_t l = strlen(line);
            if (n + (int)l < (int)sizeof keep - 1) {
                memcpy(keep + n, line, l);
                n += (int)l;
                keep[n] = 0;
            }
        }
        fclose(r);
    }
    FILE *w = fopen("config.cfg", "w");
    if (w) {
        fputs(keep, w);
        fprintf(w, "map_name %s\n", g->map_name);
        fclose(w);
    }
}

// --- World update (every frame from the main loop) -----------------------------
void game_update(Game *g, float dt)
{
    if (!g->world_loaded) return;
    // Logic: the world only lives while playing; frozen in menus/pause
    if (app_state() == APP_PLAY) {
        node_update_tree(g->root, dt);
        fx_update(dt);              // effect emitters (rain/fire/...)
        entity_update_all(dt);

        // --- Player health: fall damage, regen, death/respawn ---
        // Fall damage: safe up to ~12 m/s (high jumps/small ledges),
        // above that it grows linearly. movement.c reports the ground-impact speed.
        float fall = movement_take_fall_impact();
        const float FALL_SAFE = 12.0f, FALL_DMG_K = 6.5f;
        if (fall > FALL_SAFE && !g->cam->cam.noclip && !g->cam->cam.fly)
            health_damage(&g->player.hp, (fall - FALL_SAFE) * FALL_DMG_K);
        int respawn = 0;
        health_update(&g->player.hp, dt, &respawn);
        // Dead: freeze input/movement (no walking, no looking around) and
        // gently "drop" the camera — the gaze settles downward as if the player
        // fell. Respawn lifts the freeze and teleports to the spawn point.
        int dead = health_is_dead(&g->player.hp);
        camera_set_frozen(dead);
        if (dead) {
            const float DEATH_PITCH = 0.62f;   // pitch+ = looking down (see movement)
            float k = dt * 3.5f; if (k > 1.0f) k = 1.0f;
            g->cam->cam.pitch += (DEATH_PITCH - g->cam->cam.pitch) * k;
            g->cam->cam.vx = g->cam->cam.vy = g->cam->cam.vz = 0.0f;
        }
        if (respawn) {
            game_apply_spawn(g, g->player.spawn_pos,
                             g->player.spawn_yaw, g->player.spawn_pitch);
            health_revive(&g->player.hp);
            camera_set_frozen(0);
        }

        if (mapdata_field_attached() && g->field_node) {
            g->field_node->pos[0] = g->cam->cam.pos[0];
            g->field_node->pos[2] = g->cam->cam.pos[2];
        }
        map_actions_update(g);      // triggers + interaction (E)

        // ECS layer: pull node transforms -> systems -> push into changed
        // nodes. AFTER all logic and BEFORE node_update_transforms — the world
        // matrices for the renderer are computed from final positions.
        {
            static int s_ecs_was_on = -1;
            int on = cvar_get("ecs_enable") != 0.0f;
            if (on && s_ecs_was_on == 0) ecs_scene_new_generation();
            // Without a new gen, toggling "on the fly" would spawn duplicates:
            // the pool remembers last run's pairs while pull would create new ones.
            s_ecs_was_on = on;
            ecs_scene_enable(on);
            if (on) ecs_scene_frame(g->root, dt);
        }
    } else {
        // The camera recomputes its matrices even in menus/pause (input blocked):
        // FOV from the menu applies immediately, not only when entering the game.
        camera_node_update(g->cam_node, dt);
    }
    // Node world matrices: AFTER all logic (positions/rotations are final) and
    // BEFORE rendering. Children attached to model bones get fresh attachment
    // point matrices in this same frame.
    node_update_transforms(g->root);
    voxfog_update(g->cam->cam.pos);
    audio_set_listener(g->cam->cam.pos);
    audio_set_world(&g->world);   // audio occlusion by terrain (heights==NULL is fine)
    // Map message timer (message from triggers/use)
    if (g->ui_msg_t > 0.0f && app_state() == APP_PLAY)
        g->ui_msg_t -= dt;
}
