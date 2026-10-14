// ============================================================================
//  phys_body.c — rigid body implementation (see phys_body.h)
// ----------------------------------------------------------------------------
//  Bodies are a layer on top of the same Verlet world: particles and links are
//  ordinary; this layer adds ONLY what the core lacks: «particle-particle» and
//  «particle-static» collisions (otherwise a crate passes through a crate) and
//  grouping particles into bodies. No allocations at all: every pool is static.
// ============================================================================
#include "phys_body.h"

#include "console.h"
#include "cvars.h"
#include "game.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PART_TAG_NONE (-1)

static PhysBody     s_bodies[PHYS_BODY_MAX];
static int          s_nbody = 0;
static PhysStatic   s_static[PHYS_STATIC_MAX];
static int          s_nstatic = 0;
static int          s_tag[PHYS_MAX_PARTS];    // part -> body (or NONE)
static float        s_vel0[PHYS_MAX_PARTS][3]; // velocity before the collision pass
static int          s_enabled = 0;
static int          s_inited = 0;

// restitution: the share of normal velocity preserved on impact. 0 = sticky
// (the crate does not bounce), 0.35 = lively rubber. Tuned by the phys_bounce cvar.
static float bounce_get(void)
{
    float v = cvar_get("phys_bounce");
    if (v < 0.0f) v = 0.0f;
    if (v > 0.9f) v = 0.9f;
    return v;
}

// --- Tags ------------------------------------------------------------------------
static void rebuild_tags(PhysWorld *w)
{
    for (int i = 0; i < w->np; i++) s_tag[i] = PART_TAG_NONE;
    for (int b = 0; b < s_nbody; b++) {
        const PhysBody *bd = &s_bodies[b];
        for (int k = 0; k < bd->nparts; k++) {
            int pi = bd->first_part + k;
            if (pi >= 0 && pi < w->np) s_tag[pi] = b;
        }
    }
}

static void body_sort(PhysWorld *w)
{
    // Bodies are kept in first_part order; the «gaps» between them hold ropes only.
    for (int i = 1; i < s_nbody; i++) {
        PhysBody t = s_bodies[i];
        int j = i - 1;
        while (j >= 0 && s_bodies[j].first_part > t.first_part) {
            s_bodies[j + 1] = s_bodies[j];
            j--;
        }
        s_bodies[j + 1] = t;
    }
    rebuild_tags(w);
}

// --- Adding parts/links to the world ------------------------------------------------
static int add_link_clamped(PhysWorld *w, int a, int b)
{
    if (w->nl >= PHYS_MAX_LINKS) return -1;
    return physics_add_link(w, a, b);
}

int phys_body_spawn_sphere(PhysWorld *w, const float pos[3], float radius,
                          float mass)
{
    if (!s_inited || !w) return -1;
    if (s_nbody >= PHYS_BODY_MAX) {
        console_print("phys_body: лимит тел (%d)", PHYS_BODY_MAX);
        return -1;
    }
    if (w->np >= PHYS_MAX_PARTS) {
        console_print("phys_body: лимит частиц (%d)", PHYS_MAX_PARTS);
        return -1;
    }
    if (!(radius > 0.01f) || !(mass > 0.0f)) {
        console_print("phys_body: сфера: radius/mass должны быть > 0");
        return -1;
    }
    int idx = physics_add_part(w, pos, 1.0f / mass, radius);
    if (idx < 0) return -1;
    s_bodies[s_nbody] = (PhysBody){
        PHYS_SPHERE, idx, 1, radius, { radius, radius, radius }, 0
    };
    s_nbody++;
    body_sort(w);
    return s_nbody - 1;
}

// Rigid frame: 8 corners, 12 edges, 4 face diagonals + 4 «crosses» —
// the links hold the shape (following the ragdoll pattern: parent+grandparent).
int phys_body_spawn_box(PhysWorld *w, const float pos[3], const float ext[3],
                       float mass)
{
    if (!s_inited || !w) return -1;
    if (s_nbody >= PHYS_BODY_MAX) {
        console_print("phys_body: лимит тел (%d)", PHYS_BODY_MAX);
        return -1;
    }
    if (w->np + 8 > PHYS_MAX_PARTS) {
        console_print("phys_body: лимит частиц для бокса (%d)", PHYS_MAX_PARTS);
        return -1;
    }
    float hx = ext[0], hy = ext[1], hz = ext[2];
    if (!(hx > 0.01f) || !(hy > 0.01f) || !(hz > 0.01f) || !(mass > 0.0f)) {
        console_print("phys_body: бокс: размеры/mass должны быть > 0");
        return -1;
    }
    const float sx[2] = { -hx, hx }, sy[2] = { -hy, hy }, sz[2] = { -hz, hz };
    int first = w->np;
    int corner[8];
    float diag = sqrtf(hx * hx + hy * hy + hz * hz);
    for (int i = 0; i < 8; i++) {
        float p[3] = { pos[0] + sx[(i >> 2) & 1],
                       pos[1] + sy[(i >> 1) & 1],
                       pos[2] + sz[i & 1] };
        corner[i] = physics_add_part(w, p, 1.0f / (mass / 8.0f),
                                     diag * 0.25f);
        // radius = quarter of the diagonal: corners do not «dig into» each other
        // (a body's own particles don't collide — see s_tag), but the body does
        // not poke through other bodies' planes.
        if (corner[i] < 0) {          // partially created body — roll back
            w->np = first;
            return -1;
        }
    }
    static const int edges[12][2] = {
        {0,1},{0,2},{0,4},{3,1},{3,2},{3,7},{6,1},{6,2},{6,7},{5,4},{5,7},{5,1}
    };
    static const int face_diag[8][2] = {
        {0,7},{1,6},{2,5},{4,3},{0,3},{1,2},{4,5},{6,7}
    };
    for (int i = 0; i < 12; i++) add_link_clamped(w, corner[edges[i][0]],
                                                  corner[edges[i][1]]);
    for (int i = 0; i < 8; i++) add_link_clamped(w, corner[face_diag[i][0]],
                                                 corner[face_diag[i][1]]);
    s_bodies[s_nbody] = (PhysBody){
        PHYS_BOX, first, 8, diag, { hx, hy, hz }, 0
    };
    s_nbody++;
    body_sort(w);
    return s_nbody - 1;
}

// Remove bodies while keeping ropes: compact — shift the world array tails.
static void remove_range(PhysWorld *w, int from, int count)
{
    if (count <= 0 || from < 0 || from + count > w->np) return;
    int remap[PHYS_MAX_PARTS];
    for (int i = 0; i < w->np; i++) remap[i] = i;
    for (int i = from + count; i < w->np; i++) remap[i] = i - count;
    memmove(&w->part[from], &w->part[from + count],
            (size_t)(w->np - from - count) * sizeof(PhysPart));
    w->np -= count;
    int nw = 0;
    for (int i = 0; i < w->nl; i++) {
        int a = w->link[i].a, b = w->link[i].b;
        if (a >= from && a < from + count) continue;
        if (b >= from && b < from + count) continue;
        w->link[nw].a = remap[a];
        w->link[nw].b = remap[b];
        w->link[nw].rest = w->link[i].rest;
        nw++;
    }
    w->nl = nw;
}

void phys_body_kill_all(PhysWorld *w)
{
    if (!w) return;
    // Bodies are ordered by first_part. Remove from start to end, each time
    // shifting the indices of the REMAINING bodies by the size of the removed
    // range: the gaps between bodies (ropes) stay in place relative to the
    // surviving particles.
    int removed = 0;
    for (int b = 0; b < s_nbody; b++) {
        remove_range(w, s_bodies[b].first_part - removed, s_bodies[b].nparts);
        removed += s_bodies[b].nparts;
    }
    s_nbody = 0;
    rebuild_tags(w);
}

const PhysBody *phys_body_get(int idx)
{
    if (idx < 0 || idx >= s_nbody) return NULL;
    return &s_bodies[idx];
}
int phys_body_count(void) { return s_nbody; }

void phys_body_center(int idx, float out[3])
{
    out[0] = out[1] = out[2] = 0.0f;
    PhysWorld *w = physics_world();
    const PhysBody *b = phys_body_get(idx);
    if (!b || !w) return;
    for (int k = 0; k < b->nparts; k++) {
        const PhysPart *p = &w->part[b->first_part + k];
        out[0] += p->pos[0]; out[1] += p->pos[1]; out[2] += p->pos[2];
    }
    out[0] /= b->nparts; out[1] /= b->nparts; out[2] /= b->nparts;
}

void phys_body_impulse(int idx, const float vel[3])
{
    const PhysBody *b = phys_body_get(idx);
    if (!b) return;
    PhysWorld *w = physics_world();
    float s = w->dt > 1e-6f ? w->dt : (1.0f / 100.0f);
    for (int k = 0; k < b->nparts; k++) {
        PhysPart *p = &w->part[b->first_part + k];
        if (p->inv_mass <= 0.0f) continue;
        p->prev[0] -= vel[0] * s;
        p->prev[1] -= vel[1] * s;
        p->prev[2] -= vel[2] * s;
    }
}

// --- Statics --------------------------------------------------------------------
static int static_add(PhysStatic s)
{
    if (!s_inited) return -1;
    if (s_nstatic >= PHYS_STATIC_MAX) {
        console_print("phys_body: лимит статических коллайдеров (%d)",
                      PHYS_STATIC_MAX);
        return -1;
    }
    s_static[s_nstatic] = s;
    return s_nstatic++;
}

int phys_static_add_sphere(const float c[3], float r)
{
    if (!(r > 0.01f)) return -1;
    PhysStatic s;
    memset(&s, 0, sizeof s);
    s.kind = 0;
    s.c[0] = c[0]; s.c[1] = c[1]; s.c[2] = c[2];
    s.r = r;
    return static_add(s);
}

int phys_static_add_cylinder(float x, float z, float r, float base, float top)
{
    if (!(r > 0.01f) || !(top > base)) return -1;
    PhysStatic s;
    memset(&s, 0, sizeof s);
    s.kind = 1;
    s.c[0] = x; s.c[2] = z;
    s.r = r; s.base = base; s.top = top;
    return static_add(s);
}

int phys_static_add_box(const float c[3], const float e[3])
{
    if (!(e[0] > 0.01f) || !(e[1] > 0.01f) || !(e[2] > 0.01f)) return -1;
    PhysStatic s;
    memset(&s, 0, sizeof s);
    s.kind = 2;
    s.c[0] = c[0]; s.c[1] = c[1]; s.c[2] = c[2];
    s.e[0] = e[0]; s.e[1] = e[1]; s.e[2] = e[2];
    return static_add(s);
}

void phys_static_clear(void) { s_nstatic = 0; }
int  phys_static_count(void) { return s_nstatic; }
const PhysStatic *phys_static_get(int i)
{
    if (i < 0 || i >= s_nstatic) return NULL;
    return &s_static[i];
}

// --- Collisions ----------------------------------------------------------------------
// Push a particle out of a static primitive. Returns 1 if it was moved.
static int collide_static(PhysPart *p, const PhysStatic *s)
{
    float r = p->radius;
    if (r <= 0.0f) return 0;
    int moved = 0;
    if (s->kind == 0) {                    // sphere
        float dx = p->pos[0] - s->c[0];
        float dy = p->pos[1] - s->c[1];
        float dz = p->pos[2] - s->c[2];
        float rr = s->r + r;
        float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 >= rr * rr) return 0;
        float d = sqrtf(d2);
        if (d < 1e-5f) { p->pos[1] = s->c[1] + rr; return 1; }
        float k = (rr - d) / d;
        p->pos[0] += dx * k; p->pos[1] += dy * k; p->pos[2] += dz * k;
        moved = 1;
    } else if (s->kind == 1) {             // vertical cylinder
        if (p->pos[1] + r < s->base || p->pos[1] - r > s->top) return 0;
        float dx = p->pos[0] - s->c[0];
        float dz = p->pos[2] - s->c[2];
        float rr = s->r + r;
        float d2 = dx * dx + dz * dz;
        if (d2 >= rr * rr) return 0;
        // There is a Y overlap — push out horizontally OR vertically
        // (whichever is closer): otherwise the particle «sticks» in the cylinder lid.
        float dxy = d2 < 1e-6f ? 0.0f : sqrtf(d2);
        float pen_xy = rr - dxy;
        float pen_up = (p->pos[1] + r) - s->base;     // downward from above
        float pen_dn = s->top - (p->pos[1] - r);      // upward from below
        float vpen = pen_up < pen_dn ? pen_up : pen_dn;
        if (pen_xy <= vpen) {
            float k = dxy > 1e-5f ? pen_xy / dxy : 1.0f;
            if (dxy <= 1e-5f) { p->pos[0] += rr; }
            else { p->pos[0] += dx * k; p->pos[2] += dz * k; }
        } else if (pen_up <= pen_dn) p->pos[1] += pen_up;
        else p->pos[1] -= pen_dn;
        moved = 1;
    } else {                               // box
        float dx = p->pos[0] - s->c[0];
        float dy = p->pos[1] - s->c[1];
        float dz = p->pos[2] - s->c[2];
        float ex = s->e[0] + r, ey = s->e[1] + r, ez = s->e[2] + r;
        if (fabsf(dx) >= ex || fabsf(dy) >= ey || fabsf(dz) >= ez) return 0;
        float px = ex - fabsf(dx), py = ey - fabsf(dy), pz = ez - fabsf(dz);
        if (px <= py && px <= pz) p->pos[0] += (dx < 0.0f ? -px : px);
        else if (py <= pz)        p->pos[1] += (dy < 0.0f ? -py : py);
        else                      p->pos[2] += (dz < 0.0f ? -pz : pz);
        moved = 1;
    }
    return moved;
}

// Verlet velocity = pos - prev (the step displacement). Positional separation
// consumes it entirely (elasticity 0); we restore the bounce share along the normal.
static void save_vel(PhysWorld *w)
{
    for (int i = 0; i < w->np; i++) {
        s_vel0[i][0] = w->part[i].pos[0] - w->part[i].prev[0];
        s_vel0[i][1] = w->part[i].pos[1] - w->part[i].prev[1];
        s_vel0[i][2] = w->part[i].pos[2] - w->part[i].prev[2];
    }
}

static void collide_parts(PhysWorld *w, float bounce)
{
    for (int i = 0; i < w->np; i++) {
        PhysPart *a = &w->part[i];
        if (a->radius <= 0.0f) continue;
        for (int j = i + 1; j < w->np; j++) {
            PhysPart *b = &w->part[j];
            if (b->radius <= 0.0f) continue;
            if (s_tag[i] != PART_TAG_NONE && s_tag[i] == s_tag[j]) continue;
            float dx = b->pos[0] - a->pos[0];
            float dy = b->pos[1] - a->pos[1];
            float dz = b->pos[2] - a->pos[2];
            float rr = a->radius + b->radius;
            float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 >= rr * rr) continue;
            float wsum = a->inv_mass + b->inv_mass;
            if (wsum <= 0.0f) continue;
            float d = sqrtf(d2);
            if (d < 1e-5f) {          // centers coincide — separate along Y
                if (a->inv_mass > 0.0f) a->pos[1] -= rr * (a->inv_mass / wsum);
                if (b->inv_mass > 0.0f) b->pos[1] += rr * (b->inv_mass / wsum);
                continue;
            }
            float k = (rr - d) / d;
            float ka = a->inv_mass / wsum, kb = b->inv_mass / wsum;
            a->pos[0] -= dx * k * ka; a->pos[1] -= dy * k * ka;
            a->pos[2] -= dz * k * ka;
            b->pos[0] += dx * k * kb; b->pos[1] += dy * k * kb;
            b->pos[2] += dz * k * kb;
            // Bounce: restore the share of normal velocity eaten by the
            // positional separation. Relative velocity along the normal
            // (a->b) being negative = bodies approaching; a resting stack
            // gets no correction.
            if (bounce > 0.0f) {
                float nx = dx / d, ny = dy / d, nz = dz / d;
                float vax = s_vel0[i][0], vay = s_vel0[i][1], vaz = s_vel0[i][2];
                float vbx = s_vel0[j][0], vby = s_vel0[j][1], vbz = s_vel0[j][2];
                float vrel = (vbx - vax) * nx + (vby - vay) * ny + (vbz - vaz) * nz;
                if (vrel < -1e-5f) {
                    // prev -= n*corr*a: velocity (pos-prev) INCREASES along +n,
                    // i.e. the bodies fly apart faster after the separation.
                    float corr = -bounce * vrel;
                    w->part[i].prev[0] -= nx * corr * ka;
                    w->part[i].prev[1] -= ny * corr * ka;
                    w->part[i].prev[2] -= nz * corr * ka;
                    w->part[j].prev[0] += nx * corr * kb;
                    w->part[j].prev[1] += ny * corr * kb;
                    w->part[j].prev[2] += nz * corr * kb;
                }
            }
        }
    }
}

static void collide_statics(PhysWorld *w)
{
    for (int i = 0; i < w->np; i++) {
        PhysPart *p = &w->part[i];
        if (p->inv_mass <= 0.0f || p->radius <= 0.0f) continue;
        for (int s = 0; s < s_nstatic; s++)
            collide_static(p, &s_static[s]);
    }
}

// Sleep detection: every particle of the body moved less than 1 mm during the step.
static void update_sleep(PhysWorld *w, float dt)
{
    (void)dt;
    const float eps = 0.001f;
    for (int b = 0; b < s_nbody; b++) {
        const PhysBody *bd = &s_bodies[b];
        int still = 1;
        for (int k = 0; k < bd->nparts; k++) {
            const PhysPart *p = &w->part[bd->first_part + k];
            float vx = p->pos[0] - p->prev[0];
            float vy = p->pos[1] - p->prev[1];
            float vz = p->pos[2] - p->prev[2];
            if (vx * vx + vy * vy + vz * vz > eps * eps) { still = 0; break; }
        }
        ((PhysBody *)bd)->sleeping = still;
    }
}

void phys_body_step(PhysWorld *w, float dt)
{
    if (!s_inited || !w) { physics_step(w, dt); return; }
    // The kill switch turns bodies OFF completely (they stay where they stopped),
    // but the base Verlet keeps running: ropes and ragdolls are not «bodies»,
    // they always move. Trick: temporarily zero out body particles (inv_mass=0),
    // step the world, then restore.
    if (!s_enabled) {
        static float saved[PHYS_BODY_MAX * 8];
        int n = 0;
        for (int b = 0; b < s_nbody; b++) {
            const PhysBody *bd = &s_bodies[b];
            for (int k = 0; k < bd->nparts && n < (int)(sizeof saved /
                 sizeof saved[0]); k++) {
                int pi = bd->first_part + k;
                if (pi < 0 || pi >= w->np) continue;
                saved[n++] = w->part[pi].inv_mass;
                w->part[pi].inv_mass = 0.0f;
            }
        }
        physics_step(w, dt);
        n = 0;
        for (int b = 0; b < s_nbody; b++) {
            const PhysBody *bd = &s_bodies[b];
            for (int k = 0; k < bd->nparts && n < (int)(sizeof saved /
                 sizeof saved[0]); k++) {
                int pi = bd->first_part + k;
                if (pi < 0 || pi >= w->np) continue;
                w->part[pi].inv_mass = saved[n++];
            }
        }
        return;
    }
    float bounce = bounce_get();
    int its = w->iterations > 0 ? (w->iterations < 4 ? w->iterations : 4) : 4;
    rebuild_tags(w);
    physics_step(w, dt);                 // integration + links + terrain
    save_vel(w);                         // velocity BEFORE the collision pass
    for (int it = 0; it < its; it++) {
        collide_statics(w);
        collide_parts(w, bounce);
    }
    update_sleep(w, dt);
}

// --- Pushing by the player ------------------------------------------------------------------
void phys_body_push(const float pos[3], const float vel[3], float pr,
                    float speed_limit)
{
    if (!s_inited || !s_enabled) return;
    PhysWorld *w = physics_world();
    float sp = sqrtf(vel[0] * vel[0] + vel[2] * vel[2]);
    if (sp < 0.5f) return;                      // standing still — no push
    if (sp > speed_limit) sp = speed_limit;
    float dirx = vel[0], dirz = vel[2];
    float dl = sqrtf(dirx * dirx + dirz * dirz);
    if (dl < 1e-6f) return;
    dirx /= dl; dirz /= dl;
    for (int b = 0; b < s_nbody; b++) {
        const PhysBody *bd = &s_bodies[b];
        for (int k = 0; k < bd->nparts; k++) {
            int pi = bd->first_part + k;
            if (pi < 0 || pi >= w->np) continue;
            PhysPart *p = &w->part[pi];
            if (p->inv_mass <= 0.0f) continue;
            float dx = p->pos[0] - pos[0];
            float dy = p->pos[1] - pos[1];
            float dz = p->pos[2] - pos[2];
            if (fabsf(dy) > 1.8f) continue;      // player's waist band only
            float rr = pr + p->radius;
            if (dx * dx + dz * dz >= rr * rr) continue;
            float s = w->dt > 1e-6f ? w->dt : (1.0f / 100.0f);
            p->prev[0] -= dirx * sp * s;
            p->prev[2] -= dirz * sp * s;
            p->prev[1] -= 0.25f * sp * s;        // lift slightly: getting down off a step
        }
    }
}

// --- Debug ------------------------------------------------------------------------------
int phys_body_index_of_part(int part_idx)
{
    // A body's particles lie consecutively (the first one is created earlier,
    // indices grow), so iterating over the ranges is enough.
    for (int b = 0; b < s_nbody; b++) {
        const PhysBody *bd = &s_bodies[b];
        if (part_idx >= bd->first_part &&
            part_idx < bd->first_part + bd->nparts) return b;
    }
    return -1;
}

// --- Initialization / kill switch ---------------------------------------------------------
void phys_body_init(void)
{
    if (s_inited) return;
    s_inited = 1;
    PhysWorld *w = physics_world();
    if (w->gravity <= 0.0f) physics_reset(w);
    cvar_register_ex("phys_bodies", 0.0f, 0.0f, 1.0f, CVAR_ARCHIVE,
                     "твёрдые тела (ящики/сферы) в Verlet-мире");
    cvar_register_ex("phys_bounce", 0.25f, 0.0f, 0.9f, CVAR_ARCHIVE,
                     "упругость тел при ударе (0=липко, 0.9=резина)");
    cvar_register_ex("phys_push_speed", 4.0f, 0.0f, 12.0f, CVAR_ARCHIVE,
                     "макс скорость, с которой игрок толкает тела, м/с");
}

void phys_body_enable(int on) { s_enabled = on ? 1 : 0; }
int  phys_body_enabled(void)  { return s_enabled; }

// --- Commands --------------------------------------------------------------------------------
// A body is placed IN FRONT OF the camera along its view direction, not along world +Z:
// otherwise the «stack at the camera» ends up behind your back as soon as the player
// turns their head.
// off = { right, up, forward } in meters.
static void cam_place(const Camera *cam, const float off[3], float out[3])
{
    float fx = sinf(cam->yaw), fz = cosf(cam->yaw);   // horizontal «forward»
    out[0] = cam->pos[0] + off[2] * fx - off[0] * fz;
    out[1] = cam->pos[1] + off[1];
    out[2] = cam->pos[2] + off[2] * fz + off[0] * fx;
}

static void spawn_at_cam(const float off[3], int box, float size)
{
    Game *g = game_instance();
    if (!g || !g->cam) { console_print("phys: нет камеры"); return; }
    float pos[3];
    cam_place(&g->cam->cam, off, pos);
    pos[1] += size;
    PhysWorld *w = physics_world();
    int idx;
    if (box) {
        float ext[3] = { size * 0.5f, size * 0.5f, size * 0.5f };
        idx = phys_body_spawn_box(w, pos, ext, size * size * size * 80.0f);
    } else {
        idx = phys_body_spawn_sphere(w, pos, size * 0.5f,
                                     size * size * size * 80.0f);
    }
    if (idx >= 0) console_print("phys: %s #%d у камеры",
                                box ? "box" : "sphere", idx);
    phys_body_enable(1);
}

void cmd_phys_sphere(const char *args)
{
    float r = args && *args ? (float)atof(args) : 0.5f;
    if (!(r > 0.05f)) r = 0.05f;
    if (r > 4.0f) r = 4.0f;
    const float off[3] = { 0.0f, 1.0f, 3.0f };
    spawn_at_cam(off, 0, r * 2.0f);
}

void cmd_phys_box(const char *args)
{
    float s = args && *args ? (float)atof(args) : 0.8f;
    if (!(s > 0.1f)) s = 0.1f;
    if (s > 4.0f) s = 4.0f;
    const float off[3] = { 0.0f, 1.0f, 3.0f };
    spawn_at_cam(off, 1, s);
}

void cmd_phys_props(const char *args)
{
    int n = args && *args ? atoi(args) : 8;
    if (n < 1) n = 1;
    if (n > 32) n = 32;
    Game *g = game_instance();
    if (!g || !g->cam) { console_print("phys: нет камеры"); return; }
    phys_body_enable(1);
    PhysWorld *w = physics_world();
    const Camera *cam = &g->cam->cam;
    int made = 0;
    for (int i = 0; i < n; i++) {
        float size = 0.7f;
        float ext[3] = { size * 0.5f, size * 0.5f, size * 0.5f };
        float off[3] = { (float)((i % 4) - 1) * 1.2f,
                         1.0f + (float)(i / 4) * 0.9f, 3.0f };
        float pos[3];
        cam_place(cam, off, pos);
        if (phys_body_spawn_box(w, pos, ext, size * size * size * 80.0f) >= 0)
            made++;
    }
    console_print("phys: %d ящиков (стопка), тел всего %d", made,
                  phys_body_count());
    // Statics come from map objects: without them crates fall «through the world».
    SDL_Log("phys: props %d, тел %d, статика %d", made, phys_body_count(),
            phys_static_count());
}

void cmd_phys_bodies(const char *args)
{
    (void)args;
    console_print("phys_bodies: %d (вкл=%d), статика %d",
                  phys_body_count(), phys_body_enabled(), s_nstatic);
    for (int i = 0; i < phys_body_count(); i++) {
        const PhysBody *b = phys_body_get(i);
        float c[3];
        phys_body_center(i, c);
        console_print("  #%d %s parts %d c=(%.1f, %.1f, %.1f) %s",
                      i, b->shape == PHYS_BOX ? "box" : "sphere", b->nparts,
                      c[0], c[1], c[2], b->sleeping ? "sleep" : "move");
    }
}

// --- Self-check ---------------------------------------------------------------------------
static int st_fail;
static void st_check(int cond, const char *what)
{
    if (!cond) { st_fail++; SDL_Log("phys_body selftest FAIL: %s", what); }
}

int phys_body_selftest(void)
{
    st_fail = 0;
    phys_body_init();
    PhysWorld *w = physics_world();
    physics_reset(w);
    phys_body_kill_all(w);
    phys_static_clear();
    phys_body_enable(1);

    // Floor — a static 20x1x20 box at y=-0.5..0.5, bodies are placed above it.
    float bc[3] = { 0.0f, -0.5f, 0.0f }, be[3] = { 10.0f, 0.5f, 10.0f };
    st_check(phys_static_add_box(bc, be) >= 0, "static box add");

    // 1) A sphere falls onto statics and stays on it (does not sink through).
    float p0[3] = { 0.0f, 3.0f, 0.0f };
    int a = phys_body_spawn_sphere(w, p0, 0.5f, 2.0f);
    st_check(a >= 0 && w->np == 1, "sphere spawn");
    for (int i = 0; i < 200; i++) phys_body_step(w, 1.0f / 100.0f);
    float c[3];
    phys_body_center(a, c);
    st_check(c[1] > 0.2f && c[1] < 1.6f, "sphere rests on static box");
    st_check(fabsf(c[0]) < 0.5f, "sphere stays over box");

    // 2) Stack: a second sphere on the first — both stay above the floor.
    float p1[3] = { 0.0f, 5.0f, 0.0f };
    int b = phys_body_spawn_sphere(w, p1, 0.5f, 2.0f);
    st_check(b >= 0, "second sphere spawn");
    for (int i = 0; i < 400; i++) phys_body_step(w, 1.0f / 100.0f);
    phys_body_center(a, c);
    float c2[3];
    phys_body_center(b, c2);
    st_check(c[1] > 0.4f && c2[1] > c[1], "stack: both above floor, top higher");

    // 3) An impulse moves the body (and does not launch it into the stratosphere).
    float before[3];
    phys_body_center(a, before);
    float imp[3] = { 5.0f, 0.0f, 0.0f };
    phys_body_impulse(a, imp);
    for (int i = 0; i < 60; i++) phys_body_step(w, 1.0f / 100.0f);
    phys_body_center(a, c);
    st_check(c[0] - before[0] > 0.1f, "impulse moves body");
    st_check(c[0] - before[0] < 20.0f, "impulse bounded (no 600 m/s)");

    // 4) Box: 8 particles + links, rests on statics.
    float bp[3] = { 4.0f, 3.0f, 0.0f };
    float ext[3] = { 0.4f, 0.4f, 0.4f };
    int np0 = w->np;
    int box = phys_body_spawn_box(w, bp, ext, 5.0f);
    st_check(box >= 0, "box spawn");
    st_check(w->np == np0 + 8, "box added 8 parts");
    for (int i = 0; i < 400; i++) phys_body_step(w, 1.0f / 100.0f);
    phys_body_center(box, c);
    st_check(c[1] > 0.3f && c[1] < 2.0f, "box rests on static");
    st_check(c[2] < 1.0f, "box did not tunnel through static");

    // 5) Cylinder collider: a particle does not pass through it along XZ.
    phys_static_clear();
    st_check(phys_static_add_cylinder(0.0f, 0.0f, 1.0f, -1.0f, 5.0f) >= 0,
             "cylinder add");
    physics_reset(w);
    phys_body_kill_all(w);
    float sp[3] = { -3.0f, 1.0f, 0.0f };
    int s = phys_body_spawn_sphere(w, sp, 0.3f, 1.0f);
    float hv[3] = { 3.0f, 0.0f, 0.0f };
    phys_body_impulse(s, hv);
    for (int i = 0; i < 200; i++) phys_body_step(w, 1.0f / 100.0f);
    phys_body_center(s, c);
    st_check(c[0] < -0.2f || c[0] > 1.4f, "sphere stopped/bounced at cylinder");
    st_check(c[1] < 3.0f, "sphere not launched into sky");

    // 6) Overflow: body limit = -1 + message, no crash.
    physics_reset(w);
    phys_body_kill_all(w);
    int made = 0;
    float pp[3] = { 0.0f, 1.0f, 0.0f };
    for (int i = 0; i < PHYS_BODY_MAX + 4; i++)
        if (phys_body_spawn_sphere(w, pp, 0.3f, 1.0f) >= 0) made++;
    st_check(made == PHYS_BODY_MAX, "body limit respected");

    // 7) Kill switch: the disabled layer does not move bodies.
    phys_body_enable(0);
    phys_body_center(0, c);
    for (int i = 0; i < 50; i++) phys_body_step(w, 1.0f / 100.0f);
    float c_after[3];
    phys_body_center(0, c_after);
    st_check(c_after[0] == c[0] && c_after[1] == c[1], "disable freezes bodies");

    // 8) Push from a point (simulating the player): a body within the waist band and
    // radius moves along the pusher's velocity; a distant one stays put.
    physics_reset(w);
    phys_body_kill_all(w);
    phys_body_enable(1);
    float near_p[3] = { 0.0f, 1.0f, 0.0f };
    float far_p[3]  = { 20.0f, 1.0f, 0.0f };
    int bn = phys_body_spawn_sphere(w, near_p, 0.4f, 2.0f);
    int bf = phys_body_spawn_sphere(w, far_p, 0.4f, 2.0f);
    float b0[3], f0[3];
    phys_body_center(bn, b0);
    phys_body_center(bf, f0);
    float pvel[3] = { 0.0f, 0.0f, 5.0f };
    phys_body_push(b0, pvel, 0.5f, 4.0f);
    for (int i = 0; i < 30; i++) phys_body_step(w, 1.0f / 100.0f);
    float b1[3], f1[3];
    phys_body_center(bn, b1);
    phys_body_center(bf, f1);
    st_check(b1[2] - b0[2] > 0.05f, "push moves nearby body");
    st_check(fabsf(f1[2] - f0[2]) < 0.05f, "push ignores distant body");

    // Cleanup: the test leaves no garbage in the world.
    phys_body_enable(1);
    phys_body_kill_all(w);
    phys_static_clear();
    st_check(phys_body_count() == 0, "cleanup");
    return st_fail;
}
