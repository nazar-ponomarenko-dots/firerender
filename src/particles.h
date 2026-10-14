// ============================================================================
//  particles.h — particle system (instanced sprites, CPU update)
// ----------------------------------------------------------------------------
//  Fixed-size particle pool. Each particle is a billboard quad with a
//  texture, additive or alpha blend. Update runs in the scene tick
//  (particles_update with dt from the fixed step), render is ONE instanced
//  draw call for the whole pool.
//
//  Particle behaviour is defined by ParticleParams (size/alpha over lifetime,
//  gravity, air drag, wind, sway, streak for rain, ground bounce for blood).
//  Named effect presets and continuous emitters live in fx.h.
//
//  Usage:
//    particles_make_node(w, rc)     — scene node (created on world load)
//    particles_burst(&pp, count)    — particle burst from the given params
//    particles_clear()              — reset the pool (map change)
//    particles_cleanup(node)        — free the GL resources
// ============================================================================
#ifndef PARTICLES_H
#define PARTICLES_H
#include "gl_loader.h"
#include "render_ctx.h"
#include "scene.h"
#include "world.h"

#define PARTICLES_MAX 4096         // pool size (instancing: 2 draw calls)

// --- Particle parameters -------------------------------------------------------
typedef struct {
    float pos[3];              // base position (center of the spawn area)
    float dir[3];              // base direction (0,1,0 = up)
    float spread;              // spread, rad (0 = exactly along dir)
    float speed0, speed1;      // initial speed range, m/s
    float life0, life1;        // lifetime range, s
    float size0, size1;        // size at the start and end of life, m
    float alpha0, alpha1;      // alpha at the start and end of life
    float r, g, b;             // color
    float gravity;             // m/s2 (>0 down; <0 up, e.g. smoke)
    float drag;                // air drag: fraction of speed lost per s
    float sway;                // sway amplitude (snow/dust), m/s
    float turb;                // turbulence: trajectory swirl, m/s
    float wind[3];             // constant wind, m/s
    int   blend;               // 0 = alpha blend, 1 = additive (light)
    int   streak;              // 1 = stretch along velocity (rain/drops)
    int   plain;               // 1 = NO texture: bright circle (snowflake)
    int   bounce;              // 1 = bounce off the ground (blood/sparks) — needs world
    int   collide;             // collide with terrain AND the water surface:
                               // 1 = dies on contact (rain),
                               // 2 = settles on the surface and fades (snow)
    // Speed distribution (snow: large/small flakes fall at different speeds).
    // With nbucket > 0, on emission a bucket_mul factor is picked by weights
    // bucket_w: it scales BOTH the speed AND the gravity
    // (terminal velocity = gravity/drag, so both must be multiplied).
    int   nbucket;
    float bucket_mul[4];
    float bucket_w[4];
} ParticleParams;

// Emits ONE particle from the params (random speed/life/size within the
// ranges). Direction dir + spread cone.
void particles_emit(const ParticleParams *pp);

// count particles from one point/area.
void particles_burst(const ParticleParams *pp, int count);

// Updates all live particles (dt — fixed step). Call from
// game_update or the node's update.
void particles_update(float dt);

// Renders all live particles (two instanced draws: alpha blend + additive).
void particles_draw(const RendererContext *rc);

// Creates the Particles node. w — heightmap (for bounce particle ground hits).
// NULL on error.
SceneNode *particles_make_node(const WorldState *w, const RendererContext *rc);

// Resets the pool (on map change / world unload).
void particles_clear(void);

// Frees the node's GL resources.
void particles_cleanup(SceneNode *n);

#endif
