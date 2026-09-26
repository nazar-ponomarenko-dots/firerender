// ============================================================================
//  player.h — состояние игрока (модуль «движок↔игра»)
// ----------------------------------------------------------------------------
//  Граница между «движком» и «игроком»: всё, что касается персонажа —
//  стартовая позиция (спавн), сохранение/загрузка, телепорт — живёт здесь,
//  а не в game.c. Движок (камера, физика, рендер) не знает игрока; модули
//  игрового слоя (commands.c, lua) работают через Player* и game_apply_spawn.
//
//  Физическое состояние (позиция/скорости/взгляд) — это Camera из camera.h;
//  Player хранит только «высокоуровневое» состояние персонажа: где спавн
//  и куда телепортироваться/грузить.
// ============================================================================
#ifndef PLAYER_H
#define PLAYER_H
#include "camera.h"
#include "health.h"

typedef struct {
    // Стартовая позиция (спавн) — из map.json, для пункта «Новая игра»
    float spawn_pos[3];
    float spawn_yaw, spawn_pitch;
    // Сохранение (F5/F9): позиция + взгляд + карта (save.cfg)
    float save_pos[3];
    float save_yaw, save_pitch;
    char save_map[64];
    int pending_spawn;       // телепорт после загрузки мира (load из меню)
    Health hp;               // здоровье игрока (health.h)
} Player;

// Обнуляет состояние, дефолтные спавн-углы (по умолчанию yaw: PI).
void player_init(Player *p);

// Задаёт точку спавна (позиция + взгляд). Вызывается при построении мира.
void player_set_spawn(Player *p, const float pos[3], float yaw, float pitch);

// Телепорт игрока: позиция + взгляд на Camera, сброс скоростей/полёта.
void player_teleport(Player *p, Camera *cam, const float pos[3],
                     float yaw, float pitch);

// Сохранить текущее состояние игрока (позиция/взгляд камеры + карта).
void player_save(Player *p, const Camera *cam, const char *map_name);

// Загрузить сохранённую позицию/взгляд/карту (из save.cfg).
void player_load(Player *p, float x, float y, float z,
                 float yaw, float pitch, const char *map_name);

#endif