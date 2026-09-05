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

// Слоты СТАБИЛЬНЫ: entindex = индекс в s_entities[] и НЕ меняется во время
// жизни сущности (раньше swap-with-last переставлял хвост на место убитой —
// любой сохранённый указатель тихо указывал на другую сущность). Свободные
// слоты ищутся линейно, у каждого слота есть номер серии serial, который
// растёт при каждом освобождении. Слот занят, пока s_entities[i].classname
// != NULL.
static Entity s_entities[ENTITY_MAX];
static int     s_serial[ENTITY_MAX];   // серия слота: растёт при kill
static int     s_count = 0;            // живых сущностей (любой порядок слотов)
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
    e->owns_node = owns_node;    // только свои узлы освобождаем при kill
    s_count++;
    events_emit(EV_ENTITY_SPAWN, e, NULL);
    if (e->cls && e->cls->spawn) e->cls->spawn(e);
    // Колбэк spawn мог (редко) убить самою сущность — тогда слот уже свободен.
    return e->classname ? e : NULL;
}

int entity_handle(Entity *e)
{
    if (!e) return 0;
    long idx = e - s_entities;
    if (idx < 0 || idx >= ENTITY_MAX) return 0;
    if (!s_entities[idx].classname) return 0;
    int serial = s_serial[idx] & 0xFFFF;
    int num = (int)(idx + 1) & 0xFFFF;
    return (serial << 16) | num;     // != 0, num>=1; serial в верхних 16 бит
}

Entity *entity_get(int handle)
{
    if (handle <= 0) return NULL;
    int num  = handle & 0xFFFF;              // 1..ENTITY_MAX
    int serial = (unsigned)handle >> 16;
    if (num < 1 || num > ENTITY_MAX) return NULL;
    int idx = num - 1;
    if (s_serial[idx] != serial) return NULL;        // слот переиспользован
    if (!s_entities[idx].classname) return NULL;     // слот свободен
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
    if (!l->classname) return;                  // уже убит
    if (l->cls && l->cls->kill) l->cls->kill(l);
    events_emit(EV_ENTITY_KILL, l, NULL);
    if (l->owns_node && l->node) {
        node_remove(l->node);                  // из дерева — иначе двойной free
        node_free_tree(l->node);                 // узел создавал entity_spawn
    }
    s_serial[idx] = (s_serial[idx] + 1) & 0xFFFF;   // серия: пометить слот переиспользованным
    l->classname = NULL;
    l->node = NULL;
    s_count--;
}

void entity_update_all(float dt)
{
    // Проход по ВСЕМ слотам: kill больше НЕ переставляет элементы (слоты
    // стабильны), поэтому сущность, убившая себя или другую в think, не ломает
    // индексы. Свободные слоты просто пропускаются.
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
    // i-я (с нуля) ЖИВАЯ сущность в порядке номеров слотов; NULL — нет такой.
    if (i < 0) return NULL;
    for (int j = 0; j < ENTITY_MAX; j++)
        if (s_entities[j].classname && i-- == 0) return &s_entities[j];
    return NULL;
}

Entity *entity_find_class(const char *classname, int n)
{
    // n-я (с нуля) сущность данного класса; NULL — такой нет.
    int found = 0;
    for (int i = 0; i < ENTITY_MAX; i++) {
        if (!s_entities[i].classname) continue;
        if (strcmp(s_entities[i].classname, classname) != 0) continue;
        if (found++ == n) return &s_entities[i];
    }
    return NULL;
}
