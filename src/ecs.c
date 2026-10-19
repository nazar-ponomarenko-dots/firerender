// ============================================================================
//  ecs.c — ECS core implementation (see ecs.h)
// ----------------------------------------------------------------------------
//  Everything is static: entity slots and component pools are fixed-capacity
//  arrays. No allocations, so ECS cannot "fall over on out-of-memory" in a
//  frame — overflow is visible in the log and fixed by a constant.
// ============================================================================
#include "ecs.h"

#include "console.h"
#include "ecs_scene.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- Entity slots -----------------------------------------------------------
typedef struct {
    uint64_t mask;               // which components are attached
    int      alive;
} EcsSlot;
static EcsSlot s_ent[ECS_MAX_ENTS];
static int     s_serial[ECS_MAX_ENTS];   // grows on kill: handles age out
static int     s_count = 0;              // number of living ones

static int s_next;                       // cursor searching for a free slot

// Component handles (owner serial) — for ecs_owner by pointer.
static int s_owner_serial[ECS_MAX_ENTS];

// --- Component registry -----------------------------------------------------
typedef struct {
    const char *name;
    int size;
    int used;                        // registry slot taken
    int count;                       // entities having this component
    void *data;                      // ECS_MAX_ENTS slots of size bytes
} EcsCompType;
static EcsCompType s_comp[ECS_MAX_COMPONENTS];

// --- System registry ----------------------------------------------------------
typedef struct {
    const char *name;
    uint64_t all;
    int order;
    EcsSystemFn fn;
    void *ud;
    int used;
} EcsSystem;
static EcsSystem s_sys[ECS_MAX_SYSTEMS];
static int s_sys_n = 0;

// Deferred removal: ecs_kill inside a system run does not free the slot
// immediately — otherwise a running loop would read dead data and the killer
// would "eat" its own ID.
#define ECS_PENDING_MAX ECS_MAX_ENTS
static EcsId s_pending[ECS_PENDING_MAX];
static int   s_pending_n = 0;
static int   s_iterating = 0;          // 1 = walking entities, kill goes to the queue

static int slot_find_free(void)
{
    for (int i = 0; i < ECS_MAX_ENTS; i++) {
        int idx = s_next;
        s_next = (s_next + 1) % ECS_MAX_ENTS;
        if (!s_ent[idx].alive) return idx;
    }
    return -1;
}

// --- Components -----------------------------------------------------------------
int ecs_register_component(const char *name, int size)
{
    if (!name || !name[0] || size <= 0) {
        console_print("ecs: register_component: имя/размер некорректны");
        return 0;
    }
    for (int i = 0; i < ECS_MAX_COMPONENTS; i++) {
        if (!s_comp[i].used) {
            if (!s_comp[i].data) {
                s_comp[i].data = calloc(ECS_MAX_ENTS, (size_t)size);
                if (!s_comp[i].data) {
                    console_print("ecs: %s — нехватка памяти пула", name);
                    return 0;
                }
            }
            s_comp[i].name = name;
            s_comp[i].size = size;
            s_comp[i].count = 0;
            memset(s_comp[i].data, 0, (size_t)ECS_MAX_ENTS * (size_t)size);
            s_comp[i].used = 1;
            return i + 1;                // id 0 reserved for the error case
        }
    }
    console_print("ecs: реестр компонентов полон (%d)", ECS_MAX_COMPONENTS);
    return 0;
}

static EcsCompType *comp_type(int comp)
{
    if (comp < 1 || comp > ECS_MAX_COMPONENTS) return NULL;
    EcsCompType *t = &s_comp[comp - 1];
    return t->used ? t : NULL;
}

const char *ecs_component_name(int comp)
{
    EcsCompType *t = comp_type(comp);
    return t ? t->name : "?";
}

int ecs_component_count(int comp)
{
    EcsCompType *t = comp_type(comp);
    return t ? t->count : 0;
}

// --- Entities -------------------------------------------------------------------
EcsId ecs_create(void)
{
    int idx = slot_find_free();
    if (idx < 0) {
        console_print("ecs: create: пул сущностей полон (%d)", ECS_MAX_ENTS);
        return -1;
    }
    s_ent[idx].mask = 0;
    s_ent[idx].alive = 1;
    s_owner_serial[idx] = s_serial[idx];
    s_count++;
    return idx;
}

int ecs_is_alive(EcsId id)
{
    return id >= 0 && id < ECS_MAX_ENTS && s_ent[id].alive;
}

uint64_t ecs_mask(EcsId id)
{
    return ecs_is_alive(id) ? s_ent[id].mask : 0;
}

int ecs_has(EcsId id, int comp)
{
    if (!comp_type(comp)) return 0;
    return ecs_is_alive(id) && (s_ent[id].mask & (uint64_t)1 << (comp - 1));
}

int ecs_attach(EcsId id, int comp)
{
    EcsCompType *t = comp_type(comp);
    if (!t) return 0;
    if (!ecs_is_alive(id)) return 0;
    uint64_t bit = (uint64_t)1 << (comp - 1);
    if (s_ent[id].mask & bit) return 1;          // already there — no duplicate
    memset((char *)t->data + (size_t)id * (size_t)t->size, 0, (size_t)t->size);
    s_ent[id].mask |= bit;
    t->count++;
    return 1;
}

void ecs_detach(EcsId id, int comp)
{
    EcsCompType *t = comp_type(comp);
    if (!t || !ecs_is_alive(id)) return;
    uint64_t bit = (uint64_t)1 << (comp - 1);
    if (!(s_ent[id].mask & bit)) return;
    memset((char *)t->data + (size_t)id * (size_t)t->size, 0, (size_t)t->size);
    s_ent[id].mask &= ~bit;
    if (t->count > 0) t->count--;
}

void *ecs_get(EcsId id, int comp)
{
    if (!ecs_has(id, comp)) return NULL;
    EcsCompType *t = comp_type(comp);
    return (char *)t->data + (size_t)id * (size_t)t->size;
}

EcsId ecs_owner(const void *comp_ptr, int comp)
{
    EcsCompType *t = comp_type(comp);
    if (!t || !comp_ptr) return -1;
    char *base = (char *)t->data;
    const char *p = (const char *)comp_ptr;
    if (p < base || p >= base + (size_t)ECS_MAX_ENTS * (size_t)t->size) return -1;
    size_t off = (size_t)(p - base);
    int id = (int)(off / (size_t)t->size);
    if (!ecs_has(id, comp)) return -1;
    // Slot reused after death — the old pointer is no longer "ours".
    if (s_owner_serial[id] != s_serial[id]) return -1;
    return id;
}

static void ent_free(int id)
{
    if (!s_ent[id].alive) return;
    for (int c = 0; c < ECS_MAX_COMPONENTS; c++) {
        if (!(s_ent[id].mask & ((uint64_t)1 << c))) continue;
        EcsCompType *t = &s_comp[c];
        memset((char *)t->data + (size_t)id * (size_t)t->size, 0, (size_t)t->size);
        if (t->count > 0) t->count--;
    }
    s_ent[id].mask = 0;
    s_ent[id].alive = 0;
    s_serial[id] = (s_serial[id] + 1) & 0xFFFF;   // unsigned ring of serials
    s_count--;
}

void ecs_kill(EcsId id)
{
    if (!ecs_is_alive(id)) return;
    if (s_iterating) {
        if (s_pending_n < ECS_PENDING_MAX) s_pending[s_pending_n++] = id;
        return;
    }
    ent_free(id);
}

// --- Handles ----------------------------------------------------------------------
int ecs_handle(EcsId id)
{
    if (!ecs_is_alive(id)) return 0;
    unsigned serial = (unsigned)(s_serial[id] & 0xFFFF);
    if (serial == 0) serial = 1;         // top bits 0 = "no handle"
    s_serial[id] = (int)serial;
    return (int)((serial << 16) | (unsigned)(id + 1));
}

EcsId ecs_get_by_handle(int handle)
{
    if (handle == 0) return -1;
    unsigned num = (unsigned)handle & 0xFFFF;
    unsigned serial = ((unsigned)handle) >> 16;
    if (num < 1 || num > ECS_MAX_ENTS) return -1;
    int idx = (int)num - 1;
    if (!s_ent[idx].alive) return -1;
    if ((unsigned)(s_serial[idx] & 0xFFFF) != serial) return -1;
    return idx;
}

// --- Systems -------------------------------------------------------------------------
void ecs_register_system(const char *name, uint64_t all, int order,
                         EcsSystemFn fn, void *ud)
{
    if (!name || !fn || !all) {
        console_print("ecs: register_system: имя/fn/mask обязательны");
        return;
    }
    if (s_sys_n >= ECS_MAX_SYSTEMS) {
        console_print("ecs: реестр систем полон (%d)", ECS_MAX_SYSTEMS);
        return;
    }
    int at = s_sys_n;
    while (at > 0 && s_sys[at - 1].order > order) at--;   // insert by order
    for (int i = s_sys_n; i > at; i--) s_sys[i] = s_sys[i - 1];
    s_sys[at] = (EcsSystem){ name, all, order, fn, ud, 1 };
    s_sys_n++;
}

void ecs_update(float dt)
{
    (void)dt;
    if (s_iterating) return;             // reentrant call — not a copy of the loop
    s_iterating = 1;
    for (int id = 0; id < ECS_MAX_ENTS; id++) {
        // Run each system with a FRESH liveness and mask check: the previous
        // system could detach a component or kill the entity.
        for (int s = 0; s < s_sys_n; s++) {
            if (!s_ent[id].alive) break;
            if ((s_ent[id].mask & s_sys[s].all) != s_sys[s].all) continue;
            s_sys[s].fn(id, s_sys[s].ud);
        }
    }
    s_iterating = 0;
    for (int i = 0; i < s_pending_n; i++) ent_free(s_pending[i]);
    s_pending_n = 0;
}

// --- Debug ---------------------------------------------------------------------------
int ecs_count(void) { return s_count; }
int ecs_system_count(void) { return s_sys_n; }
int ecs_component_types(void)
{
    int n = 0;
    for (int i = 0; i < ECS_MAX_COMPONENTS; i++) if (s_comp[i].used) n++;
    return n;
}

void ecs_list(void)
{
    console_print("ecs: сущностей %d/%d, систем %d", s_count, ECS_MAX_ENTS, s_sys_n);
    for (int c = 0; c < ECS_MAX_COMPONENTS; c++) {
        if (!s_comp[c].used) continue;
        console_print("  компонент %-16s id %d: %d", s_comp[c].name, c + 1,
                      s_comp[c].count);
    }
    for (int s = 0; s < s_sys_n; s++)
        console_print("  система  %-16s order %d mask 0x%llX", s_sys[s].name,
                      s_sys[s].order, (unsigned long long)s_sys[s].all);
    int shown = 0;
    for (int id = 0; id < ECS_MAX_ENTS && shown < 32; id++) {
        if (!s_ent[id].alive) continue;
        shown++;
        char bits[128];
        int n = 0;
        bits[0] = 0;
        for (int c = 0; c < ECS_MAX_COMPONENTS; c++) {
            if (!(s_ent[id].mask & ((uint64_t)1 << c))) continue;
            if (n < (int)sizeof bits - 24)
                n += snprintf(bits + n, sizeof bits - (size_t)n, "%s%s",
                              n ? "," : "", s_comp[c].name);
        }
        console_print("  #%d h=%d [%s]", id, ecs_handle(id), bits);
    }
    if (s_count > shown) console_print("  ... ещё %d", s_count - shown);
}

void ecs_shutdown(void)
{
    for (int id = 0; id < ECS_MAX_ENTS; id++)
        if (s_ent[id].alive) ent_free(id);
    s_pending_n = 0;
    s_iterating = 0;
    s_next = 0;
}

// --- Commands -----------------------------------------------------------------------------
void cmd_ecs_list(const char *args)
{
    (void)args;
    ecs_list();
}

void cmd_ecs_stats(const char *args)
{
    (void)args;
    console_print("ecs: %d/%d сущностей, %d типов компонентов, %d систем",
                  ecs_count(), ECS_MAX_ENTS, ecs_component_types(),
                  ecs_system_count());
    console_print("ecs_scene: мост %s, %d узельных (gen %d)",
                  ecs_scene_enabled() ? "вкл" : "выкл",
                  ecs_scene_synced(), ecs_scene_generation());
    for (int c = 1; c <= ECS_MAX_COMPONENTS; c++) {
        int n = ecs_component_count(c);
        if (n > 0) console_print("  %-16s %d", ecs_component_name(c), n);
    }
}

void cmd_ecs_test(const char *args)
{
    (void)args;
    int fails = ecs_selftest();
    console_print("ecs: selftest %s (провалов %d)", fails ? "FAIL" : "OK", fails);
}

// --- Self-test ----------------------------------------------------------------------------
// Deterministic and repeatable: components are registered once (static guard),
// entities and systems are fully removed at the end.
static int ecs_st_comp_cpos = 0, ecs_st_comp_cnct = 0;

typedef struct { float x, y, z; } StPos;
typedef struct { int hits; } StCount;

static int st_fail;
static void st_check(int cond, const char *what)
{
    if (!cond) { st_fail++; console_print("ecs selftest FAIL: %s", what); }
}

static void st_system(EcsId id, void *ud)
{
    StCount *c = (StCount *)ud;
    (void)id;
    c->hits++;
}

static void st_system_killself(EcsId id, void *ud)
{
    StCount *c = (StCount *)ud;
    ecs_kill(id);                        // suicide during the run — goes to the queue
    c->hits++;
}

int ecs_selftest(void)
{
    st_fail = 0;
    static StCount ud;
    ecs_shutdown();
    s_sys_n = 0;

    if (!ecs_st_comp_cpos) {
        ecs_st_comp_cpos = ecs_register_component("st_pos", (int)sizeof(StPos));
        ecs_st_comp_cnct = ecs_register_component("st_count",
                                                  (int)sizeof(StCount));
    }
    int cpos = ecs_st_comp_cpos, cnct = ecs_st_comp_cnct;
    st_check(cpos && cnct, "register components");

    EcsId a = ecs_create();
    EcsId b = ecs_create();
    st_check(a >= 0 && b >= 0 && a != b, "create distinct");
    st_check(ecs_is_alive(a) && ecs_count() == 2, "alive/count");
    st_check(ecs_attach(a, cpos) && ecs_attach(b, cpos), "attach");
    st_check(ecs_has(a, cpos) && !ecs_has(a, cnct), "has");
    st_check(ecs_attach(a, cpos) == 1, "attach twice idempotent");
    StPos *p = ecs_get(a, cpos);
    st_check(p && p->x == 0.0f && p->y == 0.0f, "zero-init");
    if (p) { p->x = 1.5f; p->y = -2.0f; }
    StPos *p2 = ecs_get(a, cpos);
    st_check(p2 && p2 == p && p2->x == 1.5f, "stable pointer + data");
    st_check(ecs_owner(p, cpos) == a, "owner by pointer");
    st_check(ecs_owner(NULL, cpos) == -1, "owner null");
    st_check(ecs_get(b, cnct) == NULL, "absent component -> NULL");

    int ha = ecs_handle(a), hb = ecs_handle(b);
    st_check(ecs_get_by_handle(ha) == a, "handle roundtrip");
    st_check(ecs_get_by_handle(hb) == b, "handle roundtrip b");
    st_check(ecs_get_by_handle(0) == -1, "handle 0");
    st_check(ecs_handle((EcsId)-1) == 0 && ecs_get_by_handle(-1) == -1,
             "bad id/handle");

    uint64_t m = (uint64_t)1 << (cpos - 1);
    ud.hits = 0;
    ecs_register_system("st_count_sys", m, 10, st_system, &ud);
    ecs_update(0.016f);
    st_check(ud.hits == 2, "system ran for both");

    // Suicide during the run: deferred kill, the traversal is not broken.
    st_check(ecs_attach(a, cnct), "attach counter");
    ud.hits = 0;
    ecs_register_system("st_kill", m | ((uint64_t)1 << (cnct - 1)), 20,
                        st_system_killself, &ud);
    ecs_update(0.016f);
    st_check(!ecs_is_alive(a), "deferred self-kill executed");
    st_check(ecs_is_alive(b), "sibling unaffected");
    st_check(ecs_count() == 1, "count after kill");
    st_check(ud.hits == 3, "both systems ran that frame");
    st_check(ecs_get(a, cpos) == NULL && ecs_owner(p, cpos) == -1,
             "dead entity: no components, stale pointer");
    st_check(ecs_handle(a) == 0, "dead entity handle 0");
    st_check(ecs_get_by_handle(ha) == -1, "stale handle dead");

    // Slot reuse: the old handle is stale, the new one is valid.
    ecs_kill(b);
    st_check(ecs_get_by_handle(hb) == -1, "slot reuse invalidates old handle");
    EcsId r = ecs_create();
    st_check(r >= 0, "slot reusable");
    st_check(ecs_get_by_handle(ecs_handle(r)) == r, "new handle valid");
    int overflow_ok = 0;
    for (int i = 0; i < ECS_MAX_ENTS + 8; i++)
        if (ecs_create() >= 0) overflow_ok++;  // overflow: -1, no crash
    st_check(overflow_ok > 0, "pool overflow returns -1, no crash");

    ecs_shutdown();
    st_check(ecs_count() == 0, "shutdown clears");
    s_sys_n = 0;
    return st_fail;
}
