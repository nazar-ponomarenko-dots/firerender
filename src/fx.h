// ============================================================================
//  fx.h — эффекты из частиц: именованные пресеты и непрерывные эмиттеры
// ----------------------------------------------------------------------------
//  В стиле HL2/Far Cry 2: эффект — именованный набор параметров частиц
//  (ParticleParams) + количество. Эффекты можно запускать из консоли
//  (команда fx) и из Lua (engine.fx_*). Непрерывные эмиттеры (дождь, снег,
//  огонь, дым из трубы) спавнят частицы с заданной частотой в области.
//
//  Встроенные эффекты: blood, smoke, explosion, fire, rain, snow, sparks,
//  dust, muzzle_flash. Регистрация своих — fx_register (из кода/Lua).
// ============================================================================
#ifndef FX_H
#define FX_H

#include "particles.h"

#define FX_MAX_PRESETS 64
#define FX_MAX_EMITTERS 8
#define FX_NAME_LEN 32

typedef struct {
    char name[FX_NAME_LEN];
    ParticleParams pp;
    int count;             // сколько частиц в burst
} FxPreset;

// --- Пресеты ----------------------------------------------------------------
// Регистрирует именованный пресет (заменяет существующий с тем же именем).
// count — число частиц по умолчанию для fx_burst(name).
void fx_register(const char *name, const ParticleParams *pp, int count);

// Ищет пресет по имени. NULL — нет такого.
const FxPreset *fx_find(const char *name);

// Запускает burst пресета в точке (count — или 0 = дефолт пресета).
void fx_burst(const char *name, float x, float y, float z, int count);

// --- Эмиттеры (непрерывные) --------------------------------------------------
// Запускает эмиттер: спавн частиц пресета с rate шт/сек в боксе
// [pos±ext]. life — длительность в сек (<=0 — пока не остановят).
// name — имя пресета; у эмиттера тот же name (stop по имени).
void fx_emitter_start(const char *name, const float *pos, const float *ext,
                      float rate, float life);
// Останавливает эмиттеры с данным именем (все).
void fx_emitter_stop(const char *name);
// Останавливает все эмиттеры.
void fx_emitter_stop_all(void);

// Тикает эмиттеры (вызывать из particles_node_update/particles_update).
void fx_update(float dt);

// --- Управление и консоль -----------------------------------------------------
// Список пресетов в консоль (fx_list).
void fx_print_list(void);
// Регистрирует консольные команды: fx, fx_burst, fx_list, fx_clear.
void fx_init_console(void);

#endif
