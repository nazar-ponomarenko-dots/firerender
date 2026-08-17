// ============================================================================
//  game.c — игровое состояние, загрузка/выгрузка мира и обновление (см. game.h)
// ----------------------------------------------------------------------------
//  Всё состояние мира — в одной структуре Game (узлы сцены, камера/HUD,
//  карта, спавн/сохранение, демо, данные мира WorldState). Модули получают
//  Game* параметром; game_instance() — для функций без параметра
//  (консольные команды).
//
//  Загрузка мира разбита на ЯВНЫЕ ФАЗЫ (порядок зафиксирован):
//     1. world_load_geometry — пак карты, map.json, высоты/нормали, поле;
//     2. scene_build          — тени, дерево узлов, сущности, FBO, первый кадр;
//     3. map_apply            — cvar'ы и команды карты;
//     4. audio_load_map       — музыка, ambient/ветер, позиционные звуки.
//  Выгрузка — СТРОГИЙ ИНВЕРС: audio_stop_map -> scene_teardown ->
//  world_unload_geometry (каждый шаг освобождает то, что создан последним).
// ============================================================================
#include "game.h"

#include "actions.h"
#include "app.h"
#include "audio.h"
#include "console.h"
#include "cvars.h"
#include "decals.h"
#include "entity.h"
#include "fauna.h"
#include "field.h"
#include "fx.h"
#include "lua_engine.h"
#include "loading.h"
#include "mapdata.h"
#include "material.h"
#include "objects.h"
#include "particles.h"
#include "playerbox.h"
#include "renderer.h"
#include "resources.h"
#include "selection.h"
#include "shadow.h"
#include "sky.h"
#include "sun.h"
#include "terrain.h"
#include "vegetation.h"
#include "voxfog.h"
#include "water.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Единственный экземпляр: main.c создаёт Game на стеке и регистрирует
// через game_init; game_instance() отдаёт его командам и прочим.
static Game *s_game = NULL;

// Функции фаз определены ниже в этом же файле; forward-объявления для
// вызовов из scene_build (частичный rollback) и game_load_world.
static void scene_teardown(Game *g);
static void world_unload_geometry(Game *g);

void game_init(Game *g)
{
    s_game = g;
    memset(g, 0, sizeof *g);
    snprintf(g->map_name, sizeof g->map_name, "%s", "map1");
    player_init(&g->player);
}

Game *game_instance(void)
{
    return s_game;
}

static int map_is_zip(const Game *g)
{
    return strcmp(g->map_name, "map") != 0;
}

// --- Демо-сцена (клавиша Q): автополёт с замером FPS -------------------------
#define DEMO_DUR 116.0f

// Дерево для круга (tree1 из map.json) и эллипс вокруг поля
static const float DEMO_TREE[2] = { 880.0f, 380.0f };
#define DEMO_FCX 1000.0f
#define DEMO_FCZ 562.0f
#define DEMO_A 280.0f     // полуоси эллипса вокруг поля (за его краем)
#define DEMO_B 190.0f

#define DEMO_H(w, x, z) sample_height(w, (x), (z))

// Шаг демо: время t от начала, ведём камеру по фазам на уровне игрока
static void demo_step(const WorldState *w, float t, CameraParams *cam)
{
    float hx = 0, hz = 0;                    // позиция
    float gx, gy, gz;                        // точка взгляда (на высоте земли)

    if (t < 8.0f) {
        hx = 1000.0f;
        hz = 408.0f + 2.5f * t;
        gx = 1000.0f; gz = hz + 30.0f;
        gy = DEMO_H(w, gx, gz);
    } else if (t < 12.0f) {
        float k = (t - 8.0f) / 4.0f;
        k = k * k * (3.0f - 2.0f * k);
        float d = 20.0f + (2.5f + (12.0f - 2.5f) * k) * (t - 8.0f);
        hx = 1000.0f;
        hz = 408.0f + d;
        gx = 1000.0f; gz = hz + 40.0f;
        gy = DEMO_H(w, gx, gz);
    } else if (t < 20.0f) {
        float k = (t - 12.0f) / 8.0f;
        k = k * k * (3.0f - 2.0f * k);
        float d = 58.0f + (12.0f + (22.0f - 12.0f) * k) * (t - 12.0f);
        hx = 1000.0f;
        hz = 408.0f + d;
        gx = 1000.0f; gz = hz + 50.0f;
        gy = DEMO_H(w, gx, gz);
    } else if (t < 35.0f) {
        float k = (t - 20.0f) / 15.0f;
        float p0x = 1000.0f, p0z = 408.0f + 58.0f + 176.0f;
        float p2x = DEMO_TREE[0], p2z = DEMO_TREE[1];
        float c0x = 930.0f, c0z = 480.0f;
        float ik = 1.0f - k;
        hx = ik * ik * p0x + 2.0f * k * ik * c0x + k * k * p2x;
        hz = ik * ik * p0z + 2.0f * k * ik * c0z + k * k * p2z;
        gx = DEMO_TREE[0]; gz = DEMO_TREE[1];
        gy = DEMO_H(w, DEMO_TREE[0], DEMO_TREE[1]) + 6.0f;
    } else if (t < 55.0f) {
        float th = 1.5708f + 0.44f * (t - 35.0f);
        hx = DEMO_TREE[0] + cosf(th) * 12.0f;
        hz = DEMO_TREE[1] + sinf(th) * 12.0f;
        gx = DEMO_TREE[0]; gz = DEMO_TREE[1];
        gy = DEMO_H(w, DEMO_TREE[0], DEMO_TREE[1]) + 6.0f;
    } else if (t < 63.0f) {
        float k = (t - 55.0f) / 8.0f;
        if (k > 1.0f) k = 1.0f;
        k = k * k * (3.0f - 2.0f * k);
        float th = 1.5708f + 0.44f * 20.0f;
        float ox = DEMO_TREE[0] + cosf(th) * 12.0f;
        float oz = DEMO_TREE[1] + sinf(th) * 12.0f;
        float ea = atan2f(DEMO_TREE[0] - DEMO_FCX, DEMO_TREE[1] - DEMO_FCZ);
        float ex = DEMO_FCX + sinf(ea) * DEMO_A;
        float ez = DEMO_FCZ + cosf(ea) * DEMO_B;
        hx = ox + (ex - ox) * k;
        hz = oz + (ez - oz) * k;
        gx = DEMO_FCX; gz = DEMO_FCZ;
        gy = DEMO_H(w, gx, gz);
    } else if (t < 110.0f) {
        float k = (t - 63.0f) / 47.0f;
        float ea = atan2f(DEMO_TREE[0] - DEMO_FCX, DEMO_TREE[1] - DEMO_FCZ);
        float th = ea + 6.28318f * k;
        hx = DEMO_FCX + sinf(th) * DEMO_A;
        hz = DEMO_FCZ + cosf(th) * DEMO_B;
        gx = DEMO_FCX; gz = DEMO_FCZ;
        gy = DEMO_H(w, gx, gz);
    } else {
        float k = (t - 110.0f) / 6.0f;
        if (k > 1.0f) k = 1.0f;
        k = k * k * (3.0f - 2.0f * k);
        float ea = atan2f(DEMO_TREE[0] - DEMO_FCX, DEMO_TREE[1] - DEMO_FCZ);
        float ox = DEMO_FCX + sinf(ea + 6.28318f) * DEMO_A;
        float oz = DEMO_FCZ + cosf(ea + 6.28318f) * DEMO_B;
        hx = ox + (1000.0f - ox) * k;
        hz = oz + (562.0f - oz) * k;
        gx = 1000.0f; gz = 562.0f;
        gy = DEMO_H(w, gx, gz);
    }

    cam->cam.pos[0] = hx;
    cam->cam.pos[1] = DEMO_H(w, hx, hz) + EYE_H;
    cam->cam.pos[2] = hz;
    float dx = gx - hx, dy = gy - cam->cam.pos[1], dz = gz - hz;
    float hl = sqrtf(dx * dx + dz * dz);
    cam->cam.yaw = atan2f(dx, dz);
    cam->cam.pitch = atan2f(dy, hl);
}

// --- Телепорт игрока (спавн, setpos, load, телепорт-триггеры) ------------------
void game_apply_spawn(Game *g, const float pos[3], float yaw, float pitch)
{
    if (!g->cam) return;
    player_teleport(&g->player, &g->cam->cam, pos, yaw, pitch);
}

// --- Демо-сцена (клавиша Q) ----------------------------------------------------
void game_demo_toggle(Game *g)
{
    if (!g->world_loaded || app_state() != APP_PLAY || !g->cam || !g->hud) return;
    g->demo_on = !g->demo_on;
    camera_demo_set(g->demo_on);
    if (g->demo_on) {
        g->demo_t = 0.0f;
        g->demo_frames = 0;
        g->demo_elapsed = 0;
        g->demo_min_dt = 1e9;
        g->demo_max_dt = 0;
        hud_demo_start(g->hud);
        SDL_Log("DEMO: start (автополёт, замер FPS)");
    } else {
        hud_demo_stop(g->hud);
        SDL_Log("DEMO: stopped");
    }
}

void game_demo_stop(Game *g)
{
    if (!g->demo_on) return;
    g->demo_on = 0;
    camera_demo_set(0);
    if (g->hud) hud_demo_stop(g->hud);
}

// Шаг демо-сцены: ведём камеру по маршруту и меряем FPS.
void game_demo_tick(Game *g, float dt, float inst_fps)
{
    if (!g->demo_on || !g->world_loaded || app_state() != APP_PLAY) return;
    g->demo_t += dt;
    if (g->demo_t >= DEMO_DUR) {
        double avg = g->demo_elapsed > 0.0 ? g->demo_frames / g->demo_elapsed : 0.0;
        SDL_Log("DEMO: end | frames=%.0f time=%.1fs | avg=%.0f min=%.0f max=%.0f fps",
                g->demo_frames, g->demo_elapsed, avg,
                g->demo_max_dt > 0.0 ? 1.0 / g->demo_max_dt : 0.0,
                g->demo_min_dt < 1e9 ? 1.0 / g->demo_min_dt : 0.0);
        g->demo_on = 0;
        camera_demo_set(0);
        hud_demo_stop(g->hud);
    } else {
        demo_step(&g->world, g->demo_t, g->cam);
        g->demo_frames += 1;
        g->demo_elapsed += dt;
        if (dt > 0.0005f && dt < 0.1f) {
            if (dt < g->demo_min_dt) g->demo_min_dt = dt;
            if (dt > g->demo_max_dt) g->demo_max_dt = dt;
        }
        hud_demo_frame(g->hud, inst_fps, dt);
    }
}

// --- Интерактив и триггеры карты -----------------------------------------------
// Выполняет действие карты: команды, сообщение, звук (позиционно из pos),
// телепорт. ВАЖНО: console_exec может выгрузить мир (команда "map X") — после
// него НЕЛЬЗЯ трогать g->cam (он освобождён в scene_teardown). Поэтому позицию
// передаём копией, а доступ к камере — только при живом мире.
static void map_action_run(Game *g, const char commands[MAP_ACTIONS_MAX][MAP_CMD_LEN],
                           int cmd_n, const char *message, const char *sound,
                           const float pos[3], const float *tp, int has_tp)
{
    for (int i = 0; i < cmd_n; i++) {
        console_exec(commands[i]);
        if (!g->world_loaded) return;   // команда выгрузила мир — действие прервано
    }
    if (message && message[0]) {
        snprintf(g->ui_msg, sizeof g->ui_msg, "%s", message);
        g->ui_msg_t = 4.0f;
    }
    if (sound && sound[0] && pos) {
        Sound *s = audio_load(sound);
        if (s)
            audio_3d_play_once(s, 0.4f * cvar_get("snd_dialog"), pos, 30.0f);
    }
    if (has_tp && tp && g->cam)
        game_apply_spawn(g, tp, g->cam->cam.yaw, g->cam->cam.pitch);
}

// Проверка триггеров и взаимодействия (E) — каждый кадр в игре
static void map_actions_update(Game *g)
{
    static int s_use_prev = 0;
    const int use_now = g_act[A_USE];
    const int use_press = use_now && !s_use_prev;
    s_use_prev = use_now;

    if (!g->cam) return;
    // Позиция — КОПИЕЙ: консольная команда триггера может выгрузить мир
    // (команда "map ..."), и указатель на камеру повиснет (use-after-free).
    float px = g->cam->cam.pos[0];
    float pz = g->cam->cam.pos[2];

    // --- Триггеры: вошёл в область -> действие ---
    for (int i = 0; i < mapdata_trigger_count(); i++) {
        const MapTrigger *t = mapdata_trigger(i);
        if (!t) continue;
        int inside = fabsf(px - t->pos[0]) <= t->size[0] * 0.5f &&
                     fabsf(pz - t->pos[2]) <= t->size[1] * 0.5f;
        int edge = inside && !t->entered;
        MapTrigger *mt = (MapTrigger *)t;
        mt->entered = inside;
        if (!edge) continue;
        if (t->once && t->fired) continue;
        if (t->once) mt->fired = 1;
        SDL_Log("Trigger: %s", t->name[0] ? t->name : "(без имени)");
        map_action_run(g, t->commands, t->cmd_n, t->message, t->sound,
                       t->pos, t->teleport, t->has_tp);
        if (!g->world_loaded) return;   // мир сменился — триггеры новой карты
                                        // обработает следующий кадр
    }

    // --- Взаимодействие E: ближайший объект с use в радиусе 4.5 м ---
    if (!use_press) return;
    const MapObject *best = NULL;
    float best_d = 4.5f * 4.5f;
    for (int i = 0; i < mapdata_object_count(); i++) {
        const MapObject *o = mapdata_object(i);
        if (!o || !o->use.set || o->use.used) continue;
        float dx = px - o->pos[0];
        float dz = pz - o->pos[2];
        float d = dx * dx + dz * dz;
        if (d < best_d) { best_d = d; best = o; }
    }
    if (!best) return;
    MapObject *mo = (MapObject *)best;
    if (best->use.once) mo->use.used = 1;
    SDL_Log("Use: %s", best->name[0] ? best->name : "(объект)");
    map_action_run(g, best->use.commands, best->use.cmd_n,
                   best->use.message, best->use.sound, best->pos, NULL, 0);
}

// --- Фазы загрузки мира ---------------------------------------------------------
// Порядок зафиксирован: world_load_geometry -> scene_build -> map_apply ->
// audio_load_map. Выгрузка — строгий инверс (см. game_unload_world).

//  Фаза 1. Геометрия мира: карта-пак, map.json, высоты/нормали, поле пшеницы.
static int world_load_geometry(Game *g, WorldTextures *wt)
{
    WorldState *w = &g->world;

    // Карта: zip из maps/ (рабочая папка или на уровень выше) или папка map/
    if (map_is_zip(g)) {
        char path[128];
        snprintf(path, sizeof path, "maps/%s.zip", g->map_name);
        if (!resources_pak_open(path)) {
            snprintf(path, sizeof path, "../maps/%s.zip", g->map_name);
            if (!resources_pak_open(path)) return -1;   // пак не открыт — чистить нечего
        }
    }
    // Всё про карту — в её корне: map/map.json или map.json (внутри zip)
    if (mapdata_load(map_is_zip(g) ? "map.json" : "map/map.json") != 0) {
        mapdata_free();            // частично загруженный json — убрать
        resources_pak_close();     // ~ resources_pak_open
        return -1;
    }
    if (world_load(w, wt) != 0) {
        mapdata_free();            // фаза 1 откатывается целиком
        world_free(w);             // частичные высоты/текстуры
        resources_pak_close();
        return -1;
    }

    // Поле пшеницы: прямоугольник мира, общий для рельефа, растительности
    // и узла FieldArea (рамка). Координаты — из map/map.json.
    g->field_pos[0]  = w->world_w / 2;
    g->field_pos[1]  = 0;
    g->field_pos[2]  = w->world_d / 2;
    g->field_size[0] = 500;
    g->field_size[1] = 0;
    g->field_size[2] = 300;
    {
        float c[2], s[2];
        if (mapdata_field(c, s)) {
            g->field_pos[0] = c[0];
            g->field_pos[2] = c[1];
            g->field_size[0] = s[0];
            g->field_size[2] = s[1];
        }
    }
    w->field_x0 = g->field_pos[0] - g->field_size[0] * 0.5f;
    w->field_z0 = g->field_pos[2] - g->field_size[2] * 0.5f;
    w->field_w = g->field_size[0];
    w->field_d = g->field_size[2];
    if (mapdata_field_attached()) {
        w->field_x0 = 0.0f;
        w->field_z0 = 0.0f;
        w->field_w = w->world_w;
        w->field_d = w->world_d;
    }
    return 0;
}

// Подключает к корню все созданные узлы сцены (не-созданные — пропускает).
static void scene_attach_nodes(Game *g)
{
    if (!g->root) return;
#define FR_ATTACH(n) if (g->n) node_add(g->root, g->n)
    FR_ATTACH(sky_node);
    FR_ATTACH(sun_node);
    FR_ATTACH(terr_node);
    FR_ATTACH(wheat_node);
    FR_ATTACH(obj_node);
    FR_ATTACH(fauna_node);
    FR_ATTACH(particles_node);
    FR_ATTACH(decals_node);
    FR_ATTACH(water_node);
    FR_ATTACH(field_node);
    FR_ATTACH(sel_node);
    FR_ATTACH(cam_node);
    FR_ATTACH(playerbox_node);
    FR_ATTACH(mm_node);
    FR_ATTACH(hud_node);
#undef FR_ATTACH
}

// Фаза 2. Сцена: тени, дерево узлов, сущности, FBO рендера, первый кадр.
static int scene_build(Game *g, const WorldTextures *wt)
{
    WorldState *w = &g->world;

    shadow_init();                     // каскадные карты теней

    g->root = node_new("World", "Node3D", NULL, NULL, NULL, NULL, NULL);

    g->sun_node = sun_make_node();
    SceneNode *sun_node = g->sun_node;

    // Камера: старт игрока из map.json (player/pos, yaw, pitch, fov)
    float start_pos[3] = {
        w->world_w / 2,
        height_curve(w, 1.0f - world_avg_lum()) + 6.0f,
        w->world_d / 2
    };
    float start_yaw = 3.14159f, start_pitch = -0.5f, start_fov = 60.0f;
    {
        float pp[3], pyaw, ppitch, pfov = 60.0f;
        if (mapdata_player(pp, &pyaw, &ppitch, &pfov)) {
            start_pos[0] = pp[0];
            start_pos[1] = pp[1];
            start_pos[2] = pp[2];
            start_yaw = pyaw;
            start_pitch = ppitch;
            start_fov = pfov;
            if (start_pos[1] < 1.0f)
                start_pos[1] = sample_height(w, start_pos[0], start_pos[2]) + EYE_H;
        }
    }
    g->cam_node = camera_make_node(w, start_pos, start_yaw, start_pitch,
                                   start_fov, 0.1f, w->world_w * 1.5f);
    if (!sun_node || !g->cam_node) {
        // Чистим за собой: созданные узлы + корень + тени (сцена не собрана).
        scene_attach_nodes(g);
        scene_teardown(g);
        return -1;
    }
    g->cam = g->cam_node->params;

    // fov из map.json — как значение по умолчанию, затем config.cfg поверх
    cvar_set("fov", g->cam->fov_deg);
    console_autoload();

        // Спавн: стартовая точка/взгляд игрока (для «Новой игры»)
    player_set_spawn(&g->player, g->cam->cam.pos, g->cam->cam.yaw,
                     g->cam->cam.pitch);

    // Узлы сцены получают единый рендер-контекст (renderer.c) вместо cam/sun
    const RendererContext *rc = renderer_context();
    g->sky_node = sky_make_node(rc);
    g->terr_node = terrain_make_node(w, wt, rc);
    g->wheat_node = veg_make_node(w, rc);
    g->obj_node = objects_make_node(w, rc);
    g->field_node = field_make_node(w, g->field_pos, g->field_size, rc);
    g->sel_node = selection_make_node(g->root, rc);
    g->playerbox_node = playerbox_make_node(g->cam, rc);
    g->mm_node = minimap_make_node(w, wt->tex_base, rc);
    g->hud_node = hud_make_node(w, rc);
    g->fauna_node = fauna_make_node(rc);
    g->particles_node = particles_make_node(w, rc);
    g->decals_node = decals_make_node(w, rc);
    g->water_node = water_make_node(w, wt, rc);
    if (!g->sky_node || !g->terr_node || !g->wheat_node || !g->field_node ||
        !g->sel_node || !g->mm_node || !g->hud_node) {
        SDL_Log("Failed to create a scene node (missing texture/shader?)");
        // Сцена не собрана начисто: освобождаем созданные узлы и тени.
        scene_attach_nodes(g);
        scene_teardown(g);
        return -1;
    }
    g->mm = g->mm_node->params;
    g->hud = g->hud_node->params;
    selection_set_on(0);             // рамки выделения — только по H (дебаг)

    scene_attach_nodes(g);

    // Сущности: классы, мир, игрок, объекты карты
    {
        static const EntityClass cls_world = { "world", NULL, NULL, NULL };
        static const EntityClass cls_player = { "player", NULL, NULL, NULL };
        static const EntityClass cls_prop = { "prop_static", NULL, NULL, NULL };
        entity_set_root(g->root);
        entity_register_class(&cls_world);
        entity_register_class(&cls_player);
        entity_register_class(&cls_prop);
        entity_bind("world", g->root, NULL);
        entity_bind("player", g->cam_node, NULL);
        for (int i = 0; i < mapdata_object_count(); i++) {
            const MapObject *o = mapdata_object(i);
            entity_spawn(o->class[0] ? o->class : "prop_static",
                         o->pos, NULL, NULL);
        }
    }

    // Размер окна в пикселях — его видит OpenGL (glViewport)
    int vw = renderer_vw(), vh = renderer_vh();
    g->cam->aspect = (float)vw / vh;
    g->mm->vw = vw;
    g->mm->vh = vh;
    g->hud->vw = vw;
    g->hud->vh = vh;

    // Bloom и воксельный туман — FBO размера окна (рендер)
    renderer_setup(w, vw, vh);

    // Первый кадр сцены (фон готов, камера посчитана)
    node_update_tree(g->root, 1.0f / 60.0f);
    entity_update_all(1.0f / 60.0f);
    voxfog_update(g->cam->cam.pos);

    g->world_loaded = 1;
    return 0;
}

// Фаза 3. Применение карты: cvar'ы и команды (после построения сцены).
static void map_apply(void)
{
    // cvar'ы карты (cvars: { имя: значение }) — поверх config.cfg
    for (int i = 0; i < mapdata_cvar_count(); i++) {
        const char *name = mapdata_cvar_name(i);
        if (!name) continue;
        cvar_set(name, mapdata_cvar_value(i));
        SDL_Log("Map cvar: %s = %g", name, mapdata_cvar_value(i));
    }
    // команды карты (commands: [ ... ]) — консольные команды
    for (int i = 0; i < mapdata_command_count(); i++) {
        const char *cmd = mapdata_command(i);
        if (!cmd) continue;
        SDL_Log("Map command: %s", cmd);
        console_exec(cmd);
    }
}

// Фаза 4. Звук карты: музыка, ambient/ветер, позиционные источники.
// (карта может положить свои wav в zip — ресурс-менеджер подтянет их)
static void audio_load_map(Game *g)
{
    MapSound ms;
    audio_loop_stop();
    audio_music_stop();
    if (mapdata_music(&ms)) {
        g->music = audio_load(ms.sound);
        if (g->music) audio_music_start(g->music, ms.volume);
        else SDL_Log("Map music: нет %s", ms.sound);
    }
    if (mapdata_ambient(&ms)) {
        g->ambient = audio_load(ms.sound);
        if (g->ambient) audio_loop_start(g->ambient, ms.volume);
        else SDL_Log("Map ambient: нет %s", ms.sound);
    } else {
        if (!g->wind) g->wind = audio_load("audio/wind.wav");
        if (g->wind) audio_loop_start(g->wind, 0.22f);
    }
    // Позиционные звуки карты (sounds[]): громкость от дистанции до игрока
    for (int i = 0; i < mapdata_sound_count(); i++) {
        const MapSound3D *m3 = mapdata_sound(i);
        if (!m3) continue;
        Sound *s = audio_load(m3->sound);
        if (s) {
            audio_3d_play(s, m3->volume, m3->pos, m3->radius);
            SDL_Log("Map sound: %s at %.0f,%.0f (r=%.0fm)", m3->sound,
                    m3->pos[0], m3->pos[2], m3->radius);
        } else {
            SDL_Log("Map sound: нет %s", m3->sound);
        }
    }
}

// Грузит карту и строит сцену. Вызывается по "НОВАЯ ИГРА"/"ПРОДОЛЖИТЬ".
// Пока грузится мир — экран загрузки: фон img/loading.png + полоса
// прогресса справа снизу с подписью фазы (loading.c предъявляет кадры).
int game_load_world(Game *g)
{
    loading_begin();
    loading_progress(0.05f, "Загрузка уровня");
    WorldTextures wt;
    if (world_load_geometry(g, &wt) != 0) { loading_end(); return -1; } // фаза 1
    loading_progress(0.30f, "Собираем сцену");
    if (scene_build(g, &wt) != 0) {
        // scene_build уже освободила частичную сцену (scene_teardown внутри);
        // здесь — откат данных фазы 1, иначе они утекают.
        world_unload_geometry(g);
        SDL_Log("World load failed: scene_build");
        loading_end();
        return -1;                                       // фаза 2: сцена
    }
    loading_progress(0.60f, "Настройки карты");
    map_apply();                                       // фаза 3: cvar'ы/команды
    loading_progress(0.75f, "Звук карты");
    audio_load_map(g);                                 // фаза 4: звук карты

    // Статические тени (режим 2): готовим карту теней сразу при загрузке
    loading_progress(0.85f, "Тени и маски");
    if (cvar_get("r_shadows_mode") == 2.0f)
        terrain_shadows_bake(&g->world, (SunParams *)g->sun_node->params);
    // Мокрость: запечённая маска "под крышей" (всегда, полезна для мокрости)
    terrain_wetness_mask_bake(&g->world);
    SDL_Log("World loaded (%.0fx%.0f m)", g->world.world_w, g->world.world_d);

    // Скрипт карты (lua/maps/<карта>.lua): после сцены и сущностей
    loading_progress(0.95f, "Скрипт карты");
    lua_engine_map(g->map_name);
    loading_progress(1.0f, "Готово");
    loading_end();
    return 0;
}

// --- Фазы выгрузки мира (СТРОГИЙ ИНВЕРС порядка создания) ----------------------
//  Инверс фазы 4 (audio_load_map)
static void audio_stop_map(void)
{
    audio_loop_stop();           // ветер/ambient — только в игре
    audio_music_stop();          // музыка карты — тоже
    audio_3d_stop_all();         // и позиционные источники
}

//  Инверс фазы 2 (scene_build): FBO рендера -> сущности -> узлы в обратном
//  порядке создания -> корень -> тени.
static void scene_teardown(Game *g)
{
    renderer_teardown();         // ~ renderer_setup (создавался последним)
    entity_shutdown();           // ~ entity_* (kill освобождает свои узлы)
    hud_cleanup(g->hud_node);    // ~ hud_make_node (последний из узлов)
    minimap_cleanup(g->mm_node); // ~ minimap_make_node
    playerbox_cleanup(g->playerbox_node); // ~ playerbox_make_node
    selection_cleanup(g->sel_node);
    field_cleanup(g->field_node);
    if (g->fauna_node) fauna_cleanup(g->fauna_node);
    if (g->particles_node) particles_cleanup(g->particles_node);
    if (g->decals_node) decals_cleanup(g->decals_node);
    if (g->water_node) water_cleanup(g->water_node);
    if (g->obj_node) objects_cleanup(g->obj_node);
    veg_cleanup(g->wheat_node);
    terrain_cleanup(g->terr_node);
    sky_cleanup(g->sky_node);    // ~ sky_make_node (первый из узлов)
    node_free_tree(g->root);     // ~ корень и все узлы (камера, солнце)
    shadow_cleanup();            // ~ shadow_init (создавался первым)
    g->root = NULL;
    g->cam_node = g->sun_node = g->sky_node = g->terr_node = g->wheat_node = NULL;
    g->obj_node = g->field_node = g->sel_node = g->mm_node = g->hud_node = NULL;
    g->playerbox_node = NULL;
    g->fauna_node = g->particles_node = g->decals_node = g->water_node = NULL;
    g->cam = NULL;
    g->mm = NULL;
    g->hud = NULL;
    g->world_loaded = 0;
}

//  Инверс фазы 1 (world_load_geometry)
static void world_unload_geometry(Game *g)
{
    mapdata_free();              // ~ mapdata_load
    world_free(&g->world);       // ~ world_load
    resources_pak_close();       // ~ resources_pak_open
}

// Выгружает мир при выходе в главное меню
void game_unload_world(Game *g)
{
    if (!g->world_loaded) return;
    lua_engine_clear();          // сбросить таймеры/update скрипта карты
    audio_stop_map();            // фаза 4: звук карты
    scene_teardown(g);           // фаза 2: сцена
    world_unload_geometry(g);    // фаза 1: данные карты
    material_clear_all();        // материалы карты не перетекают в следующую
    SDL_Log("World unloaded");
}

// --- Выбор карты: сохранение в config.cfg (строка map_name <имя>) --------------
void game_map_config_load(Game *g)
{
    FILE *f = fopen("config.cfg", "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "map_name %63s", g->map_name) == 1) { }
    }
    fclose(f);
}

void game_map_config_save(Game *g)
{
    // Раньше map_name дописывался в конец (append) — при каждом выходе строка
    // копилась, файл рос. Теперь: читаем config.cfg (его до этого обновил
    // console_save_config), выкидываем все старые map_name и пишем ровно одну.
    char keep[4096] = { 0 };
    int n = 0;
    FILE *r = fopen("config.cfg", "r");
    if (r) {
        char line[128];
        while (fgets(line, sizeof line, r)) {
            char mname[64];
            if (sscanf(line, "map_name %63s", mname) == 1) continue;  // старую — вон
            size_t l = strlen(line);
            if (n + (int)l < (int)sizeof keep - 1) {
                memcpy(keep + n, line, l);
                n += (int)l;
                keep[n] = 0;
            }
        }
        fclose(r);
    }
    FILE *w = fopen("config.cfg", "w");
    if (w) {
        fputs(keep, w);
        fprintf(w, "map_name %s\n", g->map_name);
        fclose(w);
    }
}

// --- Обновление мира (каждый кадр из главного цикла) ---------------------------
void game_update(Game *g, float dt)
{
    if (!g->world_loaded) return;
    // Логика: мир живёт только в игре; в меню/паузе — заморожен
    if (app_state() == APP_PLAY) {
        node_update_tree(g->root, dt);
        fx_update(dt);              // эмиттеры эффектов (дождь/огонь/...)
        entity_update_all(dt);
        if (mapdata_field_attached() && g->field_node) {
            g->field_node->pos[0] = g->cam->cam.pos[0];
            g->field_node->pos[2] = g->cam->cam.pos[2];
        }
        map_actions_update(g);      // триггеры + взаимодействие (E)
    } else {
        // Камера пересчитывает матрицы и в меню/паузе (ввод заблокирован):
        // FOV из меню применяется сразу, а не только при входе в игру.
        camera_node_update(g->cam_node, dt);
    }
    voxfog_update(g->cam->cam.pos);
    audio_set_listener(g->cam->cam.pos);
    // Таймер сообщения карты (message из триггеров/use)
    if (g->ui_msg_t > 0.0f && app_state() == APP_PLAY)
        g->ui_msg_t -= dt;
}
