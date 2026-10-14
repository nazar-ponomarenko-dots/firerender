// ============================================================================
//  linalg.h — a small "hand-rolled glm": vectors and 4x4 matrices
// ----------------------------------------------------------------------------
//  Vectors are plain float[3]; 4x4 matrices are float[16] in column-major
//  order (as OpenGL expects them).
// ============================================================================
#ifndef LINALG_H
#define LINALG_H

void vec3_normalize(float *v);
void vec3_cross(const float *a, const float *b, float *out);
float vec3_dot(const float *a, const float *b);

// out = a * b (4x4 matrix multiply, column-major order)
void mat4_mul(float *out, const float *a, const float *b);

// Perspective projection matrix (fovy in radians)
void mat4_perspective(float *m, float fovy, float aspect, float zn, float zf);

// Orthographic projection matrix (box)
void mat4_ortho(float *m, float l, float r, float b, float t, float zn, float zf);

// View matrix: "look from eye at center"
void mat4_lookat(float *m, const float *eye, const float *center, const float *up);

// Inverse 4x4 matrix (Gauss-Jordan). 1 on success, 0 if singular.
int mat4_invert(float *out, const float *m);

#endif
