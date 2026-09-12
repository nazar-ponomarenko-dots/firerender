// ============================================================================
//  gl_loader.c — загрузка указателей функций OpenGL через SDL_GL_GetProcAddress
// ============================================================================
#include "gl_loader.h"

#include <SDL3/SDL.h>

// Определения указателей (в заголовке они объявлены extern).
#define F(ret, name, args) ret (APIENTRY *p_##name) args;
GL_FUNCS(F)
#undef F

// OPTIONAL GPU-driven указатели (могут остаться NULL на старых контекстах).
#define F(ret, name, args) ret (APIENTRY *p_##name) args;
GL_FUNCS_OPT(F)
#undef F

int g_gl_gpu_driven = 0;

int load_gl_funcs(void)
{
#define F(ret, name, args)                                                    \
    p_##name = (ret (APIENTRY *) args)SDL_GL_GetProcAddress(#name);           \
    if (!p_##name) {                                                          \
        SDL_Log("GL function not found: %s", #name);                          \
        return -1;                                                            \
    }
    GL_FUNCS(F)
#undef F

    // OPTIONAL GPU-driven функции: загружаем без фатальности. Если любая из
    // них отсутствует (контекст < GL 4.3) — оставляем NULL и выключаем
    // gpu-driven путь (фолбэк на инстансный рендер в vegetation.c).
#define F(ret, name, args)                                                    \
    p_##name = (ret (APIENTRY *) args)SDL_GL_GetProcAddress(#name);
    GL_FUNCS_OPT(F)
#undef F
    GLint major = 0, minor = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);
#ifdef USE_GLES_PATH
    int compute_version = major > 3 || (major == 3 && minor >= 1);
#else
    int compute_version = major > 4 || (major == 4 && minor >= 3);
#endif
    g_gl_gpu_driven = (compute_version && p_glDispatchCompute && p_glMemoryBarrier &&
                       p_glBindBufferBase && p_glBindBufferRange &&
                       p_glShaderStorageBlockBinding && p_glDrawElementsIndirect)
                      ? 1 : 0;
    return 0;
}

// --- Счётчики профилирования ----------------------------------------------------
int g_prof_draws = 0;
int g_prof_verts = 0;
int g_prof_tris = 0;

// Треугольники в вызове (по режиму примитива)
static int prim_tris(GLenum mode, GLsizei count)
{
    switch (mode) {
    case GL_TRIANGLES:       return (int)(count / 3);
    case GL_TRIANGLE_STRIP:
    case GL_TRIANGLE_FAN:    return count > 2 ? (int)count - 2 : 0;
    default:                 return 0;
    }
}

void fr_glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
    g_prof_draws++;
    g_prof_verts += count;
    g_prof_tris += prim_tris(mode, count);
    p_glDrawArrays(mode, first, count);
}

void fr_glDrawArraysInstanced(GLenum mode, GLint first, GLsizei count,
                              GLsizei primcount)
{
    g_prof_draws++;
    g_prof_verts += count * primcount;
    g_prof_tris += prim_tris(mode, count) * primcount;
    p_glDrawArraysInstanced(mode, first, count, primcount);
}

void fr_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices)
{
    g_prof_draws++;
    g_prof_verts += count;
    g_prof_tris += prim_tris(mode, count);
    p_glDrawElements(mode, count, type, indices);
}

void fr_glDrawElementsInstanced(GLenum mode, GLsizei count, GLenum type,
                                const void *indices, GLsizei primcount)
{
    g_prof_draws++;
    g_prof_verts += count * primcount;
    g_prof_tris += prim_tris(mode, count) * primcount;
    p_glDrawElementsInstanced(mode, count, type, indices, primcount);
}

void fr_glDrawElementsInstancedBaseInstance(GLenum mode, GLsizei count,
                                            GLenum type, const void *indices,
                                            GLsizei primcount, GLuint baseinstance)
{
    g_prof_draws++;
    g_prof_verts += count * primcount;
    g_prof_tris += prim_tris(mode, count) * primcount;
    if (p_glDrawElementsInstancedBaseInstance)
        p_glDrawElementsInstancedBaseInstance(mode, count, type, indices,
                                              primcount, baseinstance);
    else if (baseinstance == 0 && p_glDrawElementsInstanced)
        p_glDrawElementsInstanced(mode, count, type, indices, primcount);
    else
        SDL_Log("glDrawElementsInstancedBaseInstance not available (base=%u)", baseinstance);
}
