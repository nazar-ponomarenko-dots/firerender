// ============================================================================
//  binds.h — привязки клавиш к командам (как в Half-Life)
// ----------------------------------------------------------------------------
//  bind <key> <command> — привязать клавишу. При нажатии выполняется
//  команда (или комбинация через ";"), при отпускании — "-команда"
//  (для команд с префиксом "+"). Бинды хранятся в config.cfg
//  (строки "bind key command") и переживают перезапуск.
//  Дефолтные бинды встают, только если в config.cfg биндов нет.
// ============================================================================
#ifndef BINDS_H
#define BINDS_H
#include <SDL3/SDL.h>
#include <stdio.h>

#define BINDS_MAX 64

typedef struct {
    SDL_Scancode sc;     // клавиша (по scancode — работает на любой раскладке)
    char cmd[96];        // команда (может содержать ";" — комбинация)
} Bind;

// Виртуальные "клавиши" колеса мыши (реальных scancode у них нет)
#define BIND_WHEEL_UP   ((SDL_Scancode)SDL_SCANCODE_COUNT)
#define BIND_WHEEL_DOWN ((SDL_Scancode)(SDL_SCANCODE_COUNT + 1))

// Дефолтные бинды (если config.cfg не задаёт свои)
void binds_init(void);

// Сброс к дефолтам (команда bind_reset, меню "Сброс управления")
void binds_reset(void);

// Выполнить бинд клавиши: down=1 — команда, down=0 — "-команда" для "+"
void binds_key(SDL_Scancode sc, int down);

// Колесо мыши: up=1 — вверх, 0 — вниз (импульсное действие)
void binds_wheel(int up);

// Имя клавиши для консоли/меню (в т.ч. mouse_wheel_up/down)
const char *binds_key_name(SDL_Scancode sc);

// Имя -> клавиша (в т.ч. mouse_wheel_up/down)
SDL_Scancode binds_key_from_name(const char *name);

// Команда, привязанная к клавише (NULL — нет)
const char *binds_cmd(SDL_Scancode sc);

// Привязать/убрать
void binds_set(SDL_Scancode sc, const char *cmd);
void binds_clear(SDL_Scancode sc);
void binds_clearall(void);

// Первая клавиша, на которой висит команда (для меню настроек)
SDL_Scancode binds_key_for_cmd(const char *cmd);

// Доступ по индексу (bindlist, меню)
int binds_count(void);
const Bind *binds_at(int i);

// config.cfg: загрузка bind-строк и сохранение всех биндов
void binds_load_config(void);
void binds_save_config(FILE *f);

#endif
