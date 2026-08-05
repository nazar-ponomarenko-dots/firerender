// ============================================================================
//  scene.h — сцена в духе Godot: всё на сцене — Узел
// ----------------------------------------------------------------------------
//  Мини-дерево узлов. У каждого узла: имя, тип, положение, размер и параметры.
//  Движок каждый кадр обходит дерево: сначала update() всех узлов (логика,
//  ввод, физика), потом render() (отрисовка). Родители и дети — как в Godot.
// ============================================================================
#ifndef SCENE_H
#define SCENE_H

typedef struct SceneNode SceneNode;
typedef void (*NodeFn)(SceneNode *node, float dt);

struct SceneNode {
    const char *name;          // имя узла, как в Godot ("Sun", "Terrain"…)
    const char *type;          // тип ("DirectionalLight3D", "MeshInstance3D"…)
    SceneNode *parent;         // родитель (у корня — NULL)
    SceneNode *first_child;    // первый ребёнок
    SceneNode *last_child;     // последний ребёнок (для добавления в конец)
    SceneNode *next;           // следующий брат
    float pos[3];              // положение в мире
    float size[3];             // размер узла (у кого он есть: мир, мини-карта…)
    void *params;              // типизированные параметры узла
    NodeFn update;             // вызывается каждый кадр до render
    NodeFn render;             // вызывается каждый кадр (рисует себя)
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
void node_render_tree(SceneNode *n, float dt);
void node_free_tree(SceneNode *n);
void scene_print(SceneNode *n, int depth);

#endif
