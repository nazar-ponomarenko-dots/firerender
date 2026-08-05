// ============================================================================
//  selection.h — выделение 3D-объектов: рамки-боксы вокруг узлов сцены
// ----------------------------------------------------------------------------
//  Дебаг-режим (клавиша G): вокруг каждого 3D-узла сцены рисуется его
//  ограничивающий бокс (pos ± size/2) — как выделение в редакторе Godot.
//  Показывает размеры/позиции Sun, Terrain, Vegetation, FieldArea, Camera.
//  Рисуется тем же шейдером, что рамка поля (shaders/field.*): позиция в
//  мире + uniform-цвет, только GL_LINES и 12 рёбер бокса на узел.
// ============================================================================
#ifndef SELECTION_H
#define SELECTION_H
#include "gl_loader.h"
#include "scene.h"
#include "camera.h"

typedef struct {
    GLuint prog, vao, vbo;         // программа и буфер рёбер (каждый кадр)
    GLint u_viewproj, u_color;
    CameraParams *cam;             // матрицы вида/проекции
    SceneNode *root;               // корень дерева: по нему ищем 3D-узлы
} SelParams;

// Создаёт узел Selection (CanvasItem2D-стиля нет: рисует в мире).
SceneNode *selection_make_node(SceneNode *root, CameraParams *cam);

void selection_node_render(SceneNode *n, float dt);

// Вкл/выкл рамок выделения (клавиша G, вместе с HUD).
void selection_set_on(int on);
int selection_is_on(void);

// Освобождает GL-ресурсы узла.
void selection_cleanup(SceneNode *n);

#endif
