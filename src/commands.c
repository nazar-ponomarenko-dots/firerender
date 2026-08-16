// ============================================================================
//  commands.c — консольные команды-действия (см. commands.h)
// ----------------------------------------------------------------------------
//  Вынесено из main.c (строки 215–660): тумблеры HUD/карты/теней/тумана,
//  демо, пауза, выбор карты, save/load/setpos, сущности, скриншот, звук.
//  Регистрация всех статических команд — в ЕДИНОМ реестре (cmdreg.c).
// ============================================================================
#include "commands.h"
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "audio.h"
#include "camera.h"
#include "console.h"
#include "cvars.h"
#include "fx.h"
#include "entity.h"
#include "events.h"
#include "game.h"
#include "gl_loader.h"
#include "glb.h"
#include "glb_anim.h"
#include "mapdata.h"
#include "material.h"
#include "mesh.h"
#include "png.h"
#include "renderer.h"
#include "selection.h"
#include "shader.h"
#include "world.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifdef _WIN32
#  include <direct.h>
#endif

// --- Тумблеры и режимы ----------------------------------------------------------
void cmd_noclip(const char *args)
{
    (void)args;
    Game *g = game_instance();
    if (cvar_get("sv_cheats") < 0.5f) {
        console_print("noclip: требуется sv_cheats 1");
        return;
    }
    if (!g->cam) return;
    g->cam->cam.fly = !g->cam->cam.fly;
    if (!g->cam->cam.fly) {
        g->cam->cam.vy = 0;
        g->cam->cam.vx = g->cam->cam.vz = 0;
        g->cam->cam.pos[1] = sample_height(&g->world, g->cam->cam.pos[0],
                                           g->cam->cam.pos[2]) + EYE_H;
    }
    SDL_Log("Mode: %s", g->cam->cam.fly ? "noclip" : "walk");
}

void cmd_toggle_map(const char *args)
{
    (void)args;
    Game *g = game_instance();
    if (!g->mm) return;
    g->mm->visible = !g->mm->visible;
    SDL_Log("Map: %s", g->mm->visible ? "ON" : "OFF");
}

void cmd_toggle_hud(const char *args)
{
    (void)args;
    Game *g = game_instance();
    if (!g->hud) return;
    g->hud->visible = !g->hud->visible;
    selection_set_on(g->hud->visible);
    SDL_Log("Debug: %s", g->hud->visible ? "ON" : "OFF");
}

void cmd_toggle_shadows(const char *args)
{
    (void)args;
    // быстрый тумблер: динамические <-> выкл
    float m = cvar_get("r_shadows_mode");
    cvar_set("r_shadows_mode", m == 1.0f ? 0.0f : 1.0f);
    SDL_Log("Shadows: %s",
            cvar_get("r_shadows_mode") == 1.0f ? "CSM" : "OFF");
}

void cmd_toggle_fog(const char *args)
{
    (void)args;
    // быстрый тумблер: объёмный <-> выкл (простой режим — только через меню)
    float m = cvar_get("r_fog_mode");
    cvar_set("r_fog_mode", m == 1.0f ? 0.0f : 1.0f);
    SDL_Log("Fog mode: %d", (int)cvar_get("r_fog_mode"));
}

void cmd_demo(const char *args)
{
    (void)args;
    game_demo_toggle(game_instance());
}

void cmd_pause(const char *args)
{
    (void)args;
    app_escape();   // игра <-> пауза, в меню — назад по экранам
}

// --- Карты ----------------------------------------------------------------------
// Выбор карты: map <имя> — "map" (папка) или имя из maps/*.zip.
// Если мир загружен — перезапускает игру на новой карте прямо сейчас.
static int s_map_switching = 0;   // рекурсия: скрипт/команда грузит карту внутри game_load_world
void cmd_map_select(const char *args)
{
    Game *g = game_instance();
    if (s_map_switching) {
        console_print("map: вложенный вызов игнорируется (карта уже грузится)");
        return;
    }
    char name[64];
    if (sscanf(args, "%63s", name) != 1 || !name[0]) {
        console_print("current map: %s", g->map_name);
        console_print("usage: map <name>  (\"map\" — папка map/, иначе maps/<name>.zip)");
        console_print("maps — список карт");
        return;
    }
    if (g->world_loaded && strcmp(name, g->map_name) == 0) {
        console_print("map \"%s\" уже загружена", name);
        return;
    }
    snprintf(g->map_name, sizeof g->map_name, "%s", name);
    if (g->world_loaded) {
        // Перезагрузка мира на новой карте: выгрузить -> загрузить -> в игру.
        // Флаг рекурсии: game_load_world -> map_apply/скрипт карты может сам
        // выполнить "map X" — вложенный cmd_map_select мы отбрасываем, иначе
        // мир выгрузится внутри собственной загрузки (UAF/double free сцены).
        s_map_switching = 1;
        game_unload_world(g);
        int rc = game_load_world(g);
        s_map_switching = 0;
        if (rc != 0) {
            SDL_Log("Map %s load failed", g->map_name);
            snprintf(g->map_name, sizeof g->map_name, "map1");
            app_set_state(APP_MENU);
            return;
        }
        game_demo_stop(g);
        bool was_load = g->player.pending_spawn != 0;
        if (g->player.pending_spawn) { // load: телепорт в сохранённую точку
            game_apply_spawn(g, g->player.save_pos, g->player.save_yaw,
                             g->player.save_pitch);
            g->player.pending_spawn = 0;
        }
        if (was_load) commands_apply_saved_world_state(); // восстановить мир при load
        app_set_state(APP_PLAY);
        SDL_Log("Map loaded: %s", g->map_name);
    } else {
        SDL_Log("Map selected: %s (загрузится при НОВОЙ ИГРЕ)", g->map_name);
    }
}

// Список доступных карт (команда maps)
void cmd_maps_list(const char *args)
{
    (void)args;
    console_print("maps:");
    app_maps_print();
}

// HL-стиль: команды, адаптированные под наши системы
void cmd_restart(const char *args)
{
    (void)args;
    cmd_map_select(game_instance()->map_name);   // перезапустить текущую карту
}

void cmd_changelevel(const char *args)
{
    cmd_map_select(args);   // смена карты (у нас без сохранения состояния)
}

void cmd_r_drawhud(const char *args)
{
    Game *g = game_instance();
    float v;
    if (sscanf(args, "%f", &v) == 1) {
        if (!g->hud) return;
        g->hud->visible = v > 0.5f;
        selection_set_on(g->hud->visible);
        SDL_Log("HUD: %s", g->hud->visible ? "ON" : "OFF");
        return;
    }
    console_print("r_drawhud = %d", g->hud && g->hud->visible);
}

void cmd_mat_wireframe(const char *args)
{
    float v;
    if (sscanf(args, "%f", &v) == 1)
        cvar_set("r_wireframe", v > 0.5f ? 1.0f : 0.0f);
    console_print("mat_wireframe = %d", cvar_get("r_wireframe") > 0.5f);
}

void cmd_thirdperson(const char *args)
{
    (void)args;
    cvar_set("cam_thirdperson", 1.0f);
    console_print("camera: thirdperson");
}

void cmd_firstperson(const char *args)
{
    (void)args;
    cvar_set("cam_thirdperson", 0.0f);
    console_print("camera: firstperson");
}

// --- Сущности -------------------------------------------------------------------
// ent_create <класс> [x y z] — спавн сущности (классы из реестра)
void cmd_ent_create(const char *args)
{
    Game *g = game_instance();
    if (cvar_get("sv_cheats") < 0.5f) {
        console_print("ent_create: требуется sv_cheats 1");
        return;
    }
    char cls[64];
    float x = 0, y = 0, z = 0;
    int n = sscanf(args, "%63s %f %f %f", cls, &x, &y, &z);
    if (n < 1) {
        console_print("usage: ent_create <класс> [x y z]");
        return;
    }
    if (!g->cam) {
        console_print("ent_create: мир не загружен");
        return;
    }
    float pos[3] = { x, y, z };
    if (n == 1) {   // перед камерой, на высоте взгляда
        float fwd[3];
        camera_dirs(&g->cam->cam, fwd, NULL);
        pos[0] = g->cam->cam.pos[0] + fwd[0] * 5.0f;
        pos[1] = g->cam->cam.pos[1];
        pos[2] = g->cam->cam.pos[2] + fwd[2] * 5.0f;
    }
    Entity *e = entity_spawn(cls, pos, NULL, NULL);
    if (e)
        console_print("entity \"%s\" спавнена в (%.0f, %.0f, %.0f)",
                      cls, pos[0], pos[1], pos[2]);
}

// ent_list — список сущностей
void cmd_ent_list(const char *args)
{
    (void)args;
    console_print("entities: %d", entity_count());
    for (int i = 0; i < entity_count(); i++) {
        Entity *e = entity_at(i);
        console_print("  %-12s (%.0f, %.0f, %.0f)", e->classname,
                      e->node->pos[0], e->node->pos[1], e->node->pos[2]);
    }
}

// --- Скриншот: сохранить кадр в PNG в папке screenshots/ ------------------------
static int g_shot_req = 0;

void cmd_screenshot(const char *args)
{
    (void)args;
    g_shot_req = 1;
    console_print("screenshot: кадр будет сохранён в screenshots/");
}

// Создаёт папку (утилита, кросс-платформенно)
static void make_dir(const char *path)
{
#ifdef _WIN32
    _mkdir(path);
#else
    mkdir(path, 0755);
#endif
}

// Читает кадр и пишет PNG (перевёрнутый по Y, как смотрит игрок)
static void take_shot_png(const char *path, int vw, int vh)
{
    unsigned char *px = malloc((size_t)vw * vh * 3);
    if (!px) return;
    glReadPixels(0, 0, vw, vh, GL_RGB, GL_UNSIGNED_BYTE, px);
    // Строки GL идут снизу вверх — переворачиваем для PNG
    size_t row = (size_t)vw * 3;
    unsigned char *flip = malloc(row * (size_t)vh);
    if (flip) {
        for (int y = 0; y < vh; y++)
            memcpy(flip + (size_t)y * row, px + (size_t)(vh - 1 - y) * row, row);
        if (png_save(path, vw, vh, flip) == 0)
            SDL_Log("screenshot: %s (%dx%d)", path, vw, vh);
        else
            SDL_Log("screenshot: не удалось записать %s", path);
        free(flip);
    }
    free(px);
}

// Вызывается из главного цикла каждый кадр (команда screenshot / F12)
void commands_shot_take(int vw, int vh)
{
    if (!g_shot_req) return;
    g_shot_req = 0;
    char path[320];
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    if (!tm) {   // localtime может вернуть NULL — не пишем в tm->*
        SDL_Log("screenshot: localtime failed");
        return;
    }
    snprintf(path, sizeof path, "screenshots/shot_%04d%02d%02d_%02d%02d%02d.png",
             tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
             tm->tm_hour, tm->tm_min, tm->tm_sec);
    make_dir("screenshots");
    take_shot_png(path, vw, vh);
}

// --- Звук ------------------------------------------------------------------------
// Проверка звука: проигрывает все движковые звуки подряд
void cmd_snd_test(const char *args)
{
    (void)args;
    const char *paths[] = {
        "audio/click.wav", "audio/step1.wav", "audio/step2.wav",
        "audio/step3.wav", "audio/jump.wav", "audio/land.wav",
        "audio/wind.wav", "audio/blip.wav",
    };
    for (size_t i = 0; i < sizeof paths / sizeof paths[0]; i++) {
        Sound *s = audio_load(paths[i]);
        if (s) {
            audio_play(s, 0.45f, 1.0f);
            console_print("snd_test: %s", paths[i]);
        } else {
            console_print("snd_test: НЕТ звука %s", paths[i]);
        }
        SDL_Delay(200);
    }
}

// --- Hot-reload шейдеров ----------------------------------------------------------
void cmd_reload_shaders(const char *args)
{
    (void)args;
    int n = shader_hot_reload();
    n += resources_texture_reload();
    console_print("reload_shaders: %d", n);
}

// --- Save/Load (F5/F9): позиция игрока + карта + состояние мира -------------
// Помимо позиции сохраняет, какие триггеры уже сработали и какие use-объекты
// использованы, чтобы при загрузке мир «продолжался», а не сбрасывался.
#define SAVE_MAX_TRIGGER 64
#define SAVE_MAX_OBJUSED 128
static int s_save_triggers[SAVE_MAX_TRIGGER];    // trigger: 0/1
static int s_save_objects_used_n = 0;            // объекты: имя уже использован
static char s_save_objects_used[SAVE_MAX_OBJUSED][64];

void cmd_save(const char *args)
{
    (void)args;
    Game *g = game_instance();
    if (!g->cam) { console_print("save: мир не загружен"); return; }
    player_save(&g->player, &g->cam->cam, g->map_name);
    FILE *f = fopen("save.cfg", "w");
    if (!f) { console_print("save: не могу записать save.cfg"); return; }
    fprintf(f, "map_name %s\n", g->player.save_map);
    fprintf(f, "setpos %.2f %.2f %.2f %.4f %.4f\n",
            g->player.save_pos[0], g->player.save_pos[1], g->player.save_pos[2],
            g->player.save_yaw, g->player.save_pitch);
    // Состояние мира: сработавшие триггеры + использованные use-объекты
    int tn = mapdata_trigger_count();
    if (tn > SAVE_MAX_TRIGGER) tn = SAVE_MAX_TRIGGER;
    for (int i = 0; i < tn; i++)
        fprintf(f, "trigger_fired %d %d\n", i, mapdata_trigger_fired(i));
    for (int i = 0; i < mapdata_object_count(); i++) {
        const MapObject *o = mapdata_object(i);
        if (o && o->use.set && o->use.used)
            fprintf(f, "object_used %s\n", o->name);
    }
    fclose(f);
    console_print("save: %s (%.0f,%.0f,%.0f)", g->player.save_map,
                  g->player.save_pos[0], g->player.save_pos[1],
                  g->player.save_pos[2]);
}

// Применяет сохранённое состояние мира к текущей карте (если совпадает).
void commands_apply_saved_world_state(void)
{
    Game *g = game_instance();
    if (!g) return;
    if (!g->world_loaded || !g->map_name[0]) return;
    // только если сохранение относится к текущей карте
    if (strcmp(g->player.save_map, g->map_name) != 0) return;
    for (int i = 0; i < SAVE_MAX_TRIGGER; i++)
        mapdata_trigger_set_fired(i, s_save_triggers[i]);
    for (int i = 0; i < s_save_objects_used_n; i++)
        mapdata_object_set_used(s_save_objects_used[i], 1);
}

// --- Эффекты на точке попадения взгляда (raycast) -------------------------------
// m — искры (add), comma — дым (alpha), period — кровь (bounce)
static void cmd_fx_raycast(const char *args, const char *preset, int count)
{
    (void)args;
    Game *g = game_instance();
    if (!g || !g->cam || !g->world_loaded) {
        console_print("fx: мир не загружен");
        return;
    }
    float x, y, z;
    if (!camera_raycast_terrain(&g->cam->cam, &g->world, 50.0f, &x, &y, &z)) {
        console_print("fx: луч не попал в рельеф");
        return;
    }
    fx_burst(preset, x, y, z, count);
}

void cmd_fx_sparks(const char *args)  { cmd_fx_raycast(args, "sparks", 30); }
void cmd_fx_smoke(const char *args)   { cmd_fx_raycast(args, "smoke", 16); }
void cmd_fx_blood(const char *args)   { cmd_fx_raycast(args, "blood", 24); }

// --- Save/Load (F5/F9) -----------------------------------------------------------
void cmd_load(const char *args)
{
    (void)args;
    Game *g = game_instance();
    FILE *f = fopen("save.cfg", "r");
    if (!f) { console_print("load: нет save.cfg (F5 — сохранить)"); return; }
    char line[256], map[64] = "";
    float x = 0, y = 0, z = 0, ya = 0, pi = 0;
    // Сброс буфера состояния мира
    for (int i = 0; i < SAVE_MAX_TRIGGER; i++) s_save_triggers[i] = 0;
    s_save_objects_used_n = 0;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "map_name %63s", map) == 1) continue;
        if (sscanf(line, "setpos %f %f %f %f %f", &x, &y, &z, &ya, &pi) >= 3)
            continue;
        int ti = -1, tv = 0;
        if (sscanf(line, "trigger_fired %d %d", &ti, &tv) == 2) {
            if (ti >= 0 && ti < SAVE_MAX_TRIGGER) s_save_triggers[ti] = tv;
            continue;
        }
        char name[64] = "";
        if (sscanf(line, "object_used %63s", name) == 1) {
            if (name[0] && s_save_objects_used_n < SAVE_MAX_OBJUSED) {
                snprintf(s_save_objects_used[s_save_objects_used_n], 64, "%s", name);
                s_save_objects_used_n++;
            }
            continue;
        }
    }
    fclose(f);
    if (!map[0]) { console_print("load: save.cfg повреждён"); return; }
    player_load(&g->player, x, y, z, ya, pi, map);
    if (g->world_loaded && strcmp(g->player.save_map, g->map_name) == 0) {
        game_apply_spawn(g, g->player.save_pos, g->player.save_yaw,
                         g->player.save_pitch);
        commands_apply_saved_world_state();   // восстановить триггеры/use
        console_print("load: загружено (%.0f,%.0f,%.0f)", x, y, z);
        return;
    }
    // Другая карта (или мир ещё не загружен): загрузить и телепортироваться.
    // Состояние применится автоматически в game_load_world.
    g->player.pending_spawn = 1;
    if (g->world_loaded) {
        cmd_map_select(g->player.save_map);
    } else {
        snprintf(g->map_name, sizeof g->map_name, "%s", g->player.save_map);
        SDL_Log("Map selected: %s (из save)", g->map_name);
    }
    console_print("load: карта %s, телепорт при загрузке", g->player.save_map);
}

// Телепорт игрока: setpos x y z [yaw] [pitch]
void cmd_setpos(const char *args)
{
    Game *g = game_instance();
    float x, y, z, ya = 0, pi = 0;
    int n = sscanf(args, "%f %f %f %f %f", &x, &y, &z, &ya, &pi);
    if (n < 3) {
        console_print("usage: setpos x y z [yaw] [pitch]");
        return;
    }
    if (!g->cam) { console_print("setpos: мир не загружен"); return; }
    if (n < 4) ya = g->cam->cam.yaw;
    if (n < 5) pi = g->cam->cam.pitch;
    float p[3] = { x, y, z };
    game_apply_spawn(g, p, ya, pi);
}

// Поставить статичный меш (.obj): mesh <путь> [x y z] [scale] [tex]
void cmd_mesh(const char *args)
{
    Game *g = game_instance();
    if (!g || !g->root) { console_print("mesh: мир не загружен"); return; }
    char path[256] = "", tex[256] = "";
    float x = 0, y = 0, z = 0, scale = 1.0f;
    int n = sscanf(args, "%255s %f %f %f %f %255s", path, &x, &y, &z, &scale, tex);
    g = game_instance();
    if (n < 1) { console_print("usage: mesh <obj> [x y z] [scale] [tex]"); return; }
    if (!g || !g->root) return;
    float pos[3] = { x, y, z };
    SceneNode *nd = mesh_make_node(path, pos, scale,
                                   (n >= 6 && tex[0]) ? tex : NULL,
                                   renderer_context());
    if (!nd) { console_print("mesh: не удалось загрузить %s", path); return; }
    node_add(g->root, nd);
    console_print("mesh: %s в (%.0f,%.0f,%.0f)", path, x, y, z);
}

// Поставить бинарный glTF (.glb): glb <путь> [x y z] [scale]
static SceneNode *s_last_anim;   // последний поставленный AnimInstance3D

// Узел-анимик, на который указывает s_last_anim, может быть убит/удалён
// (entity_kill, выгрузка мира) — указатель станет висячим. Чистим его по
// EV_NODE_EXIT, пока node_free_tree ещё не освободил сам узел.
static void anim_last_exit(const Event *e, void *user)
{
    (void)user;
    if (e && e->a == (const void *)s_last_anim) s_last_anim = NULL;
}

static void anim_last_hook(void)
{
    static int s_hooked = 0;
    if (!s_hooked) { events_listen(EV_NODE_EXIT, anim_last_exit, NULL); s_hooked = 1; }
}

// Узел-анимик: по имени (если задано) или последний поставленный.
static SceneNode *anim_find(const char *name)
{
    Game *g = game_instance();
    SceneNode *nd = NULL;
    if (name && name[0] && g && g->root) nd = node_find(g->root, name);
    if (!nd) nd = s_last_anim;
    return nd;
}

void cmd_glb(const char *args)
{
    Game *g = game_instance();
    if (!g || !g->root) { console_print("glb: мир не загружен"); return; }
    char path[256] = "";
    float x = 0, y = 0, z = 0, scale = 1.0f;
    int n = sscanf(args, "%255s %f %f %f %f", path, &x, &y, &z, &scale);
    if (n < 1) { console_print("usage: glb <glb> [x y z] [scale]"); return; }
    float pos[3] = { x, y, z };
    SceneNode *nd = NULL;
    // авто-выбор: есть скин+анимация -> скелетный путь (skinning)
    if (glb_anim_has_anim(path))
        nd = glb_anim_make_node(path, pos, scale, renderer_context());
    else
        nd = glb_make_node(path, pos, scale, renderer_context());
    if (!nd) { console_print("glb: не удалось загрузить %s", path); return; }
    anim_last_hook();
    node_add(g->root, nd);
    if (glb_anim_clip_count(nd) > 0) s_last_anim = nd;
    console_print("glb: %s в (%.0f,%.0f,%.0f)", path, x, y, z);
}

// Поставить анимированный glTF (.glb): glbanim <путь> [x y z] [scale]
void cmd_glbanim(const char *args)
{
    Game *g = game_instance();
    if (!g || !g->root) { console_print("glbanim: мир не загружен"); return; }
    char path[256] = "";
    float x = 0, y = 0, z = 0, scale = 1.0f;
    int n = sscanf(args, "%255s %f %f %f %f", path, &x, &y, &z, &scale);
    if (n < 1) { console_print("usage: glbanim <glb> [x y z] [scale]"); return; }
    float pos[3] = { x, y, z };
    SceneNode *nd = glb_anim_make_node(path, pos, scale, renderer_context());
    if (!nd) { console_print("glbanim: не удалось загрузить %s", path); return; }
    anim_last_hook();
    node_add(g->root, nd);
    s_last_anim = nd;
    console_print("glbanim: %s в (%.0f,%.0f,%.0f)", path, x, y, z);
    if (glb_anim_clip_count(nd) > 0) {
        console_print("  клипы: %d, активный: %s",
                      glb_anim_clip_count(nd),
                      glb_anim_current_clip(nd) ? glb_anim_current_clip(nd) : "?");
        for (int i = 0; i < glb_anim_clip_count(nd); i++)
            console_print("  [%d] %s", i, glb_anim_clip_name(nd, i) ? glb_anim_clip_name(nd, i) : "?");
    }
}

// Список клипов анимации узла: glbclips [имя-узла]
void cmd_glbclips(const char *args)
{
    char name[128] = "";
    sscanf(args, "%127s", name);
    SceneNode *nd = anim_find(name);
    if (!nd) { console_print("glbclips: нет анимик-узла (спавн через glbanim/glb)"); return; }
    int nc = glb_anim_clip_count(nd);
    console_print("glbclips: узел %s, клипов %d, активный: %s",
                  nd->name ? nd->name : "?", nc,
                  glb_anim_current_clip(nd) ? glb_anim_current_clip(nd) : "?");
    for (int i = 0; i < nc; i++)
        console_print("  [%d] %s", i, glb_anim_clip_name(nd, i) ? glb_anim_clip_name(nd, i) : "?");
}

// Переключить клип: glbclip <индекс|имя> [имя-узла]
void cmd_glbclip(const char *args)
{
    char sel[64] = "", name[128] = "";
    int n = sscanf(args, "%63s %127s", sel, name);
    if (n < 1) { console_print("usage: glbclip <idx|имя> [имя-узла]"); return; }
    SceneNode *nd = anim_find(name);
    if (!nd) { console_print("glbclip: нет анимик-узла (спавн через glbanim/glb)"); return; }
    int idx = atoi(sel);
    if (!(sel[0] >= '0' && sel[0] <= '9') || idx > 99999) idx = glb_anim_clip_find(nd, sel);
    if (idx < 0) { console_print("glbclip: клип '%s' не найден", sel); return; }
    glb_anim_set_clip(nd, idx);
    console_print("glbclip: %s -> [%d] %s", nd->name ? nd->name : "?", idx,
                  glb_anim_clip_name(nd, idx) ? glb_anim_clip_name(nd, idx) : "?");
}

// Пауза/продолжить: glbanim_pause [имя-узла]
void cmd_glbanim_pause(const char *args)
{
    char name[128] = "";
    sscanf(args, "%127s", name);
    SceneNode *nd = anim_find(name);
    if (!nd) { console_print("glbanim_pause: нет анимик-узла"); return; }
    glb_anim_set_paused(nd, 1);
    console_print("glbanim_pause: %s", nd->name ? nd->name : "?");
}
// Продолжить: glbanim_play [имя-узла]
void cmd_glbanim_play(const char *args)
{
    char name[128] = "";
    sscanf(args, "%127s", name);
    SceneNode *nd = anim_find(name);
    if (!nd) { console_print("glbanim_play: нет анимик-узла"); return; }
    glb_anim_set_paused(nd, 0);
    console_print("glbanim_play: %s", nd->name ? nd->name : "?");
}
// Скорость времени: glbanim_speed <x> [имя-узла]
void cmd_glbanim_speed(const char *args)
{
    float s = 1.0f; char name[128] = "";
    if (sscanf(args, "%f %127s", &s, name) < 1) { console_print("usage: glbanim_speed <x> [имя-узла]"); return; }
    SceneNode *nd = anim_find(name);
    if (!nd) { console_print("glbanim_speed: нет анимик-узла"); return; }
    glb_anim_set_speed(nd, s);
    console_print("glbanim_speed: %s = %g", nd->name ? nd->name : "?", s);
}
// Зацикливание: glbanim_loop <0|1> [имя-узла]
void cmd_glbanim_loop(const char *args)
{
    int v = 1; char name[128] = "";
    if (sscanf(args, "%d %127s", &v, name) < 1) { console_print("usage: glbanim_loop <0|1> [имя-узла]"); return; }
    SceneNode *nd = anim_find(name);
    if (!nd) { console_print("glbanim_loop: нет анимик-узла"); return; }
    glb_anim_set_loop(nd, v);
    console_print("glbanim_loop: %s = %d", nd->name ? nd->name : "?", v?1:0);
}

// cubemap_capture [x y z] [size] — захватить кубомап отражений в позиции
// игрока (или в точке x y z) и сохранить 6 PNG в cubemaps/. По умолчанию
// размер 256; захваченный кубомап тут же используется водой.
void cmd_cubemap_capture(const char *args)
{
    Game *g = game_instance();
    if (!g || !g->cam || !g->world_loaded) {
        console_print("cubemap_capture: мир не загружен");
        return;
    }
    if (!renderer_refl_cubemap()) {
        console_print("cubemap_capture: кубомап недоступен");
        return;
    }
    // Позиция: игрок по умолчанию, либо из аргументов "x y z [size]"
    float pos[3] = {
        g->cam->cam.pos[0], g->cam->cam.pos[1], g->cam->cam.pos[2]
    };
    int size = 256;
    if (args && args[0]) {
        float x, y, z;
        int n = sscanf(args, "%f %f %f %d", &x, &y, &z, &size);
        if (n == 1) size = (int)x;       // "cubemap_capture 128" — только размер
        else if (n >= 3) { pos[0] = x; pos[1] = y; pos[2] = z; }
    }
    renderer_cubemap_capture_at(g, pos, size);
    console_print("cubemap_capture: (%.0f, %.0f, %.0f) size=%d сохранён в cubemaps/",
                  pos[0], pos[1], pos[2], size);
}

// Список зарегистрированных материалов по имени (matlist).
void cmd_matlist(const char *args)
{
    (void)args;
    if (material_count() == 0) {
        SDL_Log("matlist: материалов не зарегистрировано");
        return;
    }
    for (int i = 0; i < material_count(); i++) {
        const Material *m = material_find(material_name(i));
        SDL_Log("material %-24s tint=(%.2f,%.2f,%.2f) alpha=%.2f blend=%d tile=(%.2f,%.2f)",
                material_name(i),
                m->tint[0], m->tint[1], m->tint[2], m->alpha, m->blend,
                m->tile[0], m->tile[1]);
    }
}
