// ============================================================================
//  glb_anim.h — skeletal animation (skin) from binary glTF 2.0 (.glb)
// ----------------------------------------------------------------------------
//  Extends the static glb.c with animation: skins, per-vertex weights/joints
//  (JOINTS_0 / WEIGHTS_0) and animation clips (animations -> channels+samplers).
//
//  Difference from glb.c: the static path BAKES geometry into world
//  coordinates, which is unacceptable for animation — here geometry stays in
//  the BIND POSE (mesh-local coordinates), and bone rotations are applied on
//  the GPU (skinning in the skinned.vs shader via uBones).
//  Bones are packed per-primitive: each draw carries only the jointMatrixes it
//  actually uses (≤64), so models with ANY number of skin bones work on all of
//  GL 3.3 (guaranteed minimum — 64 bones per shader).
//
//  Rendering rule is the same as in glb.c: PSEUDO-PBR 2004 (material.h
//  material + scalar specular), not real PBR.
// ============================================================================
#ifndef GLB_ANIM_H
#define GLB_ANIM_H
#include "gl_loader.h"
#include "scene.h"
#include "render_ctx.h"
#include "material.h"

// --- glTF hierarchy node -------------------------------------------------------
typedef struct {
    int parent;                 // parent index (-1 = root)
    int children[64];
    int nchildren;
    char name[64];              // node/bone name from the GLB (for attach by name)
    float trs[16];              // static local transform (if given as a matrix)
    int mesh;                   // mesh index (-1 — no geometry)
    // BASE (from the file) TRS components — animation is layered on top of them
    float btrans[3]; float bquat[4]; float bscl[3];
    int has_matrix;             // 1 — static transform given as a matrix
} AnimNode;

// --- Skin ----------------------------------------------------------------------
typedef struct {
    int joint_count;
    int *joints;                // node indices
    float *inv_bind;            // 16 * joint_count (inverseBindMatrices)
} AnimSkin;

// --- Animation ------------------------------------------------------------------
typedef enum { ANIM_LINEAR = 0, ANIM_STEP, ANIM_CUBIC } AnimInterp;

typedef struct {
    int node;                   // animated node
    int path;                   // 0=translation, 1=rotation, 2=scale
    int sampler;
} AnimChannel;

typedef struct {
    float *times; int nkeys;    // key times (seconds)
    float *values;              // LINEaR/STEP: per key comp*count;
                                // CUBICSPLINE: per key 3*comp (in-tangent,
                                // value, out-tangent) — see sample_* in glb_anim.c
    int comp;                   // 3 (trans/scale) or 4 (rotation)
    AnimInterp interp;
} AnimSampler;

typedef struct {
    char name[64];                  // clip name (from the GLB) — selectable by name
    AnimChannel *channels; int nch;
    AnimSampler *samplers; int ns;
    float duration;
} AnimClip;

// --- Geometry of one mesh primitive (in bind pose, local coordinates) -------
typedef struct {
    float *verts, *normals, *uvs;      // 3/3/2 per vertex
    int vcount;
    unsigned int *indices; int index_count;
    // skinning
    int *bone_idx;                     // 4 * vcount (JOINTS_0: ubyte/us-ort
    float *bone_w;                     // 4 * vcount (WEIGHTS_0)
    // material (pseudo-PBR 2004)
    float tint[3]; float alpha; int blend; int cull;
    float tile[2]; float specular;
    unsigned char *img; int iw, ih;
    int wrap, filter;
    char matname[64];              // material name (GROUND filter in ragdoll)
} AnimPrim;

// Instance (node, prim): each node with a mesh creates one draw primitive.
// Several nodes may reference the same primitive (instancing).
typedef struct { int node, prim, skin; } AnimInstance;

typedef struct {
    AnimPrim *prims; int nprim;
    AnimNode *nodes; int nnode;
    int *skin_nodes; int nskin_nodes;   // node indices that carry a skin
    int *skin_of_node;                  // nnode: -1 or skin index
    AnimSkin *skins; int nskin;
    AnimClip *clips; int nclip;
    AnimInstance *instances; int ninst;
    char err[128];
} AnimGlb;

// Parses an animated .glb (CPU). Returns NULL on error (err). Free with
// glb_anim_free.
AnimGlb *glb_anim_parse(const char *path);

void glb_anim_free(AnimGlb *a);

// Creates an AnimInstance3D node (GPU buffers/textures/program for skinned.vs),
// position pos, scale scale. The node is NOT attached (node_add is on you).
// If the model has no skin/animation — returns a static node (uHasSkin=0).
SceneNode *glb_anim_make_node(const char *path, const float pos[3], float scale,
                              const RendererContext *rc);

// Does the model have skin+animation (so commands can pick the load path)?
// Quick peek into the JSON header, without full parsing.
int glb_anim_has_anim(const char *path);

// --- Ragdoll (Verlet): particles = skeleton bones -----------------------------------
// Enables physics on an animated model: bones become particles,
// animation is displaced by physics (bone = shortest turn toward its particle).
// hang=1 — the origin point is pinned (the model hangs and sags), 0 — it falls.
// 0 — success. The physics step for all ragdolls — glb_anim_ragdoll_step_all from
// the fixed tick (main.c), BEFORE the scene update.
int glb_anim_ragdoll_enable(SceneNode *n, int hang);
void glb_anim_ragdoll_step_all(float dt);
void glb_anim_ragdoll_kick(SceneNode *n, const float vel[3]); // kick all particles
void glb_anim_ragdoll_disable(SceneNode *n);
// Debug: world positions of all ragdolls' particles (for point rendering).
int glb_anim_ragdoll_debug_particles(float *out_xyz, int max_parts);
// Attach point of the LAST enabled ragdoll (for camera auto-aim
// in tests). 1 — written, 0 — no active ragdolls.
int glb_anim_ragdoll_aim(float out_pos[3]);

// --- Animation clips (a GLB may contain several) --------------------------------
int glb_anim_clip_count(SceneNode *n);            // how many clips the node has
const char *glb_anim_clip_name(SceneNode *n, int idx); // clip name (NULL if none)
int glb_anim_clip_find(SceneNode *n, const char *name); // index by name, -1
void glb_anim_set_clip(SceneNode *n, int idx);    // switch (time resets)
const char *glb_anim_current_clip(SceneNode *n);  // active clip name (or NULL)
void glb_anim_set_speed(SceneNode *n, float speed); // playback time multiplier
void glb_anim_set_paused(SceneNode *n, int paused); // 1 — pause, 0 — play
void glb_anim_set_loop(SceneNode *n, int loop);     // 1 — loop, 0 — play to the end and stop
void glb_anim_set_time(SceneNode *n, float t);      // set the clip time (seconds) — "from a frame"
float glb_anim_time(SceneNode *n);                  // current time of the active clip

// --- Registry of all loaded animated nodes (for the console tool anim) ----
int glb_anim_count(void);                           // how many animated nodes are loaded
SceneNode *glb_anim_by_index(int idx);              // node by registry index (0..count-1)
const char *glb_anim_path(SceneNode *n);            // GLB path (NULL for foreign nodes)
float glb_anim_scale_get(SceneNode *n);
int glb_anim_paused_get(SceneNode *n);
int glb_anim_loop_get(SceneNode *n);
float glb_anim_speed_get(SceneNode *n);
void glb_anim_set_enabled(SceneNode *n, int on);    // 0 — hide and freeze, 1 — show
int glb_anim_enabled(SceneNode *n);
void glb_anim_set_pos(SceneNode *n, const float pos[3]); // teleport (instant)
void glb_anim_move_to(SceneNode *n, const float pos[3], float speed); // smooth movement
void glb_anim_stop_move(SceneNode *n);
void glb_anim_set_scale(SceneNode *n, float s);     // model scale
void glb_anim_remove_node(SceneNode *n);            // detach and unload the node

// --- Attach points: skeleton bones (see attach.h) ------------------------------
//  An AnimInstance3D node exposes bone matrices relative to itself in world
//  space through the scene.h hook (attach_fn). Slot index = glTF NODE index
//  (bone), so you can attach to any node of the model hierarchy, not only a
//  skin joint.
int glb_anim_bone_count(SceneNode *n);                 // 0 — not a skeletal model
const char *glb_anim_bone_name(SceneNode *n, int idx); // bone name (or NULL)
int glb_anim_bone_find(SceneNode *n, const char *name);// index by name, -1

void glb_anim_node_render(SceneNode *n, float dt);

#endif
