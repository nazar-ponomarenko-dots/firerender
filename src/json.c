// ============================================================================
//  json.c — мини-парсер JSON: токенизация + дерево узлов в одной арене
// ----------------------------------------------------------------------------
//  Значения кладутся в растущий массив узлов (индексы вместо указателей:
//  realloc не ломает связи), строки — в отдельную арену. Поддерживаются
//  escape-последовательности, в т.ч. \uXXXX с суррогатными парами
//  (кириллица в именах). Зависимостей нет — чистый C + SDL_Log.
// ============================================================================
#include "json.h"

#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>

#define JSON_MAX_DEPTH 64

// --- Арены: узлы и строки -----------------------------------------------------
static JsonNode *s_nodes;
static size_t s_ncount, s_ncap;
static char *s_strs;
static size_t s_scount, s_scap;

// --- Курсор парсера -----------------------------------------------------------
static const char *s_p, *s_end;
static int s_depth;

static void skip_ws(void)
{
    while (s_p < s_end && (*s_p == ' ' || *s_p == '\t' ||
                           *s_p == '\n' || *s_p == '\r'))
        s_p++;
}

static JsonNode *new_node(void)
{
    if (s_ncount >= s_ncap) {
        size_t cap = s_ncap ? s_ncap * 2 : 256;
        JsonNode *nn = realloc(s_nodes, cap * sizeof(JsonNode));
        if (!nn) return NULL;
        s_nodes = nn;
        s_ncap = cap;
    }
    JsonNode *n = &s_nodes[s_ncount++];
    memset(n, 0, sizeof *n);
    n->child = -1;
    n->next = -1;
    return n;
}

// Копия строки в арену s_strs (NULL-terminated)
static const char *str_dup(const char *s, size_t len)
{
    if (s_scount + len + 1 > s_scap) {
        size_t cap = s_scap ? s_scap * 2 : 1024;
        while (cap < s_scount + len + 1) cap *= 2;
        char *nn = realloc(s_strs, cap);
        if (!nn) return NULL;
        s_strs = nn;
        s_scap = cap;
    }
    char *out = s_strs + s_scount;
    memcpy(out, s, len);
    out[len] = '\0';
    s_scount += len + 1;
    return out;
}

// --- Вспомогательные лексемы --------------------------------------------------

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int parse_hex4(void)
{
    int v = 0;
    for (int i = 0; i < 4; i++) {
        if (s_p >= s_end) return -1;
        int h = hex_val(*s_p++);
        if (h < 0) return -1;
        v = v * 16 + h;
    }
    return v;
}

// Строка в кавычках -> арена s_strs (с раскрытыми escape). NULL — ошибка.
static const char *parse_string(void)
{
    s_p++;   // открывающая кавычка
    size_t cap = 64, len = 0;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    for (;;) {
        if (s_p >= s_end) { free(buf); return NULL; }
        unsigned char c = (unsigned char)*s_p;
        if (c == '"') { s_p++; break; }
        s_p++;   // курсор вперёд: для обычного символа и после '\\'
        if (c == '\\') {
            if (s_p >= s_end) { free(buf); return NULL; }
            char e = *s_p++;
            switch (e) {
            case '"': case '\\': case '/':
                c = (unsigned char)e;
                break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case 'u': {
                int cp = parse_hex4();
                if (cp < 0) { free(buf); return NULL; }
                // Суррогатная пара: старшая половина + \uXXXX младшей
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    if (s_p + 1 < s_end && s_p[0] == '\\' && s_p[1] == 'u') {
                        s_p += 2;
                        int lo = parse_hex4();
                        if (lo < 0 || lo < 0xDC00 || lo > 0xDFFF)
                            { free(buf); return NULL; }
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else { free(buf); return NULL; }
                }
                // Кодовая точка -> UTF-8 (1/2/3/4 байта)
                if (cp < 0x80) {
                    c = (unsigned char)cp;
                } else if (cp < 0x800) {
                    if (len + 2 >= cap) { cap *= 2; buf = realloc(buf, cap); }
                    buf[len++] = (char)(0xC0 | (cp >> 6));
                    c = (unsigned char)(0x80 | (cp & 0x3F));
                } else if (cp < 0x10000) {
                    if (len + 3 >= cap) { cap *= 2; buf = realloc(buf, cap); }
                    buf[len++] = (char)(0xE0 | (cp >> 12));
                    buf[len++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    c = (unsigned char)(0x80 | (cp & 0x3F));
                } else {   // суррогатная пара: до 0x10FFFF — 4 байта
                    if (len + 4 >= cap) { cap *= 2; buf = realloc(buf, cap); }
                    buf[len++] = (char)(0xF0 | (cp >> 18));
                    buf[len++] = (char)(0x80 | ((cp >> 12) & 0x3F));
                    buf[len++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    c = (unsigned char)(0x80 | (cp & 0x3F));
                }
                break;
            }
            default: free(buf); return NULL;   // неизвестный escape
            }
        }
        if (len + 1 >= cap) { cap *= 2; buf = realloc(buf, cap); }
        buf[len++] = (char)c;
    }
    const char *out = str_dup(buf, len);
    free(buf);
    return out;
}

// --- Значения ----------------------------------------------------------------

// Дословное совпадение (true/false/null)
static int match_lit(const char *lit)
{
    size_t n = strlen(lit);
    if ((size_t)(s_end - s_p) < n || memcmp(s_p, lit, n) != 0) return 0;
    s_p += n;
    return 1;
}

static JsonNode *parse_value(const char *key)
{
    if (++s_depth > JSON_MAX_DEPTH) { s_depth--; return NULL; }
    skip_ws();
    if (s_p >= s_end) { s_depth--; return NULL; }
    JsonNode *n = new_node();
    if (!n) { s_depth--; return NULL; }
    n->key = key;
    char c = *s_p;

    if (c == '{') {
        n->type = JSON_OBJECT;
        s_p++;
        skip_ws();
        if (s_p < s_end && *s_p == '}') { s_p++; s_depth--; return n; }
        for (;;) {
            skip_ws();
            if (s_p >= s_end || *s_p != '"') { s_depth--; return NULL; }
            const char *k = parse_string();
            if (!k) { s_depth--; return NULL; }
            skip_ws();
            if (s_p >= s_end || *s_p != ':') { s_depth--; return NULL; }
            s_p++;
            JsonNode *v = parse_value(k);
            if (!v) { s_depth--; return NULL; }
            int vi = (int)(v - s_nodes);
            if (n->child < 0) n->child = vi;
            else {
                JsonNode *last = &s_nodes[n->child];
                while (last->next >= 0) last = &s_nodes[last->next];
                last->next = vi;
            }
            skip_ws();
            if (s_p < s_end && *s_p == ',') { s_p++; continue; }
            if (s_p < s_end && *s_p == '}') { s_p++; break; }
            s_depth--; return NULL;
        }
    } else if (c == '[') {
        n->type = JSON_ARRAY;
        s_p++;
        skip_ws();
        if (s_p < s_end && *s_p == ']') { s_p++; s_depth--; return n; }
        for (;;) {
            JsonNode *v = parse_value(NULL);
            if (!v) { s_depth--; return NULL; }
            int vi = (int)(v - s_nodes);
            if (n->child < 0) n->child = vi;
            else {
                JsonNode *last = &s_nodes[n->child];
                while (last->next >= 0) last = &s_nodes[last->next];
                last->next = vi;
            }
            skip_ws();
            if (s_p < s_end && *s_p == ',') { s_p++; continue; }
            if (s_p < s_end && *s_p == ']') { s_p++; break; }
            s_depth--; return NULL;
        }
    } else if (c == '"') {
        n->type = JSON_STRING;
        n->sval = parse_string();
        if (!n->sval) { s_depth--; return NULL; }
    } else if (c == 't') {
        if (!match_lit("true")) { s_depth--; return NULL; }
        n->type = JSON_BOOL; n->bval = 1;
    } else if (c == 'f') {
        if (!match_lit("false")) { s_depth--; return NULL; }
        n->type = JSON_BOOL; n->bval = 0;
    } else if (c == 'n') {
        if (!match_lit("null")) { s_depth--; return NULL; }
        n->type = JSON_NULL;
    } else if (c == '-' || (c >= '0' && c <= '9')) {
        char *endp = NULL;
        n->nval = strtod(s_p, &endp);
        if (endp == s_p) { s_depth--; return NULL; }
        s_p = endp;
        n->type = JSON_NUMBER;
    } else {
        s_depth--; return NULL;
    }
    s_depth--;
    return n;
}

// --- Публичный API ------------------------------------------------------------

const JsonNode *json_parse(const char *text, size_t len)
{
    json_free();
    // Арены заранее выращены до МАКСИМУМА: во время парсинга realloc НЕ
    // вызывается. Иначе выданные ранее указатели (sval/ключи, родительские
    // узлы) инвалидируются — use-after-free: на больших map.json падало
    // (30 деревьев проскакивали, 60 — краш в parse_value).
    size_t est_nodes = len / 2 + 32;      // узел почти на каждые 2 байта
    s_nodes = malloc(est_nodes * sizeof(JsonNode));
    if (!s_nodes) return NULL;
    s_ncap = est_nodes;
    s_strs = malloc(len + 1);             // суммарная длина строк ≤ len
    if (!s_strs) { json_free(); return NULL; }
    s_scap = len + 1;
    s_p = text;
    s_end = text + len;
    s_depth = 0;
    JsonNode *root = parse_value(NULL);
    if (!root) {
        SDL_Log("JSON parse error near byte %ld", (long)(s_p - text));
        json_free();
        return NULL;
    }
    skip_ws();
    if (s_p != s_end) {   // мусор после корневого значения
        SDL_Log("JSON parse error: trailing data at byte %ld", (long)(s_p - text));
        json_free();
        return NULL;
    }
    return root;
}

void json_free(void)
{
    free(s_nodes);
    s_nodes = NULL;
    s_ncount = s_ncap = 0;
    free(s_strs);
    s_strs = NULL;
    s_scount = s_scap = 0;
}

const JsonNode *json_find(const JsonNode *obj, const char *key)
{
    if (!obj || obj->type != JSON_OBJECT) return NULL;
    for (int i = obj->child; i >= 0; i = s_nodes[i].next)
        if (s_nodes[i].key && strcmp(s_nodes[i].key, key) == 0)
            return &s_nodes[i];
    return NULL;
}

const JsonNode *json_array_get(const JsonNode *arr, int idx)
{
    if (!arr || arr->type != JSON_ARRAY || idx < 0) return NULL;
    int i = arr->child;
    while (i >= 0 && idx-- > 0) i = s_nodes[i].next;
    return i >= 0 ? &s_nodes[i] : NULL;
}

int json_array_len(const JsonNode *arr)
{
    if (!arr || arr->type != JSON_ARRAY) return 0;
    int cnt = 0;
    for (int i = arr->child; i >= 0; i = s_nodes[i].next) cnt++;
    return cnt;
}

double json_num(const JsonNode *n, double def)
{
    return (n && n->type == JSON_NUMBER) ? n->nval : def;
}

const char *json_str(const JsonNode *n, const char *def)
{
    return (n && n->type == JSON_STRING) ? n->sval : def;
}

int json_bool(const JsonNode *n, int def)
{
    return (n && n->type == JSON_BOOL) ? n->bval : def;
}

const JsonNode *json_child_first(const JsonNode *parent)
{
    if (!parent) return NULL;
    return parent->child >= 0 ? &s_nodes[parent->child] : NULL;
}

const JsonNode *json_child_next(const JsonNode *parent, const JsonNode *cur)
{
    (void)parent;
    if (!cur) return NULL;
    return cur->next >= 0 ? &s_nodes[cur->next] : NULL;
}
