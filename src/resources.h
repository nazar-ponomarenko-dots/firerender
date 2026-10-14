// ============================================================================
//  resources.h — unified resource manager: files, images, textures, shaders
// ----------------------------------------------------------------------------
//  All loading in the project goes through ONE module:
//    - files:    map pak -> disk -> ../disk (in the game folder or above);
//    - images:   stb_image with a cache (key = path + channel count);
//    - textures: GL textures with a cache and refcounting — one file
//                (e.g. img/tree.png used by 60 trees) is uploaded to GL once;
//    - shaders:  cache keyed by the (vs, fs) pair — the same shader is
//                compiled once (e.g. field.vs+field.fs for FieldArea and Selection).
//  Images live until resources_shutdown() and are NOT freed manually;
//  textures/programs are freed by refcount
//  (resources_*_release instead of glDelete*).
// ============================================================================
#ifndef RESOURCES_H
#define RESOURCES_H
#include <stddef.h>       // size_t
#include "gl_loader.h"

// Reads a whole file: MAP PAK (if open) -> disk -> ../disk -> embedded data.
// Returns a malloc'ed buffer (free with free()) or NULL.
unsigned char *resources_read_file(const char *path, unsigned int *size);

// Text file (same as read_file, but with a terminating \0).
char *resources_read_text(const char *path);

// Loads an image via stb_image (cached). Returns a pointer to data OWNED
// by the cache — do NOT free; it lives until shutdown().
// channels: 3 = RGB, 4 = RGBA. If the file is missing — substitutes
// img/notfound.png (Source Engine style). NULL only if notfound.png is also missing.
unsigned char *resources_image(const char *path, int *w, int *h, int channels);

// Strict variant without substitution: NULL if the file is missing. For data
// (height maps) that are not render textures.
unsigned char *resources_image_strict(const char *path, int *w, int *h,
                                      int channels);

// Texture from file, cached and refcounted.
// wrap: GL_REPEAT / GL_CLAMP_TO_EDGE; linear: 1 = GL_LINEAR, 0 = GL_NEAREST.
GLuint resources_texture(const char *path, int channels, int wrap, int linear);

// Shader program from files (cache + refcounting).
GLuint resources_program(const char *vs_path, const char *fs_path);

// Decrease a resource's refcount; at zero — delete the GL object.
// Use INSTEAD of glDeleteProgram/glDeleteTextures for cached resources.
void resources_program_release(GLuint prog);
void resources_texture_release(GLuint tex);

// Re-request a program on hot-reload: takes a fresh program for the pair and
// releases the reference to the old one. old == 0 (first request from *_init)
// — just a load. If compilation fails, returns 0 and does NOT touch the old
// id — the caller must keep the node's previous program (it stays alive until released).
GLuint resources_program_replace(GLuint old, const char *vs_path,
                                 const char *fs_path);

// Initialization — before any loading (at the start of main), and full cleanup
// — at the very end of main, AFTER all scene modules are released.
void resources_init(void);
void resources_shutdown(void);

// --- Shader hot-reload (see shader.h) ---------------------------------------
// Rebuild hook: called after modified programs are recompiled
// (a module re-looks up its uniforms and static uniforms).
typedef void (*ReshadeFn)(void);
void resources_on_reshade(ReshadeFn fn);

// Recompiles cached shader programs whose files changed (by mtime).
// Programs from the map's zip pak are not checked. Returns the number of
// recompiled programs; when it changes, calls all registered hooks.
int resources_shader_reload(void);

// Texture hot-reload: checks the mtime of texture files on disk and
// reloads the changed ones. Textures from the zip pak are not checked.
// Returns the number of reloaded textures.
int resources_texture_reload(void);

// --- Map pak (maps/*.zip) ---------------------------------------------------
// Opens the map's zip archive: its files take priority over disk and
// embedded data (a map can pull in engine resources — they are simply put in
// the archive or left in the engine's img/). Call when loading a map.
// Returns 1 if the archive was opened.
int resources_pak_open(const char *path);
void resources_pak_close(void);
int resources_pak_is_open(void);

// Sets the base resource directory (path to the folder with map/, shaders/, img/).
// Call once at startup BEFORE any resource loading.
void resources_set_base_dir(const char *dir);

// Resolves a relative path to an absolute one using the base directory and
// the fallback chain: base_dir/path, base_dir/../path, path, ../path.
// Returns 1 if the file was found, 0 if not. The out buffer must be >= 512 bytes.
int resources_resolve_path(const char *rel_path, char *out, size_t out_sz);

#endif
