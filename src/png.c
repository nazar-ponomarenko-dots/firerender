// ============================================================================
//  png.c — saving an RGB frame to PNG (via miniz: deflate + crc32)
// ----------------------------------------------------------------------------
//  Writes an 8-bit truecolor PNG (color type 2) without filters. Only needed
//  for screenshots — hence a mini implementation without stb_image_write.
// ============================================================================
#include "png.h"

#include "miniz.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Helper: big-endian uint32 into a buffer
static void put_u32(unsigned char *b, unsigned int v)
{
    b[0] = (unsigned char)(v >> 24);
    b[1] = (unsigned char)(v >> 16);
    b[2] = (unsigned char)(v >> 8);
    b[3] = (unsigned char)v;
}

// Writes a PNG chunk: length, type, data, crc32
static void write_chunk(FILE *f, const char *type, const unsigned char *data,
                        unsigned int len)
{
    unsigned char hdr[8];
    put_u32(hdr, len);
    memcpy(hdr + 4, type, 4);
    fwrite(hdr, 1, 8, f);
    if (len > 0) fwrite(data, 1, len, f);
    unsigned int crc = (unsigned int)mz_crc32(MZ_CRC32_INIT,
                                              (const unsigned char *)type, 4);
    if (len > 0)
        crc = (unsigned int)mz_crc32(crc, data, len);
    unsigned char crcb[4];
    put_u32(crcb, crc);
    fwrite(crcb, 1, 4, f);
}

// Saves the RGB frame (w*h*3 bytes, rows top to bottom) to PNG.
// 0 on success, -1 on error.
int png_save(const char *path, int w, int h, const unsigned char *rgb)
{
    if (w <= 0 || h <= 0 || !rgb) return -1;
    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    // Raw scanlines: each row is a filter byte (0) + RGB pixels
    if ((size_t)w > (SIZE_MAX - 1) / 3) { fclose(f); return -1; }
    size_t row_bytes = (size_t)w * 3;
    if ((size_t)h > SIZE_MAX / (row_bytes + 1)) { fclose(f); return -1; }
    size_t raw_len = (row_bytes + 1) * (size_t)h;
    unsigned char *raw = malloc(raw_len);
    if (!raw) { fclose(f); return -1; }
    for (int y = 0; y < h; y++) {
        unsigned char *dst = raw + (size_t)y * (row_bytes + 1);
        dst[0] = 0;                       // filter: none
        memcpy(dst + 1, rgb + (size_t)y * row_bytes, row_bytes);
    }

    // IHDR
    unsigned char ihdr[13];
    put_u32(ihdr, (unsigned int)w);
    put_u32(ihdr + 4, (unsigned int)h);
    ihdr[8] = 8;        // bit depth
    ihdr[9] = 2;        // color type: truecolor RGB
    ihdr[10] = 0;       // compression
    ihdr[11] = 0;       // filter
    ihdr[12] = 0;       // interlace

    static const unsigned char sig[8] = {
        0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'
    };
    fwrite(sig, 1, 8, f);
    write_chunk(f, "IHDR", ihdr, 13);

    // IDAT: zlib-compressed scanlines (miniz: mz_compress — a zlib stream)
    mz_ulong clen = mz_compressBound((mz_ulong)raw_len);
    unsigned char *comp = malloc(clen);
    if (!comp) {
        free(raw);
        fclose(f);
        return -1;
    }
    int rc = mz_compress(comp, &clen, raw, (mz_ulong)raw_len);
    free(raw);
    if (rc != MZ_OK) {
        free(comp);
        fclose(f);
        return -1;
    }
    write_chunk(f, "IDAT", comp, (unsigned int)clen);
    free(comp);

    write_chunk(f, "IEND", NULL, 0);
    fclose(f);
    return 0;
}
