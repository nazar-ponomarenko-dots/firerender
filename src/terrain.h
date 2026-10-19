// ============================================================================
//  terrain.h — terrain (MeshInstance3D): ground geometry and material
// ----------------------------------------------------------------------------
//  Terrain from the heightmap + img/ground.png material with embossed grass
//  tufts (far grass is "baked into" the texture — a cheap LOD).
//
//  Geometry — 128x128 m TILES, each in 4 LOD levels (quadtree with
//  simplification: flat areas merge into large quads, slopes stay fine).
//  The LOD is chosen by tile distance to the player; tiles off-screen (frustum)
//  aren't drawn. "Skirts" close the level seams — geometry without holes.
//  Distant LODs carry a minimum of polygons.
// ============================================================================
#ifndef TERRAIN_H
#define TERRAIN_H
#include "gl_loader.h"
#include "mapdata.h"     // SPLAT_LAYERS
#include "render_ctx.h"
#include "scene.h"
#include "world.h"

// --- Terrain tile: 128x128 m, 4 LOD levels -----------------------------------
#define TERRAIN_LODS 4
#define TERRAIN_TILE 128.0f
typedef struct {
    float xmin, xmax, zmin, zmax;   // tile bounds along XZ (meters)
    float ymin, ymax;               // height range (ymin has skirt slack)
    GLuint  first[TERRAIN_LODS];    // first index of the LOD in the shared IBO
    GLsizei count[TERRAIN_LODS];    // how many indices the LOD has
} TerrainTile;

typedef struct TerrainParams {
    const WorldState *world;        // world data: sizes, heights, field
    const RendererContext *rc;      // per-frame data (renderer.c)
    SceneUniforms su;               // shared scene uniform set
    GLuint prog, vao, vbo, ibo;      // program and geometry (with indices)
    GLuint tex_base, tex_norm, tex_height;  // ground, normals, heights
    GLint u_specpow, u_specint;     // speculars (ground is matte: spec = 0)
    GLint u_heightmin, u_heightmax, u_texscale;
    GLint u_statictex, u_static_on, u_worldsize;   // static shadows
    GLint u_baked_lm, u_baked_on;   // baked light (RGB luxel, unit 9)
    GLint u_sunvis, u_sunvis_on;    // sun-visibility map (unit 16): distant
                                    // static shadows in mode 1 (CSM)
    GLint u_wetness_mask;           // wetness mask (unit 8)
    // Surfaces (splat): material mask + 4 texture layers
    GLuint tex_splat_layer[SPLAT_LAYERS];   // soil/sand/gravel/asphalt
    GLint u_splat, u_splat_on, u_layer[SPLAT_LAYERS], u_layer_scale;
    GLint u_detail_dist;            // terrain detail LOD (0 = off)
    GLint u_simple;                 // 1 = simplified render (point/spot loops -> 0)
    TerrainTile *tiles;             // tile array (tiles_n of them)
    int tiles_n;
    int vert_count;                // indices in the geometry (all tiles and LODs)
} TerrainParams;

// Creates the Terrain node: builds the terrain mesh, the ground material (with
// grass), compiles shaders and sets the static uniforms. NULL on error.
SceneNode *terrain_make_node(WorldState *w, const WorldTextures *wt,
                             const RendererContext *rc);

void terrain_node_render(SceneNode *n, float dt);

// Frees the node's GL resources (program, geometry, ground texture).
void terrain_cleanup(SceneNode *n);

// How many 128x128 m tiles are in the current terrain (for the HUD). 0 — terrain not created.
int terrain_tile_count(void);

// --- Static shadows (r_shadows_mode 2) ---------------------------------
// Traces rays from the sun over the terrain and map objects once and stores
// the result into a texture (a small resolution for the whole map).
// Dynamic CSM shadows are not rendered while it is used. The texture lives as
// long as the terrain does; calling again recomputes it.
void terrain_shadows_bake(const WorldState *w, SunParams *sun);
// 1 if the static shadow map is ready (and can be applied).
int terrain_shadows_have(void);

// --- Wetness: baked mask (under roofs/overhanging objects) -------
// Builds a wetness texture 0..1: a ray upward (0,1,0) until MAX_WETNESS_H or
// leaving the map. Hit an object (via ShadowObj cylinders) -> dry (0),
// otherwise wet (1). Noise + slope factor are applied in the shader.
void terrain_wetness_mask_bake(const WorldState *w);
int terrain_wetness_mask_have(void);
GLuint terrain_wetness_mask_tex(void);
// "Open to the sky" mask for snow: the same baked texture (RG8:
// R = open sky, G = under a roof).
int terrain_snow_mask_have(void);
GLuint terrain_snow_mask_tex(void);

// --- Baked light (offline map-compilation, FR_BAKE / make mapbake) ----
// Computes lighting in 3D once offline and writes into map/baked/:
//   lightmap.png — terrain RGB luxel (sun + direct light + hemispherical
//                  ambient with AO from the sky + 1 diffuse bounced bonus),
//                  STATIC occluders ONLY (dynamics excluded);
//   probes.bin   — 3D grid of lighting probes for DYNAMIC objects
//                  (ambient RGB + sun visibility at the point).
// At runtime the engine only READS (terrain_baked_light_load) and substitutes.
void terrain_light_bake(const WorldState *w, SunParams *sun,
                        const RendererContext *rc);
// Loads map/baked/lightmap.png (+ probes.bin) at world load. 1 — success.
int  terrain_baked_light_load(const WorldState *w);
// 1 if baked light is loaded and can be applied (cvar r_baked_light).
int  terrain_baked_light_have(void);
// 1 if the sun-visibility map is loaded (for distant shadows in CSM mode 1).
int  terrain_baked_sunvis_have(void);
// Frees the GL luxel texture and the probe grid (on world unload).
void terrain_baked_light_free(void);
// Trilinear sample of ambient (out[0..2]) and sun visibility (out[3]) from
// the probe grid at a world point (x,y,z) — for lighting dynamic objects.
void terrain_baked_probe(float x, float y, float z, float out[4]);
// 1 if the probe grid itself is loaded (the lightmap may exist without it).
int  terrain_baked_probes_have(void);

#endif
