// ============================================================================
//  world.c — загрузка карты мира, расчёт высот/нормалей (см. world.h)
// ============================================================================
#include "world.h"

#include "assets.h"
#include "gl_loader.h"
#include "mapdata.h"
#include "resources.h"
#include "splat.h"

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
    if (t <= 0.0f) return w->min_height;
    if (t >= 1.0f) return w->max_height;

    // Весь диапазон [0..1] — из smoothstep-сегментов. У smoothstep нулевой
    // наклон на обоих концах, поэтому на СТЫКАХ сегментов (band_t0, band_t1)
    // наклон совпадает — переходы плавные (C1), без резких "рёбер"/изломов,
    // как было со стыком "линейный хвост + smoothstep" (там был излом).
    // Дегенеративные параметры (band_t0==0, band_t1==band_t0, band_t1==1 —
    // кривой terrain.cfg) дают деление на ноль и NaN в рельефе: сегмент
    // нулевой ширины пропускаем (сразу его верхняя высота).
    float s;
    if (t < w->band_t0) {                                  // под играбельной полосой
        if (w->band_t0 <= 0.0f) return w->band_h0;
        s = t / w->band_t0;
        s = s * s * (3.0f - 2.0f * s);
        return w->min_height + s * (w->band_h0 - w->min_height);
    }
    if (t <= w->band_t1) {                                 // играбельная полоса
        float span = w->band_t1 - w->band_t0;
        if (span <= 0.0f) return w->band_h0;
        s = (t - w->band_t0) / span;
        s = s * s * (3.0f - 2.0f * s);
        return w->band_h0 + s * (w->band_h1 - w->band_h0);
    }
    if (1.0f - w->band_t1 <= 0.0f) return w->band_h1;
    s = (t - w->band_t1) / (1.0f - w->band_t1);            // над полосой
    s = s * s * (3.0f - 2.0f * s);
    return w->band_h1 + s * (w->max_height - w->band_h1);
}

float sample_height(const WorldState *w, float wx, float wz)
{
    // Мировая координата == индекс текселя карты высот (сетка GRID_DIV=1
    // ставит вершину на мировой координате i, которая читает тексель i).
    // Поэтому берём тексель floor(wx): коллизия совпадает с тем, что рисует
    // вершинный шейдер рельефа (GL_LINEAR между живых текселей).
    int hw = w->tex_w, hh = w->tex_h;
    if (wx < 0.0f) wx = 0.0f;
    if (wz < 0.0f) wz = 0.0f;
    if (wx > w->world_w) wx = w->world_w;
    if (wz > w->world_d) wz = w->world_d;
    int x0 = (int)floorf(wx), y0 = (int)floorf(wz);   // края: как текстура (clamp)
    if (x0 > hw - 1) x0 = hw - 1;
    if (y0 > hh - 1) y0 = hh - 1;
    int x1 = x0 + 1, y1 = y0 + 1;
    if (x1 > hw - 1) x1 = hw - 1;
    if (y1 > hh - 1) y1 = hh - 1;
    float tx = wx - x0, ty = wz - y0;
    // Читаем СГЛАЖЕННЫЕ высоты из float-массива w->heights (точность float,
    // без 8-битного квантования, которое давало полосы-ступеньки на склонах).
    // Рендер-шейдеры используют те же значения через GL_R16 — коллизия и
    // картинка совпадают, без banding.
    const float *h = w->heights;
    float v00 = h[y0*hw + x0], v10 = h[y0*hw + x1];
    float v01 = h[y1*hw + x0], v11 = h[y1*hw + x1];
    float vx0 = v00 + (v10 - v00) * tx;
    float vx1 = v01 + (v11 - v01) * tx;
    return vx0 + (vx1 - vx0) * ty;
}

// Сглаживание высот: сепарабельный box-blur. Радиус — в пикселях (1 пиксель =
// 1 метр). Сделан через префиксные суммы — O(N) на проход независимо от радиуса,
// поэтому широкими окнами (десятки метров) можно разглаживать переходы дёшево.
// Края зажимаем (как в текстуре, без wrap).
static void blur_heights(float *h, int w, int hh, int passes, int radius)
{
    if (radius < 1) radius = 1;
    int n = w > hh ? w : hh;
    float *tmp = malloc((size_t)w * hh * sizeof(float));
    float *pre = malloc((size_t)n * sizeof(float));
    if (!tmp || !pre) {   // OOM — пропуск сглаживания (лучше, чем падение)
        free(tmp); free(pre);
        SDL_Log("terrain: blur skipped (no memory)");
        return;
    }
    for (int pass = 0; pass < passes; pass++) {
        // --- горизонтальный проход (строки) ---
        for (int y = 0; y < hh; y++) {
            for (int x = 0; x < w; x++)
                pre[x] = (x ? pre[x-1] : 0.0f) + h[y*w + x];
            for (int x = 0; x < w; x++) {
                int l = x - radius; if (l < 0) l = 0;
                int r = x + radius; if (r > w - 1) r = w - 1;
                float s = pre[r] - (l ? pre[l-1] : 0.0f);
                tmp[y*w + x] = s / (float)(r - l + 1);
            }
        }
        // --- вертикальный проход (колонки) ---
        for (int x = 0; x < w; x++) {
            for (int y = 0; y < hh; y++)
                pre[y] = (y ? pre[y-1] : 0.0f) + tmp[y*w + x];
            for (int y = 0; y < hh; y++) {
                int l = y - radius; if (l < 0) l = 0;
                int r = y + radius; if (r > hh - 1) r = hh - 1;
                float s = pre[r] - (l ? pre[l-1] : 0.0f);
                h[y*w + x] = s / (float)(r - l + 1);
            }
        }
    }
    free(tmp);
    free(pre);
}

// Ограничение крутизны склонов (термальная эрозия). Если уклон между соседями
// больше tan(slope_deg), излишек перетекает к соседу. Идём ПОКА ЕСТЬ ЧТО
// рассасывать (сходятся), поэтому в итоге НИ ОДИН уклон не превосходит заданный —
// резкие переходы исключаются в принципе. passes — страховочный потолок.
static void slope_limit_heights(float *h, int w, int hh,
                                int passes, float slope_deg)
{
    float maxdh = tanf(slope_deg * (float)M_PI / 180.0f); // метров на метр
    if (maxdh <= 1e-4f) return;
    const float eps = 0.001f;   // порог: мелкий хвост крутизны не гоняем вечно
    const int dx[4] = { 1, -1, 0, 0 };
    const int dy[4] = { 0, 0, 1, -1 };
    for (int pass = 0; pass < passes; pass++) {
        int changed = 0;
        for (int y = 0; y < hh; y++) {
            for (int x = 0; x < w; x++) {
                float v = h[y*w + x];
                for (int k = 0; k < 4; k++) {
                    int nx = x + dx[k], ny = y + dy[k];
                    if (nx < 0 || nx >= w || ny < 0 || ny >= hh) continue;
                    float d = v - h[ny*w + nx];
                    if (d > maxdh + eps) {
                        float excess = (d - maxdh) * 0.5f;
                        h[ny*w + nx] += excess;
                        h[y*w + x]   -= excess;
                        v = h[y*w + x];
                        changed = 1;
                    }
                }
            }
        }
        if (!changed) break;   // сошлись: переходы везде не круче maxdh
    }
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
        w->terrain_blur_radius = t.blur_radius > 0 ? t.blur_radius : 2;
        w->terrain_erosion = t.erosion > 0 ? t.erosion : 0;
        w->terrain_erosion_slope = t.erosion_slope > 0.0f ? t.erosion_slope : 45.0f;
        SDL_Log("Terrain cfg: h %.0f..%.0f, band t %.2f..%.2f h %.0f..%.0f, "
                "blur %dx%d, erosion %d (<%.0f deg)",
                w->min_height, w->max_height, w->band_t0, w->band_t1,
                w->band_h0, w->band_h1, w->terrain_blur, w->terrain_blur_radius,
                w->terrain_erosion, w->terrain_erosion_slope);
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
    w->heights = malloc((size_t)mw * mh * sizeof(float));
    if (!lum || !color || !w->heights) {   // OOM — честный fail загрузки
        free(lum); free(color); free(w->heights);
        stbi_image_free(img);
        w->heights = NULL;                 // world_free терпит NULL
        SDL_Log("World: no memory for %dx%d map", mw, mh);
        return -1;
    }
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
    stbi_image_free(img);
    img = NULL;

    // --- Высоты: кривая с игровой полосой + сглаживание + нормали -------------
    // Высоты считаем в C (height_curve), поэтому normal map, положение пшеницы
    // и HUD всегда согласованы с рельефом. blur_heights "синтерполирует" карту:
    // углы и ступеньки сглаживаются ещё до отрисовки. 1 px = 1 м.
    for (int i = 0; i < mw * mh; i++)
        w->heights[i] = height_curve(w, 1.0f - lum[i]);
    // Сглаживание переходов + ограничение крутизны: сначала размываем, чтобы
    // убрать ступеньки, затем эродируем, чтобы уклоны не превышали erosion_slope
    // (физически можно взойти, см. sv_maxslope). Оба прохода работают и на
    // видимый рельеф, и на коллизию (один источник высот).
    blur_heights(w->heights, mw, mh, w->terrain_blur, w->terrain_blur_radius);
    if (w->terrain_erosion > 0)
        slope_limit_heights(w->heights, mw, mh, w->terrain_erosion,
                            w->terrain_erosion_slope);
    // ДОРОГИ РЕЖУТ РЕЛЬЕФ: полотно выравнивается по продольному профилю
    // ПОСЛЕ blur/эрозии и ДО нормалей — нормали, коллизия (sample_height),
    // растительность и геометрия видят один и тот же выровненный рельеф.
    splat_flatten_roads(w);
    unsigned char *nm = malloc((size_t)mw * mh * 3);
    if (!nm) {
        free(lum); free(color);
        SDL_Log("World: no memory for normal map");
        return -1;                         // w->heights дойдёт до world_free
    }
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

    // Высоты -> текстура GL_R16 (точность 1/65536) вместо GL_R8: 8-битное
    // квантование при диапазоне 500 м давало полосы-ступеньки (~2 м/уровень)
    // на склонах — "резкие переходы". 16 бит убирает их полностью.
    // Билинейная фильтрация + LINEAR дают плавную поверхность; коллизия
    // (sample_height) читает те же значения из float-массива — всё совпадает.
    unsigned short *hb = malloc((size_t)mw * mh * sizeof(unsigned short));
    if (!hb) {
        free(color); free(nm);            // lum уже освобождён выше
        SDL_Log("World: no memory for height texture");
        return -1;
    }
    // Дегенеративная кривая (max==min в map.json): деление на ноль -> NaN.
    // Зажимаем диапазон до минимум 1 м, чтобы высота квантовалась корректно.
    float hspan = w->max_height - w->min_height;
    if (hspan < 1.0f) hspan = 1.0f;
    for (int i = 0; i < mw * mh; i++) {
        float t = (w->heights[i] - w->min_height) / hspan;
        if (t < 0) t = 0;
        if (t > 1) t = 1;
        hb[i] = (unsigned short)(t * 65535.0f);
    }
    glGenTextures(1, &s_tex_height);
    glBindTexture(GL_TEXTURE_2D, s_tex_height);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
#ifdef USE_GLES_PATH
    // GLES 3.0 не гарантирует EXT_texture_norm16. RG8 хранит те же 16 бит:
    // старший байт в R, младший в G; шейдер восстанавливает значение.
    unsigned char *h_rg = malloc((size_t)mw * mh * 2);
    if (!h_rg) { free(hb); free(color); free(nm); return -1; }
    for (int i = 0; i < mw * mh; i++) {
        h_rg[i*2+0] = (unsigned char)(hb[i] >> 8);
        h_rg[i*2+1] = (unsigned char)(hb[i] & 255);
    }
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, mw, mh, 0, GL_RG,
                 GL_UNSIGNED_BYTE, h_rg);
    free(h_rg);
#else
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R16, mw, mh, 0, GL_RED,
                 GL_UNSIGNED_SHORT, hb);
#endif
    free(hb);

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
    GLuint texs[3] = { s_tex_height, s_tex_base, s_tex_norm };
    glDeleteTextures(3, texs);
    s_tex_height = s_tex_base = s_tex_norm = 0;
}

float world_avg_lum(void)
{
    return s_avg_lum;
}

// Текстура карты высот (GL_R16, LINEAR) — для волюметрик-тумана (дымка у
// земли). 0, пока мир не загружен / уже освобождён.
GLuint world_height_tex(void)
{
    return s_tex_height;
}
