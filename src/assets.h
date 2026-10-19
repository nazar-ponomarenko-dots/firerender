// ============================================================================
//  assets.h — loading images and creating textures
// ----------------------------------------------------------------------------
//  stb_image — single-header PNG/JPEG/... loader (public domain),
//  its implementation is enabled in exactly one .c file (assets.c).
// ============================================================================
#ifndef ASSETS_H
#define ASSETS_H
#include "gl_loader.h"

// Loads an image via stb_image (looks next to the project and one level up).
// channels: STBI_rgb / STBI_rgba. Returns a pointer or NULL.
unsigned char *load_img(const char *name, int *w, int *h, int channels);

// Uploads data into a 2D texture.
//   format: GL_RGB / GL_RGBA / GL_RED (heights)
//   wrap:   GL_REPEAT (ground tiling) or GL_CLAMP_TO_EDGE
//   linear: 1 = LINEAR, 0 = NEAREST. By default the engine does NOT filter
//           textures: a pixel-art texture stays pixelated. LINEAR is used
//           only by data textures (heights, normal map) —
//           their "pixelness" would otherwise show as flicker.
GLuint make_texture(int w, int h, const void *data, GLenum format,
                    int wrap, int linear);

#endif
