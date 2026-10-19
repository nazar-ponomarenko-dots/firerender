// ============================================================================
//  touch.h — touch controls (see touch.c)
// ============================================================================
#ifndef TOUCH_H
#define TOUCH_H

#include <SDL3/SDL.h>

// Handles a touch event (SDL_EVENT_FINGER_*). Called from input_process
// AFTER console_event and only in the playing state (APP_PLAY).
// Returns 1 if the event was consumed by the touch controls.
int touch_event(const SDL_Event *ev);

// UI-mode toggle (menu/console): enables synthetic mouse events generated
// from touch (taps on menu buttons) or disables them (in game, touch drives
// the virtual joystick and look). Called from input_sync_mode.
void touch_set_ui_mode(int ui_mode);

// Per-frame update: resets the tap-to-jump pulse. Called once per frame.
void touch_frame(void);

// Takes the accumulated look delta from a swipe (in window pixels).
// Counters are zeroed after reading. Called from camera.c once per frame.
void touch_look_take(float *dx, float *dy);

// Draws the virtual joystick over the HUD (only while a finger is on the joystick).
void touch_hud_draw(int vw, int vh);

// Reset: a finger "escaped" due to switching into menu/console — release everything.
void touch_reset(void);

// Whether a touch device exists (0 on desktop — the module sleeps)
int touch_available(void);

#endif
