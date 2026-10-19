// ============================================================================
//  gamepad.h — gamepad control (see gamepad.c)
// ----------------------------------------------------------------------------
//  Works on any platform where SDL sees a gamepad (Linux/Windows/Android).
//  Left stick — movement (g_act flags), right stick — look, A — jump,
//  B — crouch, RB — run, Start — pause/back.
// ============================================================================
#ifndef GAMEPAD_H
#define GAMEPAD_H

#include <SDL3/SDL.h>

// Handles SDL_EVENT_GAMEPAD_* in the game state. 1 — consumed.
int gamepad_event(const SDL_Event *ev);

// Per-frame update: sticks are smoothed and keep g_act flags while a stick is deflected.
void gamepad_frame(void);

// Takes the look delta from the right stick (rad/frame, already multiplied by sens).
void gamepad_look_take(float *dx, float *dy);

// Reset when leaving into the menu/console.
void gamepad_reset(void);

#endif
