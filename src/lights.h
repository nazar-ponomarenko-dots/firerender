// ============================================================================
//  lights.h — кластерное освещение (Forward+): реестр источников и froxel-сетка
// ----------------------------------------------------------------------------
//  Раньше движок был КЛАССИЧЕСКИМ forward: каждый шейдер получал массивы
//  uniform'ов (8 точечных + 4 прожектора) и в КАЖДОМ фрагменте перебирал их
//  все. Цена = пиксели x источники, поэтому лимит был жёстким и низким.
//
//  Теперь — КЛАСТЕРНЫЙ forward (как в Doom 2016 / UE «clustered forward»):
//   1) экран режется на плитки (CLUSTER_X x CLUSTER_Y), а вид по глубине —
//      на CLUSTER_Z логарифмических слоёв. Получается 3D-сетка «фрокселей»;
//   2) каждый кадр CPU раскладывает источники по фрокселям, которые они
//      реально задевают (по экранному прямоугольнику сферы влияния и по
//      диапазону глубин), и пишет два буфера: список индексов + диапазон
//      (offset,count) на фроксель;
//   3) фрагментный шейдер по своим gl_FragCoord и глубине находит фроксель и
//      перебирает ТОЛЬКО источники из него — обычно 0..4 вместо всех.
//  Итог: сотни источников почти без падения FPS, лимит поднят до LIGHT_MAX.
//
//  Типы источников (все с мягкими тенями по размеру источника):
//    LT_POINT — точечный (radius = физический радиус лампы, даёт мягкий блик);
//    LT_SPOT  — прожектор (конус inner/outer, плавная кромка);
//    LT_RECT  — площадной прямоугольный (окно, экран, лайтбокс): освещает
//               как площадка, а не точка — блик вытянут по её форме;
//    LT_TUBE  — линейный/трубчатый (люминесцентная лампа, неон).
//  Направленные источники (солнце и фоновые) остаются отдельно — их всего
//  несколько и они влияют на все пиксели, кластеризовать их незачем.
//
//  Требует GL >= 4.3 (SSBO во фрагментном шейдере). Если контекста нет —
//  lights_active() == 0, и шейдеры используют прежний uniform-путь.
// ============================================================================
#ifndef LIGHTS_H
#define LIGHTS_H
#include "gl_loader.h"
#include "render_ctx.h"

// Размер сетки фрокселей. 16x9 плиток (соотношение 16:9) x 24 слоя глубины.
#define CLUSTER_X 16
#define CLUSTER_Y 9
#define CLUSTER_Z 24
#define CLUSTER_COUNT (CLUSTER_X * CLUSTER_Y * CLUSTER_Z)
// Потолок источников в кадре и суммарной длины списков фрокселей.
#define LIGHT_MAX 256
#define CLUSTER_INDEX_MAX (CLUSTER_COUNT * 24)

typedef enum {
    LT_POINT = 0,
    LT_SPOT,
    LT_RECT,
    LT_TUBE
} LightType;

// Источник в мире (CPU-представление; в SSBO уходит упакованным по vec4).
typedef struct {
    int type;
    float pos[3];
    float range;             // радиус влияния, м (за ним света нет)
    float color[3];
    float intensity;
    float dir[3];            // направление (spot) / нормаль площадки (rect)
    float radius;            // радиус источника, м: мягкость блика и тени
    float cos_outer;         // spot: косинус внешнего угла
    float cos_inner;         // spot: косинус внутреннего угла (плавная кромка)
    float up[3];             // rect: вторая ось площадки; tube: ось трубки
    float size[2];           // rect: полуразмеры (w, h); tube: (длина/2, 0)
    int shadow;              // задел: источник даёт тени
} Light;

// --- Жизненный цикл -----------------------------------------------------------
// Создаёт SSBO и проверяет capability. Вызывать из renderer_setup.
void lights_init(void);
void lights_shutdown(void);

// 1, если кластерный путь доступен (GL 4.3+) и включён cvar'ом r_clustered.
int lights_active(void);

// --- Наполнение (каждый кадр, до рендера сцены) --------------------------------
// Сбрасывает список источников кадра.
void lights_begin(void);

// Добавляет источник. Возвращает индекс или -1 (лимит).
int lights_add(const Light *l);

// Удобные конструкторы (заполняют дефолты и добавляют).
int lights_add_point(const float pos[3], const float color[3],
                     float intensity, float range, float radius);
int lights_add_spot(const float pos[3], const float dir[3],
                    const float color[3], float intensity, float range,
                    float angle_outer, float angle_inner, float radius);
int lights_add_rect(const float pos[3], const float dir[3], const float up[3],
                    const float color[3], float intensity, float range,
                    float half_w, float half_h);
int lights_add_tube(const float pos[3], const float axis[3],
                    const float color[3], float intensity, float range,
                    float half_len, float radius);

// Строит сетку фрокселей и заливает буферы. Вызывать один раз за кадр после
// всех lights_add (renderer_ctx_fill). Требует матрицы вида/проекции из rc.
void lights_build(const RendererContext *rc);

// Привязывает SSBO к точкам 3/4/5 и шлёт uniform'ы сетки в программу.
// Вызывается из scene_uniforms_bind для каждого шейдера сцены.
void lights_bind(GLuint prog);

// Ищет uniform-хендлы сетки в программе (кешируются внутри по prog).
void lights_locs_init(GLuint prog);

// --- Статистика (HUD/профайлер) -----------------------------------------------
int lights_count(void);              // источников в кадре
int lights_cluster_fill(void);       // суммарная длина списков фрокселей
int lights_cluster_max(void);        // максимум источников на фроксель

#endif
