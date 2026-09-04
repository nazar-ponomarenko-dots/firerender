// ============================================================================
//  scene.h — сцена в духе Godot: всё на сцене — Узел
// ----------------------------------------------------------------------------
//  Мини-дерево узлов. У каждого узла: имя, тип, ТРАНСФОРМ (позиция, поворот,
//  масштаб), размер и параметры. Движок каждый кадр обходит дерево:
//  сначала update() всех узлов (логика, ввод, физика), затем считает мировые
//  матрицы (node_update_transforms), потом render() (отрисовка).
//
//  ТРАНСФОРМ (как Node3D в Godot): pos + rot (радианы, порядок Y->X->Z) +
//  scl. Мировая матрица world = world(родителя) * [точка крепления] *
//  local(TRS) — её считает node_update_transforms и читают рендеры мешей
//  (mesh/glb/glb_anim/sprite). Раньше каждый рендер собирал матрицу сам из
//  pos и своего скаляра scale — повернуть или прицепить узел было нельзя.
//
//  ТОЧКИ КРЕПЛЕНИЯ (attachment points): родитель может отдать ребёнку
//  дополнительную матрицу — например матрицу КОСТИ скелета. Тогда ребёнок
//  (спрайт, меш, другая модель) едет вместе с рукой персонажа. Родитель
//  выставляет attach_fn, ребёнок — attach_slot (индекс точки; -1 = нет).
//  Реализацию хука для скелетных моделей даёт glb_anim.c, удобный API —
//  attach.h.
// ============================================================================
#ifndef SCENE_H
#define SCENE_H

typedef struct SceneNode SceneNode;
typedef void (*NodeFn)(SceneNode *node, float dt);

// Хук родителя: положить в out[16] мировую-относительно-родителя матрицу
// точки крепления slot (например кости скелета). Если точки нет — оставить
// единичную матрицу (out уже единичный на входе).
typedef void (*NodeAttachFn)(SceneNode *parent, int slot, float out[16]);

struct SceneNode {
    const char *name;          // имя узла, как в Godot ("Sun", "Terrain"…)
    const char *type;          // тип ("DirectionalLight3D", "MeshInstance3D"…)
    SceneNode *parent;         // родитель (у корня — NULL)
    SceneNode *first_child;    // первый ребёнок
    SceneNode *last_child;     // последний ребёнок (для добавления в конец)
    SceneNode *next;           // следующий брат
    float pos[3];              // положение (в системе родителя)
    float rot[3];              // поворот, радианы (Y-yaw, X-pitch, Z-roll)
    float scl[3];              // масштаб (по умолчанию 1,1,1)
    float world[16];           // мировая матрица (node_update_transforms)
    float size[3];             // размер узла (у кого он есть: мир, мини-карта…)
    void *params;              // типизированные параметры узла
    NodeFn update;             // вызывается каждый кадр до render
    NodeFn render;             // вызывается каждый кадр (рисует себя)
    // Точки крепления
    NodeAttachFn attach_fn;    // РОДИТЕЛЬ: выдаёт матрицу точки по индексу
    int attach_slot;           // РЕБЁНОК: какую точку родителя занимает (-1 нет)
    int attach_upright;        // РЕБЁНОК: 1 — гасить поворот точки (только позиция)
};

SceneNode *node_new(const char *name, const char *type,
                    const float *pos, const float *size,
                    void *params, NodeFn update, NodeFn render);
void node_add(SceneNode *parent, SceneNode *child);

// Отвязать узел от родителя (сам узел и его params не освобождаются).
// Удобно перед node_free_tree, чтобы не освобождать узел дважды.
void node_remove(SceneNode *node);

// Найти узел по имени в поддереве (DFS). NULL — не найден.
SceneNode *node_find(SceneNode *root, const char *name);

void node_update_tree(SceneNode *n, float dt);

// Пересчитывает мировые матрицы всего поддерева (после update, до render).
// Вызывать для корня один раз за кадр.
void node_update_transforms(SceneNode *n);

// Локальная матрица узла из pos/rot/scl (для отладки и ручных расчётов).
void node_local_matrix(const SceneNode *n, float out[16]);

// Мировая позиция узла из его мировой матрицы.
void node_world_pos(const SceneNode *n, float out[3]);

void node_render_tree(SceneNode *n, float dt);
void node_free_tree(SceneNode *n);
void scene_print(SceneNode *n, int depth);

#endif
