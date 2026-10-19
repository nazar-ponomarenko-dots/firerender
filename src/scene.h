// ============================================================================
//  scene.h — a Godot-style scene: everything on the scene is a Node
// ----------------------------------------------------------------------------
//  Mini node tree. Each node has: name, type, TRANSFORM (position, rotation,
//  scale), size and parameters. The engine traverses the tree every frame:
//  first update() on all nodes (logic, input, physics), then computes world
//  matrices (node_update_transforms), then render() (drawing).
//
//  TRANSFORM (like Node3D in Godot): pos + rot (radians, Y->X->Z order) +
//  scl. World matrix world = world(parent) * [attachment point] *
//  local(TRS) — node_update_transforms computes it and mesh renderers
//  read it (mesh/glb/glb_anim/sprite). Previously each renderer built the
//  matrix itself from pos and its own scale scalar — you could not rotate a
//  node or attach it to something.
//
//  ATTACHMENT POINTS: a parent can hand a child an extra matrix — e.g. the
//  BONE matrix of a skeleton. Then the child (sprite, mesh, another model)
//  moves along with the character's arm. The parent sets attach_fn, the
//  child sets attach_slot (point index; -1 = none). glb_anim.c provides the
//  hook implementation for skeletal models, attach.h — a convenient API.
// ============================================================================
#ifndef SCENE_H
#define SCENE_H

typedef struct SceneNode SceneNode;
typedef void (*NodeFn)(SceneNode *node, float dt);

// Parent hook: write into out[16] the parent-relative world matrix of
// attachment point slot (e.g. a skeleton bone). If the point doesn't exist,
// leave the identity matrix (out is already identity on entry).
typedef void (*NodeAttachFn)(SceneNode *parent, int slot, float out[16]);

struct SceneNode {
    const char *name;          // node name, as in Godot ("Sun", "Terrain"…)
    const char *type;          // type ("DirectionalLight3D", "MeshInstance3D"…)
    SceneNode *parent;         // parent (NULL for the root)
    SceneNode *first_child;    // first child
    SceneNode *last_child;     // last child (for appending at the end)
    SceneNode *next;           // next sibling
    float pos[3];              // position (in the parent's space)
    float rot[3];              // rotation, radians (Y-yaw, X-pitch, Z-roll)
    float scl[3];              // scale (defaults to 1,1,1)
    float world[16];           // world matrix (node_update_transforms)
    float size[3];             // node size (where applicable: world, minimap…)
    void *params;              // typed node parameters
    NodeFn update;             // called every frame before render
    NodeFn render;             // called every frame (draws itself)
    // Attachment points
    NodeAttachFn attach_fn;    // PARENT: yields the point matrix by index
    int attach_slot;           // CHILD: which parent point it occupies (-1 = none)
    int attach_upright;        // CHILD: 1 — cancel the point's rotation (position only)
};

SceneNode *node_new(const char *name, const char *type,
                    const float *pos, const float *size,
                    void *params, NodeFn update, NodeFn render);
void node_add(SceneNode *parent, SceneNode *child);

// Detach a node from its parent (the node itself and its params are NOT freed).
// Convenient before node_free_tree so as not to free the node twice.
void node_remove(SceneNode *node);

// Find a node by name in the subtree (DFS). NULL — not found.
SceneNode *node_find(SceneNode *root, const char *name);

void node_update_tree(SceneNode *n, float dt);

// Recomputes world matrices of the whole subtree (after update, before render).
// Call once per frame on the root.
void node_update_transforms(SceneNode *n);

// Node's local matrix from pos/rot/scl (for debugging and manual computations).
void node_local_matrix(const SceneNode *n, float out[16]);

// Node's world position from its world matrix.
void node_world_pos(const SceneNode *n, float out[3]);

void node_render_tree(SceneNode *n, float dt);
void node_free_tree(SceneNode *n);
void scene_print(SceneNode *n, int depth);

#endif
