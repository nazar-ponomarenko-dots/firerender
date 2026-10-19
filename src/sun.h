// ============================================================================
//  sun.h — the sun (DirectionalLight3D)
// ----------------------------------------------------------------------------
//  Directional light: walks across the sky, provides light and shadow
//  direction. The direction is set once when the node is created (frozen).
// ============================================================================
#ifndef SUN_H
#define SUN_H
#include "scene.h"

#define SUN_MAX_LIGHTS 4      // max directional lights in the shader

typedef struct {
    float dir[3];        // sun (first directional light): direction to the light
    float color[3];      // color
    float intensity;     // strength
    float t;             // time (for the sky walk)
    int shadow_on;       // does the sun cast shadows? (from map/map.json)
    float shadow_bias;   // terrain shadow bias, in lateral texels
    float shadow_bias_veg; // vegetation shadow bias, in texels
    // ALL directional lights from map.json: the array goes into
    // separate uniforms, index 0 = the sun.
    int light_count;
    float light_dir[SUN_MAX_LIGHTS][3];
    float light_color[SUN_MAX_LIGHTS][3];
    float light_intensity[SUN_MAX_LIGHTS];
} SunParams;

// Creates the Sun node with the default direction (~39° above the horizon).
SceneNode *sun_make_node(void);

void sun_node_update(SceneNode *n, float dt);

#endif
