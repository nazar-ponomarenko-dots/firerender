// ============================================================================
//  health.c — здоровье (HP), см. health.h
// ============================================================================
#include "health.h"

#include <SDL3/SDL.h>

#define HEALTH_DEFAULT_MAX      100.0f
#define HEALTH_DEFAULT_REGEN    6.0f    // ед/с
#define HEALTH_REGEN_DELAY      4.0f    // с без урона до начала регена
#define HEALTH_RESPAWN_DELAY    2.0f    // с от смерти до респавна

void health_init(Health *h, float max)
{
    if (!h) return;
    if (max <= 0.0f) max = HEALTH_DEFAULT_MAX;
    h->max = max;
    h->hp = max;
    h->regen = HEALTH_DEFAULT_REGEN;
    h->regen_delay = HEALTH_REGEN_DELAY;
    h->since_hurt = h->regen_delay;    // старт без «свежего» урона
    h->god = 0;
    h->dead = 0;
    h->respawn_t = 0.0f;
    h->respawn_delay = HEALTH_RESPAWN_DELAY;
    h->hurt_flash = 0.0f;
}

void health_set_max(Health *h, float max, int refill)
{
    if (!h || max <= 0.0f) return;
    h->max = max;
    if (refill || h->hp > max) h->hp = max;
}

float health_frac(const Health *h)
{
    if (!h || h->max <= 0.0f) return 0.0f;
    float f = h->hp / h->max;
    return f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
}

int health_is_dead(const Health *h)
{
    return h && h->dead;
}

float health_damage(Health *h, float amount)
{
    if (!h || amount <= 0.0f || h->dead || h->god) return 0.0f;
    float before = h->hp;
    h->hp -= amount;
    h->since_hurt = 0.0f;
    // Вспышка урона: сильнее при крупном уроне, но с потолком.
    float f = amount / (h->max > 0.0f ? h->max : 1.0f);
    h->hurt_flash += f * 1.5f;
    if (h->hurt_flash > 1.0f) h->hurt_flash = 1.0f;
    if (h->hp <= 0.0f) {
        h->hp = 0.0f;
        h->dead = 1;
        h->respawn_t = h->respawn_delay;
        SDL_Log("Health: DEAD");
    }
    return before - h->hp;
}

void health_heal(Health *h, float amount)
{
    if (!h || amount <= 0.0f || h->dead) return;
    h->hp += amount;
    if (h->hp > h->max) h->hp = h->max;
}

void health_kill(Health *h)
{
    if (!h || h->dead) return;
    h->god = 0;
    health_damage(h, h->hp + 1.0f);
}

void health_update(Health *h, float dt, int *respawn_ready)
{
    if (respawn_ready) *respawn_ready = 0;
    if (!h) return;
    // Затухание экранной вспышки урона (~2.5 ед/с).
    if (h->hurt_flash > 0.0f) {
        h->hurt_flash -= dt * 2.5f;
        if (h->hurt_flash < 0.0f) h->hurt_flash = 0.0f;
    }
    if (h->dead) {
        h->respawn_t -= dt;
        if (h->respawn_t <= 0.0f && respawn_ready) *respawn_ready = 1;
        return;
    }
    // Регенерация — только спустя regen_delay без урона.
    h->since_hurt += dt;
    if (h->regen > 0.0f && h->since_hurt >= h->regen_delay && h->hp < h->max) {
        h->hp += h->regen * dt;
        if (h->hp > h->max) h->hp = h->max;
    }
}

void health_revive(Health *h)
{
    if (!h) return;
    h->dead = 0;
    h->hp = h->max;
    h->since_hurt = h->regen_delay;
    h->respawn_t = 0.0f;
    h->hurt_flash = 0.0f;
}
