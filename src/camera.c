// ============================================================================
//  camera.c — camera: input (WASD + mouse), walk physics, view/projection matrices
// ============================================================================
#include "camera.h"

#include "cvars.h"
#include "gamepad.h"
#include "linalg.h"
#include "movement.h"
#include "touch.h"
#include "world.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void camera_dirs(const Camera *cam, float *forward, float *right)
{
    float cp = cosf(cam->pitch);
    forward[0] = sinf(cam->yaw) * cp;
    forward[1] = sinf(cam->pitch);
    forward[2] = cosf(cam->yaw) * cp;
    if (right) {
        right[0] = -forward[2];   // cross(forward, up) = (-f.z, 0, f.x)
        right[1] = 0.0f;
        right[2] = forward[0];
    }
}

// Look interpolation targets (m_smooth) live in module scope so that
// camera_sync_look can snap them to the current look on a teleport.
static float s_want_yaw = 0.0f, s_want_pitch = 0.0f;
static int s_smooth_init = 0;

void camera_update(Camera *cam, const WorldState *w,
                    float dx, float dy, float dt)
{
    // Movement parameters — cvars from the console (set cl_sens 0.01 etc.)
    float sens = cvar_get("cl_sens");
    if (sens <= 0.0f) sens = MOUSE_SENS;

    // Mouse to the right (dx>0) — turn the view to the right. In our system
    // yaw grows LEFTWARD (forward = (sin yaw, 0, cos yaw)), hence the minus.
    // m_invert — vertical inversion (menu: Controls).
    // Look interpolation (cvar m_smooth, 0..1): 0 — raw motion as before,
    // 1 — heavy smoothing. The target yaw/pitch accumulate input while the
    // current look catches up to them exponentially (FPS-independent).
    // Targets are in module scope: camera_sync_look (teleports) snaps them
    // to the current look directly.
    float inv = cvar_get("m_invert") > 0.5f ? -1.0f : 1.0f;
    float lim = 3.14159f / 2.0f - 0.01f;
    if (!s_smooth_init) {
        s_want_yaw = cam->yaw;
        s_want_pitch = cam->pitch;
        s_smooth_init = 1;
    }
    // Teleport/new game/load: the look was reset externally — must NOT chase
    // the old target (the camera would slide). Snap the targets to the current look.
    if (fabsf(cam->yaw - s_want_yaw) > 3.0f ||
        fabsf(cam->pitch - s_want_pitch) > 3.0f) {
        s_want_yaw = cam->yaw;
        s_want_pitch = cam->pitch;
    }
    s_want_yaw  -= dx * sens;
    s_want_pitch -= dy * sens * inv;       // mouse up — look up
    if (s_want_pitch >  lim) s_want_pitch =  lim;
    if (s_want_pitch < -lim) s_want_pitch = -lim;
    float smooth = cvar_get("m_smooth");
    if (smooth < 0.0f) smooth = 0.0f;
    if (smooth > 1.0f) smooth = 1.0f;
    if (smooth < 0.001f) {                 // 0 — instant, as before
        cam->yaw = s_want_yaw;
        cam->pitch = s_want_pitch;
    } else {
        // Fraction of the distance closed per frame at 60 FPS: smooth=1 -> 0.05
        // ("wild" interpolation, strong drift), smooth->0 -> ~1 (nearly instant)
        float k = 0.05f + 0.95f * (1.0f - smooth);
        float f = 1.0f - powf(1.0f - k, 60.0f * dt);
        cam->yaw   += (s_want_yaw  - cam->yaw) * f;
        cam->pitch += (s_want_pitch - cam->pitch) * f;
    }
    if (cam->pitch >  lim) cam->pitch =  lim;   // don't let it go "behind the head"
    if (cam->pitch < -lim) cam->pitch = -lim;

    // Physics/movement (walk, fly, noclip, water, player sounds) is factored
    // out into a separate module movement.c so movement isn't hardcoded into the camera.
    movement_update(cam, w, dt);
}

// Snap the smoothing targets to the CURRENT look. Call after writing
// yaw/pitch directly (teleport, load, spawn): otherwise m_smooth would drag
// the look back to the old target — even with a small delta that never reaches
// the ">3 rad" heuristic in camera_update.
void camera_sync_look(Camera *cam)
{
    if (!cam) return;
    s_want_yaw = cam->yaw;
    s_want_pitch = cam->pitch;
    s_smooth_init = 1;
}

// --- Raycast: ray from the camera down to the terrain ------------------------
// Returns 1 if the ray hit the terrain, 0 if it left the world.
// out_x, out_z — hit point; out_y — terrain height at that point.
int camera_raycast_terrain(const Camera *cam, const WorldState *w,
                           float max_dist, float *out_x, float *out_y, float *out_z)
{
    if (!cam || !w) return 0;
    float fwd[3];
    camera_dirs(cam, fwd, NULL);
    float x = cam->pos[0];
    float y = cam->pos[1];
    float z = cam->pos[2];
    float step = 0.2f;   // march step (m)
    float dist = 0.0f;
    while (dist < max_dist) {
        x += fwd[0] * step;
        y += fwd[1] * step;
        z += fwd[2] * step;
        dist += step;
        // Check for leaving the world
        if (x < 1.0f || x > w->world_w - 1.0f ||
            z < 1.0f || z > w->world_d - 1.0f)
            return 0;
        float h = sample_height(w, x, z);
        if (y <= h + 0.1f) {
            if (out_x) *out_x = x;
            if (out_y) *out_y = h;
            if (out_z) *out_z = z;
            return 1;
        }
    }
    return 0;
}

SceneNode *camera_make_node(const WorldState *w, const float *start_pos,
                            float yaw, float pitch,
                            float fov_deg, float znear, float zfar)
{
    CameraParams *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->world = w;
    p->cam.pos[0] = start_pos[0];
    p->cam.pos[1] = start_pos[1];
    p->cam.pos[2] = start_pos[2];
    p->cam.yaw = yaw;
    p->cam.pitch = pitch;
    p->fov_deg = fov_deg;
    p->znear = znear;
    p->zfar = zfar;
    const float size[3] = { fov_deg, znear, zfar };   // fov, near, far — also a "size"
    SceneNode *node = node_new("Camera", "Camera3D", start_pos, size, p,
                               camera_node_update, NULL);
    if (!node) free(p);
    return node;
}

static int s_demo = 0;          // demo scene: input off, main.c drives the camera
static int s_input_lock = 0;    // console open: player input off
static int s_frozen = 0;        // player dead: input and movement frozen

void camera_demo_set(int on)
{
    s_demo = on;
}

void camera_lock_input(int lock)
{
    s_input_lock = lock;
}

void camera_set_frozen(int frozen)
{
    s_frozen = frozen;
}

void camera_node_update(SceneNode *n, float dt)
{
    CameraParams *p = n->params;
    // FOV — a cvar (console: set fov 90; the mouse wheel also writes the cvar)
    float fov = cvar_get("fov");
    if (fov > 0.0f && fov <= 180.0f) p->fov_deg = fov;
    if (!s_demo && !s_input_lock && !s_frozen) {
        float mx = 0, my = 0;
        if (!getenv("FR_NOMOUSE")) {
            SDL_GetRelativeMouseState(&mx, &my);
            float gx = 0, gy = 0;
            gamepad_look_take(&gx, &gy);
            mx += gx;
            my += gy;
#ifdef __ANDROID__
            float tx = 0, ty = 0;
            touch_look_take(&tx, &ty);
            mx += tx;
            my += ty;
#endif
        }
        camera_update(&p->cam, p->world, mx, my, dt);
    }

    // View and projection matrices. Third-person view (cvar cam_thirdperson):
    // chase camera — sits on a sphere of radius cam_tpd around the character's head
    // along the LOOK DIRECTION (pitch included: the camera orbits vertically
    // too); the target is the character's head. The camera look = the player's look
    // direction, so sky/billboards in renderer read the real view basis.
    float fwd[3], up[3] = {0, 1, 0}, center[3], view[16], proj[16];
    camera_dirs(&p->cam, fwd, NULL);
    center[0] = p->cam.pos[0] + fwd[0];
    center[1] = p->cam.pos[1] + fwd[1];
    center[2] = p->cam.pos[2] + fwd[2];
    if (cvar_get("cam_thirdperson") > 0.5f) {
        float dist = cvar_get("cam_tpd");   // camera distance, m
        if (dist < 1.0f) dist = 4.0f;
        if (dist > 12.0f) dist = 12.0f;
        // Character head — orbit center (slightly above the eye point)
        float head[3] = {
            p->cam.pos[0],
            p->cam.pos[1] + 0.3f,
            p->cam.pos[2],
        };
        // Camera — on the look sphere: pos - fwd*dist (full 3D look,
        // so moving the mouse up/down really orbits the camera around the character).
        float eye[3] = {
            head[0] - fwd[0] * dist,
            head[1] - fwd[1] * dist,
            head[2] - fwd[2] * dist,
        };
        // The camera must not sink into the terrain (on slopes/cliffs)
        float ground = sample_height(p->world, eye[0], eye[2]) + 0.35f;
        if (eye[1] < ground) eye[1] = ground;
        mat4_lookat(view, eye, head, up);
        // Actual view basis (where the renderer really looks from)
        memcpy(p->view_pos, eye, sizeof p->view_pos);
    } else {
        mat4_lookat(view, p->cam.pos, center, up);
        memcpy(p->view_pos, p->cam.pos, sizeof p->view_pos);
    }
    // Basis of the VIEW camera — taken from the finished view matrix (column-major):
    // right = (m0, m4, m8), up = (m1, m5, m9), forward = -(m2, m6, m10).
    // In first person it matches camera_dirs; in third person it is the real
    // render direction (sky/billboards/rays use it, not the player's look).
    p->view_right[0] = view[0]; p->view_right[1] = view[4]; p->view_right[2] = view[8];
    p->view_up[0]    = view[1]; p->view_up[1]    = view[5]; p->view_up[2]    = view[9];
    p->view_fwd[0]   = -view[2]; p->view_fwd[1]  = -view[6]; p->view_fwd[2]  = -view[10];
    mat4_perspective(proj, p->fov_deg * 3.14159f / 180.0f, p->aspect, p->znear, p->zfar);
    mat4_mul(p->viewproj, proj, view);
}
