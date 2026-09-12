// ============================================================================
//  gl_loader.h — мини-загрузчик функций OpenGL вместо GLEW
// ----------------------------------------------------------------------------
//  GL_FUNCS (X-макрос) генерирует указатели p_glXxx и код их загрузки через
//  SDL_GL_GetProcAddress (работает на любом бэкенде SDL: Wayland/EGL,
//  X11/GLX, Windows/WGL, macOS/CGL).
//
//  Заголовок также подключает платформенные GL-заголовки (типы/константы).
//  Имя glXxx везде в коде — макрос на p_glXxx.
// ============================================================================
#ifndef GL_LOADER_H
#define GL_LOADER_H

// --- Платформенные заголовки OpenGL ------------------------------------------
// Десктоп использует GL (gl.h + glext.h от Khronos), мобильные — GLES (gl3.h).
#if defined(__ANDROID__) || defined(__EMSCRIPTEN__) || defined(USE_GLES)
#  define USE_GLES_PATH 1
#  include <GLES3/gl3.h>          // GLES 3.x: Android, Web, ARM-платы
#elif defined(_WIN32)
#  include <GL/gl.h>              // Windows: системный GL 1.1...
#  include <GL/glext.h>           // ...плюс Khronos-заголовки с константами GL3+
#elif defined(__APPLE__)
#  include <OpenGL/gl3.h>         // macOS: фреймворк OpenGL, заголовок GL 3.2+
#else
#  include <GL/gl.h>              // Linux: системный GL (Mesa/драйвер)...
#  include <GL/glext.h>           // ...и заголовок расширений/констант Khronos
#endif

#ifndef APIENTRY
#  ifdef GL_APIENTRY
#    define APIENTRY GL_APIENTRY
#  else
#    define APIENTRY
#  endif
#endif

// --- Константы GLES 3.1 / GL 4.3 (compute + SSBO) ----------------------------
//  GLES3-заголовок (gl3.h) их не знает: это фичи GLES 3.1. Числовые значения
//  одинаковы в OpenGL и OpenGL ES — просто доопределяем. Если контекст
//  устройства младше 3.1, функции из GL_FUNCS_OPT останутся NULL и движок
//  откатится на инстансный путь (g_gl_gpu_driven == 0).
#if defined(USE_GLES_PATH) && !defined(GL_SHADER_STORAGE_BUFFER)
#define GL_COMPUTE_SHADER                0x91B9
#define GL_MAX_COMPUTE_WORK_GROUP_COUNT  0x91BE
#define GL_MAX_COMPUTE_WORK_GROUP_SIZE   0x91BF
#define GL_SHADER_STORAGE_BUFFER         0x90D2
#define GL_SHADER_STORAGE_BLOCK          0x92E6
#define GL_BUFFER_UPDATE_BARRIER_BIT     0x00000100
#define GL_COMMAND_BARRIER_BIT           0x00000400
#define GL_ELEMENT_ARRAY_BARRIER_BIT     0x00000002
#define GL_PIXEL_BUFFER_BARRIER_BIT      0x00000080
#define GL_SHADER_IMAGE_ACCESS_BARRIER_BIT 0x00000020
#define GL_TEXTURE_FETCH_BARRIER_BIT     0x00000008
#define GL_TEXTURE_UPDATE_BARRIER_BIT    0x00000100
#define GL_UNIFORM_BARRIER_BIT           0x00000004
#define GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT 0x00000001
#define GL_SHADER_STORAGE_BARRIER_BIT   0x00002000
#define GL_DRAW_INDIRECT_BUFFER         0x8F3F
#define GL_TIME_ELAPSED                 0x88BF
#define GL_R16                          0x822A   // на GLES = GL_R16_EXT
#define GL_ALL_BARRIER_BITS              0xFFFFFFFFu
#endif

#define GL_FUNCS(F)                                                              \
    F(const GLubyte *, glGetString,              (GLenum))                        \
    F(void,             glClearColor,            (GLfloat, GLfloat, GLfloat, GLfloat)) \
    F(void,             glClear,                 (GLbitfield))                    \
    F(GLuint,           glCreateShader,          (GLenum))                        \
    F(void,             glShaderSource,          (GLuint, GLsizei, const GLchar * const *, const GLint *)) \
    F(void,             glCompileShader,         (GLuint))                        \
    F(void,             glGetShaderiv,           (GLuint, GLenum, GLint *))       \
    F(void,             glGetShaderInfoLog,      (GLuint, GLsizei, GLsizei *, GLchar *)) \
    F(void,             glGetProgramiv,          (GLuint, GLenum, GLint *))       \
    F(void,             glGetProgramInfoLog,     (GLuint, GLsizei, GLsizei *, GLchar *)) \
    F(GLuint,           glCreateProgram,         (void))                          \
    F(void,             glAttachShader,          (GLuint, GLuint))                \
    F(void,             glLinkProgram,           (GLuint))                        \
    F(void,             glDeleteShader,          (GLuint))                        \
    F(void,             glDeleteProgram,         (GLuint))                        \
    F(void,             glGenVertexArrays,       (GLsizei, GLuint *))             \
    F(void,             glBindVertexArray,       (GLuint))                        \
    F(void,             glGenBuffers,            (GLsizei, GLuint *))             \
    F(void,             glBindBuffer,            (GLenum, GLuint))                \
    F(void,             glBufferData,            (GLenum, GLsizeiptr, const void *, GLenum)) \
    F(void,             glBufferSubData,         (GLenum, GLintptr, GLsizeiptr, const void *)) \
    F(void,             glVertexAttribPointer,   (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *)) \
    F(void,             glReadPixels,            (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *)) \
    F(void,             glGenFramebuffers,       (GLsizei, GLuint *))             \
    F(void,             glBindFramebuffer,       (GLenum, GLuint))                \
    F(void,             glFramebufferTexture2D,  (GLenum, GLenum, GLenum, GLuint, GLint)) \
    F(void,             glGenRenderbuffers,      (GLsizei, GLuint *))             \
    F(void,             glBindRenderbuffer,      (GLenum, GLuint))                \
    F(void,             glRenderbufferStorage,   (GLenum, GLenum, GLsizei, GLsizei)) \
    F(void,             glRenderbufferStorageMultisample, (GLenum, GLsizei, GLenum, GLsizei, GLsizei)) \
    F(void,             glBlitFramebuffer,       (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum)) \
    F(void,             glFramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint)) \
    F(GLenum,           glGetFramebufferAttachmentParameteriv, (GLenum, GLenum, GLenum, GLint *)) \
    F(void,             glGetTexLevelParameteriv, (GLenum, GLint, GLenum, GLint *)) \
    F(GLboolean,        glIsProgram,             (GLuint))                        \
    F(void,             glGetActiveUniform,      (GLuint, GLuint, GLsizei, GLsizei *, GLint *, GLenum *, GLchar *)) \
    F(GLboolean,        glIsVertexArray,         (GLuint))                        \
    F(void,             glDeleteRenderbuffers,   (GLsizei, const GLuint *))       \
    F(GLenum,           glCheckFramebufferStatus,(GLenum))                        \
    F(void,             glGetIntegerv,           (GLenum, GLint *))               \
    F(GLboolean,        glIsEnabled,             (GLenum))                        \
    F(GLenum,           glGetError,              (void))                          \
    F(void,             glDeleteFramebuffers,    (GLsizei, const GLuint *))       \
    F(void,             glPolygonOffset,         (GLfloat, GLfloat))              \
    F(void,             glVertexAttribDivisor,   (GLuint, GLuint))                 \
    F(void,             glDrawElements,          (GLenum, GLsizei, GLenum, const void *)) \
    F(void,             glDrawElementsInstanced, (GLenum, GLsizei, GLenum, const void *, GLsizei)) \
    F(void,             glDrawArraysInstanced,  (GLenum, GLint, GLsizei, GLsizei)) \

// --- OPTIONAL: GPU-driven рендер (compute + indirect) ------------------------
//  Нужны для варианта Б (compute-cull + indirect draw). Доступны с GL 4.3
//  (desktop) / GLES 3.1. Загружаются НЕ фатально: указатель == NULL, если
//  контекст старый — тогда g_gl_gpu_driven = 0 и используется инстансный
//  фолбэк. Не включены в GL_FUNCS, чтобы не ломать загрузку на GL 3.3.
#define GL_FUNCS_OPT(F)                                                          \
    F(void,             glDrawBuffer,                (GLenum))                   \
    F(void,             glReadBuffer,                (GLenum))                   \
    F(void,             glPolygonMode,               (GLenum, GLenum))           \
    F(void,             glDrawElementsInstancedBaseInstance, (GLenum, GLsizei, GLenum, const void *, GLsizei, GLuint)) \
    F(void,             glDispatchCompute,           (GLuint, GLuint, GLuint))   \
    F(void,             glMemoryBarrier,             (GLbitfield))               \
    F(void,             glBindBufferBase,            (GLenum, GLuint, GLuint))   \
    F(void,             glBindBufferRange,           (GLenum, GLuint, GLuint, GLintptr, GLsizeiptr)) \
    F(void,             glShaderStorageBlockBinding, (GLuint, GLuint, GLuint))   \
    F(void,             glDrawElementsIndirect,      (GLenum, GLenum, const void *)) \
    F(void,             glMultiDrawElementsIndirect, (GLenum, GLenum, const void *, GLsizei, GLsizei)) \
    F(GLuint,           glGetProgramResourceIndex,    (GLuint, GLenum, const char *)) \
    F(void,             glEnableVertexAttribArray,(GLuint))                       \
    F(void,             glUseProgram,            (GLuint))                        \
    F(void,             glDrawArrays,            (GLenum, GLint, GLsizei))        \
    F(void,             glDeleteVertexArrays,    (GLsizei, const GLuint *))       \
    F(void,             glDeleteBuffers,         (GLsizei, const GLuint *))       \
    F(void,             glActiveTexture,         (GLenum))                        \
    F(void,             glGenTextures,           (GLsizei, GLuint *))             \
    F(void,             glDeleteTextures,        (GLsizei, const GLuint *))       \
    F(void,             glBindTexture,           (GLenum, GLuint))                \
    F(void,             glTexParameteri,         (GLenum, GLenum, GLint))         \
    F(void,             glGenerateMipmap,        (GLenum))                         \
    F(void,             glTexImage2D,            (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *)) \
    F(void,             glCopyTexSubImage2D,     (GLenum, GLint, GLint, GLint, GLint, GLint, GLsizei, GLsizei)) \
    F(void,             glTexImage3D,            (GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *)) \
    F(void,             glTexSubImage3D,         (GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei, GLenum, GLenum, const void *)) \
    F(void,             glPixelStorei,           (GLenum, GLint))                 \
    F(GLint,            glGetUniformLocation,    (GLuint, const GLchar *))        \
    F(GLint,            glGetAttribLocation,     (GLuint, const GLchar *))        \
    F(void,             glUniform1i,             (GLint, GLint))                  \
    F(void,             glUniform1f,             (GLint, GLfloat))                \
    F(void,             glUniform2f,             (GLint, GLfloat, GLfloat))       \
    F(void,             glUniform3f,             (GLint, GLfloat, GLfloat, GLfloat)) \
    F(void,             glUniform4f,             (GLint, GLfloat, GLfloat, GLfloat, GLfloat)) \
    F(void,             glUniformMatrix4fv,      (GLint, GLsizei, GLboolean, const GLfloat *)) \
    F(void,             glUniform3fv,           (GLint, GLsizei, const GLfloat *)) \
    F(void,             glUniform4fv,           (GLint, GLsizei, const GLfloat *)) \
    F(void,             glUniform1fv,           (GLint, GLsizei, const GLfloat *)) \
    F(void,             glViewport,              (GLint, GLint, GLsizei, GLsizei)) \
    F(void,             glEnable,                (GLenum))                        \
    F(void,             glDisable,               (GLenum))                        \
    F(void,             glCullFace,              (GLenum))                        \
    F(void,             glFrontFace,             (GLenum))                        \
    F(void,             glBlendFunc,             (GLenum, GLenum))                \
    F(void,             glDepthMask,             (GLboolean))                     \
    F(void,             glGenQueries,            (GLsizei, GLuint *))             \
    F(void,             glDeleteQueries,         (GLsizei, const GLuint *))       \
    F(void,             glBeginQuery,            (GLenum, GLuint))                \
    F(void,             glEndQuery,              (GLenum))                        \
    F(void,             glGetQueryObjectiv,     (GLuint, GLenum, GLint *))       \
    F(void,             glGetQueryObjectui64v,   (GLuint, GLenum, GLuint64 *))

#define F(ret, name, args) extern ret (APIENTRY *p_##name) args;
GL_FUNCS(F)
#undef F

// OPTIONAL GPU-driven указатели (NULL, если контекст < GL 4.3).
#define F(ret, name, args) extern ret (APIENTRY *p_##name) args;
GL_FUNCS_OPT(F)
#undef F

// 1, если контекст поддерживает compute + indirect (GPU-driven рендер).
// Устанавливается в load_gl_funcs().
extern int g_gl_gpu_driven;

#define glGetString               p_glGetString
#define glClearColor              p_glClearColor
#define glClear                   p_glClear
#define glCreateShader            p_glCreateShader
#define glShaderSource            p_glShaderSource
#define glCompileShader           p_glCompileShader
#define glGetShaderiv             p_glGetShaderiv
#define glGetShaderInfoLog        p_glGetShaderInfoLog
#define glGetProgramiv            p_glGetProgramiv
#define glGetProgramInfoLog       p_glGetProgramInfoLog
#define glCreateProgram           p_glCreateProgram
#define glAttachShader            p_glAttachShader
#define glLinkProgram             p_glLinkProgram
#define glDeleteShader            p_glDeleteShader
#define glDeleteProgram           p_glDeleteProgram
#define glGenVertexArrays         p_glGenVertexArrays
#define glBindVertexArray         p_glBindVertexArray
#define glGenBuffers              p_glGenBuffers
#define glBindBuffer              p_glBindBuffer
#define glBufferData              p_glBufferData
#define glBufferSubData           p_glBufferSubData
#define glVertexAttribPointer     p_glVertexAttribPointer
#define glReadPixels              p_glReadPixels
#define glGenFramebuffers         p_glGenFramebuffers
#define glBindFramebuffer         p_glBindFramebuffer
#define glFramebufferTexture2D    p_glFramebufferTexture2D
#define glGenRenderbuffers        p_glGenRenderbuffers
#define glBindRenderbuffer        p_glBindRenderbuffer
#define glRenderbufferStorage     p_glRenderbufferStorage
#define glRenderbufferStorageMultisample p_glRenderbufferStorageMultisample
#define glBlitFramebuffer         p_glBlitFramebuffer
#define glFramebufferRenderbuffer p_glFramebufferRenderbuffer
#define glGetFramebufferAttachmentParameteriv p_glGetFramebufferAttachmentParameteriv
#define glGetTexLevelParameteriv      p_glGetTexLevelParameteriv
#define glIsProgram                   p_glIsProgram
#define glGetActiveUniform            p_glGetActiveUniform
#define glIsVertexArray               p_glIsVertexArray
#define glDeleteRenderbuffers     p_glDeleteRenderbuffers
#define glCheckFramebufferStatus  p_glCheckFramebufferStatus
#define glGetIntegerv             p_glGetIntegerv
#define glIsEnabled               p_glIsEnabled
#define glGetError                p_glGetError
#define glDrawBuffer              p_glDrawBuffer
#define glReadBuffer              p_glReadBuffer
#define glDeleteFramebuffers      p_glDeleteFramebuffers
#define glPolygonMode             p_glPolygonMode
#define glPolygonOffset           p_glPolygonOffset
#define glVertexAttribDivisor     p_glVertexAttribDivisor
#define glDrawElements            fr_glDrawElements
#define glDrawElementsInstanced   fr_glDrawElementsInstanced
#define glDrawElementsInstancedBaseInstance fr_glDrawElementsInstancedBaseInstance
// OPTIONAL GPU-driven функции: макросы на указатели (NULL, если контекст
// старый — вызов только под защитой g_gl_gpu_driven).
#define glDispatchCompute            p_glDispatchCompute
#define glMemoryBarrier              p_glMemoryBarrier
#define glBindBufferBase             p_glBindBufferBase
#define glBindBufferRange            p_glBindBufferRange
#define glShaderStorageBlockBinding  p_glShaderStorageBlockBinding
#define glDrawElementsIndirect       p_glDrawElementsIndirect
#define glMultiDrawElementsIndirect  p_glMultiDrawElementsIndirect
#define glEnableVertexAttribArray p_glEnableVertexAttribArray
#define glUseProgram              p_glUseProgram
#define glDrawArrays              fr_glDrawArrays
#define glDrawArraysInstanced     fr_glDrawArraysInstanced
#define glDeleteVertexArrays      p_glDeleteVertexArrays
#define glDeleteBuffers           p_glDeleteBuffers
#define glActiveTexture           p_glActiveTexture
#define glGenTextures             p_glGenTextures
#define glDeleteTextures          p_glDeleteTextures
#define glBindTexture             p_glBindTexture
#define glTexParameteri           p_glTexParameteri
#define glGenerateMipmap          p_glGenerateMipmap
#define glTexImage2D              p_glTexImage2D
#define glCopyTexSubImage2D       p_glCopyTexSubImage2D
#define glTexImage3D              p_glTexImage3D
#define glTexSubImage3D           p_glTexSubImage3D
#define glPixelStorei             p_glPixelStorei
#define glGetUniformLocation      p_glGetUniformLocation
#define glGetAttribLocation       p_glGetAttribLocation
#define glUniform1i               p_glUniform1i
#define glUniform1f               p_glUniform1f
#define glUniform2f               p_glUniform2f
#define glUniform3f               p_glUniform3f
#define glUniform4f               p_glUniform4f
#define glUniformMatrix4fv        p_glUniformMatrix4fv
#define glUniform3fv              p_glUniform3fv
#define glUniform4fv              p_glUniform4fv
#define glUniform1fv              p_glUniform1fv
#define glViewport                p_glViewport
#define glEnable                  p_glEnable
#define glDisable                 p_glDisable
#define glCullFace                p_glCullFace
#define glFrontFace               p_glFrontFace
#define glBlendFunc               p_glBlendFunc
#define glDepthMask               p_glDepthMask
#define glGenQueries              p_glGenQueries
#define glDeleteQueries           p_glDeleteQueries
#define glBeginQuery              p_glBeginQuery
#define glEndQuery                p_glEndQuery
#define glGetQueryObjectiv        p_glGetQueryObjectiv
#define glGetQueryObjectui64v     p_glGetQueryObjectui64v

// Загружает все указатели из текущего GL-контекста. 0 — успех, -1 — ошибка.
int load_gl_funcs(void);

// --- Счётчики профилирования (профайлер) ---------------------------------------
// Обёртки glDraw* считают число вызовов, вершин и треугольников.
// Обнуляются prof_reset() каждый кадр.
extern int g_prof_draws;
extern int g_prof_verts;
extern int g_prof_tris;
void fr_glDrawArrays(GLenum mode, GLint first, GLsizei count);
void fr_glDrawArraysInstanced(GLenum mode, GLint first, GLsizei count,
                              GLsizei primcount);
void fr_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices);
void fr_glDrawElementsInstanced(GLenum mode, GLsizei count, GLenum type,
                                const void *indices, GLsizei primcount);
void fr_glDrawElementsInstancedBaseInstance(GLenum mode, GLsizei count,
                                            GLenum type, const void *indices,
                                            GLsizei primcount, GLuint baseinstance);

#endif
