// ============================================================================
//  decals.h — декали на рельефе (проецируемые квады с текстурами)
// ----------------------------------------------------------------------------
//  Декаль — плоский квад, «лежащий» на рельефе: высота каждого угла берётся
//  из карты высот (9 сэмплов на декаль — 3x3) для плавного следования за
//  склонами. Полигон-оффсет (glPolygonOffset) вместо ручного смещения —
//  без z-fighting и парения.
//  API:
//    decal_place(x, z, hw, hd, rot, tex, r,g,b,a)      — offset=0.03f
//    decal_place_ex(x, z, hw, hd, rot, tex, r,g,b,a, off)
//    decals_clear()
// ============================================================================
#ifndef DECALS_H
#define DECALS_H

#include "gl_loader.h"
#include "render_ctx.h"
#include "scene.h"
#include "world.h"

#define DECALS_MAX 256

// Добавляет декаль на рельеф (offset=0.03f — малое прилегание).
void decal_place(float x, float z, float half_w, float half_d, float rot,
                 const char *tex_path, float r, float g, float b, float alpha);

// То же, но с явным offset (м) от рельефа (0.0f = вплотную).
void decal_place_ex(float x, float z, float half_w, float half_d, float rot,
                    const char *tex_path, float r, float g, float b,
                    float alpha, float offset);

// Создаёт узел Decals (квады на рельефе). NULL при ошибке.
SceneNode *decals_make_node(const WorldState *w, const RendererContext *rc);

// Сбрасывает все декали (смена карты / выгрузка мира).
void decals_clear(void);

// Рендер всех декалей (после terrain, до объектов). Один draw per decal.
void decals_draw(const RendererContext *rc);

// Освобождает GL-ресурсы узла.
void decals_cleanup(SceneNode *n);

#endif