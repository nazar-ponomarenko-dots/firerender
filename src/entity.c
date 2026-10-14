// ============================================================================
//  entity.c — entity implementation (see entity.h)
// ============================================================================
#include "entity.h"

#include "console.h"
#include "events.h"

#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>

#define ENTITY_MAX 256
#define CLASS_MAX  32

// Slots are STABLE: entindex = index into s_entities[] and does NOT change for
// the entity's lifetime (previously swap-with-last moved the tail over the dead
// entry — any saved pointer silently pointed at a different entity). Free slots
// are found linearly; each slot has a serial number that grows on every
// release. A slot is occupied while s_entities[i].classname != NULL.
static Entity s_entities[ENTITY_MAX];
static int     s_serial[ENTITY_MAX];   // slot serial: grows on kill
static int     s_count = 0;            // live entities (slots in any order)
static EntityClass s_classes[CLASS_MAX];
static int  s_cls_n = 0;
static SceneNode *s_root = NULL;

static const EntityClass *class_find(const char *name)
{
    if (!name) return NULL;
    for (int i = 0; i < s_cls_n; i++)
        if (strcmp(s_classes[i].classname, name) == 0) return &s_classes[i];
    return NULL;
}

void entity_set_root(SceneNode *root)
{
    s_root = root;
}

void entity_register_class(const EntityClass *cls)
{
    if (!cls || !cls->classname || !cls->classname[0]) return;
    if (class_find(cls->classname)) return;
    if (s_cls_n >= CLASS_MAX) {
        SDL_Log("entity: class registry full (%d)", CLASS_MAX);
        return;
    }
    s_classes[s_cls_n++] = *cls;
}

static Entity *entity_new(const char *classname, SceneNode *node, void *data,
                          int owns_node)
{
    int idx = -1;
    for (int i = 0; i < ENTITY_MAX; i++)
        if (!s_entities[i].classname) { idx = i; break; }
    if (idx < 0) {
        console_print("entity: list full (%d)", ENTITY_MAX);
        return NULL;
    }
    Entity *e = &s_entities[idx];
    memset(e, 0, sizeof *e);
    e->classname = classname;
    e->cls = class_find(classname);
    e->node = node;
    e->data = data;
    e->owns_node = owns_node;    // only our own nodes are freed on kill
    s_count++;
    events_emit(EV_ENTITY_SPAWN, e, NULL);
    if (e->cls && e->cls->spawn) e->cls->spawn(e);
    // The spawn callback could (rarely) have killed the entity itself —
    // then the slot is already free.
    return e->classname ? e : NULL;
}

int entity_handle(Entity *e)
{
    if (!e) return 0;
    long idx = e - s_entities;
    if (idx < 0 || idx >= ENTITY_MAX) return 0;
    if (!s_entities[idx].classname) return 0;
    int serial = s_serial[idx] & 0xFFFF;
    // Pack via an unsigned shift: serial >= 32768 would yield a negative int,
    // and below such a handle would count as invalid — a live entity would go
    // "stale" for Lua/console after 32768 slot reuses.
    return (int)(((unsigned)serial << 16) | (unsigned)(idx + 1));
}

Entity *entity_get(int handle)
{
    if (handle == 0) return NULL;            // num >= 1, so 0 means only "none"
    int num  = handle & 0xFFFF;              // 1..ENTITY_MAX
    int serial = (unsigned)handle >> 16;
    if (num < 1 || num > ENTITY_MAX) return NULL;
    int idx = num - 1;
    if (s_serial[idx] != serial) return NULL;        // slot was reused
    if (!s_entities[idx].classname) return NULL;     // slot is free
    return &s_entities[idx];
}

Entity *entity_spawn(const char *classname, const float *pos,
                     const float *size, void *data)
{
    const EntityClass *cls = class_find(classname);
    if (!cls) {
        console_print("entity: unknown class \"%s\"", classname ? classname : "?");
        return NULL;
    }
    SceneNode *node = node_new(cls->classname, "Entity3D", pos, size,
                               NULL, NULL, NULL);
    if (s_root) node_add(s_root, node);
    return entity_new(cls->classname, node, data, 1);
}

Entity *entity_bind(const char *classname, SceneNode *node, void *data)
{
    if (!class_find(classname)) {
        console_print("entity: unknown class \"%s\"", classname ? classname : "?");
        return NULL;
    }
    if (!node) return NULL;
    return entity_new(classname, node, data, 0);
}

void entity_kill(Entity *e)
{
    if (!e) return;
    long idx = e - s_entities;
    if (idx < 0 || idx >= ENTITY_MAX) return;
    Entity *l = &s_entities[idx];
    if (!l->classname) return;                  // already killed
    if (l->cls && l->cls->kill) l->cls->kill(l);
    events_emit(EV_ENTITY_KILL, l, NULL);
    if (l->owns_node && l->node) {
        node_remove(l->node);                  // from the tree — otherwise double free
        node_free_tree(l->node);                 // the node was created by entity_spawn
    }
    s_serial[idx] = (s_serial[idx] + 1) & 0xFFFF;   // serial: mark the slot as reused
    l->classname = NULL;
    l->node = NULL;
    s_count--;
}

void entity_update_all(float dt)
{
    // Iterate over ALL slots: kill no longer rearranges elements (slots are
    // stable), so an entity that kills itself or another one in think does not
    // break indices. Free slots are simply skipped.
    for (int i = 0; i < ENTITY_MAX; i++) {
        Entity *e = &s_entities[i];
        if (e->classname && e->cls && e->cls->think) e->cls->think(e, dt);
    }
}

void entity_shutdown(void)
{
    for (int i = ENTITY_MAX - 1; i >= 0; i--)
        if (s_entities[i].classname) entity_kill(&s_entities[i]);
}

int entity_count(void)
{
    return s_count;
}

Entity *entity_at(int i)
{
    // The i-th (zero-based) LIVE entity in slot-number order; NULL — no such entity.
    if (i < 0) return NULL;
    for (int j = 0; j < ENTITY_MAX; j++)
        if (s_entities[j].classname && i-- == 0) return &s_entities[j];
    return NULL;
}

Entity *entity_find_class(const char *classname, int n)
{
    // The n-th (zero-based) entity of this class; NULL — none exists.
    int found = 0;
    for (int i = 0; i < ENTITY_MAX; i++) {
        if (!s_entities[i].classname) continue;
        if (strcmp(s_entities[i].classname, classname) != 0) continue;
        if (found++ == n) return &s_entities[i];
    }
    return NULL;
}
