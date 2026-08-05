// ============================================================================
//  lua_engine.c — встраиваемый Lua: скрипты движка (см. lua_engine.h)
// ----------------------------------------------------------------------------
//  Таблица `engine` в скриптах:
//    engine.log(msg)            — печать в консоль движка
//    engine.cvar_get(name)      — значение cvar'а
//    engine.cvar_set(name, v)   — установить cvar
//    engine.exec(cmd)           — выполнить консольную команду
//    engine.cmd(name, fn)       — зарегистрировать консольную команду из Lua
//                                 (fn получает строку аргументов)
//    engine.on("событие", fn)   — подписка на событие движка
//    engine.update(fn)          — per-frame колбэк (получает dt)
//  События: entity_spawn, entity_kill, console_open, console_close.
// ============================================================================
#include "lua_engine.h"

#include "console.h"
#include "cvars.h"
#include "entity.h"
#include "events.h"
#include "game.h"
#include "resources.h"

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
    lua_engine_print(luaL_checkstring(ls, 1));   // в консоль движка
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

// --- engine.cmd: регистрация консольной команды из Lua ------------------------
// Хендлер консоли — фиксированной сигнатуры void(*)(const char*), поэтому
// каждой Lua-команде нужен свой C-трамплин. Пул статичных трамплинов.
#define LUA_CMD_MAX 16
static int s_cmd_ref[LUA_CMD_MAX];
static int s_cmd_n = 0;

static void lua_cmd_call_ref(int ref, const char *args)
{
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    lua_pushstring(L, args ? args : "");
    if (lua_pcall(L, 1, 0, 0) != 0)
        lua_engine_print(lua_tostring(L, -1));
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
    if (s_cmd_n >= LUA_CMD_MAX) {
        lua_engine_print("engine.cmd: лимит Lua-команд");
        return 0;
    }
    int i = s_cmd_n++;
    s_cmd_ref[i] = luaL_ref(ls, LUA_REGISTRYINDEX);
    console_add_command(name, "lua", s_cmd_fn[i]);
    return 0;
}

// --- engine.on: подписка на события движка ------------------------------------
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

// C-слушатель события: вызывает все Lua-колбэки типа (fn(type, a, b))
static void lua_event_dispatch(const Event *e, void *user)
{
    int type = (int)(intptr_t)user;
    for (int k = 0; k < s_ev_n[type]; k++) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, s_ev_ref[type][k]);
        lua_pushinteger(L, e->type);
        lua_pushlightuserdata(L, (void *)e->a);
        lua_pushlightuserdata(L, (void *)e->b);
        if (lua_pcall(L, 3, 0, 0) != 0)
            lua_engine_print(lua_tostring(L, -1));
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

// --- Скриптовые сущности (Lua-классы) -------------------------------------------
// engine.entity_class("имя", { spawn=fn, think=fn, kill=fn }) регистрирует
// класс сущности: C-колбэки (lua_cls_*) по e->classname находят Lua-рефы и
// вызывают их с (Entity, dt). Никаких трамплинов — идентичность по имени.
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

// Вызывает Lua-колбэк класса с (entity, dt)
static void lua_cls_call(LuaCls *c, int ref, Entity *e, float dt)
{
    if (ref == LUA_NOREF) return;
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    lua_pushlightuserdata(L, e);
    lua_pushnumber(L, dt);
    if (lua_pcall(L, 2, 0, 0) != 0)
        lua_engine_print(lua_tostring(L, -1));
}

static void lua_cls_spawn(Entity *e)
{
    LuaCls *c = lua_cls_find(e->classname);
    if (c) lua_cls_call(c, c->spawn, e, 0.0f);
}
static void lua_cls_think(Entity *e, float dt)
{
    LuaCls *c = lua_cls_find(e->classname);
    if (c) lua_cls_call(c, c->think, e, dt);
}
static void lua_cls_kill(Entity *e)
{
    LuaCls *c = lua_cls_find(e->classname);
    if (c) lua_cls_call(c, c->kill, e, 0.0f);
}

// Вспомогательная: взять колбэк из таблицы в ref (или LUA_NOREF)
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
    // Повторная регистрация (перезагрузка карты) — перезаписываем слот по имени
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
    EntityClass cls = {
        c->name,
        c->spawn >= 0 ? lua_cls_spawn : NULL,
        c->think >= 0 ? lua_cls_think : NULL,
        c->kill  >= 0 ? lua_cls_kill  : NULL
    };
    entity_register_class(&cls);
    return 0;
}

// engine.entity_spawn("класс", x, y, z) -> Entity (lightuserdata)
static int l_entity_spawn(lua_State *ls)
{
    const char *cls = luaL_checkstring(ls, 1);
    float pos[3] = { (float)luaL_optnumber(ls,2,0),
                     (float)luaL_optnumber(ls,3,0),
                     (float)luaL_optnumber(ls,4,0) };
    Entity *e = entity_spawn(cls, pos, NULL, NULL);
    lua_pushlightuserdata(ls, e);
    return 1;
}

// engine.entity_kill(entity)
static int l_entity_kill(lua_State *ls)
{
    Entity *e = lua_touserdata(ls, 1);
    if (e) entity_kill(e);
    return 0;
}

// engine.entity_find("класс", n) -> Entity (n-я с нуля) или nil
static int l_entity_find(lua_State *ls)
{
    const char *cls = luaL_checkstring(ls, 1);
    int n = (int)luaL_optinteger(ls, 2, 0);
    Entity *e = entity_find_class(cls, n);
    if (e) lua_pushlightuserdata(ls, e);
    else lua_pushnil(ls);
    return 1;
}

// engine.entity_pos(e) -> x, y, z (позиция узла сущности)
static int l_entity_pos(lua_State *ls)
{
    Entity *e = lua_touserdata(ls, 1);
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
    Entity *e = lua_touserdata(ls, 1);
    if (e && e->node) {
        e->node->pos[0] = (float)luaL_optnumber(ls, 2, 0);
        e->node->pos[1] = (float)luaL_optnumber(ls, 3, 0);
        e->node->pos[2] = (float)luaL_optnumber(ls, 4, 0);
    }
    return 0;
}

// engine.entity_classname(e) -> строка
static int l_entity_classname(lua_State *ls)
{
    Entity *e = lua_touserdata(ls, 1);
    lua_pushstring(ls, (e && e->classname) ? e->classname : "");
    return 1;
}

// engine.camera() -> x, y, z, yaw, pitch (позиция/взгляд игрока)
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

// engine.terrain_height(x, z) -> высота рельефа в точке
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

// engine.world_loaded() -> bool (мир загружен)
static int l_world_loaded(lua_State *ls)
{
    Game *g = game_instance();
    lua_pushboolean(ls, g ? g->world_loaded : 0);
    return 1;
}

// engine.entity_count() -> число сущностей
static int l_entity_count(lua_State *ls)
{
    lua_pushinteger(ls, entity_count());
    return 1;
}

// --- Таймеры: engine.after(сек, fn) ---------------------------------------------
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
    for (int i = 0; i < s_timers_n; ) {
        s_timers[i].remaining -= dt;
        if (s_timers[i].remaining > 0.0f) { i++; continue; }
        lua_rawgeti(L, LUA_REGISTRYINDEX, s_timers[i].ref);
        if (lua_pcall(L, 0, 0, 0) != 0)
            lua_engine_print(lua_tostring(L, -1));
        luaL_unref(L, LUA_REGISTRYINDEX, s_timers[i].ref);
        s_timers[i] = s_timers[--s_timers_n];   // порядок не важен
    }
}

// --- engine.update: per-frame колбэк -------------------------------------------
static int s_update_ref = LUA_NOREF;

static int l_update(lua_State *ls)
{
    luaL_checktype(ls, 1, LUA_TFUNCTION);
    s_update_ref = luaL_ref(ls, LUA_REGISTRYINDEX);
    return 0;
}

void lua_engine_update(float dt)
{
    if (!L) return;
    lua_timers_tick(dt);
    if (s_update_ref != LUA_NOREF) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, s_update_ref);
        lua_pushnumber(L, dt);
        if (lua_pcall(L, 1, 0, 0) != 0)
            lua_engine_print(lua_tostring(L, -1));
    }
}

// --- Скрипт карты: lua/maps/<карта>.lua ----------------------------------------
// Вызывается ПОСЛЕ загрузки мира (в конце game_load_world): скрипт получает
// доступ к движку (engine) и уже заспавненным сущностям. Отсутствие файла —
// не ошибка.
void lua_engine_map(const char *map_name)
{
    if (!L || !map_name || !map_name[0]) return;
    char path[128];
    snprintf(path, sizeof path, "lua/maps/%s.lua", map_name);
    char *src = resources_read_text(path);
    if (!src) return;
    int rc = luaL_loadbuffer(L, src, strlen(src), path);
    free(src);
    if (rc != 0) {
        SDL_Log("lua: ошибка %s: %s", path, lua_tostring(L, -1));
        return;
    }
    if (lua_pcall(L, 0, 0, 0) != 0)
        SDL_Log("lua: ошибка %s: %s", path, lua_tostring(L, -1));
    else
        SDL_Log("lua: карта %s: скрипт %s", map_name, path);
}

// --- Инициализация -------------------------------------------------------------
static const luaL_Reg engine_lib[] = {
    { "log",           l_log },
    { "cvar_get",      l_cvar_get },
    { "cvar_set",      l_cvar_set },
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
    { "after",         l_after },
    { NULL, NULL }
};

void lua_engine_init(void)
{
    L = luaL_newstate();
    if (!L) {
        SDL_Log("lua: не удалось создать состояние");
        return;
    }
    // Песочница: только безопасные библиотеки (без os/io/package/debug —
    // скрипт не может читать файлы и запускать процессы). Строки/таблицы/математика.
    luaL_requiref(L, "_G", luaopen_base, 1); lua_pop(L, 1);
    luaL_requiref(L, "string", luaopen_string, 1); lua_pop(L, 1);
    luaL_requiref(L, "table", luaopen_table, 1); lua_pop(L, 1);
    luaL_requiref(L, "math", luaopen_math, 1); lua_pop(L, 1);
    luaL_newlib(L, engine_lib);
    lua_setglobal(L, "engine");

    // Скрипт грузится через ресурсы — может лежать в папке или в паке карты
    char *src = resources_read_text("lua/main.lua");
    if (!src) {
        SDL_Log("lua: нет lua/main.lua — скрипты выключены");
        return;
    }
    int rc = luaL_loadbuffer(L, src, strlen(src), "main.lua");
    free(src);
    if (rc != 0) {
        lua_engine_print(lua_tostring(L, -1));
        return;
    }
    if (lua_pcall(L, 0, 0, 0) != 0)
        SDL_Log("lua: ошибка main.lua: %s", lua_tostring(L, -1));
    else
        SDL_Log("lua: скрипты загружены");
}
