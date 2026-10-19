// ============================================================================
//  lights.c — clustered lighting (Forward+): assigning lights to froxels
// ----------------------------------------------------------------------------
//  See lights.h. Assignment runs on the CPU and costs O(lights x touched froxels):
//  for each influence sphere its screen rectangle (projection of the bounding
//  sphere) and depth-slice range are computed — only those are iterated, not the
//  whole grid.
//
//  SSBO light format (4 x vec4 = 64 bytes, std430):
//    v0 = pos.xyz,   type
//    v1 = color.rgb, intensity
//    v2 = dir.xyz,   range
//    v3.x = radius / half_len, v3.y = cos_outer, v3.z = cos_inner,
//    v3.w = packed rect half-sizes (w in the integer part, h in the fraction)
//    v4 = up.xyz, size.x  (the fifth vector is needed for rect/tube lights)
//  Total 5 x vec4 = 80 bytes per light.
// ============================================================================
#include "lights.h"

#include "cvars.h"
#include "linalg.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define LIGHT_VEC4 5                       // vec4 per light in the SSBO

static Light s_lights[LIGHT_MAX];
static int s_count;

// GPU buffers: lights, froxel indices, ranges (offset,count)
static GLuint s_ssbo_lights, s_ssbo_index, s_ssbo_grid;
static int s_have_ssbo;

// CPU staging
static float s_gpu_lights[LIGHT_MAX * LIGHT_VEC4 * 4];
static unsigned int s_grid[CLUSTER_COUNT * 2];         // offset, count
static unsigned int s_index[CLUSTER_INDEX_MAX];
static int s_index_n;
static int s_cluster_max;

// Per-froxel light list during assembly (grows into s_index)
static unsigned short s_bucket[CLUSTER_COUNT][24];
static unsigned char s_bucket_n[CLUSTER_COUNT];

// Grid parameters for this frame (needed by the shader)
static float s_znear = 0.1f, s_zfar = 1000.0f;
static float s_scale, s_bias;          // log distribution of depth slices
static int s_vw = 1, s_vh = 1;

int lights_count(void) { return s_count; }
int lights_cluster_fill(void) { return s_index_n; }
int lights_cluster_max(void) { return s_cluster_max; }

int lights_active(void)
{
    return s_have_ssbo && cvar_get("r_clustered") >= 0.5f;
}

void lights_init(void)
{
    cvar_register_ex("r_clustered", 1.0f, 0.0f, 1.0f, CVAR_ARCHIVE,
                     "кластерное освещение (Forward+): много источников");
    cvar_register_ex("r_light_debug", 0.0f, 0.0f, 2.0f, CVAR_ARCHIVE,
                     "отладка кластеров: 1 = тепловая карта источников");
    if (!g_gl_gpu_driven) {
        SDL_Log("Lights: clustered path unavailable (need GL 4.3+), "
                "falling back to uniform lights");
        s_have_ssbo = 0;
        return;
    }
    if (!s_ssbo_lights) {
        glGenBuffers(1, &s_ssbo_lights);
        glGenBuffers(1, &s_ssbo_index);
        glGenBuffers(1, &s_ssbo_grid);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_ssbo_lights);
        glBufferData(GL_SHADER_STORAGE_BUFFER,
                     sizeof(float) * LIGHT_MAX * LIGHT_VEC4 * 4, NULL,
                     GL_DYNAMIC_DRAW);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_ssbo_index);
        glBufferData(GL_SHADER_STORAGE_BUFFER,
                     sizeof(unsigned int) * CLUSTER_INDEX_MAX, NULL,
                     GL_DYNAMIC_DRAW);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_ssbo_grid);
        glBufferData(GL_SHADER_STORAGE_BUFFER,
                     sizeof(unsigned int) * CLUSTER_COUNT * 2, NULL,
                     GL_DYNAMIC_DRAW);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    }
    s_have_ssbo = 1;
    SDL_Log("Lights: clustered forward+ ready (%dx%dx%d froxels, %d lights max)",
            CLUSTER_X, CLUSTER_Y, CLUSTER_Z, LIGHT_MAX);
}

void lights_shutdown(void)
{
    if (s_ssbo_lights) {
        glDeleteBuffers(1, &s_ssbo_lights);
        glDeleteBuffers(1, &s_ssbo_index);
        glDeleteBuffers(1, &s_ssbo_grid);
        s_ssbo_lights = s_ssbo_index = s_ssbo_grid = 0;
    }
    s_have_ssbo = 0;
    s_count = 0;
    s_index_n = 0;
}

void lights_begin(void)
{
    s_count = 0;
}

int lights_add(const Light *l)
{
    if (!l || s_count >= LIGHT_MAX) return -1;
    if (l->intensity <= 0.0001f || l->range <= 0.01f) return -1;
    s_lights[s_count] = *l;
    return s_count++;
}

static void vec_copy3(float *d, const float *s, float dx, float dy, float dz)
{
    if (s) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; }
    else   { d[0] = dx;   d[1] = dy;   d[2] = dz;   }
}

int lights_add_point(const float pos[3], const float color[3],
                     float intensity, float range, float radius)
{
    Light l;
    memset(&l, 0, sizeof l);
    l.type = LT_POINT;
    vec_copy3(l.pos, pos, 0, 0, 0);
    vec_copy3(l.color, color, 1, 1, 1);
    l.intensity = intensity;
    l.range = range;
    l.radius = radius > 0.0f ? radius : 0.05f;
    return lights_add(&l);
}

int lights_add_spot(const float pos[3], const float dir[3],
                    const float color[3], float intensity, float range,
                    float angle_outer, float angle_inner, float radius)
{
    Light l;
    memset(&l, 0, sizeof l);
    l.type = LT_SPOT;
    vec_copy3(l.pos, pos, 0, 0, 0);
    vec_copy3(l.dir, dir, 0, -1, 0);
    vec_copy3(l.color, color, 1, 1, 1);
    l.intensity = intensity;
    l.range = range;
    if (angle_inner <= 0.0f || angle_inner > angle_outer)
        angle_inner = angle_outer * 0.75f;
    l.cos_outer = cosf(angle_outer);
    l.cos_inner = cosf(angle_inner);
    l.radius = radius > 0.0f ? radius : 0.05f;
    return lights_add(&l);
}

int lights_add_rect(const float pos[3], const float dir[3], const float up[3],
                    const float color[3], float intensity, float range,
                    float half_w, float half_h)
{
    Light l;
    memset(&l, 0, sizeof l);
    l.type = LT_RECT;
    vec_copy3(l.pos, pos, 0, 0, 0);
    vec_copy3(l.dir, dir, 0, -1, 0);
    vec_copy3(l.up, up, 0, 0, 1);
    vec_copy3(l.color, color, 1, 1, 1);
    l.intensity = intensity;
    l.range = range;
    l.size[0] = half_w > 0.0f ? half_w : 0.5f;
    l.size[1] = half_h > 0.0f ? half_h : 0.5f;
    l.radius = 0.5f * (l.size[0] + l.size[1]);
    return lights_add(&l);
}

int lights_add_tube(const float pos[3], const float axis[3],
                    const float color[3], float intensity, float range,
                    float half_len, float radius)
{
    Light l;
    memset(&l, 0, sizeof l);
    l.type = LT_TUBE;
    vec_copy3(l.pos, pos, 0, 0, 0);
    vec_copy3(l.up, axis, 1, 0, 0);
    vec_copy3(l.color, color, 1, 1, 1);
    l.intensity = intensity;
    l.range = range;
    l.size[0] = half_len > 0.0f ? half_len : 0.5f;
    l.radius = radius > 0.0f ? radius : 0.05f;
    return lights_add(&l);
}

// --- Assignment to froxels -----------------------------------------------------
// Depth slices: logarithmic distribution (like Doom 2016):
//   slice = floor(log(z / znear) * scale + bias), z in view space (meters).
static int depth_slice(float z)
{
    if (z <= s_znear) return 0;
    int s = (int)(logf(z / s_znear) * s_scale);
    if (s < 0) s = 0;
    if (s > CLUSTER_Z - 1) s = CLUSTER_Z - 1;
    return s;
}

void lights_build(const RendererContext *rc)
{
    s_index_n = 0;
    s_cluster_max = 0;
    if (!lights_active() || s_count == 0) {
        // Empty grid: the shader sees count = 0 and takes the fast path.
        memset(s_grid, 0, sizeof s_grid);
        if (s_have_ssbo) {
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_ssbo_grid);
            glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof s_grid, s_grid);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        }
        return;
    }
    s_znear = rc->znear > 0.001f ? rc->znear : 0.1f;
    s_zfar = rc->zfar > s_znear ? rc->zfar : s_znear + 1.0f;
    s_vw = rc->view_vp[2] > 0 ? rc->view_vp[2] : rc->vw;
    s_vh = rc->view_vp[3] > 0 ? rc->view_vp[3] : rc->vh;
    s_scale = (float)CLUSTER_Z / logf(s_zfar / s_znear);
    s_bias = 0.0f;

    memset(s_bucket_n, 0, sizeof s_bucket_n);

    // View basis to transform world into view space (without building a matrix):
    // z_view = dot(pos - cam, fwd), x/y — via right/up.
    const float *cam = rc->view_pos;
    const float *fwd = rc->view_fwd;
    const float *right = rc->view_right;
    const float *up = rc->view_up;
    float tan_half = tanf(0.5f * rc->cam_fov_deg * 0.0174532925f);
    float aspect = rc->cam_aspect > 0.01f ? rc->cam_aspect : 1.7778f;

    for (int i = 0; i < s_count; i++) {
        const Light *l = &s_lights[i];
        // Influence sphere: center + radius (expanded for rect/tube lights).
        float cx = l->pos[0] - cam[0];
        float cy = l->pos[1] - cam[1];
        float cz = l->pos[2] - cam[2];
        float vz = cx * fwd[0] + cy * fwd[1] + cz * fwd[2];
        float vx = cx * right[0] + cy * right[1] + cz * right[2];
        float vy = cx * up[0] + cy * up[1] + cz * up[2];
        float r = l->range;
        if (l->type == LT_RECT)
            r += sqrtf(l->size[0] * l->size[0] + l->size[1] * l->size[1]);
        else if (l->type == LT_TUBE)
            r += l->size[0];

        // Fully behind the camera or beyond far — skip
        if (vz + r < s_znear || vz - r > s_zfar) continue;

        // Depth slice range
        int z0 = depth_slice(vz - r);
        int z1 = depth_slice(vz + r);

        // Screen rectangle of the sphere: conservatively via the near point
        float zc = vz - r;
        if (zc < s_znear) zc = s_znear;
        float half_h = tan_half * zc;          // half frustum height at zc
        float half_w = half_h * aspect;
        // NDC extent of the sphere (expanded by the radius projected at zc)
        float px0 = (vx - r) / half_w, px1 = (vx + r) / half_w;
        float py0 = (vy - r) / half_h, py1 = (vy + r) / half_h;
        if (px1 < -1.0f || px0 > 1.0f || py1 < -1.0f || py0 > 1.0f) continue;
        int tx0 = (int)((px0 * 0.5f + 0.5f) * CLUSTER_X);
        int tx1 = (int)((px1 * 0.5f + 0.5f) * CLUSTER_X);
        int ty0 = (int)((py0 * 0.5f + 0.5f) * CLUSTER_Y);
        int ty1 = (int)((py1 * 0.5f + 0.5f) * CLUSTER_Y);
        if (tx0 < 0) tx0 = 0;
        if (ty0 < 0) ty0 = 0;
        if (tx1 > CLUSTER_X - 1) tx1 = CLUSTER_X - 1;
        if (ty1 > CLUSTER_Y - 1) ty1 = CLUSTER_Y - 1;

        for (int z = z0; z <= z1; z++)
            for (int y = ty0; y <= ty1; y++)
                for (int x = tx0; x <= tx1; x++) {
                    int c = (z * CLUSTER_Y + y) * CLUSTER_X + x;
                    if (s_bucket_n[c] >= 24) continue;   // froxel overflowed
                    s_bucket[c][s_bucket_n[c]++] = (unsigned short)i;
                }
    }

    // Flat buffers: grid[(offset,count)] + one contiguous index list
    for (int c = 0; c < CLUSTER_COUNT; c++) {
        int n = s_bucket_n[c];
        if (n > s_cluster_max) s_cluster_max = n;
        s_grid[c * 2 + 0] = (unsigned int)s_index_n;
        s_grid[c * 2 + 1] = (unsigned int)n;
        for (int k = 0; k < n && s_index_n < CLUSTER_INDEX_MAX; k++)
            s_index[s_index_n++] = s_bucket[c][k];
    }

    // Pack lights into the SSBO
    for (int i = 0; i < s_count; i++) {
        const Light *l = &s_lights[i];
        float *v = &s_gpu_lights[i * LIGHT_VEC4 * 4];
        v[0] = l->pos[0];   v[1] = l->pos[1];   v[2] = l->pos[2];
        v[3] = (float)l->type;
        v[4] = l->color[0] * l->intensity;
        v[5] = l->color[1] * l->intensity;
        v[6] = l->color[2] * l->intensity;
        v[7] = l->intensity;
        // Normalize the direction here — the shader gets it ready-made
        float dl = sqrtf(l->dir[0] * l->dir[0] + l->dir[1] * l->dir[1] +
                         l->dir[2] * l->dir[2]);
        if (dl < 1e-5f) dl = 1.0f;
        v[8] = l->dir[0] / dl;  v[9] = l->dir[1] / dl;  v[10] = l->dir[2] / dl;
        v[11] = l->range;
        v[12] = l->radius;
        v[13] = l->cos_outer;
        v[14] = l->cos_inner;
        v[15] = l->size[1];
        float ul = sqrtf(l->up[0] * l->up[0] + l->up[1] * l->up[1] +
                         l->up[2] * l->up[2]);
        if (ul < 1e-5f) ul = 1.0f;
        v[16] = l->up[0] / ul;  v[17] = l->up[1] / ul;  v[18] = l->up[2] / ul;
        v[19] = l->size[0];
    }

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_ssbo_lights);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
                    (GLsizeiptr)(sizeof(float) * (size_t)s_count * LIGHT_VEC4 * 4),
                    s_gpu_lights);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_ssbo_grid);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof s_grid, s_grid);
    if (s_index_n > 0) {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_ssbo_index);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
                        (GLsizeiptr)(sizeof(unsigned int) * (size_t)s_index_n),
                        s_index);
    }
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}

// --- Binding to a program -------------------------------------------------------
// Handle cache: each program has its own locations, but the same set of them.
#define LOC_CACHE 32
static struct {
    GLuint prog;
    GLint u_on, u_grid, u_znear, u_zscale, u_screen, u_debug;
    int bound_blocks;
} s_locs[LOC_CACHE];
static int s_locs_n;

static int locs_index(GLuint prog)
{
    for (int i = 0; i < s_locs_n; i++)
        if (s_locs[i].prog == prog) return i;
    if (s_locs_n >= LOC_CACHE) return -1;
    int i = s_locs_n++;
    s_locs[i].prog = prog;
    s_locs[i].u_on     = glGetUniformLocation(prog, "uClusterOn");
    s_locs[i].u_grid   = glGetUniformLocation(prog, "uClusterGrid");
    s_locs[i].u_znear  = glGetUniformLocation(prog, "uClusterZNear");
    s_locs[i].u_zscale = glGetUniformLocation(prog, "uClusterZScale");
    s_locs[i].u_screen = glGetUniformLocation(prog, "uClusterScreen");
    s_locs[i].u_debug  = glGetUniformLocation(prog, "uLightDebug");
    s_locs[i].bound_blocks = 0;
    return i;
}

void lights_locs_init(GLuint prog)
{
    if (!prog) return;
    locs_index(prog);
}

void lights_bind(GLuint prog)
{
    if (!prog) return;
    int i = locs_index(prog);
    if (i < 0) return;
    int on = lights_active() && s_locs[i].u_on >= 0;
    if (s_locs[i].u_on >= 0)
        glUniform1f(s_locs[i].u_on, on ? 1.0f : 0.0f);
    if (!on) return;
    // Shader SSBO blocks -> binding points 3/4/5 (0..2 are taken by veg-cull,
    // but that one runs in its own program, so there is no conflict).
    if (!s_locs[i].bound_blocks && p_glGetProgramResourceIndex &&
        p_glShaderStorageBlockBinding) {
        GLuint bl = p_glGetProgramResourceIndex(prog, GL_SHADER_STORAGE_BLOCK,
                                                "ClusterLights");
        GLuint bg = p_glGetProgramResourceIndex(prog, GL_SHADER_STORAGE_BLOCK,
                                                "ClusterGrid");
        GLuint bi = p_glGetProgramResourceIndex(prog, GL_SHADER_STORAGE_BLOCK,
                                                "ClusterIndex");
        if (bl != GL_INVALID_INDEX) glShaderStorageBlockBinding(prog, bl, 3);
        if (bg != GL_INVALID_INDEX) glShaderStorageBlockBinding(prog, bg, 4);
        if (bi != GL_INVALID_INDEX) glShaderStorageBlockBinding(prog, bi, 5);
        s_locs[i].bound_blocks = 1;
    }
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, s_ssbo_lights);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, s_ssbo_grid);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, s_ssbo_index);
    if (s_locs[i].u_grid >= 0)
        glUniform3f(s_locs[i].u_grid, (float)CLUSTER_X, (float)CLUSTER_Y,
                    (float)CLUSTER_Z);
    if (s_locs[i].u_znear >= 0) glUniform1f(s_locs[i].u_znear, s_znear);
    if (s_locs[i].u_zscale >= 0) glUniform1f(s_locs[i].u_zscale, s_scale);
    if (s_locs[i].u_screen >= 0)
        glUniform2f(s_locs[i].u_screen, (float)s_vw, (float)s_vh);
    if (s_locs[i].u_debug >= 0)
        glUniform1f(s_locs[i].u_debug, cvar_get("r_light_debug"));
}
