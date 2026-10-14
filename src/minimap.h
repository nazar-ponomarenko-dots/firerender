// ============================================================================
//  minimap.h — minimap (CanvasItem2D)
// ----------------------------------------------------------------------------
//  Map with the player dot and view direction + a yellow line pointing at
//  the sun. The texture is the world's map color (world tex_base).
// ============================================================================
#ifndef MINIMAP_H
#define MINIMAP_H
#include "gl_loader.h"
#include "render_ctx.h"
#include "scene.h"
#include "world.h"

typedef struct {
    GLuint prog, vao, vbo;         // program and buffer (data rebuilt every frame)
    GLint u_basemap, u_color, u_usetex, u_usemarker;
    GLuint tex_base;               // map texture
    GLuint tex_marker;             // player marker (img/player.png)
    const WorldState *world;       // world sizes (player position in uv)
    const RendererContext *rc;     // player position/view, sun
    int vw, vh;                    // window size in pixels
    int visible;                   // key G: show/hide the map
} MinimapParams;

// Creates the Minimap node (compiles the shader, builds the VAO). NULL on error.
SceneNode *minimap_make_node(const WorldState *w, GLuint tex_base,
                             const RendererContext *rc);

void minimap_node_render(SceneNode *n, float dt);

// Frees the node's GL resources (the map texture is shared — don't delete it).
void minimap_cleanup(SceneNode *n);

#endif
