// ============================================================================
//  playerbox.h — бокс коллизии персонажа в режиме от 3-го лица
// ----------------------------------------------------------------------------
//  Персонаж в 3-м лице рисуется как полный бокс его физической коллизии
//  (ширина 1 м, высота 2 м — см. camera.h): полупрозрачная закраска граней
//  + жёлтые рёбра. В 1-м лице и в теневом проходе узел ничего не рисует.
// ============================================================================
#ifndef PLAYERBOX_H
#define PLAYERBOX_H
#include "gl_loader.h"
#include "render_ctx.h"
#include "scene.h"
#include "camera.h"

// Параметры узла. cam — физическое состояние персонажа (позиция/взгляд).
typedef struct {
    const RendererContext *rc;   // видовой базис, вид/проекция
    CameraParams *cam;           // персонаж (позиция, yaw, присед)
    GLuint prog;                 // шейдер бокса (позиция в мире + цвет)
    GLint u_viewproj, u_color;
    GLuint vao_fill, vbo_fill;   // полупрозрачные грани
    GLuint vao_line, vbo_line;   // рёбра бокса
} PlayerBoxParams;

// Создаёт узел PlayerBox. cam — параметры узла Camera (живой указатель).
SceneNode *playerbox_make_node(CameraParams *cam, const RendererContext *rc);

void playerbox_node_render(SceneNode *n, float dt);
void playerbox_cleanup(SceneNode *n);

#endif
