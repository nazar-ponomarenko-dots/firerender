// ============================================================================
//  shadow.h — shadows: cascaded shadow maps (CSM) from the sun
// ----------------------------------------------------------------------------
//  Classic scheme: the camera is placed at the light source (sun), depth is
//  rendered. A ground fragment is lit if its depth is no farther than the one
//  recorded in the map, and in shadow if something (terrain, grass, wheat)
//  blocked the light. Four cascades follow the player (8/16/96/512 m) — sharp
//  shadows nearby and coverage far away, with a smooth blend between cascades
//  (no "pop-in"). Maps are rendered every frame with the fresh sun direction —
//  light and shadows always agree. Toggle — key [.
//
//  THE SHADOW PASS IS SINGLE: shadow_render (called by renderer_frame) sets
//  pass = RP_SHADOW in RendererContext for each cascade and walks the scene
//  tree — nodes draw their own geometry into depth (terrain, vegetation,
//  objects), other nodes are skipped in the shadow pass.
// ============================================================================
#ifndef SHADOW_H
#define SHADOW_H
#include "gl_loader.h"
#include "scene.h"

typedef struct RendererContext RendererContext;   // defined in render_ctx.h

#define SHADOW_CASCADES 4
// Cascades (0 — the most detailed at the player's feet):
//   0: box 8 m   — texel size/8 (right under the player)
//   1: box 16 m  — texel size/16 (at the feet)
//   2: box 96 m  — texel size/96 (near, dynamic radius)
//   3: box 512 m — texel size/512 (mid-range, up to 256 m from the player)
// Beyond 256 m from the player there are no shadows at all (hard cut in shaders).
// Map size — from map/map.json (shadows.size, default 2048).
#define SHADOW_BOX_0   8.0f
#define SHADOW_BOX_1  16.0f
#define SHADOW_BOX_2  96.0f
#define SHADOW_BOX_3 512.0f
#define SHADOW_DEPTH_MUL 1.6f    // box depth = box*1.6 + 400 m
extern int g_shadow_size[SHADOW_CASCADES];   // all cascades (from shadows.size)
#define SHADOW_DEPTH_MUL 1.6f    // box depth = box*1.6 + 400 m
extern int g_shadow_size[SHADOW_CASCADES];   // all cascades (from shadows.size)

// Compiles the shadow shaders and creates the cascades' FBOs/textures. Called
// after the GL context is ready, before the first frame.
void shadow_init(void);

// Draws all cascades (tree walk: nodes draw depth when pass == RP_SHADOW).
// Does nothing if shadows are off. Called from renderer_frame
// BEFORE the regular pass.
void shadow_render(SceneNode *root, RendererContext *rc);

// Test toggle (FR_TEST: c) — disable chunk culling in shadow maps.
extern int g_shadow_cull;
// Test toggles (FR_TEST): terrain/vegetation/objects in shadow maps.
extern int g_shadow_terrain;
extern int g_shadow_veg;
extern int g_shadow_obj;
// Static cascade cache (2/3 are not redrawn while the snapped matrix
// is unchanged). FR_TEST 'C' disables it — for A/B measurements.
extern int g_shadow_cache;
int shadow_is_on(void);
void shadow_toggle(void);
void shadow_set(int on);   // for the console (cvar r_shadows)

// Depth texture of cascade c (for terrain_node_render).
GLuint shadow_texture(int c);

// "light -> NDC" matrix of cascade c (last rendered frame).
// Vegetation needs it to receive shadows from terrain and neighbors.
const float *shadow_matrix(int c);

// Frees the shadow programs, textures and FBOs.
void shadow_cleanup(void);

#endif
