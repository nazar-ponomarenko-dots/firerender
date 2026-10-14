// ============================================================================
//  fx.h — particle effects: named presets and continuous emitters
// ----------------------------------------------------------------------------
//  HL2/Far Cry 2 style: an effect is a named set of particle parameters
//  (ParticleParams) + a count. Effects can be launched from the console
//  (fx command) and from Lua (engine.fx_*). Continuous emitters (rain, snow,
//  fire, smoke from a chimney) spawn particles at a given rate in an area.
//
//  Built-in effects: blood, smoke, explosion, fire, rain, snow, sparks,
//  dust, muzzle_flash. Register your own via fx_register (from code/Lua).
// ============================================================================
#ifndef FX_H
#define FX_H

#include "particles.h"

#define FX_MAX_PRESETS 64
#define FX_MAX_EMITTERS 8
#define FX_NAME_LEN 32

typedef struct {
    char name[FX_NAME_LEN];
    ParticleParams pp;
    int count;             // how many particles in the burst
} FxPreset;

// --- Presets ----------------------------------------------------------------
// Registers a named preset (replaces an existing one with the same name).
// count — default particle count for fx_burst(name).
void fx_register(const char *name, const ParticleParams *pp, int count);

// Finds a preset by name. NULL — no such preset.
const FxPreset *fx_find(const char *name);

// Launches a burst of the preset at a point (count — or 0 = preset default).
void fx_burst(const char *name, float x, float y, float z, int count);

// --- Emitters (continuous) --------------------------------------------------
// Starts an emitter: spawns particles of the preset at rate units/sec in the box
// [pos±ext]. life — duration in seconds (<=0 — until stopped).
// name — preset name; the emitter gets the same name (stop by name).
void fx_emitter_start(const char *name, const float *pos, const float *ext,
                      float rate, float life);
// Stops all emitters with the given name.
void fx_emitter_stop(const char *name);
// Stops all emitters.
void fx_emitter_stop_all(void);

// Ticks the emitters (call from particles_node_update/particles_update).
void fx_update(float dt);

// --- Weather (an emitter following the player) ------------------------------------
// Starts an emitter of the preset whose center is copied each frame from the
// player position (x,z) with a vertical offset y_off (rain/snow around the player).
// stop_name — emitter name (usually equal to the preset name); life<=0 — until
// stopped.
void fx_weather_start(const char *name, float y_off, const float *ext,
                      float rate, float life);

// --- Wetness -------------------------------------------------------
// Returns the current wetness level 0..1 (grows in rain, dries without it).
float fx_wetness(void);
// Target wetness (1.0 in rain, 0.0 without rain) — for a smooth ramp.
float fx_wetness_target(void);
// Snow cover 0..1: grows while snow is falling (slowly, ~60 s), melts afterwards.
float fx_snowcover(void);
// Forced wetness (tests/console): pins the level without rain.
void fx_force_wetness(float v);

// --- Cinematic weather (rain) -----------------------------------------
// Rain intensity 0..1: ramps up (drizzle -> downpour) after weather starts.
float fx_weather_intensity(void);

// --- Control and console -----------------------------------------------------
// Prints the preset list to the console (fx_list).
void fx_print_list(void);
// Registers the built-in effect presets (call at startup BEFORE the commands).
void fx_register_builtin(void);

// --- Console command handlers (registered by the central cmdreg registry) ---
void cmd_fx(const char *args);
void cmd_fx_burst(const char *args);
void cmd_fx_emit(const char *args);
void cmd_fx_stop(const char *args);
void cmd_fx_clear(const char *args);
void cmd_fx_list(const char *args);
void cmd_weather(const char *args);

// [deprecated] Used to register both presets AND commands. Now presets —
// fx_register_builtin(), commands — the central registry (cmdreg). Kept for
// call compatibility: simply calls fx_register_builtin().
void fx_init_console(void);

#endif
