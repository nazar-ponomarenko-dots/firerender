// ============================================================================
//  water.h — water: puddles, lakes, rivers, seas (MeshInstance3D with alpha blending)
// ----------------------------------------------------------------------------
//  Water bodies are described in map.json by the waters[] block (MapWater in mapdata.h).
//  Types: puddle/lake/sea — a circle (pos + radius; a sea with radius=0 covers
//  the whole world), river — an axis line points + channel width width.
//
//  The surface is a flat grid at the level; waves move vertices in
//  the vertex shader (coarse grid), wave normals are computed analytically
//  in the fragment shader (that is why waves stay smooth at any grid step). Water
//  depth (level minus terrain) is read from the heightmap: alpha grows with
//  depth, at the shore there is a bright rim and foam.
//
//  Physics: water_surface_at/flow_at — for the camera (swimming, river current)
//  and vegetation (removed from the submerged bottom). water_underwater — for
//  the underwater screen overlay (renderer.c).
// ============================================================================
#ifndef WATER_H
#define WATER_H
#include "gl_loader.h"
#include "mapdata.h"
#include "render_ctx.h"
#include "scene.h"
#include "world.h"

typedef struct WaterInst {
    const MapWater *md;        // data from map.json (lives until mapdata_free)
    GLuint vao, vbo, ibo;      // flat grid at the water level
    int idx_count;
    float wave_len;            // wavelength, m (depends on the type)
    float bbox_min[3], bbox_max[3];   // for frustum culling
} WaterInst;

typedef struct {
    const WorldState *world;
    const RendererContext *rc;
    SceneUniforms su;          // shared scene uniform set (light, fog, shadows)
    GLuint prog;
    GLuint tex_height;         // heightmap (water depth, unit 2)
    // Own uniforms
    GLint u_time, u_level, u_waveamp, u_wavespeed, u_wavelen;
    GLint u_color, u_alpha, u_depthfade;
    GLint u_flowdir, u_flowspeed, u_foam, u_edge;
    GLint u_heightmin, u_heightmax;
    GLint u_wetness;           // wetness (rain)
    GLint u_reflection;        // uReflection sampler
    WaterInst inst[MAP_MAX_WATERS];
    int n;
    float time;                // accumulated in update (for waves/flow)
} WaterParams;

// Creates the Water node: builds grids for all water bodies of the map. NULL if there is no
// water or the shaders failed to link. After creation the water_* functions below can be called.
SceneNode *water_make_node(WorldState *w, const WorldTextures *wt,
                           const RendererContext *rc);
void water_node_update(SceneNode *n, float dt);
void water_node_render(SceneNode *n, float dt);
void water_cleanup(SceneNode *n);

// --- Water queries from code (camera, vegetation, HUD, Lua) -------------------
// Surface level of the highest-lying water body above a point (x, z).
// If no water body covers the point — returns -1e9. The terrain is NOT
// taken into account: the caller compares the level with the height itself (near the shore the
// terrain is above the level — there is effectively no water there).
float water_surface_at(const WorldState *w, float x, float z);

// Current at a point (river): returns 1 if (x, z) is inside the river channel,
// and writes flow[2] = (current vector) * speed (m/s). 0 — no current.
int water_flow_at(const WorldState *w, float x, float z, float flow[2]);

// Eyes underwater: returns 1 (writes the water body's tint into tint[3]). For
// the underwater screen overlay.
int water_underwater(const WorldState *w, const float pos[3], float tint[3]);

// Number of water bodies on the map (for the HUD).
int water_count(void);

#endif
