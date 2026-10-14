// ============================================================================
//  health.h — health (HP): a reusable health component
// ----------------------------------------------------------------------------
//  A small standalone gameplay-layer module: current/max HP, damage/healing,
//  auto-regeneration after a pause, death and the respawn countdown,
//  on-screen damage "flash". The engine (camera/physics/renderer) knows
//  nothing about it — the gameplay layer owns it (Player in player.h,
//  logic in game.c).
//
//  One Health struct is reusable: the player holds it now, but the same
//  component suits entities (npc/breakable) via Entity.data.
// ============================================================================
#ifndef HEALTH_H
#define HEALTH_H

typedef struct {
    float hp;           // current health
    float max;          // maximum
    float regen;        // recovery rate, units/s (0 — regen off)
    float regen_delay;  // regen delay after damage, s
    float since_hurt;   // timer since the last damage, s
    int   god;          // invulnerability (cheat)
    int   dead;         // 0/1 — dead
    float respawn_t;    // countdown to respawn after death, s
    float respawn_delay;// how long to wait before respawning, s
    float hurt_flash;   // 0..1 — on-screen damage flash (fades smoothly)
} Health;

// Initialization: full HP = max, default regen/delay/respawn.
void  health_init(Health *h, float max);

// Change the maximum. refill!=0 — top current HP up to the new maximum.
void  health_set_max(Health *h, float max, int refill);

float health_frac(const Health *h);   // 0..1 (for the HUD bar)
int   health_is_dead(const Health *h);

// Deal damage (amount>0). Ignored while god/dead. Returns the HP actually
// removed. Crossing 0 marks dead and starts the respawn countdown.
float health_damage(Health *h, float amount);

// Heal by amount (never above max). Does not heal the dead (needs respawn/revive).
void  health_heal(Health *h, float amount);

// Instant death (for the kill command/scripts).
void  health_kill(Health *h);

// Per-frame tick: regen (after the delay), flash decay, respawn countdown.
// If it is time to respawn — *respawn_ready is set to 1 (otherwise 0); game.c
// teleports the player to the spawn point and calls health_revive.
void  health_update(Health *h, float dt, int *respawn_ready);

// After respawn: full HP, clear the death flag.
void  health_revive(Health *h);

#endif
