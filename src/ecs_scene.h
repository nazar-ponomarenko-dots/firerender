// ============================================================================
//  ecs_scene.h — bridge between the scene tree and the ECS (see ecs.h)
// ----------------------------------------------------------------------------
//  Each scene node gets a paired ECS entity: a NodeRef component (pointer to
//  the node + parent node -> ECS parent + tree generation) and a Transform
//  component (pos/rot/scl + modified bits). Per bridge tick:
//  pull (nodes -> ECS, clears modified) -> ecs_update (systems move
//  transforms and set modified) -> push (flagged ECS -> nodes). Call it
//  AFTER node_update_tree and BEFORE node_update_transforms so world matrices
//  for rendering are computed from the final positions.
//
//  Staleness: on map change nodes die together with the tree. The bridge keeps
//  a generation counter (gen): it is incremented before each run, components
//  with a foreign gen are recreated, "orphans" (node dead, entity alive) are
//  killed. No dangling pointers: gen is checked before dereferencing node.
//
//  Pure ECS entities (without NodeRef) are left alone by the bridge — they
//  live their own life.
// ============================================================================
#ifndef ECS_SCENE_H
#define ECS_SCENE_H

#include "scene.h"
#include "ecs.h"
#include <stdint.h>

typedef struct { SceneNode *node; EcsId parent_id; int gen; } EcsNodeRef;

#define ECS_TF_POS  1u
#define ECS_TF_ROT  2u
#define ECS_TF_SCL  4u
typedef struct {
    float pos[3], rot[3], scl[3];
    uint32_t modified;                 // ECS_TF_* bits — set from systems
} EcsTransform;

// Boots the bridge: component registration. Idempotent.
void ecs_scene_init(void);

// Master switch (cvar ecs_enable). A disabled bridge does NOTHING per frame.
void ecs_scene_enable(int on);
int  ecs_scene_enabled(void);

// New generation (before the first run on a new map).
void ecs_scene_new_generation(void);

// One bridge tick: pull -> ecs_update -> push. root — the scene root (g->root).
void ecs_scene_frame(SceneNode *root, float dt);

// Kill all synchronized entities (map unload/exit).
void ecs_scene_shutdown(void);

// Bridge component ids (0 — not initialized).
int ecs_comp_noderef(void);
int ecs_comp_transform(void);

// "node-entity" mask: NodeRef & Transform — for ecs_register_system.
uint64_t ecs_scene_mask(void);

// Node -> its ECS entity, creating the pair if missing; -1 — bridge dead.
EcsId ecs_scene_ensure(SceneNode *n);

// The node's ECS entity without creating it; -1 — not synchronized.
EcsId ecs_scene_id_of(const SceneNode *n);

// Bridge entity transform; NULL — no component.
EcsTransform *ecs_scene_transform(EcsId id);

// Mark that a system changed fields (ECS_TF_* flags).
void ecs_scene_mark(EcsId id, uint32_t flags);

// Check bridge connectivity on a live tree: every node has a pair,
// transforms match, no orphans. mismatch==0 — connectivity OK.
void ecs_scene_verify(SceneNode *root, int *nodes, int *synced, int *mismatch);

// One-shot verification on the next tick (FR_TEST 'V').
void ecs_scene_verify_once(void);

// Statistics for ecs_list/the debug panel.
int  ecs_scene_synced(void);
int  ecs_scene_generation(void);

// Console command (registered in cmdreg.c).
void cmd_ecs_verify(const char *args);

#endif
