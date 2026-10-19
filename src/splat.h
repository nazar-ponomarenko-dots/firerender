// ============================================================================
//  splat.h — terrain surfaces: material splat map, roads, occupancy
// ----------------------------------------------------------------------------
//  ONE map per world, baked at load time (splat_build) and living until
//  unload. It stores three things needed by both the renderer and the logic:
//
//   1) SPLAT MASK (RGBA8, 1 texel = surfaces.resolution meters):
//      R = dirt, G = sand, B = gravel/path, A = asphalt. The base layer
//      (grass/soil terrain.ground_tex) shows wherever all channels are zero.
//      terrain.fs blends layers using this mask.
//
//   2) ROAD LEVELING: roads "cut" the terrain — the surface becomes smooth
//      along its centerline (heights in WorldState.heights are modified
//      BEFORE normals and mesh generation). Car roads level strongly,
//      footpaths — almost not at all (flatten in MapRoad).
//
//   3) OCCUPANCY MAP (R8, separate resolution): 0..1, where 1 = "something
//      stands here" (static 3D object/model) or "this is a road".
//      Vegetation (vegetation.c) doesn't seed grass where occupancy is
//      high — grass no longer grows through cars, crates and asphalt.
//
//  Everything is computed on the CPU once: a 2000x1125 map at 1 m resolution
//  is a 2.2 MB mask. No per-frame costs beyond one texture sample
//  in the terrain shader.
// ============================================================================
#ifndef SPLAT_H
#define SPLAT_H
#include "gl_loader.h"
#include "mapdata.h"
#include "world.h"

// Bakes the splat mask and occupancy map, LEVELS the terrain under roads.
// Call AFTER mapdata_load and height loading, but BEFORE building normals and
// terrain geometry (modifies w->heights). Idempotent: a repeated call
// rebuilds everything from scratch.
void splat_build(WorldState *w);

// Frees CPU arrays and textures (on world unload).
void splat_free(void);

// Height modifications under roads: called FROM world_load between height
// computation and normals. Returns 1 if the terrain changed (roads with
// flatten > 0 exist). A full splat_build cannot be called (world dimensions
// must be ready) — hence leveling is a separate step.
int splat_flatten_roads(WorldState *w);

// --- Access from the shader ----------------------------------------------------
// Splat mask texture (RGBA8, LINEAR, CLAMP). 0 — not baked.
GLuint splat_texture(void);
int splat_have(void);

// Layer parameters for terrain uniforms (tile scales and texture paths).
const MapSurfaces *splat_surfaces(void);

// --- Access from the logic -----------------------------------------------------
// Material weight at a world point (0..1). Index — SPLAT_DIRT..SPLAT_ASPHALT.
float splat_weight_at(int material, float wx, float wz);

// Grass density multiplier at a point (0 = doesn't grow, 1 = as usual).
// Accounts for both surface layers (surfaces.grass) and object occupancy.
float splat_grass_factor(float wx, float wz);

// Occupancy of a point 0..1 (1 = object stands / road lies there).
float splat_occupancy_at(float wx, float wz);

// --- Occupancy registration from code (models placed at runtime) ----------------
// Occupancy circle of radius r around (x, z) with strength (0..1).
// Models placed by the glb/mesh command register themselves here so that
// grass under them disappears on the next chunk regeneration.
void splat_occupy_circle(float x, float z, float r, float strength);

// Occupancy rectangle (AABB in XZ).
void splat_occupy_box(float x, float z, float hw, float hd, float strength);

// Marks the dynamic vegetation layer as "needs rebuild"
// (after splat_occupy_*). Returns the change counter — vegetation.c
// compares it with its own and regenerates chunks on divergence.
unsigned int splat_occupancy_version(void);

#endif
