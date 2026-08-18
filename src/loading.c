// ============================================================================
//  loading.c — экран загрузки уровня (см. loading.h)
// ----------------------------------------------------------------------------
//  Фон — img/loading.png (cover: растянут на весь экран без искажений,
//  лишнее обрезается по центру). Справа снизу — панель загрузки: название
//  карты и автор, подпись текущей фазы с процентами и полоса прогресса с
//  плавно наполняющимся заполнением. Каждый вызов loading_progress
//  предъявляет кадр, т.к. загрузка синхронная и главный цикл в это время
//  не крутится.
// ============================================================================
#include "loading.h"

#include "hud.h"
#include "mapdata.h"
#include "resources.h"
#include "shader.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static GLuint s_tex = 0;
static GLuint s_prog = 0, s_vao = 0, s_vbo = 0;
static GLint s_u_tex, s_u_usetex, s_u_usemarker, s_u_color;
static int s_iw = 0, s_ih = 0;   // размер картинки (для cover-квада)
static char s_map_name[128];     // название карты (info.name из map.json)
static char s_map_author[128];   // автор карты (info.author)
static float s_shown = 0.0f;     // отображаемый прогресс (плавно идёт к цели)

// cover-квад: картинка накрывает весь экран, uv обрезается по центру.
static void loading_quad(int vw, int vh)
{
    if (!s_iw || !s_ih) return;
    float scale = fmaxf((float)vw / s_iw, (float)vh / s_ih);
    float dw = s_iw * scale, dh = s_ih * scale;
    // видимая часть uv (обрезка по центру), v-флип: строки PNG сверху вниз,
    // а в GL v=0 — низ экрана (как в app.c для img/menu.png).
    float u0 = (1.0f - (dw > 0 ? vw / dw : 1.0f)) * 0.5f;
    float v0 = (1.0f - (dh > 0 ? vh / dh : 1.0f)) * 0.5f;
    float quad[6][4] = {
        {-1, -1, u0,         1.0f - v0}, { 1, -1, 1.0f - u0, 1.0f - v0},
        { 1,  1, 1.0f - u0,  v0},
        {-1, -1, u0,         1.0f - v0}, { 1,  1, 1.0f - u0, v0},
        {-1,  1, u0,         v0},
    };
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_DYNAMIC_DRAW);
}

void loading_begin(void)
{
    // Состояние экрана: плавный прогресс начинается с нуля; название/автора
    // карты возьмём при первом прогрессе (map.json читается в фазе 1 загрузки,
    // до этого карта ещё не открыта).
    s_shown = 0.0f;
    s_map_name[0] = 0;
    s_map_author[0] = 0;
    if (s_tex) return;   // уже подготовлен
    s_tex = resources_texture("img/loading.png", 3, GL_CLAMP_TO_EDGE, 1);
    resources_image("img/loading.png", &s_iw, &s_ih, 3);   // флипаем строки сами
    s_prog = make_program("shaders/minimap.vs", "shaders/minimap.fs");
    if (!s_prog) return;
    s_u_tex       = glGetUniformLocation(s_prog, "uBasemap");
    s_u_usetex    = glGetUniformLocation(s_prog, "uUseTex");
    s_u_usemarker = glGetUniformLocation(s_prog, "uUseMarker");
    s_u_color     = glGetUniformLocation(s_prog, "uColor");

    glGenVertexArrays(1, &s_vao);
    glBindVertexArray(s_vao);
    glGenBuffers(1, &s_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), NULL);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          (const void *)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);

    glUseProgram(s_prog);
    glUniform1i(s_u_tex, 0);
    glUniform1i(s_u_usemarker, 0);
    glUseProgram(0);
}

// Один кадр панели загрузки: фон, панель с названием карты, подписью фазы,
// процентами и полосой прогресса (s_shown = текущий показанный прогресс).
static void loading_frame(int vw, int vh, SDL_Window *win, const char *label)
{
    glViewport(0, 0, vw, vh);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDepthMask(GL_TRUE);
    glClearColor(0.04f, 0.05f, 0.07f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // Фон — img/loading.png (cover на весь экран)
    if (s_tex && s_prog) {
        loading_quad(vw, vh);
        glUseProgram(s_prog);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, s_tex);
        glUniform1f(s_u_usetex, 1.0f);
        glUniform3f(s_u_color, 1.0f, 1.0f, 1.0f);
        glBindVertexArray(s_vao);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // --- Панель загрузки: справа снизу ----------------------------------------
    float pw = (float)vw * 0.30f;                       // ширина панели
    if (pw < 260.0f) pw = 260.0f;
    if (pw > 460.0f) pw = 460.0f;
    float margin = 28.0f, pad = 16.0f, bh = 14.0f;
    float lh = hud_text_line_height_small();            // строка 15px
    float lh_mid = hud_text_line_height();              // строка 26px (название)
    float head_h = s_map_name[0] ? lh_mid * 1.15f + lh * 1.15f : 0.0f;
    float ph = pad * 2 + head_h + lh * 1.35f + bh + pad;
    float px = (float)vw - pw - margin;
    float py = (float)vh - ph - margin;

    // Подложка, акцентная полоска сверху и тонкая рамка
    hud_rect_draw(vw, vh, px, py, pw, ph,
                  (const float[4]){0.02f, 0.03f, 0.05f, 0.72f});
    hud_rect_draw(vw, vh, px, py, pw, 3.0f,
                  (const float[4]){1.0f, 0.70f, 0.25f, 0.9f});
    const float br[4] = { 1.0f, 1.0f, 1.0f, 0.10f };
    hud_rect_draw(vw, vh, px, py + ph - 1.0f, pw, 1.0f, br);
    hud_rect_draw(vw, vh, px, py, 1.0f, ph, br);
    hud_rect_draw(vw, vh, px + pw - 1.0f, py, 1.0f, ph, br);

    float x = px + pad;
    float ty = py + pad;
    const float fg[4]  = { 0.96f, 0.97f, 0.99f, 1.0f };
    const float fg2[4] = { 0.72f, 0.75f, 0.82f, 0.85f };

    // Заголовок: название карты (26px) и автор (15px, приглушённо)
    if (head_h > 0.0f) {
        hud_text_draw(vw, vh, x, ty + hud_text_ascent() * 0.9f,
                      s_map_name, fg);
        if (s_map_author[0])
            hud_text_draw_small(vw, vh, x,
                                ty + lh_mid + hud_text_ascent_small(),
                                s_map_author, fg2);
        ty += head_h;
    }

    // Подпись фазы слева, проценты — в правом углу панели
    char pcts[16];
    int pct = (int)(s_shown * 100.0f + 0.5f);
    snprintf(pcts, sizeof pcts, "%d%%", pct);
    hud_text_draw_small(vw, vh, x, ty + hud_text_ascent_small() * 0.9f,
                        label ? label : "Загрузка уровня", fg2);
    hud_text_draw_small(vw, vh, px + pw - pad - hud_text_width_small(pcts),
                        ty + hud_text_ascent_small() * 0.9f, pcts, fg);
    ty += lh * 1.35f;

    // Полоса прогресса: подложка, заполнение и блик на "носике"
    float barw = pw - pad * 2;
    hud_rect_draw(vw, vh, x, ty, barw, bh,
                  (const float[4]){0.0f, 0.0f, 0.0f, 0.55f});
    float fw = (barw - 2.0f) * s_shown;
    if (fw > 0.0f)
        hud_rect_draw(vw, vh, x + 1.0f, ty + 1.0f, fw, bh - 2.0f,
                      (const float[4]){1.0f, 0.55f, 0.18f, 1.0f});
    if (fw > 8.0f && s_shown < 0.999f)
        hud_rect_draw(vw, vh, x + 1.0f + fw - 6.0f, ty + 1.0f, 6.0f,
                      bh - 2.0f, (const float[4]){1.0f, 0.85f, 0.55f, 1.0f});

    glDisable(GL_BLEND);

    // предъявить кадр: загрузка синхронная, без этого кадр не увидит никто
    if (win) SDL_GL_SwapWindow(win);
    // короткий слип — чтобы драйвер предъявил кадр, а не слил его с следующим
    SDL_Delay(8);
}

void loading_progress(float t, const char *label)
{
    int vw = 1280, vh = 720;
    SDL_Window *win = SDL_GL_GetCurrentWindow();
    if (win) SDL_GetWindowSizeInPixels(win, &vw, &vh);
    if (vw < 2 || vh < 2) return;

    // Название/автор карты появятся, когда map.json будет прочитан (фаза 1);
    // до этого панель рисуется без них.
    if (!s_map_name[0]) {
        MapInfo mi;
        mapdata_info(&mi);
        if (mi.name[0]) {
            snprintf(s_map_name, sizeof s_map_name, "%s", mi.name);
            snprintf(s_map_author, sizeof s_map_author, "%s", mi.author);
        }
    }

    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;

    // Плавное наполнение полосы: показанный прогресс догоняет цель
    // экспоненциально (каждый кадр — 1/4 остатка), максимум ~12 кадров на
    // фазу, затем доводка. Так панель "живёт", пока идёт тяжёлая фаза.
    int frames = 0;
    do {
        float step = (t - s_shown) * 0.25f;
        if (step < 0.004f) step = 0.004f;
        if (t > s_shown) s_shown += step;
        if (s_shown >= t) s_shown = t;
        loading_frame(vw, vh, win, label);
        frames++;
    } while ((s_shown < t || (t >= 0.999f && frames < 2)) && frames < 14);

    // Доводка точно в цель (одним кадром), чтобы процент добрался до нужного
    if (s_shown < t) {
        s_shown = t;
        loading_frame(vw, vh, win, label);
    }
}

void loading_end(void)
{
    if (s_tex) { resources_texture_release(s_tex); s_tex = 0; }
    if (s_vbo) { glDeleteBuffers(1, &s_vbo); s_vbo = 0; }
    if (s_vao) { glDeleteVertexArrays(1, &s_vao); s_vao = 0; }
    if (s_prog) { resources_program_release(s_prog); s_prog = 0; }
    s_iw = s_ih = 0;
}
