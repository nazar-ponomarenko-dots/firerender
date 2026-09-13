// ============================================================================
//  vegetation.c — растительность: статичный слой (вся карта) + динамический
//  (вокруг камеры)
// ----------------------------------------------------------------------------
//  Слой 1 — СТАТИЧНЫЙ: вся карта покрыта травой/пшеницей с минимальной
//  плотностью (s_grass_floor / s_wheat_floor). Генерируется ОДИН раз при старте,
//  заливается в VBO и больше НЕ трогается — летать и "прогружать чанки"
//  не нужно: растительность есть на всей карте с первой секунды.
//  Слой 2 — ДИНАМИЧЕСКИЙ: вокруг камеры (радиус s_dyn_r) плотность растёт
//  по LOD-кривой: в 5 м от игрока — как сейчас (100/м2 пшеница, 50/м2 трава),
//  дальше плавно реже, на границе радиуса сходится к полу. При переходе
//  камеры пересоздаются только чанки, чья плотность заметно изменилась
//  (VEG_REGEN_TOL) — это пара чанков, без ступора и "прогрузки".
//
//  Растения ОРИЕНТИРУЮТСЯ ПО НОРМАЛИ земли: наклоняются вместе с рельефом,
//  а не стоят строго вертикально. Нормаль считается от тех же квантованных
//  высот (g_hq), поэтому растения лежат точно на поверхности.
//
//  Спрайты — слои 2D-массива 256x178 (0/1/2=grass.png/grass1.png/grass2.png,
//  3=dandelions.png, 4-6=wheat_lod0/1/2.png); LOD пшеницы выбирается в шейдере
//  по дистанции. Вид, поворот, размер, тон, наклон — атрибуты экземпляра
//  (12 float). Поле пшеницы задаёт main.c через g_field_*. Отрисовка —
//  4 инстанс-вызова (2 статики + 2 динамики), один шейдер.
// ============================================================================
#include "vegetation.h"

#include "assets.h"
#include "bloom.h"
#include "cvars.h"
#include "gl_loader.h"
#include "linalg.h"
#include "mapdata.h"
#include "resources.h"
#include "shader.h"
#include "shadow.h"
#include "sky.h"
#include "splat.h"
#include "water.h"
#include "world.h"
#include "fx.h"

#include "stb_image.h"
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>   // SDL_Log

// Плотности и радиусы — из map/map.json (veg = { ... }); значения по
// умолчанию совпадают с прежними константами.
static float s_grass_density = 1.2f;     // трава: шт/м2 вплотную к камере
static float s_wheat_density = 140.0f;  // пшеница: шт/м2 вплотную к камере
static float s_grass_floor = 0.012f;     // минимум травы на ВСЕЙ карте
static float s_wheat_floor = 1.0f;      // минимум пшеницы в поле
static float s_dandelion_density = 0.05f;   // одуванчики шт/м2 у камеры (редкие)
static float s_dandelion_floor   = 0.002f;  // минимум одуванчиков на карте
static float s_falloff_r = 16.0f;       // радиус падения плотности (LOD)
static float s_dyn_r = 48.0f;           // радиус динамического слоя
static float s_vis_r = 220.0f;          // радиус статики, м (у земли)

// --- Слои спрайтов 2D-массива (порядок загрузки = индекс) --------------------
#define VEG_LAY_GRASS0  0   // img/grass.png    — трава, вид 1 (высокая)
#define VEG_LAY_GRASS1  1   // img/grass1.png   — трава, вид 2 (средняя)
#define VEG_LAY_GRASS2  2   // img/grass2.png   — трава, вид 3 (низкая)
#define VEG_LAY_DANDE   3   // img/dandelions.png — одуванчик
#define VEG_LAY_WHEAT0  4   // img/wheat_lod0.png
#define VEG_LAY_WHEAT1  5   // img/wheat_lod1.png
#define VEG_LAY_WHEAT2  6   // img/wheat_lod2.png
#define VEG_LAYERS      7
// Граница класса "пшеница" в атрибуте layer (iAttr.w): всё, что >= 3.5,
// рисуется с LOD-лестницей 4..6; меньшее (0/1/2 — трава, 3 — одуванчик)
// сэмплируется как есть.
#define VEG_WHEAT_BASE  4.0f
#define VEG_WHEAT_MARK  3.5f

// --- Гибкая таблица видов растительности ---------------------------------------
// Каждый вид — строка: масштаб, тон, наклон, вес появления и индекс слоя
// 2D-массива спрайтов. Трава (3 вида) и одуванчик рисуются
// одним потоком экземпляров; вид выбирается по весу. Новый вид = одна строка
// текстуры в массиве + одна строка здесь. Плотность всей «травяной» группы
// одна (s_grass_density) — веса распределяют её между видами.
//
// Поле sink УБРАНО: растения ставятся ОСНОВАНИЕМ РОВНО НА ЗЕМЛЮ. Прозрачные
// поля спрайтов компенсирует шейдер (veg_base_v в wheat.vs), а утапливание
// на 36..46% высоты прятало низ травы под рельефом.
typedef struct {
    float s0, s1;      // мин/макс масштаб (м)
    float t0, t1;      // мин/макс тон
    float lean;        // максимальный наклон вбок (амплитуда, рад)
    float weight;      // доля вида среди всей травяной группы (0..~1)
    float layer;       // слой 2D-массива спрайтов (см. VEG_LAY_*)
} VegKind;
#define VEG_KINDS 4
#define VEG_KIND_DANDELION 3   // индекс одуванчика в таблице s_kinds
static const VegKind s_kinds[VEG_KINDS] = {
    //             s0    s1    t0    t1   lean  weight layer
    { 0.32f, 0.62f, 0.78f, 1.06f, 0.26f, 0.30f, VEG_LAY_GRASS0 },  // высокая густая
    { 0.22f, 0.46f, 0.85f, 1.12f, 0.25f, 0.28f, VEG_LAY_GRASS1 },  // средняя
    { 0.18f, 0.38f, 0.72f, 0.98f, 0.25f, 0.26f, VEG_LAY_GRASS2 },  // низкая рыхлая
    { 0.30f, 0.55f, 0.95f, 1.15f, 0.15f, 0.0f,  VEG_LAY_DANDE  },  // одуванчик (вне весов, редкий по плотности)
};
// Сумма весов ТРАВЫ (одуванчик выбирается отдельно по p_dande, его вес
// в таблицу не входит при нормировке выбора вида).
static float s_grass_total = 0.0f;
static float veg_grass_total(void)
{
    if (s_grass_total > 0.0f) return s_grass_total;
    float t = 0.0f;
    for (int i = 0; i < VEG_KINDS; i++)
        if (i != VEG_KIND_DANDELION) t += s_kinds[i].weight;
    s_grass_total = t > 0.0f ? t : 1.0f;
    return s_grass_total;
}
// Выбор вида ТРАВЫ по весу (одуванчик не участвует — он редкий и
// выбирается своей вероятностью). pick ∈ [0..grass_total) → индекс вида.
static int veg_pick_kind(unsigned int *rng)
{
    float total = veg_grass_total();
    float pick = frnd(rng) * total;
    float acc = 0.0f;
    for (int i = 0; i < VEG_KINDS; i++) {
        if (i == VEG_KIND_DANDELION) continue;   // одуванчик — отдельно
        acc += s_kinds[i].weight;
        if (pick < acc) return i;
    }
    return 0;
}
#define VEG_CHUNK       16.0f     // сторона чанка в метрах
#define VEG_REGEN_TOL    3.0f     // пересоздать чанк, если плотность сменилась больше
// Радиус видимости СТАТИЧЕСКОГО слоя вокруг камеры: дальше спрайты меньше
// пикселя и не видны, а их "вид" несёт текстура земли (наштампованная
// трава) и тон поля. С ростом высоты камеры радиус растёт (обзор с горы).
#define VEG_VIS_H       50.0f     // выше этой высоты радиус растёт
#define VEG_VIS_F        0.6f     // ...на столько метров на метр высоты
// Предел уклона для растительности убран: трава/пшеница растут на любых
// наклонных поверхностях (ориентируются по нормали земли), плотность
// компенсируется по площади поверхности — без "дырок" на склонах.
#define VEG_SLOPE_E     0.4f      // шаг проверки уклона, м
#define VEG_CAND_CAP  51200       // максимум кандидатов на чанк (пшеница 140/м2)

// Кэш динамических чанков (вокруг камеры)
typedef struct {
    float *g, *w;                // экземпляры: 12 float (см. ниже)
    int gn, wn;
    float dg_d, dw_d;            // с какой плотностью сгенерирован
} VegChunk;

static VegChunk *g_veg_chunks;   // [g_map_w * g_map_d]
static int g_map_w, g_map_d;
static float *g_static_g, *g_static_w;   // статичный слой (вся карта)
static int g_static_gc, g_static_wc;
static float *g_dyn_g, *g_dyn_w;         // динамический слой (вокруг камеры)
static int g_dyn_gc, g_dyn_wc, g_dyn_cap;
// Общие scratch-буферы генерации: раньше каждый плотный чанк временно выделял
// два массива по VEG_CAND_CAP (в сумме ~4.7 MiB) прямо в кадре.
static float *g_gen_scratch_g, *g_gen_scratch_w;
static float *g_shadow_scratch_g, *g_shadow_scratch_w;
static int g_shadow_scratch_gcap, g_shadow_scratch_wcap;
// INT_MAX: первый veg_update должен ОБЯЗАТЕЛЬНО сработать (иначе при старте
// камеры в чанке (0,0) динамический слой никогда не сгенерируется)
static int g_chunk_cam[2] = { INT_MAX, INT_MAX };
// Бюджет регенерации (r_veg_regen_budget): не больше N чанков за вызов
// veg_update, остальные — в следующих кадрах. На 2-ядерных CPU пачка
// пересозданий за один кадр = фриз; добор убирает рывки. >0 = есть хвост.
static int g_regen_pending = 0;
// VAO растительности и массив спрайтов — нужны теневому проходу (shadow.c)
static GLuint g_veg_vao[4];
static GLuint g_veg_tex = 0;
static GLuint g_veg_mesh_vbo, g_veg_mesh_ibo, g_veg_vb[4];
// Предпрореженный слой растительности для теней: статика — каждый 2-й
// экземпляр; динамика — по правилу uThin шейдера ((id*2654435761)%100<50),
// чтобы набор в карте теней был ТОЧНО тем же, что даёт прореживание в
// шейдере, но вершинный этап обрабатывал вдвое меньше экземпляров.
static GLuint g_veg_shadow_vao[4], g_veg_shadow_vbo[4];
static int g_veg_shadow_cnt[4];

// --- GPU-driven (compute-cull + indirect draw, GL 4.3+) ---------------------
//  Контекст-независимые буферы: все инстансы лежат в Src-SSBO, compute-проход
//  отсекает по фрустуму + экранному следу и КОМПАКТНО пишет выжившие в
//  Cull-SSBO, а их число копит в instanceCount команды Indirect. Отрисовка —
//  glDrawElementsIndirect: число вершин ограничено видимыми пикселями, а не
//  плотностью («миллиард» ≈ 1 травинка по затратам). Фолбэк — инстансный
//  путь ниже (если g_veg_gpudriven == 0 или cvar r_veg_gpudriven == 0).
static int g_veg_gpudriven = 0;       // путь активен (capability && cvar)
static int g_veg_src_cnt[4];          // число инстансов в Src по слоям
static GLuint g_veg_src_ssbo[4];      // все инстансы (читаются в compute)
static GLuint g_veg_cull_ssbo[4];     // компактные выжившие (-> VAO инстансов)
static GLuint g_veg_indirect[4];      // DrawElementsIndirectCommand
static GLuint g_veg_vao_cull[4];      // VAO с инстансами из Cull-SSBO
static GLuint s_cull_prog = 0;        // compute-программа отсечения
static GLint u_cull_vp, u_cull_viewh, u_cull_tan, u_cull_minpix, u_cull_src, u_cull_off;
static GLint u_cull_planes, u_cull_campos, u_cull_capacity;

// Прототипы GPU-driven (определены ниже, используются выше по файлу).
static void veg_gpu_init(VegParams *p);
static void veg_gpu_src_upload(int q, const float *src, int count);
static void veg_gpu_draw(VegParams *p, const RendererContext *rc);

// Теневой шейдер растительности (глубина с прореживанием на CPU) —
// раньше жил в shadow.c, теперь принадлежит растительности.
static GLuint s_shadowveg_prog = 0;
static GLint u_veg_viewproj, u_veg_veg, u_veg_thin, u_veg_time;
static GLint u_veg_campos, u_veg_dyn, u_veg_walk, u_veg_windboost;
static GLint u_cull_lodshift = -1;
static VegParams *s_veg = NULL;

// Проги создаются СНАРУЖИ (make_node / reshade: refcount балансируется там).
// Здесь — только uniform'ы для ТЕКУЩИХ id (p->prog и s_shadowveg_prog).
static void veg_prog_init(VegParams *p)
{
    GLuint prog = p->prog;
    u_veg_viewproj = glGetUniformLocation(s_shadowveg_prog, "uViewProj");
    u_veg_veg      = glGetUniformLocation(s_shadowveg_prog, "uVeg");
    u_veg_thin     = glGetUniformLocation(s_shadowveg_prog, "uThin");
    u_veg_time     = glGetUniformLocation(s_shadowveg_prog, "uTime");
    u_veg_campos   = glGetUniformLocation(s_shadowveg_prog, "uCamPos");
    u_veg_dyn      = glGetUniformLocation(s_shadowveg_prog, "uDyn");
    u_veg_walk     = glGetUniformLocation(s_shadowveg_prog, "uWalk");
    u_veg_windboost = glGetUniformLocation(s_shadowveg_prog, "uWindBoost");

    scene_uniforms_init(&p->su, prog);
    p->u_dyn      = glGetUniformLocation(prog, "uDyn");
    p->u_time     = glGetUniformLocation(prog, "uTime");
    p->u_walk     = glGetUniformLocation(prog, "uWalk");
    p->u_windboost = glGetUniformLocation(prog, "uWindBoost");
    p->u_lodshift = glGetUniformLocation(prog, "uLodShift");

    // Статичные uniform'ы растительности
    glUseProgram(prog);
    glUniform1i(glGetUniformLocation(prog, "uVeg"), 0);
    // Каскады на юнитах 3-6 (uShadowMap%d и сырая глубина uShadowDepth%d)
    for (int c = 0; c < SHADOW_CASCADES; c++) {
        if (p->su.u_shadowmap[c] >= 0)
            glUniform1i(p->su.u_shadowmap[c], 3 + c);
        if (p->su.u_shadowdepth[c] >= 0)
            glUniform1i(p->su.u_shadowdepth[c], 3 + c);
    }
    // Окружение/туман шлёт scene_uniforms_bind каждый кадр из rc
    glUseProgram(0);
}

// Hot-reload: пересоздать программы и uniform'ы текущего узла
// Hot-reload: пересоздать программы и uniform'ы текущего узла (replace
// балансирует refcount; 0 — пара не собралась, остаётся прежний id).
static void veg_reshade(void)
{
    if (!s_veg) return;
    GLuint prog = resources_program_replace(s_veg->prog,
                                            "shaders/wheat.vs", "shaders/wheat.fs");
    if (prog) s_veg->prog = prog;
    else SDL_Log("veg: reshade failed, keeping old program");
    GLuint sp = resources_program_replace(s_shadowveg_prog,
                                          "shaders/shadowveg.vs",
                                          "shaders/shadowveg.fs");
    if (sp) s_shadowveg_prog = sp;
    else SDL_Log("veg: shadow program reshade failed, keeping old");
    veg_prog_init(s_veg);
}
static float g_veg_time;         // время симуляции (ветер)
static int g_veg_visible = 1;    // тестовый тумблер отрисовки (perf-харнесс)
static int g_draw_static_main = 1, g_draw_dyn_main = 1;   // FR_TEST: e/f

void veg_set_visible_test(int v) { g_veg_visible = v; }
void veg_set_static_main(int v) { g_draw_static_main = v; }
void veg_set_dyn_main(int v) { g_draw_dyn_main = v; }

// --- Frustum-отсечение супер-чанков ----------------------------------------
// 6 плоскостей пильного конуса из viewproj (Gribb-Hartmann). Матрицы
// column-major (m[c*4+r]): строка r = (m[r], m[4+r], m[8+r], m[12+r]).
// Плоскости: left=row3+row0, right=row3-row0, bottom=row3+row1,
// top=row3-row1, near=row3+row2, far=row3-row2.
static float g_fr[6][4];

static void frustum_extract(const float vp[16])
{
    float r0[4] = { vp[0], vp[4], vp[8],  vp[12] };
    float r1[4] = { vp[1], vp[5], vp[9],  vp[13] };
    float r2[4] = { vp[2], vp[6], vp[10], vp[14] };
    float r3[4] = { vp[3], vp[7], vp[11], vp[15] };
    for (int i = 0; i < 6; i++) {
        const float *a = r3, *b;
        float s;
        switch (i) {
            case 0: b = r0; s =  1.0f; break;
            case 1: b = r0; s = -1.0f; break;
            case 2: b = r1; s =  1.0f; break;
            case 3: b = r1; s = -1.0f; break;
            case 4: b = r2; s =  1.0f; break;
            default: b = r2; s = -1.0f; break;
        }
        float n[4] = { a[0] + s*b[0], a[1] + s*b[1], a[2] + s*b[2],
                       a[3] + s*b[3] };
        float len = sqrtf(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]);
        if (len > 1e-8f)
            for (int k = 0; k < 4; k++) g_fr[i][k] = n[k] / len;
        else
            g_fr[i][0] = g_fr[i][1] = g_fr[i][2] = 0, g_fr[i][3] = 1.0f;
    }
}

// Ящик (по XZ, высота — весь рельеф) виден хоть одним куском конуса?
static int frustum_box(const WorldState *w, float x0, float z0, float x1, float z1)
{
    for (int i = 0; i < 6; i++) {
        const float *pl = g_fr[i];
        // вершина ящика, самая "в сторону" плоскости
        float p[3] = {
            pl[0] > 0.0f ? x1 : x0,
            pl[1] > 0.0f ? w->max_height + 10.0f : w->min_height - 10.0f,
            pl[2] > 0.0f ? z1 : z0
        };
        if (pl[0]*p[0] + pl[1]*p[1] + pl[2]*p[2] + pl[3] < 0.0f)
            return 0;
    }
    return 1;
}

// Плотность на расстоянии r от камеры: у камеры — d0 (как сейчас), дальше
// плавно падает по smoothstep до floor на границе динамического слоя
// (s_dyn_r). Статический слой идёт с той же плотностью floor — на стыке
// динамика/статика нет скачка, переход от игрока к "концу" без рывков.
static float veg_density(float r, float d0, float floor)
{
    // r_veg_falloff_mult сжимает кривую спада: при 0.5 плотность доходит
    // до floor вдвое ближе к камере (картошка: среднее кольцо реже, у ног
    // без изменений)
    float t = r / (s_dyn_r * cvar_get("r_veg_falloff_mult"));
    if (t >= 1.0f) return floor;
    t = t * t * (3.0f - 2.0f * t);   // smoothstep: медленное начало, плавный конец
    return d0 + (floor - d0) * t;
}

// Генерирует растения чанка (cx, cy). Плотность — по расстоянию до камеры
// (camx, camz); если static_run == true, берётся только пол (вся карта).
//
// Травяная ГРУППА = 3 вида травы + одуванчик. Плотность группы = плотность
// травы + СОБСТВЕННАЯ (малая) плотность одуванчиков — они добавляются
// СВЕРХ травы и не разбавляют её. Каждый кандидат группы становится
// одуванчиком с вероятностью d_dande / (d_grass + d_dande) — редкие точки.
// Остальные кандидаты распределяются между 3 видами травы по весам s_kinds.
//
// Экземпляр — 12 float: x y z, nx nz (нормаль земли), yaw, scale, tint,
// layer, lean, dh/dx, dh/dz (уклон земли — для посадки на рельеф без
// выборки текстуры высот в шейдере). Семя зависит от координат чанка —
// повтор даёт ту же траву.
static void veg_gen_chunk(const WorldState *w, int cx, int cy, float camx, float camz,
                          bool static_run, float *gdst, float *wdst,
                          int *gn, int *wn, float *dgo, float *dwo)
{
    float x0 = (float)cx * VEG_CHUNK, z0 = (float)cy * VEG_CHUNK;
    float r = static_run ? 1e9f : sqrtf((x0 + 8.0f - camx) * (x0 + 8.0f - camx)
                                      + (z0 + 8.0f - camz) * (z0 + 8.0f - camz));
    // Плотности у камеры — из cvar'ов (меню «Графика»). Одуванчики — своя
    // малая плотность, добавляется СВЕРХ травы (не ест её долю).
    float dg_g = veg_density(r, cvar_get("r_grass_density"), s_grass_floor);
    float dg_d = veg_density(r, cvar_get("r_dandelion_density"),
                             s_dandelion_floor);
    float dw = veg_density(r, cvar_get("r_wheat_density"), s_wheat_floor);
    float dg = dg_g + dg_d;                      // вся травяная группа
    if (dwo) *dwo = dw;
    if (dgo) *dgo = dg;
    float wmax = dw > 2.0f * dg ? dw : 2.0f * dg;
    int cands = (int)(VEG_CHUNK * VEG_CHUNK * wmax) + 1;
    if (cands > VEG_CAND_CAP) cands = VEG_CAND_CAP;
    unsigned int rng = 0x9e3779b9u ^ (unsigned)((unsigned)cx * 73856093u
                                                ^ (unsigned)cy * 19349663u);
    // Статика и динамика — РАЗНЫЕ сиды: иначе у них совпадают позиции растений
    // (динамика рисуется поверх статики в тех же точках — "наслаивание").
    if (static_run) rng ^= 0x5bd1e995u;
    // Вероятность одуванчика среди травяной группы (редкие пятна).
    float p_dande = dg > 1e-4f ? dg_d / dg : 0.0f;
    if (p_dande > 1.0f) p_dande = 1.0f;
    int gc = 0, wc = 0;
    float e = VEG_SLOPE_E;
    for (int i = 0; i < cands; i++) {
        float x = x0 + frnd(&rng) * VEG_CHUNK;
        float z = z0 + frnd(&rng) * VEG_CHUNK;
        if (x < 1.5f || z < 1.5f || x > w->world_w - 1.5f || z > w->world_d - 1.5f)
            continue;                            // не за край мира
        int wheat = (x >= w->field_x0 && x <= w->field_x0 + w->field_w &&
                     z >= w->field_z0 && z <= w->field_z0 + w->field_d);
        float gy = sample_height(w, x, z);
        if (gy < w->min_height + 4.0f) continue;    // крайние низины — тоже
        // Под водой растительности нет: дно моря/реки/лужи остаётся чистым
        if (water_surface_at(w, x, z) > gy) continue;
        // ЗАНЯТОСТЬ И ПОКРЫТИЕ (splat.c): трава не растёт внутри статических
        // 3D-объектов/моделей и на дорогах; на песке/щебне/асфальте её нет,
        // на грунте она редкая (surfaces.grass). Один множитель на кандидата.
        float cover = splat_grass_factor(x, z);
        if (cover <= 0.001f) continue;
        // уклон и нормаль земли (знаковые разности — как в normal map)
        float sdx = sample_height(w, x + e, z)
                  - sample_height(w, x - e, z);
        float sdz = sample_height(w, x, z + e)
                  - sample_height(w, x, z - e);
        float slope = sqrtf(sdx*sdx + sdz*sdz) / (2.0f * e);
        // Плотность считаем по ПЛОЩАДИ ПОВЕРХНОСТИ, а не по горизонтали:
        // на наклоне горизонтальный квадрат метра меньше, и без поправки
        // трава/пшеница редеют — остаются "дырки". Поправка = 1/cos = sqrt(1+s²).
        float surf = sqrtf(1.0f + slope * slope);
        float want = (wheat ? dw : dg) * surf * cover;
        if (frnd(&rng) >= want / wmax) continue; // прореживание под плотность
        float nx = -sdx, nz = -sdz;              // нормаль: (-dh/dx, 2e, -dh/dz)
        float nl = sqrtf(nx*nx + nz*nz + 4.0f * e * e);
        nx /= nl; nz /= nl;

        float *dst = wheat ? wdst : gdst;
        dst[0] = x;
        // ОСНОВАНИЕ РОВНО НА ЗЕМЛЕ: ни подскока, ни утапливания. Низ спрайта
        // совпадает с поверхностью рельефа (прозрачные поля картинки убирает
        // шейдер, см. veg_base_v в wheat.vs).
        dst[1] = gy;
        dst[2] = z;
        dst[3] = nx;
        dst[4] = nz;
        dst[5] = frnd(&rng) * 6.2831853f;        // случайный поворот вокруг Y
        if (wheat) {
            dst[6] = 0.18f + 0.14f * frnd(&rng); // колос: узкий, 0.18..0.32 м
            dst[7] = 0.90f + 0.20f * frnd(&rng); // тон
            dst[8] = VEG_WHEAT_BASE;             // вид: пшеница (layer 4)
            dst[9] = 0.0f;                       // без наклона
        } else {
            // Травяная группа: одуванчик с вероятностью p_dande (редкий,
            // не разбавляет траву), иначе один из 3 видов по весам.
            int kind = frnd(&rng) < p_dande ? VEG_KIND_DANDELION
                                            : veg_pick_kind(&rng);
            const VegKind *k = &s_kinds[kind];
            dst[6] = k->s0 + (k->s1 - k->s0) * frnd(&rng);   // масштаб (м)
            dst[7] = k->t0 + (k->t1 - k->t0) * frnd(&rng);   // тон
            dst[8] = k->layer;                    // слой спрайта (вид)
            dst[9] = (frnd(&rng) - 0.5f) * k->lean;  // наклон вбок, как в жизни
        }
        dst[10] = sdx / (2.0f * e);              // уклон: dh/dx, dh/dz
        dst[11] = sdz / (2.0f * e);
        if (wheat) { wdst += 12; wc++; }
        else       { gdst += 12; gc++; }
    }
    if (gn) *gn = gc;
    if (wn) *wn = wc;
}

// Настраивает VAO растительности на общий меш и буфер экземпляров
// (см. определение ниже — используется статикой и теневыми буферами).
static void veg_setup_vao(GLuint vao, GLuint inst_vbo);

static void veg_instance_offset(GLuint inst_vbo, int first)
{
    const GLsizei stride = 12 * (GLsizei)sizeof(float);
    const uintptr_t base = (uintptr_t)first * (uintptr_t)stride;
    glBindBuffer(GL_ARRAY_BUFFER, inst_vbo);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, stride, (const void *)(base));
    glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, stride,
                          (const void *)(base + 3 * sizeof(float)));
    glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, stride,
                          (const void *)(base + 5 * sizeof(float)));
    glVertexAttribPointer(5, 1, GL_FLOAT, GL_FALSE, stride,
                          (const void *)(base + 9 * sizeof(float)));
    glVertexAttribPointer(6, 2, GL_FLOAT, GL_FALSE, stride,
                          (const void *)(base + 10 * sizeof(float)));
}

// --- Супер-чанки статики (64 м = 4x4 чанка по 16 м) ------------------------
// Статический слой рисуется НЕ целиком, а только супер-чанками в радиусе
// видимости камеры (glDrawElementsInstancedBaseInstance). Дальше спрайты
// меньше пикселя — их "вид" несёт текстура земли. Экономия: на земле
// рисуется ~10% статики вместо 100%.
#define VEG_SUPER 4          // чанков в стороне супер-чанка
static int g_sc_gw, g_sc_gh, g_sc_n;   // сетка и число супер-чанков
static int *g_sc_cnt_g, *g_sc_cnt_w;   // растений на супер-чанк
static int *g_sc_off_g, *g_sc_off_w;   // префикс-суммы (базы для инстансов)
static float *g_sc_cx, *g_sc_cz;       // центры супер-чанков (метры)
static int g_has_base_instance;        // есть ли ARB_base_instance

// Статичный слой: вся карта, минимальная плотность. Один раз при старте.
// Экземпляры раскладываются ПО СУПЕР-ЧАНКАМ (64 м), чтобы рендер мог
// рисовать только видимые супер-чанки (см. veg_node_render).
static int veg_static_init(VegParams *p)
{
    g_sc_gw = (g_map_w + VEG_SUPER - 1) / VEG_SUPER;
    g_sc_gh = (g_map_d + VEG_SUPER - 1) / VEG_SUPER;
    g_sc_n = g_sc_gw * g_sc_gh;
    g_sc_cnt_g = calloc((size_t)g_sc_n, sizeof(int));
    g_sc_cnt_w = calloc((size_t)g_sc_n, sizeof(int));
    g_sc_off_g = malloc((size_t)(g_sc_n + 1) * sizeof(int));
    g_sc_off_w = malloc((size_t)(g_sc_n + 1) * sizeof(int));
    g_sc_cx = malloc((size_t)g_sc_n * sizeof(float));
    g_sc_cz = malloc((size_t)g_sc_n * sizeof(float));
    g_gen_scratch_g = malloc((size_t)VEG_CAND_CAP * 12 * sizeof(float));
    g_gen_scratch_w = malloc((size_t)VEG_CAND_CAP * 12 * sizeof(float));
    if (!g_sc_cnt_g || !g_sc_cnt_w || !g_sc_off_g || !g_sc_off_w ||
        !g_sc_cx || !g_sc_cz || !g_gen_scratch_g || !g_gen_scratch_w)
        return 0;

    // проход 1: считаем растения на каждый супер-чанк
    for (int sy = 0; sy < g_sc_gh; sy++) {
        for (int sx = 0; sx < g_sc_gw; sx++) {
            g_sc_cx[sy * g_sc_gw + sx] = (float)(sx * VEG_SUPER) * VEG_CHUNK
                                       + VEG_SUPER * VEG_CHUNK * 0.5f;
            g_sc_cz[sy * g_sc_gw + sx] = (float)(sy * VEG_SUPER) * VEG_CHUNK
                                       + VEG_SUPER * VEG_CHUNK * 0.5f;
            int g = 0, w = 0;
            for (int dy = 0; dy < VEG_SUPER; dy++)
                for (int dx = 0; dx < VEG_SUPER; dx++) {
                    int i = sx * VEG_SUPER + dx, j = sy * VEG_SUPER + dy;
                    if (i >= g_map_w || j >= g_map_d) continue;
                    int gn = 0, wn = 0;
                    veg_gen_chunk(p->world, i, j, 0, 0, true,
                                   g_gen_scratch_g, g_gen_scratch_w,
                                   &gn, &wn, NULL, NULL);
                    g += gn; w += wn;
                }
            g_sc_cnt_g[sy * g_sc_gw + sx] = g;
            g_sc_cnt_w[sy * g_sc_gw + sx] = w;
        }
    }
    // префикс-суммы: базы инстансов для glDrawElementsInstancedBaseInstance
    g_sc_off_g[0] = g_sc_off_w[0] = 0;
    for (int s = 0; s < g_sc_n; s++) {
        g_sc_off_g[s + 1] = g_sc_off_g[s] + g_sc_cnt_g[s];
        g_sc_off_w[s + 1] = g_sc_off_w[s] + g_sc_cnt_w[s];
    }
    g_static_gc = g_sc_off_g[g_sc_n];
    g_static_wc = g_sc_off_w[g_sc_n];
    g_static_g = g_static_gc ? malloc((size_t)g_static_gc * 12 * sizeof(float)) : NULL;
    g_static_w = g_static_wc ? malloc((size_t)g_static_wc * 12 * sizeof(float)) : NULL;
    if ((g_static_gc && !g_static_g) || (g_static_wc && !g_static_w)) return 0;

    // проход 2: заполняем буферы (генерация детерминирована — те же счётчики)
    int pos_g = 0, pos_w = 0;
    for (int sy = 0; sy < g_sc_gh; sy++) {
        for (int sx = 0; sx < g_sc_gw; sx++) {
            for (int dy = 0; dy < VEG_SUPER; dy++)
                for (int dx = 0; dx < VEG_SUPER; dx++) {
                    int i = sx * VEG_SUPER + dx, j = sy * VEG_SUPER + dy;
                    if (i >= g_map_w || j >= g_map_d) continue;
                    int gn = 0, wn = 0;
                    veg_gen_chunk(p->world, i, j, 0, 0, true,
                                   g_gen_scratch_g, g_gen_scratch_w,
                                   &gn, &wn, NULL, NULL);
                    if (gn) memcpy(g_static_g + (size_t)pos_g * 12,
                                   g_gen_scratch_g,
                                   (size_t)gn * 12 * sizeof(float));
                    if (wn) memcpy(g_static_w + (size_t)pos_w * 12,
                                   g_gen_scratch_w,
                                   (size_t)wn * 12 * sizeof(float));
                    pos_g += gn;
                    pos_w += wn;
                }
        }
    }
    glBindBuffer(GL_ARRAY_BUFFER, p->vbo_sg);
    glBufferData(GL_ARRAY_BUFFER, (size_t)g_static_gc * 12 * sizeof(float),
                 g_static_g, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, p->vbo_sw);
    glBufferData(GL_ARRAY_BUFFER, (size_t)g_static_wc * 12 * sizeof(float),
                 g_static_w, GL_STATIC_DRAW);
    SDL_Log("Veg static: %d grass, %d wheat (whole map, %d super-chunks)",
            g_static_gc, g_static_wc, g_sc_n);
    return 1;
}

// Предпрореженный статический слой для теней: каждый 2-й экземпляр.
// Тени должны совпадать с видимыми растениями (иначе выглядит "криво"),
// поэтому прореживание минимальное — вдвое.
// Буфер строится ПО СУПЕР-ЧАНКАМ (в том же порядке, что основной статик):
// теневой проход рисует только супер-чанки внутри бокса каскада и не гонит
// через вершинный шейдер 318 тыс. экземпляров ради 10 видимых.
static int *g_sc_shcnt[2], *g_sc_shoff[2];   // предпрореженные диапазоны
static void veg_shadow_build(void)
{
    g_sc_shcnt[0] = calloc((size_t)g_sc_n, sizeof(int));
    g_sc_shcnt[1] = calloc((size_t)g_sc_n, sizeof(int));
    g_sc_shoff[0] = malloc((size_t)(g_sc_n + 1) * sizeof(int));
    g_sc_shoff[1] = malloc((size_t)(g_sc_n + 1) * sizeof(int));
    for (int q = 0; q < 2; q++) {
        const float *src = q == 0 ? g_static_g : g_static_w;
        int srcn = q == 0 ? g_static_gc : g_static_wc;
        int n = (srcn + 1) / 2;
        float *thin = malloc((size_t)n * 12 * sizeof(float));
        int k = 0;
        g_sc_shoff[q][0] = 0;
        for (int s = 0; s < g_sc_n; s++) {
            int a = q == 0 ? g_sc_off_g[s] : g_sc_off_w[s];
            int b = q == 0 ? g_sc_off_g[s + 1] : g_sc_off_w[s + 1];
            int start = a + (a & 1);   // первый ЧЁТНЫЙ индекс в [a,b): правило
            // глобального прореживания i%2==0 должно сохраниться в точности
            g_sc_shcnt[q][s] = start < b ? (b - start + 1) / 2 : 0;
            for (int i = start; i < b; i += 2)
                memcpy(thin + (size_t)k++ * 12, src + (size_t)i * 12,
                       12 * sizeof(float));
            g_sc_shoff[q][s + 1] = k;
        }
        g_veg_shadow_cnt[q] = n;
        glGenVertexArrays(1, &g_veg_shadow_vao[q]);
        glGenBuffers(1, &g_veg_shadow_vbo[q]);
        veg_setup_vao(g_veg_shadow_vao[q], g_veg_shadow_vbo[q]);
        glBindBuffer(GL_ARRAY_BUFFER, g_veg_shadow_vbo[q]);
        glBufferData(GL_ARRAY_BUFFER, (size_t)n * 12 * sizeof(float),
                     thin, GL_STATIC_DRAW);
        free(thin);
    }
    // Теневые VAO динамики (прореженные копии vbo_dg/vbo_dw — заполняются
    // в veg_update при перестройке буферов).
    for (int q = 2; q < 4; q++) {
        glGenVertexArrays(1, &g_veg_shadow_vao[q]);
        glGenBuffers(1, &g_veg_shadow_vbo[q]);
        veg_setup_vao(g_veg_shadow_vao[q], g_veg_shadow_vbo[q]);
        glBindBuffer(GL_ARRAY_BUFFER, g_veg_shadow_vbo[q]);
        glBufferData(GL_ARRAY_BUFFER, 1, NULL, GL_STREAM_DRAW);
    }
}

// Динамический слой: при переходе камеры пересоздаёт только те чанки,
// чья плотность заметно изменилась, и заливает их в VBO.
static void veg_dyn_thin_build(int need[][2], int nn, VegParams *p);
static void veg_update(const float cam_pos[3], VegParams *p)
{
    int cx = (int)floorf(cam_pos[0] / VEG_CHUNK);
    int cy = (int)floorf(cam_pos[2] / VEG_CHUNK);
    // Карта занятости могла измениться (поставили модель командой glb/mesh —
    // splat_occupy_* повышает версию): динамические чанки надо пересобрать,
    // чтобы трава ушла из-под новой модели. Сравниваем версию со своей.
    static unsigned int s_occ_seen = 0;
    unsigned int occ_now = splat_occupancy_version();
    int occ_dirty = (occ_now != s_occ_seen);
    if (occ_dirty) s_occ_seen = occ_now;
    if (!occ_dirty && g_regen_pending <= 0 &&
        cx == g_chunk_cam[0] && cy == g_chunk_cam[1]) return;
    g_chunk_cam[0] = cx;
    g_chunk_cam[1] = cy;

    float dr2 = s_dyn_r * s_dyn_r;
    // список нужных чанков (в радиусе), в порядке индексов
    int need[512][2], nn = 0;
    for (int j = 0; j < g_map_d && nn < 512; j++)
        for (int i = 0; i < g_map_w && nn < 512; i++) {
            float dx = (float)(i * 16 + 8) - cam_pos[0];
            float dz = (float)(j * 16 + 8) - cam_pos[2];
            if (dx*dx + dz*dz <= dr2) { need[nn][0] = i; need[nn][1] = j; nn++; }
        }
    // выгружаем чанки, вышедшие из радиуса
    int dirty = 0;
    for (int idx = 0; idx < g_map_w * g_map_d; idx++) {
        VegChunk *c = &g_veg_chunks[idx];
        if (!c->g && !c->w) continue;
        bool keep = false;
        for (int k = 0; k < nn && !keep; k++)
            if (need[k][1] * g_map_w + need[k][0] == idx) keep = true;
        if (!keep) {
            free(c->g); free(c->w);
            c->g = NULL; c->w = NULL; c->gn = c->wn = 0;
            dirty = 1;
        }
    }
    // Ближние чанки — первыми (бюджет может срезать дальние до следующего
    // кадра; у ног растительность должна появляться сразу)
    for (int a = 1; a < nn; a++) {
        int bi = need[a][0], bj = need[a][1];
        float bd = (float)(bi * 16 + 8) - cam_pos[0];
        float bz = (float)(bj * 16 + 8) - cam_pos[2];
        float br2 = bd * bd + bz * bz;
        int b2 = a - 1;
        while (b2 >= 0) {
            float ddx = (float)(need[b2][0] * 16 + 8) - cam_pos[0];
            float ddz = (float)(need[b2][1] * 16 + 8) - cam_pos[2];
            if (ddx * ddx + ddz * ddz <= br2) break;
            need[b2 + 1][0] = need[b2][0];
            need[b2 + 1][1] = need[b2][1];
            b2--;
        }
        need[b2 + 1][0] = bi;
        need[b2 + 1][1] = bj;
    }
    // пересоздаём чанки, чья плотность заметно изменилась
    int changed = 0;
    int budget = (int)cvar_get("r_veg_regen_budget");
    if (budget < 1) budget = 1;
    if (budget > 2) budget = 2;
    g_regen_pending = 0;
    for (int k = 0; k < nn; k++) {
        int i = need[k][0], j = need[k][1];
        VegChunk *c = &g_veg_chunks[j * g_map_w + i];
        float dx = (float)(i * 16 + 8) - cam_pos[0];
        float dz = (float)(j * 16 + 8) - cam_pos[2];
        float r = sqrtf(dx*dx + dz*dz);
        float tgd = veg_density(r, cvar_get("r_grass_density"), s_grass_floor)
                  + veg_density(r, cvar_get("r_dandelion_density"),
                                s_dandelion_floor);
        float twd = veg_density(r, cvar_get("r_wheat_density"), s_wheat_floor);
        // ВАЖНО: та же формула плотности, что пишет veg_gen_chunk (травяная
        // группа = трава + одуванчики, из cvar'ов). Раньше цель считалась
        // из map.json-констант (s_*_density), а чанк запоминал плотность из
        // cvar'ов — из-за рассогласования КАЖДЫЙ чанк не проходил допуск, и
        // вся растительность пересоздавалась на каждом переходе чанка
        // (regen storm).
        bool need_gen = (!c->g && !c->w) || occ_dirty ||
                        fabsf(c->dg_d - tgd) > VEG_REGEN_TOL ||
                        fabsf(c->dw_d - twd) > VEG_REGEN_TOL;
        if (need_gen) {
            if (changed >= budget) {   // хвост — в следующих кадрах
                g_regen_pending = 1;
                continue;
            }
            if (!g_gen_scratch_g)
                g_gen_scratch_g = malloc((size_t)VEG_CAND_CAP * 12 * sizeof(float));
            if (!g_gen_scratch_w)
                g_gen_scratch_w = malloc((size_t)VEG_CAND_CAP * 12 * sizeof(float));
            if (!g_gen_scratch_g || !g_gen_scratch_w) {
                SDL_Log("Vegetation: generation scratch allocation failed");
                g_regen_pending = 1;
                return;
            }
            int gn = 0, wn = 0;
            float dg_d, dw_d;
            veg_gen_chunk(p->world, i, j, cam_pos[0], cam_pos[2], false,
                          g_gen_scratch_g, g_gen_scratch_w, &gn, &wn,
                          &dg_d, &dw_d);
            float *gb = gn ? malloc((size_t)gn * 12 * sizeof(float)) : NULL;
            float *wb = wn ? malloc((size_t)wn * 12 * sizeof(float)) : NULL;
            if ((gn && !gb) || (wn && !wb)) {
                free(gb); free(wb);
                SDL_Log("Vegetation: chunk allocation failed");
                g_regen_pending = 1;
                return;
            }
            if (gn) memcpy(gb, g_gen_scratch_g, (size_t)gn * 12 * sizeof(float));
            if (wn) memcpy(wb, g_gen_scratch_w, (size_t)wn * 12 * sizeof(float));
            free(c->g); free(c->w);
            c->g = gb; c->w = wb; c->gn = gn; c->wn = wn;
            c->dg_d = dg_d; c->dw_d = dw_d;
            changed++;
            dirty = 1;
        }
    }
    // При хвосте бюджета бывают кадры без готового нового чанка. Раньше они
    // всё равно копировали всю динамику и переопределяли VBO/SSBO/тени.
    if (!dirty) return;
    // плоский динамический буфер -> VBO
    int gc = 0, wc = 0;
    for (int k = 0; k < nn; k++) {
        VegChunk *c = &g_veg_chunks[need[k][1] * g_map_w + need[k][0]];
        gc += c->gn; wc += c->wn;
    }
    if (gc > g_dyn_cap || wc > g_dyn_cap) {
        int cap = (int)((gc > wc ? gc : wc) * 1.2f) + 1;
        float *ng = realloc(g_dyn_g, (size_t)cap * 12 * sizeof(float));
        if (!ng) {
            SDL_Log("Vegetation: dynamic grass buffer allocation failed");
            return;
        }
        g_dyn_g = ng;
        float *nw = realloc(g_dyn_w, (size_t)cap * 12 * sizeof(float));
        if (!nw) {
            SDL_Log("Vegetation: dynamic wheat buffer allocation failed");
            return;
        }
        g_dyn_w = nw;
        g_dyn_cap = cap;
    }
    float *gd = g_dyn_g, *wd = g_dyn_w;
    for (int k = 0; k < nn; k++) {
        VegChunk *c = &g_veg_chunks[need[k][1] * g_map_w + need[k][0]];
        if (c->gn) { memcpy(gd, c->g, (size_t)c->gn * 12 * sizeof(float)); gd += (size_t)c->gn * 12; }
        if (c->wn) { memcpy(wd, c->w, (size_t)c->wn * 12 * sizeof(float)); wd += (size_t)c->wn * 12; }
    }
    g_dyn_gc = gc;
    g_dyn_wc = wc;
    glBindBuffer(GL_ARRAY_BUFFER, p->vbo_dg);
    glBufferData(GL_ARRAY_BUFFER, (size_t)gc * 12 * sizeof(float), g_dyn_g, GL_STREAM_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, p->vbo_dw);
    glBufferData(GL_ARRAY_BUFFER, (size_t)wc * 12 * sizeof(float), g_dyn_w, GL_STREAM_DRAW);
    // GPU-driven: те же динамические слои — в Src-SSBO для compute-cull.
    veg_gpu_src_upload(2, g_dyn_g, g_dyn_gc);
    veg_gpu_src_upload(3, g_dyn_w, g_dyn_wc);
    // Прореженные копии для теней: ровно правило uThin из shadowveg.vs
    // ((flat_index*2654435761)%100<50) — карты теней байт-в-байт те же, но
    // вершинный этап теневого прохода обрабатывает вдвое меньше экземпляров.
    // Диапазоны запоминаются ПО ЧАНКАМ (в порядке need-списка): каскады 0/1
    // (боксы 8/16 м) рисуют только чанки внутри бокса, а не все 512.
    veg_dyn_thin_build(need, nn, p);
    if (changed)
        SDL_Log("Veg dyn: %d grass, %d wheat (%d chunks regen)", gc, wc, changed);
}

// Центры и предпрореженные диапазоны динамических чанков (для теней)
static float g_dyn_sh_x[512], g_dyn_sh_z[512];
static int g_dyn_sh_go[512], g_dyn_sh_gc[512];
static int g_dyn_sh_wo[512], g_dyn_sh_wc[512];
static int g_dyn_sh_n;

// Правило прореживания uThin из shadowveg.vs для ГЛОБАЛЬНОГО индекса
// буфера (одинаковое для grass и wheat): hash(id*2654435761)%100 < 50
static int g_dyn_sh_keep(int idx)
{
    return (unsigned)idx * 2654435761u % 100u < 50u;
}

static void veg_dyn_thin_build(int need[][2], int nn, VegParams *p)
{
    (void)p;
    g_dyn_sh_n = nn;
    if (nn == 0) return;
    int main_g0[512], main_w0[512];
    // 1) flat-старты чанков в ОСНОВНОМ буфере (hash-правило — по ним)
    int sgo = 0, swo = 0;
    for (int k = 0; k < nn; k++) {
        VegChunk *c = &g_veg_chunks[need[k][1] * g_map_w + need[k][0]];
        main_g0[k] = sgo;  main_w0[k] = swo;
        sgo += c->gn;      swo += c->wn;
    }
    // 2) сколько экземпляров остаётся после прореживания, тонкие старты,
    //    центры чанков
    int og = 0, ow = 0;
    for (int k = 0; k < nn; k++) {
        VegChunk *c = &g_veg_chunks[need[k][1] * g_map_w + need[k][0]];
        int g0 = main_g0[k], w0 = main_w0[k];
        int gc2 = 0, wc2 = 0;
        for (int t = 0; t < c->gn; t++) if (g_dyn_sh_keep(g0 + t)) gc2++;
        for (int t = 0; t < c->wn; t++) if (g_dyn_sh_keep(w0 + t)) wc2++;
        g_dyn_sh_go[k] = og;  og += gc2;
        g_dyn_sh_wo[k] = ow;  ow += wc2;
        g_dyn_sh_gc[k] = gc2;
        g_dyn_sh_wc[k] = wc2;
        g_dyn_sh_x[k] = (float)need[k][0] * VEG_CHUNK + 8.0f;
        g_dyn_sh_z[k] = (float)need[k][1] * VEG_CHUNK + 8.0f;
    }
    g_veg_shadow_cnt[2] = og;
    g_veg_shadow_cnt[3] = ow;
    // 3) заполняем тонкие буферы в порядке need-списка (тот же порядок
    //    экземпляров, что и раньше: карты теней идентичны)
    if (og > g_shadow_scratch_gcap) {
        g_shadow_scratch_gcap = og + og / 4 + 1;
        g_shadow_scratch_g = realloc(g_shadow_scratch_g,
            (size_t)g_shadow_scratch_gcap * 12 * sizeof(float));
    }
    if (ow > g_shadow_scratch_wcap) {
        g_shadow_scratch_wcap = ow + ow / 4 + 1;
        g_shadow_scratch_w = realloc(g_shadow_scratch_w,
            (size_t)g_shadow_scratch_wcap * 12 * sizeof(float));
    }
    float *tg = g_shadow_scratch_g;
    float *tw = g_shadow_scratch_w;
    int kg = 0, kw = 0;
    for (int k = 0; k < nn; k++) {
        VegChunk *c = &g_veg_chunks[need[k][1] * g_map_w + need[k][0]];
        int g0 = main_g0[k], w0 = main_w0[k];
        for (int t = 0; t < c->gn; t++)
            if (g_dyn_sh_keep(g0 + t)) {
                memcpy(tg + (size_t)kg * 12, c->g + (size_t)t * 12,
                       12 * sizeof(float));
                kg++;
            }
        for (int t = 0; t < c->wn; t++)
            if (g_dyn_sh_keep(w0 + t)) {
                memcpy(tw + (size_t)kw * 12, c->w + (size_t)t * 12,
                       12 * sizeof(float));
                kw++;
            }
    }
    glBindBuffer(GL_ARRAY_BUFFER, g_veg_shadow_vbo[2]);
    glBufferData(GL_ARRAY_BUFFER, (size_t)og * 12 * sizeof(float), tg, GL_STREAM_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, g_veg_shadow_vbo[3]);
    glBufferData(GL_ARRAY_BUFFER, (size_t)ow * 12 * sizeof(float), tw, GL_STREAM_DRAW);
}

// Теневой проход (RP_SHADOW): прореженные буферы, диапазоны по
// супер-чанкам/чанкам внутри бокса каскада (логика переехала из shadow.c).
static void veg_shadow_draw(const RendererContext *rc)
{
    if (cvar_get("r_simple_render") > 0.5f && rc->shadow_cascade > 0) return;
    glDisable(GL_CULL_FACE);     // биллборды — с двух сторон
    glUseProgram(s_shadowveg_prog);
    glUniformMatrix4fv(u_veg_viewproj, 1, GL_FALSE, rc->shadow_vp);
    glUniform1i(u_veg_veg, 0);
    glUniform1f(u_veg_time, g_veg_time);
    glUniform3f(u_veg_campos, rc->cam_pos[0], rc->cam_pos[1], rc->cam_pos[2]);
    glUniform1f(u_veg_walk, rc->cam_fly ? 0.0f : 1.0f);
    glUniform1f(u_veg_windboost, rc->wetness);   // тени синхронны с ветром
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, g_veg_tex);
    glUniform1i(u_veg_thin, 0);    // прореживание уже сделано на CPU

    // Статика: полный LOD (в т.ч. lod2). Только супер-чанки внутри бокса.
    glUniform1f(u_veg_dyn, 0.0f);
    if (rc->shadow_static && g_shadow_veg) {
        if (rc->shadow_cull && g_has_base_instance) {
            // бокс + полудиагональ супер-чанка (64 м: 32*1.414 ≈ 45.3)
            float r2 = (45.3f + rc->shadow_box_cur * 0.7071f) *
                       (45.3f + rc->shadow_box_cur * 0.7071f);
            float pcx = rc->cam_pos[0], pcz = rc->cam_pos[2];
            for (int s = 0; s < g_sc_n; s++) {
                int cnt = g_sc_shcnt[0][s];
                if (cnt <= 0) continue;
                float dx = g_sc_cx[s] - pcx;
                float dz = g_sc_cz[s] - pcz;
                if (dx*dx + dz*dz > r2) continue;
                glBindVertexArray(g_veg_shadow_vao[0]);
                glDrawElementsInstancedBaseInstance(GL_TRIANGLES, 6,
                        GL_UNSIGNED_INT, NULL, cnt, (GLuint)g_sc_shoff[0][s]);
            }
            for (int s = 0; s < g_sc_n; s++) {
                int cnt = g_sc_shcnt[1][s];
                if (cnt <= 0) continue;
                float dx = g_sc_cx[s] - pcx;
                float dz = g_sc_cz[s] - pcz;
                if (dx*dx + dz*dz > r2) continue;
                glBindVertexArray(g_veg_shadow_vao[1]);
                glDrawElementsInstancedBaseInstance(GL_TRIANGLES, 6,
                        GL_UNSIGNED_INT, NULL, cnt, (GLuint)g_sc_shoff[1][s]);
            }
        } else {
            glBindVertexArray(g_veg_shadow_vao[0]);
            glDrawElementsInstanced(GL_TRIANGLES, 6, GL_UNSIGNED_INT, NULL,
                                    g_veg_shadow_cnt[0]);
            glBindVertexArray(g_veg_shadow_vao[1]);
            glDrawElementsInstanced(GL_TRIANGLES, 6, GL_UNSIGNED_INT, NULL,
                                    g_veg_shadow_cnt[1]);
        }
    }
    // Динамика: только lod0/lod1 (без lod2). Чанки внутри бокса.
    glUniform1f(u_veg_dyn, 1.0f);
    if (rc->shadow_dyn && g_shadow_veg) {
        if (rc->shadow_cull && g_has_base_instance) {
            // бокс + полудиагональ чанка (16 м: 11.3)
            float r2 = (11.3f + rc->shadow_box_cur * 0.7071f) *
                       (11.3f + rc->shadow_box_cur * 0.7071f);
            float pcx = rc->cam_pos[0], pcz = rc->cam_pos[2];
            for (int k = 0; k < g_dyn_sh_n; k++) {
                float dx = g_dyn_sh_x[k] - pcx;
                float dz = g_dyn_sh_z[k] - pcz;
                if (dx*dx + dz*dz > r2) continue;
                if (g_dyn_sh_gc[k] > 0) {
                    glBindVertexArray(g_veg_shadow_vao[2]);
                    glDrawElementsInstancedBaseInstance(GL_TRIANGLES, 6,
                            GL_UNSIGNED_INT, NULL, g_dyn_sh_gc[k],
                            (GLuint)g_dyn_sh_go[k]);
                }
                if (g_dyn_sh_wc[k] > 0) {
                    glBindVertexArray(g_veg_shadow_vao[3]);
                    glDrawElementsInstancedBaseInstance(GL_TRIANGLES, 6,
                            GL_UNSIGNED_INT, NULL, g_dyn_sh_wc[k],
                            (GLuint)g_dyn_sh_wo[k]);
                }
            }
        } else {
            glBindVertexArray(g_veg_shadow_vao[2]);
            glDrawElementsInstanced(GL_TRIANGLES, 6, GL_UNSIGNED_INT, NULL,
                                    g_veg_shadow_cnt[2]);
            glBindVertexArray(g_veg_shadow_vao[3]);
            glDrawElementsInstanced(GL_TRIANGLES, 6, GL_UNSIGNED_INT, NULL,
                                    g_veg_shadow_cnt[3]);
        }
    }
    glEnable(GL_CULL_FACE);
}

void veg_node_render(SceneNode *n, float dt)
{
    (void)dt;
    // Полное отключение травы одной галкой — раньше floor=1.0 оставлял поле при density 0
    if (cvar_get("r_wheat_density") < 0.01f && cvar_get("r_grass_density") < 0.01f &&
        cvar_get("r_dandelion_density") < 0.01f) {
        return;
    }
    VegParams *p = n->params;
    const RendererContext *rc = p->rc;
    if (rc->pass == RP_SHADOW) {
        veg_shadow_draw(rc);
        return;
    }
    g_veg_time += dt * (1.0f + fx_wetness());
    // Плотности из cvar'ов изменились (меню «Графика») — перегенерить чанки:
    // сброс позиции камеры-чанка заставляет veg_update пересоздать всё.
    {
        static float s_prev_w = -1.0f, s_prev_g = -1.0f, s_prev_d = -1.0f,
                     s_prev_fo = -1.0f;
        float w = cvar_get("r_wheat_density");
        float g = cvar_get("r_grass_density");
        float d = cvar_get("r_dandelion_density");
        float fo = cvar_get("r_veg_falloff_mult");
        if (w != s_prev_w || g != s_prev_g || d != s_prev_d ||
            fo != s_prev_fo) {
            s_prev_w = w; s_prev_g = g; s_prev_d = d; s_prev_fo = fo;
            g_chunk_cam[0] = g_chunk_cam[1] = INT_MAX;
            SDL_Log("Veg density changed: wheat %.0f, grass %.1f, dandelion %.2f",
                    w, g, d);
        }
    }
    veg_update(rc->cam_pos, p);
    if (g_static_gc + g_static_wc + g_dyn_gc + g_dyn_wc <= 0) return;
    if (!g_veg_visible) return;
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);             // биллборды видны с обеих сторон
    glUseProgram(p->prog);
    // MSAA не сглаживает alpha-cutout края спрайтов (discard) — включаем
    // alpha-to-coverage: полупрозрачная кромка текселя даёт частичное
    // покрытие сэмплов и край сглаживается. На теневую карту не влияет.
    int a2c = bloom_msaa_active() > 0;
    if (a2c) glEnable(GL_SAMPLE_ALPHA_TO_COVERAGE);
    // Общий набор: виды, свет, окружение, туман, каскады теней (из rc)
    scene_uniforms_bind(&p->su, rc);
    // Растения: свои bias/градиент (shadow_bias_veg из map.json — тени
    // вплотную к растениям, без слан-терма: нормаль биллборда бессмысленна)
    for (int c = 0; c < SHADOW_CASCADES; c++) {
        if (p->su.u_shadowbias[c] >= 0)
            glUniform1f(p->su.u_shadowbias[c], rc->shadow_bias_veg[c]);
        if (p->su.u_shadowgrad[c] >= 0)
            glUniform1f(p->su.u_shadowgrad[c], rc->shadow_grad_veg[c]);
    }
    glUniform1f(p->u_time, g_veg_time);
    glUniform1f(p->u_walk, rc->cam_fly ? 0.0f : 1.0f);
    glUniform1f(p->u_windboost, rc->wetness);
    if (p->u_lodshift >= 0) glUniform1f(p->u_lodshift, cvar_get("r_veg_lod_shift"));
    // Frustum-отсечение статики: 6 плоскостей конуса из viewproj (метод
    // Gribb-Hartmann, матрицы column-major — строки m[c*4+r]).
    frustum_extract(rc->viewproj);
    glBindTexture(GL_TEXTURE_2D_ARRAY, p->tex);

    // GPU-driven (compute-cull + indirect): вершины ограничены видимыми
    // пикселями, а не плотностью. Затраты «миллиард» ≈ «1».
    if (g_veg_gpudriven && (int)cvar_get("r_veg_gpudriven") > 0.5f) {
        veg_gpu_draw(p, rc);
        glEnable(GL_CULL_FACE);
        if (a2c) glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
        return;
    }

    // Иначе — инстансный фолбэк (старый путь).
    // Статика: только супер-чанки в радиусе видимости (вместо всей карты).
    // Дальше спрайты меньше пикселя — их вид несёт текстура земли. При
    // подъёме камеры радиус растёт, чтобы обзор с горы не пустел.
    glUniform1f(p->u_dyn, 0.0f);   // статика: полный LOD (в т.ч. lod2)
    if (g_draw_static_main) {
        float vis = s_vis_r * cvar_get("r_veg_vis_mult");
        if (rc->cam_pos[1] > VEG_VIS_H)
            vis += (rc->cam_pos[1] - VEG_VIS_H) * VEG_VIS_F;
        float vr2 = vis * vis;
        float half = VEG_SUPER * VEG_CHUNK * 0.5f;
        for (int pass = 0; pass < 2; pass++) {
            GLuint vao = pass ? p->vao_sw : p->vao_sg;
            GLuint vbo = pass ? p->vbo_sw : p->vbo_sg;
            glBindVertexArray(vao);
            for (int s = 0; s < g_sc_n; s++) {
                int cnt = pass ? g_sc_cnt_w[s] : g_sc_cnt_g[s];
                if (cnt <= 0) continue;
                if (!frustum_box(p->world, g_sc_cx[s] - half, g_sc_cz[s] - half,
                                 g_sc_cx[s] + half, g_sc_cz[s] + half)) continue;
                float dx = g_sc_cx[s] - rc->cam_pos[0];
                float dz = g_sc_cz[s] - rc->cam_pos[2];
                if (dx*dx + dz*dz > vr2) continue;
                GLuint first = (GLuint)(pass ? g_sc_off_w[s] : g_sc_off_g[s]);
                if (g_has_base_instance) {
                    glDrawElementsInstancedBaseInstance(GL_TRIANGLES, 6,
                            GL_UNSIGNED_INT, NULL, cnt, first);
                } else {
                    veg_instance_offset(vbo, (int)first);
                    glDrawElementsInstanced(GL_TRIANGLES, 6,
                                            GL_UNSIGNED_INT, NULL, cnt);
                }
            }
            if (!g_has_base_instance) veg_instance_offset(vbo, 0);
        }
    }
    glUniform1f(p->u_dyn, 1.0f);   // динамика: только lod0/lod1 (без lod2)
    if (g_draw_dyn_main) {
        glBindVertexArray(p->vao_dg);
        glDrawElementsInstanced(GL_TRIANGLES, 6, GL_UNSIGNED_INT, NULL, g_dyn_gc);
        glBindVertexArray(p->vao_dw);
        glDrawElementsInstanced(GL_TRIANGLES, 6, GL_UNSIGNED_INT, NULL, g_dyn_wc);
    }
    glEnable(GL_CULL_FACE);
    if (a2c) glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
}

int veg_count(int q)
{
    const int cnt[4] = { g_static_gc, g_static_wc, g_dyn_gc, g_dyn_wc };
    return cnt[q];
}

// Теневой проход: рисовать только супер-чанки внутри бокса каскада —
// возвращает диапазон супер-чанков [*first, *last) для сброса глобального
// счётчика (супер-чанки идут подряд, culling идёт по ним).
// Динамика в тенях: чанки и их предпрореженные диапазоны.
// Настраивает VAO растительности на общий меш (квад 4 вершины, 6 индексов)
// и буфер экземпляров (12 float: pos3, нормаль2, yaw/scale/tint/layer, lean,
// slope2 — см. veg_gen_chunk). Используется для всех VAO: обычных и теневых.
static void veg_setup_vao(GLuint vao, GLuint inst_vbo)
{
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_veg_mesh_vbo);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), NULL);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float),
                          (const void *)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_veg_mesh_ibo);
    glBindBuffer(GL_ARRAY_BUFFER, inst_vbo);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, 12 * sizeof(float), NULL);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, 12 * sizeof(float),
                          (const void *)(3 * sizeof(float)));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, 12 * sizeof(float),
                          (const void *)(5 * sizeof(float)));
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(5, 1, GL_FLOAT, GL_FALSE, 12 * sizeof(float),
                          (const void *)(9 * sizeof(float)));
    glEnableVertexAttribArray(5);
    glVertexAttribPointer(6, 2, GL_FLOAT, GL_FALSE, 12 * sizeof(float),
                          (const void *)(10 * sizeof(float)));
    glEnableVertexAttribArray(6);
    glVertexAttribDivisor(2, 1);
    glVertexAttribDivisor(3, 1);
    glVertexAttribDivisor(4, 1);
    glVertexAttribDivisor(5, 1);
    glVertexAttribDivisor(6, 1);
    glBindVertexArray(0);
}

// --- GPU-driven: compute-cull + indirect draw -------------------------------
// Создаёт SSBO/indirect/VAO и компилирует compute-шейдер отсечения. При
// любой ошибке выключает путь (g_veg_gpudriven = 0) — останется инстансный
// фолбэк. Вызывается из veg_make_node, только если контекст >= GL 4.3.
static void veg_gpu_init(VegParams *p)
{
    (void)p;
    s_cull_prog = make_compute_program("shaders/veg_cull.cs");
    if (!s_cull_prog) { g_veg_gpudriven = 0; return; }
    u_cull_vp     = glGetUniformLocation(s_cull_prog, "uViewProj");
    u_cull_viewh  = glGetUniformLocation(s_cull_prog, "uViewH");
    u_cull_tan    = glGetUniformLocation(s_cull_prog, "uTanHalfFov");
    u_cull_minpix = glGetUniformLocation(s_cull_prog, "uMinPix");
    u_cull_lodshift = glGetUniformLocation(s_cull_prog, "uLodShift");
    u_cull_src    = glGetUniformLocation(s_cull_prog, "uSrcCount");
    u_cull_off    = glGetUniformLocation(s_cull_prog, "uOffset");
    u_cull_planes = glGetUniformLocation(s_cull_prog, "uPlanes");
    u_cull_campos = glGetUniformLocation(s_cull_prog, "uCamPos");
    u_cull_capacity = glGetUniformLocation(s_cull_prog, "uDstCapacity");
    // Привязка SSBO-блоков compute-программы к binding points 0/1/2.
    GLuint bsrc = (p_glGetProgramResourceIndex)
                  ? p_glGetProgramResourceIndex(s_cull_prog, GL_SHADER_STORAGE_BLOCK, "Src")
                  : GL_INVALID_INDEX;
    GLuint bdst = (p_glGetProgramResourceIndex)
                  ? p_glGetProgramResourceIndex(s_cull_prog, GL_SHADER_STORAGE_BLOCK, "Dst")
                  : GL_INVALID_INDEX;
    GLuint bind = (p_glGetProgramResourceIndex)
                  ? p_glGetProgramResourceIndex(s_cull_prog, GL_SHADER_STORAGE_BLOCK, "Indirect")
                  : GL_INVALID_INDEX;
    if (bsrc == GL_INVALID_INDEX || bdst == GL_INVALID_INDEX || bind == GL_INVALID_INDEX) {
        SDL_Log("veg: SSBO block binding failed, GPU-driven off");
        glDeleteProgram(s_cull_prog);
        s_cull_prog = 0; g_veg_gpudriven = 0; return;
    }
    glShaderStorageBlockBinding(s_cull_prog, bsrc, 0);
    glShaderStorageBlockBinding(s_cull_prog, bdst, 1);
    glShaderStorageBlockBinding(s_cull_prog, bind, 2);
    for (int q = 0; q < 4; q++) {
        g_veg_src_cnt[q] = 0;
        glGenBuffers(1, &g_veg_src_ssbo[q]);
        glGenBuffers(1, &g_veg_cull_ssbo[q]);
        // Cull-SSBO должен существовать ДО VAO (инстансный буфер): 1 float.
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_veg_cull_ssbo[q]);
        glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(float), NULL, GL_DYNAMIC_COPY);
        glGenBuffers(1, &g_veg_indirect[q]);
        GLuint cmd[5] = { 6, 0, 0, 0, 0 };   // count=6 (квад), instanceCount=0
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, g_veg_indirect[q]);
        glBufferData(GL_DRAW_INDIRECT_BUFFER, sizeof(cmd), cmd, GL_DYNAMIC_DRAW);
        glGenVertexArrays(1, &g_veg_vao_cull[q]);
        veg_setup_vao(g_veg_vao_cull[q], g_veg_cull_ssbo[q]);
        glBindVertexArray(0);
    }
    SDL_Log("Veg GPU-driven: compute-cull ENABLED (4 layers)");
}

// Заливает слой q (0=стат трава,1=стат пшен,2=дин трава,3=дин пшен) в Src-SSBO
// и подгоняет под него Cull-SSBO (худший случай — все выжили). Вызывается
// после генерации статического слоя и после каждой пересборки динамики.
static void veg_gpu_src_upload(int q, const float *src, int count)
{
    g_veg_src_cnt[q] = count;
    if (!g_veg_gpudriven || count <= 0) return;
    size_t bytes = (size_t)count * 12 * sizeof(float);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_veg_src_ssbo[q]);
    glBufferData(GL_SHADER_STORAGE_BUFFER, bytes, src, GL_DYNAMIC_COPY);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_veg_cull_ssbo[q]);
    glBufferData(GL_SHADER_STORAGE_BUFFER, bytes, NULL, GL_DYNAMIC_COPY);
}

// GPU-driven проход: cull + indirect-отрисовка 4 слоёв. Затраты на вершины
// ограничены видимыми пикселями (дальние субпиксельные травинки отсекаются
// в compute ещё до вершинного шейдера). СТАТИКА пред-отсекается по супер-
// чанкам ТОЧНО так же, как инстансный фолбэк (frustum + радиус видимости) —
// compute гоняется только по видимым супер-чанкам, визуально набор тот же,
// но перебирается в разы меньше инстансов. ОДИН барьер на все слои.
static void veg_gpu_draw(VegParams *p, const RendererContext *rc)
{
    float fov = rc->cam_fov_deg * 0.0174532925f;
    float tanhf = tanf(0.5f * fov);
    glUseProgram(s_cull_prog);
    glUniformMatrix4fv(u_cull_vp, 1, GL_FALSE, rc->viewproj);
    glUniform1f(u_cull_viewh, (float)rc->vh);
    glUniform1f(u_cull_tan, tanhf);
    // Порог видимости — cvar: 1 px по умолчанию. Отсечение идёт СФЕРОЙ вокруг
    // всего спрайта (см. veg_cull.cs), поэтому растения на кромке экрана
    // остаются видны — раньше тест точкой обрезал их целиком.
    glUniform1f(u_cull_minpix, cvar_get("r_veg_minpix"));
    if (u_cull_lodshift >= 0) glUniform1f(u_cull_lodshift, cvar_get("r_veg_lod_shift"));
    glUniform4fv(u_cull_planes, 6, &rc->frustum.p[0][0]);
    glUniform3f(u_cull_campos, rc->cam_pos[0], rc->cam_pos[1], rc->cam_pos[2]);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, p->tex);

    // --- PASS 1: compute-cull всех слоёв (накопить в indirect) ------------
    for (int q = 0; q < 4; q++) {
        if (g_veg_src_cnt[q] <= 0) continue;
        if ((q < 2 && !g_draw_static_main) || (q >= 2 && !g_draw_dyn_main)) continue;
        // обнулить instanceCount перед накоплением слоя
        GLuint zero = 0;
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, g_veg_indirect[q]);
        glBufferSubData(GL_DRAW_INDIRECT_BUFFER, 4, 4, &zero);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, g_veg_src_ssbo[q]);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, g_veg_cull_ssbo[q]);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, g_veg_indirect[q]);
        if (u_cull_capacity >= 0)
            glUniform1i(u_cull_capacity, g_veg_src_cnt[q]);

        if (q < 2) {
            // СТАТИКА: только видимые супер-чанки (как фолбэк). Это и есть
            // главная экономия: не гонять compute по ВСЕЙ карте каждый кадр.
            float vis = s_vis_r * cvar_get("r_veg_vis_mult");
            if (rc->cam_pos[1] > VEG_VIS_H)
                vis += (rc->cam_pos[1] - VEG_VIS_H) * VEG_VIS_F;
            float vr2 = vis * vis;
            float half = VEG_SUPER * VEG_CHUNK * 0.5f;
            const int *off = q == 0 ? g_sc_off_g : g_sc_off_w;
            const int *cnt = q == 0 ? g_sc_cnt_g : g_sc_cnt_w;
            for (int s = 0; s < g_sc_n; s++) {
                if (cnt[s] <= 0) continue;
                if (!frustum_box(p->world, g_sc_cx[s] - half, g_sc_cz[s] - half,
                                 g_sc_cx[s] + half, g_sc_cz[s] + half)) continue;
                float dx = g_sc_cx[s] - rc->cam_pos[0];
                float dz = g_sc_cz[s] - rc->cam_pos[2];
                if (dx*dx + dz*dz > vr2) continue;
                glUniform1i(u_cull_off, off[s]);
                glUniform1i(u_cull_src, cnt[s]);
                glDispatchCompute((GLuint)((cnt[s] + 63) / 64), 1, 1);
            }
        } else {
            // ДИНАМИКА: весь слой (уже ограничен радиусом вокруг камеры)
            glUniform1i(u_cull_off, 0);
            glUniform1i(u_cull_src, g_veg_src_cnt[q]);
            glDispatchCompute((GLuint)((g_veg_src_cnt[q] + 63) / 64), 1, 1);
        }
    }
    // ОДИН барьер: каждый слой пишет свой буфер, читать будем после всех.
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, 0);

    // --- PASS 2: indirect-отрисовка выживших ------------------------------
    glUseProgram(p->prog);
    if (p->u_lodshift >= 0) glUniform1f(p->u_lodshift, cvar_get("r_veg_lod_shift"));
    for (int q = 0; q < 4; q++) {
        if (g_veg_src_cnt[q] <= 0) continue;
        if ((q < 2 && !g_draw_static_main) || (q >= 2 && !g_draw_dyn_main)) continue;
        glUniform1f(p->u_dyn, q >= 2 ? 1.0f : 0.0f);
        glBindVertexArray(g_veg_vao_cull[q]);
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, g_veg_indirect[q]);
        glDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT, (const void *)0);
    }
    glBindVertexArray(0);
}

SceneNode *veg_make_node(const WorldState *w, const RendererContext *rc)
{
    // Плотности и радиусы — из map/map.json (veg = { ... })
    {
        MapVeg v;
        mapdata_veg(&v);
        s_grass_density = v.grass_density;
        s_wheat_density = v.wheat_density;
        s_grass_floor = v.grass_floor;
        s_wheat_floor = v.wheat_floor;
        s_dandelion_density = v.dandelion_density;
        s_dandelion_floor   = v.dandelion_floor;
        s_falloff_r = v.falloff;
        s_dyn_r = v.dyn_radius;
        s_vis_r = v.vis_radius;
        // Плотности как cvar'ы (меню «Графика»): дефолты из map.json,
        // применяются при перегенерации чанков.
        cvar_register("r_wheat_density", s_wheat_density, "пшеница шт/м2");
        cvar_register("r_grass_density", s_grass_density, "трава шт/м2");
        cvar_register("r_dandelion_density", s_dandelion_density,
                      "одуванчики шт/м2 (сверх травы)");
        // Значения КАРТЫ применяются только к cvar'ам, которые юзер/пресет
        // явно не задавал (CVAR_CFGSET): настройки пользователя важнее карты,
        // а на первом запуске поле выглядит так, как задумал автор карты.
        cvar_set_default("r_wheat_density", s_wheat_density);
        cvar_set_default("r_grass_density", s_grass_density);
        cvar_set_default("r_dandelion_density", s_dandelion_density);
        // Множитель радиуса статической пшеницы: <1 — дальнее поле остаётся
        // только в текстуре земли (режим «КАРТОШКА», минус овердров)
        cvar_register_ex("r_veg_vis_mult", 1.0f, 0.25f, 1.0f, CVAR_ARCHIVE,
                         "множитель радиуса прорисовки пшеницы (1 = карта)");
        cvar_register_ex("r_veg_falloff_mult", 1.0f, 0.3f, 1.0f, CVAR_ARCHIVE,
                         "спад плотности поля: <1 — реже уже к среднему кольцу");
        cvar_register_ex("r_veg_lod_shift", 0.0f, 0.0f, 2.0f, CVAR_ARCHIVE,
                         "сдвиг LOD пшеницы к камере (0=как на карте, 1-2=шире раньше)");
        cvar_register_ex("r_veg_regen_budget", 4096.0f, 1.0f, 4096.0f,
                         CVAR_ARCHIVE, "чанков регенерации за кадр (меньше = "
                         "меньше рывков CPU, дорисовывается в следующих)");
        SDL_Log("Veg cfg: wheat %.0f/m2, grass %.1f/m2, dandelion %.2f/m2, "
                "dyn %.0fm, vis %.0fm",
                s_wheat_density, s_grass_density, s_dandelion_density,
                s_dyn_r, s_vis_r);
    }
    GLuint prog = make_program("shaders/wheat.vs", "shaders/wheat.fs");
    // Спрайты растений в один 2D-массив (холст = самый большой слой,
    // остальные прижаты к левому верхнему углу):
    //   слой 0  травa вид 1 (высокая)   img/grass.png      128x128
    //   слой 1  трава вид 2 (средняя)   img/grass1.png     128x128
    //   слой 2  трава вид 3 (низкая)    img/grass2.png     128x128
    //   слой 3  одуванчик               img/dandelions.png 128x128
    //   слой 4  пшеница lod0            img/wheat_lod0.png 128x178
    //   слой 5  пшеница lod1            img/wheat_lod1.png 152x178
    //   слой 6  пшеница lod2            img/wheat_lod2.png 256x178
    // Порядок индексов ДОЛЖЕН совпадать с VEG_LAY_* (см. верх файла).
    int ww[7] = {0}, hh[7] = {0};
    unsigned char *img[7];
    static const char *const path[7] = {
        "img/grass.png", "img/grass1.png", "img/grass2.png",
        "img/dandelions.png",
        "img/wheat_lod0.png", "img/wheat_lod1.png", "img/wheat_lod2.png",
    };
    for (int i = 0; i < 7; i++)
        img[i] = load_img(path[i], &ww[i], &hh[i], STBI_rgb_alpha);
    if (!prog) return NULL;
    for (int i = 0; i < 7; i++)
        if (!img[i]) { SDL_Log("veg: texture %s missing", path[i]); return NULL; }
    // Холст: максимум по каждому измерению (определяет пшеница lod2).
    int cw = 1, ch = 1;
    for (int i = 0; i < 7; i++) {
        if (ww[i] > cw) cw = ww[i];
        if (hh[i] > ch) ch = hh[i];
    }
    // Спрайты — в 2D-массиве: вся растительность рисуется одним draw call,
    // а слой (атрибут layer экземпляра) выбирает, чей спрайт показывать.
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D_ARRAY, tex);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, cw, ch, VEG_LAYERS, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    for (int i = 0; i < 7; i++)
        glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, i, ww[i], hh[i], 1,
                        GL_RGBA, GL_UNSIGNED_BYTE, img[i]);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    // Пиксели во владении ресурсного кеша — не освобождаем (см. resources.h)
    g_veg_tex = tex;

    // Трава — одиночный квад с лёгким наклоном, пшеница — крест из двух
    // квадов: так трава выглядит естественно, а колос объёмно.
    const float veg_mesh[4][5] = {
        {-0.5f, 0, 0, 0, 1}, { 0.5f, 0, 0, 1, 1},
        {-0.5f, 1, 0, 0, 0}, { 0.5f, 1, 0, 1, 0},
    };
    const GLuint veg_idx[6] = { 0,1,2, 1,3,2 };
    glGenBuffers(1, &g_veg_mesh_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g_veg_mesh_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof veg_mesh, veg_mesh, GL_STATIC_DRAW);
    glGenBuffers(1, &g_veg_mesh_ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_veg_mesh_ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof veg_idx, veg_idx, GL_STATIC_DRAW);

    // 4 VAO растительности: статик/динамик x трава/пшеница. Базовая сетка
    // одна (квад), экземпляр — 12 float (см. veg_gen_chunk).
    GLuint veg_vbo[4];
    for (int q = 0; q < 4; q++) {
        glGenVertexArrays(1, &g_veg_vao[q]);
        glGenBuffers(1, &veg_vbo[q]);
        veg_setup_vao(g_veg_vao[q], veg_vbo[q]);
        glBindBuffer(GL_ARRAY_BUFFER, veg_vbo[q]);
        glBufferData(GL_ARRAY_BUFFER, 1, NULL, GL_STREAM_DRAW);
    }
    g_veg_vb[0] = veg_vbo[0];
    g_veg_vb[1] = veg_vbo[1];
    g_veg_vb[2] = veg_vbo[2];
    g_veg_vb[3] = veg_vbo[3];
    glBindVertexArray(0);

    VegParams *p = calloc(1, sizeof *p);
    if (!p) {
        resources_program_release(prog);
        glDeleteTextures(1, &tex);
        glDeleteBuffers(1, &g_veg_mesh_vbo);
        glDeleteBuffers(1, &g_veg_mesh_ibo);
        for (int q = 0; q < 4; q++) {
            glDeleteBuffers(1, &g_veg_vb[q]);
            glDeleteVertexArrays(1, &g_veg_vao[q]);
            g_veg_vb[q] = g_veg_vao[q] = 0;
        }
        g_veg_mesh_vbo = g_veg_mesh_ibo = 0;
        return NULL;
    }
    p->world = w;
    p->rc = rc;
    p->prog = prog;
    p->vao_sg = g_veg_vao[0];
    p->vao_sw = g_veg_vao[1];
    p->vao_dg = g_veg_vao[2];
    p->vao_dw = g_veg_vao[3];
    p->vbo_sg = g_veg_vb[0];
    p->vbo_sw = g_veg_vb[1];
    p->vbo_dg = g_veg_vb[2];
    p->vbo_dw = g_veg_vb[3];
    p->tex = tex;
    // Теневой шейдер (глубина с прореживанием): после veg_cleanup() == 0,
    // поэтому replace здесь эквивалентен plain-загрузке.
    s_shadowveg_prog = resources_program_replace(s_shadowveg_prog,
                                                 "shaders/shadowveg.vs",
                                                 "shaders/shadowveg.fs");
    if (!s_shadowveg_prog) {
        SceneNode partial = {0};
        partial.params = p;
        veg_cleanup(&partial);
        free(p);
        return NULL;
    }
    veg_prog_init(p);              // uniform'ы + статичные
    s_veg = p;                     // для hot-reload
    shader_on_reshade(veg_reshade);

    // Чанки динамического слоя (вокруг камеры)
    g_map_w = (int)(w->world_w / VEG_CHUNK) + 1;
    g_map_d = (int)(w->world_d / VEG_CHUNK) + 1;
    g_veg_chunks = calloc((size_t)g_map_w * g_map_d, sizeof(VegChunk));
    g_dyn_g = g_dyn_w = NULL;
    g_dyn_cap = 0;

    // Статичный слой растительности: вся карта, один раз при старте
    if (!veg_static_init(p)) {
        SDL_Log("Vegetation: static layer allocation failed");
        SceneNode partial = {0};
        partial.params = p;
        veg_cleanup(&partial);
        free(p);
        return NULL;
    }

    // GLES не имеет core base-instance. Даже если desktop-драйвер экспортирует
    // адрес расширения в ES-контексте, используем гарантированную эмуляцию.
#ifdef USE_GLES_PATH
    g_has_base_instance = 0;
#else
    g_has_base_instance = p_glDrawElementsInstancedBaseInstance != NULL;
#endif
    SDL_Log("Vegetation draw: %s",
            g_has_base_instance ? "instanced super-chunks (base-instance)"
                                : "instanced super-chunks (offset emulation)");

    // Предпрореженный слой для теней (каждый 2-й экземпляр)
    veg_shadow_build();

    // GPU-driven (compute-cull + indirect): контекст >= GL 4.3. Статические
    // слои уже сгенерированы — заливаем их в Src-SSBO. Динамика дольётся в
    // veg_update при первом же движении камеры.
    g_veg_gpudriven = g_gl_gpu_driven;
    cvar_register("r_veg_gpudriven", 1.0f, "GPU-driven compute-cull травы (0=фолбэк)");
    cvar_register_ex("r_veg_minpix", 1.0f, 0.0f, 8.0f, CVAR_ARCHIVE,
                     "мин. экранный след травинки, px (0 = не отсекать)");
    if (g_veg_gpudriven) {
        veg_gpu_init(p);
        veg_gpu_src_upload(0, g_static_g, g_static_gc);
        veg_gpu_src_upload(1, g_static_w, g_static_wc);
    }

    const float size[3] = { w->world_w, w->max_height - w->min_height,
                            w->world_d };
    SceneNode *node = node_new("Vegetation", "MeshInstance3D", NULL, size, p,
                               NULL, veg_node_render);
    if (!node) {
        SceneNode partial = {0};
        partial.params = p;
        veg_cleanup(&partial);
        free(p);
    }
    return node;
}

void veg_cleanup(SceneNode *n)
{
    if (!n) return;   // безопасно при частичном построении сцены (error-path)
    VegParams *p = n->params;
    if (!p) return;
    if (g_veg_chunks) {
        for (int i = 0; i < g_map_w * g_map_d; i++) {
            free(g_veg_chunks[i].g);
            free(g_veg_chunks[i].w);
        }
        free(g_veg_chunks);
        g_veg_chunks = NULL;
    }
    // Сброс позиции камеры-чанка: при повторной загрузке карты динамический
    // слой перегенерируется сразу (иначе поле полупустое до смены чанка).
    g_chunk_cam[0] = g_chunk_cam[1] = INT_MAX;
    free(g_static_g); free(g_static_w);
    free(g_dyn_g); free(g_dyn_w);
    free(g_gen_scratch_g); free(g_gen_scratch_w);
    free(g_shadow_scratch_g); free(g_shadow_scratch_w);
    g_static_g = g_static_w = g_dyn_g = g_dyn_w = NULL;
    g_gen_scratch_g = g_gen_scratch_w = NULL;
    g_shadow_scratch_g = g_shadow_scratch_w = NULL;
    g_shadow_scratch_gcap = g_shadow_scratch_wcap = 0;
    free(g_sc_cnt_g); free(g_sc_cnt_w);
    free(g_sc_off_g); free(g_sc_off_w);
    free(g_sc_cx); free(g_sc_cz);
    free(g_sc_shcnt[0]); free(g_sc_shcnt[1]);
    free(g_sc_shoff[0]); free(g_sc_shoff[1]);
    g_sc_cnt_g = g_sc_cnt_w = NULL;
    g_sc_off_g = g_sc_off_w = NULL;
    g_sc_cx = g_sc_cz = NULL;
    g_sc_shcnt[0] = g_sc_shcnt[1] = NULL;
    g_sc_shoff[0] = g_sc_shoff[1] = NULL;
    glDeleteBuffers(1, &g_veg_mesh_vbo);
    glDeleteBuffers(1, &g_veg_mesh_ibo);
    for (int q = 0; q < 4; q++) {
        glDeleteBuffers(1, &g_veg_vb[q]);
        glDeleteVertexArrays(1, &g_veg_vao[q]);
    }
    if (g_veg_gpudriven) {
        for (int q = 0; q < 4; q++) {
            glDeleteBuffers(1, &g_veg_src_ssbo[q]);
            glDeleteBuffers(1, &g_veg_cull_ssbo[q]);
            glDeleteBuffers(1, &g_veg_indirect[q]);
            glDeleteVertexArrays(1, &g_veg_vao_cull[q]);
        }
        if (s_cull_prog) glDeleteProgram(s_cull_prog);
        s_cull_prog = 0;
        g_veg_gpudriven = 0;
    }
    for (int q = 0; q < 4; q++) {
        glDeleteBuffers(1, &g_veg_shadow_vbo[q]);
        glDeleteVertexArrays(1, &g_veg_shadow_vao[q]);
    }
    glDeleteTextures(1, &p->tex);
    resources_program_release(s_shadowveg_prog);
    s_shadowveg_prog = 0;
    if (s_veg == p) s_veg = NULL;
    resources_program_release(p->prog);
}
