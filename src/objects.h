// ============================================================================
//  objects.h — objects from map/map.json (objects[]): textured billboards
// ----------------------------------------------------------------------------
//  Each object is a camera-facing quad with a PNG texture (model = path to the
//  file, e.g. "img/tree.png"). Position — pos [x,y,z] from JSON; y <= 0 means
//  the height is taken from the terrain. Scale — scale (default 1): height = 5*scale.y.
//  Lighting — all directional lights + the sun's CSM shadows + fog,
//  same as vegetation.
// ============================================================================
#ifndef OBJECTS_H
#define OBJECTS_H
#include "gl_loader.h"
#include "render_ctx.h"
#include "scene.h"
#include "world.h"

// Creates the Objects node: loads object textures from map.json, compiles
// the shader, builds one shared quad. NULL on error.
SceneNode *objects_make_node(const WorldState *w, const RendererContext *rc);

void objects_node_render(SceneNode *n, float dt);

// Object collisions (collision=1): pushes the point (x,z) out of the
// radius+pr cylinders around object bases. Call after player movement (not while flying).
void objects_collide(float *x, float *z, float pr);

// Frees the node's GL resources.
void objects_cleanup(SceneNode *n);

#endif
