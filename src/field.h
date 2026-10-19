// ============================================================================
//  field.h — wheat field (FieldArea3D)
// ----------------------------------------------------------------------------
//  Visually — a frame on the ground (5 vertices of a closed line, height taken
//  from the terrain). Defines where wheat grows: node coordinates = center,
//  size = extent. The world rectangle (g_field_*) is set by main.c before the
//  nodes are created.
// ============================================================================
#ifndef FIELD_H
#define FIELD_H
#include "gl_loader.h"
#include "render_ctx.h"
#include "scene.h"
#include "world.h"

typedef struct {
    GLuint prog, vao, vbo;         // rectangle outline
    GLint u_viewproj, u_color;
    const WorldState *world;       // terrain heights (the frame lies on the ground)
    const RendererContext *rc;     // view/projection matrices
} FieldParams;

// Creates the FieldArea node: field center and size, compiles the shader.
// center/size must match g_field_* (world globals).
SceneNode *field_make_node(const WorldState *w, const float *center,
                           const float *size, const RendererContext *rc);

void field_node_render(SceneNode *n, float dt);

// Frees the node's GL resources.
void field_cleanup(SceneNode *n);

#endif
