// ============================================================================
//  physics.c — mini body physics engine (Verlet), see physics.h
// ----------------------------------------------------------------------------
//  Render-free core: particles + constraints, Verlet integration, constraint
//  relaxation, terrain collision through a callback (main.c supplies
//  sample_height). The test world is a single global one (rope/chain from
//  the console); it exercises the foundation for the future ragdoll
//  (particles = skeleton bones).
// ============================================================================
#include "physics.h"

#include <math.h>
#include <string.h>

// Terrain height callback (set by main.c: the game world's sample_height)
static float (*s_ground)(float x, float z) = NULL;

void physics_set_ground(float (*fn)(float x, float z))
{
    s_ground = fn;
}

PhysWorld *physics_world(void)
{
    static PhysWorld w;
    return &w;
}

void physics_reset(PhysWorld *w)
{
    memset(w, 0, sizeof *w);
    w->gravity = 9.8f;
    w->iterations = 6;
    w->damping = 0.005f;
}

int physics_add_part(PhysWorld *w, const float pos[3], float inv_mass,
                     float radius)
{
    if (w->np >= PHYS_MAX_PARTS) return -1;
    PhysPart *p = &w->part[w->np];
    memset(p, 0, sizeof *p);
    p->pos[0] = p->prev[0] = pos[0];
    p->pos[1] = p->prev[1] = pos[1];
    p->pos[2] = p->prev[2] = pos[2];
    p->inv_mass = inv_mass;
    p->radius = radius;
    return w->np++;
}

int physics_add_link(PhysWorld *w, int a, int b)
{
    if (w->nl >= PHYS_MAX_LINKS || a < 0 || b < 0 || a >= w->np || b >= w->np)
        return -1;
    PhysPart *pa = &w->part[a], *pb = &w->part[b];
    float dx = pb->pos[0] - pa->pos[0];
    float dy = pb->pos[1] - pa->pos[1];
    float dz = pb->pos[2] - pa->pos[2];
    w->link[w->nl].a = a;
    w->link[w->nl].b = b;
    w->link[w->nl].rest = sqrtf(dx * dx + dy * dy + dz * dz);
    return w->nl++;
}

void physics_pin(PhysWorld *w, int idx, int pin)
{
    if (idx < 0 || idx >= w->np) return;
    w->part[idx].inv_mass = pin ? 0.0f : 1.0f;
}

void physics_impulse(PhysWorld *w, int idx, const float vel[3])
{
    if (idx < 0 || idx >= w->np) return;
    PhysPart *p = &w->part[idx];
    if (p->inv_mass <= 0.0f) return;
    // Verlet: velocity = pos - prev (DISPLACEMENT PER STEP). The impulse is
    // given in m/s — convert it to a per-step displacement: dprev = vel * dt.
    // Without this a 6 m/s shove became 6 m/STEP = 600 m/s and the model
    // flew off into the stratosphere.
    float s = w->dt > 1e-6f ? w->dt : (1.0f / 100.0f);
    p->prev[0] -= vel[0] * s;
    p->prev[1] -= vel[1] * s;
    p->prev[2] -= vel[2] * s;
}

// Relax one constraint: particles slide together/apart toward the rest
// length, proportionally to their inverse masses.
static void link_relax(PhysWorld *w, const PhysLink *l)
{
    PhysPart *a = &w->part[l->a], *b = &w->part[l->b];
    float wsum = a->inv_mass + b->inv_mass;
    if (wsum <= 0.0f) return;
    float dx = b->pos[0] - a->pos[0];
    float dy = b->pos[1] - a->pos[1];
    float dz = b->pos[2] - a->pos[2];
    float d = sqrtf(dx * dx + dy * dy + dz * dz);
    if (d < 1e-6f) return;
    float diff = (d - l->rest) / d;
    float ka = a->inv_mass / wsum, kb = b->inv_mass / wsum;
    a->pos[0] += dx * diff * ka;
    a->pos[1] += dy * diff * ka;
    a->pos[2] += dz * diff * ka;
    b->pos[0] -= dx * diff * kb;
    b->pos[1] -= dy * diff * kb;
    b->pos[2] -= dz * diff * kb;
}

void physics_relax(PhysWorld *w, int iterations)
{
    for (int it = 0; it < iterations; it++)
        for (int i = 0; i < w->nl; i++)
            link_relax(w, &w->link[i]);
}

void physics_step(PhysWorld *w, float dt)
{
    if (w->np <= 0) return;
    w->dt = dt;
    float g = w->gravity * dt * dt;

    // 1) Verlet integration: pos += (pos - prev) * (1 - damping) + g
    for (int i = 0; i < w->np; i++) {
        PhysPart *p = &w->part[i];
        if (p->inv_mass <= 0.0f) continue;
        float nx = p->pos[0] + (p->pos[0] - p->prev[0]) * (1.0f - w->damping);
        float ny = p->pos[1] + (p->pos[1] - p->prev[1]) * (1.0f - w->damping)
                 - g;
        float nz = p->pos[2] + (p->pos[2] - p->prev[2]) * (1.0f - w->damping);
        p->prev[0] = p->pos[0];
        p->prev[1] = p->pos[1];
        p->prev[2] = p->pos[2];
        p->pos[0] = nx;
        p->pos[1] = ny;
        p->pos[2] = nz;
    }

    // 2) Constraint relaxation + collisions (several iterations: the mesh settles)
    for (int it = 0; it < w->iterations; it++) {
        for (int i = 0; i < w->nl; i++)
            link_relax(w, &w->link[i]);
        // Terrain collision: a particle never below ground + radius
        if (s_ground) {
            for (int i = 0; i < w->np; i++) {
                PhysPart *p = &w->part[i];
                if (p->inv_mass <= 0.0f || p->radius <= 0.0f) continue;
                float h = s_ground(p->pos[0], p->pos[2]) + p->radius;
                if (p->pos[1] < h) {
                    // friction: damp the horizontal slide on contact
                    p->pos[1] = h;
                    p->prev[0] += (p->pos[0] - p->prev[0]) * 0.5f;
                    p->prev[2] += (p->pos[2] - p->prev[2]) * 0.5f;
                }
            }
        }
    }
}
