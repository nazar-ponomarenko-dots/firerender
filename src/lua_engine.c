// ============================================================================
//  lua_engine.c — embedded Lua: engine scripts (see lua_engine.h)
// ----------------------------------------------------------------------------
//  The `engine` table in scripts:
//    engine.log(msg)            — print to the engine console
//    engine.cvar_get(name)      — cvar value
//    engine.cvar_set(name, v)   — set a cvar
//    engine.exec(cmd)           — execute a console command
//    engine.cmd(name, fn)       — register a console command from Lua
//                                 (fn receives the argument string)
//    engine.on("event", fn)     — subscribe to an engine event
//    engine.update(fn)          — per-frame callback (receives dt)
//  Events: entity_spawn, entity_kill, console_open, console_close.
// ============================================================================
#include "lua_engine.h"

#include "console.h"
#include "cvars.h"
#include "entity.h"
#include "events.h"
#include "game.h"
#include "resources.h"
#include "fx.h"
#include "binds.h"
#include "material.h"
#include "mesh.h"
#include "glb.h"
#include "glb_anim.h"
#include "renderer.h"

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static lua_State *L = NULL;

void lua_engine_print(const char *msg)
{
    if (msg) console_print("%s", msg);
}

// --- engine API ---------------------------------------------------------------
static int l_log(lua_State *ls)
{
    lua_engine_print(luaL_checkstring(ls, 1));   // to the engine console
    return 0;
}

static int l_cvar_get(lua_State *ls)
{
    lua_pushnumber(ls, cvar_get(luaL_checkstring(ls, 1)));
    return 1;
}

static int l_cvar_set(lua_State *ls)
{
    cvar_set(luaL_checkstring(ls, 1), (float)luaL_checknumber(ls, 2));
    return 0;
}

static int l_exec(lua_State *ls)
{
    console_exec(luaL_checkstring(ls, 1));
    return 0;
}

// --- engine.cmd: registering a console command from Lua ----------------------
// The console handler has a fixed signature void(*)(const char*), so each Lua
// command needs its own C trampoline. A pool of static trampolines.
#define LUA_CMD_MAX 16
static int s_cmd_ref[LUA_CMD_MAX];
static char s_cmd_name[LUA_CMD_MAX][64];
static int s_cmd_n = 0;

static void lua_cmd_call_ref(int ref, const char *args)
{
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    lua_pushstring(L, args ? args : "");
    if (lua_pcall(L, 1, 0, 0) != 0) {
        lua_engine_print(lua_tostring(L, -1));
        lua_pop(L, 1);
    }
}

#define DEF_TRAMP(i) \
    static void lua_cmd_##i(const char *a) { lua_cmd_call_ref(s_cmd_ref[i], a); }
DEF_TRAMP(0)  DEF_TRAMP(1)  DEF_TRAMP(2)  DEF_TRAMP(3)
DEF_TRAMP(4)  DEF_TRAMP(5)  DEF_TRAMP(6)  DEF_TRAMP(7)
DEF_TRAMP(8)  DEF_TRAMP(9)  DEF_TRAMP(10) DEF_TRAMP(11)
DEF_TRAMP(12) DEF_TRAMP(13) DEF_TRAMP(14) DEF_TRAMP(15)

static const ConsoleCmdFn s_cmd_fn[LUA_CMD_MAX] = {
    lua_cmd_0, lua_cmd_1, lua_cmd_2, lua_cmd_3,
    lua_cmd_4, lua_cmd_5, lua_cmd_6, lua_cmd_7,
    lua_cmd_8, lua_cmd_9, lua_cmd_10, lua_cmd_11,
    lua_cmd_12, lua_cmd_13, lua_cmd_14, lua_cmd_15,
};

static int l_cmd(lua_State *ls)
{
    const char *name = luaL_checkstring(ls, 1);
    luaL_checktype(ls, 2, LUA_TFUNCTION);
    // Re-registration (map/script reload) — reuse the slot by name. Otherwise
    // every reload would burn a trampoline and a Lua ref until the pool runs
    // out (and console_add_command silently rejected the duplicate anyway —
    // the ref leaked).
    int i = -1;
    for (int k = 0; k < s_cmd_n; k++)
        if (strcmp(s_cmd_name[k], name) == 0) { i = k; break; }
    if (i < 0) {
        if (s_cmd_n >= LUA_CMD_MAX) {
            lua_engine_print("engine.cmd: лимит Lua-команд");
            return 0;
        }
        i = s_cmd_n++;
        snprintf(s_cmd_name[i], sizeof s_cmd_name[i], "%s", name);
        if (!console_add_command(name, "lua", s_cmd_fn[i])) {
            lua_engine_print("engine.cmd: команда уже зарегистрирована");
            s_cmd_n--;                       // name taken by a C command — return the slot
            return 0;
        }
    } else {
        luaL_unref(ls, LUA_REGISTRYINDEX, s_cmd_ref[i]);  // old handler
    }
    s_cmd_ref[i] = luaL_ref(ls, LUA_REGISTRYINDEX);
    return 0;
}

// --- engine.on: subscribing to engine events ---------------------------------
#define LUA_EV_MAX 4
static int s_ev_ref[EVENTS_MAX_TYPES][LUA_EV_MAX];
static int s_ev_n[EVENTS_MAX_TYPES];
static int s_ev_registered[EVENTS_MAX_TYPES];

static int ev_type_from_name(const char *n)
{
    if (!strcmp(n, "entity_spawn"))  return EV_ENTITY_SPAWN;
    if (!strcmp(n, "entity_kill"))   return EV_ENTITY_KILL;
    if (!strcmp(n, "console_open"))  return EV_CONSOLE_OPENED;
    if (!strcmp(n, "console_close")) return EV_CONSOLE_CLOSED;
    return -1;
}

// C event listener: calls all Lua callbacks of the type (fn(type, a, b))
static void lua_event_dispatch(const Event *e, void *user)
{
    int type = (int)(intptr_t)user;
    for (int k = 0; k < s_ev_n[type]; k++) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, s_ev_ref[type][k]);
        lua_pushinteger(L, e->type);
        lua_pushlightuserdata(L, (void *)e->a);
        lua_pushlightuserdata(L, (void *)e->b);
        if (lua_pcall(L, 3, 0, 0) != 0) {
            lua_engine_print(lua_tostring(L, -1));
            lua_pop(L, 1);          // pcall leaves the message on the stack on error
        }
    }
}

static int l_on(lua_State *ls)
{
    const char *name = luaL_checkstring(ls, 1);
    int type = ev_type_from_name(name);
    if (type < 0) {
        lua_engine_print("engine.on: неизвестное событие");
        return 0;
    }
    luaL_checktype(ls, 2, LUA_TFUNCTION);
    if (s_ev_n[type] >= LUA_EV_MAX) {
        lua_engine_print("engine.on: лимит подписок на событие");
        return 0;
    }
    s_ev_ref[type][s_ev_n[type]++] = luaL_ref(ls, LUA_REGISTRYINDEX);
    if (!s_ev_registered[type]) {
        events_listen(type, lua_event_dispatch, (void *)(intptr_t)type);
        s_ev_registered[type] = 1;
    }
    return 0;
}

// --- Scripted entities (Lua classes) ------------------------------------------
// engine.entity_class("name", { spawn=fn, think=fn, kill=fn }) registers an
// entity class: the C callbacks (lua_cls_*) find Lua refs by e->classname and
// call them with (Entity, dt). No trampolines — identity by name.
#define LUACLS_MAX 16
typedef struct { char name[64]; int spawn, think, kill; } LuaCls;
static LuaCls s_luacls[LUACLS_MAX];
static int s_luacls_n = 0;

static LuaCls *lua_cls_find(const char *name)
{
    if (!name) return NULL;
    for (int i = 0; i < s_luacls_n; i++)
        if (strcmp(s_luacls[i].name, name) == 0) return &s_luacls[i];
    return NULL;
}

// Calls the class Lua callback with (entity, dt). The entity is passed as a
// HANDLE (int), not a pointer — so Lua never holds a raw Entity* and cannot
// accidentally touch a killed entity. At callback time the entity is known alive.
static void lua_cls_call(int ref, Entity *e, float dt)
{
    if (ref == LUA_NOREF) return;
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    lua_pushnumber(L, entity_handle(e));
    lua_pushnumber(L, dt);
    if (lua_pcall(L, 2, 0, 0) != 0) {
        lua_engine_print(lua_tostring(L, -1));
        lua_pop(L, 1);          // pcall leaves the message on the stack on error
    }
}

static void lua_cls_spawn(Entity *e)
{
    LuaCls *c = lua_cls_find(e->classname);
    if (c) lua_cls_call(c->spawn, e, 0.0f);
}
static void lua_cls_think(Entity *e, float dt)
{
    LuaCls *c = lua_cls_find(e->classname);
    if (c) lua_cls_call(c->think, e, dt);
}
static void lua_cls_kill(Entity *e)
{
    LuaCls *c = lua_cls_find(e->classname);
    if (c) lua_cls_call(c->kill, e, 0.0f);
}

// Helper: take a callback from the table at ref (or LUA_NOREF)
static void lua_cls_take(lua_State *ls, int tblidx, const char *field, int *ref)
{
    lua_getfield(ls, tblidx, field);
    if (lua_isfunction(ls, -1)) *ref = luaL_ref(ls, LUA_REGISTRYINDEX);
    else lua_pop(ls, 1);
}

static int l_entity_class(lua_State *ls)
{
    const char *name = luaL_checkstring(ls, 1);
    luaL_checktype(ls, 2, LUA_TTABLE);
    // Re-registration (map reload) — overwrite the slot by name
    LuaCls *c = lua_cls_find(name);
    if (c) {
        luaL_unref(ls, LUA_REGISTRYINDEX, c->spawn);
        luaL_unref(ls, LUA_REGISTRYINDEX, c->think);
        luaL_unref(ls, LUA_REGISTRYINDEX, c->kill);
    } else {
        if (s_luacls_n >= LUACLS_MAX) {
            lua_engine_print("engine.entity_class: лимит Lua-классов");
            return 0;
        }
        c = &s_luacls[s_luacls_n++];
    }
    snprintf(c->name, sizeof c->name, "%s", name);
    c->spawn = c->think = c->kill = LUA_NOREF;
    lua_cls_take(ls, 2, "spawn", &c->spawn);
    lua_cls_take(ls, 2, "think", &c->think);
    lua_cls_take(ls, 2, "kill",  &c->kill);
    // entity_register_class ignores a repeated name, so ALWAYS attach all
    // three dispatchers. They look up the current ref by name at call time
    // (lua_cls_find) — re-registration with new callbacks is picked up.
    // If the ref is absent, lua_cls_call returns by itself (LUA_NOREF).
    EntityClass cls = { c->name, lua_cls_spawn, lua_cls_think, lua_cls_kill };
    entity_register_class(&cls);
    return 0;
}

// Entity from a handle argument (int) or NULL. Guards against dangling refs:
// entity_get checks the slot serial and returns NULL if the entity was killed.
static Entity *lua_ent_get(lua_State *ls, int idx)
{
    return entity_get((int)luaL_optinteger(ls, idx, 0));
}

// engine.entity_spawn("class", x, y, z) -> int (handle) or nil
static int l_entity_spawn(lua_State *ls)
{
    const char *cls = luaL_checkstring(ls, 1);
    float pos[3] = { (float)luaL_optnumber(ls,2,0),
                     (float)luaL_optnumber(ls,3,0),
                     (float)luaL_optnumber(ls,4,0) };
    Entity *e = entity_spawn(cls, pos, NULL, NULL);
    if (e) lua_pushnumber(ls, entity_handle(e));
    else lua_pushnil(ls);
    return 1;
}

// engine.entity_kill(entity)
static int l_entity_kill(lua_State *ls)
{
    Entity *e = lua_ent_get(ls, 1);
    if (e) entity_kill(e);
    return 0;
}

// engine.entity_find("class", n) -> int (handle) or nil
static int l_entity_find(lua_State *ls)
{
    const char *cls = luaL_checkstring(ls, 1);
    int n = (int)luaL_optinteger(ls, 2, 0);
    Entity *e = entity_find_class(cls, n);
    if (e) lua_pushnumber(ls, entity_handle(e));
    else lua_pushnil(ls);
    return 1;
}

// engine.entity_pos(e) -> x, y, z (entity node position); dead entity — zeros
static int l_entity_pos(lua_State *ls)
{
    Entity *e = lua_ent_get(ls, 1);
    float x = 0, y = 0, z = 0;
    if (e && e->node) {
        x = e->node->pos[0]; y = e->node->pos[1]; z = e->node->pos[2];
    }
    lua_pushnumber(ls, x);
    lua_pushnumber(ls, y);
    lua_pushnumber(ls, z);
    return 3;
}

// engine.entity_set_pos(e, x, y, z)
static int l_entity_set_pos(lua_State *ls)
{
    Entity *e = lua_ent_get(ls, 1);
    if (e && e->node) {
        e->node->pos[0] = (float)luaL_optnumber(ls, 2, 0);
        e->node->pos[1] = (float)luaL_optnumber(ls, 3, 0);
        e->node->pos[2] = (float)luaL_optnumber(ls, 4, 0);
    }
    return 0;
}

// engine.entity_classname(e) -> string
static int l_entity_classname(lua_State *ls)
{
    Entity *e = lua_ent_get(ls, 1);
    lua_pushstring(ls, (e && e->classname) ? e->classname : "");
    return 1;
}

// engine.camera() -> x, y, z, yaw, pitch (player position/view)
static int l_camera(lua_State *ls)
{
    Game *g = game_instance();
    float x = 0, y = 0, z = 0, yaw = 0, pit = 0;
    if (g && g->cam) {
        x = g->cam->cam.pos[0]; y = g->cam->cam.pos[1]; z = g->cam->cam.pos[2];
        yaw = g->cam->cam.yaw; pit = g->cam->cam.pitch;
    }
    lua_pushnumber(ls, x); lua_pushnumber(ls, y); lua_pushnumber(ls, z);
    lua_pushnumber(ls, yaw); lua_pushnumber(ls, pit);
    return 5;
}

// engine.terrain_height(x, z) -> terrain height at the point
static int l_terrain_height(lua_State *ls)
{
    Game *g = game_instance();
    float h = 0;
    if (g && g->world_loaded)
        h = sample_height(&g->world, (float)luaL_checknumber(ls, 1),
                          (float)luaL_checknumber(ls, 2));
    lua_pushnumber(ls, h);
    return 1;
}

// engine.teleport(x, y, z, yaw, pitch) — teleport the player
static int l_teleport(lua_State *ls)
{
    Game *g = game_instance();
    if (g) {
        float pos[3] = { (float)luaL_optnumber(ls,1,0),
                         (float)luaL_optnumber(ls,2,0),
                         (float)luaL_optnumber(ls,3,0) };
        float yaw = (float)luaL_optnumber(ls,4,0);
        float pit = (float)luaL_optnumber(ls,5,0);
        game_apply_spawn(g, pos, yaw, pit);
    }
    return 0;
}

// --- Player health (HP) ---
// engine.player_hp() -> hp, max, dead
static int l_player_hp(lua_State *ls)
{
    Game *g = game_instance();
    Health *h = g ? &g->player.hp : NULL;
    lua_pushnumber(ls, h ? h->hp : 0.0);
    lua_pushnumber(ls, h ? h->max : 0.0);
    lua_pushboolean(ls, h ? h->dead : 0);
    return 3;
}

// engine.player_damage(n) — deal n damage to the player
static int l_player_damage(lua_State *ls)
{
    Game *g = game_instance();
    if (g) health_damage(&g->player.hp, (float)luaL_checknumber(ls, 1));
    return 0;
}

// engine.player_heal(n) — heal the player by n (without n — to full)
static int l_player_heal(lua_State *ls)
{
    Game *g = game_instance();
    if (g) health_heal(&g->player.hp,
                        (float)luaL_optnumber(ls, 1, g->player.hp.max));
    return 0;
}

// engine.player_set_max(n) — set max HP (top up to full)
static int l_player_set_max(lua_State *ls)
{
    Game *g = game_instance();
    if (g) health_set_max(&g->player.hp, (float)luaL_checknumber(ls, 1), 1);
    return 0;
}

// engine.msg(text) — map message at the screen center (like triggers)
static int l_msg(lua_State *ls)
{
    Game *g = game_instance();
    const char *m = luaL_optstring(ls, 1, "");
    if (g) {
        snprintf(g->ui_msg, sizeof g->ui_msg, "%s", m);
        g->ui_msg_t = 4.0f;
    }
    return 0;
}

// engine.time() -> seconds since engine start
static int l_time(lua_State *ls)
{
    lua_pushnumber(ls, SDL_GetTicks() / 1000.0);
    return 1;
}

// --- String cvars: engine.cvar_get_string / engine.cvar_set_string -----------
static int l_cvar_get_string(lua_State *ls)
{
    lua_pushstring(ls, cvar_get_string(luaL_checkstring(ls, 1)));
    return 1;
}

static int l_cvar_set_string(lua_State *ls)
{
    cvar_set_string(luaL_checkstring(ls, 1), luaL_checkstring(ls, 2));
    return 0;
}

// --- Binds from Lua: engine.bind/unbind (input control, like Far Cry ActionMap)
static int l_bind(lua_State *ls)
{
    const char *key = luaL_checkstring(ls, 1);
    const char *cmd = luaL_checkstring(ls, 2);
    SDL_Scancode sc = binds_key_from_name(key);
    if (sc == SDL_SCANCODE_UNKNOWN) {
        lua_engine_print("engine.bind: неизвестная клавиша");
        return 0;
    }
    binds_set(sc, cmd);
    return 0;
}

static int l_unbind(lua_State *ls)
{
    const char *key = luaL_checkstring(ls, 1);
    SDL_Scancode sc = binds_key_from_name(key);
    if (sc != SDL_SCANCODE_UNKNOWN) binds_clear(sc);
    return 0;
}

// --- Materials from Lua: engine.material_register(name, { ... }) --------------
// Table: tint={r,g,b}, alpha, blend (0/1/2/3), tile={u,v}, offset={u,v}.
// The renderer later finds the material by name (material_find) — data-driven,
// like an HL2 material library. Limit — MATERIALS_MAX in material.c's registry.
static int l_material_register(lua_State *ls)
{
    const char *name = luaL_checkstring(ls, 1);
    luaL_checktype(ls, 2, LUA_TTABLE);
    Material m;
    material_defaults(&m);
    m.set = 1;
    float tmp = -1.0f;   // -1 = not set (the MAT_NUM macro writes only from numbers)
#define MAT_NUM(field, dst) \
    lua_getfield(ls, 2, field); \
    if (lua_isnumber(ls, -1)) dst = (float)lua_tonumber(ls, -1); \
    lua_pop(ls, 1)
    MAT_NUM("alpha", m.alpha);
    MAT_NUM("blend", tmp); if (tmp >= 0) m.blend = (int)tmp;
    {
        float tint[3] = { 1, 1, 1 };
        lua_getfield(ls, 2, "tint");
        if (lua_istable(ls, -1)) {
            for (int i = 0; i < 3; i++) {
                lua_rawgeti(ls, -1, i + 1);
                if (lua_isnumber(ls, -1)) tint[i] = (float)lua_tonumber(ls, -1);
                lua_pop(ls, 1);
            }
        }
        lua_pop(ls, 1);
        m.tint[0] = tint[0]; m.tint[1] = tint[1]; m.tint[2] = tint[2];
    }
    {
        float tile[2] = { 1, 1 };
        lua_getfield(ls, 2, "tile");
        if (lua_istable(ls, -1)) {
            lua_rawgeti(ls, -1, 1); if (lua_isnumber(ls, -1)) tile[0] = (float)lua_tonumber(ls, -1); lua_pop(ls, 1);
            lua_rawgeti(ls, -1, 2); if (lua_isnumber(ls, -1)) tile[1] = (float)lua_tonumber(ls, -1); lua_pop(ls, 1);
        }
        lua_pop(ls, 1);
        m.tile[0] = tile[0]; m.tile[1] = tile[1];
    }
#undef MAT_NUM
    material_register(name, &m);
    lua_pushboolean(ls, 1);
    return 1;
}

// engine.material_list() -> "name1 name2 ..." (for debugging)
static int l_material_list(lua_State *ls)
{
    char buf[512] = "";
    size_t off = 0;
    for (int i = 0; i < material_count(); i++) {
        const char *n = material_name(i);
        if (!n) continue;
        off += (size_t)snprintf(buf + off, sizeof buf - off, "%s%s",
                                off ? " " : "", n);
        if (off >= sizeof buf - 1) break;
    }
    lua_pushstring(ls, buf);
    return 1;
}

// engine.mesh(obj, x, y, z, [scale], [tex]) — place a static mesh (.obj) as a
// MeshInstance3D node. Returns 1 on success. Duplicate node lists are not
// tracked (prop_static is loaded once from the map).
static int l_mesh(lua_State *ls)
{
    const char *path = luaL_checkstring(ls, 1);
    float pos[3] = { (float)luaL_optnumber(ls,2,0),
                     (float)luaL_optnumber(ls,3,0),
                     (float)luaL_optnumber(ls,4,0) };
    float scale = (float)luaL_optnumber(ls, 5, 1.0f);
    const char *tex = lua_isstring(ls, 6) ? lua_tostring(ls, 6) : NULL;
    Game *g = game_instance();
    if (!g || !g->root) { lua_pushboolean(ls, 0); return 1; }
    SceneNode *n = mesh_make_node(path, pos, scale, tex, renderer_context());
    if (!n) { lua_pushboolean(ls, 0); return 1; }
    node_add(g->root, n);
    lua_pushboolean(ls, 1);
    return 1;
}

// engine.glb(obj, x, y, z, [scale]) — place a binary glTF (.glb).
// If the model has a skin + animation — an AnimInstance3D node (skinning),
// otherwise a static GlbInstance3D. Returns 1 on success.
static int l_glb(lua_State *ls)
{
    const char *path = luaL_checkstring(ls, 1);
    float pos[3] = { (float)luaL_optnumber(ls,2,0),
                     (float)luaL_optnumber(ls,3,0),
                     (float)luaL_optnumber(ls,4,0) };
    float scale = (float)luaL_optnumber(ls, 5, 1.0f);
    Game *g = game_instance();
    if (!g || !g->root) { lua_pushboolean(ls, 0); return 1; }
    SceneNode *n = glb_anim_has_anim(path)
        ? glb_anim_make_node(path, pos, scale, renderer_context())
        : glb_make_node(path, pos, scale, renderer_context());
    if (!n) { lua_pushboolean(ls, 0); return 1; }
    node_add(g->root, n);
    lua_pushboolean(ls, 1);
    return 1;
}

// --- Particle effects: engine.fx_* (see fx.h) ----------------------------------
// engine.fx_burst("name", x, y, z, [count]) — effect burst at a point
static int l_fx_burst(lua_State *ls)
{
    const char *name = luaL_checkstring(ls, 1);
    float pos[3] = { (float)luaL_optnumber(ls,2,0),
                     (float)luaL_optnumber(ls,3,0),
                     (float)luaL_optnumber(ls,4,0) };
    int count = (int)luaL_optinteger(ls, 5, 0);
    fx_burst(name, pos[0], pos[1], pos[2], count);
    return 0;
}

// engine.fx_emit("name", x, y, z, rate, [life]) — emitter at a point
static int l_fx_emit(lua_State *ls)
{
    const char *name = luaL_checkstring(ls, 1);
    float pos[3] = { (float)luaL_optnumber(ls,2,0),
                     (float)luaL_optnumber(ls,3,0),
                     (float)luaL_optnumber(ls,4,0) };
    float rate = (float)luaL_optnumber(ls, 5, 10.0f);
    float life = (float)luaL_optnumber(ls, 6, 0.0f);
    fx_emitter_start(name, pos, NULL, rate, life);
    return 0;
}

// engine.fx_stop("name") — stop emitters; without an argument — all
static int l_fx_stop(lua_State *ls)
{
    const char *name = luaL_optstring(ls, 1, "");
    if (name[0]) fx_emitter_stop(name);
    else fx_emitter_stop_all();
    return 0;
}

// engine.fx_clear() — stop all emitters and clear the particle pool
static int l_fx_clear(lua_State *ls)
{
    (void)ls;
    fx_emitter_stop_all();
    particles_clear();
    return 0;
}

// engine.fx_weather("name", rate) — weather behind the player; "off"/0 — stop
static int l_fx_weather(lua_State *ls)
{
    const char *name = luaL_optstring(ls, 1, "");
    float rate = (float)luaL_optnumber(ls, 2, 100.0f);
    if (!name[0] || strcmp(name, "off") == 0 || strcmp(name, "clear") == 0) {
        fx_emitter_stop_all();
        return 0;
    }
    float ext[3] = { 20.0f, 10.0f, 20.0f };
    fx_weather_start(name, 12.0f, ext, rate, 0.0f);
    return 0;
}

// engine.fx_register("name", { ... }) — a custom preset from Lua.
// Table: dir={x,y,z}, spread, speed={min,max}, life={min,max},
// size={min,max}, alpha={a0,a1}, color={r,g,b}, gravity, drag, sway,
// wind={x,y,z}, blend (0/1), streak (0/1), bounce (0/1),
// collide (0/1/2 — collision with terrain/water), count.
static int l_fx_register(lua_State *ls)
{
    const char *name = luaL_checkstring(ls, 1);
    luaL_checktype(ls, 2, LUA_TTABLE);
    ParticleParams pp;
    memset(&pp, 0, sizeof pp);
    pp.dir[1] = 1.0f;
    pp.spread = 0.5f;
    pp.speed0 = 1.0f; pp.speed1 = 2.0f;
    pp.life0 = 0.5f;  pp.life1 = 1.0f;
    pp.size0 = 0.2f;  pp.size1 = 0.2f;
    pp.alpha0 = 1.0f; pp.alpha1 = 0.0f;
    pp.r = 1.0f; pp.g = 1.0f; pp.b = 1.0f;
    int count = 8;
    float tmp = 0.0f;    // initialized: GET_NUM writes only when a number is present

#define GET_VEC(field, dst) \
    lua_getfield(ls, 2, field); \
    if (lua_istable(ls, -1)) { \
        for (int i = 0; i < 3; i++) { \
            lua_rawgeti(ls, -1, i + 1); \
            if (lua_isnumber(ls, -1)) dst[i] = (float)lua_tonumber(ls, -1); \
            lua_pop(ls, 1); \
        } \
    } \
    lua_pop(ls, 1)
#define GET_NUM(field, dst) \
    lua_getfield(ls, 2, field); \
    if (lua_isnumber(ls, -1)) dst = (float)lua_tonumber(ls, -1); \
    lua_pop(ls, 1)
#define GET_PAIR(field, a, b) \
    lua_getfield(ls, 2, field); \
    if (lua_istable(ls, -1)) { \
        lua_rawgeti(ls, -1, 1); if (lua_isnumber(ls, -1)) a = (float)lua_tonumber(ls, -1); lua_pop(ls, 1); \
        lua_rawgeti(ls, -1, 2); if (lua_isnumber(ls, -1)) b = (float)lua_tonumber(ls, -1); lua_pop(ls, 1); \
    } \
    lua_pop(ls, 1)

    GET_VEC("dir", pp.dir);
    GET_NUM("spread", pp.spread);
    GET_PAIR("speed", pp.speed0, pp.speed1);
    GET_PAIR("life", pp.life0, pp.life1);
    GET_PAIR("size", pp.size0, pp.size1);
    GET_PAIR("alpha", pp.alpha0, pp.alpha1);
    {
        float col[3] = { pp.r, pp.g, pp.b };
        GET_VEC("color", col);
        pp.r = col[0]; pp.g = col[1]; pp.b = col[2];
    }
    GET_NUM("gravity", pp.gravity);
    GET_NUM("drag", pp.drag);
    GET_NUM("sway", pp.sway);
    GET_VEC("wind", pp.wind);
    GET_NUM("blend", tmp); if (tmp > 0.5f) pp.blend = 1;
    GET_NUM("streak", tmp); if (tmp > 0.5f) pp.streak = 1;
    GET_NUM("plain", tmp); if (tmp > 0.5f) pp.plain = 1;
    GET_NUM("bounce", tmp); if (tmp > 0.5f) pp.bounce = 1;
    // collide: 1 = dies against terrain/water, 2 = settles and fades (snow)
    GET_NUM("collide", tmp); pp.collide = (int)tmp;
    GET_NUM("count", tmp); if (tmp > 0.0f) count = (int)tmp;

#undef GET_VEC
#undef GET_NUM
#undef GET_PAIR

    fx_register(name, &pp, count);
    return 0;
}

// engine.world_loaded() -> bool (world loaded)
static int l_world_loaded(lua_State *ls)
{
    Game *g = game_instance();
    lua_pushboolean(ls, g ? g->world_loaded : 0);
    return 1;
}

// engine.entity_count() -> number of entities
static int l_entity_count(lua_State *ls)
{
    lua_pushinteger(ls, entity_count());
    return 1;
}

// --- Timers: engine.after(sec, fn) ---------------------------------------------
#define LUA_TIMERS_MAX 32
typedef struct { float remaining; int ref; } LuaTimer;
static LuaTimer s_timers[LUA_TIMERS_MAX];
static int s_timers_n = 0;

static int l_after(lua_State *ls)
{
    float d = (float)luaL_checknumber(ls, 1);
    luaL_checktype(ls, 2, LUA_TFUNCTION);
    if (s_timers_n >= LUA_TIMERS_MAX) {
        lua_engine_print("engine.after: лимит таймеров");
        return 0;
    }
    s_timers[s_timers_n].remaining = d > 0.0f ? d : 0.0f;
    s_timers[s_timers_n].ref = luaL_ref(ls, LUA_REGISTRYINDEX);
    s_timers_n++;
    return 0;
}

static void lua_timers_tick(float dt)
{
    int end = s_timers_n;
    int kept = 0;
    for (int i = 0; i < end; i++) {
        s_timers[i].remaining -= dt;
        if (s_timers[i].remaining > 0.0f) {
            // Compaction by shifting (not swap-with-last): timers added by a
            // callback during THIS tick must not be pulled up into position i.
            if (kept != i) s_timers[kept] = s_timers[i];
            kept++;
            continue;
        }
        lua_rawgeti(L, LUA_REGISTRYINDEX, s_timers[i].ref);
        if (lua_pcall(L, 0, 0, 0) != 0) {
            lua_engine_print(lua_tostring(L, -1));
            lua_pop(L, 1);
        }
        luaL_unref(L, LUA_REGISTRYINDEX, s_timers[i].ref);
    }
    // Tail [end, s_timers_n) — timers added during the tick: shift them onto
    // the compacted spots, they wait for the next tick.
    int tail = s_timers_n - end;
    if (tail > 0 && kept != end)
        memmove(&s_timers[kept], &s_timers[end], (size_t)tail * sizeof s_timers[0]);
    s_timers_n = kept + tail;
}

// --- engine.update: list of per-frame callbacks (engine + map) -----------------
#define LUA_UPDATE_MAX 8
static int s_update_refs[LUA_UPDATE_MAX];
static int s_update_n = 0;
// Indices where the CURRENT map's updates/timers/subscriptions start
// (see lua_engine_map) — lua_engine_clear() uses them to remove only map
// state (engine scripts in main.lua stay).
static int s_map_update0 = 0;
static int s_map_timer0 = 0;
static int s_map_ev0[EVENTS_MAX_TYPES];

static int l_update(lua_State *ls)
{
    luaL_checktype(ls, 1, LUA_TFUNCTION);
    if (s_update_n >= LUA_UPDATE_MAX) {
        lua_engine_print("engine.update: лимит колбэков");
        return 0;
    }
    s_update_refs[s_update_n++] = luaL_ref(ls, LUA_REGISTRYINDEX);
    return 0;
}

void lua_engine_update(float dt)
{
    if (!L) return;
    lua_timers_tick(dt);
    // Snapshot of the end: callbacks added during the update will run from the next frame
    int end = s_update_n;
    for (int i = 0; i < end; i++) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, s_update_refs[i]);
        lua_pushnumber(L, dt);
        if (lua_pcall(L, 1, 0, 0) != 0) {
            lua_engine_print(lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    }
}

// --- Map script: lua/maps/<map>.lua --------------------------------------------
// Runs AFTER the world loads (at the end of game_load_world): the script gets
// access to the engine (engine) and already-spawned entities. A missing file
// is not an error.
void lua_engine_map(const char *map_name)
{
    if (!L || !map_name || !map_name[0]) return;
    // Indices from which this map's updates/timers/subscriptions start adding —
    // lua_engine_clear() uses them on unload to remove only map state
    // (engine scripts in main.lua stay).
    s_map_update0 = s_update_n;
    s_map_timer0 = s_timers_n;
    for (int t = 0; t < EVENTS_MAX_TYPES; t++) s_map_ev0[t] = s_ev_n[t];
    char path[128];
    snprintf(path, sizeof path, "lua/maps/%s.lua", map_name);
    char *src = resources_read_text(path);
    if (!src) return;
    int rc = luaL_loadbuffer(L, src, strlen(src), path);
    free(src);
    if (rc != 0) {
        SDL_Log("lua: error %s: %s", path, lua_tostring(L, -1));
        lua_pop(L, 1);               // error message off the stack
        return;
    }
    if (lua_pcall(L, 0, 0, 0) != 0) {
        SDL_Log("lua: error %s: %s", path, lua_tostring(L, -1));
        lua_pop(L, 1);
    } else {
        SDL_Log("lua: map %s: script %s", map_name, path);
    }
}

// --- Map unload: reset the map script's state ----------------------------------
// Removes update callbacks, timers and event subscriptions added by the map
// script (lua_engine_map) so they do not "tick" in the next map. Entity
// classes are overwritten by name (dedup in lua_cls_find) — leave them alone.
void lua_engine_clear(void)
{
    if (!L) return;
    while (s_timers_n > s_map_timer0)
        luaL_unref(L, LUA_REGISTRYINDEX, s_timers[--s_timers_n].ref);
    while (s_update_n > s_map_update0)
        luaL_unref(L, LUA_REGISTRYINDEX, s_update_refs[--s_update_n]);
    for (int t = 0; t < EVENTS_MAX_TYPES; t++)
        while (s_ev_n[t] > s_map_ev0[t])
            luaL_unref(L, LUA_REGISTRYINDEX, s_ev_ref[t][--s_ev_n[t]]);
}

// --- Initialization -------------------------------------------------------------
static const luaL_Reg engine_lib[] = {
    { "log",           l_log },
    { "cvar_get",      l_cvar_get },
    { "cvar_set",      l_cvar_set },
    { "cvar_get_string", l_cvar_get_string },
    { "cvar_set_string", l_cvar_set_string },
    { "bind",          l_bind },
    { "unbind",        l_unbind },
    { "material_register", l_material_register },
    { "material_list", l_material_list },
    { "mesh",          l_mesh },
    { "glb",           l_glb },
    { "exec",          l_exec },
    { "cmd",           l_cmd },
    { "on",            l_on },
    { "update",        l_update },
    { "entity_class",  l_entity_class },
    { "entity_spawn",  l_entity_spawn },
    { "entity_kill",   l_entity_kill },
    { "entity_find",   l_entity_find },
    { "entity_count",  l_entity_count },
    { "entity_pos",    l_entity_pos },
    { "entity_set_pos", l_entity_set_pos },
    { "entity_classname", l_entity_classname },
    { "world_loaded",  l_world_loaded },
    { "camera",        l_camera },
    { "terrain_height", l_terrain_height },
    { "teleport",      l_teleport },
    { "player_hp",     l_player_hp },
    { "player_damage", l_player_damage },
    { "player_heal",   l_player_heal },
    { "player_set_max", l_player_set_max },
    { "after",         l_after },
    { "msg",           l_msg },
    { "time",          l_time },
    { "fx_burst",      l_fx_burst },
    { "fx_emit",       l_fx_emit },
    { "fx_stop",       l_fx_stop },
    { "fx_clear",      l_fx_clear },
    { "fx_weather",    l_fx_weather },
    { "fx_register",   l_fx_register },
    { NULL, NULL }
};

void lua_engine_init(void)
{
    L = luaL_newstate();
    if (!L) {
        SDL_Log("lua: failed to create state");
        return;
    }
    // Sandbox: only safe libraries (no os/io/package/debug — the script cannot
    // read files or spawn processes). Strings/tables/math.
    luaL_requiref(L, "_G", luaopen_base, 1);
    // Close sandbox holes: dofile/loadfile read files through stdio directly
    // and work WITHOUT the io library — any map could read out process files
    // and leak them via engine.log/exec.
    lua_pushnil(L); lua_setfield(L, -2, "dofile");
    lua_pushnil(L); lua_setfield(L, -2, "loadfile");
    lua_pushnil(L); lua_setfield(L, -2, "load");   // load from a string — same temptation
    lua_pop(L, 1);
    luaL_requiref(L, "string", luaopen_string, 1); lua_pop(L, 1);
    luaL_requiref(L, "table", luaopen_table, 1); lua_pop(L, 1);
    luaL_requiref(L, "math", luaopen_math, 1); lua_pop(L, 1);
    luaL_newlib(L, engine_lib);
    lua_setglobal(L, "engine");

    // The script loads through resources — it may live in a folder or in a map pak
    char *src = resources_read_text("lua/main.lua");
    if (!src) {
        SDL_Log("lua: no lua/main.lua — scripts disabled");
        return;
    }
    int rc = luaL_loadbuffer(L, src, strlen(src), "main.lua");
    free(src);
    if (rc != 0) {
        lua_engine_print(lua_tostring(L, -1));
        lua_pop(L, 1);               // error message off the stack
        return;
    }
    if (lua_pcall(L, 0, 0, 0) != 0) {
        SDL_Log("lua: main.lua error: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
    } else {
        SDL_Log("lua: scripts loaded");
    }
}
