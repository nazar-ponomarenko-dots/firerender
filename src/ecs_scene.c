// ============================================================================
//  ecs_scene.c — реализация моста сцена<->ECS (см. ecs_scene.h)
// ----------------------------------------------------------------------------
//  Индекс узел->сущность — самовалидируемый (хранит gen): после смены карты
//  записи старого поколения считаются miss и пересобираются пулом, поэтому
//  висячих указателей в кеше нет по построению.
// ============================================================================
#include "ecs_scene.h"

#include "console.h"
#include "game.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

#define CACHE_CAP 512                    // степень двойки; дерево карты < 100 узлов

static int s_c_node = 0, s_c_tf = 0;
static int s_gen = 1;                    // 0 зарезервирован под «не задано»
static int s_enabled = 0;
static int s_synced = 0;
static uint32_t s_live[ECS_MAX_ENTS / 32 + 1]; // битсет «узел жив в этом прогоне»
static int s_verify_once = 0;               // FR_TEST 'V': прогнать verify на следующем тике

typedef struct { const SceneNode *node; EcsId id; int gen; } IdxEntry;
static IdxEntry s_idx[CACHE_CAP];

static void live_set(EcsId id, int on)
{
    if (id < 0 || id >= ECS_MAX_ENTS) return;
    if (on) s_live[id / 32] |= 1u << (id % 32);
    else    s_live[id / 32] &= ~(1u << (id % 32));
}
static int live_get(EcsId id)
{
    if (id < 0 || id >= ECS_MAX_ENTS) return 0;
    return (int)((s_live[id / 32] >> (id % 32)) & 1u);
}

static uint32_t ptr_hash(const SceneNode *n)
{
    uintptr_t v = (uintptr_t)n;
    v >>= 3;                                   // узлы выделяются подряд по 8 байт
    v *= 0x9E3779B1u;
    return (uint32_t)v & (CACHE_CAP - 1);
}

static EcsId idx_lookup(const SceneNode *n)
{
    uint32_t h = ptr_hash(n);
    for (int i = 0; i < CACHE_CAP; i++) {
        IdxEntry *e = &s_idx[(h + (uint32_t)i) & (CACHE_CAP - 1)];
        if (!e->node) return -1;
        if (e->gen == s_gen && e->node == n) return e->id;
    }
    return -1;
}

static void idx_put(const SceneNode *n, EcsId id)
{
    uint32_t h = ptr_hash(n);
    for (int i = 0; i < CACHE_CAP; i++) {
        IdxEntry *e = &s_idx[(h + (uint32_t)i) & (CACHE_CAP - 1)];
        if (!e->node || e->gen != s_gen || e->node == n) {
            *e = (IdxEntry){ n, id, s_gen };
            return;
        }
    }
    // Кеш забился (дерево > 512 узлов с коллизиями) — не критично: следующий
    // lookup уйдёт в линейный скан пула и вставит запись заменом старого gen.
}

// Медленный путь: линейный скан пула (только при miss индекса).
static EcsId pool_find_node(const SceneNode *n)
{
    for (int id = 0; id < ECS_MAX_ENTS; id++) {
        if (!ecs_is_alive(id)) continue;
        EcsNodeRef *r = (EcsNodeRef *)ecs_get(id, s_c_node);
        if (r && r->gen == s_gen && r->node == n) return id;
    }
    return -1;
}

static EcsId entity_for_node(SceneNode *n, EcsId parent_id)
{
    EcsId id = idx_lookup(n);
    if (id < 0) id = pool_find_node(n);
    if (id < 0) {
        id = ecs_create();
        if (id < 0) return -1;                 // пул полон — узел без ECS-пары
        if (!ecs_attach(id, s_c_node) || !ecs_attach(id, s_c_tf)) {
            ecs_kill(id);
            return -1;
        }
        EcsNodeRef *r = (EcsNodeRef *)ecs_get(id, s_c_node);
        r->node = n;
        r->gen = s_gen;
        s_synced++;
    }
    idx_put(n, id);
    EcsNodeRef *r = (EcsNodeRef *)ecs_get(id, s_c_node);
    r->parent_id = parent_id;
    live_set(id, 1);
    EcsTransform *t = (EcsTransform *)ecs_get(id, s_c_tf);
    memcpy(t->pos, n->pos, sizeof t->pos);
    memcpy(t->rot, n->rot, sizeof t->rot);
    memcpy(t->scl, n->scl, sizeof t->scl);
    t->modified = 0;                            // pull — источник истины
    return id;
}

static void pull_tree(SceneNode *n, EcsId parent_id)
{
    EcsId id = entity_for_node(n, parent_id);
    for (SceneNode *c = n->first_child; c; c = c->next) pull_tree(c, id);
}

static void sweep_orphans(void)
{
    // Узельная сущность — сирота, если её узла нет в дереве этого прогона:
    // либо gen прошлый (дерево пересобрали со сменой карты), либо узел
    // удалили из дерева в текущем поколении (node_remove без ecs-чистки).
    // Чистые ECS-сущности (без NodeRef) проходом не трогаются.
    for (int id = 0; id < ECS_MAX_ENTS; id++) {
        if (!ecs_is_alive(id) || live_get(id)) continue;
        EcsNodeRef *r = (EcsNodeRef *)ecs_get(id, s_c_node);
        if (!r) continue;
        ecs_kill(id);
        if (s_synced > 0) s_synced--;
    }
}

void ecs_scene_init(void)
{
    if (s_c_node) return;
    s_c_node = ecs_register_component("NodeRef", (int)sizeof(EcsNodeRef));
    s_c_tf   = ecs_register_component("Transform", (int)sizeof(EcsTransform));
    if (!s_c_node || !s_c_tf) {
        console_print("ecs_scene: реестр компонентов заполнен — мост выключен");
        s_c_node = s_c_tf = 0;
    }
}

void ecs_scene_enable(int on) { s_enabled = on ? 1 : 0; }
int  ecs_scene_enabled(void)  { return s_enabled; }
int  ecs_scene_synced(void)   { return s_synced; }
int  ecs_scene_generation(void) { return s_gen; }
int  ecs_comp_noderef(void)   { return s_c_node; }
int  ecs_comp_transform(void) { return s_c_tf; }

uint64_t ecs_scene_mask(void)
{
    if (!s_c_tf) return 0;
    return ((uint64_t)1 << (s_c_node - 1)) | ((uint64_t)1 << (s_c_tf - 1));
}

EcsId ecs_scene_id_of(const SceneNode *n)
{
    if (!s_c_node || !n) return -1;
    EcsId id = idx_lookup(n);
    if (id >= 0) return id;
    return pool_find_node(n);
}

EcsId ecs_scene_ensure(SceneNode *n)
{
    if (!s_c_node || !n) return -1;
    EcsId id = entity_for_node(n, -1);
    return id;
}

EcsTransform *ecs_scene_transform(EcsId id)
{
    return s_c_tf ? (EcsTransform *)ecs_get(id, s_c_tf) : NULL;
}

void ecs_scene_mark(EcsId id, uint32_t flags)
{
    EcsTransform *t = ecs_scene_transform(id);
    if (t) t->modified |= flags;
}

void ecs_scene_new_generation(void)
{
    // Старьё прошлого поколения убиваем сразу: иначе включение «на лету»
    // или перезапуск теста оставили бы мёртвые пары в пуле до первого свипа.
    if (s_c_node) {
        for (int id = ECS_MAX_ENTS - 1; id >= 0; id--) {
            if (!ecs_is_alive(id)) continue;
            EcsNodeRef *r = (EcsNodeRef *)ecs_get(id, s_c_node);
            if (r && r->gen != s_gen) ecs_kill(id);
        }
    }
    s_gen++;
    s_synced = 0;
    memset(s_live, 0, sizeof s_live);
    // Индекс чистить не нужно: записи старого gen — автоматический miss.
}

void ecs_scene_frame(SceneNode *root, float dt)
{
    // verify независим от рубильника: команда/FR_TEST 'V' при выключенном
    // мосте должна честно докладывать о расхождениях, а не молчать.
    // Прогоняется в КОНЦЕ тика — после pull, иначе мерить несинхронное дерево.
    int do_verify = s_verify_once;
    s_verify_once = 0;
    if (!s_enabled || !s_c_node || !root) {
        if (do_verify) cmd_ecs_verify(NULL);
        return;
    }
    memset(s_live, 0, sizeof s_live);
    pull_tree(root, -1);
    sweep_orphans();
    ecs_update(dt);
    uint64_t need = ecs_scene_mask();
    for (int id = 0; id < ECS_MAX_ENTS; id++) {
        if (!ecs_is_alive(id)) continue;
        if ((ecs_mask(id) & need) != need) continue;
        EcsTransform *t = (EcsTransform *)ecs_get(id, s_c_tf);
        if (!t || !t->modified) continue;
        EcsNodeRef *r = (EcsNodeRef *)ecs_get(id, s_c_node);
        if (!r || r->gen != s_gen || !r->node) { if (t) t->modified = 0; continue; }
        if (t->modified & ECS_TF_POS) memcpy(r->node->pos, t->pos, sizeof r->node->pos);
        if (t->modified & ECS_TF_ROT) memcpy(r->node->rot, t->rot, sizeof r->node->rot);
        if (t->modified & ECS_TF_SCL) memcpy(r->node->scl, t->scl, sizeof r->node->scl);
        t->modified = 0;
    }
    if (do_verify) cmd_ecs_verify(NULL);
}

// --- Самодиагностика -------------------------------------------------------------
void ecs_scene_verify_once(void) { s_verify_once = 1; }

static void verify_tree(const SceneNode *n, int *nodes, int *synced, int *mismatch)
{
    (*nodes)++;
    EcsId id = ecs_scene_id_of(n);
    if (id < 0) { (*mismatch)++; }
    else {
        (*synced)++;
        const EcsTransform *t = ecs_scene_transform(id);
        if (!t || memcmp(t->pos, n->pos, sizeof t->pos) ||
            memcmp(t->rot, n->rot, sizeof t->rot) ||
            memcmp(t->scl, n->scl, sizeof t->scl)) (*mismatch)++;
    }
    for (const SceneNode *c = n->first_child; c; c = c->next)
        verify_tree(c, nodes, synced, mismatch);
}

void ecs_scene_verify(SceneNode *root, int *nodes, int *synced, int *mismatch)
{
    if (nodes) *nodes = 0;
    if (synced) *synced = 0;
    if (mismatch) *mismatch = 0;
    if (!root || !s_c_node) { if (mismatch) (*mismatch)++; return; }
    verify_tree(root, nodes, synced, mismatch);
}

void cmd_ecs_verify(const char *args)
{
    (void)args;
    Game *g = game_instance();
    if (!g || !g->root) { console_print("ecs_verify: мир не загружен"); return; }
    int nodes = 0, synced = 0, mismatch = 0;
    ecs_scene_verify(g->root, &nodes, &synced, &mismatch);
    // И в консоль, и в лог: headless-прогоны (FR_TEST 'V') читают stdout.
    const char *line = "";
    static char buf[128];
    snprintf(buf, sizeof buf, "ecs_verify: %s — узлов %d, пар %d, расхождений %d",
             mismatch ? "FAIL" : "OK", nodes, synced, mismatch);
    line = buf;
    console_print("%s", line);
    SDL_Log("%s", line);
}

void ecs_scene_shutdown(void)
{
    if (!s_c_node) return;
    for (int id = ECS_MAX_ENTS - 1; id >= 0; id--) {
        if (!ecs_is_alive(id)) continue;
        if (!ecs_has(id, s_c_node)) continue;
        ecs_kill(id);
    }
    s_synced = 0;
    memset(s_live, 0, sizeof s_live);
    memset(s_idx, 0, sizeof s_idx);
    ecs_scene_new_generation();
}
