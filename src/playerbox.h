// ============================================================================
//  playerbox.h — character collision box in third-person mode
// ----------------------------------------------------------------------------
//  In third person the character is drawn as a full box of its physical
//  collision (1 m wide, 2 m tall — see camera.h): translucent filled faces
//  + yellow edges. In first person and in the shadow pass the node draws nothing.
// ============================================================================
#ifndef PLAYERBOX_H
#define PLAYERBOX_H
#include "gl_loader.h"
#include "render_ctx.h"
#include "scene.h"
#include "camera.h"

// Node parameters. cam — the character's physical state (position/view).
typedef struct {
    const RendererContext *rc;   // view basis, view/projection
    CameraParams *cam;           // character (position, yaw, crouch)
    GLuint prog;                 // box shader (world position + color)
    GLint u_viewproj, u_color;
    GLuint vao_fill, vbo_fill;   // translucent faces
    GLuint vao_line, vbo_line;   // box edges
} PlayerBoxParams;

// Creates the PlayerBox node. cam — the Camera node's parameters (live pointer).
SceneNode *playerbox_make_node(CameraParams *cam, const RendererContext *rc);

void playerbox_node_render(SceneNode *n, float dt);
void playerbox_cleanup(SceneNode *n);

#endif
