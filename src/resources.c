// ============================================================================
//  resources.c — resource manager implementation (see resources.h)
// ----------------------------------------------------------------------------
//  One shared cache (linked list): images (pixels), textures and shader
//  programs. The key is a string; textures/programs also have a refcount.
// ============================================================================
#include "resources.h"

#include "assets.h"      // make_texture
#include "miniz/miniz.h" // map zip packs
#include "shader.h"      // compile_shader_file

#define STB_IMAGE_IMPLEMENTATION   // stb_image implementation lives right here
#include "stb_image.h"

#include <SDL3/SDL.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>   // access()
#include <time.h>

// --- Map pack (maps/*.zip) ----------------------------------------------------
static mz_zip_archive s_pak;
static int s_pak_open = 0;
static char s_base_dir[512] = ".";   // resource base directory (defaults to CWD)

// Sets the base directory for resource lookup.
// Call once at startup (from main, after the exe path is determined).
void resources_set_base_dir(const char *dir)
{
    if (!dir || !dir[0]) return;
    snprintf(s_base_dir, sizeof s_base_dir, "%s", dir);
    // Strip the trailing slash
    size_t len = strlen(s_base_dir);
    if (len > 1 && (s_base_dir[len-1] == '/' || s_base_dir[len-1] == '\\'))
        s_base_dir[len-1] = 0;
    SDL_Log("Resources base dir: %s", s_base_dir);
}

// Purges map resources from the cache (declared below, used in pak_close)
static void cache_purge_pak(void);

int resources_pak_open(const char *path)
{
    if (s_pak_open) resources_pak_close();   // guard against double open
    memset(&s_pak, 0, sizeof s_pak);
    if (mz_zip_reader_init_file(&s_pak, path, 0)) {
        s_pak_open = 1;
        SDL_Log("Map pak opened: %s", path);
        return 1;
    }
    SDL_Log("Map pak open failed: %s", path);
    return 0;
}

void resources_pak_close(void)
{
    if (s_pak_open) {
        mz_zip_reader_end(&s_pak);
        s_pak_open = 0;
        cache_purge_pak();       // map resources — drop from the cache
        SDL_Log("Map pak closed");
    }
}

int resources_pak_is_open(void)
{
    return s_pak_open;
}

// Extracts a file from the pack (malloc'ed buffer), or NULL
static unsigned char *pak_read(const char *path, unsigned int *size)
{
    if (!s_pak_open) return NULL;
    size_t n = 0;
    void *buf = mz_zip_reader_extract_file_to_heap(&s_pak, path, &n, 0);
    if (!buf) return NULL;
    if (size) *size = (unsigned int)n;
    return buf;
}

// --- Files: map pack -> disk -> ../disk --------------------------------------
// The game pulls resources from its own folder (or one level up — when
// launched from build/). There is no baked-in data: running requires the folders nearby.
// Resolves a relative path to the first existing file along the fallback chain
// (single source of truth for ALL modules: audio, mapdata, lua, textures).
int resources_resolve_path(const char *rel_path, char *out, size_t out_sz)
{
    char path[512];
    snprintf(path, sizeof path, "%s", rel_path);
    // Local file (relative path already absolute or present in CWD)
    if (access(path, F_OK) == 0) {
        snprintf(out, out_sz, "%s", path);
        return 1;
    }
    // Via the base directory
    if (s_base_dir[0] && s_base_dir[0] != '.') {
        char cand[512];
        int wr = snprintf(cand, sizeof cand, "%s/%s", s_base_dir, path);
        if (wr > 0 && wr < (int)sizeof cand && access(cand, F_OK) == 0) {
            snprintf(out, out_sz, "%s", cand);
            return 1;
        }
        wr = snprintf(cand, sizeof cand, "%s/../%s", s_base_dir, path);
        if (wr > 0 && wr < (int)sizeof cand && access(cand, F_OK) == 0) {
            snprintf(out, out_sz, "%s", cand);
            return 1;
        }
    }
    // Parent of the current directory
    char up[512];
    int wr = snprintf(up, sizeof up, "../%s", path);
    if (wr > 0 && wr < (int)sizeof up && access(up, F_OK) == 0) {
        snprintf(out, out_sz, "%s", up);
        return 1;
    }
    return 0;
}

unsigned char *resources_read_file(const char *path, unsigned int *size)
{
    // Pack: miniz returns exactly the file size — append a terminating \0
    // (needed by text files: map.json, shaders, scripts).
    unsigned int psz = 0;
    unsigned char *pb = pak_read(path, &psz);
    if (pb) {
        unsigned char *buf = malloc((size_t)psz + 1);
        if (buf) {
            memcpy(buf, pb, psz);
            buf[psz] = 0;
        }
        free(pb);
        if (size) *size = psz;
        return buf;
    }

    char resolved[512];
    if (resources_resolve_path(path, resolved, sizeof resolved)) {
        FILE *f = fopen(resolved, "rb");
        if (f) {
            if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
            long szl = ftell(f);
            if (szl < 0 || (unsigned long)szl > UINT_MAX ||
                fseek(f, 0, SEEK_SET) != 0) {
                fclose(f);
                return NULL;
            }
            size_t sz = (size_t)szl;
            unsigned char *buf = malloc(sz + 1);
            if (buf && fread(buf, 1, sz, f) == sz) {
                fclose(f);
                buf[sz] = 0;
                if (size) *size = (unsigned int)sz;
                return buf;
            }
            free(buf);
            fclose(f);
        }
    }
    return NULL;
}

char *resources_read_text(const char *path)
{
    unsigned int sz = 0;
    return (char *)resources_read_file(path, &sz);
}

// --- Cache: one shared list of entries -----------------------------------------
typedef struct ResEntry {
    char key[256];       // key (path#parameters)
    void *data;          // pixels / GL texture / GL program
    int refs;            // refcount (textures/programs)
    int from_pak;        // 1 = resource came from the map zip (dropped when the map changes)
    int iw, ih;          // dimensions (images)
    // Shader program: file paths and mtimes (for hot-reload, see resources_shader_reload)
    char vs[128], fs[128];
    uint64_t vs_stamp, fs_stamp;
    // Texture: creation parameters and file mtime (for texture hot-reload)
    char tex_path[128];
    int tex_channels, tex_wrap, tex_linear;
    long tex_mt;
    struct ResEntry *next;
} ResEntry;

static ResEntry *s_cache;

static ResEntry *cache_find(const char *key)
{
    for (ResEntry *e = s_cache; e; e = e->next)
        if (strcmp(e->key, key) == 0) return e;
    return NULL;
}

static ResEntry *cache_add(const char *key, void *data, int refs)
{
    ResEntry *e = malloc(sizeof *e);
    if (!e) return NULL;
    memset(e, 0, sizeof *e);
    snprintf(e->key, sizeof e->key, "%s", key);
    e->data = data;
    e->refs = refs;
    e->from_pak = s_pak_open;   // resource from the current zip map
    e->iw = e->ih = 0;
    e->next = s_cache;
    s_cache = e;
    return e;
}

// Removes an entry from the list (frees the struct, leaves data alone).
static void cache_remove(ResEntry *e)
{
    if (s_cache == e) {
        s_cache = e->next;
    } else {
        ResEntry *p;
        for (p = s_cache; p && p->next != e; p = p->next)
            ;
        if (p) p->next = e->next;
    }
    free(e);
}

// Drops from the cache every resource that came from the map pack: when the map
// changes, the same paths (img/tree.png etc.) must reload from scratch.
// Uses safe pointer-to-pointer iteration so several
// consecutive elements can be removed correctly.
static void cache_purge_pak(void)
{
    ResEntry **pp = &s_cache;
    while (*pp) {
        ResEntry *e = *pp;
        if (e->from_pak) {
            // The map pack is closing — its GL objects are no longer valid.
            // Normally scene_teardown freed them beforehand via refcount
            // (resources_*_release: refs->0 + glDelete + cache_remove), so they
            // are already gone from the cache. If an entry is still here
            // (refs > 0 — someone skipped release), free the object
            // forcefully, otherwise it leaks forever. img# (stbi pixels)
            // belong to the cache.
            if (strncmp(e->key, "tex#", 4) == 0) {
                GLuint t = (GLuint)(uintptr_t)e->data;
                glDeleteTextures(1, &t);
            } else if (strncmp(e->key, "prog#", 5) == 0) {
                glDeleteProgram((GLuint)(uintptr_t)e->data);
            } else if (strncmp(e->key, "img#", 4) == 0) {
                stbi_image_free(e->data);
            }
            *pp = e->next;
            free(e);
        } else {
            pp = &e->next;
        }
    }
}

// File mtime through the unified path resolver (like resources_read_file)
static long file_mtime(const char *path)
{
    char resolved[512];
    struct stat st;
    if (resources_resolve_path(path, resolved, sizeof resolved) &&
        stat(resolved, &st) == 0)
        return (long)st.st_mtime;
    return 0;
}

// --- Images ----------------------------------------------------------------
// Loads pixels WITHOUT caching (map pack -> disk via the resolver).
// Returns a malloc buffer (stbi) that the caller must free
// with stbi_image_free(). NULL — load failed.
// from_pak (may be NULL) gets 1 if the data CAME FROM THE MAP PACK:
// only such cache entries must die together with the pack (cache_purge_pak).
// Engine img/*.png files are read from disk and survive across maps.
static unsigned char *image_load_raw(const char *path, int *w, int *h,
                                     int channels, int *from_pak)
{
    int iw = 0, ih = 0, c = 0;
    unsigned char *p = NULL;
    if (from_pak) *from_pak = 0;
    {   // map pack
        unsigned int sz = 0;
        unsigned char *pb = pak_read(path, &sz);
        if (pb) {
            p = stbi_load_from_memory(pb, (int)sz, &iw, &ih, &c, channels);
            free(pb);
            if (p && from_pak) *from_pak = 1;
        }
        if (!p) {
            char resolved[512];
            if (resources_resolve_path(path, resolved, sizeof resolved))
                p = stbi_load(resolved, &iw, &ih, &c, channels);
        }
    }
    if (!p) {
        SDL_Log("Cannot load %s: %s", path, stbi_failure_reason());
        return NULL;
    }
    if (w) *w = iw;
    if (h) *h = ih;
    return p;
}

unsigned char *resources_image(const char *path, int *w, int *h, int channels)
{
    char key[256];
    snprintf(key, sizeof key, "img#%s#%d", path, channels);
    ResEntry *e = cache_find(key);
    if (e) {
        if (w) *w = e->iw;
        if (h) *h = e->ih;
        return e->data;
    }

    int iw = 0, ih = 0;
    int from_pak = 0;
    unsigned char *p = image_load_raw(path, &iw, &ih, channels, &from_pak);
    if (!p) {
        // Source Engine style: a missing image is replaced with notfound.png.
        // If that is gone too — honest NULL (no recursion: guarded below).
        if (strcmp(path, "img/notfound.png") == 0) return NULL;
        SDL_Log("Texture %s not found -> using img/notfound.png", path);
        p = image_load_raw("img/notfound.png", &iw, &ih, channels, &from_pak);
    }
    if (!p) return NULL;
    ResEntry *ne = cache_add(key, p, 1);
    if (!ne) { stbi_image_free(p); return NULL; }
    ne->from_pak = from_pak;      // from the pack — only if actually from the archive
    ne->iw = iw;
    ne->ih = ih;
    if (w) *w = iw;
    if (h) *h = ih;
    return p;
}

// Strict variant without substituting non-texture data (height map): NULL —
// if the image does not exist at all, do not fall back to notfound.png.
unsigned char *resources_image_strict(const char *path, int *w, int *h,
                                      int channels)
{
    return image_load_raw(path, w, h, channels, NULL);
}

// --- Textures ----------------------------------------------------------------
GLuint resources_texture(const char *path, int channels, int wrap, int linear)
{
    char key[256];
    snprintf(key, sizeof key, "tex#%s#%d#%d#%d", path, channels, wrap, linear);
    ResEntry *e = cache_find(key);
    if (e) {
        e->refs++;
        return (GLuint)(uintptr_t)e->data;
    }
    int w = 0, h = 0;
    unsigned char *img = resources_image(path, &w, &h, channels);
    if (!img) {
        // Source Engine style: a missing texture is replaced with notfound.png.
        // If that is gone too — honest 0 (black) instead of garbage (no recursion).
        if (strcmp(path, "img/notfound.png") == 0) return 0;
        return resources_texture("img/notfound.png", channels, wrap, linear);
    }
    GLuint tex = make_texture(w, h, img, channels == 4 ? GL_RGBA : GL_RGB,
                              wrap, linear);
    ResEntry *ne = cache_add(key, (void *)(uintptr_t)tex, 1);
    if (!ne) { glDeleteTextures(1, &tex); return 0; }
    // We inherit "from the pack" from the PIXELS entry: engine img/*.png files are read
    // from disk even with a pack open and must survive a map change (otherwise
    // on a reload they get loaded again — with foreign global stb_image
    // flags and extra work).
    {
        char ikey[256];
        snprintf(ikey, sizeof ikey, "img#%s#%d", path, channels);
        ResEntry *im = cache_find(ikey);
        ne->from_pak = im ? im->from_pak : 0;
    }
    ne->iw = w;
    ne->ih = h;
    // For texture hot-reload: path and parameters + mtime (if not from the pack)
    snprintf(ne->tex_path, sizeof ne->tex_path, "%s", path);
    ne->tex_channels = channels;
    ne->tex_wrap = wrap;
    ne->tex_linear = linear;
    ne->tex_mt = ne->from_pak ? 0 : file_mtime(path);
    return tex;
}

// Texture hot-reload: reloads files that changed on disk
int resources_texture_reload(void)
{
    int changed = 0;
    for (ResEntry *e = s_cache; e; e = e->next) {
        if (strncmp(e->key, "tex#", 4) != 0) continue;
        if (e->from_pak || e->tex_path[0] == 0) continue;
        long mt = file_mtime(e->tex_path);
        if (mt == e->tex_mt) continue;
        int w = 0, h = 0;
        unsigned char *img = image_load_raw(e->tex_path, &w, &h,
                                            e->tex_channels, NULL);
        if (!img) continue;
        GLuint tex = (GLuint)(uintptr_t)e->data;
        GLenum fmt = e->tex_channels == 4 ? GL_RGBA : GL_RGB;
        GLint internal = fmt == GL_RGBA ? GL_RGBA8
                       : fmt == GL_RED ? GL_R8
                       : GL_RGB8;
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                        e->tex_linear ? GL_LINEAR : GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                        e->tex_linear ? GL_LINEAR : GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, e->tex_wrap);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, e->tex_wrap);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, internal, w, h, 0, fmt,
                     GL_UNSIGNED_BYTE, img);
        e->iw = w;
        e->ih = h;
        e->tex_mt = mt;
        // Also update the image cache (img#...) — fresh pixels for new textures
        {
            char ikey[256];
            snprintf(ikey, sizeof ikey, "img#%s#%d", e->tex_path,
                     e->tex_channels);
            ResEntry *im = cache_find(ikey);
            if (im) {
                stbi_image_free(im->data);
                im->data = img;
                im->iw = w;
                im->ih = h;
            } else {
                stbi_image_free(img);
            }
        }
        SDL_Log("texture reloaded: %s", e->tex_path);
        changed++;
    }
    return changed;
}

void resources_texture_release(GLuint tex)
{
    for (ResEntry *e = s_cache; e; e = e->next) {
        if (strncmp(e->key, "tex#", 4) != 0) continue;
        if ((GLuint)(uintptr_t)e->data != tex) continue;
        if (--e->refs <= 0) {
            glDeleteTextures(1, &tex);
            cache_remove(e);
        }
        return;
    }
    SDL_Log("resources: release of unknown texture %u", tex);
}

// --- Shader programs ------------------------------------------------------
// Links vs+fs into a program (shared by creation and hot-reload)
static GLuint program_link(const char *vs_path, const char *fs_path)
{
    GLuint vs = compile_shader_file(GL_VERTEX_SHADER, vs_path);
    GLuint fs = compile_shader_file(GL_FRAGMENT_SHADER, fs_path);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return 0;
    }
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(prog, sizeof log, NULL, log);
        SDL_Log("Program link error (%s + %s): %s", vs_path, fs_path, log);
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

GLuint resources_program(const char *vs_path, const char *fs_path)
{
    char key[256];
    snprintf(key, sizeof key, "prog#%s#%s", vs_path, fs_path);
    ResEntry *e = cache_find(key);
    if (e) {
        e->refs++;
        return (GLuint)(uintptr_t)e->data;
    }
    GLuint prog = program_link(vs_path, fs_path);
    if (!prog) return 0;
    ResEntry *ne = cache_add(key, (void *)(uintptr_t)prog, 1);
    if (!ne) { glDeleteProgram(prog); return 0; }
    // For hot-reload: file paths and mtimes. cache_add sets from_pak = "pack
    // open", but the shader may live on DISK (the pack only contains its map's
    // shaders). Refine based on whether the file is actually in the archive.
    if (s_pak_open)
        ne->from_pak = (mz_zip_reader_locate_file(&s_pak, vs_path, NULL, 0) >= 0 ||
                        mz_zip_reader_locate_file(&s_pak, fs_path, NULL, 0) >= 0);
    else
        ne->from_pak = 0;
    snprintf(ne->vs, sizeof ne->vs, "%s", vs_path);
    snprintf(ne->fs, sizeof ne->fs, "%s", fs_path);
    if (!ne->from_pak) {
        ne->vs_stamp = shader_source_stamp(vs_path);
        ne->fs_stamp = shader_source_stamp(fs_path);
    }
    return prog;
}

// --- Shader hot-reload --------------------------------------------------------
static ReshadeFn *s_reshade;
static int s_reshade_n, s_reshade_cap;

// Deferred deletion of old programs after hot-reload. The old program must NOT be
// deleted immediately: live nodes still hold its id (glb/mesh/anim do not listen to
// reshade), and they keep drawing with it. Keep it until ALL holders return
// it via resources_program_release (remaining — how many releases are left).
typedef struct { GLuint prog; int remaining; } PendingProg;
static PendingProg *s_prog_pending;
static int s_prog_pending_n, s_prog_pending_cap;

static void prog_pending_add(GLuint prog, int remaining)
{
    if (remaining <= 0) { glDeleteProgram(prog); return; }   // no holders
    // This id is already pending (repeat reload before release) — sum the releases
    for (int i = 0; i < s_prog_pending_n; i++)
        if (s_prog_pending[i].prog == prog) { s_prog_pending[i].remaining += remaining; return; }
    if (s_prog_pending_n == s_prog_pending_cap) {
        int cap = s_prog_pending_cap ? s_prog_pending_cap * 2 : 32;
        PendingProg *p = realloc(s_prog_pending, (size_t)cap * sizeof *p);
        if (!p) {
            // Live nodes still use the old id: deleting it under OOM is not allowed.
            // Safer to keep a rare leak until the GL context goes away.
            SDL_Log("resources: pending shader list OOM; keeping program %u", prog);
            return;
        }
        s_prog_pending = p;
        s_prog_pending_cap = cap;
    }
    s_prog_pending[s_prog_pending_n].prog = prog;
    s_prog_pending[s_prog_pending_n].remaining = remaining;
    s_prog_pending_n++;
}

// Returns 1 if the id was found among the pending ones and handled (release counted).
static int prog_pending_release(GLuint prog)
{
    for (int i = 0; i < s_prog_pending_n; i++) {
        if (s_prog_pending[i].prog != prog) continue;
        if (--s_prog_pending[i].remaining <= 0) {
            glDeleteProgram(prog);
            s_prog_pending[i] = s_prog_pending[--s_prog_pending_n];
        }
        return 1;
    }
    return 0;
}

void resources_on_reshade(ReshadeFn fn)
{
    if (!fn) return;
    for (int i = 0; i < s_reshade_n; i++)
        if (s_reshade[i] == fn) return;
    if (s_reshade_n == s_reshade_cap) {
        int cap = s_reshade_cap ? s_reshade_cap * 2 : 16;
        ReshadeFn *p = realloc(s_reshade, (size_t)cap * sizeof *p);
        if (!p) { SDL_Log("resources: reshade callback OOM"); return; }
        s_reshade = p;
        s_reshade_cap = cap;
    }
    s_reshade[s_reshade_n++] = fn;
}

int resources_shader_reload(void)
{
    int changed = 0;
    for (ResEntry *e = s_cache; e; e = e->next) {
        if (strncmp(e->key, "prog#", 5) != 0) continue;
        if (e->from_pak || e->vs[0] == 0) continue;
        uint64_t stamp0 = shader_source_stamp(e->vs);
        uint64_t stamp1 = shader_source_stamp(e->fs);
        if (stamp0 == e->vs_stamp && stamp1 == e->fs_stamp) continue;
        // Recompilation: the new id goes into the cache, and the old program is handed to
        // deferred deletion — nodes that do not listen to reshade are still drawing with it.
        // IMPORTANT: the refcount of the OLD program moves into pending, so for the
        // new id we zero the counter: nodes listening to reshade re-request
        // it via make_program (+1 ref), while old holders release their
        // old id through the pending list. If we don't zero it, the new id inherits
        // someone else's count and never gets deleted.
        GLuint prog = program_link(e->vs, e->fs);
        if (!prog) continue;
        GLuint old = (GLuint)(uintptr_t)e->data;
        prog_pending_add(old, e->refs);
        e->data = (void *)(uintptr_t)prog;
        e->refs = 0;                 // new id: holders will re-request it
        e->vs_stamp = stamp0;
        e->fs_stamp = stamp1;
        SDL_Log("shader reloaded: %s + %s", e->vs, e->fs);
        changed++;
    }
    if (changed)
        for (int i = 0; i < s_reshade_n; i++)
            s_reshade[i]();
    return changed;
}

void resources_program_release(GLuint prog)
{
    if (!prog) return;
    // First — the pending (old, post-hot-reload) programs
    if (prog_pending_release(prog)) return;
    for (ResEntry *e = s_cache; e; e = e->next) {
        if (strncmp(e->key, "prog#", 5) != 0) continue;
        if ((GLuint)(uintptr_t)e->data != prog) continue;
        if (--e->refs <= 0) {
            glDeleteProgram(prog);
            cache_remove(e);
        }
        return;
    }
    SDL_Log("resources: release of unknown program %u", prog);
}

// Program re-request for hot-reload: takes a fresh reference to the current
// program of the pair and only then releases the old one. That is exactly what balances
// the refcount on reload: without release(old), every hot-reload leaked one
// program reference per reshade listener.
// Order matters: request the new one first (if the pair fails to link, the old
// id stays with the node alive and unharmed), release the old one only after success.
// old == 0 (first call from *_init) — release is skipped.
GLuint resources_program_replace(GLuint old, const char *vs_path,
                                 const char *fs_path)
{
    GLuint prog = resources_program(vs_path, fs_path);
    if (!prog) return 0;                 // failed to link — leave the old one alone
    if (old) resources_program_release(old);
    return prog;
}

// --- Lifecycle -----------------------------------------------------------
void resources_init(void)
{
    s_cache = NULL;
}

void resources_shutdown(void)
{
    // Pending programs (old ids after hot-reload): their holders may not
    // have lived until shutdown — on exit delete them all at once.
    for (int i = 0; i < s_prog_pending_n; i++)
        glDeleteProgram(s_prog_pending[i].prog);
    free(s_prog_pending);
    s_prog_pending = NULL;
    s_prog_pending_n = s_prog_pending_cap = 0;
    int n = 0;
    ResEntry *e = s_cache;
    while (e) {
        ResEntry *next = e->next;
        if (strncmp(e->key, "tex#", 4) == 0) {
            GLuint t = (GLuint)(uintptr_t)e->data;
            glDeleteTextures(1, &t);
        } else if (strncmp(e->key, "prog#", 5) == 0) {
            glDeleteProgram((GLuint)(uintptr_t)e->data);
        } else {
            stbi_image_free(e->data);
        }
        free(e);
        e = next;
        n++;
    }
    s_cache = NULL;
    free(s_reshade);
    s_reshade = NULL;
    s_reshade_n = s_reshade_cap = 0;
    SDL_Log("resources: %d cached entries freed", n);
}
