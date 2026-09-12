// ============================================================================
//  gamepad.c — управление с геймпада (см. gamepad.h)
// ----------------------------------------------------------------------------
//  SDL_Event-driven: стики приходят как SDL_EVENT_GAMEPAD_AXIS_MOTION, кнопки
//  — как BUTTON_DOWN/UP. Флаги движения ставим те же (g_act), что и клавиши —
//  физика/камера не знают, откуда пришёл ввод. Обзор правого стика копится в
//  пиксельную дельту, которую camera.c прибавляет к движению мыши.
// ============================================================================
#include "gamepad.h"

#include "actions.h"
#include "app.h"
#include "cvars.h"

#include <SDL3/SDL.h>
#include <string.h>

#define STICK_DEAD  0.35f   // мёртвая зона стиков
#define LOOK_SPEED  1200.0f // пикселей обзора в секунду при полном отклонении

static SDL_Gamepad *s_pad = NULL;
static float s_lx, s_ly, s_rx, s_ry;   // стики, -1..1
static bool s_btn_jump, s_btn_duck, s_btn_rb, s_btn_lstick;
static float s_look_dx, s_look_dy;
static Uint64 s_last_frame;

static void set_action(ActionId id, bool on)
{
    action_set(ACTION_SRC_GAMEPAD, id, on);
}

// Пороговое состояние стика -> флаги движения (аналоговые скорости в движке
// нет — флаги бинарые, как у клавиатуры)
static void stick_move(void)
{
    set_action(A_LEFT, s_lx < -STICK_DEAD);
    set_action(A_RIGHT, s_lx > STICK_DEAD);
    set_action(A_FORWARD, s_ly < -STICK_DEAD);
    set_action(A_BACK, s_ly > STICK_DEAD);
    float m = SDL_max(SDL_fabsf(s_lx), SDL_fabsf(s_ly));
    set_action(A_SPEED, s_btn_rb || s_btn_lstick || m > 0.92f);
}

static void apply_buttons(void)
{
    set_action(A_JUMP, s_btn_jump);
    set_action(A_DUCK, s_btn_duck);
}

void gamepad_reset(void)
{
    s_lx = s_ly = s_rx = s_ry = 0;
    s_btn_jump = s_btn_duck = s_btn_rb = s_btn_lstick = false;
    s_look_dx = s_look_dy = 0;
    s_last_frame = 0;
    action_clear_source(ACTION_SRC_GAMEPAD);
}

int gamepad_event(const SDL_Event *ev)
{
    switch (ev->type) {
    case SDL_EVENT_GAMEPAD_ADDED: {
        if (!s_pad) {
            s_pad = SDL_OpenGamepad(ev->gdevice.which);
            if (s_pad) SDL_Log("gamepad: %s", SDL_GetGamepadName(s_pad));
        }
        return 1;
    }
    case SDL_EVENT_GAMEPAD_REMOVED:
        if (s_pad && SDL_GetGamepadID(s_pad) == ev->gdevice.which) {
            SDL_CloseGamepad(s_pad);
            s_pad = NULL;
            gamepad_reset();
        }
        return 1;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        float v = ev->gaxis.value / 32768.0f;
        switch (ev->gaxis.axis) {
        case SDL_GAMEPAD_AXIS_LEFTX:  s_lx = v; break;
        case SDL_GAMEPAD_AXIS_LEFTY:  s_ly = v; break;
        case SDL_GAMEPAD_AXIS_RIGHTX: s_rx = v; break;
        case SDL_GAMEPAD_AXIS_RIGHTY: s_ry = v; break;
        default: return 0;
        }
        stick_move();
        return 1;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        bool down = ev->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        switch (ev->gbutton.button) {
        case SDL_GAMEPAD_BUTTON_SOUTH:          // A — прыжок
            s_btn_jump = down;
            break;
        case SDL_GAMEPAD_BUTTON_EAST:           // B — присесть
            s_btn_duck = down;
            break;
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: // RB — бег
            s_btn_rb = down;
            break;
        case SDL_GAMEPAD_BUTTON_LEFT_STICK:
            s_btn_lstick = down;
            break;
        case SDL_GAMEPAD_BUTTON_START:          // Start — пауза/назад
            if (down) app_escape();
            break;
        default:
            return 0;
        }
        apply_buttons();
        return 1;
    }
    default:
        return 0;
    }
}

void gamepad_frame(void)
{
    Uint64 now = SDL_GetTicks();
    float dt = (s_last_frame ? (float)(now - s_last_frame) / 1000.0f : 0.016f);
    s_last_frame = now;
    if (SDL_fabsf(s_rx) > STICK_DEAD || SDL_fabsf(s_ry) > STICK_DEAD) {
        // Правый стик -> пиксели обзора (камера умножит на cl_sens сам)
        s_look_dx += s_rx * LOOK_SPEED * dt;
        s_look_dy += s_ry * LOOK_SPEED * dt;
    }
}

void gamepad_look_take(float *dx, float *dy)
{
    *dx = s_look_dx;
    *dy = s_look_dy;
    s_look_dx = s_look_dy = 0;
}
