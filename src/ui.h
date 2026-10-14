// ============================================================================
//  ui.h — immediate-mode UI (меню в стиле Half-Life 1/2)
// ----------------------------------------------------------------------------
//  Виджеты рисуются СРАЗУ при вызове (иммедиейт-режим), ввод — мышь
//  (наведение, клик, перетаскивание) + клавиатура (стрелки/WASD, Enter).
//  Текущий пункт (фокус) подсвечивается, Enter активирует — как в HL1.
//
//  Использование:
//    ui_begin(&u, vw, vh, big);       // big: 1 = шрифт 26px, 0 = 15px
//    ui_col(&u, x, y, w);             // колонка виджетов
//    if (ui_button(&u, "НАСТРОЙКИ")) ...
//    float v = cvar_get("fov");
//    if (ui_slider(&u, "FOV", &v, 5, 120, "%.0f")) cvar_set("fov", v);
//    ui_end(&u);
//
//  Рисование — через hud_rect_draw/hud_text_draw(_small): один шейдер,
//  без новых текстур. Блендинг включается в ui_begin, выключается в ui_end.
// ============================================================================
#ifndef UI_H
#define UI_H
#include <stdbool.h>

typedef struct {
    int vw, vh;              // окно в пикселях
    int big;                 // 1 = шрифт 26px (меню), 0 = 15px (настройки)
    float font_h;            // высота строки текущего шрифта

    // Мышь (читается в ui_begin из SDL)
    int mx, my;              // позиция в окне
    bool mouse_down;         // ЛКМ зажата
    bool mouse_click;        // клик в этом кадре (только что нажали)
    bool mouse_release;      // отпустили в этом кадре

    // Клавиатура (читается в ui_begin из SDL, фронты нажатия)
    bool key_up, key_down, key_left, key_right, key_enter;
    bool key_home, key_end;

    // Состояние виджетов (персистентно между кадрами)
    int hot;                 // id виджета под курсором (-1 нет)
    int active;              // id виджета, зажатого мышью (слайдер тянется)
    int focus;               // id виджета с клавиатурным фокусом (0 — нет)
    int focus_prev;          // фокус на начало кадра (для звука навигации)
    bool eat_home_end;       // слайдер съел Home/End (ui_end не листает фокус)

    // Лейаут колонки
    float cx, cy;            // текущая позиция
    float col_w;             // ширина колонки

    // Внутреннее: предыдущее состояние ввода
    bool m_down_p;
    bool k_up_p, k_down_p, k_left_p, k_right_p, k_enter_p;
    bool k_home_p, k_end_p;
    int id;                  // счётчик виджетов кадра (с 1)
} Ui;

// Начало кадра UI: читает мышь/клавиши, сбрасывает счётчик виджетов.
// big: 1 = крупный шрифт (пункты меню), 0 = мелкий (настройки).
// input: 1 = ввод активен (мышь/клавиши); 0 = только рендер (консоль открыта).
// Включает блендинг и depth test off.
void ui_begin(Ui *u, int vw, int vh, int big, int input);

// Конец кадра: обработка клавиш навигации (стрелки двигают фокус),
// выключает блендинг.
void ui_end(Ui *u);

// Сброс клавиатурного фокуса (при смене экрана меню).
void ui_focus_reset(Ui *u);

// Фокус на ПЕРВЫЙ пункт (при входе на экран: Enter сразу выбирает его).
void ui_focus_first(Ui *u);

// Начинает колонку виджетов в (x, y), ширина w.
void ui_col(Ui *u, float x, float y, float w);
float ui_layout_scale(Ui *u);
float ui_layout_item_h(Ui *u);

// --- Виджеты (возвращают true при активации/изменении) ------------------------
bool ui_button(Ui *u, const char *label);
bool ui_checkbox(Ui *u, const char *label, int *value);
bool ui_slider(Ui *u, const char *label, float *value,
               float min, float max, const char *fmt);
void ui_text(Ui *u, const char *label);      // статичный текст в колонке
void ui_sep(Ui *u);                          // разделитель
void ui_title(Ui *u, const char *label);     // заголовок по центру верха
void ui_title_big(Ui *u, const char *label); // крупный заголовок меню (56px)
void ui_hint(Ui *u, const char *label);      // подсказка внизу экрана

// Текст/кнопка в произвольной точке (окна, крестики, подписи)
void ui_text_at(Ui *u, float x, float y, const char *s, const float c[4]);
bool ui_button_at(Ui *u, float x, float y, float w, float h, const char *label);

// Слот бинда: действие слева, [клавиша] справа. Возвращает true при активации
// (клик/Enter — начать запись новой клавиши).
bool ui_key_slot(Ui *u, const char *action, const char *key);

#endif
