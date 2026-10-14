// ============================================================================
//  assets.c — texture creation; image loading goes through resources.c
// ----------------------------------------------------------------------------
//  stb_image (single-header PNG/JPEG/... loader) is included in
//  resources.c (STB_IMAGE_IMPLEMENTATION lives there). Here — only the
//  load_img() wrapper over the resource cache and GL texture creation.
// ============================================================================
#include "assets.h"

#include "resources.h"

#include <SDL3/SDL.h>
#include <math.h>

unsigned char *load_img(const char *name, int *w, int *h, int channels)
{
    // Cache lives in resources.c: disk -> ../disk -> embedded data. Do NOT free
    // the returned pixels — the cache owns them (they live until resources_shutdown).
    return resources_image(name, w, h, channels);
}

GLuint make_texture(int w, int h, const void *data, GLenum format,
                    int wrap, int linear)
{
    GLuint id;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                    linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    GLint internal = format == GL_RGBA ? GL_RGBA8
                   : format == GL_RED ? GL_R8
                   : GL_RGB8;
    glTexImage2D(GL_TEXTURE_2D, 0, internal, w, h, 0, format,
                 GL_UNSIGNED_BYTE, data);
    return id;
}
