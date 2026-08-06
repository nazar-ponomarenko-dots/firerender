// ============================================================================
//  decals.h — декали на рельефе (проецируемые квады с текстурами)
// ----------------------------------------------------------------------------
//  Декаль — плоский квад, «лежащий» на рельефе: высота каждого угла берётся
//  из карты высот (имитация проекции на поверхность). Текстура — спрайт
//  (например, следы, кровь, тень). Альфа-бленд, смещение вверх на 0.1 м от
//  рельефа — без z-fighting. Один draw на декаль (их немного).
//
//  Использование:
//    decals_make_node(w, rc)  — узел сцены (во время загрузки мира);
//    decals_place(~)          — добавить декаль;
//    decals_clear()           — сбросить (смена карты);
//    decals_cleanup(node)     — освободить GL.
// ============================================================================
#ifndef DECALS_H
#define DECALS_H
#include "gl_loader.h"
#include "render_ctx.h"
#include "scene.h"
#include "world.h"

#define DECALS_MAX 64

// Добавляет декаль: центр (x,y,z), размер (полуширина/полувысота), поворот
// вокруг вертикали (рад), текстура (path), цвет/интенсивность (0..1).
void decal_place(float x, float z, float half_w, float half_d, float rot,
                 const char *tex_path, float r, float g, float b, float alpha);

// Создаёт узел Decals (квады на рельефе). NULL при ошибке.
SceneNode *decals_make_node(const WorldState *w, const RendererContext *rc);

// Сбрасывает все декали (смена карты / выгрузка мира).
void decals_clear(void);

// Рендер всех декалей (после terrain, до объектов). Один draw per decal.
void decals_draw(const RendererContext *rc);

// Освобождает GL-ресурсы узла.
void decals_cleanup(SceneNode *n);

#endif