// ============================================================================
//  physics.h — mini rigid-body physics engine (Verlet) — foundation for ragdolls
// ----------------------------------------------------------------------------
//  Classic "particles + links" scheme (as in Hitman / Dzhopas Vermenis):
//    * particle: position + previous position (velocity = difference), mass;
//    * link (stick): two particles at a fixed distance — after N
//      relaxation iterations the particle network holds its shape (cloth, rope,
//      ragdoll skeleton);
//    * Verlet integration is stable at large steps and cheap (no derivatives);
//    * collisions: terrain (sample_height — same as the player), sphere-ground.
//
//  This is the CORE, no rendering: the renderer reads particle states (now —
//  debug points in renderer_end, later — ragdoll skeleton bones).
//
//  Console: phys_rope [length] — a rope 5 m in front of the camera (demo).
// ============================================================================
#ifndef PHYSICS_H
#define PHYSICS_H

#define PHYS_MAX_PARTS  512
#define PHYS_MAX_LINKS  2048

typedef struct {
    float pos[3];        // current position
    float prev[3];       // position at the previous step (Verlet velocity)
    float inv_mass;      // 1/mass (0 = pinned in place)
    float radius;        // radius for terrain collision (0 = point)
} PhysPart;

typedef struct {
    int a, b;            // particle indices
    float rest;          // rest length
} PhysLink;

typedef struct {
    PhysPart part[PHYS_MAX_PARTS];
    int np;
    PhysLink link[PHYS_MAX_LINKS];
    int nl;
    float gravity;       // m/s^2 (down)
    int iterations;      // link relaxation iterations (usually 4..8)
    float damping;       // 0..1: fraction of velocity lost per step (air)
    float dt;            // step of the last physics_step (for impulse units)
} PhysWorld;

// Reset the world to empty with default settings.
void physics_reset(PhysWorld *w);

// Global test world (rope/chain from the console; ragdoll foundation).
PhysWorld *physics_world(void);

// Terrain height callback (main.c: the game world's sample_height).
void physics_set_ground(float (*fn)(float x, float z));

// Simulation step (call with a fixed dt, 1/100 s — like the game logic).
void physics_step(PhysWorld *w, float dt);

// Only link relaxation (no integration): external constraints (joint
// limits) moved the particles — links are tightened alternately with them.
void physics_relax(PhysWorld *w, int iterations);

// Particles and links. Returns the index (-1 on overflow).
int physics_add_part(PhysWorld *w, const float pos[3], float inv_mass,
                     float radius);
int physics_add_link(PhysWorld *w, int a, int b);

// Pin/unpin a particle (pin = 1 — pinned).
void physics_pin(PhysWorld *w, int idx, int pin);

// Impulse on a particle (instant velocity, m/s).
void physics_impulse(PhysWorld *w, int idx, const float vel[3]);

#endif
