// ============================================================================
//  gpu_gl.c — реализация RHI на OpenGL 3.3 (единственный пока бэкенд)
// ----------------------------------------------------------------------------
//  Маппинг абстракций gpu.h на настоящие GL-вызовы. Все GL-точки входа
//  подгружены через gl_loader.h (SDL_GL_GetProcAddress) до gpu_create().
// ============================================================================
#include "gpu.h"

#include "gl_loader.h"
#include "resources.h"
#include "shader.h"

#include <stdlib.h>

// Бэкенд-девайс: непрозрачный тип завершаем здесь (в реализации). Для GL
// никакого внутреннего состояния не нужно — просто маркер "GL-бэкенд".
struct GpuDevice { int backend; };
static const GpuDevice s_dev = { 0 };

const GpuDevice *gpu_create(void)
{
    return &s_dev;
}

// --- Пресет кадра ---
void gpu_bind_framebuffer(const GpuDevice *d, GpuFbo fbo)
{
    (void)d;
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
}

void gpu_viewport(const GpuDevice *d, int x, int y, int w, int h)
{
    (void)d;
    glViewport(x, y, w, h);
}

void gpu_clear(const GpuDevice *d, float r, float g, float b, float a)
{
    (void)d;
    glClearColor(r, g, b, a);
    glDepthMask(GL_TRUE);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

// --- Состояние ---
void gpu_depth_mask(const GpuDevice *d, int write_enabled)
{
    (void)d;
    glDepthMask(write_enabled ? GL_TRUE : GL_FALSE);
}

void gpu_cap(const GpuDevice *d, GpuCap cap, int on)
{
    (void)d;
    GLenum glcap;
    switch (cap) {
        case GPU_CAP_DEPTH_TEST: glcap = GL_DEPTH_TEST; break;
        case GPU_CAP_CULL_FACE:  glcap = GL_CULL_FACE;  break;
        case GPU_CAP_BLEND:      glcap = GL_BLEND;      break;
        default: return;
    }
    if (on) glEnable(glcap); else glDisable(glcap);
}

static GLenum blend_fac(GpuBlendFac f)
{
    switch (f) {
        case GPU_BLEND_SRC_ALPHA: return GL_SRC_ALPHA;
        case GPU_BLEND_ONE_MINUS_SRC_ALPHA: return GL_ONE_MINUS_SRC_ALPHA;
        case GPU_BLEND_ZERO: return GL_ZERO;
        case GPU_BLEND_ONE: return GL_ONE;
        default: return GL_SRC_ALPHA;
    }
}

void gpu_blend_func(const GpuDevice *d, GpuBlendFac src, GpuBlendFac dst)
{
    (void)d;
    glBlendFunc(blend_fac(src), blend_fac(dst));
}

void gpu_wireframe(const GpuDevice *d, int on)
{
    (void)d;
#ifndef USE_GLES_PATH
    glPolygonMode(GL_FRONT_AND_BACK, on ? GL_LINE : GL_FILL);
#endif
}

// --- Текстуры ---
GpuTex gpu_tex_gen(const GpuDevice *d)
{
    (void)d;
    GLuint id = 0;
    glGenTextures(1, &id);
    return id;
}

void gpu_tex_delete(const GpuDevice *d, GpuTex *t)
{
    (void)d;
    if (!t || !*t) return;
    glDeleteTextures(1, t);
    *t = 0;
}

void gpu_bind_cubemap(const GpuDevice *d, GpuTex t)
{
    (void)d;
    glBindTexture(GL_TEXTURE_CUBE_MAP, t);
}

void gpu_bind_2d(const GpuDevice *d, GpuTex t)
{
    (void)d;
    glBindTexture(GL_TEXTURE_2D, t);
}

void gpu_cubemap_rgba8(const GpuDevice *d, GpuTex t, int size)
{
    (void)d;
    glBindTexture(GL_TEXTURE_CUBE_MAP, t);
    for (int face = 0; face < 6; face++) {
        glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, 0, GL_RGBA8,
                     size, size, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    }
}

void gpu_cubemap_face_rgba8(const GpuDevice *d, GpuTex t, int face,
                            int w, int h, const void *pixels)
{
    (void)d;
    glBindTexture(GL_TEXTURE_CUBE_MAP, t);
    glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, 0, GL_RGBA8,
                 w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
}

void gpu_cubemap_face_rgb8(const GpuDevice *d, GpuTex t, int face,
                           int w, int h, const void *pixels)
{
    (void)d;
    glBindTexture(GL_TEXTURE_CUBE_MAP, t);
    glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, 0, GL_RGB8,
                 w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, pixels);
}

void gpu_cubemap_face_depth24(const GpuDevice *d, GpuTex t, int face, int size)
{
    (void)d;
    glBindTexture(GL_TEXTURE_CUBE_MAP, t);
    glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, 0, GL_DEPTH_COMPONENT24,
                 size, size, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, NULL);
}

void gpu_tex_params_cubemap(const GpuDevice *d, GpuTex t, int linear_min)
{
    (void)d;
    glBindTexture(GL_TEXTURE_CUBE_MAP, t);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER,
                    linear_min ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER,
                    linear_min ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
}

void gpu_active_texture(const GpuDevice *d, int unit)
{
    (void)d;
    glActiveTexture((GLenum)(GL_TEXTURE0 + unit));
}

// --- Фреймбуферы ---
GpuFbo gpu_fbo_gen(const GpuDevice *d)
{
    (void)d;
    GLuint id = 0;
    glGenFramebuffers(1, &id);
    return id;
}

void gpu_fbo_delete(const GpuDevice *d, GpuFbo *fbo)
{
    (void)d;
    if (!fbo || !*fbo) return;
    glDeleteFramebuffers(1, fbo);
    *fbo = 0;
}

void gpu_fbo_attach_cubemap_color(const GpuDevice *d, GpuFbo fbo,
                                  GpuTex t, int face)
{
    (void)d;
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, t, 0);
}

void gpu_fbo_attach_cubemap_depth(const GpuDevice *d, GpuFbo fbo,
                                  GpuTex t, int face)
{
    (void)d;
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                           GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, t, 0);
}

int gpu_fbo_complete(const GpuDevice *d)
{
    (void)d;
    return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
}

int gpu_fbo_current(const GpuDevice *d)
{
    (void)d;
    GLint id = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &id);
    return (int)id;
}

// --- Чтение обратно ---
void gpu_read_rgb(const GpuDevice *d, int x, int y, int w, int h, void *out)
{
    (void)d;
    glReadPixels(x, y, w, h, GL_RGB, GL_UNSIGNED_BYTE, out);
}

// --- Программы/шейдеры ---
GpuProg gpu_program_new(const GpuDevice *d, const char *vs_path, const char *fs_path)
{
    (void)d;
    return make_program(vs_path, fs_path);
}

void gpu_program_delete(const GpuDevice *d, GpuProg *prog)
{
    (void)d;
    if (!prog || !*prog) return;
    resources_program_release(*prog);
    *prog = 0;
}

void gpu_use_program(const GpuDevice *d, GpuProg prog)
{
    (void)d;
    glUseProgram(prog);
}

GpuLoc gpu_program_loc(const GpuDevice *d, GpuProg prog, const char *name)
{
    (void)d;
    return (GpuLoc)glGetUniformLocation(prog, name);
}

// --- Вершинные массивы и буферы ---
GpuVao gpu_vao_gen(const GpuDevice *d)
{
    (void)d;
    GLuint id = 0;
    glGenVertexArrays(1, &id);
    return id;
}

void gpu_vao_delete(const GpuDevice *d, GpuVao *vao)
{
    (void)d;
    if (!vao || !*vao) return;
    glDeleteVertexArrays(1, vao);
    *vao = 0;
}

void gpu_vao_bind(const GpuDevice *d, GpuVao vao)
{
    (void)d;
    glBindVertexArray(vao);
}

GpuBuf gpu_buf_gen(const GpuDevice *d)
{
    (void)d;
    GLuint id = 0;
    glGenBuffers(1, &id);
    return id;
}

void gpu_buf_delete(const GpuDevice *d, GpuBuf *buf)
{
    (void)d;
    if (!buf || !*buf) return;
    glDeleteBuffers(1, buf);
    *buf = 0;
}

void gpu_buf_bind_array(const GpuDevice *d, GpuBuf buf)
{
    (void)d;
    glBindBuffer(GL_ARRAY_BUFFER, buf);
}

void gpu_buf_upload_array(const GpuDevice *d, const void *data,
                          unsigned int bytes, int stream)
{
    (void)d;
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)bytes, data,
                 stream ? GL_STREAM_DRAW : GL_STATIC_DRAW);
}

void gpu_vertex_attr_3f(const GpuDevice *d, GpuLoc loc,
                        int stride_bytes, const void *offset)
{
    (void)d;
    glVertexAttribPointer(loc, 3, GL_FLOAT, GL_FALSE, stride_bytes, offset);
    glEnableVertexAttribArray(loc);
}

// --- Юниформы ---
void gpu_uniform_mat4(const GpuDevice *d, GpuLoc loc, const float *m)
{
    (void)d;
    glUniformMatrix4fv(loc, 1, GL_FALSE, m);
}

void gpu_uniform_float3(const GpuDevice *d, GpuLoc loc, float x, float y, float z)
{
    (void)d;
    glUniform3f(loc, x, y, z);
}

// --- Рисование ---
void gpu_draw_arrays(const GpuDevice *d, GpuPrim prim, int vertex_count)
{
    (void)d;
    glDrawArrays(prim == GPU_PRIM_LINES ? GL_LINES : GL_TRIANGLES, 0, vertex_count);
}