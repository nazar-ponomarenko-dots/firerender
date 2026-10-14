// ============================================================================
//  vegetation.h — grass and wheat (MeshInstance3D): instancing, 2 layers
// ----------------------------------------------------------------------------
//  Layer 1 — STATIC: the whole map is covered with grass/wheat at minimal
//  density. Generated ONCE at startup, uploaded to a VBO and then
//  never touched again — vegetation exists across the whole map from the first second.
//  Layer 2 — DYNAMIC: around the camera the density grows along an LOD curve,
//  only chunks whose density changed noticeably are rebuilt.
//
//  Sprites — layers of a 256x178 2D array: 0/1/2 = three grass kinds
//  (grass.png, grass1.png, grass2.png), 3 = dandelion (dandelions.png),
//  4/5/6 = wheat LODs (wheat_lod0/1/2.png, the LOD is picked in the shader by
//  distance). Kind, rotation/size/tint/tilt — instance attributes
//  (12 float). Drawing — 4 instanced calls (2 static + 2 dynamic),
//  one shader.
//  Layer (quad index q): 0=static grass, 1=static wheat,
//                        2=dynamic grass, 3=dynamic wheat.
// ============================================================================
#ifndef VEGETATION_H
#define VEGETATION_H
#include "gl_loader.h"
#include "render_ctx.h"
#include "scene.h"
#include "world.h"

typedef struct {
    const WorldState *world;            // world data: sizes, heights, field
    const RendererContext *rc;          // per-frame data (renderer.c)
    SceneUniforms su;                   // scene uniform set shared with the rest
    GLuint prog;                        // shared vegetation program
    GLuint vao_sg, vao_sw, vao_dg, vao_dw;   // static/dynamic x grass/wheat
    GLuint vbo_sg, vbo_sw, vbo_dg, vbo_dw;
    GLuint tex;                         // sprite array (0-2=grass, 3=dandelion, 4-6=wheat)
    GLint u_time;                       // time (wind)
    GLint u_walk;                       // 1 = walking (light wind)
    GLint u_dyn;                        // 1 = dynamic layer (lod0/lod1 only)
    GLint u_windboost;                  // wind: 1 = calm, 2 = rain (x2)
    GLint u_lodshift;                   // wheat LOD shift (mobile low-end)
    GLint u_depthonly;                  // 1 = z-prepass (depth only, no lighting)
    GLint u_simple;                     // 1 = simplified render: light loops/SSS/outline → 0
    GLint u_grassfall;                  // grass density falloff distance (m) — smooth thinning in the shader
    GLint u_grassfloor;                 // floor share/full (the density grass thins towards at the field edge)
} VegParams;

// Creates the Vegetation node: compiles the shader, loads sprites into a 2D array,
// builds 4 instancing VAOs, generates the static layer over the whole map.
// NULL on error. The field rectangle (g_field_*) must be set beforehand.
SceneNode *veg_make_node(const WorldState *w, const RendererContext *rc);

void veg_node_render(SceneNode *n, float dt);

// For the HUD: plant counts per layer.
// q: 0=static grass, 1=static wheat, 2=dynamic grass, 3=dynamic wheat.
int veg_count(int q);
void veg_set_visible_test(int v);

// Frees the CPU chunk arrays and the node's GL resources.
void veg_cleanup(SceneNode *n);

#endif

// Test toggles for performance measurements (FR_TEST in main.c)
void veg_set_static_main(int v);
void veg_set_dyn_main(int v);
