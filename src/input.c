// ============================================================================
//  input.c — input: main-loop events and mouse mode (see input.h)
// ----------------------------------------------------------------------------
//  Moved out of main.c (lines 659–688 and the main-loop event parsing):
//  the console grabs input first, then the binds (HL-style), ESC —
//  pause/back; a window resize updates the scene sizes and render FBO.
// ============================================================================
#include "input.h"

#include "app.h"
#include "actions.h"
#include "binds.h"
#include "console.h"
#include "gamepad.h"
#include "gl_loader.h"
#include "renderer.h"
#include "touch.h"
#include "world.h"

#include <SDL3/SDL.h>

static SDL_Window *s_win = NULL;
static Game *s_game = NULL;     // world state (for resize)
static int s_ui_mode = -1;      // current mouse mode
static int s_text_input = -1;   // current text input state
static int s_quit = 0;          // SDL_EVENT_QUIT received
static int s_background = 0;
static int s_resumed = 0;

static void input_release_all(void)
{
    action_clear_source(ACTION_SRC_BIND);
    action_clear_source(ACTION_SRC_TOUCH);
    action_clear_source(ACTION_SRC_GAMEPAD);
    gamepad_reset();
    touch_set_ui_mode(1);
}

void input_init(SDL_Window *win, Game *g)
{
    s_win = win;
    s_game = g;
    s_ui_mode = -1;
    s_text_input = -1;
}

int input_quit(void)
{
    return s_quit;
}

int input_background(void)
{
    return s_background;
}

int input_take_resumed(void)
{
    int resumed = s_resumed;
    s_resumed = 0;
    return resumed;
}

// --- Input mode sync ---------------------------------------------------------
void input_sync_mode(void)
{
    bool ui_mode = (app_state() != APP_PLAY) || console_active();
    if (ui_mode != s_ui_mode) {
        s_ui_mode = ui_mode;
        touch_set_ui_mode(ui_mode);
        gamepad_reset();
        if (s_win) SDL_SetWindowRelativeMouseMode(s_win, !ui_mode);
        if (ui_mode) {
            SDL_ShowCursor();
        } else {
            SDL_HideCursor();
            // Flush accumulated mouse motion: in menus the cursor moved in
            // absolute mode, and without a flush the camera would jolt by
            // that whole distance.
            float mx = 0, my = 0;
            SDL_GetRelativeMouseState(&mx, &my);
            SDL_GetRelativeMouseState(&mx, &my);
        }
        camera_lock_input(ui_mode ? 1 : 0);
    }
    bool ti = console_active();
    if (ti != s_text_input) {
        s_text_input = ti;
        if (s_win) {
            if (ti) SDL_StartTextInput(s_win);
            else SDL_StopTextInput(s_win);
        }
    }
}

// --- Per-frame event processing ------------------------------------------------
void input_process(void)
{
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_EVENT_QUIT || ev.type == SDL_EVENT_TERMINATING)
            s_quit = 1;
        if (ev.type == SDL_EVENT_WINDOW_FOCUS_LOST) input_release_all();
        if (ev.type == SDL_EVENT_WILL_ENTER_BACKGROUND ||
            ev.type == SDL_EVENT_DID_ENTER_BACKGROUND) {
            s_background = 1;
            input_release_all();
        }
        if (ev.type == SDL_EVENT_WILL_ENTER_FOREGROUND ||
            ev.type == SDL_EVENT_DID_ENTER_FOREGROUND) {
            s_background = 0;
            s_resumed = 1;
            s_ui_mode = -1;
            s_text_input = -1;
        }
        if (ev.type == SDL_EVENT_WINDOW_FOCUS_GAINED) {
            s_ui_mode = -1;
            s_text_input = -1;
        }
        if (console_event(&ev)) continue;
        if (ev.type == SDL_EVENT_WINDOW_RESIZED ||
            ev.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
            int vw = 0, vh = 0;
            SDL_GetWindowSizeInPixels(s_win, &vw, &vh);
            if (vw > 0 && vh > 0) {
                glViewport(0, 0, vw, vh);
                renderer_set_viewport(vw, vh);
                if (s_game->cam) s_game->cam->aspect = (float)vw / vh;
                if (s_game->mm) { s_game->mm->vw = vw; s_game->mm->vh = vh; }
                if (s_game->hud) { s_game->hud->vw = vw; s_game->hud->vh = vh; }
                if (s_game->world_loaded) renderer_resize(vw, vh);
            }
            continue;
        }
        if (console_active()) continue;

        if (gamepad_event(&ev)) continue;
#ifdef __ANDROID__
        if (s_game->world_loaded && app_state() == APP_PLAY) {
            if (touch_event(&ev)) continue;
        }
#endif

        // Binds (Half-Life): key -> command. KEYDOWN runs the command
        // (or a "cmd1; cmd2" combination), KEYUP cancels the "+actions".
        // Bind recording from the "Keys" menu intercepts keys first.
        if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat) {
            if (app_bind_key(ev.key.scancode)) continue;
            binds_key(ev.key.scancode, 1);
        } else if (ev.type == SDL_EVENT_KEY_UP) {
            binds_key(ev.key.scancode, 0);
        }
        // Mouse wheel — binds (bind mouse_wheel_up "cmd"):
        // e.g. bind mouse_wheel_up "set fov 90"
        if (ev.type == SDL_EVENT_MOUSE_WHEEL && ev.wheel.y != 0) {
            binds_wheel(ev.wheel.y > 0);
        }

        // ESC / Android Back (back gesture): pause/back — handled in app
        if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat &&
            (ev.key.key == SDLK_ESCAPE || ev.key.key == SDLK_AC_BACK)) {
            app_escape();
            continue;
        }

        if (!s_game->world_loaded || app_state() != APP_PLAY) continue;
    }
#ifdef __ANDROID__
    touch_frame();
#endif
    gamepad_frame();
}
