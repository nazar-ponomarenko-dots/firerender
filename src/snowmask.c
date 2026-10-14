// ============================================================================
//  snowmask.c — пометка вершин меша "открыто небу сверху" (для снега на GLB)
// ----------------------------------------------------------------------------
//  Запечённая мировая маска "видимость неба" (terrain_wetness_mask_bake)
//  построена по статике карты и НЕ видит glb-модели, поэтому над машиной
//  она показывает "открыто" и снег сыпался внутрь салона. Здесь мы делаем
//  локальный, на модель, тест: для каждой вершины пускаем строго вертикальный
//  луч вверх и смотрим, не пересекает ли он собственную геометрию модели.
//  Если над вершиной что-то есть (крыша над полом/сиденьем) — 0 (снег не
//  дойдёт). Если сверху чисто (крыша/капот/багажник) — 1.
//
//  Ускорение: кубик-грид по XZ проекции; в каждой ячейке лежат только те
//  треугольники, чья XZ-проекция её покрывает. Для равной модели это
//  десятки ячеек и по паре треугольников на запрос.
// ============================================================================
#include "snowmask.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

// Самая маленькая "высота" над точкой, которую считаем преградой (м).
#define SNOWMASK_EPS 1e-4f

float *snow_mask_compute(const float *const *verts, const int *vcounts,
                         const unsigned *const *indices, const int *idxcounts,
                         int nprims, int *out_total)
{
    int total = 0;
    for (int p = 0; p < nprims; p++) total += vcounts[p];
    if (out_total) *out_total = total;
    if (nprims <= 0 || total <= 0) return NULL;

    long ntris = 0;
    for (int p = 0; p < nprims; p++) ntris += idxcounts[p] / 3;
    if (ntris <= 0) return NULL;

    // Собираем треугольники объединения и его XZ-рамку.
    float *tri = malloc((size_t)ntris * 9 * sizeof(float));
    if (!tri) return NULL;
    float xmin = 1e30f, xmax = -1e30f, zmin = 1e30f, zmax = -1e30f;
    long t = 0;
    for (int p = 0; p < nprims; p++) {
        const float *v = verts[p];
        const unsigned *idx = indices[p];
        for (int j = 0; j + 2 < idxcounts[p]; j += 3) {
            unsigned i0 = idx[j], i1 = idx[j + 1], i2 = idx[j + 2];
            for (int k = 0; k < 3; k++) {
                tri[9 * t + 3 * k + 0] = v[3 * (k == 0 ? i0 : k == 1 ? i1 : i2) + 0];
                tri[9 * t + 3 * k + 1] = v[3 * (k == 0 ? i0 : k == 1 ? i1 : i2) + 1];
                tri[9 * t + 3 * k + 2] = v[3 * (k == 0 ? i0 : k == 1 ? i1 : i2) + 2];
            }
            float ax = tri[9 * t + 0], az = tri[9 * t + 2];
            float bx = tri[9 * t + 3], bz = tri[9 * t + 5];
            float cx = tri[9 * t + 6], cz = tri[9 * t + 8];
            if (ax < xmin) xmin = ax;
            if (ax > xmax) xmax = ax;
            if (bx < xmin) xmin = bx;
            if (bx > xmax) xmax = bx;
            if (cx < xmin) xmin = cx;
            if (cx > xmax) xmax = cx;
            if (az < zmin) zmin = az;
            if (az > zmax) zmax = az;
            if (bz < zmin) zmin = bz;
            if (bz > zmax) zmax = bz;
            if (cz < zmin) zmin = cz;
            if (cz > zmax) zmax = cz;
            t++;
        }
    }
    float xs = xmax - xmin, zs = zmax - zmin;
    if (xs < 1e-4f) xs = 1e-4f;
    if (zs < 1e-4f) zs = 1e-4f;
    float cs = fmaxf(0.12f, fmaxf(xs, zs) / 256.0f);
    int nx = (int)(xs / cs) + 1, nz = (int)(zs / cs) + 1;
    nx = nx < 1 ? 1 : nx;
    nz = nz < 1 ? 1 : nz;
    size_t ncells = (size_t)nx * nz;

    // Пасс 1: сколько треугольников в упор попадёт в каждую ячейку.
    size_t *cnt = calloc(ncells, sizeof *cnt);
    for (long k = 0; k < ntris; k++) {
        float ax = tri[9 * k + 0], az = tri[9 * k + 2];
        float bx = tri[9 * k + 3], bz = tri[9 * k + 5];
        float cx = tri[9 * k + 6], cz = tri[9 * k + 8];
        float mnx = fminf(ax, fminf(bx, cx)), mxx = fmaxf(ax, fmaxf(bx, cx));
        float mnz = fminf(az, fminf(bz, cz)), mxz = fmaxf(az, fmaxf(bz, cz));
        int i0 = (int)((mnx - xmin) / cs), i1 = (int)((mxx - xmin) / cs);
        int j0 = (int)((mnz - zmin) / cs), j1 = (int)((mxz - zmin) / cs);
        if (i0 < 0) i0 = 0;
        if (i0 >= nx) i0 = nx - 1;
        if (i1 < 0) i1 = 0;
        if (i1 >= nx) i1 = nx - 1;
        if (j0 < 0) j0 = 0;
        if (j0 >= nz) j0 = nz - 1;
        if (j1 < 0) j1 = 0;
        if (j1 >= nz) j1 = nz - 1;
        for (int j = j0; j <= j1; j++)
            for (int i = i0; i <= i1; i++)
                cnt[(size_t)j * nx + i]++;
    }
    size_t *off = malloc(((size_t)ncells + 1) * sizeof *off);
    size_t *cur = malloc(ncells * sizeof *cur);
    off[0] = 0;
    for (size_t c = 0; c < ncells; c++) { off[c + 1] = off[c] + cnt[c]; cur[c] = off[c]; }
    size_t *lst = malloc((off[ncells] > 0 ? off[ncells] : 1) * sizeof *lst);
    for (long k = 0; k < ntris; k++) {
        float ax = tri[9 * k + 0], az = tri[9 * k + 2];
        float bx = tri[9 * k + 3], bz = tri[9 * k + 5];
        float cx = tri[9 * k + 6], cz = tri[9 * k + 8];
        float mnx = fminf(ax, fminf(bx, cx)), mxx = fmaxf(ax, fmaxf(bx, cx));
        float mnz = fminf(az, fminf(bz, cz)), mxz = fmaxf(az, fmaxf(bz, cz));
        int i0 = (int)((mnx - xmin) / cs), i1 = (int)((mxx - xmin) / cs);
        int j0 = (int)((mnz - zmin) / cs), j1 = (int)((mxz - zmin) / cs);
        if (i0 < 0) i0 = 0;
        if (i0 >= nx) i0 = nx - 1;
        if (i1 < 0) i1 = 0;
        if (i1 >= nx) i1 = nx - 1;
        if (j0 < 0) j0 = 0;
        if (j0 >= nz) j0 = nz - 1;
        if (j1 < 0) j1 = 0;
        if (j1 >= nz) j1 = nz - 1;
        for (int j = j0; j <= j1; j++)
            for (int i = i0; i <= i1; i++)
                lst[cur[(size_t)j * nx + i]++] = (size_t)k;
    }

    // Пасс 2: для каждой вершины — вертикальный луч вверх по её ячейке.
    float *out = malloc((size_t)total * sizeof(float));
    long voff = 0;
    for (int p = 0; p < nprims; p++) {
        const float *v = verts[p];
        for (int i = 0; i < vcounts[p]; i++) {
            float x = v[3 * i + 0], y = v[3 * i + 1], z = v[3 * i + 2];
            int ix = (int)((x - xmin) / cs);
            int iz = (int)((z - zmin) / cs);
            if (ix < 0) ix = 0;
            if (ix >= nx) ix = nx - 1;
            if (iz < 0) iz = 0;
            if (iz >= nz) iz = nz - 1;
            size_t cell = (size_t)iz * nx + ix;
            int open = 1;
            for (size_t e = off[cell]; e < off[cell + 1]; e++) {
                const float *a = &tri[9 * lst[e]];
                float e0x = a[3] - a[0], e0y = a[4] - a[1], e0z = a[5] - a[2];
                float e2x = a[6] - a[0], e2y = a[7] - a[1], e2z = a[8] - a[2];
                // h = cross(d,e2), d=(0,1,0) -> h = (e2z, 0, -e2x)
                float hx = e2z, hy = 0.0f, hz = -e2x;
                float sx = x - a[0], sy = y - a[1], sz = z - a[2];
                float det = e0x * hx + e0y * hy + e0z * hz;
                if (det > -1e-12f && det < 1e-12f) continue;
                float inv = 1.0f / det;
                float u = inv * (sx * hx + sy * hy + sz * hz);
                // q = cross(s,e0)
                float qx = sy * e0z - sz * e0y;
                float qy = sz * e0x - sx * e0z;
                float qz = sx * e0y - sy * e0x;
                float vv = inv * (0.0f * qx + 1.0f * qy + 0.0f * qz); // dot(d,q)
                if (u < -1e-6f || vv < -1e-6f || u + vv > 1.0f + 1e-6f) continue;
                float tt = inv * (e2x * qx + e2y * qy + e2z * qz);  // dot(e2,q)
                if (tt > SNOWMASK_EPS) { open = 0; break; }
            }
            out[voff + i] = open ? 1.0f : 0.0f;
        }
        voff += vcounts[p];
    }

    free(cnt); free(off); free(cur); free(lst); free(tri);
    return out;
}