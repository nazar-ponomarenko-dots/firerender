// ============================================================================
//  camera.h — camera (first-person POV)
// ----------------------------------------------------------------------------
//  Two modes (key F):
//    fly  — free flight: WASD along the view, SPACE/CTRL up/down;
//    walk — walking with minimal physics: gravity, collision with the terrain
//           (ground = the same quantized height the shader draws), jump.
// ============================================================================
#ifndef CAMERA_H
#define CAMERA_H
#include <stdbool.h>
#include "scene.h"
#include "world.h"

#define CAM_SPEED 25.0f     // Flight speed (m/s), Shift multiplies by 4
#define WALK_SPEED 2.5f     // Maximum walking speed (m/s)
#define MOUSE_SENS 0.0025f  // Mouse sensitivity (rad/pixel)
#define EYE_H   0.8f        // eye height above ground (the character is short)
#define EYE_DUCK 0.35f      // eye height when crouching (+duck)
#define GRAVITY 20.0f       // free-fall acceleration, m/s2
#define JUMP_V   7.0f       // jump velocity, m/s

// --- Character physical collision (hitbox) ------------------------------------
// Width/depth 1 m -> half 0.5 m; height 2 m. Vegetation (wheat) uses its OWN
// push radius in wheat.vs and does not depend on these constants.
#define PLAYER_HALF_W  0.5f   // half collision width/depth, m (1 m overall)
#define PLAYER_HEIGHT  2.0f   // character collision height, m

typedef struct {
    float pos[3];   // position in the world
    float yaw;      // left-right rotation (rad)
    float pitch;    // up-down tilt (rad)
    float vy;       // vertical velocity (walking physics)
    float vx, vz;   // horizontal velocity (HL1 controller: inertia)
    bool fly;       // flight mode (F)
    bool noclip;    // noclip: character collisions off (terrain/objects
                    // are passable) — plain on/off, no teleporting
} Camera;

typedef struct {
    Camera cam;          // position, yaw, pitch
    const WorldState *world;  // world data: heights, sizes, field
    float fov_deg;       // field of view angle
    float znear, zfar;   // near/far clipping planes
    float aspect;        // window aspect ratio (updated by the main loop)
    float viewproj[16];  // view * projection — the node computes it each frame
    // The actual basis of the VIEW camera — the point the render really looks from
    // (in third person the camera stands behind the character and looks at the aim
    // point, not where the player's view is aimed). Sky, particles, fog and
    // post-processing take the basis FROM HERE — otherwise the skybox and rays
    // drifted apart from the scene in third person.
    float view_pos[3];
    float view_fwd[3], view_right[3], view_up[3];
} CameraParams;

// Camera view directions (forward, right) — for WASD movement.
// right = cross(forward, up) — the SAME basis mat4_lookat builds (there s = cross(f, up)),
// otherwise "right" would come out mirrored.
void camera_dirs(const Camera *cam, float *forward, float *right);

// Bind the mouse smoothing targets (m_smooth) to the camera's current yaw/pitch.
// Call after writing the view directly (teleport/spawn/load): otherwise the
// interpolation will drag the view back toward the old target.
void camera_sync_look(Camera *cam);

// Movement by actions (actions.h: +forward/-forward etc.) and mouse look
// (dx, dy). In walking mode — gravity and the ground underfoot;
// in flight — free.
void camera_update(Camera *cam, const WorldState *w, float dx, float dy, float dt);

// Creates a Camera3D node with the given start position and view parameters.
SceneNode *camera_make_node(const WorldState *w, const float *start_pos,
                            float yaw, float pitch,
                            float fov_deg, float znear, float zfar);

void camera_node_update(SceneNode *n, float dt);

// Demo scene (key Q): 1 — the camera ignores input, main.c drives position
// and view (autopilot along a route); 0 — normal input.
void camera_demo_set(int on);

// Player input lock (1 = console open): mouse and WASD do not touch
// the camera while the console is accepting text.
void camera_lock_input(int lock);

// Character freeze (independent of the demo flag): input and movement
// (WASD/mouse) are blocked until unfrozen. Separate from camera_lock_input —
// different layers set them.
void camera_set_frozen(int frozen);

// Raycast: a ray from the camera down to the terrain.
// Returns 1 if it hit the terrain within max_dist (m).
// out_x/out_y/out_z — the hit point (y = terrain height).
int camera_raycast_terrain(const Camera *cam, const WorldState *w,
                           float max_dist, float *out_x, float *out_y, float *out_z);

#endif
