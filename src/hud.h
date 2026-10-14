// ============================================================================
//  hud.h — debug HUD (CanvasItem2D): player position and info on the left
// ----------------------------------------------------------------------------
//  Text uses the TTF font img/Inter_18pt-Black.ttf: the atlas is rasterized
//  once via stb_truetype (third_party/stb_truetype.h), glyphs positioned by the
//  font metrics (advance/bearing). The panel and letters are drawn by a single
//  shader shaders/text.* (uUseTex: 0 = panel, 1 = glyph; glyph coverage in the
//  R channel).
// ============================================================================
#ifndef HUD_H
#define HUD_H
#include "gl_loader.h"
#include "render_ctx.h"
#include "scene.h"
#include "world.h"

typedef struct {
    GLuint prog, vao, vbo;         // program and vertex buffer (every frame)
    GLuint tex_font;               // font atlas
    GLint u_font, u_color, u_usetex;
    const RendererContext *rc;     // player position/view, sun (renderer.c)
    const WorldState *world;       // world data (GRND/ALT, sizes, grid)
    int vw, vh;                    // window in pixels
    float fps;                     // updated by the main loop (EMA)
    float last_x, last_y, last_z;  // previous position (for speed)
    int visible;                   // panel shown? (key H)
    char lines[16][96];
    int count;                     // how many lines are filled

    // Demo mode (key Q): the left panel is hidden, on the right — a panel
    // with current FPS, avg/min/max and a live FPS graph.
#define HUD_DEMO_MAX 180
    int demo;                      // 1 = demo mode
    float demo_fps[HUD_DEMO_MAX];  // ring of recent FPS values (for the graph)
    int demo_n;                    // how many values collected
    float demo_avg, demo_min, demo_max, demo_time;
} HudParams;

// Creates the DebugHUD node (compiles the shader, builds the font atlas and VAO).
SceneNode *hud_make_node(const WorldState *w, const RendererContext *rc);

void hud_node_update(SceneNode *n, float dt);
void hud_node_render(SceneNode *n, float dt);

// Demo mode: start (resets statistics) and add a frame (fps per frame).
void hud_demo_start(HudParams *p);
void hud_demo_frame(HudParams *p, float fps, float dt);
void hud_demo_stop(HudParams *p);

// --- Shared text/rectangle rendering (for the console and future UI) ---------
// Drawn with the same shader, atlas and buffer as the HUD itself. GL state
// (blending on, depth test off) is the caller's responsibility.
// Coordinates are window pixels, y from the top; color is rgba 0..1.
// Plain functions — HUD font (26px), *_small — console font (15px).
float hud_text_width(const char *s);
float hud_char_width(int ch);       // advance of one character (ASCII byte)
float hud_cp_width(int cp);         // advance of a codepoint ('?' if no glyph)
int hud_cp_supported(int cp);       // is the glyph in the atlas (Cyrillic — yes)
float hud_text_line_height(void);
float hud_cp_width_small(int cp);
float hud_text_width_small(const char *s);
float hud_text_line_height_small(void);
float hud_text_ascent_small(void);  // height above the baseline (cursor)
float hud_text_descent_small(void); // height below the baseline
float hud_text_ascent(void);        // same for the 26px font (menu)
float hud_text_descent(void);
void hud_text_draw(int vw, int vh, float x, float y, const char *s,
                   const float color[4]);
void hud_text_draw_small(int vw, int vh, float x, float y, const char *s,
                         const float color[4]);
// Large font (56px) — menu headings
float hud_text_width_big(const char *s);
float hud_text_line_height_big(void);
float hud_text_ascent_big(void);
float hud_text_descent_big(void);
void hud_text_draw_big(int vw, int vh, float x, float y, const char *s,
                       const float color[4]);
void hud_rect_draw(int vw, int vh, float x, float y, float w, float h,
                   const float color[4]);

// Game icon button: an RGBA texture from img/*.png on a quad (x,y,w,h).
// A missing file is substituted by the engine with img/notfound.png (Source style,
// see resources_texture); tint — colorization + transparency.
void hud_button_draw(int vw, int vh, const char *img_path,
                     float x, float y, float w, float h,
                     const float tint[4]);

// A quad with an already loaded GL texture (UV 0..1), same shader.
void hud_tex_quad_draw(int vw, int vh, GLuint tex,
                       float x, float y, float w, float h,
                       const float color[4]);

// Frees the node's GL resources.
void hud_cleanup(SceneNode *n);

// --- Shared font and text rendering (live for the WHOLE session, independent of the world) ------
// Call at startup (before the menu) and on exit. The console and menu need it
// even before a map is loaded.
void hud_shared_init(void);
void hud_shared_cleanup(void);

#endif
