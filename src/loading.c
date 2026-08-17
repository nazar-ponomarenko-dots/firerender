// ============================================================================
//  loading.c — экран загрузки уровня (см. loading.h)
// ----------------------------------------------------------------------------
//  Фон — img/loading.png (cover: растянут на весь экран без искажений,
//  лишнее обрезается по центру). Справа снизу — полоса прогресса загрузки
//  с подписью текущей фазы. Каждый вызов loading_progress предъявляет кадр,
//  т.к. загрузка синхронная и главный цикл в это время не крутится.
// ============================================================================
#include "loading.h"

#include "hud.h"
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

void loading_progress(float t, const char *label)
{
    int vw = 1280, vh = 720;
    SDL_Window *win = SDL_GL_GetCurrentWindow();
    if (win) SDL_GetWindowSizeInPixels(win, &vw, &vh);
    if (vw < 2 || vh < 2) return;

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

    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // Полоса загрузки — справа снизу
    float bw = (float)vw * 0.34f;
    if (bw < 220.0f) bw = 220.0f;
    if (bw > 420.0f) bw = 420.0f;
    float bh = 18.0f;
    float margin = 24.0f;
    float x = (float)vw - bw - margin;
    float y = (float)vh - bh - margin;

    // подпись над полосой («Загрузка уровня… 37%»)
    char cap[96];
    float pct = t * 100.0f + 0.5f;
    snprintf(cap, sizeof cap, "%s… %d%%", label ? label : "Загрузка уровня",
             (int)pct);
    float lh = hud_text_line_height_small();
    float cap_w = hud_text_width_small(cap);
    float cap_x = x + bw - cap_w;
    if (cap_x < x) cap_x = x;
    const float fg[4] = { 0.95f, 0.96f, 0.98f, 1.0f };
    const float bg[4] = { 0.0f, 0.0f, 0.0f, 0.45f };
    hud_rect_draw(vw, vh, x, y - lh - 8.0f, bw, lh + 6.0f, bg);
    hud_text_draw_small(vw, vh, cap_x, y - lh - 6.0f + hud_text_ascent_small(),
                        cap, fg);

    // подложка + рамка + заполнение полосы
    hud_rect_draw(vw, vh, x, y, bw, bh, (const float[4]){0.0f, 0.0f, 0.0f, 0.55f});
    const float br[4] = { 0.85f, 0.88f, 0.95f, 0.90f };
    hud_rect_draw(vw, vh, x, y, bw, 1.5f, br);
    hud_rect_draw(vw, vh, x, y + bh - 1.5f, bw, 1.5f, br);
    hud_rect_draw(vw, vh, x, y, 1.5f, bh, br);
    hud_rect_draw(vw, vh, x + bw - 1.5f, y, 1.5f, bh, br);
    const float fill[4] = { 1.0f, 0.70f, 0.25f, 1.0f };
    hud_rect_draw(vw, vh, x + 2.0f, y + 2.0f, (bw - 4.0f) * t, bh - 4.0f, fill);
    glDisable(GL_BLEND);

    // предъявить кадр: загрузка синхронная, без этого кадр не увидит никто
    if (win) SDL_GL_SwapWindow(win);
    // короткий слип — чтобы драйвер предъявил кадр, а не слил его с следующим
    SDL_Delay(8);
}

void loading_end(void)
{
    if (s_tex) { resources_texture_release(s_tex); s_tex = 0; }
    if (s_vbo) { glDeleteBuffers(1, &s_vbo); s_vbo = 0; }
    if (s_vao) { glDeleteVertexArrays(1, &s_vao); s_vao = 0; }
    if (s_prog) { resources_program_release(s_prog); s_prog = 0; }
    s_iw = s_ih = 0;
}
