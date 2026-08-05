// ============================================================================
//  hud.h — дебаг-HUD (CanvasItem2D): позиция игрока и инфа слева
// ----------------------------------------------------------------------------
//  Текст — TTF-шрифт img/Inter_18pt-Black.ttf: атлас растеризуется один раз
//  через stb_truetype (third_party/stb_truetype.h), глифы ставятся по
//  метрикам шрифта (advance/беринг). Панель и буквы рисует один шейдер
//  shaders/text.* (uUseTex: 0 = панель, 1 = глиф; покрытие глифа в R-канале).
// ============================================================================
#ifndef HUD_H
#define HUD_H
#include "gl_loader.h"
#include "scene.h"
#include "camera.h"
#include "sun.h"
#include "world.h"

typedef struct {
    GLuint prog, vao, vbo;         // программа и буфер вершин (каждый кадр)
    GLuint tex_font;               // атлас шрифта
    GLint u_font, u_color, u_usetex;
    CameraParams *cam;             // позиция игрока (узел Camera)
    SunParams *sun;                // направление солнца (узел Sun)
    const WorldState *world;       // данные мира (GRND/ALT, размеры, сетка)
    int vw, vh;                    // окно в пикселях
    float fps;                     // обновляет главный цикл (EMA)
    float last_x, last_y, last_z;  // предыдущая позиция (для скорости)
    int visible;                   // панель показана? (клавиша H)
    char lines[16][96];
    int count;                     // сколько строк заполнено

    // Режим демо (клавиша Q): левая панель скрывается, справа — панель
    // с текущим FPS, avg/min/max и живым графиком FPS.
#define HUD_DEMO_MAX 180
    int demo;                      // 1 = демо-режим
    float demo_fps[HUD_DEMO_MAX];  // кольцо последних FPS (для графика)
    int demo_n;                    // сколько значений накоплено
    float demo_avg, demo_min, demo_max, demo_time;
} HudParams;

// Создаёт узел DebugHUD (компилирует шейдер, строит атлас шрифта и VAO).
SceneNode *hud_make_node(const WorldState *w, CameraParams *cam, SunParams *sun);

void hud_node_update(SceneNode *n, float dt);
void hud_node_render(SceneNode *n, float dt);

// Демо-режим: включить (сброс статистики) и добавить кадр (fps за кадр).
void hud_demo_start(HudParams *p);
void hud_demo_frame(HudParams *p, float fps, float dt);
void hud_demo_stop(HudParams *p);

// --- Общий рендер текста/прямоугольников (для консоли и будущего UI) ---------
// Рисуют тем же шейдером, атласом и буфером, что и сам HUD. Состояние
// GL (блендинг вкл, depth test выкл) — ответственность вызывающего.
// Координаты — в пикселях окна, y от верха; color — rgba 0..1.
// Обычные функции — шрифт HUD (26px), *_small — консольный (15px).
float hud_text_width(const char *s);
float hud_char_width(int ch);       // шаг одного символа (ASCII-байт)
float hud_cp_width(int cp);         // шаг codepoint'а ('?' если нет глифа)
int hud_cp_supported(int cp);       // есть ли глиф в атласе (кириллица — да)
float hud_text_line_height(void);
float hud_cp_width_small(int cp);
float hud_text_width_small(const char *s);
float hud_text_line_height_small(void);
float hud_text_ascent_small(void);  // высота над базовой линией (курсор)
float hud_text_descent_small(void); // высота под базовой линией
float hud_text_ascent(void);        // то же для шрифта 26px (меню)
float hud_text_descent(void);
void hud_text_draw(int vw, int vh, float x, float y, const char *s,
                   const float color[4]);
void hud_text_draw_small(int vw, int vh, float x, float y, const char *s,
                         const float color[4]);
// Крупный шрифт (56px) — заголовки меню
float hud_text_width_big(const char *s);
float hud_text_line_height_big(void);
float hud_text_ascent_big(void);
float hud_text_descent_big(void);
void hud_text_draw_big(int vw, int vh, float x, float y, const char *s,
                       const float color[4]);
void hud_rect_draw(int vw, int vh, float x, float y, float w, float h,
                   const float color[4]);

// Освобождает GL-ресурсы узла.
void hud_cleanup(SceneNode *n);

// --- Общий шрифт и рендер текста (живут ВСЮ сессию, не зависят от мира) ------
// Вызывать при старте (до меню) и при выходе. Нужны консоли и меню даже
// до загрузки карты.
void hud_shared_init(void);
void hud_shared_cleanup(void);

#endif
