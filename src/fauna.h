// ============================================================================
//  fauna.h — living map: fireflies, butterflies, birds (see fauna.c)
// ============================================================================
#ifndef FAUNA_H
#define FAUNA_H
#include "gl_loader.h"
#include "render_ctx.h"
#include "scene.h"

// Creates the fauna node from map.json (fauna[]). NULL if the map has no fauna.
SceneNode *fauna_make_node(const RendererContext *rc);

void fauna_node_update(SceneNode *n, float dt);
void fauna_node_render(SceneNode *n, float dt);

// Frees the node's GL resources.
void fauna_cleanup(SceneNode *n);

#endif
