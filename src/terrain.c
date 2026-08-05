// ============================================================================
//  terrain.c — рельеф: сетка из карты высот, материал земли, рендер с тенями
// ============================================================================
#include "terrain.h"

#include "assets.h"
#include "cvars.h"
#include "profiler.h"
#include "gl_loader.h"
#include "linalg.h"
#include "mapdata.h"
#include "resources.h"
#include "shader.h"
#include "sky.h"

#include "stb_image.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

#include <SDL3/SDL.h>

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
static int ray_hits_object(float x, float y, float z, float dz)
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

// Строит сетку g_grid_w x g_grid_h квадов поверх всей карты. Каждая вершина:
// (x, z) + (u, v). Высоту вершина получает в шейдере из яркости текстуры.
// UV берутся в ЦЕНТРАХ пикселей карты (GL_LINEAR между ними даёт
// билинейную интерполяцию) — рельеф гладкий, без "ступенек" по пикселям.
// Вершины — сетка без дублей (g_grid_w+1)x(g_grid_h+1), индексный буфер —
// 6 индексов на квад (на 33% меньше работы вершинного шейдера, чем
// 6 отдельных вершин на квад).
static void terrain_geometry_create(WorldState *w,
                                    GLuint *vao, GLuint *vbo, GLuint *ibo,
                                    int *vert_count)
{
    int gx = w->grid_w + 1, gy = w->grid_h + 1;
    int quads = w->grid_w * w->grid_h;
    *vert_count = quads * 6;          // индексов: два треугольника на квад
    w->vert_count = *vert_count;      // для HUD (GRID/TRIS)
    float *verts = malloc((size_t)gx * gy * 4 * sizeof(float));
    float *v = verts;
    float tw = w->world_w / w->grid_w;        // размер квада в метрах
    float td = w->world_d / w->grid_h;
    for (int j = 0; j <= w->grid_h; j++) {
        for (int i = 0; i <= w->grid_w; i++) {
            v[0] = (float)i * tw;
            v[1] = (float)j * td;
            v[2] = ((float)i * w->tex_per_quad + 0.5f) / w->tex_w;
            v[3] = ((float)j * w->tex_per_quad + 0.5f) / w->tex_h;
            v += 4;
        }
    }
    GLuint *idx = malloc((size_t)quads * 6 * sizeof(GLuint));
    GLuint *q = idx;
    // Лицевые стороны смотрят ВВЕРХ (CCW при взгляде сверху): с отсечением
    // задних граней снизу рельеф не виден — он сплошной.
    for (int j = 0; j < w->grid_h; j++) {
        for (int i = 0; i < w->grid_w; i++) {
            GLuint a = (GLuint)(j * gx + i);           // (x0, z0)
            GLuint b = (GLuint)((j + 1) * gx + i);     // (x0, z1)
            GLuint c = (GLuint)(j * gx + i + 1);       // (x1, z0)
            GLuint d = (GLuint)((j + 1) * gx + i + 1); // (x1, z1)
            *q++ = a; *q++ = b; *q++ = c;
            *q++ = b; *q++ = d; *q++ = c;
        }
    }
    glGenVertexArrays(1, vao);
    glBindVertexArray(*vao);
    glGenBuffers(1, vbo);
    glBindBuffer(GL_ARRAY_BUFFER, *vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)((size_t)gx * gy * 4 * sizeof(float)),
                 verts, GL_STATIC_DRAW);
    glGenBuffers(1, ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, *ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)((size_t)quads * 6 * sizeof(GLuint)),
                 idx, GL_STATIC_DRAW);
    free(verts);
    free(idx);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), NULL);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          (const void *)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);
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
                             CameraParams *cam, SunParams *sun)
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

    GLuint vao, vbo, ibo;
    int vert_count;
    terrain_geometry_create(w, &vao, &vbo, &ibo, &vert_count);

    TerrainParams *p = calloc(1, sizeof *p);
    p->world = w;
    p->prog = prog;
    p->vao = vao;    p->vbo = vbo;
    p->ibo = ibo;
    p->tex_base = tex_ground;             // земля (unit 0)
    p->tex_norm = wt->tex_norm;           // нормали (unit 1)
    p->tex_height = wt->tex_height;       // высоты (unit 2)
    p->vert_count = vert_count;
    p->cam = cam;                         // берём матрицы у Camera
    p->sun = sun;                         // и свет у Sun
    p->u_viewproj = glGetUniformLocation(prog, "uViewProj");
    p->u_campos   = glGetUniformLocation(prog, "uCamPos");
    p->u_lightdir = glGetUniformLocation(prog, "uLightDir");
    p->u_lightcolor = glGetUniformLocation(prog, "uLightColor");
    p->u_lightint = glGetUniformLocation(prog, "uLightIntensity");
    p->u_lightcount = glGetUniformLocation(prog, "uLightCount");
    p->u_ambient  = glGetUniformLocation(prog, "uAmbient");
    p->u_skycolor = glGetUniformLocation(prog, "uSkyColor");
    p->u_groundcolor = glGetUniformLocation(prog, "uGroundColor");
    p->u_fogcolor = glGetUniformLocation(prog, "uFogColor");
    p->u_fogstart = glGetUniformLocation(prog, "uFogStart");
    p->u_fogend   = glGetUniformLocation(prog, "uFogEnd");
    p->u_fogon    = glGetUniformLocation(prog, "uFogOn");
    p->u_specpow  = glGetUniformLocation(prog, "uSpecPower");
    p->u_specint  = glGetUniformLocation(prog, "uSpecIntensity");
    p->u_heightmin = glGetUniformLocation(prog, "uHeightMin");
    p->u_heightmax = glGetUniformLocation(prog, "uHeightMax");
    p->u_texscale  = glGetUniformLocation(prog, "uTexScale");
    for (int c = 0; c < SHADOW_CASCADES; c++) {
        char name[64];
        snprintf(name, sizeof name, "uShadowMap%d", c);
        p->u_shadowmap[c] = glGetUniformLocation(prog, name);
        snprintf(name, sizeof name, "uShadowMatrix%d", c);
        p->u_shadowmatrix[c] = glGetUniformLocation(prog, name);
        snprintf(name, sizeof name, "uShadowBias%d", c);
        p->u_shadowbias[c] = glGetUniformLocation(prog, name);
        snprintf(name, sizeof name, "uShadowDepth%d", c);
        p->u_shadowdepth[c] = glGetUniformLocation(prog, name);
        snprintf(name, sizeof name, "uShadowGrad%d", c);
        p->u_shadowgrad[c] = glGetUniformLocation(prog, name);
    }
    p->u_shadow_on = glGetUniformLocation(prog, "uShadowOn");
    p->u_statictex = glGetUniformLocation(prog, "uStaticShadow");
    p->u_static_on = glGetUniformLocation(prog, "uStaticOn");
    p->u_worldsize = glGetUniformLocation(prog, "uWorldSize");

    // Материал рельефа: статичные параметры шейдера
    float fog[3];
    sky_fog_color(fog);
    glUseProgram(prog);
    glUniform1i(glGetUniformLocation(prog, "uBasemap"), 0);   // земля
    glUniform1i(glGetUniformLocation(prog, "uNormalmap"), 1); // нормали
    glUniform1i(glGetUniformLocation(prog, "uHeightmap"), 2); // высоты
    glUniform1i(glGetUniformLocation(prog, "uShadowMap0"), 3);   // тени
    glUniform1i(glGetUniformLocation(prog, "uShadowMap1"), 4);
    glUniform1i(glGetUniformLocation(prog, "uShadowMap2"), 5);
    glUniform1i(glGetUniformLocation(prog, "uShadowMap3"), 6);
    // Сырая глубина каскадов — ТЕ ЖЕ текстуры (юниты 3-6): при выборке
    // через sampler2D режим сравнения игнорируется, читается глубина.
    for (int c = 0; c < SHADOW_CASCADES; c++)
        glUniform1i(p->u_shadowdepth[c], 3 + c);
    glUniform1i(p->u_statictex, 7);               // статические тени (unit 7)
    glUniform1i(p->u_static_on, 0);
    glUniform2f(p->u_worldsize, w->world_w, w->world_d);
    glUniform3f(p->u_ambient, 0.75f, 0.8f, 0.9f);      // сила полусферы
    glUniform3f(p->u_skycolor, fog[0], fog[1], fog[2]);
    glUniform3f(p->u_groundcolor, 0.22f, 0.30f, 0.18f); // отражение от травы
    glUniform3f(p->u_fogcolor, fog[0], fog[1], fog[2]);
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
    // Туман — за пределами карты: видна вся карта целиком
    glUniform1f(p->u_fogstart, w->world_w * tc.fog_start);
    glUniform1f(p->u_fogend, w->world_w * tc.fog_end);
    glUseProgram(0);

    const float size[3] = { w->world_w, w->max_height - w->min_height,
                            w->world_d };
    return node_new("Terrain", "MeshInstance3D", NULL, size, p,
                    NULL, terrain_node_render);
}

void terrain_node_render(SceneNode *n, float dt)
{
    (void)dt;
    TerrainParams *p = n->params;
    // Каскадные карты теней: камеры на солнце, пишем глубину. Каждый кадр
    // со свежим направлением солнца (p->sun->dir) — свет и тени всегда
    // в одном направлении. Карты выровнены по текселям, чтобы тени не
    // "плыли" при движении камеры.
    {
        Uint64 t0 = SDL_GetPerformanceCounter();
        prof_gpu_begin(PROF_SHADOW);
        shadow_render(p);
        prof_gpu_end(PROF_SHADOW);
        Uint64 t1 = SDL_GetPerformanceCounter();
        prof_add_ms(PROF_SHADOW,
                    (float)(t1 - t0) * 1000.0f /
                    (float)SDL_GetPerformanceFrequency());
    }

    // обычный проход
    glViewport(0, 0, g_vw, g_vh);
    glEnable(GL_DEPTH_TEST);   // 3D: ближние склоны закрывают дальние
    glDepthMask(GL_TRUE);      // (скайбокс её выключает)
    glUseProgram(p->prog);
    glUniformMatrix4fv(p->u_viewproj, 1, GL_FALSE, p->cam->viewproj);
    glUniform3f(p->u_campos, p->cam->cam.pos[0], p->cam->cam.pos[1], p->cam->cam.pos[2]);
    // Все направленные источники: массивом в шейдер.
    // Индекс 0 — солнце (единственный с каскадными тенями).
    {
        float dirs[SUN_MAX_LIGHTS * 3], cols[SUN_MAX_LIGHTS * 3];
        float ints[SUN_MAX_LIGHTS];
        memset(dirs, 0, sizeof dirs);
        memset(cols, 0, sizeof cols);
        memset(ints, 0, sizeof ints);
        int n = p->sun->light_count > SUN_MAX_LIGHTS ? SUN_MAX_LIGHTS : p->sun->light_count;
        for (int i = 0; i < n; i++) {
            memcpy(&dirs[i * 3], p->sun->light_dir[i], 3 * sizeof(float));
            memcpy(&cols[i * 3], p->sun->light_color[i], 3 * sizeof(float));
            ints[i] = p->sun->light_intensity[i];
        }
        glUniform3fv(p->u_lightdir, SUN_MAX_LIGHTS, dirs);
        glUniform3fv(p->u_lightcolor, SUN_MAX_LIGHTS, cols);
        glUniform1fv(p->u_lightint, SUN_MAX_LIGHTS, ints);
        glUniform1i(p->u_lightcount, n);
    }
    {
        // bias по каскадам — 3 латеральных текселя (PCF теперь аппаратный,
        // шаг задаёт GL_LINEAR, поэтому uShadowTexel больше не нужен).
        const float boxes[SHADOW_CASCADES] = { SHADOW_BOX_0, SHADOW_BOX_1, SHADOW_BOX_2, SHADOW_BOX_3 };
        const int sizes[SHADOW_CASCADES] = { g_shadow_size[0], g_shadow_size[1], g_shadow_size[2], g_shadow_size[3] };
        for (int c = 0; c < SHADOW_CASCADES; c++) {
            float depth = boxes[c] * SHADOW_DEPTH_MUL + 400.0f;
            // bias против acne: shadow_bias латеральных текселей (из
            // map/map.json, по умолчанию 4) на ближних каскадах; на дальнем
            // (бокс 2048, тексель 2 м) — всего 1/4 (иначе тень отрывается
            // от гор на метры; там acne и так не виден). В NDC: бокс
            // глубины 2*depth занимает ровно 2 единицы NDC, поэтому
            // метры -> NDC делятся на depth.
            float bias_world = (c == 3 ? 0.5f : 1.0f) * p->sun->shadow_bias
                               * boxes[c] / sizes[c];
            glUniformMatrix4fv(p->u_shadowmatrix[c], 1, GL_FALSE, p->shadow_mvp[c]);
            glUniform1f(p->u_shadowbias[c], bias_world / depth);
            // Градиент тени рельефа: полная темнота после ~10 м вглубь
            // тени — длинный плавный переход от края (в NDC: метры/depth).
            glUniform1f(p->u_shadowgrad[c], 10.0f / depth);
        }
    }
    glUniform1f(p->u_shadow_on,
                (shadow_is_on() && p->sun->shadow_on) ? 1.0f : 0.0f);
    // Классический туман в шейдере: выкл при r_fog_mode 0 (меню «Туман»)
    glUniform1f(p->u_fogon, cvar_get("r_fog_mode") > 0.5f ? 1.0f : 0.0f);
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
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, p->tex_base);     // земля (unit 0)
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, p->tex_norm);     // нормали (unit 1)
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, p->tex_height);   // высоты (unit 2)
    for (int c = 0; c < SHADOW_CASCADES; c++) {
        glActiveTexture(GL_TEXTURE3 + c);
        glBindTexture(GL_TEXTURE_2D, shadow_texture(c));   // каскады (unit 3-6)
    }
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(p->vao);
    glDrawElements(GL_TRIANGLES, p->vert_count, GL_UNSIGNED_INT, NULL);
}

void terrain_cleanup(SceneNode *n)
{
    TerrainParams *p = n->params;
    resources_program_release(p->prog);
    glDeleteBuffers(1, &p->vbo);
    glDeleteBuffers(1, &p->ibo);
    glDeleteVertexArrays(1, &p->vao);
    glDeleteTextures(1, &p->tex_base);   // вычисленная текстура — не из кеша
    if (s_static_tex) {
        glDeleteTextures(1, &s_static_tex);
        s_static_tex = 0;
        s_static_baked = 0;
    }
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
                    if (ray_hits_object(px, py, pz, dz)) { shadow = 1; break; }
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

int terrain_shadows_have(void)
{
    return s_static_baked && s_static_tex;
}
