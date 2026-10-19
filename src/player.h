// ============================================================================
//  player.h — player state (the "engine↔game" module)
// ----------------------------------------------------------------------------
//  The boundary between the "engine" and the "player": everything about the
//  character — spawn position, save/load, teleport — lives here, not in
//  game.c. The engine (camera, physics, renderer) does not know the player;
//  game-layer modules (commands.c, lua) work through Player* and
//  game_apply_spawn.
//
//  The physical state (position/velocities/view) is the Camera from
//  camera.h; Player keeps only the "high-level" character state: where the
//  spawn is and where to teleport/load to.
// ============================================================================
#ifndef PLAYER_H
#define PLAYER_H
#include "camera.h"
#include "health.h"

typedef struct {
    // Starting position (spawn) — from map.json, for the "New Game" menu item
    float spawn_pos[3];
    float spawn_yaw, spawn_pitch;
    // Save (F5/F9): position + view + map (save.cfg)
    float save_pos[3];
    float save_yaw, save_pitch;
    char save_map[64];
    int pending_spawn;       // teleport after the world loads (load from menu)
    Health hp;               // player health (health.h)
} Player;

// Zeroes the state and sets the default spawn angles (yaw defaults to PI).
void player_init(Player *p);

// Sets the spawn point (position + view). Called while building the world.
void player_set_spawn(Player *p, const float pos[3], float yaw, float pitch);

// Teleports the player: position + view onto the Camera, resets velocities/fly.
void player_teleport(Player *p, Camera *cam, const float pos[3],
                     float yaw, float pitch);

// Save the player's current state (camera position/view + map).
void player_save(Player *p, const Camera *cam, const char *map_name);

// Load the saved position/view/map (from save.cfg).
void player_load(Player *p, float x, float y, float z,
                 float yaw, float pitch, const char *map_name);

#endif