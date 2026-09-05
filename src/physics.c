// ============================================================================
//  physics.c — мини-движок физики тел (Verlet), см. physics.h
// ----------------------------------------------------------------------------
//  Ядро без рендера: частицы + связи, интеграция Верле, релаксация связей,
//  коллизия с рельефом через колбэк (main.c подсовывает sample_height).
//  Тестовый мир — один глобальный (верёвка/цепь из консоли); на нём
//  отрабатывается фундамент под будущий рэгдолл (частицы = кости скелета).
// ============================================================================
#include "physics.h"

#include <math.h>
#include <string.h>

// Колбэк высоты рельефа (ставит main.c: sample_height мира игры)
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
    // Верле: скорость = pos - prev, поэтому импульс = сдвиг prev назад
    p->prev[0] -= vel[0];
    p->prev[1] -= vel[1];
    p->prev[2] -= vel[2];
}

// Релаксация одной связи: частицы съезжаются/разъезжаются к длине покоя,
// пропорционально обратным массам.
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

void physics_step(PhysWorld *w, float dt)
{
    if (w->np <= 0) return;
    float g = w->gravity * dt * dt;

    // 1) Интеграция Верле: pos += (pos - prev) * (1 - damping) + g
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

    // 2) Релаксация связей + коллизии (несколько итераций: сеть «дожимается»)
    for (int it = 0; it < w->iterations; it++) {
        for (int i = 0; i < w->nl; i++)
            link_relax(w, &w->link[i]);
        // Коллизия с рельефом: частица не ниже земли + радиус
        if (s_ground) {
            for (int i = 0; i < w->np; i++) {
                PhysPart *p = &w->part[i];
                if (p->inv_mass <= 0.0f || p->radius <= 0.0f) continue;
                float h = s_ground(p->pos[0], p->pos[2]) + p->radius;
                if (p->pos[1] < h) {
                    // трение: гасим горизонтальный сдвиг при контакте
                    p->pos[1] = h;
                    p->prev[0] += (p->pos[0] - p->prev[0]) * 0.5f;
                    p->prev[2] += (p->pos[2] - p->prev[2]) * 0.5f;
                }
            }
        }
    }
}
