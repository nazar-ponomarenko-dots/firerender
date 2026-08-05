// ============================================================================
//  field.h — поле пшеницы (FieldArea3D)
// ----------------------------------------------------------------------------
//  Визуально — рамка на земле (5 вершин замкнутой линии, высота берётся из
//  рельефа). Задаёт, где растёт пшеница: координаты узла = центр, size =
//  размер. Прямоугольник мира (g_field_*) main.c задаёт до создания узлов.
// ============================================================================
#ifndef FIELD_H
#define FIELD_H
#include "gl_loader.h"
#include "render_ctx.h"
#include "scene.h"
#include "world.h"

typedef struct {
    GLuint prog, vao, vbo;         // линия-прямоугольник
    GLint u_viewproj, u_color;
    const WorldState *world;       // высоты рельефа (рамка ложится на землю)
    const RendererContext *rc;     // матрицы вида/проекции
} FieldParams;

// Создаёт узел FieldArea: центр и размер поля, компилирует шейдер.
// center/size должны совпадать с g_field_* (глобалы мира).
SceneNode *field_make_node(const WorldState *w, const float *center,
                           const float *size, const RendererContext *rc);

void field_node_render(SceneNode *n, float dt);

// Освобождает GL-ресурсы узла.
void field_cleanup(SceneNode *n);

#endif
