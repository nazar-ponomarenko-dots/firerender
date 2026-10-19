// ============================================================================
//  glb.h — binary glTF 2.0 (.glb) loader for static meshes (prop_static)
// ----------------------------------------------------------------------------
//  The user loves .glb, not .obj: the engine can load full binary glTF —
//  node hierarchies with transforms (TRS/matrices), meshes with primitives
//  (POSITION/NORMAL/TEXCOORD_0 + indices), materials and embedded textures
//  (bufferView data dereferenced). Everything is "baked" into world coordinates.
//
//  RENDER RULE: our engine is PSEUDO-PBR 2004 (like HL2/Source, Far Cry 1),
//  we have no real PBR (metal/roughness). So genuine PBR data from the GLB is
//  NOT reproduced verbatim but RE-FORMATTED into our material (material.h)
//  plus a scalar specular:
//    baseColorFactor  -> tint;   alphaMode -> blend/alpha
//    roughnessFactor  -> specular (smoothness); metallic -> bonus to specular
//    emissiveFactor   -> addition to tint;  normalTexture -> (so far) ignored
//  This is a deliberate "in the style of 2004" assumption, not a shortcoming.
// ============================================================================
#ifndef GLB_H
#define GLB_H
#include "gl_loader.h"
#include "scene.h"
#include "render_ctx.h"
#include "material.h"

// One geometry bucket grouped by material (already in world coordinates,
// indexed). geometry::CPU, no GL — for parsing and tests.
typedef struct {
    float *verts, *normals, *uvs;    // 3 / 3 / 2 per vertex
    int vcount;                      // unique vertices
    unsigned int *indices;           // 3 * tri_count
    int index_count;                 // = 3*tri_count
    // Material re-formatted into PSEUDO-PBR 2004
    float tint[3], alpha;
    int blend;                       // MatBlend
    int cull;                        // MatCull
    float tile[2];                   // UV tiling
    float specular;                  // specular strength 0..1
    // Albedo texture (RGBA pixels, decoded by stb_image).
    unsigned char *img; int iw, ih;
    int wrap, filter;                // MatWrap / MatFilter
} GlbItem;

typedef struct {
    GlbItem *items;
    int count;
    char err[128];                   // error text (or empty)
} GlbModel;

// Parses a .glb: reads the file, decodes chunks, bakes the hierarchy into world
// coordinates, groups by material, decodes albedo. Does NOT touch GL —
// safe to call in selftests/headless. NULL on error (fills model->err).
// Free with glb_model_free.
GlbModel *glb_parse_file(const char *path);

void glb_model_free(GlbModel *m);

// Creates a GlbInstance3D node: loads the .glb, builds GL buffers and textures
// per bucket, attaches a node at position pos with scale scale.
// Returns NULL on error. The node is NOT attached to a parent (node_add).
SceneNode *glb_make_node(const char *path, const float pos[3], float scale,
                         const RendererContext *rc);

void glb_node_render(SceneNode *n, float dt);

#endif