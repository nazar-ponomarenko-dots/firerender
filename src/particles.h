// ============================================================================
//  particles.h — система частиц (instanced-спрайты, CPU-обновление)
// ----------------------------------------------------------------------------
//  Пул частиц фиксированного размера. Каждая частица — биллборд-квад с
//  текстурой (спрайт), аддитивный или альфа-бленд. Обновление — в тике
//  сцены (particles_update с dt из фиксированного шага 100 Гц), рендер —
//  ОДИН instanced draw call на весь пул (uParticles[] array texture-free,
//  per-instance buffer: pos, size, color, alpha).
//
//  Использование:
//    particles_make_node(rc)      — узел сцены (создаётся при загрузке мира)
//    particles_emit(..)           — спавн burst'а частиц
//    particles_clear()            — сбросить пул (смена карты)
//    particles_cleanup(node)      — освободить GL
// ============================================================================
#ifndef PARTICLES_H
#define PARTICLES_H
#include "gl_loader.h"
#include "render_ctx.h"
#include "scene.h"
#include "world.h"

#define PARTICLES_MAX 512          // размер пула (instancing: 1 draw call)

// Эмитит burst частиц из точки. dir — базовое направление (может быть 0),
// spread — разброс (рад), speed — скорость, life — жизнь (с), size — размер
// квада (м), r/g/b — цвет 0..1, gravity — м/с2 (0 для дыма/искр).
void particles_emit(float x, float y, float z,
                    const float *dir, float spread, float speed, float life,
                    float size, float r, float g, float b, float gravity);

// count частиц из одной точки (burst) — то же самое, но пачкой.
void particles_burst(float x, float y, float z,
                     const float *dir, float spread, float speed, float life,
                     float size, float r, float g, float b, float gravity,
                     int count);

// Обновляет все живые частицы (dt — фиксированный шаг). Вызывать из
// game_update или update узла.
void particles_update(float dt);

// Рендер всех живых частиц (один instanced draw). Вызывать после terrain
// (частицы летят над рельефом), в цветном проходе.
void particles_draw(const RendererContext *rc);

// Создаёт узел Particles (биллборд-спрайты). NULL при ошибке.
SceneNode *particles_make_node(const RendererContext *rc);

// Сбрасывает пул (при смене карты / выгрузке мира).
void particles_clear(void);

// Освобождает GL-ресурсы узла.
void particles_cleanup(SceneNode *n);

#endif
