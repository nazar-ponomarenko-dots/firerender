// ============================================================================
//  movement.c — player physics/movement (see movement.h)
// ----------------------------------------------------------------------------
//  Moved from camera.c: everything not about the view — walking (gravity,
//  ground, map-object collision, water/buoyancy), flying (fly), noclip
//  pass-through and player sounds (steps/jump/landing/rustle). The camera
//  (view) stayed in camera.c; here only movement and its side effects.
// ============================================================================
#include "movement.h"

#include "actions.h"
#include "audio.h"
#include "cvars.h"
#include "linalg.h"     // M_PI for slope steepness computation
#include "objects.h"    // objects_collide — collision with map objects
#include "phys_body.h"  // phys_body_push — the player pushes rigid bodies
#include "water.h"      // water_surface_at / water_flow_at

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

// --- Player sounds (steps/jump/landing) ------------------------------------
// Loaded lazily, through resources: the map may override them from its zip.
static Sound *s_step[3] = { NULL, NULL, NULL };
static Sound *s_land = NULL;
static Sound *s_rustle[3] = { NULL, NULL, NULL };  // three rustle variants
static Sound *s_jump = NULL;
static int s_rustle_sel = 0;
static int s_step_tried = 0;
static int s_step_sel = 0;
static float s_step_timer = 0.0f;
static float s_rustle_timer = 0.0f;
static int s_was_air = 0;    // was the player airborne last frame (landing)
static int s_prev_duck = 0;  // were we crouched last frame (change is not a fall)
static float s_fall_impact = 0.0f;  // fall speed at the moment of landing (m/s)

float movement_take_fall_impact(void)
{
    float v = s_fall_impact;
    s_fall_impact = 0.0f;
    return v;
}

// Player position (listener = camera; the sound source — the player)
#define SOUND_RADIUS 25.0f    // audibility radius of player sounds, m
static float s_player_pos[3] = { 0, 0, 0 };

static void player_sounds_load(void)
{
    if (s_step_tried) return;
    s_step_tried = 1;
    for (int i = 0; i < 3; i++) {
        char p[64];
        snprintf(p, sizeof p, "audio/step%d.wav", i + 1);
        s_step[i] = audio_load(p);
    }
    s_land = audio_load("audio/land.wav");
    for (int i = 0; i < 3; i++) {
        char p[64];
        snprintf(p, sizeof p, "audio/rustle%d.wav", i + 1);
        s_rustle[i] = audio_load(p);
    }
    s_jump = audio_load("audio/jump.wav");
}

// Counter of played steps (for snd_debug in renderer.c)
int g_step_calls = 0;

static void player_step_sound(void)
{
    if (!s_step[0] && !s_step[1] && !s_step[2]) return;
    Sound *s = s_step[s_step_sel++ % 3];
    // Crouched — quieter steps. Positional: source at the player's spot.
    float vol = g_act[A_DUCK] ? 0.25f : 0.5f;
    g_step_calls++;
    audio_3d_play_once(s, vol, s_player_pos, SOUND_RADIUS);
}

void movement_update(Camera *cam, const WorldState *w, float dt)
{
    if (!cam || !w) return;
    player_sounds_load();

    float fwd[3], right[3];
    camera_dirs(cam, fwd, right);

    // Movement parameters — cvars from the console (set cl_walkspeed 5 etc.)
    float maxspeed = cvar_get((cam->fly || cam->noclip) ? "cl_flightspeed"
                                                        : "cl_walkspeed");
    if (!cam->fly && !cam->noclip) {   // sv_maxspeed overrides walking (HL-style)
        float ms = cvar_get("sv_maxspeed");
        if (ms > 0.0f) maxspeed = ms;
    }
    if (maxspeed <= 0.0f) maxspeed = (cam->fly || cam->noclip)
                                   ? CAM_SPEED : WALK_SPEED;
    if (g_act[A_SPEED]) maxspeed *= 4.0f;          // +speed — run
    float speed = maxspeed * dt;

    // NOCLIP: character collisions are temporarily off. A separate
    // controller, like flight (fly): free movement ALONG THE VIEW (forward/
    // backward with a vertical component), Space/Ctrl — up/down. No gravity,
    // ground or objects. The position is NOT teleported — when noclip is
    // switched off the character stays where it was and obeys walk collision again.
    if (cam->noclip) {
        if (g_act[A_FORWARD]) { cam->pos[0] += fwd[0]*speed; cam->pos[1] += fwd[1]*speed; cam->pos[2] += fwd[2]*speed; }
        if (g_act[A_BACK])    { cam->pos[0] -= fwd[0]*speed; cam->pos[1] -= fwd[1]*speed; cam->pos[2] -= fwd[2]*speed; }
        if (g_act[A_RIGHT])   { cam->pos[0] += right[0]*speed; cam->pos[2] += right[2]*speed; }
        if (g_act[A_LEFT])    { cam->pos[0] -= right[0]*speed; cam->pos[2] -= right[2]*speed; }
        if (g_act[A_JUMP]) cam->pos[1] += speed;
        if (g_act[A_DOWN]) cam->pos[1] -= speed;
        cam->vx = cam->vz = 0; cam->vy = 0;   // walking starts from zero
        s_was_air = 0;
    } else

    if (cam->fly) {
        // --- flight: free movement along the view ---
        if (g_act[A_FORWARD]) { cam->pos[0] += fwd[0]*speed; cam->pos[1] += fwd[1]*speed; cam->pos[2] += fwd[2]*speed; }
        if (g_act[A_BACK])    { cam->pos[0] -= fwd[0]*speed; cam->pos[1] -= fwd[1]*speed; cam->pos[2] -= fwd[2]*speed; }
        if (g_act[A_RIGHT])   { cam->pos[0] += right[0]*speed; cam->pos[2] += right[2]*speed; }
        if (g_act[A_LEFT])    { cam->pos[0] -= right[0]*speed; cam->pos[2] -= right[2]*speed; }
        if (g_act[A_JUMP]) cam->pos[1] += speed;
        if (g_act[A_DOWN]) cam->pos[1] -= speed;
    } else {
        // --- walking: a Half-Life 1 style controller ---
        // Crouching (+duck): lower eyes, slower, quieter steps.
        // Toggling crouch/stand instantly puts the camera on the ground —
        // that is NOT a fall: the landing sound does not play.
        bool duck = g_act[A_DUCK];
        float eye = duck ? EYE_DUCK : EYE_H;
        float ground = sample_height(w, cam->pos[0], cam->pos[2]) + eye;
        // There is no ground beyond the map edge: the player falls down (with
        // no invisible wall; otherwise sample_height clamped the height to the
        // edge and the player "hung" over the rim)
        bool offmap = cam->pos[0] < 0.0f || cam->pos[0] > w->world_w ||
                      cam->pos[2] < 0.0f || cam->pos[2] > w->world_d;
        if (offmap) ground = -1e30f;
        if (duck != s_prev_duck) {
            s_prev_duck = duck;
            if (ground > -1e29f) {     // no instant crouch off-map
                cam->pos[1] = ground;    // instantly sit/stand
                cam->vy = 0;
            }
            s_was_air = 0;
        }
        bool on_ground = cam->pos[1] <= ground + 0.01f && cam->vy <= 0.01f;
        if (duck) maxspeed *= 0.5f;          // slower while crouched

        float wishx = 0, wishz = 0;
        if (g_act[A_FORWARD]) { wishx += fwd[0]; wishz += fwd[2]; }
        if (g_act[A_BACK])    { wishx -= fwd[0]; wishz -= fwd[2]; }
        if (g_act[A_RIGHT])   { wishx += right[0]; wishz += right[2]; }
        if (g_act[A_LEFT])    { wishx -= right[0]; wishz -= right[2]; }
        float wlen = sqrtf(wishx*wishx + wishz*wishz);
        bool want_move = wlen > 1e-4f;
        if (want_move) { wishx /= wlen; wishz /= wlen; }

        // Solid terrain (sv_maxslope): a slope steeper than max slope acts
        // like a wall. We slide along the equal-height contour instead of climbing
        // a too-steep slope.
        if (want_move && !cam->fly) {
            float max_slope = cvar_get("sv_maxslope");
            if (max_slope > 0.0f && max_slope < 90.0f) {
                float e = 0.75f;   // height-gradient sample radius, m
                float hx = sample_height(w, cam->pos[0] + e, cam->pos[2])
                         - sample_height(w, cam->pos[0] - e, cam->pos[2]);
                float hz = sample_height(w, cam->pos[0], cam->pos[2] + e)
                         - sample_height(w, cam->pos[0], cam->pos[2] - e);
                float glen = sqrtf(hx*hx + hz*hz);          // |height gradient|
                float slope_sin = glen / sqrtf(glen*glen + 4.0f*e*e); // sin of the slope angle
                float step_up = wishx*hx + wishz*hz;        // are we moving up the slope
                if (step_up > 0.0f && slope_sin > sinf(max_slope * (float)M_PI / 180.0f)) {
                    // moving up a too-steep slope: project the wish along the
                    // equal-height contour (perpendicular to the gradient)
                    float cx = -hz, cz = hx;                // along the contour
                    float clen = sqrtf(cx*cx + cz*cz);
                    if (clen > 1e-5f) {
                        cx /= clen; cz /= clen;
                        float along = wishx*cx + wishz*cz;
                        wishx = cx * along;
                        wishz = cz * along;
                        wlen = fabsf(along);
                    } else {
                        wishx = wishz = 0.0f;
                        wlen = 0.0f;
                    }
                    want_move = wlen > 1e-4f;
                }
            }
        }

        // acceleration toward the desired speed (weaker in the air)
        if (want_move) {
            float dot = cam->vx*wishx + cam->vz*wishz;
            float add = maxspeed - dot;
            if (add > 0.0f) {
                float a = (on_ground ? 8.0f : 2.5f) * maxspeed * dt;
                if (a > add) a = add;
                cam->vx += wishx * a;
                cam->vz += wishz * a;
            }
        } else if (on_ground) {
            // ground friction
            float fr = 1.0f - 6.0f * dt;
            if (fr < 0.0f) fr = 0.0f;
            cam->vx *= fr;
            cam->vz *= fr;
        }
        // speed ceiling
        float sp2 = cam->vx*cam->vx + cam->vz*cam->vz;
        if (sp2 > maxspeed*maxspeed) {
            float s = maxspeed / sqrtf(sp2);
            cam->vx *= s; cam->vz *= s;
        }
        cam->pos[0] += cam->vx * dt;
        cam->pos[2] += cam->vz * dt;
        // Map object collisions (collision=1): do not pass through them.
        // Character radius = half the collision width/depth (1 m -> 0.5 m,
        // see camera.h PLAYER_HALF_W); wheat is parted separately in wheat.vs.
        objects_collide(&cam->pos[0], &cam->pos[2], PLAYER_HALF_W);

        // Bodies (phys_body): the player pushes spheres/boxes at the waist —
        // impulse from the horizontal speed. Only in the walking branch: in
        // fly/noclip there are no collisions by contract (see src/movement.c:107).
        {
            float vel[3] = { cam->vx, 0.0f, cam->vz };
            float lim = cvar_get("phys_push_speed");
            if (lim <= 0.0f) lim = 4.0f;
            phys_body_push(cam->pos, vel, PLAYER_HALF_W, lim);
        }

        float gravity = cvar_get("sv_gravity");
        if (gravity <= 0.0f) gravity = GRAVITY;
        cam->vy -= gravity * dt;                 // gravity

        // Water: if the point is covered by a water body and the eyes are under
        // the surface (water deeper than the player's height) — we swim.
        // Buoyancy pushes toward the surface, and the river current
        // (water_flow_at) carries the player along.
        float wl = water_surface_at(w, cam->pos[0], cam->pos[2]);
        if (offmap) wl = -1e30f;       // no water off-map either
        bool in_water = wl > ground - 0.05f;     // surface above the "bottom"
        if (in_water && cam->pos[1] < wl - 0.05f) {   // eyes under water
            // Buoyancy: underwater gravity is damped and the push-out exceeds
            // it (24 > 20) — the player floats up to the surface instead of
            // sinking to the bottom (there the water was not visible "up close").
            cam->vy += (24.0f - gravity * 0.5f) * dt;
            cam->vy *= 0.98f;                    // viscous drag
            float flo[2];
            if (water_flow_at(w, cam->pos[0], cam->pos[2], flo)) {
                cam->vx += flo[0] * 3.5f * dt;   // the river current carries along
                cam->vz += flo[1] * 3.5f * dt;
            }
        }

        float ny = cam->pos[1] + cam->vy * dt;
        if (in_water && ny > wl) ny = wl;        // don't surface into the air
        if (ny <= ground) {                      // landed (or already standing)
            float fall = cam->vy;
            cam->pos[1] = ground;
            cam->vy = 0;
            float jump = cvar_get("sv_jumpspeed");
            if (jump <= 0.0f) jump = JUMP_V;
            if (g_act[A_JUMP]) {
                cam->vy = jump;                  // jump
                if (s_jump)
                    audio_3d_play_once(s_jump, 0.3f, s_player_pos, SOUND_RADIUS);
            } else if (s_was_air && fall < -1.5f) {
                // Landing after falling from a height: sound ALWAYS,
                // volume depends on the fall speed.
                if (s_land) {
                    float vol = 0.2f + (-fall) / 14.0f;
                    if (vol > 0.6f) vol = 0.6f;
                    audio_3d_play_once(s_land, vol, s_player_pos, SOUND_RADIUS);
                }
                // Fall speed -> game layer (fall damage, game.c).
                s_fall_impact = -fall;
            }
            s_was_air = 0;
        } else {
            cam->pos[1] = ny;                    // flying/falling
            s_was_air = 1;
        }

        // Steps: moving on the ground (not flying, not falling). The timer
        // depends on speed: running — more often. Crouching — quieter.
        float vsp2 = cam->vx * cam->vx + cam->vz * cam->vz;
        if (!in_water && cam->vy <= 0.1f && vsp2 > 0.5f) {
            s_step_timer -= dt;
            if (s_step_timer <= 0.0f) {
                float spd = sqrtf(vsp2);
                s_step_timer = (spd > 1.5f) ? 0.34f : 0.5f;
                player_step_sound();
            }
        } else {
            s_step_timer = 0.0f;
        }

        // Rustling: only when moving through wheat (a field). Grass/bushes
        // make no sound. The three rustle variants alternate (like steps).
        if (!in_water && cam->vy <= 0.1f && vsp2 > 0.5f) {
            bool in_field =
                cam->pos[0] >= w->field_x0 && cam->pos[0] <= w->field_x0 + w->field_w &&
                cam->pos[2] >= w->field_z0 && cam->pos[2] <= w->field_z0 + w->field_d;
            if (in_field) {
                s_rustle_timer -= dt;
                if (s_rustle_timer <= 0.0f) {
                    s_rustle_timer = 0.45f;
                    Sound *s = s_rustle[s_rustle_sel++ % 3];
                    if (s)
                        audio_3d_play_once(s, 0.32f, s_player_pos, SOUND_RADIUS);
                }
            } else {
                s_rustle_timer = 0.0f;
            }
        } else {
            s_rustle_timer = 0.0f;
        }
    }
    // player position — for positional sounds (steps etc.)
    s_player_pos[0] = cam->pos[0];
    s_player_pos[1] = cam->pos[1];
    s_player_pos[2] = cam->pos[2];
}