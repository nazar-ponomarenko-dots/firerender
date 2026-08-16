// ============================================================================
//  water.h — вода: лужи, озёра, реки, моря (MeshInstance3D с альфа-блендом)
// ----------------------------------------------------------------------------
//  Водоёмы описываются в map.json блоком waters[] (MapWater в mapdata.h).
//  Типы: лужа/озеро/море — круг (pos + radius; море с radius=0 накрывает
//  весь мир), река — осевая линия points + ширина русла width.
//
//  Поверхность — плоская сетка на уровне level; волны двигают вершины в
//  вершинном шейдере (мелкая сетка), нормали волн считаются аналитически
//  во фрагментном (поэтому волны гладкие при любом шаге сетки). Глубина
//  воды (уровень минус рельеф) читается из карты высот: альфа растёт с
//  глубиной, у берега — светлая кромка и пена.
//
//  Физика: water_surface_at/flow_at — для камеры (плавание, течение реки)
//  и растительности (уходит с погруженного дна). water_underwater — для
//  подводного оверлея экрана (renderer.c).
// ============================================================================
#ifndef WATER_H
#define WATER_H
#include "gl_loader.h"
#include "mapdata.h"
#include "render_ctx.h"
#include "scene.h"
#include "world.h"

typedef struct WaterInst {
    const MapWater *md;        // данные из map.json (живёт до mapdata_free)
    GLuint vao, vbo, ibo;      // плоская сетка на уровне воды
    int idx_count;
    float wave_len;            // длина волны, м (зависит от типа)
    float bbox_min[3], bbox_max[3];   // для фрустум-отсечения
} WaterInst;

typedef struct {
    const WorldState *world;
    const RendererContext *rc;
    SceneUniforms su;          // общий набор сцены (свет, туман, тени)
    GLuint prog;
    GLuint tex_height;         // карта высот (глубина воды, unit 2)
    // Свои uniform'ы
    GLint u_time, u_level, u_waveamp, u_wavespeed, u_wavelen;
    GLint u_color, u_alpha, u_depthfade;
    GLint u_flowdir, u_flowspeed, u_foam, u_edge;
    GLint u_heightmin, u_heightmax;
    GLint u_wetness;           // мокрость (дождь)
    GLint u_reflection;        // uReflection sampler
    WaterInst inst[MAP_MAX_WATERS];
    int n;
    float time;                // накапливается в update (для волн/потока)
} WaterParams;

// Создаёт узел Water: строит сетки всех водоёмов карты. NULL, если воды
// нет или шейдеры не собрались. После создания можно звать water_* ниже.
SceneNode *water_make_node(WorldState *w, const WorldTextures *wt,
                           const RendererContext *rc);
void water_node_update(SceneNode *n, float dt);
void water_node_render(SceneNode *n, float dt);
void water_cleanup(SceneNode *n);

// --- Запросы воды из кода (камера, растительность, HUD, Lua) -------------------
// Уровень поверхности самого высоко лежащего водоёма над точкой (x, z).
// Если ни один водоём точку не накрывает — возвращает -1e9. Рельеф НЕ
// учитывается: вызывающий сравнивает уровень с высотой сам (у берега
// рельеф выше уровня — воды там фактически нет).
float water_surface_at(const WorldState *w, float x, float z);

// Течение в точке (река): возвращает 1, если (x, z) внутри русла реки,
// и пишет в flow[2] = (вектор течения) * speed (м/с). 0 — течения нет.
int water_flow_at(const WorldState *w, float x, float z, float flow[2]);

// Глаза под водой: возвращает 1 (тон цвета водоёма в tint[3]). Для
// подводного оверлея экрана.
int water_underwater(const WorldState *w, const float pos[3], float tint[3]);

// Число водоёмов карты (для HUD).
int water_count(void);

#endif
