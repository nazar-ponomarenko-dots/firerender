// ============================================================================
//  material.h — surface material: unified description of how a sprite renders
// ----------------------------------------------------------------------------
//  Refined MapObjectRender (item 14): blend/cull/depth_write/wrap/filter/
//  tint/alpha/UV animation — DATA, not code in the render loop. GL state and
//  shader uniforms are applied by material_bind (pattern "the child supplies
//  the shader with uniforms"): a new texture/UV feature goes into Material +
//  material_bind — the render loop in objects.c stays untouched.
//
//  Defaults (for objects without a material block in JSON) are set by material_defaults.
// ============================================================================
#ifndef MATERIAL_H
#define MATERIAL_H
#include "gl_loader.h"

typedef enum {
    MAT_BLEND_ALPHATEST = 0,   // alpha test: transparent background discarded (default)
    MAT_BLEND_ALPHA,           // regular blending (translucency)
    MAT_BLEND_ADDITIVE,        // additive (glow, fire)
    MAT_BLEND_OPAQUE           // opaque (alpha ignored)
} MatBlend;

typedef enum {
    MAT_CULL_NONE = 0,         // two-sided (default)
    MAT_CULL_BACK,             // cull the back face
    MAT_CULL_FRONT             // cull the front face
} MatCull;

typedef enum {
    MAT_WRAP_CLAMP = 0,        // edges repeat the border color (default)
    MAT_WRAP_REPEAT,           // tiling
    MAT_WRAP_MIRROR            // mirrored tiling
} MatWrap;

typedef enum {
    MAT_FILTER_LINEAR = 0,     // smoothing (default)
    MAT_FILTER_NEAREST,        // pixelated (retro, sprites)
    MAT_FILTER_MIPMAP          // mip levels (distant objects stay sharp)
} MatFilter;

typedef struct {
    float tint[3];       // color tint (1 = unchanged)
    float alpha;         // opacity 0..1 (for alpha/additive)
    int blend;           // MatBlend
    int cull;            // MatCull
    int depth_write;     // 1 — write to the depth buffer
    int wrap;            // MatWrap (requires its own texture)
    int filter;          // MatFilter
    float tile[2];       // UV tiling (1 = one texture tile)
    float offset[2];     // UV offset in tiles
    float scroll[2];     // UV animation: tiles per second
    int fixed_size;      // 1 — width/height in meters (not from the texture)
    float width, height; // quad size, m (if fixed_size)
    int set;             // a material block was present in the JSON
} Material;

// Material defaults (for objects without a material block).
void material_defaults(Material *m);

// --- Material registry by name (data-driven, like the HL2 material library) -----
// A material can be registered by name once (from map.json, Lua or code)
// and then retrieved by constant: the renderer doesn't duplicate parameters.
// The pattern resembles IMaterialSystem::FindMaterial from Source.
#define MATERIALS_MAX 32

// Registers/overwrites a material by name (copy).
void material_register(const char *name, const Material *m);

// Looks up a material by name; NULL — no such material.
const Material *material_find(const char *name);

// Returns the material by name, OR defaults if not registered.
// out is always filled (material -> defaults). Returns 1 if found.
int material_get(const char *name, Material *out);

// Number of registered materials (for matlist / debugging).
int material_count(void);
const char *material_name(int i);

// Clears the registry (on world unload).
void material_clear_all(void);

// Material uniform handles in the shader (looked up once; optional ones = -1).
typedef struct {
    GLint u_tint, u_blend, u_alpha;
    GLint u_uvtile, u_uvoffset, u_uvscroll, u_time;
} MaterialLocs;

void material_locs_init(MaterialLocs *ml, GLuint prog);

// Applies the material: GL state (blending/culling/depth) and uniforms
// (tint/alpha/blend/UV animation). Call per instance after binding the
// program. time — elapsed time (for scroll animation).
void material_bind(const Material *m, const MaterialLocs *ml, float time);

// Same, but skips a fully identical material within the same program.
// cache holds a copy of the last Material; valid is reset when the program changes.
void material_bind_cached(const Material *m, const MaterialLocs *ml, float time,
                          Material *cache, int *valid);

#endif
