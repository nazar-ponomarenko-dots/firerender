// ============================================================================
//  minimap.h — мини-карта (CanvasItem2D)
// ----------------------------------------------------------------------------
//  Карта с точкой игрока и направлением взгляда + жёлтая линия направления
//  на солнце. Текстура — цвет карты из мира (world tex_base).
// ============================================================================
#ifndef MINIMAP_H
#define MINIMAP_H
#include "gl_loader.h"
#include "scene.h"
#include "camera.h"
#include "sun.h"
#include "world.h"

typedef struct {
    GLuint prog, vao, vbo;         // программа и буфер (данные каждый кадр)
    GLint u_basemap, u_color, u_usetex, u_usemarker;
    GLuint tex_base;               // текстура карты
    GLuint tex_marker;             // маркер игрока (img/player.png)
    const WorldState *world;       // размеры мира (позиция игрока в uv)
    CameraParams *cam;             // ссылка на узел Camera (позиция игрока)
    SunParams *sun;                // направление на солнце (маркер)
    int vw, vh;                    // размер окна в пикселях
    int visible;                   // клавиша G: показать/скрыть карту
} MinimapParams;

// Создаёт узел Minimap (компилирует шейдер, строит VAO). NULL при ошибке.
SceneNode *minimap_make_node(const WorldState *w, GLuint tex_base,
                             CameraParams *cam, SunParams *sun);

void minimap_node_render(SceneNode *n, float dt);

// Освобождает GL-ресурсы узла (текстура карты — общая, её не удаляем).
void minimap_cleanup(SceneNode *n);

#endif
