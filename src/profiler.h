// ============================================================================
//  profiler.h — profiler: per-module scene timings (see profiler.c)
// ----------------------------------------------------------------------------
//  Every frame prof_reset() zeroes the counters; modules add their time via
//  prof_add_ms(). The panel draws at the bottom right (profiler command).
//  Vertex/triangle/draw-call counters are collected in gl_loader.c
//  automatically on every glDraw*.
// ============================================================================
#ifndef PROFILER_H
#define PROFILER_H

typedef enum {
    PROF_SKY = 0,      // skybox
    PROF_TERRAIN,      // terrain (without shadows)
    PROF_VEG,          // vegetation
    PROF_OBJECTS,      // sprite objects
    PROF_FAUNA,        // fauna
    PROF_FIELD,        // field frame
    PROF_SEL,          // selection frames
    PROF_MM,           // minimap
    PROF_HUD,          // HUD
    PROF_SHADOW,       // cascaded shadows
    PROF_FOG,          // volumetric fog
    PROF_BLOOM,        // bloom
    PROF_SSR,          // screen-space reflections
    PROF_GI,           // global illumination (indirect light + AO)
    PROF_N
} ProfSlot;

// Zeroes timings and counters (call at the start of the frame).
void prof_reset(void);

// Adds time (ms) to a slot.
void prof_add_ms(ProfSlot s, float ms);

// --- GPU timings (GL timer queries) -------------------------------------------
// Node wrapper: prof_gpu_begin(slot) before rendering, prof_gpu_end(slot) after.
// The result (GPU time, ms) is available from the next frame — the panel
// shows g_prof_gpu_ms[slot] (from the previous frame).
void prof_gpu_begin(ProfSlot s);
void prof_gpu_end(ProfSlot s);
float prof_gpu_ms(ProfSlot s);

const char *prof_name(ProfSlot s);
float prof_ms(ProfSlot s);

// Total time of all 3D slots (for percentages).
float prof_total_ms(void);
float prof_gpu_total_ms(void);

#endif
