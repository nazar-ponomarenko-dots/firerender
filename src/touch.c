// ============================================================================
//  touch.c — сенсорное управление (см. touch.h)
// ----------------------------------------------------------------------------
//  Схема: SDL конвертирует тапы в мышь, пока мы в меню (SDL_TOUCH_MOUSE_EVENTS
//  = 1 — кнопки UI кликаются пальцем как раньше). В игре хинт выключается, и
//  тач ведёт этот модуль:
//    • левая треть экрана — виртуальный джойстик: направление = флаги g_act
//      (те же, что у "+forward"/"+left" и т.д.), у края — бег;
//    • остальной экран — свайп = обзор (дельту забирает camera.c);
//    • короткий тап справа — прыжок (пульс +jump на 3 кадра).
//  На десктопе (нет тач-устройств) модуль спит и ничего не стоит.
// ============================================================================
#include "touch.h"

#include "actions.h"
#include "hud.h"
#include "cvars.h"
#include "renderer.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <string.h>

// Зоны экрана (доли ширины) — увеличены для телефона
#define JOY_ZONE    0.42f    // левая 42% — джойстик (было 0.35, удобно большим пальцем)
#define JOY_BASE_R  150.0f   // радиус базы джойстика, px (было 90, на 1080p мелко)
#define JOY_DEAD    0.22f    // мёртвая зона чуть меньше — отзывчивее
#define TAP_MS      250      // тап короче этого = прыжок
#define TAP_MOVE_PX 28.0f    // ...и сместился меньше этого (шкала телефона)

static int s_avail = -1;         // кеш: есть ли тач-устройства (-1 — не проверяли)
static int s_ui_mode = 1;        // 1 — меню (синтетика мыши включена)
static int s_joystick = 0;       // палец на джойстике
static SDL_FingerID s_joy_id = 0;
static float s_joy_ox, s_joy_oy; // центр джойстика (точка касания)
static float s_joy_kx, s_joy_ky; // текущее смещение (для рисования)
static int s_look = 0;           // палец обзора
static SDL_FingerID s_look_id = 0;
static float s_look_dx, s_look_dy;    // накопленная дельта для камеры
static float s_look_moved;            // суммарный путь пальца (для тапа)
static Uint64 s_look_t0;              // время касания (для тапа)
static int s_jump_pulse;              // оставшиеся кадры пульса прыжка

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
    return 1; // на телефоне всегда тач, SDL_GetTouchDevices может вернуть 0 до первого тапа
#endif
    if (s_avail < 0) {
        int count = 0;
        SDL_TouchID *ids = SDL_GetTouchDevices(&count);
        SDL_free(ids);
        s_avail = count > 0 ? 1 : 0;
    }
    return s_avail;
}

void touch_set_ui_mode(int ui_mode)
{
    if (!touch_available()) return;
    if (ui_mode == s_ui_mode) return;
    s_ui_mode = ui_mode;
    // Хинт читается SDL живьём (есть hint-колбэк): в меню тапы = клики мыши,
    // в игре синтетика отключена — камеру не дёргает.
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
        set_action(A_JUMP, false);   // пульс прыжка закончился
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
        // Кнопка прыжка — правый нижний угол 160x160 (скейлится как в hud_draw)
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
            // Чувствительность — тот же cvar cl_sens (рад/пиксель), что и у
            // мыши: камера получает пиксели и умножает сама.
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
            // Короткий тап без движения — прыжок (пульс на несколько кадров,
            // чтобы camera_update точно увидел флаг между кадрами)
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
    // На телефоне джойстик крупнее + кнопка прыжка справа всегда видна
    const float base[4] = { 0.5f, 0.5f, 0.5f, 0.28f };
    const float knob[4] = { 1.0f, 1.0f, 1.0f, 0.40f };
    const float jump[4] = { 0.85f, 0.95f, 1.0f, 0.22f };
    // Кнопка прыжка — правый нижний угол, 140x140 px
    float jb = 140.0f;
    float scale = touch_scale((float)vw, (float)vh);
    jb *= scale;
    hud_rect_draw(vw, vh, (float)vw - jb - 30.0f, (float)vh - jb - 30.0f, jb, jb, jump);
    const float ja[4] = { 1.0f, 1.0f, 1.0f, 0.45f };
    hud_rect_draw(vw, vh, (float)vw - jb - 30.0f + jb*0.35f, (float)vh - jb - 30.0f + jb*0.35f, jb*0.3f, jb*0.12f, ja);

    if (!s_joystick) return;
    float bs = JOY_BASE_R * 2.0f * scale, ks = 68.0f * scale;
    hud_rect_draw(vw, vh, s_joy_ox - bs * 0.5f, s_joy_oy - bs * 0.5f, bs, bs, base);
    hud_rect_draw(vw, vh, s_joy_ox + s_joy_kx - ks * 0.5f,
                  s_joy_oy + s_joy_ky - ks * 0.5f, ks, ks, knob);
}
