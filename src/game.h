// ============================================================================
//  game.h — игровое состояние и жизненный цикл мира (см. game.c)
// ----------------------------------------------------------------------------
//  Всё состояние мира собрано в ОДНУ структуру Game (раньше — 13 узловых
//  глобалов + глобалы карты/спавна в main.c): узлы сцены, камера/HUD,
//  выбранная карта, спавн/сохранение, сообщение карты, демо и данные мира
//  (WorldState из world.h). Модули получают Game* параметром, а не хватают
//  глобал; для функций без параметра (консольные команды) есть
//  game_instance().
// ============================================================================
#ifndef GAME_H
#define GAME_H
#include "audio.h"
#include "camera.h"
#include "hud.h"
#include "minimap.h"
#include "player.h"
#include "scene.h"
#include "sun.h"
#include "world.h"

// --- Игровое состояние ---------------------------------------------------------
typedef struct {
    // --- Узлы сцены (создаются в game_load_world) ---
    SceneNode *root;           // корень дерева (World)
    SceneNode *cam_node;
    SceneNode *sun_node;
    SceneNode *sky_node, *terr_node, *wheat_node;
    SceneNode *obj_node, *field_node;
    SceneNode *fauna_node;
    SceneNode *particles_node, *decals_node;
    SceneNode *sel_node, *mm_node, *hud_node;
    CameraParams *cam;         // параметры узла Camera
    MinimapParams *mm;         // параметры узла Minimap
    HudParams *hud;            // параметры узла DebugHUD
    int world_loaded;

    // Текущая карта: maps/<имя>.zip (единственный формат — карта-пак)
    char map_name[64];

    // Поле пшеницы: позиция/размер узла FieldArea (считает фаза
    // world_load_geometry, использует scene_build)
    float field_pos[3];
    float field_size[3];

    // Игрок: спавн, сохранение/загрузка, телепорт (см. player.h)
    Player player;

    // Сообщение карты (message из триггеров/use) — рисует renderer.c
    char ui_msg[256];
    float ui_msg_t;

    // Демо-сцена (клавиша Q): автополёт с замером FPS
    int demo_on;
    float demo_t;
    double demo_frames, demo_elapsed, demo_min_dt, demo_max_dt;

    // Звуки карты (петли): ветер по умолчанию, ambient и music из map.json
    Sound *wind, *ambient, *music;

    // Данные мира (высоты, размеры, поле) — заполняет world_load
    WorldState world;
} Game;

// --- Функции ------------------------------------------------------------------
// Регистрирует единственный экземпляр (вызывать до остальных функций).
// Обнуляет состояние, дефолтные карта и кривая высот.
void game_init(Game *g);

// Единственный экземпляр (для функций без параметра: консольные команды,
// input.c, sync_cvars в main.c).
Game *game_instance(void);

// Телепорт игрока: позиция + взгляд, сброс скоростей.
void game_apply_spawn(Game *g, const float pos[3], float yaw, float pitch);

// Загрузка/выгрузка мира: карта-пак, map.json, узлы сцены, звук карты.
// load возвращает 0 при успехе. Вызывать по "НОВАЯ ИГРА"/"ПРОДОЛЖИТЬ".
int game_load_world(Game *g);
void game_unload_world(Game *g);

// Выбранная карта: config.cfg (строка map_name <имя>)
void game_map_config_load(Game *g);
void game_map_config_save(Game *g);

// Обновление мира: логика узлов, триггеры/use, туман, звук карты.
// Вызывать каждый кадр из главного цикла (в меню/паузе — только камера).
void game_update(Game *g, float dt);

// Демо-сцена (клавиша Q): автополёт с замером FPS. inst_fps — FPS кадра.
void game_demo_toggle(Game *g);
void game_demo_stop(Game *g);
void game_demo_tick(Game *g, float dt, float inst_fps);

#endif
