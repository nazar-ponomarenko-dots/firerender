// ============================================================================
//  objects.h — объекты из map/map.json (objects[]): биллборды с текстурой
// ----------------------------------------------------------------------------
//  Каждый объект — camera-facing квад с PNG-текстурой (model = путь к файлу,
//  напр. "img/tree.png"). Позиция — pos [x,y,z] из JSON; y <= 0 — высота
//  берётся из рельефа. Масштаб — scale (по умолчанию 1): высота = 5*scale.y.
//  Свет — все направленные источники + CSM-тени солнца + туман, как у
//  растительности.
// ============================================================================
#ifndef OBJECTS_H
#define OBJECTS_H
#include "gl_loader.h"
#include "render_ctx.h"
#include "scene.h"
#include "world.h"

// Создаёт узел Objects: грузит текстуры объектов из map.json, компилирует
// шейдер, строит общий квад. NULL при ошибке.
SceneNode *objects_make_node(const WorldState *w, const RendererContext *rc);

void objects_node_render(SceneNode *n, float dt);

// Коллизии объектов (collision=1): выталкивает точку (x,z) из цилиндров
// radius+pr вокруг баз объектов. Вызывать после движения игрока (не в полёте).
void objects_collide(float *x, float *z, float pr);

// Освобождает GL-ресурсы узла.
void objects_cleanup(SceneNode *n);

#endif
