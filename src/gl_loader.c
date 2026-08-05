// ============================================================================
//  gl_loader.c — загрузка указателей функций OpenGL через SDL_GL_GetProcAddress
// ============================================================================
#include "gl_loader.h"

#include <SDL3/SDL.h>

// Определения указателей (в заголовке они объявлены extern).
#define F(ret, name, args) ret (APIENTRY *p_##name) args;
GL_FUNCS(F)
#undef F

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
    p_glDrawElementsInstancedBaseInstance(mode, count, type, indices,
                                          primcount, baseinstance);
}
