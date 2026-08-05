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

GLuint compile_shader_file(GLenum type, const char *path)
{
    char *body = resources_read_text(path);
    if (!body) {
        SDL_Log("Cannot read shader file: %s", path);
        return 0;
    }
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
