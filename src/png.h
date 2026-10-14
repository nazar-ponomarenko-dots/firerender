// ============================================================================
//  png.h — saving an RGB frame to PNG (see png.c)
// ============================================================================
#ifndef PNG_H
#define PNG_H

// Saves the RGB frame (w*h*3 bytes, rows top to bottom) to PNG at path.
// 0 on success, -1 on error (missing file/memory).
int png_save(const char *path, int w, int h, const unsigned char *rgb);

#endif
