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

// Fingerprint of the source together with its recursive #include files on
// disk. Changes when a shared GLSL file is edited, even if the main .vs/.fs
// was untouched.
uint64_t shader_source_stamp(const char *path);

// Compiles a shader from a file; prints the log on error. Returns the ID or 0.
GLuint compile_shader_file(GLenum type, const char *path);

// Links the vertex + fragment shaders into a program (cached in resources.c).
GLuint make_program(const char *vs_path, const char *fs_path);

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
