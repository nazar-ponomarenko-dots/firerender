// ============================================================================
//  sun.c — sun: a frozen directional light source
// ============================================================================
#include "sun.h"

#include "linalg.h"
#include "mapdata.h"

#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>

SceneNode *sun_make_node(void)
{
    SunParams *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    // Default: sun at ~39° elevation (long shadows), azimuth slightly
    // east of south. normalize(0.25, 0.6, 0.7). If map/map.json defines
    // directional light (lights[]) — take direction/color/intensity from there.
    p->dir[0] = 0.25f; p->dir[1] = 0.6f; p->dir[2] = 0.7f;
    vec3_normalize(p->dir);
    p->color[0] = 1.0f; p->color[1] = 0.95f; p->color[2] = 0.85f;
    p->intensity = 1.0f;
    // Default shadows: same as the previous constants (4 terrain texels, 2 — grass)
    p->shadow_on = 1;
    p->shadow_bias = 4.0f;
    p->shadow_bias_veg = 2.0f;
    // All directional lights from JSON — the sun (index 0) and extra lights.
    // The array goes to the shader as an array: each light shades separately.
    p->light_count = 0;
    for (int i = 0; i < mapdata_light_count() && p->light_count < SUN_MAX_LIGHTS; i++) {
        const MapLight *l = mapdata_light(i);
        if (l->type != LIGHT_DIRECTIONAL) continue;
        int k = p->light_count;
        memcpy(p->light_dir[k], l->dir, sizeof p->light_dir[k]);
        memcpy(p->light_color[k], l->color, sizeof p->light_color[k]);
        p->light_intensity[k] = l->intensity;
        vec3_normalize(p->light_dir[k]);
        p->light_count++;
        if (k == 0) {
            memcpy(p->dir, l->dir, sizeof p->dir);
            memcpy(p->color, l->color, sizeof p->color);
            p->intensity = l->intensity;
            p->shadow_on = l->shadow_on;
            p->shadow_bias = l->shadow_bias;
            p->shadow_bias_veg = l->shadow_bias_veg;
            vec3_normalize(p->dir);
            SDL_Log("Sun: '%s' dir=(%.2f %.2f %.2f) color=(%.2f %.2f %.2f) "
                    "int=%.2f shadows=%d bias=%.2f/%.2f",
                    l->name, p->dir[0], p->dir[1], p->dir[2],
                    p->color[0], p->color[1], p->color[2], p->intensity,
                    p->shadow_on, p->shadow_bias, p->shadow_bias_veg);
        }
    }
    SDL_Log("Lights total: %d (directional)", p->light_count);
    const float pos[3] = { 0, 100, 0 };        // high above the world
    const float size[3] = { 1, 1, 1 };
    SceneNode *node = node_new("Sun", "DirectionalLight3D", pos, size, p,
                               sun_node_update, NULL);
    if (!node) free(p);
    return node;
}

void sun_node_update(SceneNode *n, float dt)
{
    (void)n; (void)dt;
    // The sun is FROZEN: direction is set once at initialization
    // (sun->dir in sun_make_node). To make it walk across the sky again —
    // uncomment the lines below.
    // SunParams *p = n->params;
    // p->t += dt;
    // float a = p->t * 0.008f;              // slowly moves across the sky (~0.5 deg/s)
    // p->dir[0] = sinf(a) * 0.5f;
    // p->dir[1] = 0.7f;
    // p->dir[2] = cosf(a) * 0.5f;
    // vec3_normalize(p->dir);
}
