// ============================================================================
//  gl_loader.c — загрузка указателей функций OpenGL через SDL_GL_GetProcAddress
// ============================================================================
#include "gl_loader.h"

#include <SDL3/SDL.h>
#include <stdlib.h>

// Определения указателей (в заголовке они объявлены extern).
#define F(ret, name, args) ret (APIENTRY *p_##name) args;
GL_FUNCS(F)
#undef F

// OPTIONAL GPU-driven указатели (могут остаться NULL на старых контекстах).
#define F(ret, name, args) ret (APIENTRY *p_##name) args;
GL_FUNCS_OPT(F)
#undef F

int g_gl_gpu_driven = 0;

// --- Кэш GL-состояния (отсев избыточных bind/use) ---------------------------
//  Единственная воронка: все модули зовут glXxx-макросы, которые ведут сюда.
//  Тень хранит, что реально привязано, и не шлёт драйверу повтор. Значение
//  GLC_UNKNOWN = "неизвестно" (следующий вызов пройдёт обязательно).
#define GLC_MAX_UNITS 16
#define GLC_TARGETS   4                 // 2D, 3D, CUBE, 2D_ARRAY
#define GLC_UNKNOWN   0xFFFFFFFFu

int g_prof_state_skipped = 0;
static int    s_glcache_on = 1;
static GLuint s_cur_prog   = GLC_UNKNOWN;
static GLuint s_cur_vao    = GLC_UNKNOWN;
static int    s_cur_unit   = 0;         // активный тексюнит (по спеке GL — 0)
static GLuint s_tex[GLC_MAX_UNITS][GLC_TARGETS];

static int glc_target_idx(GLenum target)
{
    switch (target) {
        case GL_TEXTURE_2D:       return 0;
        case GL_TEXTURE_3D:       return 1;
        case GL_TEXTURE_CUBE_MAP: return 2;
#ifdef GL_TEXTURE_2D_ARRAY
        case GL_TEXTURE_2D_ARRAY: return 3;
#endif
        default:                  return -1;   // прочие цели — без кэша
    }
}

static void glc_reset(void)
{
    const char *e = getenv("FR_GLCACHE");
    s_glcache_on = !(e && e[0] == '0');
    s_cur_prog = GLC_UNKNOWN;
    s_cur_vao  = GLC_UNKNOWN;
    s_cur_unit = 0;
    for (int u = 0; u < GLC_MAX_UNITS; u++)
        for (int t = 0; t < GLC_TARGETS; t++) s_tex[u][t] = GLC_UNKNOWN;
}

int load_gl_funcs(void)
{
    glc_reset();
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

// --- Обёртки отсева состояния -----------------------------------------------
void fr_glUseProgram(GLuint prog)
{
    if (s_glcache_on && prog == s_cur_prog) { g_prof_state_skipped++; return; }
    s_cur_prog = prog;
    p_glUseProgram(prog);
}

void fr_glBindVertexArray(GLuint vao)
{
    if (s_glcache_on && vao == s_cur_vao) { g_prof_state_skipped++; return; }
    s_cur_vao = vao;
    p_glBindVertexArray(vao);
}

void fr_glActiveTexture(GLenum texture)
{
    int unit = (int)(texture - GL_TEXTURE0);
    if (s_glcache_on && unit >= 0 && unit < GLC_MAX_UNITS && unit == s_cur_unit) {
        g_prof_state_skipped++;
        return;
    }
    // Юнит вне отслеживаемого диапазона — помечаем как «нетрекнутый»,
    // тогда fr_glBindTexture для него не кэширует (всегда шлёт).
    s_cur_unit = (unit >= 0 && unit < GLC_MAX_UNITS) ? unit : -1;
    p_glActiveTexture(texture);
}

void fr_glBindTexture(GLenum target, GLuint tex)
{
    int ti = glc_target_idx(target);
    if (s_glcache_on && ti >= 0 && s_cur_unit >= 0 && s_cur_unit < GLC_MAX_UNITS) {
        if (s_tex[s_cur_unit][ti] == tex) { g_prof_state_skipped++; return; }
        s_tex[s_cur_unit][ti] = tex;
    }
    p_glBindTexture(target, tex);
}

void fr_glDeleteTextures(GLsizei n, const GLuint *textures)
{
    // Удаляемая текстура могла быть привязана к любому юниту/цели — сбрасываем
    // весь теневой кэш привязок, чтобы переиспользование id не дало ложный скип.
    for (int u = 0; u < GLC_MAX_UNITS; u++)
        for (int t = 0; t < GLC_TARGETS; t++) s_tex[u][t] = GLC_UNKNOWN;
    p_glDeleteTextures(n, textures);
}

void fr_glDeleteProgram(GLuint prog)
{
    if (prog == s_cur_prog) s_cur_prog = GLC_UNKNOWN;
    p_glDeleteProgram(prog);
}

void fr_glDeleteVertexArrays(GLsizei n, const GLuint *arrays)
{
    s_cur_vao = GLC_UNKNOWN;
    p_glDeleteVertexArrays(n, arrays);
}
