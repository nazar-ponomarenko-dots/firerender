// ============================================================================
//  sun.c — солнце: замороженный направленный источник света
// ============================================================================
#include "sun.h"

#include "linalg.h"
#include "mapdata.h"

#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>

SceneNode *sun_make_node(void)
{
    SunParams *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    // По умолчанию: солнце на высоте ~39° (тени длинные), азимут чуть
    // восточнее юга. normalize(0.25, 0.6, 0.7). Если в map/map.json задан
    // направленный свет (lights[]) — берём направление/цвет/силу оттуда.
    p->dir[0] = 0.25f; p->dir[1] = 0.6f; p->dir[2] = 0.7f;
    vec3_normalize(p->dir);
    p->color[0] = 1.0f; p->color[1] = 0.95f; p->color[2] = 0.85f;
    p->intensity = 1.0f;
    // Тени по умолчанию: как прежние константы (4 текселя рельеф, 2 — трава)
    p->shadow_on = 1;
    p->shadow_bias = 4.0f;
    p->shadow_bias_veg = 2.0f;
    // Все направленные источники из JSON — солнце (0-й) и дополнительные
    // света. Массив идёт в шейдер массивом: каждый свет освещает отдельно.
    p->light_count = 0;
    for (int i = 0; i < mapdata_light_count() && p->light_count < SUN_MAX_LIGHTS; i++) {
        const MapLight *l = mapdata_light(i);
        if (l->type != LIGHT_DIRECTIONAL) continue;
        int k = p->light_count;
        memcpy(p->light_dir[k], l->dir, sizeof p->light_dir[k]);
        memcpy(p->light_color[k], l->color, sizeof p->light_color[k]);
        p->light_intensity[k] = l->intensity;
        vec3_normalize(p->light_dir[k]);
        p->light_count++;
        if (k == 0) {
            memcpy(p->dir, l->dir, sizeof p->dir);
            memcpy(p->color, l->color, sizeof p->color);
            p->intensity = l->intensity;
            p->shadow_on = l->shadow_on;
            p->shadow_bias = l->shadow_bias;
            p->shadow_bias_veg = l->shadow_bias_veg;
            vec3_normalize(p->dir);
            SDL_Log("Sun: '%s' dir=(%.2f %.2f %.2f) color=(%.2f %.2f %.2f) "
                    "int=%.2f shadows=%d bias=%.2f/%.2f",
                    l->name, p->dir[0], p->dir[1], p->dir[2],
                    p->color[0], p->color[1], p->color[2], p->intensity,
                    p->shadow_on, p->shadow_bias, p->shadow_bias_veg);
        }
    }
    SDL_Log("Lights total: %d (directional)", p->light_count);
    const float pos[3] = { 0, 100, 0 };        // высоко над миром
    const float size[3] = { 1, 1, 1 };
    SceneNode *node = node_new("Sun", "DirectionalLight3D", pos, size, p,
                               sun_node_update, NULL);
    if (!node) free(p);
    return node;
}

void sun_node_update(SceneNode *n, float dt)
{
    (void)n; (void)dt;
    // Солнце ЗАМОРОЖЕНО: направление задано один раз при инициализации
    // (sun->dir в sun_make_node). Чтобы снова запустить прогулку по небу —
    // раскомментировать строки ниже.
    // SunParams *p = n->params;
    // p->t += dt;
    // float a = p->t * 0.008f;              // медленно ходит по небу (~0.5 град/с)
    // p->dir[0] = sinf(a) * 0.5f;
    // p->dir[1] = 0.7f;
    // p->dir[2] = cosf(a) * 0.5f;
    // vec3_normalize(p->dir);
}
