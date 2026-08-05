// ============================================================================
//  vegetation.h — трава и пшеница (MeshInstance3D): инстансинг, 2 слоя
// ----------------------------------------------------------------------------
//  Слой 1 — СТАТИЧНЫЙ: вся карта покрыта травой/пшеницей с минимальной
//  плотностью. Генерируется ОДИН раз при старте, заливается в VBO и больше
//  НЕ трогается — растительность есть на всей карте с первой секунды.
//  Слой 2 — ДИНАМИЧЕСКИЙ: вокруг камеры плотность растёт по LOD-кривой,
//  пересоздаются только чанки, чья плотность заметно изменилась.
//
//  Спрайты — слои 2D-массива 256x178 (0=img/grass.png, 1-3=img/wheat_lod0/1/2.png;
//  LOD выбирается в шейдере по дистанции до камеры), тип и
//  поворот/размер/тон/наклон — атрибуты экземпляра (10 float). Отрисовка —
//  4 инстанс-вызова (2 статики + 2 динамики), один шейдер.
//  Слой (quad index q): 0=статика трава, 1=статика пшеница,
//                       2=динамика трава, 3=динамика пшеница.
// ============================================================================
#ifndef VEGETATION_H
#define VEGETATION_H
#include "gl_loader.h"
#include "scene.h"
#include "camera.h"
#include "sun.h"
#include "shadow.h"

typedef struct {
    const WorldState *world;            // данные мира: размеры, высоты, поле
    GLuint prog;                        // общая программа растительности
    GLuint vao_sg, vao_sw, vao_dg, vao_dw;   // статик/динамик x трава/пшеница
    GLuint vbo_sg, vbo_sw, vbo_dg, vbo_dw;
    GLuint tex;                         // массив спрайтов (0=трава, 1=пшеница)
    GLint u_viewproj, u_campos, u_fogcolor, u_fogstart, u_fogend, u_fogon;
    GLint u_lightdir, u_lightcolor, u_lightint, u_lightcount;
    GLint u_time;                       // время (ветер)
    GLint u_walk;                       // 1 = ходьба (малый ветер)
    GLint u_dyn;                        // 1 = динамический слой (только lod0/lod1)
    GLint u_shadowmap[SHADOW_CASCADES]; // каскады теней (растения принимают тени)
    GLint u_shadowmatrix[SHADOW_CASCADES];
    GLint u_shadowbias[SHADOW_CASCADES];
    GLint u_shadowdepth[SHADOW_CASCADES]; // сырая глубина (градиент тени)
    GLint u_shadowgrad[SHADOW_CASCADES];
    GLint u_shadow_on;
    CameraParams *cam;                  // матрицы вида/проекции
    SunParams *sun;                     // свет от узла Sun
} VegParams;

// Создаёт узел Vegetation: компилирует шейдер, грузит спрайты в 2D-массив,
// строит 4 VAO инстансинга, генерирует статичный слой на всю карту.
// NULL при ошибке. Прямоугольник поля (g_field_*) должен быть задан заранее.
SceneNode *veg_make_node(const WorldState *w, CameraParams *cam, SunParams *sun);

void veg_node_render(SceneNode *n, float dt);

// Для теневого прохода (shadow.c) и HUD: доступ к слоям растительности.
// q: 0=статика трава, 1=статика пшеница, 2=динамика трава, 3=динамика пшеница.
GLuint veg_vao(int q);
int veg_count(int q);
void veg_set_visible_test(int v);
GLuint veg_texture(void);

// Теневой проход: вся растительность приходит предпрореженной на CPU
// (статика — каждый 2-й экземпляр, динамика — правило uThin из
// shadowveg.vs) — карты теней идентичны прореживанию в шейдере, но
// вершинный этап обрабатывает вдвое меньше экземпляров.
GLuint veg_shadow_vao(int q);
int veg_shadow_count(int q);

// Текущее время симуляции (для ветра в теневом шейдере).
float veg_time(void);

// Освобождает CPU-массивы чанков и GL-ресурсы узла.
void veg_cleanup(SceneNode *n);

#endif

// Тестовые тумблеры замеров производительности (FR_TEST в main.c)
void veg_set_static_main(int v);
void veg_set_dyn_main(int v);

// Теневой проход: выборочная отрисовка статики по супер-чанкам и динамики
// по чанкам (только внутри бокса каскада) — без прогона всего буфера через
// вершинный шейдер. Диапазоны — в предпрореженных теневых буферах.
int veg_has_base_instance(void);
int veg_shadow_super_n(void);
int veg_shadow_super_cnt(int q, int s);
GLuint veg_shadow_super_base(int q, int s);
float veg_shadow_super_cx(int s);
float veg_shadow_super_cz(int s);
int veg_dyn_shadow_chunks(void);
void veg_dyn_shadow_chunk(int k, float *x, float *z,
                          GLuint *gbase, int *gcnt,
                          GLuint *wbase, int *wcnt);
