// ============================================================================
//  shader.c — чтение файлов шейдеров и сборка программ
// ----------------------------------------------------------------------------
//  Файлы читаются через менеджер ресурсов (resources_read_text: диск ->
//  ../диск -> вшитые данные). Программы КЕШИРУЮТСЯ по паре (vs, fs) в
//  resources.c — один и тот же шейдер компилируется один раз, сколько бы
//  узлов его ни использовали (см. resources_program).
// ============================================================================
#include "shader.h"

#include "resources.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Префикс GLSL для текущей платформы
static const char *shader_version(GLenum type)
{
#ifdef USE_GLES_PATH
    if (type == GL_FRAGMENT_SHADER)
        return "#version 300 es\nprecision highp float;\n";  // GLES: точность обязательна
    return "#version 300 es\n";
#else
    (void)type;
    return "#version 330 core\n";
#endif
}

// Разворачивает #include "файл" — рекурсивно подставляет содержимое
// (глубина до 8, циклы не проверяются). Пути — относительно директории
// шейдера. Так вся сцена делит общий код (shaders/snow.glsl, csm.glsl).
static void buf_grow(char **buf, size_t *len, size_t *cap, const char *s)
{
    size_t sl = strlen(s);
    while (*len + sl + 1 > *cap)
        *cap = *cap ? *cap * 2 : 4096;
    *buf = realloc(*buf, *cap);
    memcpy(*buf + *len, s, sl);
    *len += sl;
    (*buf)[*len] = 0;
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
        buf_grow(&out, &len, &cap, "");   // гарантированная строка
        if (depth < 8 && !strncmp(line, "#include ", 9) &&
            sscanf(line, "#include \"%255[^\"]\"", inc) == 1) {
            char path[512];
            snprintf(path, sizeof path, "%s%s", dir, inc);
            char *body = resources_read_text(path);
            if (body) {
                char *exp = shader_expand_includes(body, dir, depth + 1);
                buf_grow(&out, &len, &cap, exp);
                free(exp);
                free(body);
            } else {
                // файла нет — оставляем как есть (ошибка будет в GLSL-логе)
                buf_grow(&out, &len, &cap, line);
            }
        } else {
            buf_grow(&out, &len, &cap, line);
        }
        buf_grow(&out, &len, &cap, "\n");
        p = eol ? eol + 1 : p + ll;
    }
    return out;
}

GLuint compile_shader_file(GLenum type, const char *path)
{
    char *body = resources_read_text(path);
    if (!body) {
        SDL_Log("Cannot read shader file: %s", path);
        return 0;
    }
    // Директория шейдера — база для относительных #include
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
    body = expanded;
    const char *version = shader_version(type);
    size_t n = strlen(version) + strlen(body) + 1;
    char *buf = malloc(n);
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

GLuint make_program(const char *vs_path, const char *fs_path)
{
    // Кеш в resources.c: одна программа на пару файлов, ссылки считаются.
    return resources_program(vs_path, fs_path);
}

void shader_on_reshade(ReshadeFn fn)
{
    resources_on_reshade(fn);
}

int shader_hot_reload(void)
{
    return resources_shader_reload();
}
