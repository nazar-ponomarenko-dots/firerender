// ============================================================================
//  water.c — вода: лужи, озёра, реки, моря (см. water.h)
// ----------------------------------------------------------------------------
//  Геометрия: круглые водоёмы — кольцевая сетка (ровный край у берега),
//  море без радиуса — сетка на весь мир, река — лента вдоль осевой линии
//  (митеринг на стыках сегментов). Поверхность плоская на уровне level;
//  волны двигают вершины в вершинном шейдере, нормали считаются во
//  фрагментном конечными разностями той же функции (гладкие при любой сетке).
// ============================================================================
#include "water.h"

#include "cvars.h"
#include "frustum.h"
#include "mapdata.h"
#include "renderer.h"
#include "resources.h"
#include "shader.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static WaterParams *s_water = NULL;   // для hot-reload (water_reshade)

// Длина волны по типу (шаг сетки в 3..4 раза мельче)
static float water_wavelen(const MapWater *m)
{
    switch (m->type) {
    case WATER_PUDDLE: return 1.5f;
    case WATER_LAKE:   return 6.0f;
    case WATER_RIVER:  return 3.0f;
    case WATER_SEA:    return 14.0f;
    }
    return 6.0f;
}

// Ширина кромки (полоса, где альфа сходит на нет): лужа — узкая, море — нет
static float water_edge_fade(const MapWater *m)
{
    switch (m->type) {
    case WATER_PUDDLE: return 1.5f;
    case WATER_LAKE:   return 3.0f;
    case WATER_RIVER: {
        float hw = m->width * 0.5f;
        float e = hw * 0.6f;
        return e < 0.5f ? 0.5f : (e > 4.0f ? 4.0f : e);
    }
    case WATER_SEA:
        return m->radius > 0.0f ? 3.0f : 1e6f;  // море без края
    }
    return 2.0f;
}

// --- Общие CPU-запросы (камера, растительность, рендер) -------------------------
// Накрывает ли водоём точку (x, z)? Река — по расстоянию до осевой линии,
// круг — по радиусу, море без радиуса — весь мир.
static int water_covers(const MapWater *m, float x, float z)
{
    if (m->type == WATER_RIVER) {
        if (m->point_n < 2) return 0;
        float hw = m->width * 0.5f;
        // расстояние до ЛЕНТЫ: до ближайшего отрезка осевой линии
        float best = 1e30f;
        for (int i = 0; i + 1 < m->point_n; i++) {
            float ax = m->points[i][0], az = m->points[i][1];
            float bx = m->points[i + 1][0], bz = m->points[i + 1][1];
            float dx = bx - ax, dz = bz - az;
            float l2 = dx * dx + dz * dz;
            float t = l2 > 1e-6f ?
                ((x - ax) * dx + (z - az) * dz) / l2 : 0.0f;
            if (t < 0.0f) t = 0.0f;
            if (t > 1.0f) t = 1.0f;
            float px = ax + dx * t - x;
            float pz = az + dz * t - z;
            float d2 = px * px + pz * pz;
            if (d2 < best) best = d2;
        }
        return best <= hw * hw;
    }
    if (m->type == WATER_SEA && m->radius <= 0.0f) return 1;
    float r = m->radius;
    if (r <= 0.0f) return 0;
    float dx = x - m->center[0], dz = z - m->center[1];
    return dx * dx + dz * dz <= r * r;
}

float water_surface_at(const WorldState *w, float x, float z)
{
    float best = -1e9f;
    for (int i = 0; i < mapdata_water_count(); i++) {
        const MapWater *m = mapdata_water(i);
        if (!m || !water_covers(m, x, z)) continue;
        // Вода есть только там, где рельеф ПОД уровнем: берег (рельеф
        // выше уровня) маскируем, чтобы плавание/растения не «застревали»
        // в пересохшей кромке.
        if (sample_height(w, x, z) >= m->level) continue;
        if (m->level > best) best = m->level;
    }
    return best;
}

int water_flow_at(const WorldState *w, float x, float z, float flow[2])
{
    for (int i = 0; i < mapdata_water_count(); i++) {
        const MapWater *m = mapdata_water(i);
        if (!m || m->type != WATER_RIVER) continue;
        if (!water_covers(m, x, z)) continue;
        if (sample_height(w, x, z) >= m->level) continue;
        float fx = m->flow[0], fz = m->flow[1];
        float l = sqrtf(fx * fx + fz * fz);
        if (l < 1e-4f) continue;
        flow[0] = fx / l * m->flow_speed;
        flow[1] = fz / l * m->flow_speed;
        return 1;
    }
    return 0;
}

int water_underwater(const WorldState *w, const float pos[3], float tint[3])
{
    float best = -1e9f;
    const MapWater *bestw = NULL;
    for (int i = 0; i < mapdata_water_count(); i++) {
        const MapWater *m = mapdata_water(i);
        if (!m || !water_covers(m, pos[0], pos[2])) continue;
        if (sample_height(w, pos[0], pos[2]) >= m->level) continue;
        if (m->level > best) { best = m->level; bestw = m; }
    }
    if (bestw && pos[1] < best - 0.05f) {
        tint[0] = bestw->color[0];
        tint[1] = bestw->color[1];
        tint[2] = bestw->color[2];
        return 1;
    }
    return 0;
}

int water_count(void)
{
    return mapdata_water_count();
}

// --- Геометрия ------------------------------------------------------------------
// Круглый водоём: центр + кольца + ровный обод. aEdge — расстояние до края
// (для мягкой кромки во фрагментном шейдере). UV = мировые/4 (тайл волн).
static int build_disc(const MapWater *m, float **verts, GLuint **idx,
                      int *vert_n, int *idx_n, float step)
{
    float r = m->radius;
    int nr = (int)ceilf(r / step);
    if (nr < 2) nr = 2;
    if (nr > 96) nr = 96;
    float dr = r / nr;
    int nseg = (int)(6.2831853f * r / step);
    if (nseg < 12) nseg = 12;
    if (nseg > 256) nseg = 256;

    int vn = 1 + nr * nseg;
    float *v = malloc((size_t)vn * 5 * sizeof(float));
    GLuint *q = malloc((size_t)(nseg * (nr - 1) * 2 + nseg) * 3 * sizeof(GLuint)
                       + (size_t)nseg * 3 * sizeof(GLuint));
    if (!v || !q) { free(v); free(q); return -1; }
    float cx = m->center[0], cz = m->center[1];

    // центр
    v[0] = cx; v[1] = cz; v[2] = cx / 4.0f; v[3] = cz / 4.0f; v[4] = r;
    int vi = 1;
    for (int ring = 1; ring <= nr; ring++) {
        float rr = (ring == nr) ? r : dr * ring;    // обод ровно на радиусе
        for (int s = 0; s < nseg; s++) {
            float th = 6.2831853f * s / nseg;
            float x = cx + cosf(th) * rr;
            float z = cz + sinf(th) * rr;
            float *p = v + (size_t)vi * 5;
            p[0] = x; p[1] = z;
            p[2] = x / 4.0f; p[3] = z / 4.0f;
            p[4] = r - rr;                          // 0 у берега
            vi++;
        }
    }
    int qi = 0;
    // веер от центра к первому кольцу (1..nseg)
    for (int s = 0; s < nseg; s++) {
        q[qi++] = 0;
        q[qi++] = (GLuint)(1 + s);
        q[qi++] = (GLuint)(1 + (s + 1) % nseg);
    }
    // ленты между кольцами (ring 1..nr-1 -> ring+1)
    for (int ring = 1; ring < nr; ring++) {
        int a0 = 1 + (ring - 1) * nseg;
        int b0 = 1 + ring * nseg;
        for (int s = 0; s < nseg; s++) {
            int s1 = (s + 1) % nseg;
            q[qi++] = (GLuint)(a0 + s);
            q[qi++] = (GLuint)(b0 + s);
            q[qi++] = (GLuint)(a0 + s1);
            q[qi++] = (GLuint)(b0 + s);
            q[qi++] = (GLuint)(b0 + s1);
            q[qi++] = (GLuint)(a0 + s1);
        }
    }
    *verts = v; *idx = q;
    *vert_n = vn; *idx_n = qi;
    return 0;
}

// Море на весь мир: прямоугольная сетка (волны двигают вершины).
static int build_sea_grid(const WorldState *w, float **verts, GLuint **idx,
                          int *vert_n, int *idx_n, float step)
{
    int nx = (int)ceilf(w->world_w / step);
    int nz = (int)ceilf(w->world_d / step);
    if (nx < 4) nx = 4;
    if (nz < 4) nz = 4;
    int vn = (nx + 1) * (nz + 1);
    float *v = malloc((size_t)vn * 5 * sizeof(float));
    GLuint *q = malloc((size_t)nx * nz * 6 * sizeof(GLuint));
    if (!v || !q) { free(v); free(q); return -1; }
    int vi = 0;
    for (int j = 0; j <= nz; j++) {
        for (int i = 0; i <= nx; i++) {
            float x = i * w->world_w / nx;
            float z = j * w->world_d / nz;
            float *p = v + (size_t)vi * 5;
            p[0] = x; p[1] = z;
            p[2] = x / 4.0f; p[3] = z / 4.0f;
            p[4] = 1e6f;                            // края нет
            vi++;
        }
    }
    int qi = 0;
    for (int j = 0; j < nz; j++) {
        for (int i = 0; i < nx; i++) {
            GLuint a = (GLuint)(j * (nx + 1) + i);
            GLuint b = (GLuint)((j + 1) * (nx + 1) + i);
            GLuint c = a + 1, d = b + 1;
            q[qi++] = a; q[qi++] = b; q[qi++] = c;
            q[qi++] = b; q[qi++] = d; q[qi++] = c;
        }
    }
    *verts = v; *idx = q;
    *vert_n = vn; *idx_n = qi;
    return 0;
}

// Река: лента вдоль осевой линии, ширина width. Митеринг стыков — нормаль
// усредняется между соседними сегментами. aUV.x = дистанция вдоль русла.
static int build_river(const MapWater *m, float **verts, GLuint **idx,
                       int *vert_n, int *idx_n)
{
    int np = m->point_n;
    if (np < 2) return -1;
    float hw = m->width * 0.5f;
    int vn = np * 2;
    float *v = malloc((size_t)vn * 5 * sizeof(float));
    GLuint *q = malloc((size_t)(np - 1) * 6 * sizeof(GLuint));
    if (!v || !q) { free(v); free(q); return -1; }

    float along = 0.0f;
    for (int i = 0; i < np; i++) {
        // направление в точке: среднее соседних сегментов
        float dx = 0, dz = 0;
        if (i > 0) {
            dx += m->points[i][0] - m->points[i - 1][0];
            dz += m->points[i][1] - m->points[i - 1][1];
        }
        if (i + 1 < np) {
            dx += m->points[i + 1][0] - m->points[i][0];
            dz += m->points[i + 1][1] - m->points[i][1];
        }
        float l = sqrtf(dx * dx + dz * dz);
        if (l < 1e-6f) { dx = 1.0f; dz = 0.0f; l = 1.0f; }
        dx /= l; dz /= l;
        if (i > 0) {
            float sx = m->points[i][0] - m->points[i - 1][0];
            float sz = m->points[i][1] - m->points[i - 1][1];
            along += sqrtf(sx * sx + sz * sz);
        }
        // нормаль в плоскости XZ: (-dz, dx) / (dz, -dx)
        for (int side = 0; side < 2; side++) {
            float sgn = side ? 1.0f : -1.0f;
            float *p = v + (size_t)(i * 2 + side) * 5;
            float nx = -dz * sgn, nz = dx * sgn;
            p[0] = m->points[i][0] + nx * hw;
            p[1] = m->points[i][1] + nz * hw;
            p[2] = along / 4.0f;
            p[3] = 0.0f;
            p[4] = 0.0f;                            // у берега
        }
        v[(size_t)i * 10 + 3] = -hw / 4.0f;         // левый борт
        v[(size_t)i * 10 + 8] =  hw / 4.0f;         // правый борт
    }
    int qi = 0;
    for (int i = 0; i + 1 < np; i++) {
        int a = i * 2, b = (i + 1) * 2;
        q[qi++] = (GLuint)a;
        q[qi++] = (GLuint)b;
        q[qi++] = (GLuint)(a + 1);
        q[qi++] = (GLuint)(a + 1);
        q[qi++] = (GLuint)b;
        q[qi++] = (GLuint)(b + 1);
    }
    *verts = v; *idx = q;
    *vert_n = vn; *idx_n = qi;
    return 0;
}

static GLuint upload_geometry(const float *verts, int vert_n,
                              const GLuint *idx, int idx_n,
                              GLuint *vbo, GLuint *ibo)
{
    GLuint vao;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, vbo);
    glBindBuffer(GL_ARRAY_BUFFER, *vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)((size_t)vert_n * 5 * sizeof(float)),
                 verts, GL_STATIC_DRAW);
    glGenBuffers(1, ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, *ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)((size_t)idx_n * sizeof(GLuint)),
                 idx, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), NULL);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float),
                          (const void *)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, 5 * sizeof(float),
                          (const void *)(4 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glBindVertexArray(0);
    return vao;
}

// --- Uniform'ы ------------------------------------------------------------------
// Проги создаются СНАРУЖИ (make_node / reshade): здесь только локации и
// статичные значения для текущего p->prog.
static void water_prog_init(WaterParams *p, const WorldState *w)
{
    GLuint prog = p->prog;
    scene_uniforms_init(&p->su, prog);
    p->u_time = glGetUniformLocation(prog, "uTime");
    p->u_level = glGetUniformLocation(prog, "uLevel");
    p->u_waveamp = glGetUniformLocation(prog, "uWaveAmp");
    p->u_wavespeed = glGetUniformLocation(prog, "uWaveSpeed");
    p->u_wavelen = glGetUniformLocation(prog, "uWaveLen");
    p->u_color = glGetUniformLocation(prog, "uWaterColor");
    p->u_alpha = glGetUniformLocation(prog, "uAlpha");
    p->u_depthfade = glGetUniformLocation(prog, "uDepthFade");
    p->u_flowdir = glGetUniformLocation(prog, "uFlowDir");
    p->u_flowspeed = glGetUniformLocation(prog, "uFlowSpeed");
    p->u_foam = glGetUniformLocation(prog, "uFoam");
    p->u_edge = glGetUniformLocation(prog, "uEdgeFade");
    p->u_heightmin = glGetUniformLocation(prog, "uHeightMin");
    p->u_heightmax = glGetUniformLocation(prog, "uHeightMax");
    p->u_wetness = glGetUniformLocation(prog, "uWetness");
    p->u_reflection = glGetUniformLocation(prog, "uReflCube");

    glUseProgram(prog);
    glUniform1i(glGetUniformLocation(prog, "uHeightmap"), 2);
    glUniform1i(glGetUniformLocation(prog, "uReflCube"), 7);  // unit 7
    glUniform2f(glGetUniformLocation(prog, "uWorldSize"),
                w->world_w, w->world_d);
    glUniform1f(p->u_heightmin, w->min_height);
    glUniform1f(p->u_heightmax, w->max_height);
    glUniform1f(p->u_edge, 2.0f);        // переопределяется на инстанс
    // Каскады теней на юнитах 3..6 (как в рельефе)
    for (int c = 0; c < SHADOW_CASCADES; c++) {
        if (p->su.u_shadowmap[c] >= 0) glUniform1i(p->su.u_shadowmap[c], 3 + c);
        if (p->su.u_shadowdepth[c] >= 0) glUniform1i(p->su.u_shadowdepth[c], 3 + c);
    }
    glUseProgram(0);
}

// Hot-reload: пересоздать программу и uniform'ы (паттерн terrain_reshade).
static void water_reshade(void)
{
    if (!s_water) return;
    GLuint prog = resources_program_replace(s_water->prog,
                                            "shaders/water.vs",
                                            "shaders/water.fs");
    if (prog) s_water->prog = prog;
    else SDL_Log("water: reshade failed, keeping old program");
    water_prog_init(s_water, s_water->world);
}

// --- Узел -----------------------------------------------------------------------
SceneNode *water_make_node(WorldState *w, const WorldTextures *wt,
                           const RendererContext *rc)
{
    int n = mapdata_water_count();
    if (n > 0) {
        GLuint prog = make_program("shaders/water.vs", "shaders/water.fs");
        if (!prog) {
            SDL_Log("water: shaders failed, water disabled");
            return NULL;
        }
        WaterParams *p = calloc(1, sizeof *p);
        if (!p) { resources_program_release(prog); return NULL; }
        p->world = w;
        p->rc = rc;
        p->prog = prog;
        p->tex_height = wt->tex_height;

        int built = 0;
        for (int i = 0; i < n && p->n < MAP_MAX_WATERS; i++) {
            const MapWater *m = mapdata_water(i);
            if (!m) continue;
            WaterInst *it = &p->inst[p->n];
            it->md = m;
            it->wave_len = water_wavelen(m);
            float *verts = NULL;
            GLuint *idx = NULL;
            int vert_n = 0, idx_n = 0;
            int ok = -1;
            float step = it->wave_len / 3.0f;
            if (step < 0.5f) step = 0.5f;
            if (step > 8.0f) step = 8.0f;
            if (m->type == WATER_RIVER) {
                ok = build_river(m, &verts, &idx, &vert_n, &idx_n);
            } else if (m->type == WATER_SEA && m->radius <= 0.0f) {
                ok = build_sea_grid(w, &verts, &idx, &vert_n, &idx_n, step);
            } else {
                ok = build_disc(m, &verts, &idx, &vert_n, &idx_n, step);
            }
            if (ok != 0 || !verts || !idx || idx_n < 3) {
                free(verts); free(idx);
                SDL_Log("water: '%s' geometry failed, skipped",
                        m->name[0] ? m->name : "?");
                continue;
            }
            it->vao = upload_geometry(verts, vert_n, idx, idx_n,
                                      &it->vbo, &it->ibo);
            it->idx_count = idx_n;
            free(verts); free(idx);
            // BBOX для фрустум-отсечения (с запасом на волну)
            float pad = m->wave_amp * 2.0f + 1.0f;
            if (m->type == WATER_RIVER) {
                float hw = m->width * 0.5f + pad;
                it->bbox_min[0] = it->bbox_max[0] = m->points[0][0];
                it->bbox_min[2] = it->bbox_max[2] = m->points[0][1];
                for (int k = 1; k < m->point_n; k++) {
                    if (m->points[k][0] < it->bbox_min[0])
                        it->bbox_min[0] = m->points[k][0];
                    if (m->points[k][0] > it->bbox_max[0])
                        it->bbox_max[0] = m->points[k][0];
                    if (m->points[k][1] < it->bbox_min[2])
                        it->bbox_min[2] = m->points[k][1];
                    if (m->points[k][1] > it->bbox_max[2])
                        it->bbox_max[2] = m->points[k][1];
                }
                it->bbox_min[0] -= hw; it->bbox_max[0] += hw;
                it->bbox_min[2] -= hw; it->bbox_max[2] += hw;
            } else if (m->type == WATER_SEA && m->radius <= 0.0f) {
                it->bbox_min[0] = 0; it->bbox_min[2] = 0;
                it->bbox_max[0] = w->world_w; it->bbox_max[2] = w->world_d;
            } else {
                it->bbox_min[0] = m->center[0] - m->radius - pad;
                it->bbox_max[0] = m->center[0] + m->radius + pad;
                it->bbox_min[2] = m->center[1] - m->radius - pad;
                it->bbox_max[2] = m->center[1] + m->radius + pad;
            }
            // BBOX по вертикали расширен ВНИЗ на глубину: камера на берегу
            // или под водой не должна выкидывать водный слой по фрустуму
            // (тонкий 2-метровый бокс пропадал "вблизи"). Низ — дно мировое.
            it->bbox_min[1] = w->min_height - 1.0f;
            it->bbox_max[1] = m->level + pad;
            p->n++;
            built++;
        }
        if (built == 0) {
            // Ни одной воды не построилось — программу вернуть, узла нет
            resources_program_release(p->prog);
            free(p);
            return NULL;
        }
        water_prog_init(p, w);
        s_water = p;
        shader_on_reshade(water_reshade);
        SDL_Log("Water: %d water bodies (%d drawn)", n, built);
        const float size[3] = { w->world_w, 1.0f, w->world_d };
        SceneNode *node = node_new("Water", "MeshInstance3D", NULL, size, p,
                                   water_node_update, water_node_render);
        if (!node) {
            SceneNode partial = {0};
            partial.params = p;
            water_cleanup(&partial);
            free(p);
        }
        return node;
    }
    return NULL;
}

void water_node_update(SceneNode *n, float dt)
{
    WaterParams *p = n->params;
    p->time += dt;
}

// Набирает uniform'ы одного инстанса (вызывается перед glDrawElements)
static void water_instance_uniforms(WaterParams *p, const WaterInst *it)
{
    const MapWater *m = it->md;
    glUniform1f(p->u_time, p->time);
    glUniform1f(p->u_level, m->level);
    glUniform1f(p->u_waveamp, m->wave_amp);
    glUniform1f(p->u_wavespeed, m->wave_speed);
    glUniform1f(p->u_wavelen, it->wave_len);
    glUniform3f(p->u_color, m->color[0], m->color[1], m->color[2]);
    glUniform1f(p->u_alpha, m->alpha);
    glUniform1f(p->u_depthfade, m->depth > 0.01f ? m->depth : 0.01f);
    glUniform2f(p->u_flowdir, m->flow[0], m->flow[1]);
    glUniform1f(p->u_flowspeed, m->flow_speed);
    glUniform1f(p->u_foam, m->foam);
    glUniform1f(p->u_edge, water_edge_fade(m));
}

void water_node_render(SceneNode *n, float dt)
{
    (void)dt;
    WaterParams *p = n->params;
    const RendererContext *rc = p->rc;
    // вода не в карты теней и не в свой же кубомап отражений (самоотражение)
    if (rc->pass == RP_SHADOW || rc->pass == RP_CUBEMAP_FACE) return;
    if (p->n == 0) return;

    glViewport(rc->view_vp[0], rc->view_vp[1], rc->view_vp[2], rc->view_vp[3]);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_CULL_FACE);             // видно и снизу (из-под воды)

    glUseProgram(p->prog);
    scene_uniforms_bind(&p->su, rc);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, p->tex_height);
    glActiveTexture(GL_TEXTURE7);
    glBindTexture(GL_TEXTURE_CUBE_MAP, renderer_refl_cubemap());
    glActiveTexture(GL_TEXTURE0);
    glUniform1f(p->u_wetness, rc->wetness);

    for (int i = 0; i < p->n; i++) {
        WaterInst *it = &p->inst[i];
        if (!frustum_test_aabb(&rc->frustum, it->bbox_min, it->bbox_max))
            continue;
        water_instance_uniforms(p, it);
        glBindVertexArray(it->vao);
        glDrawElements(GL_TRIANGLES, it->idx_count, GL_UNSIGNED_INT, NULL);
    }

    glBindVertexArray(0);
    glDisable(GL_BLEND);
    glEnable(GL_CULL_FACE);
}

void water_cleanup(SceneNode *n)
{
    if (!n) return;   // error-path scene_build: узел мог не создаться
    WaterParams *p = n->params;
    if (s_water == p) s_water = NULL;
    for (int i = 0; i < p->n; i++) {
        WaterInst *it = &p->inst[i];
        if (it->vbo) glDeleteBuffers(1, &it->vbo);
        if (it->ibo) glDeleteBuffers(1, &it->ibo);
        if (it->vao) glDeleteVertexArrays(1, &it->vao);
    }
    resources_program_release(p->prog);
}
