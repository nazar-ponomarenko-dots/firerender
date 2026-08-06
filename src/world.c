// ============================================================================
//  world.c — загрузка карты мира, расчёт высот/нормалей (см. world.h)
// ============================================================================
#include "world.h"

#include "assets.h"
#include "gl_loader.h"
#include "mapdata.h"
#include "resources.h"

#include "stb_image.h"
#include <math.h>
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>

// Текстуры мира и средняя яркость карты (принадлежат этому модулю)
static GLuint s_tex_height, s_tex_base, s_tex_norm;
static float s_avg_lum;

float frnd(unsigned int *s)
{
    *s ^= *s << 13;
    *s ^= *s >> 17;
    *s ^= *s << 5;
    return (float)(*s & 0xFFFFFF) / 16777216.0f;
}

float height_curve(const WorldState *w, float t)
{
    if (t <= w->band_t0)
        return w->min_height + t / w->band_t0 * (w->band_h0 - w->min_height);
    if (t >= w->band_t1)
        return w->band_h1 + (t - w->band_t1) / (1.0f - w->band_t1) *
               (w->max_height - w->band_h1);
    float s = (t - w->band_t0) / (w->band_t1 - w->band_t0);
    s = s * s * (3.0f - 2.0f * s);
    return w->band_h0 + s * (w->band_h1 - w->band_h0);
}

float sample_height(const WorldState *w, float wx, float wz)
{
    float fx = wx - 0.5f, fy = wz - 0.5f;
    int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
    float tx = fx - x0, ty = fy - y0;
    int hw = w->tex_w, hh = w->tex_h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x0 > hw - 1) x0 = hw - 1;      // края: в обе стороны, как в текстуре
    if (y0 > hh - 1) y0 = hh - 1;
    int x1 = x0 + 1, y1 = y0 + 1;
    if (x1 > hw - 1) x1 = hw - 1;
    if (y1 > hh - 1) y1 = hh - 1;
    const unsigned char *h = w->hq;
    float v00 = h[y0*hw + x0] / 255.0f, v10 = h[y0*hw + x1] / 255.0f;
    float v01 = h[y1*hw + x0] / 255.0f, v11 = h[y1*hw + x1] / 255.0f;
    float vx0 = v00 + (v10 - v00) * tx;
    float vx1 = v01 + (v11 - v01) * tx;
    return w->min_height + (vx0 + (vx1 - vx0) * ty) *
           (w->max_height - w->min_height);
}

// Сглаживание высот: "синтерполировать карту, чтобы не было углов".
// Пара проходов сепарабельного box-blur радиуса 2 (2 метра), края зажимаем.
static void blur_heights(float *h, int w, int hh, int passes)
{
    float *tmp = malloc((size_t)w * hh * sizeof(float));
    for (int pass = 0; pass < passes; pass++) {
        for (int y = 0; y < hh; y++) {
            for (int x = 0; x < w; x++) {
                int x0 = x - 2, x1 = x + 2;          // 5 пикселей по X
                if (x0 < 0) x0 = 0;
                if (x1 > w - 1) x1 = w - 1;
                float s = 0;
                for (int i = x0; i <= x1; i++) s += h[y*w + i];
                tmp[y*w + x] = s / (float)(x1 - x0 + 1);
            }
        }
        for (int y = 0; y < hh; y++) {
            for (int x = 0; x < w; x++) {
                int y0 = y - 2, y1 = y + 2;          // и столько же по Z
                if (y0 < 0) y0 = 0;
                if (y1 > hh - 1) y1 = hh - 1;
                float s = 0;
                for (int j = y0; j <= y1; j++) s += tmp[j*w + x];
                h[y*w + x] = s / (float)(y1 - y0 + 1);
            }
        }
    }
    free(tmp);
}

int world_load(WorldState *w, WorldTextures *out)
{
    // Кривая высот и сглаживание — из map/map.json (terrain = { ... })
    {
        MapTerrain t;
        mapdata_terrain(&t);
        w->min_height = t.min_height;
        w->max_height = t.max_height;
        w->band_t0 = t.band_t0;
        w->band_t1 = t.band_t1;
        w->band_h0 = t.band_h0;
        w->band_h1 = t.band_h1;
        w->terrain_blur = t.blur > 0 ? t.blur : 0;
        SDL_Log("Terrain cfg: h %.0f..%.0f, band t %.2f..%.2f h %.0f..%.0f, blur %d",
                w->min_height, w->max_height, w->band_t0, w->band_t1,
                w->band_h0, w->band_h1, w->terrain_blur);
    }
    // Карта высот: из пака карты (zip) или из папки map/
    char map_path[512];
    if (resources_pak_is_open())
        snprintf(map_path, sizeof map_path, "%s", mapdata_image());
    else
        snprintf(map_path, sizeof map_path, "map/%s", mapdata_image());
    int mw = 0, mh = 0;
    // Карта высот — данные геометрии, НЕ рендер-текстура: подмена notfound
    // сломала бы рельеф (все высоты одного цвета = плоский мир). strict —
    // честный fail, если карты карты.w нет нигде.
    unsigned char *img = resources_image_strict(map_path, &mw, &mh, STBI_rgb);
    if (!img) return -1;
    SDL_Log("Map loaded: %s (%dx%d)", map_path, mw, mh);

    // --- Мир из карты: 1 пиксель = 1 метр -------------------------------------
    // Размеры мира, текстур и сетки рельефа зависят от разрешения карты.
    // Карта 2000x1125 -> мир 2000x1125 м, карта 6k -> мир 6k м. Сетка —
    // 1 квад на GRID_DIV пикселей. Высота берётся шейдером из текстуры
    // (билинейно), поэтому сетка может быть крупной — картинка та же,
    // а треугольников в 16 раз меньше.
    w->tex_w = mw;
    w->tex_h = mh;
    w->world_w = (float)mw;
    w->world_d = (float)mh;
    w->grid_w = mw / GRID_DIV;
    w->grid_h = mh / GRID_DIV;
    if (w->grid_w < 64) w->grid_w = 64;
    if (w->grid_h < 64) w->grid_h = 64;
    if (w->grid_w > GRID_MAX) w->grid_w = GRID_MAX;
    if (w->grid_h > GRID_MAX) w->grid_h = GRID_MAX;
    w->tex_per_quad = (float)mw / w->grid_w;
    SDL_Log("World: %.0fx%.0f m, grid %dx%d quads",
            w->world_w, w->world_d, w->grid_w, w->grid_h);

    // Яркость (высота) и цвет карты в полном разрешении
    float *lum = malloc((size_t)mw * mh * sizeof(float));
    unsigned char *color = malloc((size_t)mw * mh * 3);
    for (int i = 0; i < mw * mh; i++) {
        const unsigned char *px = img + i * 3;
        color[i*3 + 0] = px[0];
        color[i*3 + 1] = px[1];
        color[i*3 + 2] = px[2];
        lum[i] = (0.299f*px[0] + 0.587f*px[1] + 0.114f*px[2]) / 255.0f;
    }
    s_avg_lum = 0;
    for (int i = 0; i < mw * mh; i++) s_avg_lum += lum[i];
    s_avg_lum /= (float)(mw * mh);

    // --- Высоты: кривая с игровой полосой + сглаживание + нормали -------------
    // Высоты считаем в C (height_curve), поэтому normal map, положение пшеницы
    // и HUD всегда согласованы с рельефом. blur_heights "синтерполирует" карту:
    // углы и ступеньки сглаживаются ещё до отрисовки. 1 px = 1 м.
    w->heights = malloc((size_t)mw * mh * sizeof(float));
    for (int i = 0; i < mw * mh; i++)
        w->heights[i] = height_curve(w, 1.0f - lum[i]);
    blur_heights(w->heights, mw, mh, w->terrain_blur);
    unsigned char *nm = malloc((size_t)mw * mh * 3);
    for (int y = 0; y < mh; y++) {
        for (int x = 0; x < mw; x++) {
            int xl = x > 0 ? x - 1 : x;                  // края — без wrap,
            int xr = x < mw - 1 ? x + 1 : x;             // как в текстуре
            int yu = y > 0 ? y - 1 : y;
            int yd = y < mh - 1 ? y + 1 : y;
            float nx = -(w->heights[y*mw + xr] - w->heights[y*mw + xl]) * 0.5f;
            float nz = -(w->heights[yd*mw + x]  - w->heights[yu*mw + x]) * 0.5f;
            float len = sqrtf(nx*nx + nz*nz + 1.0f);
            unsigned char *outp = nm + (y*mw + x)*3;
            outp[0] = (unsigned char)((nx/len * 0.5f + 0.5f) * 255.0f);
            outp[1] = (unsigned char)((1.0f/len * 0.5f + 0.5f) * 255.0f);
            outp[2] = (unsigned char)((nz/len * 0.5f + 0.5f) * 255.0f);
        }
    }
    free(lum);
    // Пиксели карты во владении ресурсного кеша — не освобождаем
    // (см. resources.h; stb-буфер живёт до resources_shutdown)

    // Высоты -> текстура GL_R8 (0..1 = min..max), LINEAR + билинейная
    // интерполяция в вершинном шейдере дают плавный рельеф.
    unsigned char *hb = malloc((size_t)mw * mh);
    for (int i = 0; i < mw * mh; i++) {
        float t = (w->heights[i] - w->min_height) /
                  (w->max_height - w->min_height);
        if (t < 0) t = 0;
        if (t > 1) t = 1;
        hb[i] = (unsigned char)(t * 255.0f);
    }
    s_tex_height = make_texture(mw, mh, hb, GL_RED, GL_CLAMP_TO_EDGE, 1);
    w->hq = hb;   // оставляем квантованные высоты: растения стоят ТОЧНО на рельефе

    s_tex_base = make_texture(mw, mh, color, GL_RGB, GL_CLAMP_TO_EDGE, 0); // мини-карта
    s_tex_norm = make_texture(mw, mh, nm, GL_RGB, GL_CLAMP_TO_EDGE, 1);
    free(color);
    free(nm);

    out->tex_height = s_tex_height;
    out->tex_base = s_tex_base;
    out->tex_norm = s_tex_norm;
    return 0;
}

void world_free(WorldState *w)
{
    free(w->heights);
    w->heights = NULL;
    free(w->hq);
    w->hq = NULL;
    GLuint texs[3] = { s_tex_height, s_tex_base, s_tex_norm };
    glDeleteTextures(3, texs);
    s_tex_height = s_tex_base = s_tex_norm = 0;
}

float world_avg_lum(void)
{
    return s_avg_lum;
}
