// ============================================================================
//  glb_anim.h — скелетная анимация (skinning) из бинарного glTF 2.0 (.glb)
// ----------------------------------------------------------------------------
//  Дополняет статичный glb.c анимацией: кожи (skins), весов/костей вершин
//  (JOINTS_0 / WEIGHTS_0) и клипов анимации (animations -> channels+samplers).
//
//  Отличие от glb.c: статичный путь ЗАПЕКАЕТ геометрию в мировые координаты,
//  что для анимации недопустимо — здесь геометрия остаётся в ПОЗЕ ПРИВЯЗКИ
//  (bind pose, локальные координаты меша), а повороты костей применяются
//  на GPU (скиннинг в шейдере skinned.vs через uBones).
//  Кости ужимаются per-примитив: каждый draw несёт лишь те jointMatrix, что
//  реально использует (≤64), поэтому модели с ЛЮБЫМ числом костей в коже
//  работают на всём GL 3.3 (гарантированный минимум — 64 кости на шейдер).
//
//  Правило рендера то же, что у glb.c: ПСЕВДО-PBR 2004 (материал material.h +
//  scalar-спекуляр), а не настоящий PBR.
// ============================================================================
#ifndef GLB_ANIM_H
#define GLB_ANIM_H
#include "gl_loader.h"
#include "scene.h"
#include "render_ctx.h"
#include "material.h"

// --- Узел иерархии glTF -------------------------------------------------------
typedef struct {
    int parent;                 // индекс родителя (-1 = корень)
    int children[64];
    int nchildren;
    char name[64];              // имя узла/кости из GLB (для attach по имени)
    float trs[16];              // статичный локальный трансформ (если задан матрицей)
    int mesh;                   // индекс меша (-1 — нет геометрии)
    // БАЗОВЫЕ (из файла) TRS-компоненты — анимация накладывается поверх них
    float btrans[3]; float bquat[4]; float bscl[3];
    int has_matrix;             // 1 — статичный трансформ задан матрицей
} AnimNode;

// --- Кожа ----------------------------------------------------------------------
typedef struct {
    int joint_count;
    int *joints;                // node-индексы
    float *inv_bind;            // 16 * joint_count (inverseBindMatrices)
} AnimSkin;

// --- Анимация ------------------------------------------------------------------
typedef enum { ANIM_LINEAR = 0, ANIM_STEP, ANIM_CUBIC } AnimInterp;

typedef struct {
    int node;                   // анимируемый узел
    int path;                   // 0=translation, 1=rotation, 2=scale
    int sampler;
} AnimChannel;

typedef struct {
    float *times; int nkeys;    // время ключей (сек)
    float *values;              // LINEaR/STEP: per key comp*count;
                                // CUBICSPLINE: per key 3*comp (in-tangent,
                                // value, out-tangent) — cm. sample_* в glb_anim.c
    int comp;                   // 3 (trans/scale) или 4 (rotation)
    AnimInterp interp;
} AnimSampler;

typedef struct {
    char name[64];                  // имя клипа (из GLB) — можно выбрать по имени
    AnimChannel *channels; int nch;
    AnimSampler *samplers; int ns;
    float duration;
} AnimClip;

// --- Геометрия одного меш-примитива (в bind pose, локальные координаты) -------
typedef struct {
    float *verts, *normals, *uvs;      // 3/3/2 на вершину
    int vcount;
    unsigned int *indices; int index_count;
    // скиннинг
    int *bone_idx;                     // 4 * vcount (JOINTS_0: ubyte/us-ort
    float *bone_w;                     // 4 * vcount (WEIGHTS_0)
    // материал (псевдо-PBR 2004)
    float tint[3]; float alpha; int blend; int cull;
    float tile[2]; float specular;
    unsigned char *img; int iw, ih;
    int wrap, filter;
} AnimPrim;

// Экземпляр (node, prim): каждый узел с мешем создаёт по draw-примитиву.
// Несколько узлов могут ссылаться на один и тот же примитив (instancing).
typedef struct { int node, prim, skin; } AnimInstance;

typedef struct {
    AnimPrim *prims; int nprim;
    AnimNode *nodes; int nnode;
    int *skin_nodes; int nskin_nodes;   // node-индексы со скином
    int *skin_of_node;                  // nnode: -1 или индекс кожи
    AnimSkin *skins; int nskin;
    AnimClip *clips; int nclip;
    AnimInstance *instances; int ninst;
    char err[128];
} AnimGlb;

// Парсит .glb с анимацией (CPU). Вернёт NULL при ошибке (err). Освобождение —
// glb_anim_free.
AnimGlb *glb_anim_parse(const char *path);

void glb_anim_free(AnimGlb *a);

// Создаёт узел AnimInstance3D (GPU-буферы/текстуры/программу для skinned.vs),
// позиция pos, масштаб scale. Узел НЕ привязан (node_add за вами).
// Если в модели нет скина/анимации — вернёт узел-статик (uHasSkin=0).
SceneNode *glb_anim_make_node(const char *path, const float pos[3], float scale,
                              const RendererContext *rc);

// Есть ли в модели скин+анимация (для команд, чтобы выбрать путь загрузки)?
// Быстрый peek в заголовок JSON, без полного парсинга.
int glb_anim_has_anim(const char *path);

// --- Клипы анимации (в GLB может быть несколько) --------------------------------
int glb_anim_clip_count(SceneNode *n);            // сколько клипов у узла
const char *glb_anim_clip_name(SceneNode *n, int idx); // имя клипа (NULL если нет)
int glb_anim_clip_find(SceneNode *n, const char *name); // индекс по имени, -1
void glb_anim_set_clip(SceneNode *n, int idx);    // переключить (время сбрасывается)
const char *glb_anim_current_clip(SceneNode *n);  // имя активного клипа (или NULL)
void glb_anim_set_speed(SceneNode *n, float speed); // множитель времени воспроизведения
void glb_anim_set_paused(SceneNode *n, int paused); // 1 — пауза, 0 — играть
void glb_anim_set_loop(SceneNode *n, int loop);     // 1 — зациклить, 0 — до конца и стоп
void glb_anim_set_time(SceneNode *n, float t);      // поставить время клипа (сек) — «с кадра»
float glb_anim_time(SceneNode *n);                  // текущее время активного клипа

// --- Реестр всех загруженных анимик-узлов (для консольного инструмента anim) ----
int glb_anim_count(void);                           // сколько анимик-узлов загружено
SceneNode *glb_anim_by_index(int idx);              // узел по индексу реестра (0..count-1)
const char *glb_anim_path(SceneNode *n);            // путь GLB (NULL у чужих узлов)
float glb_anim_scale_get(SceneNode *n);
int glb_anim_paused_get(SceneNode *n);
int glb_anim_loop_get(SceneNode *n);
float glb_anim_speed_get(SceneNode *n);
void glb_anim_set_enabled(SceneNode *n, int on);    // 0 — скрыть и заморозить, 1 — показать
int glb_anim_enabled(SceneNode *n);
void glb_anim_set_pos(SceneNode *n, const float pos[3]); // телепорт (мгновенно)
void glb_anim_move_to(SceneNode *n, const float pos[3], float speed); // плавное движение
void glb_anim_stop_move(SceneNode *n);
void glb_anim_set_scale(SceneNode *n, float s);     // масштаб модели
void glb_anim_remove_node(SceneNode *n);            // отвязать и выгрузить узел

// --- Точки крепления: кости скелета (см. attach.h) ------------------------------
//  Узел AnimInstance3D отдаёт мировые-относительно-себя матрицы костей через
//  хук scene.h (attach_fn). Индекс слота = индекс УЗЛА glTF (кости), поэтому
//  прицепить можно к любому узлу иерархии модели, не только к суставу кожи.
int glb_anim_bone_count(SceneNode *n);                 // 0 — не скелетная модель
const char *glb_anim_bone_name(SceneNode *n, int idx); // имя кости (или NULL)
int glb_anim_bone_find(SceneNode *n, const char *name);// индекс по имени, -1

void glb_anim_node_render(SceneNode *n, float dt);

#endif
