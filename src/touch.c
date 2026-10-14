// ============================================================================
//  touch.c — touch controls (see touch.h)
// ----------------------------------------------------------------------------
//  Scheme: SDL converts taps into mouse events while we are in menus
//  (SDL_TOUCH_MOUSE_EVENTS = 1 — UI buttons get clicked by finger as before).
//  In game the hint is disabled and this module owns touch:
//    • left third of the screen — virtual joystick: direction = g_act flags
//      (the same as "+forward"/"+left" etc.), at the edge — run;
//    • the rest of the screen — swipe = look (camera.c takes the delta);
//    • short tap on the right — jump (a +jump pulse for 3 frames).
//  On desktop (no touch devices) the module sleeps and costs nothing.
// ============================================================================
#include "touch.h"

#include "actions.h"
#include "hud.h"
#include "cvars.h"
#include "renderer.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <string.h>

// Screen zones (width fractions) — enlarged for phones
#define JOY_ZONE    0.42f    // left 42% — joystick (was 0.35, comfy for a thumb)
#define JOY_BASE_R  150.0f   // joystick base radius, px (was 90, too small at 1080p)
#define JOY_DEAD    0.22f    // dead zone slightly smaller — more responsive
#define TAP_MS      250      // a tap shorter than this = jump
#define TAP_MOVE_PX 28.0f    // ...and moved less than this (phone scale)

static int s_avail = -1;         // cache: whether touch devices exist (-1 — not checked)
static int s_ui_mode = 1;        // 1 — menus (mouse synthesis enabled)
static int s_joystick = 0;       // finger on the joystick
static SDL_FingerID s_joy_id = 0;
static float s_joy_ox, s_joy_oy; // joystick center (touch point)
static float s_joy_kx, s_joy_ky; // current offset (for drawing)
static int s_look = 0;           // look finger
static SDL_FingerID s_look_id = 0;
static float s_look_dx, s_look_dy;    // accumulated delta for the camera
static float s_look_moved;            // total finger path (for tap detection)
static Uint64 s_look_t0;              // touch time (for tap detection)
static int s_jump_pulse;              // remaining frames of the jump pulse

static float touch_scale(float w, float h)
{
#ifdef __ANDROID__
    float scale = fminf(w / 1280.0f, h / 720.0f);
    if (scale < 1.0f) scale = 1.0f;
    if (scale > 2.0f) scale = 2.0f;
    return scale;
#else
    (void)w; (void)h;
    return 1.0f;
#endif
}

static void touch_size(float *w, float *h)
{
    *w = (float)renderer_vw();
    *h = (float)renderer_vh();
    if (*w < 1.0f) *w = 1920.0f;
    if (*h < 1.0f) *h = 1080.0f;
}

static void set_action(ActionId id, bool on)
{
    action_set(ACTION_SRC_TOUCH, id, on);
}

int touch_available(void)
{
#ifdef __ANDROID__
    return 1; // on a phone it is always touch; SDL_GetTouchDevices may return 0 before the first tap
#endif
    if (s_avail < 0) {
        int count = 0;
        SDL_TouchID *ids = SDL_GetTouchDevices(&count);
        SDL_free(ids);
        s_avail = count > 0 ? 1 : 0;
    }
    return s_avail;
}

// Single switch for touch elements. Default — automatic: Android or
// touchscreen -> 1, plain desktop -> 0. The user overrides it in config.cfg.
static void touch_register_cvar(void)
{
    static int done = 0;
    if (done) return;
    done = 1;
    int auto_on =
#ifdef __ANDROID__
        1;
#else
        touch_available();
#endif
    cvar_register_ex("r_touch_ui", (float)auto_on, 0.0f, 1.0f, CVAR_ARCHIVE,
                     "тач-элементы управления (кнопка прыжка, джойстик)");
}

void touch_set_ui_mode(int ui_mode)
{
    if (!touch_available()) return;
    if (ui_mode == s_ui_mode) return;
    s_ui_mode = ui_mode;
    // SDL reads the hint live (there is a hint callback): in menus taps = mouse
    // clicks, in game the synthesis is off — the camera doesn't jerk.
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, ui_mode ? "1" : "0");
    if (!ui_mode) touch_reset();
}

void touch_reset(void)
{
    s_joystick = s_look = 0;
    s_joy_kx = s_joy_ky = 0;
    s_look_dx = s_look_dy = s_look_moved = 0;
    s_jump_pulse = 0;
    action_clear_source(ACTION_SRC_TOUCH);
}

void touch_frame(void)
{
    if (s_jump_pulse > 0 && --s_jump_pulse == 0)
        set_action(A_JUMP, false);   // jump pulse ended
}

void touch_look_take(float *dx, float *dy)
{
    *dx = s_look_dx;
    *dy = s_look_dy;
    s_look_dx = s_look_dy = 0;
}

static void joy_apply(float dx, float dy, float radius)
{
    float nx = dx / radius, ny = dy / radius;
    float len = SDL_sqrtf(nx * nx + ny * ny);
    if (len > 1.0f) {
        s_joy_kx = dx / len;
        s_joy_ky = dy / len;
    } else {
        s_joy_kx = dx;
        s_joy_ky = dy;
    }
    if (len < JOY_DEAD) {
        set_action(A_FORWARD, false); set_action(A_BACK, false);
        set_action(A_LEFT, false); set_action(A_RIGHT, false);
        set_action(A_SPEED, false);
        return;
    }
    set_action(A_SPEED, len > 0.85f);
    nx /= len;
    ny /= len;
    set_action(A_FORWARD, ny < -0.45f);
    set_action(A_BACK, ny > 0.45f);
    set_action(A_LEFT, nx < -0.45f);
    set_action(A_RIGHT, nx > 0.45f);
}

int touch_event(const SDL_Event *ev)
{
    if (!touch_available()) return 0;

    switch (ev->type) {
    case SDL_EVENT_FINGER_DOWN: {
        float w, h;
        touch_size(&w, &h);
        float px = ev->tfinger.x * w, py = ev->tfinger.y * h;
        // Jump button — bottom-right corner 160x160 (scaled the same way as in hud_draw)
        float jb = 140.0f * touch_scale(w, h);
        if (px > w - jb - 30.0f && py > h - jb - 30.0f) {
            s_jump_pulse = 6; set_action(A_JUMP, true); return 1;
        }
        if (!s_joystick && px < w * JOY_ZONE) {
            s_joystick = 1;
            s_joy_id = ev->tfinger.fingerID;
            s_joy_ox = px;
            s_joy_oy = py;
            joy_apply(0, 0, JOY_BASE_R * touch_scale(w, h));
            return 1;
        }
        if (!s_look) {
            s_look = 1;
            s_look_id = ev->tfinger.fingerID;
            s_look_moved = 0;
            s_look_t0 = SDL_GetTicks();
        }
        return 1;
    }
    case SDL_EVENT_FINGER_MOTION: {
        float w, h;
        touch_size(&w, &h);
        float px = ev->tfinger.x * w, py = ev->tfinger.y * h;
        if (s_joystick && ev->tfinger.fingerID == s_joy_id) {
            joy_apply(px - s_joy_ox, py - s_joy_oy,
                      JOY_BASE_R * touch_scale(w, h));
            return 1;
        }
        if (s_look && ev->tfinger.fingerID == s_look_id) {
            // Sensitivity — the same cl_sens cvar (radians/pixel) as for the
            // mouse: the camera receives pixels and multiplies itself.
            s_look_dx += ev->tfinger.dx * w;
            s_look_dy += ev->tfinger.dy * h;
            s_look_moved += SDL_fabsf(ev->tfinger.dx * w) +
                            SDL_fabsf(ev->tfinger.dy * h);
            return 1;
        }
        return s_look || s_joystick;
    }
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_CANCELED: {
        if (s_joystick && ev->tfinger.fingerID == s_joy_id) {
            s_joystick = 0;
            set_action(A_FORWARD, false); set_action(A_BACK, false);
            set_action(A_LEFT, false); set_action(A_RIGHT, false);
            set_action(A_SPEED, false);
            return 1;
        }
        if (s_look && ev->tfinger.fingerID == s_look_id) {
            // Short tap without movement — jump (a pulse for a few frames
            // so that camera_update definitely sees the flag between frames)
            if (SDL_GetTicks() - s_look_t0 < TAP_MS && s_look_moved < TAP_MOVE_PX) {
                s_jump_pulse = 3;
                set_action(A_JUMP, true);
            }
            s_look = 0;
            return 1;
        }
        return 0;
    }
    default:
        return 0;
    }
}

void touch_hud_draw(int vw, int vh)
{
    // Buttons/joystick gated by the r_touch_ui cvar (default — auto by touch presence).
    // On a plain desktop this is a stray artifact in the corner; there it is off.
    touch_register_cvar();
    if (cvar_get("r_touch_ui") < 0.5f) return;
    // On a phone the joystick is bigger + the jump button on the right is always visible
    const float base[4] = { 0.5f, 0.5f, 0.5f, 0.28f };
    const float knob[4] = { 1.0f, 1.0f, 1.0f, 0.40f };
    // Jump button — bottom-right corner, 140x140 px. The image is
    // img/ui/jump.png; no file -> the engine shows img/notfound.png
    // (Source style, see hud_button_draw / resources_texture).
    float jb = 140.0f;
    float scale = touch_scale((float)vw, (float)vh);
    jb *= scale;
    const float tint[4] = { 1.0f, 1.0f, 1.0f, 0.85f };
    hud_button_draw(vw, vh, "img/ui/jump.png",
                    (float)vw - jb - 30.0f, (float)vh - jb - 30.0f,
                    jb, jb, tint);

    if (!s_joystick) return;
    float bs = JOY_BASE_R * 2.0f * scale, ks = 68.0f * scale;
    hud_rect_draw(vw, vh, s_joy_ox - bs * 0.5f, s_joy_oy - bs * 0.5f, bs, bs, base);
    hud_rect_draw(vw, vh, s_joy_ox + s_joy_kx - ks * 0.5f,
                  s_joy_oy + s_joy_ky - ks * 0.5f, ks, ks, knob);
}
