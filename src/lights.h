// ============================================================================
//  lights.h — clustered lighting (Forward+): light registry and froxel grid
// ----------------------------------------------------------------------------
//  The engine used to be CLASSICAL forward: each shader got uniform arrays
//  (8 point + 4 spot) and iterated over ALL of them in EVERY fragment.
//  Cost = pixels x lights, so the limit was hard and low.
//
//  Now — CLUSTERED forward (as in Doom 2016 / UE "clustered forward"):
//   1) the screen is cut into tiles (CLUSTER_X x CLUSTER_Y), and the view along
//      depth — into CLUSTER_Z logarithmic layers. Result: a 3D grid of "froxels";
//   2) each frame the CPU sorts lights into the froxels they
//      actually hit (by the on-screen rect of their sphere of influence and by
//      their depth range), and writes two buffers: an index list + a per-froxel
//      (offset,count) range;
//   3) the fragment shader finds its froxel from gl_FragCoord and depth and
//      iterates ONLY the lights in it — typically 0..4 instead of all.
//  Result: hundreds of lights with almost no FPS drop, limit raised to LIGHT_MAX.
//
//  Light types (all with soft shadows based on the source size):
//    LT_POINT — point light (radius = physical bulb radius, gives a soft highlight);
//    LT_SPOT  — spotlight (inner/outer cone, smooth edge);
//    LT_RECT  — rectangular area light (window, screen, lightbox): lights
//               as an area, not a point — the highlight stretches along its shape;
//    LT_TUBE  — linear/tube light (fluorescent lamp, neon).
//  Directional lights (sun and ambient) stay separate — there are only a few
//  of them and they affect every pixel, no point in clustering them.
//
//  Requires GL >= 4.3 (SSBO in the fragment shader). If the context is missing —
//  lights_active() == 0, and shaders use the previous uniform path.
// ============================================================================
#ifndef LIGHTS_H
#define LIGHTS_H
#include "gl_loader.h"
#include "render_ctx.h"

// Froxel grid size. 16x9 tiles (16:9 ratio) x 24 depth layers.
#define CLUSTER_X 16
#define CLUSTER_Y 9
#define CLUSTER_Z 24
#define CLUSTER_COUNT (CLUSTER_X * CLUSTER_Y * CLUSTER_Z)
// Cap on lights per frame and on the total length of froxel lists.
#define LIGHT_MAX 256
#define CLUSTER_INDEX_MAX (CLUSTER_COUNT * 24)

typedef enum {
    LT_POINT = 0,
    LT_SPOT,
    LT_RECT,
    LT_TUBE
} LightType;

// A light in the world (CPU representation; goes into the SSBO packed per vec4).
typedef struct {
    int type;
    float pos[3];
    float range;             // influence radius, m (no light beyond it)
    float color[3];
    float intensity;
    float dir[3];            // direction (spot) / area normal (rect)
    float radius;            // light source radius, m: highlight and shadow softness
    float cos_outer;         // spot: cosine of the outer angle
    float cos_inner;         // spot: cosine of the inner angle (smooth edge)
    float up[3];             // rect: second axis of the area; tube: tube axis
    float size[2];           // rect: half-sizes (w, h); tube: (length/2, 0)
    int shadow;              // placeholder: the light casts shadows
} Light;

// --- Lifecycle -------------------------------------------------------------
// Creates the SSBO and checks capability. Call from renderer_setup.
void lights_init(void);
void lights_shutdown(void);

// 1 if the clustered path is available (GL 4.3+) and enabled by cvar r_clustered.
int lights_active(void);

// --- Filling (every frame, before the scene render) --------------------------
// Resets the frame's light list.
void lights_begin(void);

// Adds a light. Returns the index or -1 (limit hit).
int lights_add(const Light *l);

// Convenient constructors (fill defaults and add).
int lights_add_point(const float pos[3], const float color[3],
                     float intensity, float range, float radius);
int lights_add_spot(const float pos[3], const float dir[3],
                    const float color[3], float intensity, float range,
                    float angle_outer, float angle_inner, float radius);
int lights_add_rect(const float pos[3], const float dir[3], const float up[3],
                    const float color[3], float intensity, float range,
                    float half_w, float half_h);
int lights_add_tube(const float pos[3], const float axis[3],
                    const float color[3], float intensity, float range,
                    float half_len, float radius);

// Builds the froxel grid and fills the buffers. Call once per frame after
// all lights_add (renderer_ctx_fill). Requires the view/projection matrices from rc.
void lights_build(const RendererContext *rc);

// Binds the SSBO to points 3/4/5 and sends the grid uniforms to the program.
// Called from scene_uniforms_bind for every scene shader.
void lights_bind(GLuint prog);

// Finds the grid uniform handles in the program (cached internally per prog).
void lights_locs_init(GLuint prog);

// --- Statistics (HUD/profiler) -----------------------------------------------
int lights_count(void);              // lights in the current frame
int lights_cluster_fill(void);       // total length of froxel lists
int lights_cluster_max(void);        // max lights per froxel

#endif
