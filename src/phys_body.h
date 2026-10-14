// ============================================================================
//  phys_body.h — rigid bodies on top of the Verlet world (see physics.h)
// ----------------------------------------------------------------------------
//  The engine physics stays "particles + constraints" (cheap Verlet, no C++ SDK).
//  This layer adds what the core lacked: a BODY as a group of particles with
//  a shape, STATIC scene colliders (spheres/cylinders/boxes from GLB props
//  and map.json) and "particle-vs-particle" interaction — otherwise a crate
//  falls through a crate.
//
//    * sphere  = 1 particle with radius;
//    * box     = 8 corners + edges + face diagonals (a rigid frame, like the
//                ragdoll skeleton in glb_anim.c: constraints hold the shape);
//    * gravity/integration/constraints — inherited from physics.c, nothing
//      is computed twice;
//    * masses — inv_mass per particle, a body of mass m gives 8 particles of m/8.
//
//  Bodies live in the GLOBAL world physics_world() alongside ropes, so a rope
//  can grab a crate. The switch phys_bodies (cvar) disables body simulation,
//  states are preserved (turn it on — bodies are where they were).
//
//  Console: phys_sphere/phys_box/phys_props/phys_bodies; FR_TEST 'u' — selftest,
//  'U' — a stack of crates in front of the camera (live run).
// ============================================================================
#ifndef PHYS_BODY_H
#define PHYS_BODY_H

#include "physics.h"

#define PHYS_BODY_MAX      128     // bodies in the world
#define PHYS_STATIC_MAX    256     // static colliders

typedef enum {
    PHYS_SPHERE = 0,
    PHYS_BOX
} PhysBodyShape;

typedef struct {
    int      shape;
    int      first_part;           // index of the particle in PhysWorld
    int      nparts;
    float    half;                 // sphere: radius; box: half-diagonal
    float    half_ext[3];          // box: half sizes (local axes)
    int      sleeping;             // at rest (for statistics/debugging)
} PhysBody;

// Static collider: sphere, vertical cylinder (x,z,r + base..top)
// or axis-aligned box (center c, half sizes e).
typedef struct {
    int kind;                      // 0 sphere, 1 cylinder, 2 box
    float c[3];
    float r;                       // sphere/cylinder
    float e[3];                    // box: half sizes
    float base, top;               // cylinder: bottom/top Y
} PhysStatic;

// Layer initialization (registry empty, default settings). Idempotent.
void phys_body_init(void);
// Layer switch (cvar phys_bodies): 0 — bodies don't move.
void phys_body_enable(int on);
int  phys_body_enabled(void);

// One fixed world step: integration+constraints (physics_step) plus
// "particle-particle" and "particle-static" collisions. Call from the main
// loop instead of physics_step for the global world.
void phys_body_step(PhysWorld *w, float dt);

// Body creation (pos — center; mass — kg; rest = starts asleep). -1 — limit/error.
int  phys_body_spawn_sphere(PhysWorld *w, const float pos[3], float radius,
                           float mass);
int  phys_body_spawn_box(PhysWorld *w, const float pos[3], const float ext[3],
                        float mass);
void phys_body_kill_all(PhysWorld *w);   // remove only bodies, ropes stay intact

const PhysBody *phys_body_get(int idx);
int  phys_body_count(void);
void phys_body_impulse(int idx, const float vel[3]);
// Body center (average of particles) — for rendering/debugging.
void phys_body_center(int idx, float out[3]);

// --- Static colliders ---------------------------------------------------
int  phys_static_add_sphere(const float c[3], float r);      // idx or -1
int  phys_static_add_cylinder(float x, float z, float r,
                              float base, float top);
int  phys_static_add_box(const float c[3], const float e[3]);
void phys_static_clear(void);
int  phys_static_count(void);
const PhysStatic *phys_static_get(int i);

// Push bodies away from a point (the player): bodies within pr+r receive
// an impulse along (dir) * speed. dir — the player's horizontal velocity, m/s.
void phys_body_push(const float pos[3], const float vel[3], float pr,
                    float speed_limit);

// Debug query: the body number a world particle belongs to (or -1).
// Body particles already live in physics_world() and are drawn by the shared
// point loop in renderer.c — this query exists to color them apart from ropes.
int  phys_body_index_of_part(int part_idx);

// Console commands (registered in cmdreg.c).
void cmd_phys_sphere(const char *args);
void cmd_phys_box(const char *args);
void cmd_phys_props(const char *args);
void cmd_phys_bodies(const char *args);

// Layer selftest (FR_TEST 'u'): the stack rests, impulse works,
// no passing through statics, overflow = -1. Number of failures.
int  phys_body_selftest(void);

#endif
