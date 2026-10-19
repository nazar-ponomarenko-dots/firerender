// ============================================================================
//  shader.c — reading shader files and building programs
// ----------------------------------------------------------------------------
//  Files are read through the resource manager (resources_read_text: disk ->
//  ../disk -> embedded data). Programs are CACHED by the (vs, fs) pair in
//  resources.c — the same shader is compiled once no matter how many nodes
//  use it (see resources_program).
// ============================================================================
#include "shader.h"

#include "resources.h"

#include <SDL3/SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t stamp_mix(uint64_t h, uint64_t v)
{
    h ^= v;
    return h * UINT64_C(1099511628211);
}

static uint64_t shader_stamp_recursive(const char *path, int depth)
{
    char *text = resources_read_text(path);
    if (!text) return 0;
    uint64_t h = UINT64_C(1469598103934665603);
    for (const unsigned char *s = (const unsigned char *)text; *s; s++)
        h = stamp_mix(h, *s);
    if (depth >= 8) { free(text); return h; }
    char dir[512] = "";
    const char *slash = strrchr(path, '/');
    if (slash) {
        size_t n = (size_t)(slash - path) + 1;
        if (n >= sizeof dir) n = sizeof dir - 1;
        memcpy(dir, path, n);
        dir[n] = 0;
    }
    for (const char *p = text; *p;) {
        const char *eol = strchr(p, '\n');
        size_t n = eol ? (size_t)(eol - p) : strlen(p);
        char line[1024], inc[256];
        size_t copy = n < sizeof line - 1 ? n : sizeof line - 1;
        memcpy(line, p, copy);
        line[copy] = 0;
        if (!strncmp(line, "#include ", 9) &&
            sscanf(line, "#include \"%255[^\"]\"", inc) == 1) {
            char child[512];
            snprintf(child, sizeof child, "%s%s", dir, inc);
            h = stamp_mix(h, shader_stamp_recursive(child, depth + 1));
        }
        p = eol ? eol + 1 : p + n;
    }
    free(text);
    return h;
}

uint64_t shader_source_stamp(const char *path)
{
    return path ? shader_stamp_recursive(path, 0) : 0;
}

// GLSL prefix for the current platform
static const char *shader_version(GLenum type)
{
#ifdef USE_GLES_PATH
    if (type == GL_COMPUTE_SHADER)
        return "#version 310 es\nprecision highp float;\nprecision highp int;\n#define FR_HEIGHT_RG8 1\n";
    return FR_GLSL_VERSION_LINE
           "precision highp float;\nprecision highp int;\n#define FR_HEIGHT_RG8 1\n";
#else
    if (type == GL_COMPUTE_SHADER)
        return "#version 430 core\n";   // compute shaders require GLSL 4.30+
    return FR_GLSL_VERSION_LINE;
#endif
}

// Expands #include "file" — recursively substitutes the contents
// (depth up to 8, cycles are not checked). Paths are relative to the shader
// directory. This way the whole scene shares common code (shaders/snow.glsl,
// csm.glsl).
static int buf_grow(char **buf, size_t *len, size_t *cap, const char *s)
{
    size_t sl = strlen(s);
    if (sl > SIZE_MAX - *len - 1) return 0;
    size_t need = *len + sl + 1;
    size_t next = *cap;
    while (need > next) {
        if (next > SIZE_MAX / 2) { next = need; break; }
        next = next ? next * 2 : 4096;
    }
    if (next != *cap) {
        char *p = realloc(*buf, next);
        if (!p) return 0;
        *buf = p;
        *cap = next;
    }
    memcpy(*buf + *len, s, sl);
    *len += sl;
    (*buf)[*len] = 0;
    return 1;
}

static char *shader_expand_includes(const char *text, const char *dir, int depth)
{
    char *out = NULL;
    size_t len = 0, cap = 0;
    const char *p = text;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t ll = eol ? (size_t)(eol - p) : strlen(p);
        char line[1024];
        size_t cl = ll < sizeof line - 1 ? ll : sizeof line - 1;
        memcpy(line, p, cl);
        line[cl] = 0;
        char inc[256];
        if (!buf_grow(&out, &len, &cap, "")) goto oom;
        if (depth < 8 && !strncmp(line, "#include ", 9) &&
            sscanf(line, "#include \"%255[^\"]\"", inc) == 1) {
            char path[512];
            snprintf(path, sizeof path, "%s%s", dir, inc);
            char *body = resources_read_text(path);
            if (body) {
                char *exp = shader_expand_includes(body, dir, depth + 1);
                free(body);
                if (!exp || !buf_grow(&out, &len, &cap, exp)) {
                    free(exp);
                    goto oom;
                }
                free(exp);
            } else {
                // file missing — leave the line as is (the error shows in the GLSL log)
                if (!buf_grow(&out, &len, &cap, line)) goto oom;
            }
        } else {
            if (!buf_grow(&out, &len, &cap, line)) goto oom;
        }
        if (!buf_grow(&out, &len, &cap, "\n")) goto oom;
        p = eol ? eol + 1 : p + ll;
    }
    return out;
oom:
    free(out);
    return NULL;
}

GLuint compile_shader_file(GLenum type, const char *path)
{
    char *body = resources_read_text(path);
    if (!body) {
        SDL_Log("Cannot read shader file: %s", path);
        return 0;
    }
    // Shader directory — base for relative #include paths
    char dir[512] = "";
    const char *slash = strrchr(path, '/');
    if (slash) {
        size_t dl = (size_t)(slash - path) + 1;
        if (dl >= sizeof dir) dl = sizeof dir - 1;
        memcpy(dir, path, dl);
        dir[dl] = 0;
    }
    char *expanded = shader_expand_includes(body, dir, 0);
    free(body);
    if (!expanded) {
        SDL_Log("Shader include expansion OOM: %s", path);
        return 0;
    }
    body = expanded;
    const char *version = shader_version(type);
    size_t vn = strlen(version), bn = strlen(body);
    if (bn > SIZE_MAX - vn - 1) { free(body); return 0; }
    size_t n = vn + bn + 1;
    char *buf = malloc(n);
    if (!buf) { free(body); return 0; }
    snprintf(buf, n, "%s%s", version, body);
    free(body);
    const char *src = buf;

    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(sh, sizeof log, NULL, log);
        SDL_Log("Shader compile error (%s): %s", path, log);
        glDeleteShader(sh);
        free(buf);
        return 0;
    }
    free(buf);
    return sh;
}

// Compiles an inline body with the same dialect prefix the file loader uses.
static GLuint compile_shader_string(GLenum type, const char *body)
{
    const char *version = shader_version(type);
    size_t n = strlen(version) + strlen(body) + 1;
    char *buf = malloc(n);
    if (!buf) return 0;
    snprintf(buf, n, "%s%s", version, body);
    const char *src = buf;

    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(sh, sizeof log, NULL, log);
        SDL_Log("GLSL probe: %s compile failed: %s",
                type == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
        glDeleteShader(sh);
        free(buf);
        return 0;
    }
    free(buf);
    return sh;
}

int shader_dialect_supported(void)
{
    // One probe per run: the driver does not change mid-session, and the
    // result is queried from several places (boot message, map load).
    static int cached = -1;
    if (cached >= 0) return cached;
    if (getenv("FR_GL_PROBE_FAIL")) return (cached = 0);  // test knob

    static const char *vs_body =
        "in vec3 aPos;\n"
        "void main(){ gl_Position = vec4(aPos, 1.0); }\n";
    static const char *fs_body =
        "out vec4 FragColor;\n"
        "void main(){ FragColor = vec4(1.0); }\n";
    GLuint vs = compile_shader_string(GL_VERTEX_SHADER, vs_body);
    GLuint fs = compile_shader_string(GL_FRAGMENT_SHADER, fs_body);
    int ok = 0;
    if (vs && fs) {
        GLuint prog = glCreateProgram();
        glAttachShader(prog, vs);
        glAttachShader(prog, fs);
        glLinkProgram(prog);
        GLint linked = 0;
        glGetProgramiv(prog, GL_LINK_STATUS, &linked);
        if (!linked) {
            char log[512];
            glGetProgramInfoLog(prog, sizeof log, NULL, log);
            SDL_Log("GLSL probe: link failed: %s", log);
        }
        ok = linked ? 1 : 0;
        glDeleteProgram(prog);
    }
    if (vs) glDeleteShader(vs);
    if (fs) glDeleteShader(fs);
    return (cached = ok);
}

GLuint make_program(const char *vs_path, const char *fs_path)
{
    // Cache lives in resources.c: one program per file pair, refcounted.
    return resources_program(vs_path, fs_path);
}

GLuint make_compute_program(const char *cs_path)
{
    GLuint cs = compile_shader_file(GL_COMPUTE_SHADER, cs_path);
    if (!cs) return 0;
    GLuint prog = glCreateProgram();
    glAttachShader(prog, cs);
    glLinkProgram(prog);
    glDeleteShader(cs);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(prog, sizeof log, NULL, log);
        SDL_Log("Compute link error (%s): %s", cs_path, log);
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

void shader_on_reshade(ReshadeFn fn)
{
    resources_on_reshade(fn);
}

int shader_hot_reload(void)
{
    return resources_shader_reload();
}
