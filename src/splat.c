// ============================================================================
//  splat.c — поверхности рельефа: splat-маска, дороги, карта занятости
// ----------------------------------------------------------------------------
//  См. splat.h. Порядок работы при загрузке карты:
//    world_load: высоты -> splat_flatten_roads (режем полотно дорог)
//                       -> нормали/текстуры (по УЖЕ выровненным высотам)
//    scene_build: splat_build (маска материалов + занятость объектами)
// ============================================================================
#include "splat.h"

#include "resources.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

// --- Состояние ----------------------------------------------------------------
static MapSurfaces s_surf;
static int s_sw, s_sh;             // разрешение splat-маски (тексели)
static float s_spm;                // метров на тексель
static unsigned char *s_mask;      // RGBA8: dirt, sand, gravel, asphalt
static GLuint s_tex;
static float s_world_w, s_world_d;

// Карта занятости: своё (более грубое) разрешение — 2 м/тексель хватает,
// чтобы трава не росла в машине, и вдвое дешевле по памяти.
#define OCC_METERS 2.0f
static int s_ow, s_oh;
static unsigned char *s_occ;
static unsigned int s_occ_version = 1;

int splat_have(void) { return s_mask != NULL && s_tex != 0; }
GLuint splat_texture(void) { return s_tex; }
const MapSurfaces *splat_surfaces(void) { return &s_surf; }
unsigned int splat_occupancy_version(void) { return s_occ_version; }

// --- Геометрия: расстояние от точки до отрезка (XZ) ---------------------------
static float seg_dist2(float px, float pz, float ax, float az,
                       float bx, float bz)
{
    float dx = bx - ax, dz = bz - az;
    float len2 = dx * dx + dz * dz;
    float t = 0.0f;
    if (len2 > 1e-8f) {
        t = ((px - ax) * dx + (pz - az) * dz) / len2;
        if (t < 0.0f) t = 0.0f;
        else if (t > 1.0f) t = 1.0f;
    }
    float cx = ax + dx * t, cz = az + dz * t;
    float ex = px - cx, ez = pz - cz;
    return ex * ex + ez * ez;
}

// Минимальное расстояние до ломаной (осевой линии дороги/полосы), метры.
static float polyline_dist(const float pts[][2], int n, float px, float pz)
{
    if (n <= 0) return 1e9f;
    if (n == 1) {
        float dx = px - pts[0][0], dz = pz - pts[0][1];
        return sqrtf(dx * dx + dz * dz);
    }
    float best = 1e30f;
    for (int i = 0; i + 1 < n; i++) {
        float d2 = seg_dist2(px, pz, pts[i][0], pts[i][1],
                             pts[i + 1][0], pts[i + 1][1]);
        if (d2 < best) best = d2;
    }
    return sqrtf(best);
}

// AABB ломаной с запасом margin — чтобы не гонять всю карту на каждую дорогу.
static void polyline_bounds(const float pts[][2], int n, float margin,
                            float out[4])
{
    out[0] = out[2] = 1e30f;
    out[1] = out[3] = -1e30f;
    for (int i = 0; i < n; i++) {
        if (pts[i][0] < out[0]) out[0] = pts[i][0];
        if (pts[i][0] > out[1]) out[1] = pts[i][0];
        if (pts[i][1] < out[2]) out[2] = pts[i][1];
        if (pts[i][1] > out[3]) out[3] = pts[i][1];
    }
    out[0] -= margin; out[1] += margin;
    out[2] -= margin; out[3] += margin;
}

// --- Выравнивание рельефа под дороги ------------------------------------------
//  Полотно дороги должно быть проезжим: вдоль осевой линии высота
//  сглаживается ОЧЕНЬ сильно (продольный профиль по опорным точкам линейно
//  интерполируется), поперёк — плавно переходит в исходный рельеф на
//  расстоянии width/2 + flatten_extra.
//
//  Опорная высота считается по СЫРОМУ рельефу вдоль линии и сглаживается
//  скользящим средним: дорога идёт по местности, но без ям и кочек.

// Высота исходного рельефа в точке (метры) прямо из массива высот.
static float raw_height(const WorldState *w, float x, float z)
{
    if (!w->heights) return 0.0f;
    float fx = x - 0.5f, fz = z - 0.5f;
    int x0 = (int)floorf(fx), z0 = (int)floorf(fz);
    float tx = fx - x0, tz = fz - z0;
    int x1 = x0 + 1, z1 = z0 + 1;
    if (x0 < 0) x0 = 0;
    if (x0 > w->tex_w - 1) x0 = w->tex_w - 1;
    if (x1 < 0) x1 = 0;
    if (x1 > w->tex_w - 1) x1 = w->tex_w - 1;
    if (z0 < 0) z0 = 0;
    if (z0 > w->tex_h - 1) z0 = w->tex_h - 1;
    if (z1 < 0) z1 = 0;
    if (z1 > w->tex_h - 1) z1 = w->tex_h - 1;
    float h00 = w->heights[(size_t)z0 * w->tex_w + x0];
    float h10 = w->heights[(size_t)z0 * w->tex_w + x1];
    float h01 = w->heights[(size_t)z1 * w->tex_w + x0];
    float h11 = w->heights[(size_t)z1 * w->tex_w + x1];
    float a = h00 + (h10 - h00) * tx;
    float b = h01 + (h11 - h01) * tx;
    return a + (b - a) * tz;
}

// Сэмплы вдоль дороги: центр линии с шагом ~2 м, высота сглажена окном.
#define ROAD_SAMPLE_STEP 2.0f
#define ROAD_SMOOTH_WIN  12          // окно сглаживания профиля (сэмплов)

typedef struct { float x, z, h; } RoadSample;

// Строит продольный профиль дороги. Возвращает число сэмплов (0 при OOM).
static int road_profile(const WorldState *w, const MapRoad *r,
                        RoadSample **out)
{
    // Длина ломаной -> число сэмплов
    float total = 0.0f;
    for (int i = 0; i + 1 < r->point_n; i++) {
        float dx = r->points[i + 1][0] - r->points[i][0];
        float dz = r->points[i + 1][1] - r->points[i][1];
        total += sqrtf(dx * dx + dz * dz);
    }
    int n = (int)(total / ROAD_SAMPLE_STEP) + 2;
    if (n < 2) n = 2;
    RoadSample *s = malloc((size_t)n * sizeof *s);
    if (!s) return 0;
    // Раскладываем точки равномерно по длине
    for (int k = 0; k < n; k++) {
        float want = total * (float)k / (float)(n - 1);
        float acc = 0.0f;
        float px = r->points[0][0], pz = r->points[0][1];
        for (int i = 0; i + 1 < r->point_n; i++) {
            float dx = r->points[i + 1][0] - r->points[i][0];
            float dz = r->points[i + 1][1] - r->points[i][1];
            float seg = sqrtf(dx * dx + dz * dz);
            if (acc + seg >= want || i + 2 == r->point_n) {
                float t = seg > 1e-6f ? (want - acc) / seg : 0.0f;
                if (t < 0.0f) t = 0.0f;
                if (t > 1.0f) t = 1.0f;
                px = r->points[i][0] + dx * t;
                pz = r->points[i][1] + dz * t;
                break;
            }
            acc += seg;
        }
        s[k].x = px;
        s[k].z = pz;
        s[k].h = raw_height(w, px, pz);
    }
    // Сглаживание профиля скользящим средним (дорога без кочек)
    float *sm = malloc((size_t)n * sizeof(float));
    if (sm) {
        for (int k = 0; k < n; k++) {
            int a = k - ROAD_SMOOTH_WIN / 2, b = k + ROAD_SMOOTH_WIN / 2;
            if (a < 0) a = 0;
            if (b > n - 1) b = n - 1;
            float acc = 0.0f;
            for (int i = a; i <= b; i++) acc += s[i].h;
            sm[k] = acc / (float)(b - a + 1);
        }
        for (int k = 0; k < n; k++) s[k].h = sm[k];
        free(sm);
    }
    *out = s;
    return n;
}

// Высота полотна в точке (ближайший сэмпл профиля) + расстояние до оси.
static void road_query(const RoadSample *s, int n, float px, float pz,
                       float *out_h, float *out_dist)
{
    float best = 1e30f, h = 0.0f;
    for (int i = 0; i + 1 < n; i++) {
        float d2 = seg_dist2(px, pz, s[i].x, s[i].z, s[i + 1].x, s[i + 1].z);
        if (d2 >= best) continue;
        best = d2;
        // высота вдоль сегмента: интерполяция между сэмплами по проекции
        float dx = s[i + 1].x - s[i].x, dz = s[i + 1].z - s[i].z;
        float len2 = dx * dx + dz * dz;
        float t = len2 > 1e-8f
                ? ((px - s[i].x) * dx + (pz - s[i].z) * dz) / len2 : 0.0f;
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        h = s[i].h + (s[i + 1].h - s[i].h) * t;
    }
    *out_h = h;
    *out_dist = sqrtf(best);
}

int splat_flatten_roads(WorldState *w)
{
    int nroads = mapdata_road_count();
    if (nroads <= 0 || !w->heights) return 0;
    int changed = 0;
    for (int ri = 0; ri < nroads; ri++) {
        const MapRoad *r = mapdata_road(ri);
        if (!r || r->flatten <= 0.001f) continue;
        RoadSample *prof = NULL;
        int pn = road_profile(w, r, &prof);
        if (pn <= 0) continue;
        float half = r->width * 0.5f;
        float reach = half + r->shoulder + r->flatten_extra;
        float bb[4];
        polyline_bounds(r->points, r->point_n, reach + 2.0f, bb);
        int x0 = (int)floorf(bb[0]), x1 = (int)ceilf(bb[1]);
        int z0 = (int)floorf(bb[2]), z1 = (int)ceilf(bb[3]);
        if (x0 < 0) x0 = 0;
        if (z0 < 0) z0 = 0;
        if (x1 > w->tex_w - 1) x1 = w->tex_w - 1;
        if (z1 > w->tex_h - 1) z1 = w->tex_h - 1;
        for (int j = z0; j <= z1; j++) {
            for (int i = x0; i <= x1; i++) {
                float px = (float)i + 0.5f, pz = (float)j + 0.5f;
                float rh, d;
                road_query(prof, pn, px, pz, &rh, &d);
                if (d > reach) continue;
                // Вес выравнивания: 1 на полотне, плавно к 0 на reach
                float t;
                if (d <= half) t = 1.0f;
                else {
                    t = 1.0f - (d - half) / (reach - half + 1e-5f);
                    t = t * t * (3.0f - 2.0f * t);   // smoothstep
                }
                float k = t * r->flatten;
                if (k <= 0.001f) continue;
                size_t idx = (size_t)j * w->tex_w + i;
                float src = w->heights[idx];
                float target = rh;
                // Колеи грунтовки: две продавленные полосы по краям полотна
                if (r->ruts > 0.001f && d <= half) {
                    float rut_c = half * 0.55f;         // центр колеи
                    float rut_w = half * 0.28f;         // полуширина
                    float dd = fabsf(d - rut_c);
                    float rut = 1.0f - dd / (rut_w + 1e-5f);
                    if (rut > 0.0f)
                        target -= rut * rut * r->ruts * 0.14f;   // до 14 см
                }
                w->heights[idx] = src + (target - src) * k;
                changed = 1;
            }
        }
        free(prof);
        SDL_Log("Road '%s': flattened (type %d, w=%.1fm, flatten=%.2f)",
                r->name[0] ? r->name : "?", r->type, r->width, r->flatten);
    }
    return changed;
}

// --- Splat-маска ---------------------------------------------------------------
// Добавляет вес слоя в тексель (насыщение до 255, остальные слои ужимаются:
// сумма каналов не должна превышать 255, иначе шейдеру нечего нормировать).
static void mask_add(int i, int j, int layer, float v)
{
    if (v <= 0.001f) return;
    if (i < 0 || j < 0 || i >= s_sw || j >= s_sh) return;
    if (v > 1.0f) v = 1.0f;
    unsigned char *px = s_mask + ((size_t)j * s_sw + i) * 4;
    int add = (int)(v * 255.0f + 0.5f);
    int cur = px[layer];
    int nv = cur + add;
    if (nv > 255) nv = 255;
    px[layer] = (unsigned char)nv;
    // Остальные слои уступают место (новый материал перекрывает старый)
    int room = 255 - nv;
    int other = 0;
    for (int k = 0; k < 4; k++) if (k != layer) other += px[k];
    if (other > room && other > 0) {
        float sc = (float)room / (float)other;
        for (int k = 0; k < 4; k++)
            if (k != layer) px[k] = (unsigned char)(px[k] * sc);
    }
}

static void occ_add(int i, int j, float v)
{
    if (v <= 0.001f) return;
    if (i < 0 || j < 0 || i >= s_ow || j >= s_oh) return;
    unsigned char *px = s_occ + (size_t)j * s_ow + i;
    int nv = *px + (int)(v * 255.0f + 0.5f);
    *px = (unsigned char)(nv > 255 ? 255 : nv);
}

// Круг занятости в мировых координатах
void splat_occupy_circle(float x, float z, float r, float strength)
{
    if (!s_occ || r <= 0.0f) return;
    int i0 = (int)floorf((x - r) / OCC_METERS);
    int i1 = (int)ceilf((x + r) / OCC_METERS);
    int j0 = (int)floorf((z - r) / OCC_METERS);
    int j1 = (int)ceilf((z + r) / OCC_METERS);
    for (int j = j0; j <= j1; j++)
        for (int i = i0; i <= i1; i++) {
            float px = ((float)i + 0.5f) * OCC_METERS;
            float pz = ((float)j + 0.5f) * OCC_METERS;
            float dx = px - x, dz = pz - z;
            if (dx * dx + dz * dz > r * r) continue;
            occ_add(i, j, strength);
        }
    s_occ_version++;
}

void splat_occupy_box(float x, float z, float hw, float hd, float strength)
{
    if (!s_occ || hw <= 0.0f || hd <= 0.0f) return;
    int i0 = (int)floorf((x - hw) / OCC_METERS);
    int i1 = (int)ceilf((x + hw) / OCC_METERS);
    int j0 = (int)floorf((z - hd) / OCC_METERS);
    int j1 = (int)ceilf((z + hd) / OCC_METERS);
    for (int j = j0; j <= j1; j++)
        for (int i = i0; i <= i1; i++)
            occ_add(i, j, strength);
    s_occ_version++;
}

// Покраска: пятно материала (круг/прямоугольник/полоса)
static void paint_apply(const MapPaint *p)
{
    float reach = 0.0f;
    float bb[4];
    switch (p->shape) {
    case PAINT_CIRCLE:
        reach = p->radius + p->edge;
        bb[0] = p->pos[0] - reach; bb[1] = p->pos[0] + reach;
        bb[2] = p->pos[1] - reach; bb[3] = p->pos[1] + reach;
        break;
    case PAINT_RECT:
        bb[0] = p->pos[0] - p->size[0] * 0.5f - p->edge;
        bb[1] = p->pos[0] + p->size[0] * 0.5f + p->edge;
        bb[2] = p->pos[1] - p->size[1] * 0.5f - p->edge;
        bb[3] = p->pos[1] + p->size[1] * 0.5f + p->edge;
        break;
    default:   // PAINT_PATH
        reach = p->width * 0.5f + p->edge;
        polyline_bounds(p->points, p->point_n, reach, bb);
        break;
    }
    int i0 = (int)floorf(bb[0] / s_spm), i1 = (int)ceilf(bb[1] / s_spm);
    int j0 = (int)floorf(bb[2] / s_spm), j1 = (int)ceilf(bb[3] / s_spm);
    for (int j = j0; j <= j1; j++) {
        for (int i = i0; i <= i1; i++) {
            float px = ((float)i + 0.5f) * s_spm;
            float pz = ((float)j + 0.5f) * s_spm;
            float w = 0.0f;
            if (p->shape == PAINT_CIRCLE) {
                float dx = px - p->pos[0], dz = pz - p->pos[1];
                float d = sqrtf(dx * dx + dz * dz);
                w = 1.0f - (d - p->radius) / (p->edge + 1e-5f);
            } else if (p->shape == PAINT_RECT) {
                float dx = fabsf(px - p->pos[0]) - p->size[0] * 0.5f;
                float dz = fabsf(pz - p->pos[1]) - p->size[1] * 0.5f;
                float d = dx > dz ? dx : dz;
                w = 1.0f - d / (p->edge + 1e-5f);
            } else {
                float d = polyline_dist(p->points, p->point_n, px, pz);
                w = 1.0f - (d - p->width * 0.5f) / (p->edge + 1e-5f);
            }
            if (w <= 0.0f) continue;
            if (w > 1.0f) w = 1.0f;
            w = w * w * (3.0f - 2.0f * w);          // smoothstep-край
            mask_add(i, j, p->material, w * p->strength);
        }
    }
}

// Дорога: полотно своим материалом, обочина — своим, плюс занятость.
static void road_paint(const MapRoad *r)
{
    float half = r->width * 0.5f;
    float outer = half + r->shoulder;
    float reach = outer + r->edge + 1.0f;
    float bb[4];
    polyline_bounds(r->points, r->point_n, reach, bb);
    int i0 = (int)floorf(bb[0] / s_spm), i1 = (int)ceilf(bb[1] / s_spm);
    int j0 = (int)floorf(bb[2] / s_spm), j1 = (int)ceilf(bb[3] / s_spm);
    for (int j = j0; j <= j1; j++) {
        for (int i = i0; i <= i1; i++) {
            float px = ((float)i + 0.5f) * s_spm;
            float pz = ((float)j + 0.5f) * s_spm;
            float d = polyline_dist(r->points, r->point_n, px, pz);
            if (d > reach) continue;
            // Полотно: полный вес до half, спад на edge
            float road_w = 1.0f - (d - half) / (r->edge + 1e-5f);
            if (road_w > 1.0f) road_w = 1.0f;
            if (road_w > 0.0f) {
                road_w = road_w * road_w * (3.0f - 2.0f * road_w);
                mask_add(i, j, r->material, road_w);
            }
            // Обочина: между half и outer, гаснет к краю
            if (r->shoulder > 0.01f && r->shoulder_material != r->material) {
                float sh = 0.0f;
                if (d > half && d <= outer) sh = 1.0f;
                else if (d > outer)
                    sh = 1.0f - (d - outer) / (r->edge + r->shoulder * 0.5f + 1e-5f);
                if (sh > 0.0f) {
                    if (sh > 1.0f) sh = 1.0f;
                    sh = sh * sh * (3.0f - 2.0f * sh);
                    mask_add(i, j, r->shoulder_material, sh * (1.0f - road_w));
                }
            }
        }
    }
    // Занятость: на полотне (и чуть шире) растительности нет
    if (!r->veg_clear || !s_occ) return;
    float occ_r = half + 0.35f;
    for (int i = 0; i + 1 < r->point_n; i++) {
        float ax = r->points[i][0], az = r->points[i][1];
        float bx = r->points[i + 1][0], bz = r->points[i + 1][1];
        float dx = bx - ax, dz = bz - az;
        float len = sqrtf(dx * dx + dz * dz);
        int steps = (int)(len / (OCC_METERS * 0.5f)) + 1;
        for (int k = 0; k <= steps; k++) {
            float t = (float)k / (float)steps;
            splat_occupy_circle(ax + dx * t, az + dz * t, occ_r, 1.0f);
        }
    }
}

// Уклон рельефа в точке (тангенс) — для авто-щебня на крутых склонах.
static float slope_at(const WorldState *w, float x, float z)
{
    const float e = 1.5f;
    float dx = raw_height(w, x + e, z) - raw_height(w, x - e, z);
    float dz = raw_height(w, x, z + e) - raw_height(w, x, z - e);
    return sqrtf(dx * dx + dz * dz) / (2.0f * e);
}

void splat_build(WorldState *w)
{
    splat_free();
    mapdata_surfaces(&s_surf);
    s_world_w = w->world_w;
    s_world_d = w->world_d;
    s_spm = s_surf.resolution > 0.05f ? s_surf.resolution : 1.0f;
    s_sw = (int)(w->world_w / s_spm) + 1;
    s_sh = (int)(w->world_d / s_spm) + 1;
    if (s_sw < 4) s_sw = 4;
    if (s_sh < 4) s_sh = 4;
    s_mask = calloc((size_t)s_sw * s_sh, 4);
    s_ow = (int)(w->world_w / OCC_METERS) + 1;
    s_oh = (int)(w->world_d / OCC_METERS) + 1;
    s_occ = calloc((size_t)s_ow * s_oh, 1);
    if (!s_mask || !s_occ) {
        SDL_Log("splat: no memory for %dx%d mask", s_sw, s_sh);
        splat_free();
        return;
    }
    Uint64 t0 = SDL_GetTicks();

    // 1) Авто-материалы по рельефу: на крутых склонах базовая трава не
    //    держится — проступает щебень (как в жизни: осыпь на обрыве).
    if (s_surf.slope_rock > 1.0f) {
        float tan_min = tanf(s_surf.slope_rock * (float)M_PI / 180.0f);
        float tan_max = tan_min * 1.6f + 0.2f;
        for (int j = 0; j < s_sh; j++)
            for (int i = 0; i < s_sw; i++) {
                float px = ((float)i + 0.5f) * s_spm;
                float pz = ((float)j + 0.5f) * s_spm;
                float s = slope_at(w, px, pz);
                if (s <= tan_min) continue;
                float t = (s - tan_min) / (tan_max - tan_min + 1e-5f);
                if (t > 1.0f) t = 1.0f;
                mask_add(i, j, SPLAT_GRAVEL, t * 0.9f);
            }
    }

    // 2) Пятна покраски из карты
    for (int i = 0; i < mapdata_paint_count(); i++) {
        const MapPaint *p = mapdata_paint(i);
        if (p) paint_apply(p);
    }

    // 3) Дороги (полотно + обочина + занятость)
    for (int i = 0; i < mapdata_road_count(); i++) {
        const MapRoad *r = mapdata_road(i);
        if (r) road_paint(r);
    }

    // 4) Занятость от статических объектов карты (биллборды-деревья, модели):
    //    трава не растёт в стволе/кузове. Радиус — из коллизии, если задана,
    //    иначе из масштаба объекта.
    for (int i = 0; i < mapdata_object_count(); i++) {
        const MapObject *o = mapdata_object(i);
        if (!o || !o->model[0]) continue;
        float sxz = o->scale[0] > 0.01f ? o->scale[0] : 1.0f;
        float r = o->collision && o->radius > 0.01f ? o->radius
                                                    : 0.9f * sxz;
        splat_occupy_circle(o->pos[0], o->pos[2], r, 1.0f);
    }
    // Коллизии-квадраты из collisions[] (дома, платформы)
    for (int i = 0; i < mapdata_collision_count(); i++) {
        const MapCollision *c = mapdata_collision(i);
        if (!c || c->size[0] <= 0.01f || c->size[1] <= 0.01f) continue;
        splat_occupy_box(c->pos[0], c->pos[2],
                         c->size[0] * 0.5f, c->size[1] * 0.5f, 1.0f);
    }

    // Текстура splat-маски: LINEAR (края материалов плавные), CLAMP.
    glGenTextures(1, &s_tex);
    glBindTexture(GL_TEXTURE_2D, s_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, s_sw, s_sh, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, s_mask);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    // Статистика: сколько площади под каким материалом (для лога)
    long cov[4] = { 0, 0, 0, 0 };
    long total = (long)s_sw * s_sh;
    for (long k = 0; k < total; k++)
        for (int c = 0; c < 4; c++)
            if (s_mask[k * 4 + c] > 96) cov[c]++;
    SDL_Log("Splat baked (%dx%d, %.1f m/px) in %llu ms: "
            "dirt %.1f%%, sand %.1f%%, gravel %.1f%%, asphalt %.1f%%",
            s_sw, s_sh, s_spm,
            (unsigned long long)(SDL_GetTicks() - t0),
            100.0 * cov[0] / total, 100.0 * cov[1] / total,
            100.0 * cov[2] / total, 100.0 * cov[3] / total);
}

void splat_free(void)
{
    if (s_tex) { glDeleteTextures(1, &s_tex); s_tex = 0; }
    free(s_mask);
    s_mask = NULL;
    free(s_occ);
    s_occ = NULL;
    s_sw = s_sh = s_ow = s_oh = 0;
    s_occ_version++;
}

// --- Запросы ------------------------------------------------------------------
float splat_weight_at(int material, float wx, float wz)
{
    if (!s_mask || material < 0 || material >= SPLAT_LAYERS) return 0.0f;
    int i = (int)(wx / s_spm);
    int j = (int)(wz / s_spm);
    if (i < 0) i = 0;
    if (j < 0) j = 0;
    if (i > s_sw - 1) i = s_sw - 1;
    if (j > s_sh - 1) j = s_sh - 1;
    return s_mask[((size_t)j * s_sw + i) * 4 + material] / 255.0f;
}

float splat_occupancy_at(float wx, float wz)
{
    if (!s_occ) return 0.0f;
    int i = (int)(wx / OCC_METERS);
    int j = (int)(wz / OCC_METERS);
    if (i < 0) i = 0;
    if (j < 0) j = 0;
    if (i > s_ow - 1) i = s_ow - 1;
    if (j > s_oh - 1) j = s_oh - 1;
    return s_occ[(size_t)j * s_ow + i] / 255.0f;
}

float splat_grass_factor(float wx, float wz)
{
    // Занятость: 1 = стоит объект/дорога -> травы нет вовсе.
    float occ = splat_occupancy_at(wx, wz);
    if (occ > 0.5f) return 0.0f;
    float f = 1.0f - occ * 2.0f;               // 0.5 занятости -> уже 0
    if (!s_mask) return f;
    // Слои поверхности: базовая трава уступает материалу пропорционально
    // его весу, а на самом материале трава растёт с коэффициентом grass.
    int i = (int)(wx / s_spm);
    int j = (int)(wz / s_spm);
    if (i < 0) i = 0;
    if (j < 0) j = 0;
    if (i > s_sw - 1) i = s_sw - 1;
    if (j > s_sh - 1) j = s_sh - 1;
    const unsigned char *px = s_mask + ((size_t)j * s_sw + i) * 4;
    float base = 1.0f, mat = 0.0f;
    for (int c = 0; c < SPLAT_LAYERS; c++) {
        float wv = px[c] / 255.0f;
        base -= wv;
        mat += wv * s_surf.grass[c];
    }
    if (base < 0.0f) base = 0.0f;
    float k = base + mat;
    if (k > 1.0f) k = 1.0f;
    return f * k;
}
