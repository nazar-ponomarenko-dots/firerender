// ============================================================================
//  entity.c — реализация сущностей (см. entity.h)
// ============================================================================
#include "entity.h"

#include "console.h"
#include "events.h"

#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>

#define ENTITY_MAX 256
#define CLASS_MAX  32

static Entity s_entities[ENTITY_MAX];
static int  s_ent_n = 0;
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
    if (s_ent_n >= ENTITY_MAX) {
        console_print("entity: list full (%d)", ENTITY_MAX);
        return NULL;
    }
    Entity *e = &s_entities[s_ent_n];
    memset(e, 0, sizeof *e);
    e->classname = classname;
    e->cls = class_find(classname);
    e->node = node;
    e->data = data;
    e->owns_node = owns_node;    // только свои узлы освобождаем при kill
    s_ent_n++;
    events_emit(EV_ENTITY_SPAWN, e, NULL);
    if (e->cls && e->cls->spawn) e->cls->spawn(e);
    return e;
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
    if (!e || e->classname == NULL) return;      // уже убит
    if (e->cls && e->cls->kill) e->cls->kill(e);
    events_emit(EV_ENTITY_KILL, e, NULL);
    if (e->owns_node && e->node) {
        node_remove(e->node);                  // из дерева — иначе двойной free
        node_free_tree(e->node);                 // узел создавал entity_spawn
    }
    e->classname = NULL;
    e->node = NULL;

    // сдвигаем список (порядок не важен)
    int idx = (int)(e - s_entities);
    if (idx >= 0 && idx < s_ent_n - 1)
        s_entities[idx] = s_entities[s_ent_n - 1];
    s_ent_n--;
}

void entity_update_all(float dt)
{
    for (int i = 0; i < s_ent_n; i++) {
        Entity *e = &s_entities[i];
        if (e->cls && e->cls->think) e->cls->think(e, dt);
    }
}

void entity_shutdown(void)
{
    while (s_ent_n > 0) entity_kill(&s_entities[s_ent_n - 1]);
}

int entity_count(void)
{
    return s_ent_n;
}

Entity *entity_at(int i)
{
    return (i >= 0 && i < s_ent_n) ? &s_entities[i] : NULL;
}

Entity *entity_find_class(const char *classname, int n)
{
    // n-я (с нуля) сущность данного класса; NULL — такой нет.
    int found = 0;
    for (int i = 0; i < s_ent_n; i++) {
        if (strcmp(s_entities[i].classname, classname) != 0) continue;
        if (found++ == n) return &s_entities[i];
    }
    return NULL;
}
