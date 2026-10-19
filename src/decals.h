// ============================================================================
//  decals.h — terrain decals (projected textured quads)
// ----------------------------------------------------------------------------
//  A decal is a flat quad "lying" on the terrain: each corner's height is
//  taken from the heightmap (9 samples per decal — 3x3) so it follows slopes
//  smoothly. Polygon offset (glPolygonOffset) instead of manual displacement —
//  no z-fighting and no floating.
//  API:
//    decal_place(x, z, hw, hd, rot, tex, r,g,b,a)      — offset=0.03f
//    decal_place_ex(x, z, hw, hd, rot, tex, r,g,b,a, off)
//    decals_clear()
// ============================================================================
#ifndef DECALS_H
#define DECALS_H

#include "gl_loader.h"
#include "render_ctx.h"
#include "scene.h"
#include "world.h"

#define DECALS_MAX 256

// Adds a decal on the terrain (offset=0.03f — small standoff).
void decal_place(float x, float z, float half_w, float half_d, float rot,
                 const char *tex_path, float r, float g, float b, float alpha);

// Same, with an explicit offset (m) from the terrain (0.0f = flush).
void decal_place_ex(float x, float z, float half_w, float half_d, float rot,
                    const char *tex_path, float r, float g, float b,
                    float alpha, float offset);

// Creates the Decals node (quads on the terrain). NULL on error.
SceneNode *decals_make_node(const WorldState *w, const RendererContext *rc);

// Clears all decals (map change / world unload).
void decals_clear(void);

// Renders all decals (after terrain, before objects). One draw per decal.
void decals_draw(const RendererContext *rc);

// Frees the node's GL resources.
void decals_cleanup(SceneNode *n);

#endif