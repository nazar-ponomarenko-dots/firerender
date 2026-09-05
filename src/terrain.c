// ============================================================================
//  terrain.c — рельеф: сетка из карты высот, материал земли, рендер с тенями
// ============================================================================
#include "terrain.h"

#include "assets.h"
#include "cvars.h"
#include "jobs.h"
#include "profiler.h"
#include "gl_loader.h"
#include "linalg.h"
#include "mapdata.h"
#include "resources.h"
#include "shader.h"
#include "sky.h"
#include "splat.h"

#include "stb_image.h"
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include <SDL3/SDL.h>

// --- Теневой проход (pass == RP_SHADOW) ----------------------------------------
// Глубина рельефа в каскад: та же сетка, высота из карты высот в шейдере.
// Программа раньше жила в shadow.c — теперь принадлежит рельефу.
static GLuint s_shadow_prog = 0;
static GLint u_sh_viewproj, u_sh_heightmap, u_sh_hmin, u_sh_hmax;
static TerrainParams *s_terr = NULL;   // для hot-reload (shader_on_reshade)

// Пересоздаёт программы, ищет uniform'ы и статичные uniform'ы. Вызывается
// при создании узла и при hot-reload шейдеров (shader_hot_reload).
// Проги создаются СНАРУЖИ (make_node / reshade: refcount балансируется там).
// Здесь — только uniform'ы для ТЕКУЩИХ id (p->prog и s_shadow_prog).
static void terrain_prog_init(TerrainParams *p, const WorldState *w)
{
    GLuint prog = p->prog;
    u_sh_viewproj = glGetUniformLocation(s_shadow_prog, "uViewProj");
    u_sh_heightmap = glGetUniformLocation(s_shadow_prog, "uHeightmap");
    u_sh_hmin = glGetUniformLocation(s_shadow_prog, "uHeightMin");
    u_sh_hmax = glGetUniformLocation(s_shadow_prog, "uHeightMax");

    scene_uniforms_init(&p->su, prog);    // общий набор сцены
    p->u_specpow  = glGetUniformLocation(prog, "uSpecPower");
    p->u_specint  = glGetUniformLocation(prog, "uSpecIntensity");
    p->u_heightmin = glGetUniformLocation(prog, "uHeightMin");
    p->u_heightmax = glGetUniformLocation(prog, "uHeightMax");
    p->u_texscale  = glGetUniformLocation(prog, "uTexScale");
    p->u_statictex = glGetUniformLocation(prog, "uStaticShadow");
    p->u_static_on = glGetUniformLocation(prog, "uStaticOn");
    p->u_worldsize = glGetUniformLocation(prog, "uWorldSize");
    p->u_wetness_mask = glGetUniformLocation(prog, "uWetnessMask");
    // Поверхности (splat): маска + 4 слоя. Юниты: 10 — маска, 12..15 — слои
    // (0..9 и 11 заняты базой/нормалями/высотами/каскадами/статикой/
    // мокростью/кубомапом/шумом — см. ниже).
    p->u_splat = glGetUniformLocation(prog, "uSplat");
    p->u_splat_on = glGetUniformLocation(prog, "uSplatOn");
    p->u_detail_dist = glGetUniformLocation(prog, "uDetailDist");
    p->u_layer_scale = glGetUniformLocation(prog, "uLayerScale");
    for (int i = 0; i < SPLAT_LAYERS; i++) {
        char name[16];
        snprintf(name, sizeof name, "uLayer%d", i);
        p->u_layer[i] = glGetUniformLocation(prog, name);
    }

    // Материал рельефа: статичные параметры шейдера
    MapTerrain tc;
    mapdata_terrain(&tc);
    glUseProgram(prog);
    glUniform1i(glGetUniformLocation(prog, "uBasemap"), 0);   // земля
    glUniform1i(glGetUniformLocation(prog, "uNormalmap"), 1); // нормали
    glUniform1i(glGetUniformLocation(prog, "uHeightmap"), 2); // высоты
    glUniform1i(p->u_wetness_mask, 8);             // маска мокрости (unit 8)
    // Каскады теней на юнитах 3-6 (uShadowMap%d и сырая глубина uShadowDepth%d —
    // ТЕ ЖЕ текстуры: при выборке через sampler2D режим сравнения
    // игнорируется, читается глубина).
    for (int c = 0; c < SHADOW_CASCADES; c++) {
        if (p->su.u_shadowmap[c] >= 0)
            glUniform1i(p->su.u_shadowmap[c], 3 + c);
        if (p->su.u_shadowdepth[c] >= 0)
            glUniform1i(p->su.u_shadowdepth[c], 3 + c);
    }
    glUniform1i(p->u_statictex, 7);               // статические тени (unit 7)
    glUniform1i(p->u_static_on, 0);
    glUniform2f(p->u_worldsize, w->world_w, w->world_d);
    // Слои поверхностей: сэмплеры и масштабы тайлов (метров на тайл).
    if (p->u_splat >= 0) glUniform1i(p->u_splat, 10);
    for (int i = 0; i < SPLAT_LAYERS; i++)
        if (p->u_layer[i] >= 0) glUniform1i(p->u_layer[i], 12 + i);
    {
        const MapSurfaces *sf = splat_surfaces();
        if (p->u_layer_scale >= 0)
            glUniform4f(p->u_layer_scale, sf->scale[0], sf->scale[1],
                        sf->scale[2], sf->scale[3]);
        if (p->u_splat_on >= 0)
            glUniform1f(p->u_splat_on, splat_have() ? 1.0f : 0.0f);
    }
    // Окружение/туман шлёт scene_uniforms_bind каждый кадр из rc
    glUniform1f(p->u_specpow, 40.0f);
    glUniform1f(p->u_specint, 0.0f);   // земля матовая — без отражения солнца
    glUniform1f(p->u_heightmin, w->min_height);
    glUniform1f(p->u_heightmax, w->max_height);
    // Тайл земли: текстура на tex_scale метров (из map.json)
    glUniform2f(glGetUniformLocation(prog, "uTexScale"),
                w->world_w / tc.tex_scale, w->world_d / tc.tex_scale);
    // Поле пшеницы: тон почвы внутри прямоугольника (вместо травы)
    glUniform4f(glGetUniformLocation(prog, "uField"),
                w->field_x0, w->field_x0 + w->field_w,
                w->field_z0, w->field_z0 + w->field_d);
    glUniform3f(glGetUniformLocation(prog, "uFieldColor"),
                tc.field_color[0], tc.field_color[1], tc.field_color[2]);
    glUniform1f(glGetUniformLocation(prog, "uFieldBlend"), tc.field_blend);
    // Туман (uFogStart/uFogEnd) шлёт scene_uniforms_bind из rc
    glUseProgram(0);
}

// Hot-reload: пересоздать программы и uniform'ы текущего узла.
// Старые программы отдаём в релиз: make_program считает ссылки, и без release
// каждый reload утекал бы и p->prog, и теневую программу (refs не падали,
// отложенное удаление в resources.c никогда не срабатывало).
// replace отдаёт старую ссылку и берёт свежую. Если пара не пересобралась,
// возвращает 0 — НЕ меняем id, рисуем прежнюю (она жива до релиза).
static void terrain_reshade(void)
{
    if (!s_terr) return;
    GLuint prog = resources_program_replace(s_terr->prog,
                                            "shaders/terrain.vs",
                                            "shaders/terrain.fs");
    if (prog) s_terr->prog = prog;
    else SDL_Log("terrain: reshade failed, keeping old program");
    GLuint sp = resources_program_replace(s_shadow_prog,
                                          "shaders/shadow.vs",
                                          "shaders/shadow.fs");
    if (sp) s_shadow_prog = sp;
    else SDL_Log("terrain: shadow program reshade failed, keeping old");
    terrain_prog_init(s_terr, s_terr->world);
}

// --- Статические тени (режим r_shadows_mode 2) ---------------------------------
// Разрешение карты теней: ~1 тексель на 1 м (2000x1125 -> 2048x1152).
#define STATIC_SHADOW_W 2048
#define STATIC_SHADOW_H 1152
#define STATIC_SHADOW_STEP 12.0f  // шаг луча по рельефу, м
#define STATIC_SHADOW_STEPS 40    // длина луча: 480 м
#define STATIC_OBJ_STEP 4.0f      // шаг луча по объектам, м (мельче: кроны узкие)
#define STATIC_OBJ_STEPS 60       // длина: 240 м

// Цилиндры объектов для трассировки (собраны и отсортированы по x один раз)
#define STATIC_OBJ_MAX 256
typedef struct {
    float x, z;      // позиция
    float r;         // радиус кроны, м
    float h;         // высота кроны, м
    float base;      // высота земли у объекта
} ShadowObj;
static ShadowObj s_shobjs[STATIC_OBJ_MAX];
static int s_shobjs_n = 0;

static int shobj_cmp(const void *a, const void *b)
{
    float ax = ((const ShadowObj *)a)->x;
    float bx = ((const ShadowObj *)b)->x;
    return (ax > bx) - (ax < bx);
}

static void shobjs_build(const WorldState *w)
{
    s_shobjs_n = 0;
    for (int i = 0; i < mapdata_object_count() && s_shobjs_n < STATIC_OBJ_MAX; i++) {
        const MapObject *o = mapdata_object(i);
        if (!o) continue;
        ShadowObj *s = &s_shobjs[s_shobjs_n];
        float sy = o->scale[1] > 0.01f ? o->scale[1] : 1.0f;
        s->x = o->pos[0];
        s->z = o->pos[2];
        s->r = 4.5f * sy;                     // радиус кроны
        s->h = 9.0f * sy + 2.0f;              // высота кроны
        s->base = sample_height(w, s->x, s->z);
        s_shobjs_n++;
    }
    qsort(s_shobjs, (size_t)s_shobjs_n, sizeof s_shobjs[0], shobj_cmp);
}

// Луч к солнцу: задевает ли крону дерева? Бинарный поиск по x отсекает
// почти все объекты — мелкий шаг (4 м) не даёт "дырок" в тени.
static int ray_hits_object(float x, float y, float z)
{
    if (s_shobjs_n == 0) return 0;
    // диапазон объектов с |ox| < 120 м (тень от дальних деревьев незаметна)
    int lo = 0, hi = s_shobjs_n - 1, first = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (s_shobjs[mid].x >= x - 120.0f) { first = mid; hi = mid - 1; }
        else lo = mid + 1;
    }
    if (first < 0) return 0;
    for (int k = first; k < s_shobjs_n; k++) {
        const ShadowObj *o = &s_shobjs[k];
        if (o->x > x + 120.0f) break;
        // луч почти горизонтален (dz по z-компоненте солнца); точка луча
        // на дистанции dist2d от дерева: тень от дерева тянется на dist
        float ddx = x - o->x;
        float ddz = z - o->z;
        if (ddx * ddx + ddz * ddz > o->r * o->r) continue;
        if (y < o->base - 1.0f || y > o->base + o->h + 1.0f) continue;
        return 1;
    }
    return 0;
}

static GLuint s_static_tex = 0;
static int s_static_baked = 0;

// --- Тайловая геометрия с 4 LOD -------------------------------------------------
// Карта режется на тайлы 128x128 м. Каждый тайл строится в 4 LOD-уровнях
// (шаг квада 2/8/32/128 м) — геометрия НЕ зависит от плотности текселей:
// высоту вершина берёт из карты высот в шейдере (GL_R16 LINEAR), поэтому
// крупные квады на дальних LOD дают гладкий рельеф при минимуме полигонов.
//
// Тайл — ровная плоская сетка (без склеек и плит), чтобы рельеф выглядел
// гладким, а не «блоками». Стыки разных LOD закрывает МЕЛКАЯ юбка —
// вертикальная стенка по границе тайла, опущенная всего на несколько метров
// (aDrop в шейдере): щелей нет, а снизу тайл не выглядит массивным блоком.
// Рисуем ВСЕ тайлы каждый кадр (без отсечения фрустумом) — LOD по дистанции
// держит число полигонов малым.
//
// Вершина: (x, z, u, v, drop) — 5 float. drop = 0 у поверхности, у нижней
// кромки юбки drop = TERRAIN_SKIRT_DEPTH (шейдер: y = h - drop).

#define TERRAIN_SKIRT_DEPTH 4.0f    // глубина юбки, м (скрывает стыки LOD)
// Дистанции переключения LOD (до ближайшей точки тайла), м.
// Реализованы как cvar'ы r_terrain_lod1..3: можно тюнить дальность отрисовки
// без пересборки (например, на слабых машинах уменьшить LOD3 = грубее дальний план).
static float s_lod1 = 200.0f;
static float s_lod2 = 500.0f;
static float s_lod3 = 1200.0f;
static const float s_lod_step[TERRAIN_LODS] = { 2.0f, 8.0f, 32.0f, 128.0f };

// Растущие буферы вершин/индексов (один VBO+IBO на весь рельеф)
typedef struct {
    float *v;    size_t nv, cv;    // 5 float на вершину
    GLuint *idx; size_t ni, ci;
} TerrMesh;

static GLuint tm_push_vert(TerrMesh *m, float x, float z,
                           float u, float v, float drop)
{
    if (m->nv + 5 > m->cv) {
        m->cv = m->cv ? m->cv * 2 : 1 << 18;
        m->v = realloc(m->v, m->cv * sizeof(float));
    }
    GLuint id = (GLuint)(m->nv / 5);
    m->v[m->nv++] = x;  m->v[m->nv++] = z;
    m->v[m->nv++] = u;  m->v[m->nv++] = v;
    m->v[m->nv++] = drop;
    return id;
}

static void tm_push_tri(TerrMesh *m, GLuint a, GLuint b, GLuint c)
{
    if (m->ni + 3 > m->ci) {
        m->ci = m->ci ? m->ci * 2 : 1 << 19;
        m->idx = realloc(m->idx, m->ci * sizeof(GLuint));
    }
    m->idx[m->ni++] = a;  m->idx[m->ni++] = b;  m->idx[m->ni++] = c;
}

// Квад поверхности (xa,za)-(xb,zb), лицевая сторона вверх (CCW сверху)
static void tm_push_quad(TerrMesh *m, const WorldState *w,
                         float xa, float za, float xb, float zb)
{
    float tex_w = (float)w->tex_w, tex_h = (float)w->tex_h;
    GLuint v00 = tm_push_vert(m, xa, za, (xa + 0.5f) / tex_w, (za + 0.5f) / tex_h, 0);
    GLuint v01 = tm_push_vert(m, xa, zb, (xa + 0.5f) / tex_w, (zb + 0.5f) / tex_h, 0);
    GLuint v10 = tm_push_vert(m, xb, za, (xb + 0.5f) / tex_w, (za + 0.5f) / tex_h, 0);
    GLuint v11 = tm_push_vert(m, xb, zb, (xb + 0.5f) / tex_w, (zb + 0.5f) / tex_h, 0);
    tm_push_tri(m, v00, v01, v10);
    tm_push_tri(m, v01, v11, v10);
}

// Вершина юбки: та же точка, но опущенная на drop (0 = верх юбки)
static GLuint tm_push_skirt_vert(TerrMesh *m, const WorldState *w,
                                 float x, float z)
{
    return tm_push_vert(m, x, z, (x + 0.5f) / (float)w->tex_w,
                        (z + 0.5f) / (float)w->tex_h, TERRAIN_SKIRT_DEPTH);
}

// Юбка вдоль края тайла: узлы xs[0..n-1] на прямой z=const (или x=const).
// Стенка смотрит НАРУЖУ тайла; нижняя кромка опущена всего на
// TERRAIN_SKIRT_DEPTH метров (мелкая: скрывает стыки соседних LOD, но
// тайл снизу не выглядит массивным блоком до пола мира).
static void tm_push_skirt_edge(TerrMesh *m, const WorldState *w,
                               const float *xs, int n,
                               float fixed, int along_x, int outward_pos)
{
    for (int i = 0; i + 1 < n; i++) {
        float xa = xs[i], xb = xs[i + 1];
        // верхняя кромка совпадает с краем поверхности тайла
        GLuint ta, tb, ba, bb;
        if (along_x) {   // край вдоль X: z = fixed (юг/север)
            ta = tm_push_vert(m, xa, fixed, (xa + 0.5f) / (float)w->tex_w,
                              (fixed + 0.5f) / (float)w->tex_h, 0);
            tb = tm_push_vert(m, xb, fixed, (xb + 0.5f) / (float)w->tex_w,
                              (fixed + 0.5f) / (float)w->tex_h, 0);
        } else {         // край вдоль Z: x = fixed (запад/восток)
            ta = tm_push_vert(m, fixed, xa, (fixed + 0.5f) / (float)w->tex_w,
                              (xa + 0.5f) / (float)w->tex_h, 0);
            tb = tm_push_vert(m, fixed, xb, (fixed + 0.5f) / (float)w->tex_w,
                              (xb + 0.5f) / (float)w->tex_h, 0);
        }
        ba = tm_push_skirt_vert(m, w, along_x ? xa : fixed, along_x ? fixed : xa);
        bb = tm_push_skirt_vert(m, w, along_x ? xb : fixed, along_x ? fixed : xb);
        // Нормаль наружу: юг(-z), запад(-x) -> (ta,tb,ba); север(+z), восток(+x) -> (ta,ba,tb)
        if ((along_x && !outward_pos) || (!along_x && outward_pos)) {
            tm_push_tri(m, ta, tb, ba);
            tm_push_tri(m, tb, bb, ba);
        } else {
            tm_push_tri(m, ta, ba, tb);
            tm_push_tri(m, tb, ba, bb);
        }
    }
}

// Один LOD тайла: ровная плоская сетка с шагом step + мелкие юбки.
// Возвращает диапазон индексов [first, first+count) в общем IBO.
// Ровная сетка БЕЗ склеек в плиты — рельеф гладкий, чанки не выглядят
// ступенчатыми блоками.
static void terrain_tile_lod(const WorldState *w, TerrMesh *m,
                             float x0, float z0, float x1, float z1,
                             float step,
                             GLuint *first, GLsizei *count)
{
    *first = (GLuint)m->ni;
    float wt = x1 - x0, dt = z1 - z0;
    int ncx = (int)(wt / step + 0.5f);           // целых клеток по X
    if ((float)ncx * step < wt - 0.01f) ncx++;   // + хвостовая (крупнее/мельче)
    if (ncx < 1) ncx = 1;
    int ncy = (int)(dt / step + 0.5f);
    if ((float)ncy * step < dt - 0.01f) ncy++;
    if (ncy < 1) ncy = 1;
    int nnx = ncx + 1, nny = ncy + 1;

    float *xs = malloc((size_t)nnx * sizeof(float));
    float *zs = malloc((size_t)nny * sizeof(float));
    if (!xs || !zs) {
        free(xs); free(zs);
        *count = 0;
        return;
    }
    for (int i = 0; i <= ncx; i++) xs[i] = (i < ncx) ? x0 + i * step : x1;
    for (int j = 0; j <= ncy; j++) zs[j] = (j < ncy) ? z0 + j * step : z1;

    // Ровная сетка: каждая клетка — один квад (без слияний в плиты)
    for (int j = 0; j < ncy; j++)
        for (int i = 0; i < ncx; i++)
            tm_push_quad(m, w, xs[i], zs[j], xs[i + 1], zs[j + 1]);

    // Мелкие юбки по четырём границам тайла
    tm_push_skirt_edge(m, w, xs, nnx, z0, 1, 0);              // юг
    tm_push_skirt_edge(m, w, xs, nnx, z1, 1, 1);              // север
    tm_push_skirt_edge(m, w, zs, nny, x0, 0, 0);              // запад
    tm_push_skirt_edge(m, w, zs, nny, x1, 0, 1);              // восток
    free(xs); free(zs);
    *count = (GLsizei)(m->ni - *first);
}

// Разбивает карту на тайлы 128x128 м, строит 4 LOD на тайл (слияние ровных
// квадов + юбки) и кладёт всё в один VBO/IBO. Формат вершины:
// (x, z, u, v, drop) — 5 float; UV в центрах текселей карты (GL_LINEAR
// между ними даёт билинейную высоту, как в прежней сетке 1 квад/метр).
static void terrain_geometry_create(WorldState *w,
                                    GLuint *vao, GLuint *vbo, GLuint *ibo,
                                    int *vert_count,
                                    TerrainTile **out_tiles, int *out_tiles_n)
{
    int tiles_x = (int)ceilf(w->world_w / TERRAIN_TILE);
    int tiles_z = (int)ceilf(w->world_d / TERRAIN_TILE);
    if (tiles_x < 1) tiles_x = 1;
    if (tiles_z < 1) tiles_z = 1;
    TerrainTile *tiles = malloc((size_t)tiles_x * tiles_z * sizeof *tiles);
    TerrMesh m = {0};
    Uint64 t0 = SDL_GetTicks();
    for (int tj = 0; tj < tiles_z; tj++) {
        for (int ti = 0; ti < tiles_x; ti++) {
            TerrainTile *t = &tiles[tj * tiles_x + ti];
            t->xmin = ti * TERRAIN_TILE;
            t->zmin = tj * TERRAIN_TILE;
            t->xmax = t->xmin + TERRAIN_TILE; if (t->xmax > w->world_w) t->xmax = w->world_w;
            t->zmax = t->zmin + TERRAIN_TILE; if (t->zmax > w->world_d) t->zmax = w->world_d;
            // Диапазон высот тайла (для AABB фрустум-теста): скан с шагом 4 м
            float ymin = 1e9f, ymax = -1e9f;
            for (float z = t->zmin; z <= t->zmax + 0.5f; z += 4.0f) {
                float zz = z > t->zmax ? t->zmax : z;
                for (float x = t->xmin; x <= t->xmax + 0.5f; x += 4.0f) {
                    float xx = x > t->xmax ? t->xmax : x;
                    float h = sample_height(w, xx, zz);
                    if (h < ymin) ymin = h;
                    if (h > ymax) ymax = h;
                }
            }
            t->ymin = ymin; t->ymax = ymax;
            for (int l = 0; l < TERRAIN_LODS; l++)
                terrain_tile_lod(w, &m, t->xmin, t->zmin, t->xmax, t->zmax,
                                 s_lod_step[l],
                                 &t->first[l], &t->count[l]);
        }
    }
    SDL_Log("Terrain: %dx%d tiles (%d), 4 LODs: %d verts / %d indices in %llu ms",
            tiles_x, tiles_z, tiles_x * tiles_z,
            (int)(m.nv / 5), (int)m.ni,
            (unsigned long long)(SDL_GetTicks() - t0));

    glGenVertexArrays(1, vao);
    glBindVertexArray(*vao);
    glGenBuffers(1, vbo);
    glBindBuffer(GL_ARRAY_BUFFER, *vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(m.nv * sizeof(float)),
                 m.v, GL_STATIC_DRAW);
    glGenBuffers(1, ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, *ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(m.ni * sizeof(GLuint)),
                 m.idx, GL_STATIC_DRAW);
    free(m.v);
    free(m.idx);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), NULL);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float),
                          (const void *)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, 5 * sizeof(float),
                          (const void *)(4 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glBindVertexArray(0);

    *vert_count = (int)m.ni;
    w->vert_count = *vert_count;      // для HUD (GRID/TRIS)
    *out_tiles = tiles;
    *out_tiles_n = tiles_x * tiles_z;
}

// Возводит точку в расстояние до AABB тайла (0 = внутри)
static float terrain_tile_dist2(const TerrainTile *t, const float *p)
{
    float dx = p[0] < t->xmin ? t->xmin - p[0] : (p[0] > t->xmax ? p[0] - t->xmax : 0);
    float dy = p[1] < t->ymin ? t->ymin - p[1] : (p[1] > t->ymax ? p[1] - t->ymax : 0);
    float dz = p[2] < t->zmin ? t->zmin - p[2] : (p[2] > t->zmax ? p[2] - t->zmax : 0);
    return dx * dx + dy * dy + dz * dz;
}

// Рисует тайлы. Цветной проход: отсечение фрустумом + LOD по дистанции.
// Теневой проход: отсечение по орто-боксу каскада (rc->shadow_vp).
static void terrain_draw_tiles(const TerrainParams *p,
                               const RendererContext *rc)
{
    if (!p->tiles) return;               // OOM при построении — нечего рисовать
    int shadow = (rc->pass == RP_SHADOW);
    // LOD-дистанции из cvar'ов (r_terrain_lod1..3) — применяются сразу
    float lod1 = cvar_get("r_terrain_lod1");
    float lod2 = cvar_get("r_terrain_lod2");
    float lod3 = cvar_get("r_terrain_lod3");
    s_lod1 = lod1; s_lod2 = lod2; s_lod3 = lod3;
    Frustum lf;                              // фрустум света (для тени)
    if (shadow) frustum_build(&lf, rc->shadow_vp);
    glBindVertexArray(p->vao);
    for (int i = 0; i < p->tiles_n; i++) {
        const TerrainTile *t = &p->tiles[i];
        float mn[3] = { t->xmin, t->ymin, t->zmin };
        float mx[3] = { t->xmax, t->ymax, t->zmax };
        // Фрустум-куллинг: цветной проход использует камеру, теневой — свет
        if (shadow) {
            if (!frustum_test_aabb(&lf, mn, mx))
                continue;
        } else {
            if (!frustum_test_aabb(&rc->frustum, mn, mx))
                continue;
        }
        float d2 = terrain_tile_dist2(t, rc->cam_pos);
        int lod;
        if (shadow) {
            float box = rc->shadow_box_cur;
            lod = box <= 16.0f ? (d2 < 32.0f * 32.0f ? 0 : 1)
                : box <= 96.0f ? (d2 < 96.0f * 96.0f ? 1 : 2)
                :                (d2 < 256.0f * 256.0f ? 2 : 3);
        } else {
            lod = d2 < s_lod1 * s_lod1 ? 0
                : d2 < s_lod2 * s_lod2 ? 1
                : d2 < s_lod3 * s_lod3 ? 2 : 3;
        }
        // Убрано: доклад в консоль "Terrain/frame: ..." каждые 240 кадров (шум).
        if (!t->count[lod]) continue;
        glDrawElements(GL_TRIANGLES, t->count[lod], GL_UNSIGNED_INT,
                       (const void *)(uintptr_t)((size_t)t->first[lod] *
                                                 sizeof(GLuint)));
    }
}

// Материал земли: ground.png с НАШТАМПОВАННЫМИ пучками травы (grass.png):
// дальняя трава "вшита" в текстуру, поэтому ВСЯ карта выглядит покрытой
// травой даже там, где объёмные пучки уже не видны (LOD-текстура —
// стандартный приём, дешёвле миллионов дальних спрайтов).
static GLuint terrain_ground_texture(const unsigned char *ground_img,
                                     const unsigned char *grass_img)
{
    int cw = 512, ch = 512;
    unsigned char *cover = malloc((size_t)cw * ch * 3);
    memcpy(cover, ground_img, (size_t)cw * ch * 3);   // фон = земля 1:1
    unsigned int rng2 = 0x51f15e;
    // Пучков много (2200): дальний вид растительности несёт ИМЕННО эта
    // текстура — статический слой спрайтов рисуется только вокруг камеры.
    for (int s = 0; s < 2200; s++) {
        float cx = frnd(&rng2) * cw, cy = frnd(&rng2) * ch;
        float sc = 0.12f + 0.16f * frnd(&rng2);   // пучок 15..36 px
        float ang = frnd(&rng2) * 6.2831853f;
        float tint = 0.80f + 0.25f * frnd(&rng2);
        float ca = cosf(ang), sa = sinf(ang);
        int hs = (int)(64 * sc) + 4;
        for (int dy = -hs; dy <= hs; dy++)
          for (int dx = -hs; dx <= hs; dx++) {
            float ux = ( dx * ca + dy * sa) / sc;
            float uy = (-dx * sa + dy * ca) / sc;
            int sx = (int)(ux + 64), sy = (int)(uy + 64);
            if ((unsigned)sx > 127 || (unsigned)sy > 127) continue;
            const unsigned char *sp = grass_img + ((size_t)sy * 128 + sx) * 4;
            if (sp[3] < 128) continue;
            int px = (int)cx + dx, py = (int)cy + dy;
            if ((unsigned)px >= (unsigned)cw || (unsigned)py >= (unsigned)ch) continue;
            unsigned char *d = cover + ((size_t)py * cw + px) * 3;
            float a = sp[3] / 255.0f * 0.55f;   // полупрозрачные пучки
            d[0] = (unsigned char)(d[0] * (1 - a) + sp[0] * tint * a);
            d[1] = (unsigned char)(d[1] * (1 - a) + sp[1] * tint * a);
            d[2] = (unsigned char)(d[2] * (1 - a) + sp[2] * tint * a);
        }
    }
    GLuint id = make_texture(cw, ch, cover, GL_RGB, GL_REPEAT, 0);
    free(cover);
    return id;
}

SceneNode *terrain_make_node(WorldState *w, const WorldTextures *wt,
                             const RendererContext *rc)
{
    // Материал земли: текстура из map.json (terrain.ground_tex), трава —
    // всегда img/grass.png (штампуется в землю)
    MapTerrain tc;
    mapdata_terrain(&tc);
    int gw = 0, gh = 0, aw = 0, ah = 0;
    unsigned char *ground_img = load_img(tc.ground_tex, &gw, &gh, STBI_rgb);
    unsigned char *grass_img  = load_img("img/grass.png", &aw, &ah, STBI_rgb_alpha);
    if (!ground_img || !grass_img) return NULL;
    GLuint tex_ground = terrain_ground_texture(ground_img, grass_img);
    // Пиксели во владении ресурсного кеша — не освобождаем (см. resources.h)

    GLuint prog = make_program("shaders/terrain.vs", "shaders/terrain.fs");
    if (!prog) return NULL;
    // Теневая программа (глубина рельефа): после cleanup() s_shadow_prog == 0,
    // поэтому replace здесь эквивалентен plain-загрузке.
    s_shadow_prog = resources_program_replace(s_shadow_prog,
                                              "shaders/shadow.vs",
                                              "shaders/shadow.fs");
    if (!s_shadow_prog) { resources_program_release(prog); return NULL; }

    GLuint vao, vbo, ibo;
    int vert_count;
    TerrainTile *tiles = NULL;
    int tiles_n = 0;
    terrain_geometry_create(w, &vao, &vbo, &ibo, &vert_count,
                            &tiles, &tiles_n);

    TerrainParams *p = calloc(1, sizeof *p);
    p->world = w;
    p->rc = rc;                           // кадровые данные (renderer.c)
    p->prog = prog;
    p->vao = vao;    p->vbo = vbo;
    p->ibo = ibo;
    p->tex_base = tex_ground;             // земля (unit 0)
    p->tex_norm = wt->tex_norm;           // нормали (unit 1)
    p->tex_height = wt->tex_height;       // высоты (unit 2)
    p->tiles = tiles;
    p->tiles_n = tiles_n;
    p->vert_count = vert_count;
    // Слои поверхностей (splat): текстуры тайлятся (REPEAT) с мип-фильтром —
    // асфальт/песок на дистанции не рябят. Пути и масштабы — из map.json
    // (surfaces = { ... }), дефолты — img/dirt|sand|gravel|asphalt.png.
    {
        const MapSurfaces *sf = splat_surfaces();
        for (int i = 0; i < SPLAT_LAYERS; i++)
            p->tex_splat_layer[i] = resources_texture(sf->tex[i], 3,
                                                      GL_REPEAT, 1);
    }
    terrain_prog_init(p, w);              // uniform'ы (+ статичные)
    s_terr = p;                           // для hot-reload
    shader_on_reshade(terrain_reshade);

    const float size[3] = { w->world_w, w->max_height - w->min_height,
                            w->world_d };
    return node_new("Terrain", "MeshInstance3D", NULL, size, p,
                    NULL, terrain_node_render);
}

void terrain_node_render(SceneNode *n, float dt)
{
    (void)dt;
    TerrainParams *p = n->params;
    const RendererContext *rc = p->rc;

    // Регистрация LOD-дистанций как cvar'ов (no-op при повторе).
    cvar_register_ex("r_terrain_lod1", s_lod1, 10.0f, 2000.0f, CVAR_ARCHIVE,
                     "рельеф LOD1: дальность детальной сетки, м");
    cvar_register_ex("r_terrain_lod2", s_lod2, 20.0f, 4000.0f, CVAR_ARCHIVE,
                     "рельеф LOD2: дальность средней сетки, м");
    cvar_register_ex("r_terrain_lod3", s_lod3, 30.0f, 8000.0f, CVAR_ARCHIVE,
                     "рельеф LOD3: дальность грубой сетки, м");
    cvar_register_ex("r_terr_detail_dist", 0.0f, 0.0f, 500.0f, CVAR_ARCHIVE,
                     "LOD деталей террейна: дальше — базовая карта (0 = выкл)");

    // Теневой проход (RP_SHADOW): глубина рельефа в текущий каскад.
    // Оркестрацию делает shadow.c (см. shadow_render в renderer_frame).
    // Без отсечения фрустумом: дальние тайлы дают длинные тени в каскад.
    if (rc->pass == RP_SHADOW) {
        if (g_shadow_terrain) {
            glUseProgram(s_shadow_prog);
            glUniformMatrix4fv(u_sh_viewproj, 1, GL_FALSE, rc->shadow_vp);
            glUniform1i(u_sh_heightmap, 2);
            glUniform1f(u_sh_hmin, p->world->min_height);
            glUniform1f(u_sh_hmax, p->world->max_height);
            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_2D, p->tex_height);
            terrain_draw_tiles(p, rc);   // кулинг по объёму каскада + LOD
        }
        return;
    }

    // обычный проход (вьюпорт задаёт рендер-контекст: окно или грань кубомапа)
    glViewport(rc->view_vp[0], rc->view_vp[1], rc->view_vp[2], rc->view_vp[3]);
    glEnable(GL_DEPTH_TEST);   // 3D: ближние склоны закрывают дальние
    glDepthMask(GL_TRUE);      // (скайбокс её выключает)
    glUseProgram(p->prog);
    // Общий набор: виды, свет, окружение, туман, каскады теней (из rc)
    scene_uniforms_bind(&p->su, p->rc);
    // Статические тени (режим 2): множитель вместо CSM
    {
        float on = 0.0f;
        if (cvar_get("r_shadows_mode") == 2.0f && terrain_shadows_have())
            on = 1.0f;
        glUniform1f(p->u_static_on, on);
        if (on > 0.5f) {
            glActiveTexture(GL_TEXTURE7);
            glBindTexture(GL_TEXTURE_2D, s_static_tex);
        }
    }
    // Маска мокрости (под крышей): unit 8
    if (terrain_wetness_mask_have()) {
        glActiveTexture(GL_TEXTURE8);
        glBindTexture(GL_TEXTURE_2D, terrain_wetness_mask_tex());
    }
    // LOD деталей: 0 = полный сплаттинг (дефолт), >0 = радиус деталей (м).
    // Дальше — чистая базовая карта: 6-8 текстурных выборок на пиксель
    // экономим на БОЛЬШОЙ части экрана (режим «КАРТОШКА»).
    if (p->u_detail_dist >= 0)
        glUniform1f(p->u_detail_dist, cvar_get("r_terr_detail_dist"));
    // Поверхности (splat): маска на юните 10, слои на 12..15
    if (p->u_splat_on >= 0)
        glUniform1f(p->u_splat_on, splat_have() ? 1.0f : 0.0f);
    if (splat_have()) {
        glActiveTexture(GL_TEXTURE10);
        glBindTexture(GL_TEXTURE_2D, splat_texture());
        for (int i = 0; i < SPLAT_LAYERS; i++) {
            if (!p->tex_splat_layer[i]) continue;
            glActiveTexture(GL_TEXTURE12 + i);
            glBindTexture(GL_TEXTURE_2D, p->tex_splat_layer[i]);
        }
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, p->tex_base);     // земля (unit 0)
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, p->tex_norm);     // нормали (unit 1)
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, p->tex_height);   // высоты (unit 2)
    glActiveTexture(GL_TEXTURE0);
    terrain_draw_tiles(p, rc);           // LOD по дистанции + отсечение фрустумом
}

void terrain_cleanup(SceneNode *n)
{
    if (!n) return;   // безопасно при частичном построении сцены (error-path)
    TerrainParams *p = n->params;
    resources_program_release(s_shadow_prog);
    s_shadow_prog = 0;
    if (s_terr == p) s_terr = NULL;
    resources_program_release(p->prog);
    glDeleteBuffers(1, &p->vbo);
    glDeleteBuffers(1, &p->ibo);
    glDeleteVertexArrays(1, &p->vao);
    glDeleteTextures(1, &p->tex_base);   // вычисленная текстура — не из кеша
    for (int i = 0; i < SPLAT_LAYERS; i++)
        if (p->tex_splat_layer[i]) {
            resources_texture_release(p->tex_splat_layer[i]);
            p->tex_splat_layer[i] = 0;
        }
    free(p->tiles);
    if (s_static_tex) {
        glDeleteTextures(1, &s_static_tex);
        s_static_tex = 0;
        s_static_baked = 0;
    }
}

int terrain_tile_count(void)
{
    return s_terr ? s_terr->tiles_n : 0;
}

// --- Статические тени (режим r_shadows_mode 2) ---------------------------------
// (константы и статики — в начале файла)
static float static_ground(const WorldState *w, float x, float z)
{
    return sample_height(w, x, z);
}

void terrain_shadows_bake(const WorldState *w, SunParams *sun)
{
    if (!sun || sun->light_count < 1) return;
    // Луч к солнцу (первый направленный источник)
    const float *ld = sun->light_dir[0];
    float len = sqrtf(ld[0] * ld[0] + ld[1] * ld[1] + ld[2] * ld[2]);
    if (len < 0.001f) return;
    float dx = ld[0] / len, dy = ld[1] / len, dz = ld[2] / len;

    unsigned char *tex = malloc((size_t)STATIC_SHADOW_W * STATIC_SHADOW_H);
    if (!tex) return;
    shobjs_build(w);   // цилиндры объектов: один раз, отсортированы по x
    Uint64 t0 = SDL_GetTicks();
    int shadow_px = 0, total_px = 0;
    for (int j = 0; j < STATIC_SHADOW_H; j++) {
        for (int i = 0; i < STATIC_SHADOW_W; i++) {
            float x = ((float)i + 0.5f) / STATIC_SHADOW_W * w->world_w;
            float z = ((float)j + 0.5f) / STATIC_SHADOW_H * w->world_d;
            float y = static_ground(w, x, z);
            int shadow = 0;
            // 1) рельеф: крупный шаг до 480 м
            for (int s = 1; s <= STATIC_SHADOW_STEPS; s++) {
                float tt = s * STATIC_SHADOW_STEP;
                float px = x + dx * tt;
                float pz = z + dz * tt;
                float py = y + dy * tt;
                if (px < 0 || pz < 0 || px > w->world_w || pz > w->world_d)
                    break;                       // вышли за карту — свет
                if (static_ground(w, px, pz) > py + 1.0f) { shadow = 1; break; }
            }
            // 2) объекты: МЕЛКИЙ шаг (4 м), до 240 м — непрерывные тени
            //    от крон (крупный шаг "проскакивал" узкие деревья)
            if (!shadow) {
                for (int s = 1; s <= STATIC_OBJ_STEPS; s++) {
                    float tt = s * STATIC_OBJ_STEP;
                    float px = x + dx * tt;
                    float pz = z + dz * tt;
                    float py = y + dy * tt;
                    if (px < 0 || pz < 0 || px > w->world_w || pz > w->world_d)
                        break;
                    if (ray_hits_object(px, py, pz)) { shadow = 1; break; }
                }
            }
            // В OpenGL первый ряд данных glTexImage2D — это v=0 (НИЗ текстуры).
            // У нас z=0 должен оказаться в v=0 (UV в шейдере = z/world_d),
            // поэтому пишем БЕЗ инверсии (инверсия здесь давала тени
            // зеркально по Z — "в сторону солнца").
            tex[j * STATIC_SHADOW_W + i] = shadow ? 64 : 255;   // тень ~25%
            total_px++;
            if (shadow) shadow_px++;
        }
    }
    SDL_Log("Static shadows baked (%dx%d) in %llu ms: %d/%d px in shadow (%.1f%%)",
            STATIC_SHADOW_W, STATIC_SHADOW_H,
            (unsigned long long)(SDL_GetTicks() - t0),
            shadow_px, total_px,
            total_px ? 100.0 * shadow_px / total_px : 0.0);

    // Отладка: дамп карты теней в PPM (FR_SHADOW_DUMP=1)
    if (getenv("FR_SHADOW_DUMP")) {
        FILE *f = fopen("/tmp/static_shadow.ppm", "wb");
        if (f) {
            fprintf(f, "P6\n%d %d\n255\n", STATIC_SHADOW_W, STATIC_SHADOW_H);
            for (int j = STATIC_SHADOW_H - 1; j >= 0; j--)
                for (int i = 0; i < STATIC_SHADOW_W; i++) {
                    unsigned char v = tex[j * STATIC_SHADOW_W + i];
                    fputc(v, f); fputc(v, f); fputc(v, f);
                }
            fclose(f);
            SDL_Log("Static shadow dump: /tmp/static_shadow.ppm");
        }
    }

    // Текстура: R8, nearest, clamp
    if (!s_static_tex)
        glGenTextures(1, &s_static_tex);
    glBindTexture(GL_TEXTURE_2D, s_static_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, STATIC_SHADOW_W, STATIC_SHADOW_H,
                 0, GL_RED, GL_UNSIGNED_BYTE, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    free(tex);
    s_static_baked = 1;
}

// --- Wetness mask (baked "under roof" detection) ---------------------------
// текстура видимости неба: RG8. R = 255 = открытое небо
// (дождь/снег попадают), G = 255 = под крышей (сухо).
#define WETNESS_MASK_W 512
#define WETNESS_MASK_H 512
#define WETNESS_MASK_MAX_H 500.0f   // макс. высота луча вверх
#define WETNESS_MASK_STEPS 256      // шагов луча

static GLuint s_wetness_mask_tex = 0;
static int s_wetness_mask_baked = 0;

// Луч вверх: задевает ли крону дерева? Аналогично ray_hits_object, но луч
// направлен строго вверх (0, 1, 0).
static int ray_hits_object_up(float x, float y, float z)
{
    if (s_shobjs_n == 0) return 0;
    // диапазон объектов с |ox| < 120 м
    int lo = 0, hi = s_shobjs_n - 1, first = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (s_shobjs[mid].x >= x - 120.0f) { first = mid; hi = mid - 1; }
        else lo = mid + 1;
    }
    if (first < 0) return 0;
    for (int k = first; k < s_shobjs_n; k++) {
        const ShadowObj *o = &s_shobjs[k];
        if (o->x > x + 120.0f) break;
        float ddx = x - o->x;
        float ddz = z - o->z;
        if (ddx * ddx + ddz * ddz > o->r * o->r) continue;
        if (y < o->base - 1.0f || y > o->base + o->h + 1.0f) continue;
        return 1;
    }
    return 0;
}

// Один кусок строк маски мокрости для job-пула (см. jobs.h). Данные только
// читаем (s_shobjs построен, w->heights готовы) — безопасно параллелить.
typedef struct {
    const WorldState *w;
    unsigned char *tex;
    SDL_AtomicInt dry, total;
} WetnessJob;

static void wetness_rows(int i0, int i1, void *ud)
{
    WetnessJob *j = (WetnessJob *)ud;
    const WorldState *w = j->w;
    unsigned char *tex = j->tex;
    int dry_local = 0, total_local = 0;
    for (int jj = i0; jj < i1; jj++) {
        for (int i = 0; i < WETNESS_MASK_W; i++) {
            float x = ((float)i + 0.5f) / WETNESS_MASK_W * w->world_w;
            float z = ((float)jj + 0.5f) / WETNESS_MASK_H * w->world_d;
            float y = static_ground(w, x, z) + 0.1f; // чуть выше земли
            int dry = 0;

            // Луч вверх: ищем нависающие объекты
            for (int s = 1; s <= WETNESS_MASK_STEPS; s++) {
                float tt = s * (WETNESS_MASK_MAX_H / WETNESS_MASK_STEPS);
                float py = y + tt;
                if (ray_hits_object_up(x, py, z)) { dry = 1; break; }
            }

            // В OpenGL первый ряд = v=0 (НИЗ текстуры), z=0 -> v=0.
            // R = открытое небо (падает дождь/снег), G = под крышей (сухо).
            size_t k = ((size_t)jj * WETNESS_MASK_W + i) * 2;
            tex[k]     = dry ? 0 : 255;
            tex[k + 1] = dry ? 255 : 0;
            total_local++;
            if (dry) dry_local++;
        }
    }
    SDL_AddAtomicInt(&j->dry, dry_local);
    SDL_AddAtomicInt(&j->total, total_local);
}

void terrain_wetness_mask_bake(const WorldState *w)
{
    if (s_wetness_mask_baked) return; // already baked this session
    shobjs_build(w);   // строим список объектов (цилиндры крон)

    unsigned char *tex = malloc((size_t)WETNESS_MASK_W * WETNESS_MASK_H * 2);
    if (!tex) return;

    Uint64 t0 = SDL_GetTicks();

    // Параллельно по строкам: 512 строк * 256 шагов луча — чистый CPU,
    // ни GL, ни общих записей (текстура режется по строкам без пересечений)
    WetnessJob j;
    j.w = w;
    j.tex = tex;
    SDL_SetAtomicInt(&j.dry, 0);
    SDL_SetAtomicInt(&j.total, 0);
    jobs_parallel_for(0, WETNESS_MASK_H, wetness_rows, &j);
    int dry_px = SDL_GetAtomicInt(&j.dry), total_px = SDL_GetAtomicInt(&j.total);
    SDL_Log("Wetness mask baked (%dx%d) in %llu ms: %d/%d px dry (%.1f%%)",
            WETNESS_MASK_W, WETNESS_MASK_H,
            (unsigned long long)(SDL_GetTicks() - t0),
            dry_px, total_px,
            total_px ? 100.0 * dry_px / total_px : 0.0);

    if (!s_wetness_mask_tex)
        glGenTextures(1, &s_wetness_mask_tex);
    glBindTexture(GL_TEXTURE_2D, s_wetness_mask_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, WETNESS_MASK_W, WETNESS_MASK_H,
                 0, GL_RG, GL_UNSIGNED_BYTE, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    free(tex);
    s_wetness_mask_baked = 1;
}

int terrain_wetness_mask_have(void)
{
    return s_wetness_mask_baked && s_wetness_mask_tex;
}

GLuint terrain_wetness_mask_tex(void)
{
    return s_wetness_mask_tex;
}

int terrain_snow_mask_have(void)
{
    return terrain_wetness_mask_have();
}

GLuint terrain_snow_mask_tex(void)
{
    return s_wetness_mask_tex;
}

int terrain_shadows_have(void)
{
    return s_static_baked && s_static_tex;
}
