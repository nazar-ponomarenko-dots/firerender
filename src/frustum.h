// ============================================================================
//  frustum.h — отсечение фрустумом (6 плоскостей из матрицы viewproj)
// ----------------------------------------------------------------------------
//  Экстракция плоскостей по Gribb-Hartmann из viewproj. Плоскости в форме
//  ax+by+cz+d=0, нормали наружу. Проверки: точка, сфера (радиус), AABB.
//  Используется объектами, фауной и другими меш-модулями: если объект вне
//  фрустума — он не попадает в draw call (экономия на дорогих операциях
//  выборки текстур и переключении программы на СРАВНИМО дешёвых квадах).
// ============================================================================
#ifndef FRUSTUM_H
#define FRUSTUM_H

typedef struct {
    float p[6][4];   // 6 плоскостей: 0..5 = left,right,bottom,top,near,far
} Frustum;

// Строит фрустум из матрицы вид*проекция (OpenGL, колоночный порядок).
void frustum_build(Frustum *f, const float *viewproj);

// 1 = точка внутри (или на границе) фрустума
int frustum_test_point(const Frustum *f, float x, float y, float z);

// 1 = сфера (центр + радиус) пересекает фрустум (или внутри)
int frustum_test_sphere(const Frustum *f, float x, float y, float z, float r);

// 1 = AABB (min/max) пересекает фрустум
int frustum_test_aabb(const Frustum *f, const float *min, const float *max);

#endif
