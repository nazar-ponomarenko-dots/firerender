// ============================================================================
//  terrain.h — рельеф (MeshInstance3D): геометрия и материал земли
// ----------------------------------------------------------------------------
//  Рельеф из карты высот + материал img/ground.png с наштампованными
//  пучками травы (дальняя трава "вшита" в текстуру — дешёвый LOD).
//
//  Геометрия — ТАЙЛЫ 128x128 м, каждый в 4 LOD-уровнях (квадродерево с
//  упрощением: ровные участки склеиваются в большие квады, на склонах —
//  мелкие). LOD выбирается по дистанции тайла до игрока, тайлы за
//  экраном (frustum) не рисуются. Стыки уровней закрывают "юбки" —
//  геометрия без дыр. На дальние LOD приходится минимум полигонов.
// ============================================================================
#ifndef TERRAIN_H
#define TERRAIN_H
#include "gl_loader.h"
#include "mapdata.h"     // SPLAT_LAYERS
#include "render_ctx.h"
#include "scene.h"
#include "world.h"

// --- Тайл рельефа: 128x128 м, 4 LOD-уровня -----------------------------------
#define TERRAIN_LODS 4
#define TERRAIN_TILE 128.0f
typedef struct {
    float xmin, xmax, zmin, zmax;   // границы тайла по XZ (метры)
    float ymin, ymax;               // диапазон высот (ymin с запасом на юбку)
    GLuint  first[TERRAIN_LODS];    // первый индекс LOD'а в общем IBO
    GLsizei count[TERRAIN_LODS];    // сколько индексов у LOD'а
} TerrainTile;

typedef struct TerrainParams {
    const WorldState *world;        // данные мира: размеры, высоты, поле
    const RendererContext *rc;      // кадровые данные (renderer.c)
    SceneUniforms su;               // общий uniform-набор сцены
    GLuint prog, vao, vbo, ibo;      // программа и геометрия (с индексами)
    GLuint tex_base, tex_norm, tex_height;  // земля, нормали, высоты
    GLint u_specpow, u_specint;     // блики (земля матовая: spec = 0)
    GLint u_heightmin, u_heightmax, u_texscale;
    GLint u_statictex, u_static_on, u_worldsize;   // статические тени
    GLint u_baked_lm, u_baked_on;   // запечённый свет (RGB-люксель, unit 9)
    GLint u_sunvis, u_sunvis_on;    // карта видимости солнца (unit 16): дальние
                                    // статичные тени в режиме 1 (CSM)
    GLint u_wetness_mask;           // маска мокрости (unit 8)
    // Поверхности (splat): маска материалов + 4 слоя текстур
    GLuint tex_splat_layer[SPLAT_LAYERS];   // грунт/песок/щебень/асфальт
    GLint u_splat, u_splat_on, u_layer[SPLAT_LAYERS], u_layer_scale;
    GLint u_detail_dist;            // LOD деталей террейна (0 = выкл)
    GLint u_simple;                 // 1 = упрощённый рендер (циклы точек/прожекторов → 0)
    TerrainTile *tiles;             // массив тайлов (tiles_n штук)
    int tiles_n;
    int vert_count;                // индексов в геометрии (все тайлы и LOD)
} TerrainParams;

// Создаёт узел Terrain: строит сетку рельефа, материал земли (с травой),
// компилирует шейдеры и настраивает статичные uniform'ы. NULL при ошибке.
SceneNode *terrain_make_node(WorldState *w, const WorldTextures *wt,
                             const RendererContext *rc);

void terrain_node_render(SceneNode *n, float dt);

// Освобождает GL-ресурсы узла (программа, геометрия, текстура земли).
void terrain_cleanup(SceneNode *n);

// Сколько тайлов 128x128 м в текущем рельефе (для HUD). 0 — рельеф не создан.
int terrain_tile_count(void);

// --- Статические тени (режим r_shadows_mode 2) ---------------------------------
// Трассирует лучи от солнца по рельефу и объектам карты один раз и
// складывает результат в текстуру (небольшое разрешение на всю карту).
// Динамические CSM-тени при этом не рендерятся. Текстура живёт, пока жив
// рельеф; повторный вызов пересчитывает.
void terrain_shadows_bake(const WorldState *w, SunParams *sun);
// 1, если статическая карта теней готова (и её можно применять).
int terrain_shadows_have(void);

// --- Мокрость: запечённая маска (под крышей/нависающими объектами) -------
// Строит текстуру мокрости 0..1: луч вверх (0,1,0) до MAX_WETNESS_H или
// выхода за карту. Попал в объект (по цилиндрам ShadowObj) -> сухо (0),
// иначе мокрое (1). Шум + slope factor применяются в шейдере.
void terrain_wetness_mask_bake(const WorldState *w);
int terrain_wetness_mask_have(void);
GLuint terrain_wetness_mask_tex(void);
// Маска "открыто небу" для снега: та же запечённая текстура (RG8:
// R = открытое небо, G = под крышей).
int terrain_snow_mask_have(void);
GLuint terrain_snow_mask_tex(void);

// --- Запечённый свет (offline map-compilation, FR_BAKE / make mapbake) ----
// Считает освещение в 3D один раз офлайн и пишет в map/baked/:
//   lightmap.png  — RGB-люксель рельефа (солнце + прямой свет + полусферный
//                   ambient с AO по небу + 1 диффузный переотражённый бонс),
//                   ТОЛЬКО статические окклюдеры (динамика исключена);
//   probes.bin    — 3D-сетка зондов освещённости для ДИНАМИЧЕСКИХ объектов
//                   (ambient RGB + видимость солнца в точке).
// Движок в рантайме только ЧИТАЕТ (terrain_baked_light_load) и подставляет.
void terrain_light_bake(const WorldState *w, SunParams *sun,
                        const RendererContext *rc);
// Грузит map/baked/lightmap.png (+ probes.bin) при загрузке мира. 1 — успех.
int  terrain_baked_light_load(const WorldState *w);
// 1, если запечённый свет загружен и его можно применять (cvar r_baked_light).
int  terrain_baked_light_have(void);
// 1, если загружена карта видимости солнца (для дальних теней в режиме 1 CSM).
int  terrain_baked_sunvis_have(void);
// Освобождает GL-текстуру люкселя и сетку зондов (при выгрузке мира).
void terrain_baked_light_free(void);
// Трилинейная выборка ambient (out[0..2]) и видимости солнца (out[3]) из
// сетки зондов в мировой точке (x,y,z) — для освещения динамических объектов.
void terrain_baked_probe(float x, float y, float z, float out[4]);
// 1, если загружена именно сетка зондов (лайтмапа может быть и без неё).
int  terrain_baked_probes_have(void);

#endif
