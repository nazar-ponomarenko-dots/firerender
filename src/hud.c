// ============================================================================
//  hud.c — дебаг-HUD: панель с позицией игрока, FPS, скоростью и прочим
// ============================================================================
#include "hud.h"

#include "audio.h"
#include "cvars.h"
#include "assets.h"
#include "gl_loader.h"
#include "linalg.h"
#include "resources.h"
#include "shader.h"
#include "shadow.h"
#include "terrain.h"
#include "utf8.h"
#include "vegetation.h"
#include "voxfog.h"
#include "world.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

static float hud_mobile_scale(int vw, int vh);

// TTF-шрифт Inter 18pt Black (img/Inter_18pt-Black.ttf): атлас растеризуется
// один раз при старте через stb_truetype, глифы ставятся по метрикам шрифта
// (advance/беринг) — строки не "летят", как с фиксированной сеткой 8x8.
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#define HUD_LINES 13
#define HUD_LINE_LEN 96
#define HUD_MAX_GLYPHS 1536

// Атлас: 1024x256 покрывает ASCII (95) + кириллицу (66) Inter Black 26px.
#define FONT_ATLAS_W 1024
#define FONT_ATLAS_H 256
#define FONT_PX 26                  // высота растеризации глифов
#define FONT_PAD 2                  // зазор между глифами (края билинейного)
#define FONT_ASCII_FIRST 32
#define FONT_ASCII_LAST 126
#define FONT_N_ASCII (FONT_ASCII_LAST - FONT_ASCII_FIRST + 1)   // 95
#define FONT_N_EXTRA 71              // Ё, ё, А..я (66) + — ← ↑ → ↓ (5)
#define FONT_N_GLYPHS (FONT_N_ASCII + FONT_N_EXTRA)             // 166

// Атлас HUD: 1024x256 при 26px, консоль: 512x256 при 15px.
#define FONT_ATLAS_W 1024
#define FONT_ATLAS_H 256
#define FONT_PX 26                  // высота растеризации глифов (HUD)
#define FONT_SMALL_W 512
#define FONT_SMALL_H 256
#define FONT_SMALL_PX 15            // высота растеризации (консоль)
#define FONT_BIG_W 1024             // заголовки меню: 56px
#define FONT_BIG_H 512
#define FONT_BIG_PX 56
#define FONT_PAD 2                  // зазор между глифами (края билинейного)

typedef struct {
    float w, h;          // размер битмапа глифа, px
    float xoff, yoff;    // смещение от базовой линии (y — вниз от неё)
    float advance;       // шаг до следующего глифа, px
    float u0, v0, u1, v1; // кусок атласа
} HudGlyph;

// Шрифт: атлас + глифы + метрики. Два экземпляра: HUD (26px) и консоль (15px).
typedef struct {
    HudGlyph glyphs[FONT_N_GLYPHS];
    GLuint tex;
    float scale;         // stbtt-масштаб
    float ascent_px;     // базовая линия от верха строки (вниз — glyph top)
    float descent_px;    // сколько под базовой линией
    float line_h_px;     // высота строки
    int ok;
} HudFont;

static stbtt_fontinfo g_font;        // данные шрифта (общие для обоих атласов)
static int s_cp_slot[0x2300];        // codepoint -> слот глифа, -1 = нет
                                     // (до 0x22FF: покрывает типографику ←↑→↓)
static HudFont s_font;               // 26px — HUD
static HudFont s_font_small;         // 15px — консоль
static HudFont s_font_big;           // 56px — заголовки меню
static int g_font_ok = 0;
static HudParams *g_hud = NULL;      // экземпляр HUD — для общих отрисовок (консоль)

// Слот глифа по codepoint'у (-1 = глифа нет)
static int font_slot(int cp)
{
    return (cp >= 0 && cp < (int)(sizeof s_cp_slot / sizeof s_cp_slot[0]))
               ? s_cp_slot[cp] : -1;
}

// Карта codepoint -> слот: ASCII 32..126, затем кириллица (Ё, ё, А..я),
// затем типографика (—, ← ↑ → ↓). Общая для всех атласов.
static void font_build_slots(void)
{
    for (int i = 0; i < (int)(sizeof s_cp_slot / sizeof s_cp_slot[0]); i++)
        s_cp_slot[i] = -1;
    for (int c = FONT_ASCII_FIRST; c <= FONT_ASCII_LAST; c++)
        s_cp_slot[c] = c - FONT_ASCII_FIRST;
    int slot = FONT_N_ASCII;
    s_cp_slot[0x401] = slot++;          // Ё
    s_cp_slot[0x451] = slot++;          // ё
    for (int c = 0x410; c <= 0x44F; c++)
        s_cp_slot[c] = slot++;          // А..я
    s_cp_slot[0x2014] = slot++;         // — длинное тире
    s_cp_slot[0x2190] = slot++;         // ←
    s_cp_slot[0x2191] = slot++;         // ↑
    s_cp_slot[0x2192] = slot++;         // →
    s_cp_slot[0x2193] = slot++;         // ↓
}

// Читает TTF через менеджер ресурсов (диск -> ../диск -> вшитые данные).
// Данные остаются жить (stbtt_fontinfo хранит указатель на них).
static unsigned char *hud_read_font(unsigned int *sz)
{
    return resources_read_file("img/Inter_18pt-Black.ttf", sz);
}

// Растеризует один codepoint в атлас (при переполнении — копия '?')
static void font_rasterize(HudFont *f, int cp, unsigned char *atlas,
                           int atlas_w, int atlas_h, int pad,
                           int *cx, int *cy, int *row_h)
{
    int slot = font_slot(cp);
    if (slot < 0) slot = font_slot('?');
    int gi = stbtt_FindGlyphIndex(&g_font, cp);
    if (!gi) gi = stbtt_FindGlyphIndex(&g_font, '?');
    int adv = 0, lsb = 0;
    stbtt_GetCodepointHMetrics(&g_font, cp, &adv, &lsb);
    int x0, y0, x1, y1;
    stbtt_GetGlyphBitmapBox(&g_font, gi, f->scale, f->scale, &x0, &y0, &x1, &y1);
    int w = x1 - x0, h = y1 - y0;
    if (w < 0 || h < 0) w = h = 0;
    if (*cx + w + pad > atlas_w) {   // строка кончилась
        *cx = pad;
        *cy += *row_h + pad;
        *row_h = 0;
    }
    if (*cy + h + pad > atlas_h) {   // атлас кончился — '?'
        if (w > 0 && h > 0) f->glyphs[slot] = f->glyphs[font_slot('?')];
        return;
    }
    if (w > 0 && h > 0)
        stbtt_MakeGlyphBitmap(&g_font, atlas + *cy * atlas_w + *cx,
                              w, h, atlas_w, f->scale, f->scale, gi);
    HudGlyph *g = &f->glyphs[slot];
    g->w = (float)w;
    g->h = (float)h;
    g->xoff = (float)x0;
    g->yoff = (float)y0;                  // от базовой линии, y вниз
    g->advance = (float)adv * f->scale;
    g->u0 = (float)*cx / atlas_w;
    g->v0 = (float)*cy / atlas_h;
    g->u1 = (float)(*cx + w) / atlas_w;
    g->v1 = (float)(*cy + h) / atlas_h;
    *cx += w + pad;
    if (h > *row_h) *row_h = h;
}

// Растеризует атлас шрифта: ASCII + кириллица + типографика.
static void font_rasterize_all(HudFont *f, int atlas_w, int atlas_h, int pad)
{
    unsigned char *atlas = calloc((size_t)atlas_w * atlas_h, 1);
    if (!atlas) return;            // f->ok останется 0: растеризовать некуда
    int cx = pad, cy = pad, row_h = 0;
    for (int c = FONT_ASCII_FIRST; c <= FONT_ASCII_LAST; c++)
        font_rasterize(f, c, atlas, atlas_w, atlas_h, pad, &cx, &cy, &row_h);
    font_rasterize(f, 0x401, atlas, atlas_w, atlas_h, pad, &cx, &cy, &row_h);   // Ё
    for (int c = 0x410; c <= 0x44F; c++)
        font_rasterize(f, c, atlas, atlas_w, atlas_h, pad, &cx, &cy, &row_h);   // А..я
    font_rasterize(f, 0x451, atlas, atlas_w, atlas_h, pad, &cx, &cy, &row_h);   // ё
    font_rasterize(f, 0x2014, atlas, atlas_w, atlas_h, pad, &cx, &cy, &row_h);  // —
    font_rasterize(f, 0x2190, atlas, atlas_w, atlas_h, pad, &cx, &cy, &row_h);  // ←
    font_rasterize(f, 0x2191, atlas, atlas_w, atlas_h, pad, &cx, &cy, &row_h);  // ↑
    font_rasterize(f, 0x2192, atlas, atlas_w, atlas_h, pad, &cx, &cy, &row_h);  // →
    font_rasterize(f, 0x2193, atlas, atlas_w, atlas_h, pad, &cx, &cy, &row_h);  // ↓
    f->tex = make_texture(atlas_w, atlas_h, atlas, GL_RED,
                          GL_CLAMP_TO_EDGE, 1);
    free(atlas);
    f->ok = 1;
}

// Инициализирует шрифт заданного размера: метрики + атлас.
static void font_init(HudFont *f, float px, int atlas_w, int atlas_h, int pad)
{
    memset(f, 0, sizeof *f);
    f->scale = stbtt_ScaleForPixelHeight(&g_font, px);
    int ascent = 0, descent = 0, line_gap = 0;
    stbtt_GetFontVMetrics(&g_font, &ascent, &descent, &line_gap);
    f->ascent_px = ascent * f->scale;
    f->descent_px = -descent * f->scale;   // descent в stbtt отрицательный
    f->line_h_px = (ascent - descent + line_gap) * f->scale;
    font_rasterize_all(f, atlas_w, atlas_h, pad);
    SDL_Log("HUD: font %.0fpx, atlas %dx%d (%d glyphs), line %.0f px",
            px, atlas_w, atlas_h, FONT_N_GLYPHS, f->line_h_px);
}

// Растеризует оба шрифта (HUD 26px и консоль 15px) в общий атлас-кеш.
static void hud_font_init(void)
{
    unsigned int sz = 0;
    unsigned char *data = hud_read_font(&sz);
    if (!data) {
        SDL_Log("HUD: no Inter_18pt-Black.ttf (disk)");
        return;
    }
    if (!stbtt_InitFont(&g_font, data, 0)) {
        SDL_Log("HUD: bad font data");
        free(data);
        return;
    }
    font_build_slots();
    // Pad: зазор между глифами в атласе. Для крупного шрифта больше —
    // при билинейной фильтрации края букв не хватают соседние глифы.
    font_init(&s_font, FONT_PX, FONT_ATLAS_W, FONT_ATLAS_H, 2);
    font_init(&s_font_small, FONT_SMALL_PX, FONT_SMALL_W, FONT_SMALL_H, 2);
    font_init(&s_font_big, FONT_BIG_PX, FONT_BIG_W, FONT_BIG_H, 5);
    g_font_ok = 1;
}

// --- Общий рендер текста/прямоугольников (для консоли и меню) -----------------
// Шрифт, атласы, программа и буфер живут ВСЮ сессию (hud_shared_init/
// hud_shared_cleanup) и не зависят от загрузки мира: меню и консоль
// работают даже до загрузки карты.
static GLuint s_shared_prog = 0, s_shared_vao = 0, s_shared_vbo = 0;
static HudParams s_shared;

void hud_shared_init(void)
{
    GLuint prog = make_program("shaders/text.vs", "shaders/text.fs");
    if (!prog) return;
    hud_font_init();                        // атласы 26px + 15px
    GLuint vao, vbo;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 (GLsizeiptr)((HUD_MAX_GLYPHS + 6) * 4 * sizeof(float)),
                 NULL, GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), NULL);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          (const void *)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);

    memset(&s_shared, 0, sizeof s_shared);
    s_shared.prog = prog;
    s_shared.vao = vao;
    s_shared.vbo = vbo;
    s_shared.tex_font = s_font.tex;
    s_shared.u_font = glGetUniformLocation(prog, "uFont");
    s_shared.u_color = glGetUniformLocation(prog, "uColor");
    s_shared.u_usetex = glGetUniformLocation(prog, "uUseTex");
    glUseProgram(prog);
    glUniform1i(s_shared.u_font, 0);
    glUseProgram(0);
    s_shared_prog = prog;
    s_shared_vao = vao;
    s_shared_vbo = vbo;
    g_hud = &s_shared;          // общий экземпляр для консоли и прочего UI
}

void hud_shared_cleanup(void)
{
    if (g_hud == &s_shared) g_hud = NULL;
    resources_program_release(s_shared_prog);
    glDeleteBuffers(1, &s_shared_vbo);
    glDeleteVertexArrays(1, &s_shared_vao);
    GLuint small = s_font_small.tex;
    if (small) glDeleteTextures(1, &small);
    if (s_font.tex) glDeleteTextures(1, &s_font.tex);
    if (s_font_big.tex) glDeleteTextures(1, &s_font_big.tex);
    s_shared_prog = s_shared_vao = s_shared_vbo = 0;
}

SceneNode *hud_make_node(const WorldState *w, const RendererContext *rc)
{
    // Шрифт/программа/буферы — общие (hud_shared_init); узел — только панель
    if (!s_shared_prog) return NULL;
    HudParams *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->prog = s_shared_prog;
    p->vao = s_shared_vao;
    p->vbo = s_shared_vbo;
    p->tex_font = s_font.tex;
    p->rc = rc;
    p->world = w;                              // квантованные высоты (GRND/ALT)
    p->fps = 60.0f;
    p->visible = 0;                // дебаг-панель по умолчанию выключена (G — вкл/выкл)
    p->last_x = rc->cam_pos[0];
    p->last_y = rc->cam_pos[1];
    p->last_z = rc->cam_pos[2];
    p->u_font   = s_shared.u_font;
    p->u_color  = s_shared.u_color;
    p->u_usetex = s_shared.u_usetex;
    const float size[3] = { 0, 0, 0 };
    SceneNode *node = node_new("DebugHUD", "CanvasItem2D", NULL, size, p,
                               hud_node_update, hud_node_render);
    if (!node) free(p);
    return node;
}

// --- Общий рендер текста/прямоугольников (для консоли и будущего UI) ---------
// Рисует тем же шейдером и буфером, что и сам HUD, атлас — из шрифта f.
// Ожидает включённый блендинг и выключенный depth test (у вызывающего).
static void hud_build_line(const HudFont *f, HudParams *p, float *vb, int *nv,
                           float x_px, float y_base, const char *s);

float hud_text_width(const char *s)
{
    float w = 0;
    while (*s) {
        int cp = 0;
        s += utf8_decode(s, &cp);
        w += hud_cp_width(cp);
    }
#ifdef __ANDROID__
    // Скейл текста уже применяется в hud_build_line, но ширина для layout тоже должна скейлиться
    // Берём текущий viewport из g_hud если есть
    if (g_hud && g_hud->vw) w *= hud_mobile_scale(g_hud->vw, g_hud->vh);
#endif
    return w;
}

float hud_text_width_small(const char *s)
{
    float w = 0;
    while (*s) {
        int cp = 0;
        s += utf8_decode(s, &cp);
        w += hud_cp_width_small(cp);
    }
#ifdef __ANDROID__
    if (g_hud && g_hud->vw) w *= hud_mobile_scale(g_hud->vw, g_hud->vh);
#endif
    return w;
}

int hud_cp_supported(int cp)
{
    return font_slot(cp) >= 0;
}

float hud_cp_width(int cp)
{
    int slot = font_slot(cp);
    if (slot < 0) slot = font_slot('?');
    return s_font.glyphs[slot].advance;
}

float hud_cp_width_small(int cp)
{
    int slot = font_slot(cp);
    if (slot < 0) slot = font_slot('?');
    return s_font_small.glyphs[slot].advance;
}

float hud_char_width(int ch)
{
    return hud_cp_width(ch);   // байт 0..127 — тот же codepoint
}

float hud_text_line_height(void)
{
    return s_font.line_h_px;
}

float hud_text_line_height_small(void)
{
    return s_font_small.line_h_px;
}

// Высота над/под базовой линией (для выравнивания курсора и рамок)
float hud_text_ascent_small(void)
{
    return s_font_small.ascent_px;
}

float hud_text_descent_small(void)
{
    return s_font_small.descent_px;
}

float hud_text_ascent(void)
{
    return s_font.ascent_px;
}

float hud_text_descent(void)
{
    return s_font.descent_px;
}

// Рисует текст шрифтом f (атлас из f->tex)
static void hud_text_draw_font(const HudFont *f, int vw, int vh,
                               float x, float y, const char *s,
                               const float color[4])
{
    if (!g_hud || !g_font_ok) return;
    g_hud->vw = vw;                     // метрики глифов считаются в пикселях
    g_hud->vh = vh;
    float vb[HUD_MAX_GLYPHS * 4];
    int nv = 0;
    hud_build_line(f, g_hud, vb, &nv, x, y, s);
    if (nv == 0) return;
    glUseProgram(g_hud->prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, f->tex);
    glUniform4f(g_hud->u_color, color[0], color[1], color[2], color[3]);
    glUniform1f(g_hud->u_usetex, 1.0f);
    glBindVertexArray(g_hud->vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_hud->vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)((size_t)nv * 4 * sizeof(float)),
                 vb, GL_DYNAMIC_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, nv);
}

void hud_text_draw(int vw, int vh, float x, float y, const char *s,
                   const float color[4])
{
    hud_text_draw_font(&s_font, vw, vh, x, y, s, color);
}

// Маленький шрифт (консоль)
void hud_text_draw_small(int vw, int vh, float x, float y, const char *s,
                         const float color[4])
{
    hud_text_draw_font(&s_font_small, vw, vh, x, y, s, color);
}

// Крупный шрифт (заголовки меню)
void hud_text_draw_big(int vw, int vh, float x, float y, const char *s,
                       const float color[4])
{
    hud_text_draw_font(&s_font_big, vw, vh, x, y, s, color);
}

float hud_text_width_big(const char *s)
{
    float w = 0;
    while (*s) {
        int cp = 0;
        s += utf8_decode(s, &cp);
        int slot = font_slot(cp);
        if (slot < 0) slot = font_slot('?');
        w += s_font_big.glyphs[slot].advance;
    }
    return w;
}

float hud_text_line_height_big(void)
{
    return s_font_big.line_h_px;
}

float hud_text_ascent_big(void)
{
    return s_font_big.ascent_px;
}

float hud_text_descent_big(void)
{
    return s_font_big.descent_px;
}

static float hud_mobile_scale(int vw, int vh)
{
#ifdef __ANDROID__
    float s = fminf((float)vw / 1280.0f, (float)vh / 720.0f);
    if (s < 1.3f) s = 1.3f;
    if (s > 2.2f) s = 2.2f;
    return s;
#else
    (void)vw; (void)vh; return 1.0f;
#endif
}

void hud_rect_draw(int vw, int vh, float x, float y, float w, float h,
                   const float color[4])
{
    if (!g_hud) return;
    // Позиции панелей уже скейлятся вызывающими (app.c), глифы — в
    // hud_build_line; здесь скейл не нужен.
    float nx0 = -1.0f + 2.0f * x / vw;
    float nx1 = -1.0f + 2.0f * (x + w) / vw;
    float ny0 =  1.0f - 2.0f * (y + h) / vh;
    float ny1 =  1.0f - 2.0f * y / vh;
    float q[6][4] = {
        {nx0, ny0, 0, 0}, {nx1, ny0, 0, 0}, {nx0, ny1, 0, 0},
        {nx0, ny1, 0, 0}, {nx1, ny0, 0, 0}, {nx1, ny1, 0, 0},
    };
    glUseProgram(g_hud->prog);
    glUniform4f(g_hud->u_color, color[0], color[1], color[2], color[3]);
    glUniform1f(g_hud->u_usetex, 0.0f);
    glBindVertexArray(g_hud->vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_hud->vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof q, q, GL_DYNAMIC_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

// Квад с RGBA-текстурой: игровые кнопки/иконки HUD (shaders/ui_tex.fs).
// Шейдер общий с текстом (text.vs: NDC-позиции + UV), программа кешируется.
void hud_tex_quad_draw(int vw, int vh, GLuint tex,
                       float x, float y, float w, float h,
                       const float color[4])
{
    if (!g_hud || !tex) return;
    static GLuint prog = 0;
    static GLint u_tex = -1, u_color = -1;
    if (!prog) {
        prog = resources_program("shaders/text.vs", "shaders/ui_tex.fs");
        if (!prog) return;
        u_tex = glGetUniformLocation(prog, "uTex");
        u_color = glGetUniformLocation(prog, "uColor");
    }
    float nx0 = -1.0f + 2.0f * x / vw;
    float nx1 = -1.0f + 2.0f * (x + w) / vw;
    float ny0 =  1.0f - 2.0f * (y + h) / vh;
    float ny1 =  1.0f - 2.0f * y / vh;
    float q[6][4] = {
        {nx0, ny0, 0, 1}, {nx1, ny0, 1, 1}, {nx0, ny1, 0, 0},
        {nx0, ny1, 0, 0}, {nx1, ny0, 1, 1}, {nx1, ny1, 1, 0},
    };
    glUseProgram(prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glUniform1i(u_tex, 0);
    glUniform4f(u_color, color[0], color[1], color[2], color[3]);
    glBindVertexArray(g_hud->vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_hud->vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof q, q, GL_DYNAMIC_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

void hud_button_draw(int vw, int vh, const char *img_path,
                     float x, float y, float w, float h,
                     const float tint[4])
{
    // Кеш + счётчик ссылок в resources; отсутствующий файл автоматически
    // подменяется img/notfound.png (лог: "not found -> using notfound").
    GLuint tex = resources_texture(img_path, 4, GL_CLAMP_TO_EDGE, 1);
    if (!tex) {
        hud_rect_draw(vw, vh, x, y, w, h, tint);   // даже notfound.png нет
        return;
    }
    hud_tex_quad_draw(vw, vh, tex, x, y, w, h, tint);
}

// Добавляет глифы строки в буфер vb (позиции NDC + uv атласа).
// y_base — БАЗОВАЯ ЛИНИЯ строки (от верха окна, в пикселях). Каждый глиф
// ставится по своим метрикам (advance/беринг) — буквы не прыгают.
// Строка — UTF-8: кириллица растеризована в атлас, неподдержанные
// символы заменяются на '?'.
static void hud_build_line(const HudFont *f, HudParams *p, float *vb, int *nv,
                           float x_px, float y_base, const char *s)
{
    float vw = p->vw, vh = p->vh;
    float sc = hud_mobile_scale((int)vw, (int)vh);
    while (*s && *nv < HUD_MAX_GLYPHS - 6) {
        int cp = 0;
        int l = utf8_decode(s, &cp);
        int slot = font_slot(cp);
        if (slot < 0) slot = font_slot('?');
        const HudGlyph *g = &f->glyphs[slot];
        float gx = x_px + g->xoff * sc;
        float gy = y_base + g->yoff * sc;
        float gw = g->w * sc, gh = g->h * sc;
        float adv = g->advance * sc;
        float nx0 = -1.0f + 2.0f * gx / vw;
        float nx1 = -1.0f + 2.0f * (gx + gw) / vw;
        float ny0 =  1.0f - 2.0f * gy / vh;
        float ny1 =  1.0f - 2.0f * (gy + gh) / vh;
        // CCW (как панель): TL-BL-BR, TL-BR-TR — иначе backface-cull
        // съедает глифы (в сцене GL_CULL_FACE включён).
        float quad[6][4] = {
            {nx0, ny0, g->u0, g->v0}, {nx0, ny1, g->u0, g->v1},
            {nx1, ny1, g->u1, g->v1},
            {nx0, ny0, g->u0, g->v0}, {nx1, ny1, g->u1, g->v1},
            {nx1, ny0, g->u1, g->v0},
        };
        memcpy(vb + *nv * 4, quad, sizeof quad);
        *nv += 6;
        x_px += adv;
        s += l;
    }
}

// Ширина строки в пикселях по метрикам глифов (для рамки панели).
static float hud_line_width(const char *s)
{
    return hud_text_width(s);
}

void hud_node_update(SceneNode *n, float dt)
{
    HudParams *p = n->params;
    const RendererContext *rc = p->rc;

    // Скорость по перемещению за кадр
    float dx = rc->cam_pos[0] - p->last_x;
    float dy = rc->cam_pos[1] - p->last_y;
    float dz = rc->cam_pos[2] - p->last_z;
    p->last_x = rc->cam_pos[0]; p->last_y = rc->cam_pos[1]; p->last_z = rc->cam_pos[2];
    float speed = (dt > 0.0f) ? sqrtf(dx*dx + dy*dy + dz*dz) / dt : 0.0f;

    float ground = sample_height(p->world, rc->cam_pos[0], rc->cam_pos[2]);
    float yaw_deg = rc->cam_yaw * 180.0f / 3.14159f;
    float pit_deg = rc->cam_pitch * 180.0f / 3.14159f;

    int i = 0;
    snprintf(p->lines[i++], HUD_LINE_LEN, "firerender | FPS: %.0f (%.1f ms)",
             p->fps, p->fps > 0.0f ? 1000.0f / p->fps : 0.0f);
    snprintf(p->lines[i++], HUD_LINE_LEN, "POS:  %.1f, %.1f, %.1f",
             rc->cam_pos[0], rc->cam_pos[1], rc->cam_pos[2]);
    snprintf(p->lines[i++], HUD_LINE_LEN, "GRND: %.1f m | ALT: %+.1f m",
             ground, rc->cam_pos[1] - ground);
    snprintf(p->lines[i++], HUD_LINE_LEN, "MODE: %s (F) | VY: %+.1f",
             rc->cam_fly ? "FLY" : "WALK", rc->cam_vy);
    snprintf(p->lines[i++], HUD_LINE_LEN, "YAW: %.1f | PITCH: %.1f",
             yaw_deg, pit_deg);
    snprintf(p->lines[i++], HUD_LINE_LEN, "SPEED: %.1f m/s", speed);
    snprintf(p->lines[i++], HUD_LINE_LEN, "SUN: az %.0f deg | el %.0f deg",
             atan2f(rc->light_dir[0][0], rc->light_dir[0][2]) * 180.0f / 3.14159f,
             asinf(rc->light_dir[0][1]) * 180.0f / 3.14159f);
    const WorldState *w = p->world;
    snprintf(p->lines[i++], HUD_LINE_LEN, "MAP: %dx%d px = %.0fx%.0f m",
             w->tex_w, w->tex_h, w->world_w, w->world_d);
    snprintf(p->lines[i++], HUD_LINE_LEN, "GRID: %dx%d | TRIS: %d",
             w->grid_w, w->grid_h, w->vert_count / 3);
    snprintf(p->lines[i++], HUD_LINE_LEN, "TILES: %d (128 m, 4 LOD)",
             terrain_tile_count());
    snprintf(p->lines[i++], HUD_LINE_LEN, "HEIGHTS: %.0f..%.0f | band %.0f..%.0f",
             w->min_height, w->max_height, w->band_h0, w->band_h1);
    snprintf(p->lines[i++], HUD_LINE_LEN, "VEG: %d+%d g, %d+%d w",
             veg_count(0), veg_count(2), veg_count(1), veg_count(3));
    snprintf(p->lines[i++], HUD_LINE_LEN, "SHADOW: %s ([)",
             shadow_is_on() ? "ON" : "OFF");
    snprintf(p->lines[i++], HUD_LINE_LEN, "FOG: %s (P)",
             voxfog_is_on() ? "ON" : "OFF");
    p->count = i;
}

// --- Демо-режим (клавиша Q): статистика FPS и график -------------------------
void hud_demo_start(HudParams *p)
{
    p->demo = 1;
    p->demo_n = 0;
    p->demo_avg = p->demo_min = p->demo_max = 0.0f;
    p->demo_time = 0.0f;
}

void hud_demo_frame(HudParams *p, float fps, float dt)
{
    if (p->demo_n < HUD_DEMO_MAX)
        p->demo_fps[p->demo_n++] = fps;
    else {
        memmove(&p->demo_fps[0], &p->demo_fps[1],
                (HUD_DEMO_MAX - 1) * sizeof(float));
        p->demo_fps[HUD_DEMO_MAX - 1] = fps;
    }
    float sum = 0, mn = 1e9f, mx = 0;
    for (int i = 0; i < p->demo_n; i++) {
        sum += p->demo_fps[i];
        if (p->demo_fps[i] < mn) mn = p->demo_fps[i];
        if (p->demo_fps[i] > mx) mx = p->demo_fps[i];
    }
    p->demo_avg = p->demo_n ? sum / p->demo_n : 0.0f;
    p->demo_min = mn < 1e8f ? mn : 0.0f;
    p->demo_max = mx;
    p->demo_time += dt;
}

void hud_demo_stop(HudParams *p)
{
    p->demo = 0;
}

// Правое меню демо: панель, строки статистики и график FPS (столбики).
// Рисуется тем же шейдером: панель (uUseTex=0), глифы (uUseTex=1),
// столбики графика (uUseTex=0, цвет другой).
static void hud_render_demo(HudParams *p, int vw, int vh)
{
    const float line_h = s_font.line_h_px;
    char l1[96], l2[96], l3[96];
    snprintf(l1, sizeof l1, "DEMO | FPS: %.0f", p->fps);
    snprintf(l2, sizeof l2, "AVG %.0f | MIN %.0f | MAX %.0f",
             p->demo_avg, p->demo_min, p->demo_max);
    snprintf(l3, sizeof l3, "TIME %.1f s", p->demo_time);

    // Панель справа вверху: текст + график под ним
    float px = vw - 292.0f, py = 12.0f, pw = 280.0f;
    float gx0 = px + 10.0f, gx1 = px + pw - 10.0f;      // график
    float gy0 = py + 74.0f, gy1 = py + 196.0f;          // 122 px высоты
    float ph = 210.0f;

    float vb[HUD_MAX_GLYPHS * 4];
    int nv = 0;
    float base0 = py + 6.0f + s_font.ascent_px;
    hud_build_line(&s_font, p, vb, &nv, px + 10.0f, base0, l1);
    hud_build_line(&s_font, p, vb, &nv, px + 10.0f, base0 + line_h, l2);
    hud_build_line(&s_font, p, vb, &nv, px + 10.0f, base0 + 2.0f * line_h, l3);

    // Столбики графика: последние demo_n кадров, шкала 0..300 FPS
    float bars[6 * HUD_DEMO_MAX * 4];
    int nb = 0;
    if (p->demo_n > 1) {
        float gw = (gx1 - gx0) / HUD_DEMO_MAX;
        for (int i = 0; i < p->demo_n; i++) {
            float h = p->demo_fps[i] / 300.0f;
            if (h > 1.0f) h = 1.0f;
            if (h < 0.02f) h = 0.02f;
            float bx = gx0 + i * gw;
            float nx0 = -1.0f + 2.0f * bx / vw;
            float nx1 = -1.0f + 2.0f * (bx + gw - 1.0f) / vw;
            float ny0 = 1.0f - 2.0f * gy1 / vh;
            float ny1 = 1.0f - 2.0f * (gy1 - (gy1 - gy0) * h) / vh;
            float *q = bars + nb * 4;
            q[0] = nx0; q[1] = ny0; q[2] = 0; q[3] = 0;
            q[4] = nx1; q[5] = ny0; q[6] = 0; q[7] = 0;
            q[8] = nx0; q[9] = ny1; q[10] = 0; q[11] = 0;
            q[12] = nx1; q[13] = ny0; q[14] = 0; q[15] = 0;
            q[16] = nx1; q[17] = ny1; q[18] = 0; q[19] = 0;
            q[20] = nx0; q[21] = ny1; q[22] = 0; q[23] = 0;
            nb += 6;
        }
    }

    // Панель (6) + глифы (nv) + столбики (nb) одним буфером
    float *all = malloc((size_t)((6 + nv + nb) * 4) * sizeof(float));
    if (!all) return;
    float *q = all;
    q[0] = -1.0f + 2.0f * px / vw;
    q[1] = 1.0f - 2.0f * (py + ph) / vh;
    q[2] = 0; q[3] = 0;
    q[4] = -1.0f + 2.0f * (px + pw) / vw;
    q[5] = q[1]; q[6] = 0; q[7] = 0;
    q[8] = q[0]; q[9] = 1.0f - 2.0f * py / vh; q[10] = 0; q[11] = 0;
    q[12] = q[4]; q[13] = q[1]; q[14] = 0; q[15] = 0;
    q[16] = q[4]; q[17] = 1.0f - 2.0f * py / vh; q[18] = 0; q[19] = 0;
    q[20] = q[0]; q[21] = 1.0f - 2.0f * py / vh; q[22] = 0; q[23] = 0;
    memcpy(all + 24, vb, (size_t)nv * 4 * sizeof(float));
    if (nb > 0)
        memcpy(all + 24 + (size_t)nv * 4, bars, (size_t)nb * 4 * sizeof(float));

    glBindVertexArray(p->vao);
    glBindBuffer(GL_ARRAY_BUFFER, p->vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 (GLsizeiptr)((size_t)(6 + nv + nb) * 4 * sizeof(float)),
                 all, GL_DYNAMIC_DRAW);
    free(all);

    glUseProgram(p->prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, p->tex_font);
    glUniform4f(p->u_color, 0.0f, 0.0f, 0.0f, 0.55f);   // полупрозрачная панель
    glUniform1f(p->u_usetex, 0.0f);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glUniform4f(p->u_color, 0.85f, 0.95f, 1.0f, 1.0f);  // светлый текст
    glUniform1f(p->u_usetex, 1.0f);
    glDrawArrays(GL_TRIANGLES, 6, nv);
    if (nb > 0) {
        glUniform4f(p->u_color, 0.35f, 1.0f, 0.45f, 0.9f);   // зелёный график
        glUniform1f(p->u_usetex, 0.0f);
        glDrawArrays(GL_TRIANGLES, 6 + nv, nb);
    }
    glDisable(GL_BLEND);
}

void hud_node_render(SceneNode *n, float dt)
{
    (void)dt;
    HudParams *p = n->params;
    if (!p->visible) return;       // панель скрыта (клавиша G)
    if (p->rc->pass == RP_SHADOW) return;   // в карты теней HUD не пишет
    int vw = p->vw, vh = p->vh;
    if (vw <= 0 || vh <= 0) return;
    glDisable(GL_DEPTH_TEST);   // HUD — поверх всего
    // Текст и панель полупрозрачные: без блендинга глифы рисуются
    // непрозрачными прямоугольниками и "дырки" в буквах заливаются.
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // Демо-режим: левая панель скрыта, справа — меню с графиком FPS
    if (p->demo) {
        hud_render_demo(p, vw, vh);
        return;
    }

    const float line_h = s_font.line_h_px;
    float maxw = 0;
    for (int i = 0; i < p->count; i++) {
        float w = hud_line_width(p->lines[i]);
        if (w > maxw) maxw = w;
    }
    float px = 12.0f, py = 12.0f;                   // панель слева вверху
    float pw = maxw + 16.0f, ph = p->count * line_h + 10.0f;
    float nx0 = -1.0f + 2.0f * px / vw;
    float nx1 = -1.0f + 2.0f * (px + pw) / vw;
    float ny0 =  1.0f - 2.0f * (py + ph) / vh;
    float ny1 =  1.0f - 2.0f * py / vh;

    float vb[HUD_MAX_GLYPHS * 4];
    int nv = 0;
    // Базовая линия строки i: отступ панели + межстрочный интервал.
    // line_h из метрик шрифта уже включает спуски — строки не наезжают.
    float base0 = py + 6.0f + s_font.ascent_px;
    for (int i = 0; i < p->count; i++)
        hud_build_line(&s_font, p, vb, &nv, px + 8.0f, base0 + i * line_h,
                       p->lines[i]);

    // Панель + глифы одним буфером: 6 вершин панели + nv глифов.
    // Весь массив на стеке — без malloc/free каждый кадр.
    float all[(6 + HUD_MAX_GLYPHS) * 4];
    float *q = all;
    q[0] = nx0; q[1] = ny0; q[2] = 0; q[3] = 0;
    q[4] = nx1; q[5] = ny0; q[6] = 0; q[7] = 0;
    q[8] = nx0; q[9] = ny1; q[10] = 0; q[11] = 0;
    q[12] = nx1; q[13] = ny0; q[14] = 0; q[15] = 0;
    q[16] = nx1; q[17] = ny1; q[18] = 0; q[19] = 0;
    q[20] = nx0; q[21] = ny1; q[22] = 0; q[23] = 0;
    memcpy(all + 24, vb, (size_t)nv * 4 * sizeof(float));

    glBindVertexArray(p->vao);
    glBindBuffer(GL_ARRAY_BUFFER, p->vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 (GLsizeiptr)((size_t)(6 + nv) * 4 * sizeof(float)),
                 all, GL_DYNAMIC_DRAW);

    glUseProgram(p->prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, p->tex_font);
    glUniform4f(p->u_color, 0.0f, 0.0f, 0.0f, 0.55f);   // полупрозрачная панель
    glUniform1f(p->u_usetex, 0.0f);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glUniform4f(p->u_color, 0.85f, 0.95f, 1.0f, 1.0f);  // светлый текст
    glUniform1f(p->u_usetex, 1.0f);
    glDrawArrays(GL_TRIANGLES, 6, nv);
    glDisable(GL_BLEND);   // чтобы следующий кадр (3D) не блендился
}

void hud_cleanup(SceneNode *n)
{
    // GL-объекты — общие (hud_shared_cleanup): узел удаляется без удаления GL
    (void)n;
}
