// ============================================================================
//  gl_loader.h — a minimal OpenGL function loader instead of GLEW
// ----------------------------------------------------------------------------
//  GL_FUNCS (X-macro) generates the p_glXxx pointers and the code loading them
//  via SDL_GL_GetProcAddress (works on any SDL backend: Wayland/EGL,
//  X11/GLX, Windows/WGL, macOS/CGL).
//
//  The header also pulls in platform GL headers (types/constants).
//  The name glXxx everywhere in the code is a macro to p_glXxx.
// ============================================================================
#ifndef GL_LOADER_H
#define GL_LOADER_H

// --- Platform OpenGL headers ------------------------------------------
// Desktop uses GL (Khronos gl.h + glext.h), mobile — GLES (gl3.h).
#if defined(__ANDROID__) || defined(__EMSCRIPTEN__) || defined(USE_GLES)
#  define USE_GLES_PATH 1
#  include <GLES3/gl3.h>          // GLES 3.x: Android, Web, ARM boards
#elif defined(_WIN32)
#  include <GL/gl.h>              // Windows: system GL 1.1...
#  include <GL/glext.h>           // ...plus Khronos headers with GL3+ constants
#elif defined(__APPLE__)
#  include <OpenGL/gl3.h>         // macOS: OpenGL framework, GL 3.2+ header
#else
#  include <GL/gl.h>              // Linux: system GL (Mesa/driver)...
#  include <GL/glext.h>           // ...and the Khronos extensions/constants header
#endif

#ifndef APIENTRY
#  ifdef GL_APIENTRY
#    define APIENTRY GL_APIENTRY
#  else
#    define APIENTRY
#  endif
#endif

// --- GLES 3.1 / GL 4.3 constants (compute + SSBO) ----------------------------
//  The GLES3 header (gl3.h) does not know them: these are GLES 3.1 features.
//  The numeric values are the same in OpenGL and OpenGL ES — we just define
//  the extras. If the device context is older than 3.1, the functions in
//  GL_FUNCS_OPT stay NULL and the engine falls back to the instanced path
//  (g_gl_gpu_driven == 0).
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
#define GL_R16                          0x822A   // on GLES = GL_R16_EXT
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

// --- OPTIONAL: GPU-driven rendering (compute + indirect) ------------------------
//  Needed for variant B (compute-cull + indirect draw). Available since GL 4.3
//  (desktop) / GLES 3.1. Loaded NON-fatally: the pointer == NULL if the
//  context is old — then g_gl_gpu_driven = 0 and the instanced fallback is
//  used. Kept out of GL_FUNCS so loading doesn't break on GL 3.3.
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
    F(void,             glDepthFunc,             (GLenum))                        \
    F(void,             glColorMask,             (GLboolean, GLboolean, GLboolean, GLboolean)) \
    F(void,             glGenQueries,            (GLsizei, GLuint *))             \
    F(void,             glDeleteQueries,         (GLsizei, const GLuint *))       \
    F(void,             glBeginQuery,            (GLenum, GLuint))                \
    F(void,             glEndQuery,              (GLenum))                        \
    F(void,             glGetQueryObjectiv,     (GLuint, GLenum, GLint *))       \
    F(void,             glGetQueryObjectui64v,   (GLuint, GLenum, GLuint64 *))

#define F(ret, name, args) extern ret (APIENTRY *p_##name) args;
GL_FUNCS(F)
#undef F

// OPTIONAL GPU-driven pointers (NULL if the context is < GL 4.3).
#define F(ret, name, args) extern ret (APIENTRY *p_##name) args;
GL_FUNCS_OPT(F)
#undef F

// 1 if the context supports compute + indirect (GPU-driven rendering).
// Set in load_gl_funcs().
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
#define glDeleteProgram           fr_glDeleteProgram
#define glGenVertexArrays         p_glGenVertexArrays
#define glBindVertexArray         fr_glBindVertexArray
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
// OPTIONAL GPU-driven functions: macros onto the pointers (NULL if the context
// is old — call only under the g_gl_gpu_driven guard).
#define glDispatchCompute            p_glDispatchCompute
#define glMemoryBarrier              p_glMemoryBarrier
#define glBindBufferBase             p_glBindBufferBase
#define glBindBufferRange            p_glBindBufferRange
#define glShaderStorageBlockBinding  p_glShaderStorageBlockBinding
#define glDrawElementsIndirect       p_glDrawElementsIndirect
#define glMultiDrawElementsIndirect  p_glMultiDrawElementsIndirect
#define glEnableVertexAttribArray p_glEnableVertexAttribArray
#define glUseProgram              fr_glUseProgram
#define glDrawArrays              fr_glDrawArrays
#define glDrawArraysInstanced     fr_glDrawArraysInstanced
#define glDeleteVertexArrays      fr_glDeleteVertexArrays
#define glDeleteBuffers           p_glDeleteBuffers
#define glActiveTexture           fr_glActiveTexture
#define glGenTextures             p_glGenTextures
#define glDeleteTextures          fr_glDeleteTextures
#define glBindTexture             fr_glBindTexture
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
#define glDepthFunc               p_glDepthFunc
#define glColorMask               p_glColorMask
#define glGenQueries              p_glGenQueries
#define glDeleteQueries           p_glDeleteQueries
#define glBeginQuery              p_glBeginQuery
#define glEndQuery                p_glEndQuery
#define glGetQueryObjectiv        p_glGetQueryObjectiv
#define glGetQueryObjectui64v     p_glGetQueryObjectui64v

// Loads all pointers from the current GL context. 0 — success, -1 — error.
int load_gl_funcs(void);

// --- Profiling counters (profiler) ---------------------------------------
// glDraw* wrappers count the number of calls, vertices and triangles.
// Zeroed by prof_reset() every frame.
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

// --- Filtering out redundant GL state changes ------------------------------------
// A thin cache over the real pointers: skips bind/use when the resource is
// already bound to that slot (analog of GL_Bind in xash — the single funnel
// through which ALL modules go). Pure CPU/driver savings, the picture is
// byte-for-byte the same. FR_GLCACHE=0 disables it (for A/B).
// g_prof_state_skipped — number of calls filtered out per frame (reset by prof_reset()).
extern int g_prof_state_skipped;
void fr_glUseProgram(GLuint prog);
void fr_glBindVertexArray(GLuint vao);
void fr_glActiveTexture(GLenum texture);
void fr_glBindTexture(GLenum target, GLuint tex);
void fr_glDeleteTextures(GLsizei n, const GLuint *textures);
void fr_glDeleteProgram(GLuint prog);
void fr_glDeleteVertexArrays(GLsizei n, const GLuint *arrays);

#endif
