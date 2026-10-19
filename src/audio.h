// ============================================================================
//  audio.h — sound (SDL3 audio): effects and background loops
// ----------------------------------------------------------------------------
//  Sounds load through the resource manager (map pack -> disk -> built-in),
//  so a map can put its own audio/*.wav into a zip — they override
//  the engine ones. Playback: several stream channels bound to the
//  device (the device mixes them itself). There is a background channel for loops
//  (wind and the like).
// ============================================================================
#ifndef AUDIO_H
#define AUDIO_H

typedef struct Sound Sound;
struct WorldState;   // incomplete type: audio only needs a pointer to the map
typedef struct WorldState WorldState;   // same types as in world.h

// Audio initialization: opens the device. The engine works without
// sound (if there is no device/files). Call once after SDL_Init.
void audio_init(void);

// Every frame: services the background loop (feeds it more data).
void audio_update(void);

// Loads a wav (through resources). Cache: one file — one object.
// NULL if the file is missing or broken.
Sound *audio_load(const char *path);

// Plays a sound once. volume 0..1, pitch 0.5..2 (1 = original).
// If all channels are busy — takes the oldest one.
void audio_play(Sound *s, float volume, float pitch);

// Background loop: the sound repeats continuously until audio_loop_stop.
// volume 0..1.
void audio_loop_start(Sound *s, float volume);
void audio_loop_stop(void);
// Loop volume multiplier applied on the fly (weather: wind is louder in rain).
// 1.0 — unchanged; audio_update applies it every frame.
void audio_loop_mul_volume(float mul);

// Map music: a second independent loop (does not conflict with ambient).
void audio_music_start(Sound *s, float volume);
void audio_music_stop(void);

// --- Positional (3D) sound -----------------------------------------------------
// Source at pos[3]: volume falls off with distance to the listener
// (inaudible beyond radius), reverb echo grows with distance.
// audio_3d_play — loop (repeats until audio_3d_stop);
// audio_3d_play_once — once (the channel frees itself).
// Returns the channel id (>= 0) or -1.
int audio_3d_play(Sound *s, float volume, const float pos[3], float radius);
int audio_3d_play_once(Sound *s, float volume, const float pos[3], float radius);
void audio_3d_stop(int id);
void audio_3d_stop_all(void);

// Listener (camera position): call every frame from the main loop.
void audio_set_listener(const float pos[3]);

// World for occlusion: without it sound "always passes straight through". NULL — off.
void audio_set_world(const struct WorldState *w);

// Volume multiplier from line-of-sight occlusion LISTENER -> to
// (1 = open, lower = behind a hill/wall). Exposed — for the console and tests.
float audio_occlusion_at(const float to[3]);

// Console command: snd_occlude <x> <y> <z> (registered in cmdreg.c).
void cmd_snd_occlude(const char *args);

// Occlusion self-test on a synthetic wall (FR_TEST 'Z'). Number of failures.
int audio_selftest(void);

// Same on a real map: logs the multiplier for every sound in map.json.
// Call after the world is loaded (needs heights and the player position).
void audio_selftest_world(const WorldState *w, const float listener[3]);

// Debug: lists the sounds playing right now (space-separated names).
// Returns the sound count. buf — the string, size — its capacity.
int audio_active_names(char *buf, int size);

// Full teardown at exit.
void audio_cleanup(void);

#endif
