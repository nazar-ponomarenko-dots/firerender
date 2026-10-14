// ============================================================================
//  physics.h — мини-движок физики тел (Verlet) — фундамент для рэгдоллов
// ----------------------------------------------------------------------------
//  Классическая схема «частицы + связи» (как в Hitman/Джопас Верменис):
//    * частица: позиция + предыдущая позиция (скорость = разница), масса;
//    * связь (stick): две частицы на фиксированном расстоянии — за N
//      итераций релаксации сеть частиц держит форму (ткань, верёвка,
//      скелет рэгдолла);
//    * интеграция Верле устойчива при больших шагах и дешева (без производных);
//    * коллизии: рельеф (sample_height — как у игрока), сфера-земля.
//
//  Это ЯДРО, без рендера: состояния частиц читает рендер (сейчас —
//  отладочные точки в renderer_end, потом — кости скелета рэгдолла).
//
//  Консоль: phys_rope [длина] — верёвка в 5 м перед камерой (демо).
// ============================================================================
#ifndef PHYSICS_H
#define PHYSICS_H

#define PHYS_MAX_PARTS  512
#define PHYS_MAX_LINKS  2048

typedef struct {
    float pos[3];        // текущая позиция
    float prev[3];       // позиция прошлого шага (скорость Верле)
    float inv_mass;      // 1/масса (0 = прибита к месту)
    float radius;        // радиус для коллизии с рельефом (0 = точка)
} PhysPart;

typedef struct {
    int a, b;            // индексы частиц
    float rest;          // длина покоя
} PhysLink;

typedef struct {
    PhysPart part[PHYS_MAX_PARTS];
    int np;
    PhysLink link[PHYS_MAX_LINKS];
    int nl;
    float gravity;       // м/с^2 (вниз)
    int iterations;      // итерации релаксации связей (4..8 обычно)
    float damping;       // 0..1: доля скорости, теряемая за шаг (воздух)
    float dt;            // шаг последнего physics_step (для единиц импульса)
} PhysWorld;

// Сброс мира к пустому с настройками по умолчанию.
void physics_reset(PhysWorld *w);

// Глобальный тестовый мир (верёвка/цепь из консоли; фундамент рэгдолла).
PhysWorld *physics_world(void);

// Колбэк высоты рельефа (main.c: sample_height мира игры).
void physics_set_ground(float (*fn)(float x, float z));

// Шаг симуляции (вызывать с фиксированным dt, 1/100 с — как логика игры).
void physics_step(PhysWorld *w, float dt);

// Только релаксация связей (без интеграции): внешние ограничения (лимиты
// суставов) сдвинули частицы — связи дожимаются, чередуясь с ними.
void physics_relax(PhysWorld *w, int iterations);

// Частицы и связи. Возвращает индекс (-1 при переполнении).
int physics_add_part(PhysWorld *w, const float pos[3], float inv_mass,
                     float radius);
int physics_add_link(PhysWorld *w, int a, int b);

// Прибить/открепить частицу (pin = 1 — прибита).
void physics_pin(PhysWorld *w, int idx, int pin);

// Импульс частице (мгновенная скорость, м/с).
void physics_impulse(PhysWorld *w, int idx, const float vel[3]);

#endif
