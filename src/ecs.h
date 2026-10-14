// ============================================================================
//  ecs.h — data-oriented ECS поверх движка (сущности-ID + компоненты + системы)
// ----------------------------------------------------------------------------
//  Зачем: SceneNode/Entity — объектная модель (указатели, виртуальные методы
//  через function-поинтеры). ECS добавляет параллельный слой «данные прежде
//  кода»: сущность = целый ID, компонент = POD-структура в пуле типа, система
//  = проход по массиву. Это дешёвые массовые объекты (фауна, частицы,
//  триггеры) и предсказуемый кэш. Никакого рендера здесь нет: связка со
//  сценой — в ecs_sync.c, рисуют по-прежнему узлы.
//
//  Хэндли: наружу ID не отдаётся как указатель — только хэндл
//  (serial<<16|num, беззнаковая сборка, 0 = «нет»), как в entity.c. Мёртвый
//  или переиспользованный слот даёт хэндл, по которому ecs_get вернёт NULL.
//
//  Память: ВСЁ статично (статические пулы слотов). Ни одного malloc — ни в
//  кадре, ни при регистрации. Ёмкость фиксирована; переполнение — сообщение
//  в лог и NULL, а не рост массива (fail-soft контракт движка).
// ============================================================================
#ifndef ECS_H
#define ECS_H

#include <stdint.h>

typedef int EcsId;                 // <0 — негодный ID

#define ECS_MAX_ENTS      2048     // живых сущностей
#define ECS_MAX_COMPONENTS 64      // типов компонентов (по 1 биту в маске)
#define ECS_MAX_SYSTEMS   32

// Регистрация компонента: name для ecs_list, size — размер POD-структуры.
// id — маленький индекс (0..), присваивается порядком регистрации; НЕ годится
// как указатель и НЕ переживает ecs_shutdown. 0 при реестре полном/ошибке.
int ecs_register_component(const char *name, int size);

// Система: вызывает fn(id, ud) для каждой живой сущности, чья маска содержит
// all. Порядок — по возрастанию order (равные — по регистрации).
typedef void (*EcsSystemFn)(EcsId id, void *ud);
void ecs_register_system(const char *name, uint64_t all, int order,
                         EcsSystemFn fn, void *ud);

// Сущность. ecs_create резервирует ID без компонентов; удалить component
// можно только явно (ecs_detach) — при ecs_kill компоненты освобождаются.
EcsId        ecs_create(void);
void         ecs_kill(EcsId id);          // идемпотентно
int          ecs_is_alive(EcsId id);
uint64_t     ecs_mask(EcsId id);

// Компоненты. Присоединение инициализирует слот нулями; detach очищает.
// При смерти сущности все её компоненты detach автоматически.
// Указатель из ecs_get Валиден, пока компонент не detach и сущность жива:
// после переиспользования слота он может смотреть в чужой компонент —
// через границы Lua/консоли держите хэндл и проверяйте ecs_owner().
int   ecs_attach(EcsId id, int comp);
void  ecs_detach(EcsId id, int comp);
void *ecs_get(EcsId id, int comp);        // NULL — компонент не присоединён
int   ecs_has(EcsId id, int comp);
EcsId ecs_owner(const void *comp_ptr, int comp); // -1 — указатель не из пула

// Стабильные внешние хэндлы (для Lua/консоли/сохранений).
int   ecs_handle(EcsId id);               // 0 = негодный
EcsId ecs_get_by_handle(int handle);      // -1 = сущности нет/переиспользована

// Прогнать системы по порядку. Вызывается один раз за тик логики (game.c).
void  ecs_update(float dt);

// --- Отладка/консоль ---
int   ecs_count(void);                    // живых сущностей
int   ecs_system_count(void);             // зарегистрированных систем
int   ecs_component_types(void);          // занятых слотов реестра компонентов
int   ecs_component_count(int comp);      // сущностей с данным типом
const char *ecs_component_name(int comp);
void  ecs_list(void);                     // вывод в консоль
void  ecs_shutdown(void);                 // убить всё (смена карты/выход)

// Консольные команды (обработчики здесь, регистрация — в cmdreg.c).
void cmd_ecs_list(const char *args);
void cmd_ecs_stats(const char *args);
void cmd_ecs_test(const char *args);

// Самотест без окна (FR_TEST=Sx его зовёт). Возвращает число провалов.
int   ecs_selftest(void);

#endif
