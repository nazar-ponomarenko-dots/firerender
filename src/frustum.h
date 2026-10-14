// ============================================================================
//  frustum.h — frustum culling (6 planes extracted from the viewproj matrix)
// ----------------------------------------------------------------------------
//  Plane extraction via Gribb-Hartmann from viewproj. Planes in the form
//  ax+by+cz+d=0, normals pointing outward. Tests: point, sphere (radius), AABB.
//  Used by objects, fauna and other mesh modules: if an object is outside the
//  frustum — it doesn't reach the draw call (saving expensive texture-sampling
//  and program-switch operations on COMPARATIVELY cheap quads).
// ============================================================================
#ifndef FRUSTUM_H
#define FRUSTUM_H

typedef struct {
    float p[6][4];   // 6 planes: 0..5 = left,right,bottom,top,near,far
} Frustum;

// Builds the frustum from the view*projection matrix (OpenGL, column-major).
void frustum_build(Frustum *f, const float *viewproj);

// 1 = point inside (or on the boundary of) the frustum
int frustum_test_point(const Frustum *f, float x, float y, float z);

// 1 = sphere (center + radius) intersects the frustum (or is inside)
int frustum_test_sphere(const Frustum *f, float x, float y, float z, float r);

// 1 = AABB (min/max) intersects the frustum
int frustum_test_aabb(const Frustum *f, const float *min, const float *max);

#endif
