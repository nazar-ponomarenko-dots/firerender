// ============================================================================
//  phys_body.h — твёрдые тела поверх Verlet-мира (см. physics.h)
// ----------------------------------------------------------------------------
//  Физика движка остаётся «частицы + связи» (дешёвый Verlet, без C++-SDK).
//  Этот слой добавляет то, чего в ядре не было: ТЕЛО как группу частиц с
//  формой, СТАТИЧЕСКИЕ коллайдеры сцены (сферы/цилиндры/боксы из GLB-пропов
//  и map.json) и взаимодействие «частица-с-частицей» — иначе ящик
//  проваливается сквозь ящик.
//
//    * сфера  = 1 частица с radius;
//    * бокс   = 8 углов + рёбра + диагонали граней (жёсткий кадр, как скелет
//               рэгдолла в glb_anim.c: связи держат форму);
//    * гравитация/интеграция/связи — унаследованы из physics.c, ничего
//      своего не считается дважды;
//    * массы — inv_mass на частицу, тело массой m даёт 8 частиц по m/8.
//
//  Тела живут в ГЛОБАЛЬНОМ мире physics_world() вместе с верёвками, поэтому
//  верёвка может цеплять ящик. Рубильник phys_bodies (cvar) выключает
//  симуляцию тел, состояния сохраняются (включил — тела там же, где были).
//
//  Консоль: phys_sphere/phys_box/phys_props/phys_bodies; FR_TEST 'u' — selftest,
//  'U' — стопка ящиков перед камерой (живой прогон).
// ============================================================================
#ifndef PHYS_BODY_H
#define PHYS_BODY_H

#include "physics.h"

#define PHYS_BODY_MAX      128     // тел в мире
#define PHYS_STATIC_MAX    256     // статических коллайдеров

typedef enum {
    PHYS_SPHERE = 0,
    PHYS_BOX
} PhysBodyShape;

typedef struct {
    int      shape;
    int      first_part;           // индекс частицы в PhysWorld
    int      nparts;
    float    half;                 // сфера: радиус; бокс: полудиагональ
    float    half_ext[3];          // бокс: половинные размеры (local axes)
    int      sleeping;             // покоится (для статистики/отладки)
} PhysBody;

// Статический коллайдер: сфера, вертикальный цилиндр (x,z,r + base..top)
// или box по оси Y (центр c, половинные размеры e).
typedef struct {
    int kind;                      // 0 сфера, 1 цилиндр, 2 box
    float c[3];
    float r;                       // сфера/цилиндр
    float e[3];                    // box: половинные размеры
    float base, top;               // цилиндр: нижняя/верхняя Y
} PhysStatic;

// Инициализация слоя (реестр пуст, настройки дефолтные). Идемпотентно.
void phys_body_init(void);
// Выключатель слоя (cvar phys_bodies): 0 — тела не двигаются.
void phys_body_enable(int on);
int  phys_body_enabled(void);

// Мир один зафиксированный шаг: интеграция+связи (physics_step) плюс
// коллизии «частица-частица» и «частица-статика». Вызывать из main-цикла
// вместо physics_step для глобального мира.
void phys_body_step(PhysWorld *w, float dt);

// Создание тел (pos — центр; mass — кг; rest = покоится). -1 — лимит/ошибка.
int  phys_body_spawn_sphere(PhysWorld *w, const float pos[3], float radius,
                           float mass);
int  phys_body_spawn_box(PhysWorld *w, const float pos[3], const float ext[3],
                        float mass);
void phys_body_kill_all(PhysWorld *w);   // убрать только тела, верёвки целы

const PhysBody *phys_body_get(int idx);
int  phys_body_count(void);
void phys_body_impulse(int idx, const float vel[3]);
// Центр тела (среднее частиц) — для рендера/отладки.
void phys_body_center(int idx, float out[3]);

// --- Статические коллайдеры ---------------------------------------------------
int  phys_static_add_sphere(const float c[3], float r);      // idx или -1
int  phys_static_add_cylinder(float x, float z, float r,
                              float base, float top);
int  phys_static_add_box(const float c[3], const float e[3]);
void phys_static_clear(void);
int  phys_static_count(void);
const PhysStatic *phys_static_get(int i);

// Толкнуть тела от точки (игрок): тела в радиусе pr+r получают импульс
// вдоль (dir) * speed. dir — горизонтальная скорость игрока, м/с.
void phys_body_push(const float pos[3], const float vel[3], float pr,
                    float speed_limit);

// Отладочный вывод: номер тела, к которому принадлежит частица мира (или -1).
// Частицы тел и так лежат в physics_world() и рисуются общим циклом точек в
// renderer.c — этот запрос нужен, чтобы окрасить их отдельно от верёвок.
int  phys_body_index_of_part(int part_idx);

// Консольные команды (регистрация — в cmdreg.c).
void cmd_phys_sphere(const char *args);
void cmd_phys_box(const char *args);
void cmd_phys_props(const char *args);
void cmd_phys_bodies(const char *args);

// Самопроверка слоя (FR_TEST 'u'): стопка покоится, импульс работает,
// сквозь статику не проходит, переполнение = -1. Провалов.
int  phys_body_selftest(void);

#endif
