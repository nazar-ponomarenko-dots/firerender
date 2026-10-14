// ============================================================================
//  world.h — the world: heightmap data and terrain functions
// ----------------------------------------------------------------------------
//  All world state (sizes, grid, heights, wheat field rect) is gathered in
//  WorldState — modules receive it as a parameter instead of grabbing
//  globals. The instance belongs to game.c (Game.world), filled by world_load.
//
//  1 map pixel = 1 meter (a 2000x1125 map -> a 2000x1125 m world).
//  The heightmap lives in the map/ folder (file name comes from map/map.json,
//  see mapdata.h). Terrain height is NOT linear in brightness: black = +400 m,
//  white = -100 m, following the height_curve, which gives more resolution to
//  the gameplay band around -20..100 m (the t=0.16..0.40 band is stretched
//  over heights -50..200 m, the edges are squeezed into gentle linear
//  segments). Heights are computed once at load (world_load), smoothed
//  (blur_heights) and stored in a GL_R8 texture — the shader interpolates it
//  bilinearly.
// ============================================================================
#ifndef WORLD_H
#define WORLD_H
#include "gl_loader.h"

// --- World state -----------------------------------------------------------
// The WorldState tag exists not only for the typedef: audio.h declares a
// pointer to the incomplete type so it doesn't drag in world.h's GL headers.
typedef struct WorldState {
    // Height curve (configured from map.json: terrain = { ... }).
    // Terrain height from the map color (inverted): black (brightness 0) ->
    // max_height, white (brightness 1) -> min_height. The gameplay band
    // t=band_t0..t1 covers the whole height range band_h0..band_h1 (more
    // resolution), the edges are gentle linear segments. t = 1 - brightness.
    float min_height, max_height;
    float band_t0, band_t1, band_h0, band_h1;
    int terrain_blur;        // heightmap smoothing passes (from map.json)
    int terrain_blur_radius; // smoothing radius, meters
    int terrain_erosion;     // steepness-clamp passes (slope erosion)
    float terrain_erosion_slope; // max steepness after erosion, degrees

    // 1 map pixel = 1 world meter: world size and grid come from the heightmap
    // at load (a 2000x1125 map -> a 2000x1125 m world, a 6k map ->
    // a 6k m world).
    float world_w, world_d;  // world size along X and Z (meters)
    int tex_w, tex_h;        // map resolution (pixels)
    int grid_w, grid_h;      // quads in the terrain grid
    float tex_per_quad;      // map pixels per quad
    int vert_count;          // terrain vertices (for the HUD)
    float *heights;          // smoothed terrain heights (meters)

    // Wheat field rectangle (set by game.c at world load):
    // x0..x0+w, z0..z0+d
    float field_x0, field_z0, field_w, field_d;
} WorldState;

// 1 grid quad per GRID_DIV map pixels. GRID_DIV=1 -> a grid of 1 vertex
// per meter: the terrain smoothly bilinearly interpolates the heightmap (no
// big "facets") and matches the collision (sample_height) EXACTLY, so the
// player, grass and wheat stand right on the drawn ground. The quad cap along
// one axis is for gigantic maps (there precision approaches a coarse step).
#define GRID_DIV 1
#define GRID_MAX 2048

// World textures created by world_load (owned by world_free)
typedef struct {
    GLuint tex_height;   // terrain heights (GL_R8, LINEAR)
    GLuint tex_base;     // map color (minimap)
    GLuint tex_norm;     // terrain normal map
} WorldTextures;

// --- Functions ------------------------------------------------------------------

// Simple xorshift generator 0..1 (no rand(), so results are identical everywhere)
float frnd(unsigned int *s);

// Height curve: t = 1 - brightness (0 = white/low, 1 = black/peak) -> meters.
// The same formula is baked into terrain.vs (the height bands there are uniforms).
float height_curve(const WorldState *w, float t);

// Ground height under a point (wx, wz) — bilinear interpolation of QUANTIZED
// heights (the same GL_R8 byte the terrain vertex shader reads with LINEAR).
// Because of this, plants and the field border stand exactly on the drawn
// terrain and don't sink underground at micro-bumps (quantization step ~2 m).
// 1 map pixel = 1 meter, so the texel coordinate = world minus 0.5.
float sample_height(const WorldState *w, float wx, float wz);

// How much the segment from->to is exposed to the terrain: 1 = straight line
// of sight, 0 = the whole path is underground. Computed as the fraction of
// segment points where the ground is above the line (2.5 m step). Sound needs
// it so that "behind a hill" sounds muffled; it lives here so audio.c doesn't
// pull in the heightmap.
float world_line_open(const WorldState *w, const float from[3], const float to[3]);

// Loads the heightmap from map/ (name — from map/map.json): computes the world
// sizes, heights (curve + blur) and normals, creates the height/color/normal
// textures. Fills w. 0 — success.
int world_load(WorldState *w, WorldTextures *out);

// Frees the CPU height arrays and the world's GL textures.
void world_free(WorldState *w);

// Average map brightness (the camera needs it for the starting height).
float world_avg_lum(void);

// Heightmap texture (GL_R16) — for post-passes (volumetrics). 0 = none.
GLuint world_height_tex(void);

#endif
