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
    // Повторная регистрация (перезагрузка карты/скрипта) — переиспользуем
    // слот по имени. Иначе каждый reload сжигал бы трамплин и Lua-реф,
    // пока пул не кончится (а console_add_command дубликат и так молча
    // отбрасывал — реф утекал).
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
            s_cmd_n--;                       // имя занято C-командой — слот назад
            return 0;
        }
    } else {
        luaL_unref(ls, LUA_REGISTRYINDEX, s_cmd_ref[i]);  // старый обработчик
    }
    s_cmd_ref[i] = luaL_ref(ls, LUA_REGISTRYINDEX);
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
        if (lua_pcall(L, 3, 0, 0) != 0) {
            lua_engine_print(lua_tostring(L, -1));
            lua_pop(L, 1);          // pcall при ошибке оставляет сообщение
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
static void lua_cls_call(int ref, Entity *e, float dt)
{
    if (ref == LUA_NOREF) return;
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    lua_pushlightuserdata(L, e);
    lua_pushnumber(L, dt);
    if (lua_pcall(L, 2, 0, 0) != 0) {
        lua_engine_print(lua_tostring(L, -1));
        lua_pop(L, 1);          // pcall при ошибке оставляет сообщение
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
    // entity_register_class игнорирует повторное имя, поэтому ВСЕГДА вешаем
    // все три диспетчера. Они ищут актуальный реф по имени при вызове
    // (lua_cls_find) — повторная регистрация с новыми колбэками подхватится.
    // Если рефа нет, lua_cls_call сам вернётся (LUA_NOREF).
    EntityClass cls = { c->name, lua_cls_spawn, lua_cls_think, lua_cls_kill };
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

// engine.teleport(x, y, z, yaw, pitch) — телепорт игрока
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

// engine.msg(text) — сообщение карты по центру экрана (как у триггеров)
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

// engine.time() -> секунды с запуска движка
static int l_time(lua_State *ls)
{
    lua_pushnumber(ls, SDL_GetTicks() / 1000.0);
    return 1;
}

// --- Строковые cvar'ы: engine.cvar_get_string / engine.cvar_set_string --------
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

// --- Бинды из Lua: engine.bind/unbind (управление вводом, как Far Cry ActionMap)
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

// --- Материалы из Lua: engine.material_register(name, { ... }) ----------------
// Таблица: tint={r,g,b}, alpha, blend (0/1/2/3), tile={u,v}, offset={u,v}.
// По имени материал потом находит рендер (material_find) — data-driven,
// как material library в HL2. Лимит — MATERIALS_MAX в реестре material.c.
static int l_material_register(lua_State *ls)
{
    const char *name = luaL_checkstring(ls, 1);
    luaL_checktype(ls, 2, LUA_TTABLE);
    Material m;
    material_defaults(&m);
    m.set = 1;
    float tmp = -1.0f;   // -1 = не задано (MAT-макрос пишет только из чисел)
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

// engine.material_list() -> "name1 name2 ..." (для отладки)
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

// engine.mesh(obj, x, y, z, [scale], [tex]) — поставить статичный меш (.obj)
// как узел MeshInstance3D. Возвращает 1 при успехе. Дубликаты узлов списка
// не отслеживаются (prop_static грузятся разово из карты).
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

// engine.glb(obj, x, y, z, [scale]) — поставить бинарный glTF (.glb).
// Если в модели есть скин+анимация — узел AnimInstance3D (скиннинг),
// иначе — статичный GlbInstance3D. Возвращает 1 при успехе.
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

// --- Эффекты частиц: engine.fx_* (см. fx.h) -------------------------------------
// engine.fx_burst("имя", x, y, z, [count]) — взрыв эффекта в точке
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

// engine.fx_emit("имя", x, y, z, rate, [life]) — эмиттер в точке
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

// engine.fx_stop("имя") — остановить эмиттеры; без аргумента — все
static int l_fx_stop(lua_State *ls)
{
    const char *name = luaL_optstring(ls, 1, "");
    if (name[0]) fx_emitter_stop(name);
    else fx_emitter_stop_all();
    return 0;
}

// engine.fx_clear() — остановить все эмиттеры и очистить пул частиц
static int l_fx_clear(lua_State *ls)
{
    (void)ls;
    fx_emitter_stop_all();
    particles_clear();
    return 0;
}

// engine.fx_weather("имя", rate) — погода за игроком; "off"/0 — стоп
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

// engine.fx_register("имя", { ... }) — свой пресет из Lua.
// Таблица: dir={x,y,z}, spread, speed={min,max}, life={min,max},
// size={min,max}, alpha={a0,a1}, color={r,g,b}, gravity, drag, sway,
// wind={x,y,z}, blend (0/1), streak (0/1), bounce (0/1),
// collide (0/1/2 — столкновение с террейном/водой), count.
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
    float tmp = 0.0f;    // инициализирован: GET_NUM пишет только при наличии числа

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
    // collide: 1 = гибнет о террейн/воду, 2 = ложится и гаснет (снег)
    GET_NUM("collide", tmp); pp.collide = (int)tmp;
    GET_NUM("count", tmp); if (tmp > 0.0f) count = (int)tmp;

#undef GET_VEC
#undef GET_NUM
#undef GET_PAIR

    fx_register(name, &pp, count);
    return 0;
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
        if (lua_pcall(L, 0, 0, 0) != 0) {
            lua_engine_print(lua_tostring(L, -1));
            lua_pop(L, 1);
        }
        luaL_unref(L, LUA_REGISTRYINDEX, s_timers[i].ref);
        s_timers[i] = s_timers[--s_timers_n];   // порядок не важен
    }
}

// --- engine.update: список per-frame колбэков (движок + карта) ------------------
#define LUA_UPDATE_MAX 8
static int s_update_refs[LUA_UPDATE_MAX];
static int s_update_n = 0;
// Индексы, с которых начинаются update/таймеры/подписки ТЕКУЩЕЙ карты
// (см. lua_engine_map) — по ним lua_engine_clear() уберёт только карточное
// состояние (движковые скрипты main.lua остаются).
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
    for (int i = 0; i < s_update_n; i++) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, s_update_refs[i]);
        lua_pushnumber(L, dt);
        if (lua_pcall(L, 1, 0, 0) != 0) {
            lua_engine_print(lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    }
}

// --- Скрипт карты: lua/maps/<карта>.lua ----------------------------------------
// Вызывается ПОСЛЕ загрузки мира (в конце game_load_world): скрипт получает
// доступ к движку (engine) и уже заспавненным сущностям. Отсутствие файла —
// не ошибка.
void lua_engine_map(const char *map_name)
{
    if (!L || !map_name || !map_name[0]) return;
    // Индексы, откуда начнут добавляться update/таймеры/подписки этой карты —
    // по ним lua_engine_clear() при выгрузке уберёт только карточное состояние
    // (движковые скрипты main.lua остаются).
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
        SDL_Log("lua: ошибка %s: %s", path, lua_tostring(L, -1));
        lua_pop(L, 1);               // сообщение об ошибке со стека
        return;
    }
    if (lua_pcall(L, 0, 0, 0) != 0) {
        SDL_Log("lua: ошибка %s: %s", path, lua_tostring(L, -1));
        lua_pop(L, 1);
    } else {
        SDL_Log("lua: карта %s: скрипт %s", map_name, path);
    }
}

// --- Выгрузка карты: сбросить состояние скрипта карты --------------------------
// Убирает update-колбэки, таймеры и подписки на события, добавленные скриптом
// карты (lua_engine_map), чтобы они не «тикали» в следующей карте. Классы
// сущностей перезаписываются по имени (dedup в lua_cls_find) — их не трогаем.
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

// --- Инициализация -------------------------------------------------------------
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
        SDL_Log("lua: не удалось создать состояние");
        return;
    }
    // Песочница: только безопасные библиотеки (без os/io/package/debug —
    // скрипт не может читать файлы и запускать процессы). Строки/таблицы/математика.
    luaL_requiref(L, "_G", luaopen_base, 1);
    // Закрыть дыры песочницы: dofile/loadfile читают файлы через stdio
    // напрямую и работают БЕЗ io-библиотеки — любая карта смогла бы вычитать
    // файлы процесса и утечь через engine.log/exec.
    lua_pushnil(L); lua_setfield(L, -2, "dofile");
    lua_pushnil(L); lua_setfield(L, -2, "loadfile");
    lua_pushnil(L); lua_setfield(L, -2, "load");   // load из строки — тоже от греха
    lua_pop(L, 1);
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
        lua_pop(L, 1);               // сообщение об ошибке со стека
        return;
    }
    if (lua_pcall(L, 0, 0, 0) != 0) {
        SDL_Log("lua: ошибка main.lua: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
    } else {
        SDL_Log("lua: скрипты загружены");
    }
}
