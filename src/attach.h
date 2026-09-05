// ============================================================================
//  attach.h — крепление узлов к моделям (в т.ч. к КОСТЯМ скелета)
// ----------------------------------------------------------------------------
//  «Гибкость» 3D-моделей: к персонажу (или к любой модели/узлу) можно
//  прицепить что угодно — 2D-спрайт, статичный меш (.obj/.glb), другую
//  анимированную модель, источник эффектов. Ребёнок едет вместе с родителем,
//  а если задана КОСТЬ — вместе с этой костью (меч в руке, шляпа на голове,
//  иконка над головой).
//
//  Механика (см. scene.h): у родителя есть хук attach_fn, отдающий матрицу
//  точки крепления по индексу; у ребёнка — attach_slot (индекс) и флаг
//  attach_upright («не крутиться вместе с костью»). node_update_transforms
//  вставляет эту матрицу между мировой матрицей родителя и локальной
//  матрицей ребёнка. Сам ребёнок остаётся обычным узлом: его pos/rot/scl —
//  это СМЕЩЕНИЕ относительно точки крепления.
//
//  Слоты:
//    ATTACH_ORIGIN (-1)  — крепление к самому узлу (без кости);
//    >= 0                — индекс кости скелета (для AnimInstance3D).
//  Имя кости -> индекс: attach_bone_index (по имени узла из GLB).
// ============================================================================
#ifndef ATTACH_H
#define ATTACH_H
#include "render_ctx.h"
#include "scene.h"

#define ATTACH_ORIGIN (-1)

// Прикрепляет child к parent. bone — имя кости/узла модели (NULL или "" —
// крепление к началу координат родителя). offset — смещение от точки, м
// (может быть NULL). upright: 1 — гасить поворот кости (спрайт остаётся
// вертикальным), 0 — наследовать поворот полностью.
// Возвращает 0 при успехе, -1 если кость не найдена (child всё равно
// прикрепляется к origin — объект не теряется).
int attach_node(SceneNode *parent, SceneNode *child, const char *bone,
                const float offset[3], int upright);

// Открепляет узел: он остаётся в дереве, но точка крепления сбрасывается
// (child->attach_slot = ATTACH_ORIGIN).
void attach_detach(SceneNode *child);

// Индекс кости по имени в модели (AnimInstance3D). -1 — нет такой.
int attach_bone_index(SceneNode *model, const char *bone);

// Число костей у модели (0, если не скелетная).
int attach_bone_count(SceneNode *model);

// Имя кости по индексу (NULL, если нет).
const char *attach_bone_name(SceneNode *model, int idx);

// --- Спрайт как узел (Sprite3D) -----------------------------------------------
//  Квад с текстурой, который можно прицепить к чему угодно. Режимы:
//    billboard = 1 — всегда лицом к камере (иконки, метки, огоньки);
//    billboard = 0 — плоский квад в системе узла (наклейки, гербы, таблички).
//  Размер задаётся в метрах (w, h). Материал — обычный Material (material.h),
//  поэтому доступны блендинг, tint, альфа, анимация UV.
SceneNode *sprite_make_node(const char *tex_path, float w, float h,
                            int billboard, const RendererContext *rc);

// Настройка спрайта после создания (можно и на живом узле).
void sprite_set_size(SceneNode *n, float w, float h);
void sprite_set_tint(SceneNode *n, float r, float g, float b, float alpha);
void sprite_set_billboard(SceneNode *n, int on);
// Кадр из атласа: сетка cols x rows, index — номер кадра (слева-сверху).
void sprite_set_frame(SceneNode *n, int cols, int rows, int index);
// Свечение: 1 — аддитивный блендинг (искры, магия), 0 — обычная альфа.
void sprite_set_additive(SceneNode *n, int on);
// Без освещения: 1 — чистая текстура * tint (иконки, метки над головой).
void sprite_set_unlit(SceneNode *n, int on);

void sprite_node_render(SceneNode *n, float dt);

#endif
