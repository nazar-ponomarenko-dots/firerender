// ============================================================================
//  ecs.h — data-oriented ECS on top of the engine (entity IDs + components + systems)
// ----------------------------------------------------------------------------
//  Why: SceneNode/Entity is an object model (pointers, virtual methods via
//  function pointers). ECS adds a parallel "data over code" layer: an entity
//  = an integer ID, a component = a POD struct in a per-type pool, a system
//  = a pass over an array. This gives cheap massive objects (fauna, particles,
//  triggers) and predictable cache. There is no rendering here: the link to
//  the scene lives in ecs_sync.c, nodes still do the drawing.
//
//  Handles: an ID is never exposed outward as a pointer — only a handle
//  (serial<<16|num, unsigned assembly, 0 = "none"), as in entity.c. A dead or
//  reused slot yields a handle for which ecs_get returns NULL.
//
//  Memory: EVERYTHING is static (static slot pools). Not a single malloc —
//  neither in a frame nor at registration. Capacity is fixed; overflow —
//  a log message and NULL, not array growth (the engine's fail-soft contract).
// ============================================================================
#ifndef ECS_H
#define ECS_H

#include <stdint.h>

typedef int EcsId;                 // <0 — invalid ID

#define ECS_MAX_ENTS      2048     // live entities
#define ECS_MAX_COMPONENTS 64      // component types (1 bit each in the mask)
#define ECS_MAX_SYSTEMS   32

// Component registration: name for ecs_list, size — size of the POD struct.
// id — a small index (0..), assigned in registration order; NOT usable as a
// pointer and does NOT survive ecs_shutdown. 0 when the registry is full/on error.
int ecs_register_component(const char *name, int size);

// System: calls fn(id, ud) for every live entity whose mask contains
// all. Order — ascending by order (equal ones — by registration).
typedef void (*EcsSystemFn)(EcsId id, void *ud);
void ecs_register_system(const char *name, uint64_t all, int order,
                         EcsSystemFn fn, void *ud);

// Entity. ecs_create reserves an ID without components; a component can only
// be removed explicitly (ecs_detach) — on ecs_kill the components are freed.
EcsId        ecs_create(void);
void         ecs_kill(EcsId id);          // idempotent
int          ecs_is_alive(EcsId id);
uint64_t     ecs_mask(EcsId id);

// Components. Attaching initializes the slot with zeros; detach clears it.
// On entity death all its components are detached automatically.
// The pointer from ecs_get is VALID until the component is detached and the
// entity stays alive: after slot reuse it may point into someone else's
// component — across Lua/console boundaries keep the handle and check ecs_owner().
int   ecs_attach(EcsId id, int comp);
void  ecs_detach(EcsId id, int comp);
void *ecs_get(EcsId id, int comp);        // NULL — component not attached
int   ecs_has(EcsId id, int comp);
EcsId ecs_owner(const void *comp_ptr, int comp); // -1 — pointer not from the pool

// Stable external handles (for Lua/console/saves).
int   ecs_handle(EcsId id);               // 0 = invalid
EcsId ecs_get_by_handle(int handle);      // -1 = entity gone/reused

// Run the systems in order. Called once per logic tick (game.c).
void  ecs_update(float dt);

// --- Debug/console ---
int   ecs_count(void);                    // live entities
int   ecs_system_count(void);             // registered systems
int   ecs_component_types(void);          // occupied slots of the component registry
int   ecs_component_count(int comp);      // entities with the given type
const char *ecs_component_name(int comp);
void  ecs_list(void);                     // console output
void  ecs_shutdown(void);                 // kill everything (map change/exit)

// Console commands (handlers here, registration in cmdreg.c).
void cmd_ecs_list(const char *args);
void cmd_ecs_stats(const char *args);
void cmd_ecs_test(const char *args);

// Windowless selftest (FR_TEST=Sx calls it). Returns the number of failures.
int   ecs_selftest(void);

#endif
