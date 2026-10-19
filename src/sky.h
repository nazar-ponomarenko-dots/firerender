// ============================================================================
//  sky.h — skybox (Skybox3D) built from the panorama img/skybox.png
// ----------------------------------------------------------------------------
//  Full-screen triangle; the ray direction for every sky pixel is
//  reconstructed by the fragment shader from the camera basis (see sky.fs).
//  The module also computes the fog color from the middle of the panorama —
//  the horizon matches the sky (sky_fog_color uses terrain and vegetation).
// ============================================================================
#ifndef SKY_H
#define SKY_H
#include "render_ctx.h"
#include "scene.h"

// Creates the Sky node (compiles shaders, loads the panorama). NULL on error.
SceneNode *sky_make_node(const RendererContext *rc);

void sky_node_render(SceneNode *n, float dt);

// Fog color (average over a horizon band of the sky panorama). out[3].
void sky_fog_color(float out[3]);

// Sky panorama texture + its tint (for GI: sky light on ray misses).
// 0 if the sky isn't created yet (menu) — GI tints misses with a fog gradient.
GLuint sky_panorama(float tint[3]);

// Frees the node's GL resources (program, texture, VAO/VBO).
void sky_cleanup(SceneNode *n);

#endif
