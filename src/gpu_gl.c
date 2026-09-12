// ============================================================================
//  gpu_gl.c — тонкие обёртки старого RHI-интерфейса (gpu.h) над SIMPLE RENDER
// ----------------------------------------------------------------------------
//  Исторически gpu.h был первым сеамом оркестратора кадра (renderer.c,
//  selection.c). Теперь вся реализация живёт в srender_gl.c (sr_*), а этот
//  файл только транслирует gpu_* в sr_* — чтобы существующие вызывающие не
//  трогать. Новый код должен звать sr_* напрямую.
// ============================================================================
#include "gpu.h"

#include "srender.h"
#include "resources.h"
#include "shader.h"

static SrDevice *s_sr = NULL;

// Непрозрачный в gpu.h тип завершаем здесь: маркер «GL-бэкенд через sr».
struct GpuDevice { int backend; };

const GpuDevice *gpu_create(void)
{
    if (!s_sr) s_sr = sr_create();
    static const GpuDevice s_dev = { 0 };
    return &s_dev;
}

// --- Пресет кадра ---
void gpu_bind_framebuffer(const GpuDevice *d, GpuFbo fbo)
{ (void)d; sr_bind_fbo(s_sr, fbo); }

void gpu_viewport(const GpuDevice *d, int x, int y, int w, int h)
{ (void)d; sr_viewport(s_sr, x, y, w, h); }

void gpu_clear(const GpuDevice *d, float r, float g, float b, float a)
{ (void)d; sr_clear(s_sr, r, g, b, a, 1, 1); }

// --- Состояние ---
void gpu_depth_mask(const GpuDevice *d, int write_enabled)
{ (void)d; sr_depth_mask(s_sr, write_enabled); }

void gpu_cap(const GpuDevice *d, GpuCap cap, int on)
{
    (void)d;
    static const SrStateCap map[] = {
        [GPU_CAP_DEPTH_TEST] = SR_CAP_DEPTH_TEST,
        [GPU_CAP_CULL_FACE]  = SR_CAP_CULL_FACE,
        [GPU_CAP_BLEND]      = SR_CAP_BLEND,
    };
    sr_cap(s_sr, map[cap], on);
}

void gpu_blend_func(const GpuDevice *d, GpuBlendFac src, GpuBlendFac dst)
{
    (void)d;
    static const SrBlendFac map[] = {
        [GPU_BLEND_SRC_ALPHA] = SR_BLEND_SRC_ALPHA,
        [GPU_BLEND_ONE_MINUS_SRC_ALPHA] = SR_BLEND_ONE_MINUS_SRC_ALPHA,
        [GPU_BLEND_ZERO] = SR_BLEND_ZERO,
        [GPU_BLEND_ONE] = SR_BLEND_ONE,
    };
    sr_blend_func(s_sr, map[src], map[dst]);
}

void gpu_wireframe(const GpuDevice *d, int on)
{ (void)d; sr_wireframe(s_sr, on); }

// --- Текстуры ---
GpuTex gpu_tex_gen(const GpuDevice *d)
{ (void)d; return sr_tex_gen(s_sr); }

void gpu_tex_delete(const GpuDevice *d, GpuTex *t)
{ (void)d; sr_tex_delete(s_sr, t); }

void gpu_bind_cubemap(const GpuDevice *d, GpuTex t)
{ (void)d; sr_tex_bind(s_sr, SR_TEX_CUBE, t); }

void gpu_bind_2d(const GpuDevice *d, GpuTex t)
{ (void)d; sr_tex_bind(s_sr, SR_TEX_2D, t); }

void gpu_cubemap_rgba8(const GpuDevice *d, GpuTex t, int size)
{
    (void)d;
    for (int face = 0; face < 6; face++)
        sr_tex_image_2d(s_sr, t, face, SR_FMT_RGBA8, size, size,
                        SR_PIX_RGBA, NULL);
}

void gpu_cubemap_face_rgba8(const GpuDevice *d, GpuTex t, int face,
                            int w, int h, const void *pixels)
{ (void)d; sr_tex_image_2d(s_sr, t, face, SR_FMT_RGBA8, w, h,
                           SR_PIX_RGBA, pixels); }

void gpu_cubemap_face_rgb8(const GpuDevice *d, GpuTex t, int face,
                           int w, int h, const void *pixels)
{ (void)d; sr_tex_image_2d(s_sr, t, face, SR_FMT_RGB8, w, h,
                           SR_PIX_RGB, pixels); }

void gpu_cubemap_face_depth24(const GpuDevice *d, GpuTex t, int face, int size)
{ (void)d; sr_tex_image_2d(s_sr, t, face, SR_FMT_DEPTH24, size, size,
                           SR_PIX_DEPTH_UINT, NULL); }

void gpu_tex_params_cubemap(const GpuDevice *d, GpuTex t, int linear_min)
{
    (void)d;
    sr_tex_params(s_sr, t, SR_TEX_CUBE,
                  linear_min ? SR_FILTER_LINEAR : SR_FILTER_NEAREST,
                  linear_min ? SR_FILTER_LINEAR : SR_FILTER_NEAREST,
                  SR_WRAP_CLAMP, SR_WRAP_CLAMP, SR_WRAP_CLAMP);
}

void gpu_active_texture(const GpuDevice *d, int unit)
{ (void)d; sr_tex_active(s_sr, unit); }

// --- Фреймбуферы ---
GpuFbo gpu_fbo_gen(const GpuDevice *d)
{ (void)d; return sr_fbo_gen(s_sr); }

void gpu_fbo_delete(const GpuDevice *d, GpuFbo *fbo)
{ (void)d; sr_fbo_delete(s_sr, fbo); }

void gpu_fbo_attach_cubemap_color(const GpuDevice *d, GpuFbo fbo,
                                  GpuTex t, int face)
{
    (void)d;
    sr_fbo_attach_tex(s_sr, fbo, 0, SR_TEX_CUBE, t, face);
}

void gpu_fbo_attach_cubemap_depth(const GpuDevice *d, GpuFbo fbo,
                                  GpuTex t, int face)
{
    (void)d;
    sr_fbo_attach_tex(s_sr, fbo, -1, SR_TEX_CUBE, t, face);
}

int gpu_fbo_complete(const GpuDevice *d)
{ (void)d; return sr_fbo_complete(s_sr); }

int gpu_fbo_current(const GpuDevice *d)
{ (void)d; return sr_fbo_current(s_sr); }

// --- Чтение обратно ---
void gpu_read_rgb(const GpuDevice *d, int x, int y, int w, int h, void *out)
{ (void)d; sr_read_pixels(s_sr, x, y, w, h, SR_PIX_RGB, out); }

// --- Программы/шейдеры ---
GpuProg gpu_program_new(const GpuDevice *d, const char *vs_path, const char *fs_path)
{ (void)d; return sr_prog_new(s_sr, vs_path, fs_path); }

void gpu_program_delete(const GpuDevice *d, GpuProg *prog)
{ (void)d; sr_prog_delete(s_sr, prog); }

void gpu_use_program(const GpuDevice *d, GpuProg prog)
{ (void)d; sr_prog_use(s_sr, prog); }

GpuLoc gpu_program_loc(const GpuDevice *d, GpuProg prog, const char *name)
{ (void)d; return (GpuLoc)sr_prog_loc(s_sr, prog, name); }

// --- Вершинные массивы и буферы ---
GpuVao gpu_vao_gen(const GpuDevice *d)
{ (void)d; return sr_vao_gen(s_sr); }

void gpu_vao_delete(const GpuDevice *d, GpuVao *vao)
{ (void)d; sr_vao_delete(s_sr, vao); }

void gpu_vao_bind(const GpuDevice *d, GpuVao vao)
{ (void)d; sr_vao_bind(s_sr, vao); }

GpuBuf gpu_buf_gen(const GpuDevice *d)
{ (void)d; return sr_buf_gen(s_sr); }

void gpu_buf_delete(const GpuDevice *d, GpuBuf *buf)
{ (void)d; sr_buf_delete(s_sr, buf); }

void gpu_buf_bind_array(const GpuDevice *d, GpuBuf buf)
{ (void)d; sr_buf_bind(s_sr, SR_BUF_ARRAY, buf); }

void gpu_buf_upload_array(const GpuDevice *d, const void *data,
                          unsigned int bytes, int stream)
{ (void)d; sr_buf_upload(s_sr, SR_BUF_ARRAY, data, bytes, !stream); }

void gpu_vertex_attr_3f(const GpuDevice *d, GpuLoc loc,
                        int stride_bytes, const void *offset)
{
    (void)d;
    sr_attrib_float(s_sr, loc, 3, stride_bytes,
                    (unsigned int)(size_t)offset, 0);
}

// --- Юниформы ---
void gpu_uniform_mat4(const GpuDevice *d, GpuLoc loc, const float *m)
{ (void)d; sr_uniform_mat4(s_sr, loc, m); }

void gpu_uniform_float3(const GpuDevice *d, GpuLoc loc, float x, float y, float z)
{ (void)d; sr_uniform_float3(s_sr, loc, x, y, z); }

// --- Рисование ---
void gpu_draw_arrays(const GpuDevice *d, GpuPrim prim, int vertex_count)
{
    (void)d;
    sr_draw_arrays(s_sr, prim == GPU_PRIM_LINES ? SR_PRIM_LINES
                                                : SR_PRIM_TRIANGLES,
                   0, vertex_count);
}
