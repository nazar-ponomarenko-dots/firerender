// ============================================================================
//  attach.h — attaching nodes to models (including skeleton BONES)
// ----------------------------------------------------------------------------
//  "Flexibility" of 3D models: anything can be hooked onto a character
//  (or any model/node) — a 2D sprite, a static mesh (.obj/.glb), another
//  animated model, an effect emitter. The child travels with the parent,
//  and if a BONE is given — with that bone (sword in hand, hat on head,
//  icon over the head).
//
//  Mechanics (see scene.h): the parent has an attach_fn hook returning the
//  attachment-point matrix by index; the child has attach_slot (the index) and the
//  attach_upright flag ("do not rotate together with the bone"). node_update_transforms
//  inserts that matrix between the parent's world matrix and the child's
//  local matrix. The child itself stays an ordinary node: its pos/rot/scl are
//  the OFFSET relative to the attachment point.
//
//  Slots:
//    ATTACH_ORIGIN (-1)  — attach to the node itself (no bone);
//    >= 0                — skeleton bone index (for AnimInstance3D).
//  Bone name -> index: attach_bone_index (by the node name from the GLB).
// ============================================================================
#ifndef ATTACH_H
#define ATTACH_H
#include "render_ctx.h"
#include "scene.h"

#define ATTACH_ORIGIN (-1)

// Attaches child to parent. bone — the model bone/node name (NULL or "" —
// attach to the parent's origin). offset — displacement from the point, m
// (may be NULL). upright: 1 — cancel bone rotation (the sprite stays
// vertical), 0 — inherit the rotation fully.
// Returns 0 on success, -1 if the bone was not found (the child is still
// attached to the origin — the object is not lost).
int attach_node(SceneNode *parent, SceneNode *child, const char *bone,
                const float offset[3], int upright);

// Detaches a node: it stays in the tree, but the attachment point is reset
// (child->attach_slot = ATTACH_ORIGIN).
void attach_detach(SceneNode *child);

// Bone index by name in the model (AnimInstance3D). -1 — no such bone.
int attach_bone_index(SceneNode *model, const char *bone);

// Bone count of a model (0 if not skeletal).
int attach_bone_count(SceneNode *model);

// Bone name by index (NULL if none).
const char *attach_bone_name(SceneNode *model, int idx);

// --- Sprite as a node (Sprite3D) -----------------------------------------------
//  A textured quad that can be hooked onto anything. Modes:
//    billboard = 1 — always faces the camera (icons, labels, lights);
//    billboard = 0 — flat quad in the node's space (stickers, emblems, signs).
//  Size is given in meters (w, h). Material — a regular Material (material.h),
//  so blending, tint, alpha, UV animation are all available.
SceneNode *sprite_make_node(const char *tex_path, float w, float h,
                            int billboard, const RendererContext *rc);

// Sprite configuration after creation (works on a live node too).
void sprite_set_size(SceneNode *n, float w, float h);
void sprite_set_tint(SceneNode *n, float r, float g, float b, float alpha);
void sprite_set_billboard(SceneNode *n, int on);
// Frame from an atlas: grid cols x rows, index — frame number (from top-left).
void sprite_set_frame(SceneNode *n, int cols, int rows, int index);
// Glow: 1 — additive blending (sparks, magic), 0 — regular alpha.
void sprite_set_additive(SceneNode *n, int on);
// Unlit: 1 — pure texture * tint (icons, labels over the head).
void sprite_set_unlit(SceneNode *n, int on);

void sprite_node_render(SceneNode *n, float dt);

#endif
