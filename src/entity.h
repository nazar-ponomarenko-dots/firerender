// ============================================================================
//  entity.h — entities on top of the scene tree (like GoldSrc entities)
// ----------------------------------------------------------------------------
//  Entity = scene node + class + instance data. The class defines events:
//    spawn  — right after creation (after EV_ENTITY_SPAWN);
//    think  — every frame from the main loop;
//    kill   — before removal (and EV_ENTITY_KILL).
//  entity_spawn creates a new Node3D node in the scene root; entity_bind
//  attaches an entity to an ALREADY EXISTING node (camera, world).
//  This is the foundation for Lua (phase 4): script classes will register
//  the same events as native ones.
// ============================================================================
#ifndef ENTITY_H
#define ENTITY_H
#include "scene.h"

typedef struct Entity Entity;
typedef void (*EntityThinkFn)(Entity *e, float dt);

typedef struct {
    const char *classname;       // class name (registration key)
    void (*spawn)(Entity *e);    // after creation (may be NULL)
    EntityThinkFn think;         // every frame (may be NULL)
    void (*kill)(Entity *e);     // before removal (may be NULL)
} EntityClass;

struct Entity {
    const char *classname;       // class name (string from the registry)
    const EntityClass *cls;      // class
    SceneNode *node;             // scene node (pos/size are its own)
    void *data;                  // instance data (owned by the class)
    int owns_node;               // 1 = node created by entity_spawn (internal)
};

// Where new entity nodes get attached (the World scene root). Call
// before entity_spawn.
void entity_set_root(SceneNode *root);

// Class registration. Re-registering an existing name is ignored.
void entity_register_class(const EntityClass *cls);

// Creates an entity: a new Node3D node (name = classname) in the scene root.
// pos/size — initial node position/size (NULL — zeros).
// Emits EV_ENTITY_SPAWN and calls cls->spawn. NULL — class not found
// or the list is full.
Entity *entity_spawn(const char *classname, const float *pos,
                     const float *size, void *data);

// Attaches an entity to an existing node (no new node is created;
// on removal the node is NOT freed — it belongs to its own module).
Entity *entity_bind(const char *classname, SceneNode *node, void *data);

// --- Entity handles ----------------------------------------------------------
// Entities live in STABLE slots (entindex = slot number never changes during
// the lifetime) with a serial number. A handle encodes (serial, entindex) and
// guarantees that accessing a killed/reused entity returns NULL
// instead of a dangling pointer. Internal code holds a direct pointer (valid
// while the entity is alive and its slot not freed); across Lua boundaries
// use only the handle.
int  entity_handle(Entity *e);   // 0 = invalid/dead pointer
Entity *entity_get(int handle);  // NULL = entity absent/killed/reused

// Removal: EV_ENTITY_KILL, cls->kill, freeing the node (if created by
// entity_spawn) and its slot in the list.
void entity_kill(Entity *e);

// think of all entities (every frame from the main loop).
void entity_update_all(float dt);

// Kills all entities and clears the list (on shutdown).
void entity_shutdown(void);

// Access to the list (for debugging and a future editor).
int entity_count(void);
Entity *entity_at(int i);

// The n-th (zero-based) entity of the given class, NULL — none left.
// Useful for Lua: find all "trigger_x" etc.
Entity *entity_find_class(const char *classname, int n);

#endif
