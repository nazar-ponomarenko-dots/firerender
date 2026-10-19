// ============================================================================
//  game.h — game state and world lifecycle (see game.c)
// ----------------------------------------------------------------------------
//  All world state is gathered into ONE Game struct (previously 13 core
//  globals plus the map/spawn globals in main.c): scene nodes, camera/HUD,
//  selected map, spawn/save, map message, demo and world data
//  (WorldState from world.h). Modules receive a Game* parameter instead of
//  grabbing a global; for functions without a parameter (console commands)
//  there is game_instance().
// ============================================================================
#ifndef GAME_H
#define GAME_H
#include "audio.h"
#include "camera.h"
#include "hud.h"
#include "minimap.h"
#include "player.h"
#include "scene.h"
#include "sun.h"
#include "world.h"

// --- Game state ---------------------------------------------------------
typedef struct {
    // --- Scene nodes (created in game_load_world) ---
    SceneNode *root;           // tree root (World)
    SceneNode *cam_node;
    SceneNode *sun_node;
    SceneNode *sky_node, *terr_node, *wheat_node;
    SceneNode *obj_node, *field_node;
    SceneNode *fauna_node;
    SceneNode *particles_node, *decals_node;
    SceneNode *water_node;       // water: puddles/lakes/rivers/seas (water.c)
    SceneNode *sel_node, *mm_node, *hud_node;
    SceneNode *playerbox_node;   // character collision box (3rd person)
    CameraParams *cam;         // Camera node parameters
    MinimapParams *mm;         // Minimap node parameters
    HudParams *hud;            // DebugHUD node parameters
    int world_loaded;
    int world_loading;          // load transaction: blocks map/restart from map config/Lua

    // Current map: maps/<name>.zip (the only format — map pak)
    char map_name[64];

    // Wheat field: position/size of the FieldArea node (computed by the
    // world_load_geometry phase, uses scene_build)
    float field_pos[3];
    float field_size[3];

    // Player: spawn, save/load, teleport (see player.h)
    Player player;

    // Map message (message from triggers/use) — drawn by renderer.c
    char ui_msg[256];
    float ui_msg_t;

    // Demo scene (Q key): automated fly-through with FPS measurement
    int demo_on;
    float demo_t;
    double demo_frames, demo_elapsed, demo_min_dt, demo_max_dt;

    // Map sounds (loops): default wind, ambient and music from map.json
    Sound *wind, *ambient, *music;

    // World data (heights, sizes, field) — filled by world_load
    WorldState world;
} Game;

// --- Functions ------------------------------------------------------------------
// Registers the single instance (call before the other functions).
// Zeroes the state, sets default map and height curve.
void game_init(Game *g);

// The single instance (for functions without a parameter: console commands,
// input.c, sync_cvars in main.c).
Game *game_instance(void);

// Player teleport: position + look direction, resets velocities.
void game_apply_spawn(Game *g, const float pos[3], float yaw, float pitch);

// World load/unload: map pak, map.json, scene nodes, map sound.
// load returns 0 on success. Call from the "new game"/"continue" menu actions.
int game_load_world(Game *g);
void game_unload_world(Game *g);

// Selected map: config.cfg (line map_name <name>)
void game_map_config_load(Game *g);
void game_map_config_save(Game *g);

// World update: node logic, triggers/use, fog, map sound.
// Call every frame from the main loop (in menu/pause — camera only).
void game_update(Game *g, float dt);

// Demo scene (Q key): automated fly-through with FPS measurement. inst_fps — frame FPS.
void game_demo_toggle(Game *g);
void game_demo_stop(Game *g);
void game_demo_tick(Game *g, float dt, float inst_fps);

#endif
