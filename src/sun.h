// ============================================================================
//  sun.h — солнце (DirectionalLight3D)
// ----------------------------------------------------------------------------
//  Направленный свет: гуляет по небу, даёт свет и направление теней.
//  Направление задаётся один раз при создании узла (заморожено).
// ============================================================================
#ifndef SUN_H
#define SUN_H
#include "scene.h"

#define SUN_MAX_LIGHTS 4      // макс. направленных источников в шейдере

typedef struct {
    float dir[3];        // солнце (первый направленный свет): направление на свет
    float color[3];      // цвет
    float intensity;     // сила
    float t;             // время (для прогулки по небу)
    int shadow_on;       // солнце отбрасывает тени? (из map/map.json)
    float shadow_bias;   // bias теней рельефа, в латеральных текселях
    float shadow_bias_veg; // bias теней растительности, в текселях
    // ВСЕ направленные источники из map.json: массив приходит
    // в отдельные uniform'ы, индекс 0 = солнце.
    int light_count;
    float light_dir[SUN_MAX_LIGHTS][3];
    float light_color[SUN_MAX_LIGHTS][3];
    float light_intensity[SUN_MAX_LIGHTS];
} SunParams;

// Создаёт узел Sun со стандартным направлением (~39° над горизонтом).
SceneNode *sun_make_node(void);

void sun_node_update(SceneNode *n, float dt);

#endif
