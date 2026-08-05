// ============================================================================
//  input.c — ввод: события главного цикла и режим мыши (см. input.h)
// ----------------------------------------------------------------------------
//  Вынесено из main.c (строки 659–688 и разбор событий главного цикла):
//  консоль перехватывает ввод первой, затем бинды (HL-стиль), ESC —
//  пауза/назад; ресайз окна обновляет размеры сцены и FBO рендера.
// ============================================================================
#include "input.h"

#include "app.h"
#include "binds.h"
#include "console.h"
#include "gl_loader.h"
#include "renderer.h"
#include "world.h"

#include <SDL3/SDL.h>

static SDL_Window *s_win = NULL;
static Game *s_game = NULL;     // состояние мира (для ресайза)
static int s_ui_mode = -1;      // текущий режим мыши
static int s_text_input = -1;   // текущее состояние text input
static int s_quit = 0;          // SDL_EVENT_QUIT получен

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

// --- Синк режима ввода ---------------------------------------------------------
void input_sync_mode(void)
{
    bool ui_mode = (app_state() != APP_PLAY) || console_active();
    if (ui_mode != s_ui_mode) {
        s_ui_mode = ui_mode;
        if (s_win) SDL_SetWindowRelativeMouseMode(s_win, !ui_mode);
        if (ui_mode) {
            SDL_ShowCursor();
        } else {
            SDL_HideCursor();
            // Сброс накопленного движения мыши: в меню курсор двигался в
            // absolute-режиме, и без сброса камера дёрнется на весь этот путь.
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

// --- Разбор событий кадра -------------------------------------------------------
void input_process(void)
{
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_EVENT_QUIT) s_quit = 1;
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

        // Бинды (Half-Life): клавиша -> команда. KEYDOWN выполняет
        // команду (или комбинацию "cmd1; cmd2"), KEYUP снимает "+действия".
        // Запись биндов из меню "Клавиши" перехватывает клавиши первой.
        if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat) {
            if (app_bind_key(ev.key.scancode)) continue;
            binds_key(ev.key.scancode, 1);
        } else if (ev.type == SDL_EVENT_KEY_UP) {
            binds_key(ev.key.scancode, 0);
        }
        // Колесо мыши — бинды (bind mouse_wheel_up "cmd"):
        // например bind mouse_wheel_up "set fov 90"
        if (ev.type == SDL_EVENT_MOUSE_WHEEL && ev.wheel.y != 0) {
            binds_wheel(ev.wheel.y > 0);
        }

        // ESC: пауза (в игре), назад (в меню) — обрабатывается в app
        if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat &&
            ev.key.key == SDLK_ESCAPE) {
            app_escape();
            continue;
        }

        // Прочие клавиши — только во время игры (в меню ими управляет UI)
        if (!s_game->world_loaded || app_state() != APP_PLAY) continue;
    }
}
