// ============================================================================
//  srender_gl.c — бэкенд SIMPLE RENDER на OpenGL / OpenGL ES
// ----------------------------------------------------------------------------
//  Реализация sr_* (srender.h) поверх gl_loader.h. Один файл знает про GL —
//  весь остальной движок видит только srender.h. Работает и на desktop GL
//  (3.3+), и на GLES 3.x: версии GLSL и точности подставляет shader.c,
//  отсутствующие фичи (compute/indirect/baseinstance/queries/MSAA) честно
//  отражаются в SrCaps, вызывающие обязаны их уважать.
//
//  Draw-вызовы идут через обёртки fr_glDraw* (макросы gl_loader.h) — счётчики
//  профайлера продолжают работать. Новые бэкенды (Vulkan/Metal) реализуют тот
//  же sr_* и подсчитывают статистику сами.
// ============================================================================
#include "srender.h"

#include "gl_loader.h"
#include "resources.h"
#include "shader.h"

#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>

struct SrDevice {
    SrCaps caps;
};

static int has_ext(const char *exts, const char *needle)
{
    if (!exts || !needle || !*needle) return 0;
    size_t n = strlen(needle);
    for (const char *p = exts; (p = strstr(p, needle)) != NULL; p += n) {
        if ((p == exts || p[-1] == ' ') && (p[n] == 0 || p[n] == ' ')) return 1;
    }
    return 0;
}

static int probe_rgba16f_target(void)
{
    GLuint tex = 0, fbo = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, 2, 2, 0,
                 GL_RGBA, GL_HALF_FLOAT, NULL);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, tex, 0);
    int ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &tex);
    while (glGetError() != GL_NO_ERROR) {}
    return ok;
}

// --- Форматы -----------------------------------------------------------------
struct FmtMap { GLint internal; GLenum pix; GLenum type; };
static const struct FmtMap *fmt_map(SrFormat f)
{
    static const struct FmtMap m[] = {
        [SR_FMT_RGBA8]    = { GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE },
        [SR_FMT_RGB8]     = { GL_RGB8,  GL_RGB,  GL_UNSIGNED_BYTE },
        [SR_FMT_R8]       = { GL_R8,    GL_RED,  GL_UNSIGNED_BYTE },
        [SR_FMT_R16]      = { GL_R16,   GL_RED,  GL_UNSIGNED_SHORT },
        [SR_FMT_RG8]      = { GL_RG8,   GL_RG,   GL_UNSIGNED_BYTE },
        [SR_FMT_RGBA16F]  = { GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT },
        [SR_FMT_DEPTH24]  = { GL_DEPTH_COMPONENT24, GL_DEPTH_COMPONENT,
                              GL_UNSIGNED_INT },
    };
    return &m[f];
}

// Раскладка пикселей при загрузке с CPU может отличаться от внутреннего формата.
static void pix_map(SrPixFormat p, GLenum *fmt, GLenum *type)
{
    switch (p) {
        case SR_PIX_RGBA: *fmt = GL_RGBA;  *type = GL_UNSIGNED_BYTE; break;
        case SR_PIX_RGB:  *fmt = GL_RGB;   *type = GL_UNSIGNED_BYTE; break;
        case SR_PIX_RG:   *fmt = GL_RG;    *type = GL_UNSIGNED_BYTE; break;
        case SR_PIX_RED:  *fmt = GL_RED;   *type = GL_UNSIGNED_BYTE; break;
        case SR_PIX_DEPTH_UINT: *fmt = GL_DEPTH_COMPONENT;
                                *type = GL_UNSIGNED_INT; break;
        default: *fmt = GL_RGBA; *type = GL_UNSIGNED_BYTE; break;
    }
}

static GLenum tex_target(SrTexTarget t)
{
    switch (t) {
        case SR_TEX_3D:   return GL_TEXTURE_3D;
        case SR_TEX_CUBE: return GL_TEXTURE_CUBE_MAP;
        default:          return GL_TEXTURE_2D;
    }
}

static GLenum buf_target(SrBufTarget t)
{
    switch (t) {
        case SR_BUF_ELEMENT:   return GL_ELEMENT_ARRAY_BUFFER;
        case SR_BUF_UNIFORM:   return GL_UNIFORM_BUFFER;
        case SR_BUF_SSBO:      return GL_SHADER_STORAGE_BUFFER;
        case SR_BUF_INDIRECT:  return GL_DRAW_INDIRECT_BUFFER;
        default:               return GL_ARRAY_BUFFER;
    }
}

static GLenum prim_mode(SrPrim p)
{
    switch (p) {
        case SR_PRIM_LINES:          return GL_LINES;
        case SR_PRIM_TRIANGLE_STRIP: return GL_TRIANGLE_STRIP;
        case SR_PRIM_POINTS:         return GL_POINTS;
        default:                     return GL_TRIANGLES;
    }
}

static GLint filter_gl(SrFilter f)
{
    switch (f) {
        case SR_FILTER_NEAREST:                 return GL_NEAREST;
        case SR_FILTER_LINEAR:                  return GL_LINEAR;
        case SR_FILTER_NEAREST_MIPMAP_NEAREST:  return GL_NEAREST_MIPMAP_NEAREST;
        case SR_FILTER_LINEAR_MIPMAP_NEAREST:   return GL_LINEAR_MIPMAP_NEAREST;
        default:                                return GL_LINEAR_MIPMAP_LINEAR;
    }
}

static GLint wrap_gl(SrWrap w)
{
    switch (w) {
        case SR_WRAP_CLAMP:   return GL_CLAMP_TO_EDGE;
        case SR_WRAP_MIRROR:  return GL_MIRRORED_REPEAT;
        default:              return GL_REPEAT;
    }
}

// --- Жизненный цикл ------------------------------------------------------------
SrDevice *sr_create(void)
{
    SrDevice *d = calloc(1, sizeof *d);
    SrCaps *c = &d->caps;
    const GLubyte *s;
    GLint major = 3, minor = 0;

    static char ver[128];
    s = glGetString(GL_VERSION);
    if (s) { strncpy(ver, (const char *)s, sizeof ver - 1); c->version = ver; }
#ifdef USE_GLES_PATH
    c->gles = 1;
#endif
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);
    if (glGetError() == GL_NO_ERROR) { c->gl_major = major; c->gl_minor = minor; }
    else { c->gl_major = c->gles ? 2 : 1; }   // старые контексты не дают версии

    const char *exts = (const char *)glGetString(GL_EXTENSIONS);
    int compute_version = c->gles ? (c->gl_major > 3 ||
                          (c->gl_major == 3 && c->gl_minor >= 1))
                        : (c->gl_major > 4 ||
                          (c->gl_major == 4 && c->gl_minor >= 3));
    c->compute       = compute_version && p_glDispatchCompute && p_glMemoryBarrier &&
                        p_glBindBufferBase;
    c->indirect      = compute_version && p_glDrawElementsIndirect;
    c->base_instance = !c->gles && p_glDrawElementsInstancedBaseInstance;
    c->timer_query   = !c->gles && p_glGenQueries && p_glBeginQuery &&
                        p_glGetQueryObjectiv && p_glGetQueryObjectui64v;
    c->tex3d         = p_glTexImage3D ? 1 : 0;
    c->color_half_float = probe_rgba16f_target();
    c->texture_norm16 = !c->gles || has_ext(exts, "GL_EXT_texture_norm16");

    GLint ms = 0, ts = 0;
    glGetIntegerv(GL_MAX_SAMPLES, &ms);
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &ts);
    if (glGetError() != GL_NO_ERROR) { ms = 0; ts = 0; }
    c->msaa_max = ms;
    c->max_tex_size = ts;

    s = glGetString(GL_VENDOR);   if (s) c->vendor   = (const char *)s;
    s = glGetString(GL_RENDERER); if (s) c->renderer = (const char *)s;
    SDL_Log("Render backend: %s %d.%d, %s | half-float RT=%d norm16=%d compute=%d indirect=%d",
            c->gles ? "OpenGL ES" : "OpenGL", c->gl_major, c->gl_minor,
            c->renderer ? c->renderer : "unknown", c->color_half_float,
            c->texture_norm16, c->compute, c->indirect);
    return d;
}

void sr_destroy(SrDevice *d)
{
    free(d);
}

const SrCaps *sr_caps(const SrDevice *d)
{
    return d ? &d->caps : NULL;
}

// --- Кадр -------------------------------------------------------------------------
void sr_bind_fbo(SrDevice *d, SrFbo fbo)
{
    (void)d;
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
}

void sr_viewport(SrDevice *d, int x, int y, int w, int h)
{
    (void)d;
    glViewport(x, y, w, h);
}

void sr_clear(SrDevice *d, float r, float g, float b, float a,
              int clear_color, int clear_depth)
{
    (void)d;
    GLbitfield mask = 0;
    if (clear_color) {
        glClearColor(r, g, b, a);
        mask |= GL_COLOR_BUFFER_BIT;
    }
    if (clear_depth) {
        glDepthMask(GL_TRUE);
        mask |= GL_DEPTH_BUFFER_BIT;
    }
    if (mask) glClear(mask);
}

int sr_fbo_current(SrDevice *d)
{
    (void)d;
    GLint id = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &id);
    return (int)id;
}

int sr_fbo_complete(SrDevice *d)
{
    (void)d;
    return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
}

// --- Состояние ------------------------------------------------------------------------
void sr_cap(SrDevice *d, SrStateCap cap, int on)
{
    (void)d;
    GLenum glcap;
    switch (cap) {
        case SR_CAP_DEPTH_TEST: glcap = GL_DEPTH_TEST; break;
        case SR_CAP_CULL_FACE:  glcap = GL_CULL_FACE;  break;
        case SR_CAP_BLEND:      glcap = GL_BLEND;      break;
        default: return;
    }
    if (on) glEnable(glcap); else glDisable(glcap);
}

void sr_depth_mask(SrDevice *d, int write_enabled)
{
    (void)d;
    glDepthMask(write_enabled ? GL_TRUE : GL_FALSE);
}

void sr_blend_func(SrDevice *d, SrBlendFac src, SrBlendFac dst)
{
    (void)d;
    static const GLenum m[] = {
        [SR_BLEND_ZERO] = GL_ZERO,
        [SR_BLEND_ONE] = GL_ONE,
        [SR_BLEND_SRC_ALPHA] = GL_SRC_ALPHA,
        [SR_BLEND_ONE_MINUS_SRC_ALPHA] = GL_ONE_MINUS_SRC_ALPHA,
    };
    glBlendFunc(m[src], m[dst]);
}

void sr_cull_front(SrDevice *d, int front_ccw_is_front)
{
    (void)d;
    glFrontFace(front_ccw_is_front ? GL_CCW : GL_CW);
}

void sr_wireframe(SrDevice *d, int on)
{
    (void)d;
#ifndef USE_GLES_PATH
    if (p_glPolygonMode)
        glPolygonMode(GL_FRONT_AND_BACK, on ? GL_LINE : GL_FILL);
#endif
}

// --- Текстуры ---------------------------------------------------------------------------
SrTex sr_tex_gen(SrDevice *d)
{
    (void)d;
    GLuint id = 0;
    glGenTextures(1, &id);
    return id;
}

void sr_tex_delete(SrDevice *d, SrTex *t)
{
    (void)d;
    if (!t || !*t) return;
    glDeleteTextures(1, t);
    *t = 0;
}

void sr_tex_active(SrDevice *d, int unit)
{
    (void)d;
    glActiveTexture((GLenum)(GL_TEXTURE0 + unit));
}

void sr_tex_bind(SrDevice *d, SrTexTarget target, SrTex t)
{
    (void)d;
    glBindTexture(tex_target(target), t);
}

void sr_tex_image_2d(SrDevice *d, SrTex t, int face, SrFormat fmt,
                     int w, int h, SrPixFormat pix, const void *data)
{
    (void)d;
    const struct FmtMap *f = fmt_map(fmt);
    GLenum pf, pt;
    pix_map(pix, &pf, &pt);
    GLenum target = (face >= 0)
        ? (GLenum)(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face)
        : GL_TEXTURE_2D;
    glBindTexture(face >= 0 ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D, t);
    glTexImage2D(target, 0, f->internal, w, h, 0, pf, pt, data);
}

void sr_tex_image_3d(SrDevice *d, SrTex t, SrFormat fmt,
                     int w, int h, int depth, SrPixFormat pix,
                     const void *data)
{
    (void)d;
    if (!p_glTexImage3D) return;
    const struct FmtMap *f = fmt_map(fmt);
    GLenum pf, pt;
    pix_map(pix, &pf, &pt);
    glBindTexture(GL_TEXTURE_3D, t);
    glTexImage3D(GL_TEXTURE_3D, 0, f->internal, w, h, depth, 0, pf, pt, data);
}

void sr_tex_sub_image_3d(SrDevice *d, SrTex t, int x, int y, int z,
                         int w, int h, int depth, SrPixFormat pix,
                         const void *data)
{
    (void)d;
    if (!p_glTexSubImage3D) return;
    GLenum pf, pt;
    pix_map(pix, &pf, &pt);
    glBindTexture(GL_TEXTURE_3D, t);
    glTexSubImage3D(GL_TEXTURE_3D, 0, x, y, z, w, h, depth, pf, pt, data);
}

void sr_tex_params(SrDevice *d, SrTex t, SrTexTarget target,
                   SrFilter min_f, SrFilter mag_f,
                   SrWrap wrap_s, SrWrap wrap_t, SrWrap wrap_r)
{
    (void)d;
    GLenum tt = tex_target(target);
    glBindTexture(tt, t);
    glTexParameteri(tt, GL_TEXTURE_MIN_FILTER, filter_gl(min_f));
    glTexParameteri(tt, GL_TEXTURE_MAG_FILTER, filter_gl(mag_f));
    glTexParameteri(tt, GL_TEXTURE_WRAP_S, wrap_gl(wrap_s));
    glTexParameteri(tt, GL_TEXTURE_WRAP_T, wrap_gl(wrap_t));
    if (target != SR_TEX_2D)
        glTexParameteri(tt, GL_TEXTURE_WRAP_R, wrap_gl(wrap_r));
}

void sr_tex_generate_mipmap(SrDevice *d, SrTex t, SrTexTarget target)
{
    (void)d;
    glBindTexture(tex_target(target), t);
    glGenerateMipmap(tex_target(target));
}

// --- Рендербуферы и FBO --------------------------------------------------------------------
SrRb sr_rb_gen(SrDevice *d)
{
    (void)d;
    GLuint id = 0;
    glGenRenderbuffers(1, &id);
    return id;
}

void sr_rb_delete(SrDevice *d, SrRb *rb)
{
    (void)d;
    if (!rb || !*rb) return;
    glDeleteRenderbuffers(1, rb);
    *rb = 0;
}

void sr_rb_storage(SrDevice *d, SrRb rb, SrFormat fmt, int samples,
                   int w, int h)
{
    (void)d;
    glBindRenderbuffer(GL_RENDERBUFFER, rb);
    if (samples > 1)
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples,
                                         fmt_map(fmt)->internal, w, h);
    else
        glRenderbufferStorage(GL_RENDERBUFFER, fmt_map(fmt)->internal, w, h);
}

SrFbo sr_fbo_gen(SrDevice *d)
{
    (void)d;
    GLuint id = 0;
    glGenFramebuffers(1, &id);
    return id;
}

void sr_fbo_delete(SrDevice *d, SrFbo *fbo)
{
    (void)d;
    if (!fbo || !*fbo) return;
    glDeleteFramebuffers(1, fbo);
    *fbo = 0;
}

// Прицепить текстуру к точке: attachment 0 = color0, -1 = depth.
void sr_fbo_attach_tex(SrDevice *d, SrFbo fbo, int attachment,
                       SrTexTarget target, SrTex t, int face)
{
    (void)d;
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    GLenum point = (attachment < 0)
        ? GL_DEPTH_ATTACHMENT
        : (GLenum)(GL_COLOR_ATTACHMENT0 + attachment);
    if (target == SR_TEX_CUBE) {
        // GL требует конкретную грань кубомапы (face 0..5).
        GLenum gtex = (face >= 0)
            ? (GLenum)(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face)
            : GL_TEXTURE_CUBE_MAP;
        glFramebufferTexture2D(GL_FRAMEBUFFER, point, gtex, t, 0);
    } else {
        glFramebufferTexture2D(GL_FRAMEBUFFER, point, tex_target(target), t, 0);
    }
}

void sr_fbo_attach_rb(SrDevice *d, SrFbo fbo, int attachment, SrRb rb)
{
    (void)d;
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    GLenum point = (attachment < 0)
        ? GL_DEPTH_ATTACHMENT
        : (GLenum)(GL_COLOR_ATTACHMENT0 + attachment);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, point, GL_RENDERBUFFER, rb);
}

void sr_blit(SrDevice *d, SrFbo src, SrFbo dst,
             int w, int h, int linear, int copy_depth)
{
    (void)d;
    GLbitfield bits = copy_depth ? GL_DEPTH_BUFFER_BIT : GL_COLOR_BUFFER_BIT;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, src);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dst);
    glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, bits,
                      linear ? GL_LINEAR : GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, dst);
}

// --- Шейдерные программы -----------------------------------------------------------------------
SrProg sr_prog_new(SrDevice *d, const char *vs_path, const char *fs_path)
{
    (void)d;
    return make_program(vs_path, fs_path);
}

SrProg sr_prog_compute_new(SrDevice *d, const char *cs_path)
{
    (void)d;
    return make_compute_program(cs_path);
}

void sr_prog_delete(SrDevice *d, SrProg *prog)
{
    (void)d;
    if (!prog || !*prog) return;
    resources_program_release(*prog);
    *prog = 0;
}

void sr_prog_use(SrDevice *d, SrProg prog)
{
    (void)d;
    glUseProgram(prog);
}

SrLoc sr_prog_loc(SrDevice *d, SrProg prog, const char *name)
{
    (void)d;
    return (SrLoc)glGetUniformLocation(prog, name);
}

// --- Юниформы -------------------------------------------------------------------------------------
void sr_uniform_int(SrDevice *d, SrLoc loc, int v)
{ (void)d; glUniform1i(loc, v); }

void sr_uniform_float(SrDevice *d, SrLoc loc, float v)
{ (void)d; glUniform1f(loc, v); }

void sr_uniform_float2(SrDevice *d, SrLoc loc, float x, float y)
{ (void)d; glUniform2f(loc, x, y); }

void sr_uniform_float3(SrDevice *d, SrLoc loc, float x, float y, float z)
{ (void)d; glUniform3f(loc, x, y, z); }

void sr_uniform_float4(SrDevice *d, SrLoc loc, float x, float y, float z, float w)
{ (void)d; glUniform4f(loc, x, y, z, w); }

void sr_uniform_float1v(SrDevice *d, SrLoc loc, int n, const float *v)
{ (void)d; glUniform1fv(loc, n, v); }

void sr_uniform_float3v(SrDevice *d, SrLoc loc, int n, const float *v)
{ (void)d; glUniform3fv(loc, n, v); }

void sr_uniform_float4v(SrDevice *d, SrLoc loc, int n, const float *v)
{ (void)d; glUniform4fv(loc, n, v); }

void sr_uniform_mat4(SrDevice *d, SrLoc loc, const float *m)
{ (void)d; glUniformMatrix4fv(loc, 1, GL_FALSE, m); }

// --- Буферы и атрибуты ------------------------------------------------------------------------------
SrBuf sr_buf_gen(SrDevice *d)
{
    (void)d;
    GLuint id = 0;
    glGenBuffers(1, &id);
    return id;
}

void sr_buf_delete(SrDevice *d, SrBuf *buf)
{
    (void)d;
    if (!buf || !*buf) return;
    glDeleteBuffers(1, buf);
    *buf = 0;
}

void sr_buf_bind(SrDevice *d, SrBufTarget target, SrBuf buf)
{
    (void)d;
    glBindBuffer(buf_target(target), buf);
}

void sr_buf_upload(SrDevice *d, SrBufTarget target,
                   const void *data, unsigned int bytes, int static_use)
{
    (void)d;
    glBufferData(buf_target(target), (GLsizeiptr)bytes, data,
                 static_use ? GL_STATIC_DRAW : GL_STREAM_DRAW);
}

void sr_buf_sub_upload(SrDevice *d, SrBufTarget target, unsigned int offset,
                       const void *data, unsigned int bytes)
{
    (void)d;
    glBufferSubData(buf_target(target), (GLintptr)offset,
                    (GLsizeiptr)bytes, data);
}

void sr_buf_bind_ssbo(SrDevice *d, SrBuf buf, int binding)
{
    (void)d;
    if (!p_glBindBufferBase) return;
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, binding, buf);
}

SrVao sr_vao_gen(SrDevice *d)
{
    (void)d;
    GLuint id = 0;
    glGenVertexArrays(1, &id);
    return id;
}

void sr_vao_delete(SrDevice *d, SrVao *vao)
{
    (void)d;
    if (!vao || !*vao) return;
    glDeleteVertexArrays(1, vao);
    *vao = 0;
}

void sr_vao_bind(SrDevice *d, SrVao vao)
{
    (void)d;
    glBindVertexArray(vao);
}

void sr_attrib_float(SrDevice *d, SrLoc loc, int comps, int stride_bytes,
                     unsigned int offset, int divisor)
{
    (void)d;
    glVertexAttribPointer(loc, comps, GL_FLOAT, GL_FALSE,
                          stride_bytes, (const void *)(size_t)offset);
    glEnableVertexAttribArray(loc);
    if (divisor > 0) glVertexAttribDivisor(loc, (GLuint)divisor);
}

// --- Draw ----------------------------------------------------------------------------------------------
void sr_draw_arrays(SrDevice *d, SrPrim prim, int first, int count)
{
    (void)d;
    glDrawArrays(prim_mode(prim), first, count);
}

void sr_draw_elements(SrDevice *d, SrPrim prim, int count, int indices_u32,
                      unsigned int byte_offset)
{
    (void)d;
    glDrawElements(prim_mode(prim), count,
                   indices_u32 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT,
                   (const void *)(size_t)byte_offset);
}

void sr_draw_arrays_instanced(SrDevice *d, SrPrim prim, int first, int count,
                              int instances)
{
    (void)d;
    glDrawArraysInstanced(prim_mode(prim), first, count, instances);
}

void sr_draw_elements_instanced(SrDevice *d, SrPrim prim, int count,
                                int indices_u32, unsigned int byte_offset,
                                int instances)
{
    (void)d;
    glDrawElementsInstanced(prim_mode(prim), count,
                            indices_u32 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT,
                            (const void *)(size_t)byte_offset, instances);
}

void sr_draw_elements_instanced_base(SrDevice *d, SrPrim prim, int count,
                                     int indices_u32, unsigned int byte_offset,
                                     int instances, unsigned int base)
{
    (void)d;
    if (base == 0) {
        glDrawElementsInstanced(prim_mode(prim), count,
                                indices_u32 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT,
                                (const void *)(size_t)byte_offset, instances);
        return;
    }
    if (!p_glDrawElementsInstancedBaseInstance) {
        // caps->base_instance == 0: вызывающий не должен сюда попадать.
        static int warned = 0;
        if (!warned) {
            SDL_Log("sr: base_instance недоступен — draw пропущен");
            warned = 1;
        }
        return;
    }
    glDrawElementsInstancedBaseInstance(prim_mode(prim), count,
                                        indices_u32 ? GL_UNSIGNED_INT
                                                    : GL_UNSIGNED_SHORT,
                                        (const void *)(size_t)byte_offset,
                                        instances, base);
}

void sr_draw_elements_indirect(SrDevice *d, SrPrim prim,
                               SrBuf cmd_buf, unsigned int byte_offset)
{
    (void)d;
    if (!p_glDrawElementsIndirect) return;
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, cmd_buf);
    glDrawElementsIndirect(prim_mode(prim), GL_UNSIGNED_INT,
                           (const void *)(size_t)byte_offset);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
}

// --- Compute ---------------------------------------------------------------------------------------------
void sr_dispatch(SrDevice *d, int x, int y, int z)
{
    (void)d;
    if (p_glDispatchCompute) glDispatchCompute((GLuint)x, (GLuint)y, (GLuint)z);
}

void sr_barrier(SrDevice *d, unsigned int barrier_bits)
{
    (void)d;
    if (!p_glMemoryBarrier) return;
    GLbitfield bits = 0;
    if (barrier_bits & SR_BARRIER_SSBO)    bits |= GL_SHADER_STORAGE_BARRIER_BIT;
    if (barrier_bits & SR_BARRIER_VERTEX)  bits |= GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT;
    if (barrier_bits & SR_BARRIER_INDEX)   bits |= GL_ELEMENT_ARRAY_BARRIER_BIT;
    if (barrier_bits & SR_BARRIER_UNIFORM) bits |= GL_UNIFORM_BARRIER_BIT;
    if (barrier_bits & SR_BARRIER_COMMAND) bits |= GL_COMMAND_BARRIER_BIT;
    if (barrier_bits & SR_BARRIER_TEXTURE) bits |= GL_TEXTURE_FETCH_BARRIER_BIT;
    if (barrier_bits & SR_BARRIER_IMAGE)   bits |= GL_SHADER_IMAGE_ACCESS_BARRIER_BIT;
    glMemoryBarrier(bits);
}

// --- GPU-таймеры --------------------------------------------------------------------------------------------
SrQuery sr_query_gen(SrDevice *d)
{
    (void)d;
    if (!p_glGenQueries) return 0;
    GLuint id = 0;
    glGenQueries(1, &id);
    return id;
}

void sr_query_delete(SrDevice *d, SrQuery *q)
{
    (void)d;
    if (!q || !*q || !p_glDeleteQueries) return;
    glDeleteQueries(1, q);
    *q = 0;
}

void sr_query_begin(SrDevice *d, SrQuery q)
{
    (void)d;
    if (q && p_glBeginQuery) glBeginQuery(GL_TIME_ELAPSED, q);
}

void sr_query_end(SrDevice *d)
{
    (void)d;
    if (p_glEndQuery) glEndQuery(GL_TIME_ELAPSED);
}

float sr_query_result_ms(SrDevice *d, SrQuery q)
{
    (void)d;
    if (!q || !p_glGetQueryObjectui64v || !p_glGetQueryObjectiv) return 0.0f;
    GLint ready = 0;
    glGetQueryObjectiv(q, 0x8867 /*GL_QUERY_RESULT_AVAILABLE*/, &ready);
    if (!ready) return 0.0f;
    GLuint64 v = 0;
    glGetQueryObjectui64v(q, 0x8866 /*GL_QUERY_RESULT*/, &v);
    return (float)((double)v / 1000000.0);
}

// --- Чтение обратно --------------------------------------------------------------------------------------------
void sr_read_pixels(SrDevice *d, int x, int y, int w, int h,
                    SrPixFormat pix, void *out)
{
    (void)d;
    GLenum pf, pt;
    pix_map(pix, &pf, &pt);
    glReadPixels(x, y, w, h, pf, pt, out);
}
