// ============================================================================
//  shader.h — loading and compiling GLSL shaders from the shaders/ folder
// ----------------------------------------------------------------------------
//  The GLSL version and the GLES precision depend on the platform, so they
//  are injected by code (see compile_shader_file); shader files contain
//  only the body.
// ============================================================================
#ifndef SHADER_H
#define SHADER_H
#include "gl_loader.h"
#include "resources.h"   // ReshadeFn (hot-reload)
#include <stdint.h>

// The #version line shader.c prefixes to every graphics shader, and the same
// dialect in words — the minimum driver the game needs. One definition so the
// boot diagnostic (main.c) cannot drift from what the compiler really asks for.
#ifdef USE_GLES_PATH
#define FR_GLSL_VERSION_LINE "#version 300 es\n"
#define FR_GLSL_DIALECT      "GLSL 300 es (OpenGL ES 3.0)"
#else
#define FR_GLSL_VERSION_LINE "#version 330 core\n"
#define FR_GLSL_DIALECT      "GLSL 330 core (OpenGL 3.3)"
#endif

// Fingerprint of the source together with its recursive #include files on
// disk. Changes when a shared GLSL file is edited, even if the main .vs/.fs
// was untouched.
uint64_t shader_source_stamp(const char *path);

// Compiles a shader from a file; prints the log on error. Returns the ID or 0.
GLuint compile_shader_file(GLenum type, const char *path);

// Links the vertex + fragment shaders into a program (cached in resources.c).
GLuint make_program(const char *vs_path, const char *fs_path);

// Compiles and links a one-line shader pair in the exact GLSL dialect all of
// this engine's shaders are built with. Returns 0 when the driver cannot handle
// that dialect at all (a GL 2.1 / GLSL 1.20 driver rejects "#version 330 core"),
// which is the ground truth for "this GPU cannot render anything here".
int shader_dialect_supported(void);

// Compiles and links a COMPUTE shader (.cs) into a program.
// Used by the GPU-driven vegetation cull pass (GL 4.3+). Not cached.
// Returns the ID or 0. The caller binds SSBO blocks to binding points
// via glShaderStorageBlockBinding(prog, ...).
GLuint make_compute_program(const char *cs_path);

// --- Shader hot-reload --------------------------------------------------------
// Register a rebuild hook: after the changed programs are recompiled, a
// module re-looks-up its uniforms and static uniforms (see resources.c).
void shader_on_reshade(ReshadeFn fn);

// Recompiles the changed shaders (by file mtime) and calls the hooks.
// Call from the main loop (about every 30 frames) and via the reload_shaders
// command.
int shader_hot_reload(void);

#endif
