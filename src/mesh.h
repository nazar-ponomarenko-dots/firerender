// ============================================================================
//  mesh.h — static meshes (prop_static): .obj loading and render node
// ----------------------------------------------------------------------------
//  Phase 4: the engine can load 3D geometry from Wavefront .obj and place it
//  as a MeshInstance3D node in the scene. Unlike tree billboards, the mesh is
//  lit FOR REAL: vertex normals, Lambertian diffuse, specular,
//  cascaded shadows, fog (shaders/mesh.vs + mesh.fs).
//
//  Loader: v/vt/vn + triangles and quads (fan triangulation).
//  Vertices are EXPANDED per corner (each face gets its own vertices), so the
//  indices are trivial: we draw glDrawArrays(GL_TRIANGLES). The simplification
//  is justified for static decorations — at the cost of memory, but no complex
//  indexing.
// ============================================================================
#ifndef MESH_H
#define MESH_H
#include "gl_loader.h"
#include "scene.h"
#include "render_ctx.h"
#include "material.h"

typedef struct {
    float *verts;      // 3 * vcount  (positions, expanded per corner)
    float *normals;    // 3 * vcount  (or NULL if the OBJ has no vn)
    float *uvs;        // 2 * vcount  (or NULL if the OBJ has no vt)
    int vcount;        // total vertices = tri_count * 3
    int tri_count;     // number of triangles
    // GL resources (created on the first render)
    GLuint vao, vbo_pos, vbo_nrm, vbo_uv;
    int ready;         // 1 — geometry uploaded to GL
} Mesh;

// Loads a .obj via resources_read_text. Returns NULL on error.
// Free with mesh_free.
Mesh *mesh_load_obj(const char *path);

// Loads a .obj from a memory buffer (for tests/selftest, no file).
// name — name for the logs.
Mesh *mesh_load_obj_mem(const char *name, const char *src);

// Frees the CPU data and GL objects of the mesh.
void mesh_free(Mesh *m);

// Ensures the mesh geometry is uploaded to GL (idempotent). Called in render.
void mesh_gpu_upload(Mesh *m);

// Parameters of a mesh instance node.
typedef struct {
    const RendererContext *rc;
    Mesh *mesh;                  // shared geometry (the node does NOT own it — no free)
    GLuint prog;                 // shared shader mesh.vs+mesh.fs
    SceneUniforms su;            // shared scene uniform set
    Material mat;                // material (tint/blend/UV, material.h)
    MaterialLocs ml;             // its uniform handles
    GLint u_model, u_tex;        // own model matrix and texture
    GLuint tex;                  // albedo texture
    float scale;                 // uniform scale
} MeshLocs;

// Creates a MeshInstance3D node: loads the .obj, takes the texture tex_path
// (or img/notfound.png) and attaches a node at position pos with scale.
// Returns NULL if loading fails. The node is NOT attached to a parent (node_add).
SceneNode *mesh_make_node(const char *obj_path, const float pos[3],
                          float scale, const char *tex_path,
                          const RendererContext *rc);

void mesh_node_render(SceneNode *n, float dt);

#endif
