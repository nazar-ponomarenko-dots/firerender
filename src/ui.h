// ============================================================================
//  ui.h — immediate-mode UI (menu in the Half-Life 1/2 style)
// ----------------------------------------------------------------------------
//  Widgets are drawn IMMEDIATELY on call (immediate mode), input — mouse
//  (hover, click, drag) + keyboard (arrows/WASD, Enter).
//  The current item (focus) is highlighted, Enter activates it — like in HL1.
//
//  Usage:
//    ui_begin(&u, vw, vh, big);       // big: 1 = 26px font, 0 = 15px
//    ui_col(&u, x, y, w);             // a column of widgets
//    if (ui_button(&u, "НАСТРОЙКИ")) ...
//    float v = cvar_get("fov");
//    if (ui_slider(&u, "FOV", &v, 5, 120, "%.0f")) cvar_set("fov", v);
//    ui_end(&u);
//
//  Drawing goes through hud_rect_draw/hud_text_draw(_small): one shader,
//  no new textures. Blending is enabled in ui_begin, disabled in ui_end.
// ============================================================================
#ifndef UI_H
#define UI_H
#include <stdbool.h>

typedef struct {
    int vw, vh;              // window in pixels
    int big;                 // 1 = 26px font (menu), 0 = 15px (settings)
    float font_h;            // line height of the current font

    // Mouse (read in ui_begin from SDL)
    int mx, my;              // position within the window
    bool mouse_down;         // LMB held
    bool mouse_click;        // click this frame (just pressed)
    bool mouse_release;      // released this frame

    // Keyboard (read in ui_begin from SDL, press edges)
    bool key_up, key_down, key_left, key_right, key_enter;
    bool key_home, key_end;

    // Widget state (persistent between frames)
    int hot;                 // id of the widget under the cursor (-1 none)
    int active;              // id of the widget held by the mouse (slider dragging)
    int focus;               // id of the widget with keyboard focus (0 — none)
    int focus_prev;          // focus at the start of the frame (for navigation sound)
    bool eat_home_end;       // a slider consumed Home/End (ui_end doesn't move focus)

    // Column layout
    float cx, cy;            // current position
    float col_w;             // column width

    // Internal: previous input state
    bool m_down_p;
    bool k_up_p, k_down_p, k_left_p, k_right_p, k_enter_p;
    bool k_home_p, k_end_p;
    int id;                  // per-frame widget counter (starts at 1)
} Ui;

// Start of the UI frame: reads mouse/keys, resets the widget counter.
// big: 1 = large font (menu items), 0 = small (settings).
// input: 1 = input active (mouse/keys); 0 = render only (console open).
// Enables blending and disables depth test.
void ui_begin(Ui *u, int vw, int vh, int big, int input);

// End of the frame: handles navigation keys (arrows move the focus),
// disables blending.
void ui_end(Ui *u);

// Resets the keyboard focus (when switching menu screens).
void ui_focus_reset(Ui *u);

// Focus on the FIRST item (when entering a screen: Enter immediately selects it).
void ui_focus_first(Ui *u);

// Starts a widget column at (x, y) with width w.
void ui_col(Ui *u, float x, float y, float w);
float ui_layout_scale(Ui *u);
float ui_layout_item_h(Ui *u);

// --- Widgets (return true on activation/change) ------------------------
bool ui_button(Ui *u, const char *label);
bool ui_checkbox(Ui *u, const char *label, int *value);
bool ui_slider(Ui *u, const char *label, float *value,
               float min, float max, const char *fmt);
void ui_text(Ui *u, const char *label);      // static text in the column
void ui_sep(Ui *u);                          // separator
void ui_title(Ui *u, const char *label);     // title centered at the top
void ui_title_big(Ui *u, const char *label); // large menu title (56px)
void ui_hint(Ui *u, const char *label);      // hint at the bottom of the screen

// Text/button at an arbitrary point (windows, crosshair, captions)
void ui_text_at(Ui *u, float x, float y, const char *s, const float c[4]);
bool ui_button_at(Ui *u, float x, float y, float w, float h, const char *label);

// Key-binding slot: action on the left, [key] on the right. Returns true on
// activation (click/Enter — start recording a new key).
bool ui_key_slot(Ui *u, const char *action, const char *key);

#endif
