// ============================================================================
//  linalg.h — маленький "glm своими руками": векторы и матрицы 4x4
// ----------------------------------------------------------------------------
//  Векторы — просто float[3], матрицы 4x4 — float[16] в колоночном порядке
//  (как их ждёт OpenGL).
// ============================================================================
#ifndef LINALG_H
#define LINALG_H

void vec3_normalize(float *v);
void vec3_cross(const float *a, const float *b, float *out);
float vec3_dot(const float *a, const float *b);

// out = a * b (умножение матриц 4x4, колоночный порядок)
void mat4_mul(float *out, const float *a, const float *b);

// Матрица перспективной проекции (fovy в радианах)
void mat4_perspective(float *m, float fovy, float aspect, float zn, float zf);

// Матрица ортографической проекции (бокс)
void mat4_ortho(float *m, float l, float r, float b, float t, float zn, float zf);

// Матрица вида "смотрим из eye на center"
void mat4_lookat(float *m, const float *eye, const float *center, const float *up);

// Обратная матрица 4x4 (Gauss-Jordan). 1 — успех, 0 — вырожденная.
int mat4_invert(float *out, const float *m);

#endif
