// ============================================================================
//  terrain.h — рельеф (MeshInstance3D): геометрия и материал земли
// ----------------------------------------------------------------------------
//  Рельеф из карты высот + материал img/ground.png с наштампованными
//  пучками травы (дальняя трава "вшита" в текстуру — дешёвый LOD).
//  Рендер узла также запускает каскадные тени (shadow_render) и рисует
//  сам рельеф с псевдо-PBR освещением в стиле Half-Life 2.
// ============================================================================
#ifndef TERRAIN_H
#define TERRAIN_H
#include "gl_loader.h"
#include "render_ctx.h"
#include "scene.h"
#include "world.h"

typedef struct TerrainParams {
    const WorldState *world;        // данные мира: размеры, высоты, поле
    const RendererContext *rc;      // кадровые данные (renderer.c)
    SceneUniforms su;               // общий uniform-набор сцены
    GLuint prog, vao, vbo, ibo;      // программа и геометрия (с индексами)
    GLuint tex_base, tex_norm, tex_height;  // земля, нормали, высоты
    GLint u_specpow, u_specint;     // блики (земля матовая: spec = 0)
    GLint u_heightmin, u_heightmax, u_texscale;
    GLint u_statictex, u_static_on, u_worldsize;   // статические тени
    int vert_count;                // сколько вершин рисуем
} TerrainParams;

// Создаёт узел Terrain: строит сетку рельефа, материал земли (с травой),
// компилирует шейдеры и настраивает статичные uniform'ы. NULL при ошибке.
SceneNode *terrain_make_node(WorldState *w, const WorldTextures *wt,
                             const RendererContext *rc);

void terrain_node_render(SceneNode *n, float dt);

// Освобождает GL-ресурсы узла (программа, геометрия, текстура земли).
void terrain_cleanup(SceneNode *n);

// --- Статические тени (режим r_shadows_mode 2) ---------------------------------
// Трассирует лучи от солнца по рельефу и объектам карты один раз и
// складывает результат в текстуру (небольшое разрешение на всю карту).
// Динамические CSM-тени при этом не рендерятся. Текстура живёт, пока жив
// рельеф; повторный вызов пересчитывает.
void terrain_shadows_bake(const WorldState *w, SunParams *sun);
// 1, если статическая карта теней готова (и её можно применять).
int terrain_shadows_have(void);

#endif
