// ============================================================================
//  utf8.h — small inline helpers for UTF-8 (text input/output)
// ----------------------------------------------------------------------------
//  Used by the console (line input, per-character Backspace) and the HUD
//  (glyph rasterization, text width). No allocations, no dependencies.
// ============================================================================
#ifndef UTF8_H
#define UTF8_H

// Length of a UTF-8 sequence from its first byte (1..4)
static inline int utf8_len(unsigned char c)
{
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

// Decodes a codepoint from s (s must hold at least utf8_len(s[0]) bytes
// before the NUL). Returns the byte count. A truncated/broken sequence —
// cp = the first byte as-is.
static inline int utf8_decode(const char *s, int *cp)
{
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80) { *cp = c; return 1; }
    int l = utf8_len(c);
    if (l == 2 && (s[1] & 0xC0) == 0x80) {
        *cp = ((c & 0x1F) << 6) | (s[1] & 0x3F);
    } else if (l == 3 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        *cp = ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
    } else if (l == 4 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 &&
               (s[3] & 0xC0) == 0x80) {
        *cp = ((c & 0x07) << 18) | ((s[1] & 0x3F) << 12)
            | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
    } else {
        *cp = c;
    }
    return l;
}

// Index of the character start BEFORE pos (for ← and Backspace)
static inline int utf8_prev(const char *s, int pos)
{
    int i = pos - 1;
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) i--;
    return i;
}

#endif
