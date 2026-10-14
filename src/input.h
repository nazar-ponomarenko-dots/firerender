// ============================================================================
//  input.h — input: main-loop events and mouse-mode synchronization
// ----------------------------------------------------------------------------
//  Extracted from main.c: SDL event dispatch (console -> binds -> ESC),
//  window resize and switching mouse/text-input mode (menu <-> game).
// ============================================================================
#ifndef INPUT_H
#define INPUT_H
#include <SDL3/SDL.h>
#include "game.h"

// Stores the window and game state (for resizing). Call after
// window creation and game_init.
void input_init(SDL_Window *win, Game *g);

// Dispatches all events of the frame: console, resize, binds, ESC.
// Call from the main loop.
void input_process(void);

// Synchronizes mouse mode and text input with the game/menu/console state.
void input_sync_mode(void);

// 1 — SDL_EVENT_QUIT was received (the main loop is shutting down).
int input_quit(void);

// Android/window lifecycle: background stops simulation/render/swap;
// resume is reported once so the main loop resets the clock accumulator.
int input_background(void);
int input_take_resumed(void);

#endif
