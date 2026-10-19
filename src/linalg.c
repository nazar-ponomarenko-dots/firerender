// ============================================================================
//  math.c — vectors and matrices for the camera and projection
// ============================================================================
#include "linalg.h"

#include <math.h>
#include <string.h>

void vec3_normalize(float *v)
{
    float len = sqrtf(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
    if (len > 1e-8f) { v[0] /= len; v[1] /= len; v[2] /= len; }
}

void vec3_cross(const float *a, const float *b, float *out)
{
    out[0] = a[1]*b[2] - a[2]*b[1];
    out[1] = a[2]*b[0] - a[0]*b[2];
    out[2] = a[0]*b[1] - a[1]*b[0];
}

float vec3_dot(const float *a, const float *b)
{
    return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}

void mat4_mul(float *out, const float *a, const float *b)
{
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++) {
            out[c*4 + r] = a[0*4+r]*b[c*4+0] + a[1*4+r]*b[c*4+1]
                         + a[2*4+r]*b[c*4+2] + a[3*4+r]*b[c*4+3];
        }
}

void mat4_perspective(float *m, float fovy, float aspect, float zn, float zf)
{
    float t = tanf(fovy / 2.0f);
    memset(m, 0, 16 * sizeof(float));
    m[0]  = 1.0f / (t * aspect);
    m[5]  = 1.0f / t;
    m[10] = (zf + zn) / (zn - zf);
    m[11] = -1.0f;
    m[14] = (2.0f * zf * zn) / (zn - zf);
}

void mat4_ortho(float *m, float l, float r, float b, float t, float zn, float zf)
{
    memset(m, 0, 16 * sizeof(float));
    m[0]  = 2.0f / (r - l);
    m[5]  = 2.0f / (t - b);
    m[10] = -2.0f / (zf - zn);
    m[12] = -(r + l) / (r - l);
    m[13] = -(t + b) / (t - b);
    m[14] = -(zf + zn) / (zf - zn);
    m[15] = 1.0f;
}

void mat4_lookat(float *m, const float *eye, const float *center, const float *up)
{
    float f[3] = { center[0]-eye[0], center[1]-eye[1], center[2]-eye[2] };
    vec3_normalize(f);
    float s[3], u[3];
    vec3_cross(f, up, s);  vec3_normalize(s);  // right
    vec3_cross(s, f, u);                        // up (already unit)
    m[0]=s[0]; m[1]=u[0]; m[2]=-f[0]; m[3]=0;
    m[4]=s[1]; m[5]=u[1]; m[6]=-f[1]; m[7]=0;
    m[8]=s[2]; m[9]=u[2]; m[10]=-f[2]; m[11]=0;
    m[12]=-vec3_dot(s,eye); m[13]=-vec3_dot(u,eye); m[14]=vec3_dot(f,eye); m[15]=1;
}

// Inverse 4x4 matrix via Gauss-Jordan. out may alias m.
int mat4_invert(float *out, const float *m)
{
    float a[16], inv[16] = {
        1,0,0,0,  0,1,0,0,  0,0,1,0,  0,0,0,1,
    };
    memcpy(a, m, sizeof a);
    for (int c = 0; c < 4; c++) {
        int piv = c;
        for (int r = c + 1; r < 4; r++)
            if (fabsf(a[r*4+c]) > fabsf(a[piv*4+c])) piv = r;
        if (fabsf(a[piv*4+c]) < 1e-12f) return 0;
        if (piv != c)
            for (int k = 0; k < 4; k++) {
                float t = a[c*4+k]; a[c*4+k] = a[piv*4+k]; a[piv*4+k] = t;
                t = inv[c*4+k]; inv[c*4+k] = inv[piv*4+k]; inv[piv*4+k] = t;
            }
        float d = a[c*4+c];
        for (int k = 0; k < 4; k++) {
            a[c*4+k] /= d;
            inv[c*4+k] /= d;
        }
        for (int r = 0; r < 4; r++) {
            if (r == c) continue;
            float f = a[r*4+c];
            if (f == 0.0f) continue;
            for (int k = 0; k < 4; k++) {
                a[r*4+k] -= f * a[c*4+k];
                inv[r*4+k] -= f * inv[c*4+k];
            }
        }
    }
    memcpy(out, inv, sizeof inv);
    return 1;
}
