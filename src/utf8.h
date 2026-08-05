// ============================================================================
//  utf8.h — маленькие inline-помощники для UTF-8 (ввод/вывод текста)
// ----------------------------------------------------------------------------
//  Используются консолью (ввод строки, Backspace по символам) и HUD
//  (растеризация глифов, ширина текста). Без аллокаций и зависимостей.
// ============================================================================
#ifndef UTF8_H
#define UTF8_H

// Длина UTF-8-последовательности по первому байту (1..4)
static inline int utf8_len(unsigned char c)
{
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

// Декодирует codepoint из s (s должна содержать не менее utf8_len(s[0]) байт
// до NUL). Возвращает число байт. Обрезанная/битая последовательность —
// cp = первый байт как есть.
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

// Индекс начала символа ПЕРЕД pos (для ← и Backspace)
static inline int utf8_prev(const char *s, int pos)
{
    int i = pos - 1;
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) i--;
    return i;
}

#endif
