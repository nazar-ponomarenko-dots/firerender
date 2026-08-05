// ============================================================================
//  mapdata.c — загрузка map/map.json: поле, свет, объекты (см. mapdata.h)
// ============================================================================
#include "mapdata.h"

#include "json.h"
#include "resources.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAP_MAX_LIGHTS   16    // потолок источников света из JSON
#define MAP_MAX_OBJECTS  256   // потолок объектов (моделей) из JSON
#define MAP_MAX_COLLISIONS 256 // потолок коллизий-квадратов

static MapLight s_lights[MAP_MAX_LIGHTS];
static int s_light_count;
static MapObject s_objects[MAP_MAX_OBJECTS];
static int s_object_count;
static MapCollision s_collisions[MAP_MAX_COLLISIONS];
static int s_collision_count;
static MapSound3D s_sounds[MAP_MAX_SOUNDS];
static int s_sound_count;
static MapTrigger s_triggers[MAP_MAX_TRIGGERS];
static int s_trigger_count;
static MapFauna s_fauna[MAP_MAX_FAUNA];
static int s_fauna_count;
static char s_image[256];
static float s_field_center[2], s_field_size[2];
static int s_field_set;
static int s_field_attached;
static float s_player_pos[3];
static float s_player_yaw, s_player_pitch, s_player_fov = 60.0f;
static int s_player_set;

// Финальный формат карты: info, ambient/music, commands[], cvars{}
static MapInfo s_info;
static MapSound s_ambient, s_music;
static int s_ambient_set, s_music_set;
static char s_commands[MAP_MAX_COMMANDS][MAP_CMD_LEN];
static int s_command_count;
static char s_cvar_names[MAP_MAX_CVARS][64];
static float s_cvar_values[MAP_MAX_CVARS];
static int s_cvar_count;

// Воксельный объёмный туман (fog = { ... } в map.json)
static MapFog s_fog = {
    .enabled = 1,
    .strength = 0.9f,
    .density_ground = 0.020f,
    .ground_height = 5.0f,
    .density_field = 0.032f,
    .field_height = 8.0f,
    .color = { 0.52f, 0.56f, 0.60f },      // базовый цвет тумана (дымка)
    .color_sun = { 0.85f, 0.62f, 0.45f },  // тёплый к солнцу
    .glare = 0.20f,
    .ray_strength = 1.4f,
};

// Рельеф (terrain = { ... })
static MapTerrain s_terrain = {
    .min_height = -100.0f, .max_height = 400.0f,
    .band_t0 = 0.16f, .band_t1 = 0.40f,
    .band_h0 = -50.0f, .band_h1 = 200.0f,
    .blur = 2,
    .ground_tex = "img/ground.png",
    .tex_scale = 6.4f,
    .field_color = { 0.68f, 0.57f, 0.33f },
    .field_blend = 0.85f,
    .fog_start = 0.75f, .fog_end = 1.60f,
};

// Растительность (veg = { ... })
static MapVeg s_veg = {
    .grass_density = 0.3f, .wheat_density = 140.0f,
    .grass_floor = 0.004f, .wheat_floor = 1.0f,
    .dyn_radius = 48.0f, .vis_radius = 220.0f,
    .falloff = 16.0f,
};

// Небо (sky = { ... })
static MapSky s_sky = {
    .tint = { 1.0f, 1.0f, 1.0f },
    .tex = "img/skybox.png",
};

// Постобработка (bloom = { ... })
static MapBloom s_bloom = {
    .threshold = 0.75f, .intensity = 1.0f, .strength = 0.425f,
};

// Тени (shadows = { ... })
static MapShadows s_shadows = {
    .size = 2048,
};

// Читает map.json через менеджер ресурсов (диск -> ../диск -> вшитые данные).
// Возвращает malloc'нутый текст или NULL.
static char *read_map_json(const char *path)
{
    return resources_read_text(path);
}

// Массив [x, y, z] -> float[3] (недостающие элементы = def)
static void parse_vec3(const JsonNode *arr, float out[3], float def)
{
    out[0] = (float)json_num(json_array_get(arr, 0), def);
    out[1] = (float)json_num(json_array_get(arr, 1), def);
    out[2] = (float)json_num(json_array_get(arr, 2), def);
}

// Блок render = { ... } — полная настройка отображения объекта
static void parse_render(const JsonNode *obj, MapObjectRender *r)
{
    const JsonNode *rd = json_find(obj, "render");
    if (!rd) return;
    r->set = 1;
    r->tint[0] = r->tint[1] = r->tint[2] = 1.0f;
    r->alpha = 1.0f;
    r->blend = RENDER_BLEND_ALPHATEST;
    r->cull = RENDER_CULL_NONE;
    r->depth_write = 1;
    r->wrap = TEX_WRAP_CLAMP;
    r->filter = TEX_FILTER_LINEAR;
    r->tile[0] = r->tile[1] = 1.0f;
    r->offset[0] = r->offset[1] = 0.0f;
    r->scroll[0] = r->scroll[1] = 0.0f;
    const JsonNode *tint = json_find(rd, "tint");
    if (tint && json_array_len(tint) == 3)
        parse_vec3(tint, r->tint, 1.0f);
    r->alpha = (float)json_num(json_find(rd, "alpha"), r->alpha);
    // blend: alphatest | alpha | additive | opaque
    const char *bl = json_str(json_find(rd, "blend"), "");
    if (bl[0]) {
        if (strcmp(bl, "alpha") == 0) r->blend = RENDER_BLEND_ALPHA;
        else if (strcmp(bl, "additive") == 0) r->blend = RENDER_BLEND_ADDITIVE;
        else if (strcmp(bl, "opaque") == 0) r->blend = RENDER_BLEND_OPAQUE;
    }
    // cull: none | back | front
    const char *cu = json_str(json_find(rd, "cull"), "");
    if (cu[0]) {
        if (strcmp(cu, "back") == 0) r->cull = RENDER_CULL_BACK;
        else if (strcmp(cu, "front") == 0) r->cull = RENDER_CULL_FRONT;
    }
    r->depth_write = json_bool(json_find(rd, "depth_write"), r->depth_write);
    // Текстура: wrap | filter | tile | offset | scroll
    const JsonNode *tx = json_find(rd, "texture");
    if (tx) {
        const char *wr = json_str(json_find(tx, "wrap"), "");
        if (wr[0]) {
            if (strcmp(wr, "repeat") == 0) r->wrap = TEX_WRAP_REPEAT;
            else if (strcmp(wr, "mirror") == 0) r->wrap = TEX_WRAP_MIRROR;
        }
        const char *ft = json_str(json_find(tx, "filter"), "");
        if (ft[0]) {
            if (strcmp(ft, "nearest") == 0) r->filter = TEX_FILTER_NEAREST;
            else if (strcmp(ft, "mipmap") == 0) r->filter = TEX_FILTER_MIPMAP;
        }
        const JsonNode *tile = json_find(tx, "tile");
        if (tile && json_array_len(tile) == 2) {
            r->tile[0] = (float)json_num(json_array_get(tile, 0), 1.0);
            r->tile[1] = (float)json_num(json_array_get(tile, 1), 1.0);
        }
        const JsonNode *off = json_find(tx, "offset");
        if (off && json_array_len(off) == 2) {
            r->offset[0] = (float)json_num(json_array_get(off, 0), 0.0);
            r->offset[1] = (float)json_num(json_array_get(off, 1), 0.0);
        }
        const JsonNode *scr = json_find(tx, "scroll");
        if (scr && json_array_len(scr) == 2) {
            r->scroll[0] = (float)json_num(json_array_get(scr, 0), 0.0);
            r->scroll[1] = (float)json_num(json_array_get(scr, 1), 0.0);
        }
    }
    // Фикс. размер: size = { "width": 10, "height": 6 } (метры)
    const JsonNode *sz = json_find(rd, "size");
    if (sz) {
        r->fixed_size = 1;
        r->width = (float)json_num(json_find(sz, "width"), 5.0);
        r->height = (float)json_num(json_find(sz, "height"), 5.0);
    }
}

// Общий парсер: массив строк "commands" -> массив команд
static void parse_commands(const JsonNode *obj, const char *key,
                           char out[MAP_ACTIONS_MAX][MAP_CMD_LEN], int *n)
{
    *n = 0;
    const JsonNode *cm = json_find(obj, key);
    if (!cm) return;
    int cnt = json_array_len(cm);
    for (int i = 0; i < cnt && *n < MAP_ACTIONS_MAX; i++) {
        const char *c = json_str(json_array_get(cm, i), "");
        if (c[0]) snprintf(out[(*n)++], MAP_CMD_LEN, "%s", c);
    }
}

// Сообщение + звук (общие поля действий)
static void parse_action_sound(const JsonNode *obj, char *message, int msz,
                               char *sound, int ssz)
{
    const char *msg = json_str(json_find(obj, "message"), "");
    if (msg[0]) snprintf(message, (size_t)msz, "%s", msg);
    const char *snd = json_str(json_find(obj, "sound"), "");
    if (snd[0]) snprintf(sound, (size_t)ssz, "%s", snd);
}

// Блок use = { ... } — взаимодействие с объектом по E
static void parse_use(const JsonNode *obj, MapUse *u)
{
    const JsonNode *us = json_find(obj, "use");
    if (!us) return;
    u->set = 1;
    parse_commands(us, "commands", u->commands, &u->cmd_n);
    parse_action_sound(us, u->message, sizeof u->message,
                       u->sound, sizeof u->sound);
    u->once = json_bool(json_find(us, "once"), 0);
}

static void parse_trigger(const JsonNode *obj)
{
    if (s_trigger_count >= MAP_MAX_TRIGGERS) return;
    MapTrigger *t = &s_triggers[s_trigger_count];
    memset(t, 0, sizeof *t);
    snprintf(t->name, sizeof t->name, "%s",
             json_str(json_find(obj, "name"), ""));
    parse_vec3(json_find(obj, "pos"), t->pos, 0.0f);
    const JsonNode *sz = json_find(obj, "size");
    if (sz && json_array_len(sz) == 2) {
        t->size[0] = (float)json_num(json_array_get(sz, 0), 1.0);
        t->size[1] = (float)json_num(json_array_get(sz, 1), 1.0);
    } else {
        t->size[0] = t->size[1] = 1.0f;
    }
    t->once = json_bool(json_find(obj, "once"), 0);
    parse_commands(obj, "on_enter", t->commands, &t->cmd_n);
    parse_action_sound(obj, t->message, sizeof t->message,
                       t->sound, sizeof t->sound);
    const JsonNode *tp = json_find(obj, "teleport");
    if (tp && json_array_len(tp) == 3) {
        parse_vec3(tp, t->teleport, 0.0f);
        t->has_tp = 1;
    }
    s_trigger_count++;
}

static void parse_fauna(const JsonNode *obj)
{
    if (s_fauna_count >= MAP_MAX_FAUNA) return;
    MapFauna *f = &s_fauna[s_fauna_count];
    memset(f, 0, sizeof *f);
    const char *type = json_str(json_find(obj, "type"), "");
    if (strcmp(type, "fireflies") == 0) f->type = FAUNA_FIREFLIES;
    else if (strcmp(type, "butterflies") == 0) f->type = FAUNA_BUTTERFLIES;
    else if (strcmp(type, "birds") == 0) f->type = FAUNA_BIRDS;
    else {
        SDL_Log("Map: unknown fauna type '%s', skipped", type ? type : "?");
        return;
    }
    const JsonNode *ar = json_find(obj, "area");
    if (ar && json_array_len(ar) == 2) {
        f->area[0] = (float)json_num(json_array_get(ar, 0), 0);
        f->area[1] = (float)json_num(json_array_get(ar, 1), 0);
    }
    const JsonNode *sz = json_find(obj, "size");
    if (sz && json_array_len(sz) == 2) {
        f->size[0] = (float)json_num(json_array_get(sz, 0), 50.0);
        f->size[1] = (float)json_num(json_array_get(sz, 1), 50.0);
    } else {
        f->size[0] = f->size[1] = 50.0f;
    }
    f->count = (int)json_num(json_find(obj, "count"),
                             f->type == FAUNA_FIREFLIES ? 40 : 12);
    f->height = (float)json_num(json_find(obj, "height"),
                                f->type == FAUNA_BIRDS ? 40.0 : 2.5);
    s_fauna_count++;
}

// Тип света из строки JSON ("directional"/"point"/"spot"). -1 — неизвестный.
static int parse_light_type(const char *s)
{
    if (strcmp(s, "directional") == 0) return LIGHT_DIRECTIONAL;
    if (strcmp(s, "point") == 0) return LIGHT_POINT;
    if (strcmp(s, "spot") == 0) return LIGHT_SPOT;
    return -1;
}

static void parse_light(const JsonNode *obj)
{
    if (s_light_count >= MAP_MAX_LIGHTS) return;
    const char *type_s = json_str(json_find(obj, "type"), NULL);
    int type = type_s ? parse_light_type(type_s) : LIGHT_DIRECTIONAL;
    if (type < 0) {
        SDL_Log("Map: unknown light type '%s', skipped", type_s);
        return;
    }
    MapLight *l = &s_lights[s_light_count];
    memset(l, 0, sizeof *l);
    l->type = (MapLightType)type;
    snprintf(l->name, sizeof l->name, "%s",
             json_str(json_find(obj, "name"), ""));
    parse_vec3(json_find(obj, "pos"), l->pos, 0.0f);
    parse_vec3(json_find(obj, "dir"), l->dir, 0.0f);
    parse_vec3(json_find(obj, "color"), l->color, 1.0f);
    l->intensity = (float)json_num(json_find(obj, "intensity"), 1.0);
    // Тени источника: shadow = { enabled, bias, bias_veg }. Значения по
    // умолчанию совпадают с прежними константами (4/2 латеральных текселя).
    const JsonNode *sh = json_find(obj, "shadow");
    l->shadow_on = json_bool(json_find(sh, "enabled"), 1);
    l->shadow_bias = (float)json_num(json_find(sh, "bias"), 4.0);
    l->shadow_bias_veg = (float)json_num(json_find(sh, "bias_veg"), 2.0);
    // attached — свет следует за игроком (точечный фонарь)
    l->attached = json_bool(json_find(obj, "attached"), 0);
    s_light_count++;
}

static void parse_object(const JsonNode *obj)
{
    if (s_object_count >= MAP_MAX_OBJECTS) return;
    MapObject *o = &s_objects[s_object_count];
    memset(o, 0, sizeof *o);
    snprintf(o->name, sizeof o->name, "%s",
             json_str(json_find(obj, "name"), ""));
    snprintf(o->model, sizeof o->model, "%s",
             json_str(json_find(obj, "model"), ""));
    // Класс сущности (для entity-системы); по умолчанию — prop_static
    snprintf(o->class, sizeof o->class, "%s",
             json_str(json_find(obj, "class"), "prop_static"));
    parse_vec3(json_find(obj, "pos"), o->pos, 0.0f);
    parse_vec3(json_find(obj, "rot"), o->rot, 0.0f);
    const JsonNode *sc = json_find(obj, "scale");
    if (sc && json_array_len(sc) == 3)
        parse_vec3(sc, o->scale, 1.0f);
    else
        o->scale[0] = o->scale[1] = o->scale[2] = 1.0f;
    // Биллборд/коллизия/радиус: флаги по умолчанию выключены
    o->billboard = json_bool(json_find(obj, "billboard"), 0);
    o->collision = json_bool(json_find(obj, "collision"), 0);
    o->radius = (float)json_num(json_find(obj, "radius"), 1.5);
    // Шейдер: встроенное имя ("default"/"billboard"/"flat") или свой файл:
    //   "shader": "flat"
    //   "shader": { "vs": "shaders/my.vs", "fs": "shaders/my.fs" }
    o->shader_kind = OBJ_SHADER_DEFAULT;
    const JsonNode *sh = json_find(obj, "shader");
    if (sh) {
        if (sh->type == JSON_STRING) {
            const char *s = sh->sval;
            if (s && strcmp(s, "billboard") == 0) {
                o->shader_kind = OBJ_SHADER_BILLBOARD;
                o->billboard = 1;
            } else if (s && strcmp(s, "flat") == 0) {
                o->shader_kind = OBJ_SHADER_FLAT;
            }
        } else if (sh->type == JSON_OBJECT) {
            const char *vs = json_str(json_find(sh, "vs"), "");
            const char *fs = json_str(json_find(sh, "fs"), "");
            if (fs[0]) {
                o->shader_kind = OBJ_SHADER_CUSTOM;
                if (vs[0]) snprintf(o->shader_vs, sizeof o->shader_vs, "%s", vs);
                snprintf(o->shader_fs, sizeof o->shader_fs, "%s", fs);
            }
        }
    }
    // Полная настройка отображения (render = { ... })
    memset(&o->render, 0, sizeof o->render);
    parse_render(obj, &o->render);
    // Взаимодействие по E (use = { ... })
    memset(&o->use, 0, sizeof o->use);
    parse_use(obj, &o->use);
    s_object_count++;
}

static void parse_collision(const JsonNode *obj)
{
    if (s_collision_count >= MAP_MAX_COLLISIONS) return;
    MapCollision *c = &s_collisions[s_collision_count];
    memset(c, 0, sizeof *c);
    snprintf(c->object, sizeof c->object, "%s",
             json_str(json_find(obj, "object"), ""));
    parse_vec3(json_find(obj, "pos"), c->pos, 0.0f);
    const JsonNode *s = json_find(obj, "size");
    if (s && json_array_len(s) == 2) {
        c->size[0] = (float)json_num(json_array_get(s, 0), 1.0);
        c->size[1] = (float)json_num(json_array_get(s, 1), 1.0);
    } else {
        c->size[0] = c->size[1] = 1.0f;
    }
    c->height = (float)json_num(json_find(obj, "height"), 4.0);
    s_collision_count++;
}

static void parse_sound(const JsonNode *obj)
{
    if (s_sound_count >= MAP_MAX_SOUNDS) return;
    MapSound3D *s = &s_sounds[s_sound_count];
    memset(s, 0, sizeof *s);
    snprintf(s->sound, sizeof s->sound, "%s",
             json_str(json_find(obj, "sound"), ""));
    if (!s->sound[0]) return;
    parse_vec3(json_find(obj, "pos"), s->pos, 0.0f);
    s->volume = (float)json_num(json_find(obj, "volume"), 0.7);
    s->radius = (float)json_num(json_find(obj, "radius"), 25.0);
    s_sound_count++;
}
int mapdata_load(const char *path)
{
    char *text = read_map_json(path);
    if (!text) {
        SDL_Log("Map data: cannot read %s", path);
        return -1;
    }
    const JsonNode *root = json_parse(text, strlen(text));
    free(text);
    if (!root) {
        SDL_Log("Map data: JSON parse error in %s", path);
        return -1;
    }

    // --- Карта высот: map/image (путь относительно map/) ---------------------
    const JsonNode *mapn = json_find(root, "map");
    const char *img = json_str(json_find(mapn, "image"), NULL);
    if (img) {
        snprintf(s_image, sizeof s_image, "%s", img);
    } else {
        SDL_Log("Map data: no map.image in %s, using map.png", path);
        snprintf(s_image, sizeof s_image, "map.png");
    }

    // --- Поле пшеницы: field/center [x,z] и field/size [w,d] (метры) ---------
    s_field_set = 0;
    const JsonNode *fld = json_find(root, "field");
    if (fld) {
        const JsonNode *c = json_find(fld, "center");
        const JsonNode *s = json_find(fld, "size");
        if (c && s && json_array_len(c) >= 2 && json_array_len(s) >= 2) {
            s_field_center[0] = (float)json_num(json_array_get(c, 0), 0);
            s_field_center[1] = (float)json_num(json_array_get(c, 1), 0);
            s_field_size[0] = (float)json_num(json_array_get(s, 0), 0);
            s_field_size[1] = (float)json_num(json_array_get(s, 1), 0);
            s_field_set = 1;
        }
    }
    if (!s_field_set)
        SDL_Log("Map data: field not set in %s, default (world center)", path);

    // --- Поле прикреплено к игроку? field/attached (бесконечное поле) --------
    s_field_attached = json_bool(json_find(fld, "attached"), 0);

    // --- Игрок: player/pos [x,y,z], player/yaw, player/pitch (старт) ----------
    s_player_set = 0;
    const JsonNode *pl = json_find(root, "player");
    if (pl && json_find(pl, "pos")) {
        parse_vec3(json_find(pl, "pos"), s_player_pos, 0.0f);
        s_player_yaw = (float)json_num(json_find(pl, "yaw"), 3.14159);
        s_player_pitch = (float)json_num(json_find(pl, "pitch"), -0.5);
        s_player_fov = (float)json_num(json_find(pl, "fov"), 60.0);
        s_player_set = 1;
    }

    // --- Воксельный объёмный туман: fog = { ... } ------------------------------
    const JsonNode *fg = json_find(root, "fog");
    if (fg) {
        s_fog.enabled = json_bool(json_find(fg, "enabled"), s_fog.enabled);
        s_fog.strength = (float)json_num(json_find(fg, "strength"), s_fog.strength);
        s_fog.density_ground = (float)json_num(json_find(fg, "density_ground"),
                                               s_fog.density_ground);
        s_fog.ground_height = (float)json_num(json_find(fg, "ground_height"),
                                              s_fog.ground_height);
        s_fog.density_field = (float)json_num(json_find(fg, "density_field"),
                                              s_fog.density_field);
        s_fog.field_height = (float)json_num(json_find(fg, "field_height"),
                                             s_fog.field_height);
        const JsonNode *col = json_find(fg, "color");
        if (col && json_array_len(col) == 3)
            parse_vec3(col, s_fog.color, 0.0f);
        const JsonNode *csun = json_find(fg, "color_sun");
        if (csun && json_array_len(csun) == 3)
            parse_vec3(csun, s_fog.color_sun, 0.0f);
        s_fog.glare = (float)json_num(json_find(fg, "glare"), s_fog.glare);
        s_fog.ray_strength = (float)json_num(json_find(fg, "ray_strength"),
                                             s_fog.ray_strength);
    }

    // --- Рельеф: terrain = { ... } -------------------------------------------
    const JsonNode *tr = json_find(root, "terrain");
    if (tr) {
        s_terrain.min_height = (float)json_num(json_find(tr, "min_height"),
                                               s_terrain.min_height);
        s_terrain.max_height = (float)json_num(json_find(tr, "max_height"),
                                               s_terrain.max_height);
        s_terrain.band_t0 = (float)json_num(json_find(tr, "band_t0"),
                                            s_terrain.band_t0);
        s_terrain.band_t1 = (float)json_num(json_find(tr, "band_t1"),
                                            s_terrain.band_t1);
        s_terrain.band_h0 = (float)json_num(json_find(tr, "band_h0"),
                                            s_terrain.band_h0);
        s_terrain.band_h1 = (float)json_num(json_find(tr, "band_h1"),
                                            s_terrain.band_h1);
        s_terrain.blur = (int)json_num(json_find(tr, "blur"), s_terrain.blur);
        const char *gtex = json_str(json_find(tr, "ground_tex"), "");
        if (gtex[0]) snprintf(s_terrain.ground_tex, sizeof s_terrain.ground_tex,
                              "%s", gtex);
        s_terrain.tex_scale = (float)json_num(json_find(tr, "tex_scale"),
                                              s_terrain.tex_scale);
        const JsonNode *fc = json_find(tr, "field_color");
        if (fc && json_array_len(fc) == 3)
            parse_vec3(fc, s_terrain.field_color, 0.0f);
        s_terrain.field_blend = (float)json_num(json_find(tr, "field_blend"),
                                                s_terrain.field_blend);
        s_terrain.fog_start = (float)json_num(json_find(tr, "fog_start"),
                                              s_terrain.fog_start);
        s_terrain.fog_end = (float)json_num(json_find(tr, "fog_end"),
                                            s_terrain.fog_end);
    }

    // --- Растительность: veg = { ... } --------------------------------------
    const JsonNode *vg = json_find(root, "veg");
    if (vg) {
        s_veg.grass_density = (float)json_num(json_find(vg, "grass_density"),
                                              s_veg.grass_density);
        s_veg.wheat_density = (float)json_num(json_find(vg, "wheat_density"),
                                              s_veg.wheat_density);
        s_veg.grass_floor = (float)json_num(json_find(vg, "grass_floor"),
                                            s_veg.grass_floor);
        s_veg.wheat_floor = (float)json_num(json_find(vg, "wheat_floor"),
                                            s_veg.wheat_floor);
        s_veg.dyn_radius = (float)json_num(json_find(vg, "dyn_radius"),
                                           s_veg.dyn_radius);
        s_veg.vis_radius = (float)json_num(json_find(vg, "vis_radius"),
                                           s_veg.vis_radius);
        s_veg.falloff = (float)json_num(json_find(vg, "falloff"), s_veg.falloff);
    }

    // --- Небо: sky = { ... } -------------------------------------------------
    const JsonNode *sk = json_find(root, "sky");
    if (sk) {
        const JsonNode *tint = json_find(sk, "tint");
        if (tint && json_array_len(tint) == 3)
            parse_vec3(tint, s_sky.tint, 1.0f);
        const char *stex = json_str(json_find(sk, "tex"), "");
        if (stex[0]) snprintf(s_sky.tex, sizeof s_sky.tex, "%s", stex);
    }

    // --- Постобработка: bloom = { ... } --------------------------------------
    const JsonNode *bl = json_find(root, "bloom");
    if (bl) {
        s_bloom.threshold = (float)json_num(json_find(bl, "threshold"),
                                            s_bloom.threshold);
        s_bloom.intensity = (float)json_num(json_find(bl, "intensity"),
                                            s_bloom.intensity);
        s_bloom.strength = (float)json_num(json_find(bl, "strength"),
                                           s_bloom.strength);
    }

    // --- Тени: shadows = { ... } ---------------------------------------------
    const JsonNode *shd = json_find(root, "shadows");
    if (shd)
        s_shadows.size = (int)json_num(json_find(shd, "size"), s_shadows.size);

    // --- Информация о карте: info = { name, author, description } --------------
    memset(&s_info, 0, sizeof s_info);
    const JsonNode *inf = json_find(root, "info");
    if (inf) {
        const char *n = json_str(json_find(inf, "name"), "");
        const char *a = json_str(json_find(inf, "author"), "");
        const char *d = json_str(json_find(inf, "description"), "");
        if (n[0]) snprintf(s_info.name, sizeof s_info.name, "%s", n);
        if (a[0]) snprintf(s_info.author, sizeof s_info.author, "%s", a);
        if (d[0]) snprintf(s_info.description, sizeof s_info.description, "%s", d);
    }

    // --- Звук карты: ambient = { sound, volume }, music = { sound, volume } ----
    s_ambient_set = s_music_set = 0;
    const JsonNode *amb = json_find(root, "ambient");
    if (amb) {
        const char *snd = json_str(json_find(amb, "sound"), "");
        if (snd[0]) {
            snprintf(s_ambient.sound, sizeof s_ambient.sound, "%s", snd);
            s_ambient.volume = (float)json_num(json_find(amb, "volume"), 0.5);
            s_ambient_set = 1;
        }
    }
    const JsonNode *mus = json_find(root, "music");
    if (mus) {
        const char *snd = json_str(json_find(mus, "sound"), "");
        if (snd[0]) {
            snprintf(s_music.sound, sizeof s_music.sound, "%s", snd);
            s_music.volume = (float)json_num(json_find(mus, "volume"), 0.5);
            s_music_set = 1;
        }
    }

    // --- Cvar'ы карты: cvars = { "имя": число } --------------------------------
    s_cvar_count = 0;
    const JsonNode *cv = json_find(root, "cvars");
    if (cv) {
        for (const JsonNode *c = json_child_first(cv);
             c && s_cvar_count < MAP_MAX_CVARS; c = json_child_next(cv, c)) {
            if (!c->key || !c->key[0] || c->type != JSON_NUMBER) continue;
            snprintf(s_cvar_names[s_cvar_count], 64, "%s", c->key);
            s_cvar_values[s_cvar_count] = (float)c->nval;
            s_cvar_count++;
        }
    }

    // --- Команды карты: commands = [ "команда", ... ] --------------------------
    s_command_count = 0;
    const JsonNode *cmds = json_find(root, "commands");
    if (cmds) {
        int n = json_array_len(cmds);
        for (int i = 0; i < n && s_command_count < MAP_MAX_COMMANDS; i++) {
            const char *c = json_str(json_array_get(cmds, i), "");
            if (c[0])
                snprintf(s_commands[s_command_count++], MAP_CMD_LEN, "%s", c);
        }
    }

    // --- Источники света: lights[] -------------------------------------------
    s_light_count = 0;
    const JsonNode *lights = json_find(root, "lights");
    if (lights) {
        int n = json_array_len(lights);
        for (int i = 0; i < n; i++) parse_light(json_array_get(lights, i));
    }

    // --- Объекты (модели): objects[] — данные из JSON, рендер появится позже --
    s_object_count = 0;
    const JsonNode *objs = json_find(root, "objects");
    if (objs) {
        int n = json_array_len(objs);
        for (int i = 0; i < n; i++) parse_object(json_array_get(objs, i));
    }

    // --- Коллизии-квадраты: collisions[] (с привязкой к объектам) --------------
    s_collision_count = 0;
    const JsonNode *cols = json_find(root, "collisions");
    if (cols) {
        int n = json_array_len(cols);
        for (int i = 0; i < n; i++) parse_collision(json_array_get(cols, i));
    }

    // --- Позиционные звуки: sounds[] -------------------------------------------
    s_sound_count = 0;
    const JsonNode *snds = json_find(root, "sounds");
    if (snds) {
        int n = json_array_len(snds);
        for (int i = 0; i < n; i++) parse_sound(json_array_get(snds, i));
    }

    // --- Триггеры: triggers[] ---------------------------------------------------
    s_trigger_count = 0;
    const JsonNode *trs = json_find(root, "triggers");
    if (trs) {
        int n = json_array_len(trs);
        for (int i = 0; i < n; i++) parse_trigger(json_array_get(trs, i));
    }

    // --- Фауна: fauna[] ---------------------------------------------------------
    s_fauna_count = 0;
    const JsonNode *fa = json_find(root, "fauna");
    if (fa) {
        int n = json_array_len(fa);
        for (int i = 0; i < n; i++) parse_fauna(json_array_get(fa, i));
    }

    SDL_Log("Map data: %s, field %s (%.0f,%.0f %.0fx%.0f)%s, player %s, lights %d, objects %d, collisions %d, sounds %d, triggers %d, fauna %d%s%s",
            s_image,
            s_field_set ? "set" : "default",
            s_field_center[0], s_field_center[1],
            s_field_size[0], s_field_size[1],
            s_field_attached ? " ATTACHED" : "",
            s_player_set ? "set" : "default",
            s_light_count, s_object_count, s_collision_count,
            s_sound_count, s_trigger_count, s_fauna_count,
            s_info.name[0] ? " | " : "",
            s_info.name[0] ? s_info.name : "");
    if (s_command_count)
        SDL_Log("Map data: %d command(s) from map", s_command_count);
    if (s_cvar_count)
        SDL_Log("Map data: %d cvar(s) from map", s_cvar_count);
    return 0;
}

const char *mapdata_image(void)
{
    return s_image[0] ? s_image : "map.png";
}

int mapdata_field(float center[2], float size[2])
{
    if (!s_field_set) return 0;
    center[0] = s_field_center[0];
    center[1] = s_field_center[1];
    size[0] = s_field_size[0];
    size[1] = s_field_size[1];
    return 1;
}

int mapdata_field_attached(void)
{
    return s_field_attached;
}

int mapdata_player(float pos[3], float *yaw, float *pitch, float *fov)
{
    if (!s_player_set) return 0;
    pos[0] = s_player_pos[0];
    pos[1] = s_player_pos[1];
    pos[2] = s_player_pos[2];
    *yaw = s_player_yaw;
    *pitch = s_player_pitch;
    if (fov) *fov = s_player_fov;
    return 1;
}

int mapdata_light_count(void)
{
    return s_light_count;
}

const MapLight *mapdata_light(int i)
{
    return (i >= 0 && i < s_light_count) ? &s_lights[i] : NULL;
}

void mapdata_fog(MapFog *f)
{
    if (f) *f = s_fog;
}

void mapdata_terrain(MapTerrain *t) { if (t) *t = s_terrain; }
void mapdata_veg(MapVeg *v)         { if (v) *v = s_veg; }
void mapdata_sky(MapSky *s)         { if (s) *s = s_sky; }
void mapdata_bloom(MapBloom *b)     { if (b) *b = s_bloom; }
void mapdata_shadows(MapShadows *s) { if (s) *s = s_shadows; }

int mapdata_object_count(void)
{
    return s_object_count;
}

const MapObject *mapdata_object(int i)
{
    return (i >= 0 && i < s_object_count) ? &s_objects[i] : NULL;
}

int mapdata_collision_count(void)
{
    return s_collision_count;
}

const MapCollision *mapdata_collision(int i)
{
    return (i >= 0 && i < s_collision_count) ? &s_collisions[i] : NULL;
}

int mapdata_sound_count(void)
{
    return s_sound_count;
}

const MapSound3D *mapdata_sound(int i)
{
    return (i >= 0 && i < s_sound_count) ? &s_sounds[i] : NULL;
}

int mapdata_trigger_count(void)
{
    return s_trigger_count;
}

const MapTrigger *mapdata_trigger(int i)
{
    return (i >= 0 && i < s_trigger_count) ? &s_triggers[i] : NULL;
}

int mapdata_fauna_count(void)
{
    return s_fauna_count;
}

const MapFauna *mapdata_fauna(int i)
{
    return (i >= 0 && i < s_fauna_count) ? &s_fauna[i] : NULL;
}

void mapdata_info(MapInfo *i)
{
    if (i) *i = s_info;
}

int mapdata_ambient(MapSound *a)
{
    if (!s_ambient_set) return 0;
    if (a) *a = s_ambient;
    return 1;
}

int mapdata_music(MapSound *m)
{
    if (!s_music_set) return 0;
    if (m) *m = s_music;
    return 1;
}

int mapdata_command_count(void)
{
    return s_command_count;
}

const char *mapdata_command(int i)
{
    return (i >= 0 && i < s_command_count) ? s_commands[i] : NULL;
}

int mapdata_cvar_count(void)
{
    return s_cvar_count;
}

const char *mapdata_cvar_name(int i)
{
    return (i >= 0 && i < s_cvar_count) ? s_cvar_names[i] : NULL;
}

float mapdata_cvar_value(int i)
{
    return (i >= 0 && i < s_cvar_count) ? s_cvar_values[i] : NAN;
}

void mapdata_free(void)
{
    json_free();
    s_light_count = 0;
    s_object_count = 0;
    s_collision_count = 0;
    s_field_set = 0;
    s_field_attached = 0;
    s_player_set = 0;
    s_image[0] = '\0';
    s_ambient_set = 0;
    s_music_set = 0;
    s_command_count = 0;
    s_cvar_count = 0;
    s_sound_count = 0;
    s_trigger_count = 0;
    s_fauna_count = 0;
}
