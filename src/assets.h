// ============================================================================
//  assets.h — загрузка картинок и создание текстур
// ----------------------------------------------------------------------------
//  stb_image — single-header загрузчик PNG/JPEG/... (публичное достояние),
//  реализация включается ровно в одном .c-файле (assets.c).
// ============================================================================
#ifndef ASSETS_H
#define ASSETS_H
#include "gl_loader.h"

// Загружает картинку через stb_image (ищет рядом с проектом и на уровень
// выше). channels: STBI_rgb / STBI_rgba. Возвращает указатель или NULL.
unsigned char *load_img(const char *name, int *w, int *h, int channels);

// Загружает данные в 2D-текстуру.
//   format: GL_RGB / GL_RGBA / GL_RED (высоты)
//   wrap:   GL_REPEAT (тайл земли) или GL_CLAMP_TO_EDGE
//   linear: 1 = LINEAR, 0 = NEAREST. По умолчанию движок НЕ сглаживает
//           текстуры: пиксельная текстура остаётся пиксельной. LINEAR
//           используют только текстуры-данные (высоты, normal map) —
//           их "пиксельность" видна была бы мерцанием.
GLuint make_texture(int w, int h, const void *data, GLenum format,
                    int wrap, int linear);

#endif
