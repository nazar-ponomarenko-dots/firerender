// ============================================================================
//  selection.h — 3D object selection: bounding-box frames around scene nodes
// ----------------------------------------------------------------------------
//  Debug mode (key G): every 3D scene node gets its bounding box drawn
//  around it (pos ± size/2) — like the selection outline in the Godot editor.
//  Shows sizes/positions of Sun, Terrain, Vegetation, FieldArea, Camera.
//  Rendered with the same shader as the field frame (shaders/field.*): world
//  position + uniform color, only GL_LINES and 12 box edges per node.
// ============================================================================
#ifndef SELECTION_H
#define SELECTION_H
#include "gpu.h"
#include "render_ctx.h"
#include "scene.h"

typedef struct {
    GpuProg prog;                  // shader program
    GpuVao vao;                    // edge vertex array
    GpuBuf vbo;                    // vertex buffer (rewritten every frame)
    GpuLoc u_viewproj, u_color;
    const RendererContext *rc;     // view/projection matrices
    SceneNode *root;               // tree root: used to find 3D nodes
} SelParams;

// Creates the Selection node (no CanvasItem2D style: it draws in the world).
SceneNode *selection_make_node(SceneNode *root, const RendererContext *rc);

void selection_node_render(SceneNode *n, float dt);

// Toggle selection frames (key G, together with the HUD).
void selection_set_on(int on);
int selection_is_on(void);

// Frees the node's GL resources.
void selection_cleanup(SceneNode *n);

#endif
