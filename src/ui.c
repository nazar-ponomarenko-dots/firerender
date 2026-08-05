// ============================================================================
//  ui.c — immediate-mode UI (см. ui.h)
// ----------------------------------------------------------------------------
//  Все виджеты рисуются через общий HUD-рендер (hud_rect_draw + текст):
//  подложка фокуса — полупрозрачный жёлтый прямоугольник, текущий пункт —
//  жёлтый текст с маркером ">", остальные — белые (стиль HL1).
//  Навигация: мышь (hover -> focus, клик -> активация) и стрелки/WASD + Enter.
// ============================================================================
#include "ui.h"

#include "audio.h"
#include "hud.h"

#include <stdlib.h>
#include <SDL3/SDL.h>
#include <stdio.h>

// Звук клика по элементу меню (audio/click.wav — движковый или из карты).
// Играет ТОЛЬКО при смене фокуса (наведение на пункт, переключение
// стрелками) — активация и удержание курсора молчат.
static Sound *s_click = NULL;
static int s_click_tried = 0;

static void ui_click_sound(void)
{
    if (!s_click && !s_click_tried) {
        s_click_tried = 1;
        s_click = audio_load("audio/click.wav");
    }
}

static void ui_click_hover(void)
{
    if (s_click) audio_play(s_click, 0.3f, 1.0f);
}

// --- Цвета ---------------------------------------------------------------------
#define C_WHITE  { 0.93f, 0.93f, 0.96f, 1.0f }
#define C_HILITE { 1.0f, 0.78f, 0.20f, 1.0f }     // текущий пункт (жёлтый)
#define C_DIM    { 0.62f, 0.66f, 0.72f, 1.0f }
#define C_SHADOW { 0.0f, 0.0f, 0.0f, 0.55f }      // тень текста

// Текст с тенью (Source-стиль): тёмная копия со смещением + основной
static void ui_draw_text(Ui *u, float x, float y, const char *s,
                         const float c[4])
{
    const float sh[4] = C_SHADOW;
    if (u->big) {
        hud_text_draw(u->vw, u->vh, x + 2.0f, y + 2.0f, s, sh);
        hud_text_draw(u->vw, u->vh, x, y, s, c);
    } else {
        hud_text_draw_small(u->vw, u->vh, x + 1.0f, y + 1.0f, s, sh);
        hud_text_draw_small(u->vw, u->vh, x, y, s, c);
    }
}

// Базовая линия, при которой ГЛИФЫ (а не метрика строки) по центру
// прямоугольника y..y+h: иначе текст "вылезает" выше кнопки.
static float ui_baseline(Ui *u, float y, float h)
{
    float asc = u->big ? hud_text_ascent() : hud_text_ascent_small();
    float desc = u->big ? hud_text_descent() : hud_text_descent_small();
    return y + h * 0.5f - (desc - asc) * 0.5f;
}

// Публичная версия: текст в произвольной точке экрана
static int ui_hit(Ui *u, int id, float x, float y, float w, float h);
static bool ui_activated(Ui *u, int id, bool hover);
static float ui_text_w(Ui *u, const char *s);

void ui_text_at(Ui *u, float x, float y, const char *s, const float c[4])
{
    ui_draw_text(u, x, y, s, c);
}

// Кнопка в произвольной точке (не в колонке)
bool ui_button_at(Ui *u, float x, float y, float w, float h, const char *label)
{
    int id = ++u->id;
    int hover = ui_hit(u, id, x, y, w, h);
    bool act = ui_activated(u, id, hover);
    float tw = ui_text_w(u, label);
    float tx = x + (w - tw) * 0.5f;
    float ty = ui_baseline(u, y, h);
    const float *col = u->focus == id ? (const float[4])C_HILITE : (const float[4])C_WHITE;
    ui_draw_text(u, tx, ty, label, col);
    return act;
}

static float ui_text_w(Ui *u, const char *s)
{
    return u->big ? hud_text_width(s) : hud_text_width_small(s);
}

// Общая обработка хитбокса виджета: hover -> hot/focus, клик -> active.
// Возвращает 1, если курсор над виджетом.
static int ui_hit(Ui *u, int id, float x, float y, float w, float h)
{
    int hover = u->mx >= x && u->mx <= x + w &&
                u->my >= y && u->my <= y + h;
    if (hover) {
        u->hot = id;
        u->focus = id;
    }
    if (hover && u->mouse_click)
        u->active = id;
    return hover;
}

// Активация фокус-виджета клавишей (Enter) или кликом.
static bool ui_activated(Ui *u, int id, bool hover)
{
    // Активация БЕЗ звука: клик меню звучит только при смене фокуса
    return (u->focus == id && u->key_enter) || (hover && u->mouse_click);
}

void ui_begin(Ui *u, int vw, int vh, int big, int input)
{
    ui_click_sound();           // ленивая загрузка клика (после audio_init)
    u->focus_prev = u->focus;   // фокус на начало кадра (для звука навигации)
    u->vw = vw;
    u->vh = vh;
    u->big = big;
    u->font_h = big ? hud_text_line_height() : hud_text_line_height_small();

    // Ввод: читаем всегда (чтобы prev был актуален и после закрытия консоли
    // не было ложных фронтов), но при input=0 события НЕ генерируем —
    // консоль поверх меню забирает клавиатуру себе.
    float mx = 0, my = 0;
    Uint32 btns = SDL_GetMouseState(&mx, &my);
    bool down = (btns & SDL_BUTTON_LMASK) != 0;
    const bool *k = SDL_GetKeyboardState(NULL);
    bool up = k[SDL_SCANCODE_UP] || k[SDL_SCANCODE_W];
    bool dn = k[SDL_SCANCODE_DOWN] || k[SDL_SCANCODE_S];
    bool lf = k[SDL_SCANCODE_LEFT] || k[SDL_SCANCODE_A];
    bool rt = k[SDL_SCANCODE_RIGHT] || k[SDL_SCANCODE_D];
    bool en = k[SDL_SCANCODE_RETURN] || k[SDL_SCANCODE_SPACE];
    bool hm = k[SDL_SCANCODE_HOME];
    bool en2 = k[SDL_SCANCODE_END];

    if (input) {
        u->mx = (int)mx;
        u->my = (int)my;
        u->mouse_click = down && !u->m_down_p;
        u->mouse_release = !down && u->m_down_p;
        u->mouse_down = down;
        u->key_up = up && !u->k_up_p;
        u->key_down = dn && !u->k_down_p;
        u->key_left = lf && !u->k_left_p;
        u->key_right = rt && !u->k_right_p;
        u->key_enter = en && !u->k_enter_p;
        u->key_home = hm && !u->k_home_p;
        u->key_end = en2 && !u->k_end_p;
    } else {
        u->mx = u->my = -10000;      // курсор "вне экрана" — hover нигде
        u->mouse_click = u->mouse_release = false;
        u->mouse_down = false;
        u->key_up = u->key_down = u->key_left = u->key_right = false;
        u->key_enter = u->key_home = u->key_end = false;
    }
    u->m_down_p = down;
    u->k_up_p = up;
    u->k_down_p = dn;
    u->k_left_p = lf;
    u->k_right_p = rt;
    u->k_enter_p = en;
    u->k_home_p = hm;
    u->k_end_p = en2;

    u->id = 0;
    u->hot = -1;
    u->cx = u->cy = 0;
    u->col_w = 0;

    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

void ui_end(Ui *u)
{
    // Навигация стрелками: фокус ходит по виджетам кадра, по кругу.
    // Home/End — в начало/конец списка.
    if (u->key_up) u->focus = (u->focus <= 1 || u->focus > u->id) ? u->id : u->focus - 1;
    if (u->key_down) u->focus = (u->focus >= u->id || u->focus < 1) ? 1 : u->focus + 1;
    if (u->key_home) u->focus = u->id ? 1 : 0;
    if (u->key_end) u->focus = u->id;
    if (u->focus < 0) u->focus = 0;
    if (u->focus > u->id) u->focus = u->id;
    // Звук навигации: клик ТОЛЬКО при переходе на другой пункт
    // (наведение мыши или стрелки). Удержание курсора на пункте — молчит.
    if (u->focus != u->focus_prev && u->focus > 0)
        ui_click_hover();
    if (u->active >= 0 && u->mouse_release) u->active = -1;
    glDisable(GL_BLEND);
}

void ui_focus_reset(Ui *u)
{
    u->focus = 0;
    u->active = -1;
}

// Фокус на ПЕРВЫЙ пункт экрана: Enter сразу выбирает его (как в HL1)
void ui_focus_first(Ui *u)
{
    u->focus = 1;
    u->active = -1;
}

// Слот бинда: действие слева, [клавиша] справа (экран "Клавиши")
static float ui_item_h(Ui *u);

bool ui_key_slot(Ui *u, const char *action, const char *key)
{
    int id = ++u->id;
    float ih = ui_item_h(u);
    int hover = ui_hit(u, id, u->cx, u->cy, u->col_w, ih);
    bool act = ui_activated(u, id, hover);

    float ty = ui_baseline(u, u->cy, ih);
    const float *col = u->focus == id ? (const float[4])C_HILITE : (const float[4])C_WHITE;
    ui_draw_text(u, u->cx + 10.0f, ty, action, col);

    char kb[64];
    snprintf(kb, sizeof kb, "[%s]", key ? key : "?");
    float kw = ui_text_w(u, kb);
    ui_draw_text(u, u->cx + u->col_w - kw - 10.0f, ty, kb, col);

    u->cy += ih + 4.0f;
    return act;
}

void ui_col(Ui *u, float x, float y, float w)
{
    u->cx = x;
    u->cy = y;
    u->col_w = w;
}

static float ui_item_h(Ui *u)
{
    return u->font_h + 10.0f;
}

// Кнопка в колонке: текст по центру строки, при фокусе/наведении — жёлтый.
// Подложек нет — только цвет текста (как просили).
bool ui_button(Ui *u, const char *label)
{
    int id = ++u->id;
    float ih = ui_item_h(u);
    int hover = ui_hit(u, id, u->cx, u->cy, u->col_w, ih);
    bool act = ui_activated(u, id, hover);

    float tw = ui_text_w(u, label);
    float tx = u->cx + (u->col_w - tw) * 0.5f;
    float ty = ui_baseline(u, u->cy, ih);
    if (u->focus == id) {
        const float hc[4] = C_HILITE;
        ui_draw_text(u, tx, ty, label, hc);
    } else {
        const float wc[4] = C_WHITE;
        ui_draw_text(u, tx, ty, label, wc);
    }
    u->cy += ih + 4.0f;
    return act;
}

bool ui_checkbox(Ui *u, const char *label, int *value)
{
    int id = ++u->id;
    float ih = ui_item_h(u);
    int hover = ui_hit(u, id, u->cx, u->cy, u->col_w, ih);
    bool changed = false;
    if (ui_activated(u, id, hover)) {
        *value = !*value;
        changed = true;
    }

    char box[8];
    snprintf(box, sizeof box, "[%c]", *value ? 'x' : ' ');
    const float *col = u->focus == id ? (const float[4])C_HILITE : (const float[4])C_WHITE;
    float ty = ui_baseline(u, u->cy, ih);
    ui_draw_text(u, u->cx + 10.0f, ty, box, col);
    ui_draw_text(u, u->cx + 48.0f, ty, label, col);
    u->cy += ih + 4.0f;
    return changed;
}

bool ui_slider(Ui *u, const char *label, float *value,
               float min, float max, const char *fmt)
{
    int id = ++u->id;
    float ih = ui_item_h(u) + 8.0f;
    int hover = ui_hit(u, id, u->cx, u->cy, u->col_w, ih);
    bool changed = false;

    // Трек: сразу после label, до зоны значения. Бегунок всегда идёт
    // ровно за курсором (t считается от начала трека).
    float label_w = ui_text_w(u, label);
    float tx = u->cx + label_w + 14.0f;
    float tw = u->col_w - label_w - 14.0f - 74.0f;
    if (tw < 40.0f) tw = 40.0f;
    float tyy = u->cy + u->font_h + 7.0f;

    // Захват и перетаскивание: клик по строке ставит значение
    if (hover && u->mouse_click) u->active = id;
    if (u->active == id && u->mouse_down) {
        float t = (u->mx - tx) / tw;
        if (t < 0) t = 0;
        if (t > 1) t = 1;
        float nv = min + t * (max - min);
        if (nv != *value) { *value = nv; changed = true; }
    }
    if (u->active == id && u->mouse_release) u->active = -1;

    // Клавиатура: ←/→ шаг 5% диапазона, Home/End — крайние значения
    if (u->focus == id) {
        float step = (max - min) * 0.05f;
        if (u->key_left)  { *value -= step; changed = true; }
        if (u->key_right) { *value += step; changed = true; }
        if (u->key_home)  { *value = min; changed = true; }
        if (u->key_end)   { *value = max; changed = true; }
    }
    if (*value < min) *value = min;
    if (*value > max) *value = max;

    float ty = ui_baseline(u, u->cy, ih);
    const float *col = u->focus == id ? (const float[4])C_HILITE : (const float[4])C_WHITE;
    ui_draw_text(u, u->cx + 10.0f, ty, label, col);

    // Трек: тёмная линия, заполненная белым до бегунка
    float t0 = (max > min) ? (*value - min) / (max - min) : 0.0f;
    const float track[4] = { 0.22f, 0.25f, 0.30f, 1.0f };
    hud_rect_draw(u->vw, u->vh, tx, tyy, tw, 3.0f, track);
    const float fill[4] = { 0.85f, 0.87f, 0.92f, 1.0f };
    if (t0 > 0.01f)
        hud_rect_draw(u->vw, u->vh, tx, tyy, tw * t0, 3.0f, fill);
    // Бегунок: светлый прямоугольник с тонкими краями
    float bx = tx + t0 * tw - 5.0f;
    const float knob[4] = { 0.95f, 0.95f, 0.97f, 1.0f };
    hud_rect_draw(u->vw, u->vh, bx, tyy - 4.0f, 10.0f, 11.0f, knob);
    const float knobline[4] = { 0.45f, 0.5f, 0.58f, 1.0f };
    hud_rect_draw(u->vw, u->vh, bx, tyy - 4.0f, 10.0f, 2.0f, knobline);
    hud_rect_draw(u->vw, u->vh, bx, tyy + 5.0f, 10.0f, 2.0f, knobline);

    // Значение справа
    char val[32];
    snprintf(val, sizeof val, fmt, *value);
    ui_draw_text(u, u->cx + u->col_w - 66.0f, ty, val, col);

    u->cy += ih + 5.0f;
    return changed;
}

void ui_text(Ui *u, const char *label)
{
    const float dim[4] = C_DIM;
    ui_draw_text(u, u->cx + 10.0f, ui_baseline(u, u->cy, u->font_h),
                 label, dim);
    u->cy += u->font_h + 4.0f;
}

void ui_sep(Ui *u)
{
    u->cy += 6.0f;
    const float line[4] = { 0.35f, 0.37f, 0.42f, 0.8f };
    hud_rect_draw(u->vw, u->vh, u->cx + 8.0f, u->cy, u->col_w - 16.0f, 2.0f, line);
    u->cy += 8.0f;
}

void ui_title(Ui *u, const char *label)
{
    float tw = ui_text_w(u, label);
    float ty = u->big ? 60.0f : 40.0f;
    const float wc[4] = C_WHITE;
    ui_draw_text(u, (u->vw - tw) * 0.5f, ty, label, wc);
    // Тонкая линия-подчёркивание под заголовком (стиль HL1)
    const float line[4] = { 0.55f, 0.60f, 0.70f, 0.9f };
    hud_rect_draw(u->vw, u->vh, (u->vw - tw) * 0.5f, ty + u->font_h + 6.0f,
                  tw, 2.0f, line);
}

void ui_hint(Ui *u, const char *label)
{
    float tw = ui_text_w(u, label);
    float ty = u->vh - u->font_h - 14.0f;
    const float dim[4] = C_DIM;
    ui_draw_text(u, (u->vw - tw) * 0.5f, ty, label, dim);
}

// Крупный заголовок меню: слева по центру, 56px, с тенью.
void ui_title_big(Ui *u, const char *label)
{
    const float sh[4] = C_SHADOW;
    const float wc[4] = C_WHITE;
    // Базовая линия по центру вертикали экрана (как глифы, не метрика)
    float y = u->vh * 0.5f - (hud_text_descent_big() - hud_text_ascent_big()) * 0.5f;
    hud_text_draw_big(u->vw, u->vh, 90.0f + 3.0f, y + 3.0f, label, sh);
    hud_text_draw_big(u->vw, u->vh, 90.0f, y, label, wc);
}
