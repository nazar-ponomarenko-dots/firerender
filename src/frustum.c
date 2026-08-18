// ============================================================================
//  frustum.c — отсечение фрустумом
// ============================================================================
#include "frustum.h"

#include <math.h>

// Gribb-Hartmann: плоскости из строк матрицы (OpenGL column-major viewproj).
// Плоскости нормализованы (деление на длину (A,B,C)).
void frustum_build(Frustum *f, const float *m)
{
    // left   = row4 + row1
    f->p[0][0] = m[3] + m[0];  f->p[0][1] = m[7] + m[4];
    f->p[0][2] = m[11] + m[8]; f->p[0][3] = m[15] + m[12];
    // right  = row4 - row1
    f->p[1][0] = m[3] - m[0];  f->p[1][1] = m[7] - m[4];
    f->p[1][2] = m[11] - m[8]; f->p[1][3] = m[15] - m[12];
    // bottom = row4 + row2
    f->p[2][0] = m[3] + m[1];  f->p[2][1] = m[7] + m[5];
    f->p[2][2] = m[11] + m[9]; f->p[2][3] = m[15] + m[13];
    // top    = row4 - row2
    f->p[3][0] = m[3] - m[1];  f->p[3][1] = m[7] - m[5];
    f->p[3][2] = m[11] - m[9]; f->p[3][3] = m[15] - m[13];
    // near   = row4 + row3
    f->p[4][0] = m[3] + m[2];  f->p[4][1] = m[7] + m[6];
    f->p[4][2] = m[11] + m[10]; f->p[4][3] = m[15] + m[14];
    // far    = row4 - row3
    f->p[5][0] = m[3] - m[2];  f->p[5][1] = m[7] - m[6];
    f->p[5][2] = m[11] - m[10]; f->p[5][3] = m[15] - m[14];

    for (int i = 0; i < 6; i++) {
        float len = sqrtf(f->p[i][0] * f->p[i][0] +
                          f->p[i][1] * f->p[i][1] +
                          f->p[i][2] * f->p[i][2]);
        if (len < 1e-6f) len = 1.0f;
        for (int k = 0; k < 4; k++) f->p[i][k] /= len;
    }
}

int frustum_test_point(const Frustum *f, float x, float y, float z)
{
    for (int i = 0; i < 6; i++) {
        float d = f->p[i][0] * x + f->p[i][1] * y + f->p[i][2] * z + f->p[i][3];
        if (d < 0.0f) return 0;   // снаружи этой плоскости
    }
    return 1;
}

int frustum_test_sphere(const Frustum *f, float x, float y, float z, float r)
{
    for (int i = 0; i < 6; i++) {
        float d = f->p[i][0] * x + f->p[i][1] * y + f->p[i][2] * z + f->p[i][3];
        if (d < -r) return 0;     // полностью за плоскостью
    }
    return 1;                     // пересекает или внутри
}

int frustum_test_aabb(const Frustum *f, const float *min, const float *max)
{
    // Плоскость хранится так, что точка внутри/на границе фрустума даёт
    // n*p + d >= 0. Бокс НЕ пересекает фрустум только если он ЦЕЛИКОМ по
    // внешнюю сторону какой-то плоскости — для этого достаточно проверить
    // одну его вершину: ту, что МАКСИМИЗИРУЕТ n*p по каждой оси (p-vertex).
    // Если даже она за плоскостью (n*p+d < 0) — весь бокс снаружи.
    for (int i = 0; i < 6; i++) {
        float px = f->p[i][0] > 0 ? max[0] : min[0];
        float py = f->p[i][1] > 0 ? max[1] : min[1];
        float pz = f->p[i][2] > 0 ? max[2] : min[2];
        float d = f->p[i][0] * px + f->p[i][1] * py + f->p[i][2] * pz + f->p[i][3];
        if (d < 0.0f) return 0;   // самая «внутренняя» вершина снаружи
    }
    return 1;
}
