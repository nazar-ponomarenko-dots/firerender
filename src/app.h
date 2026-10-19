// ============================================================================
//  app.h — application state: main menu / game / pause
// ----------------------------------------------------------------------------
//  Like Half-Life 1: launch goes to the main menu (a live scene runs behind it
//  as the background), ESC in game opens the pause menu, settings screens go
//  back. ALL menu settings write into cvar's (applied immediately) and are
//  saved to config.cfg when leaving the settings screen and when quitting game.
// ============================================================================
#ifndef APP_H
#define APP_H
#include <SDL3/SDL.h>

typedef enum {
    APP_BOOT = 0,    // startup: system init (boot_init phase in main.c)
    APP_MENU,        // main menu (world frozen, scene as background)
    APP_PLAY,        // game: world updates, HUD visible
    APP_PAUSE        // pause (ESC): menu over the frozen scene
} AppState;

// Actions the menu reports to the main loop
enum {
    APP_ACT_NONE = 0,
    APP_ACT_START,     // new game: load the world and teleport to spawn
    APP_ACT_RESUME,    // resume
    APP_ACT_MAIN,      // back to the main menu (world is unloaded)
    APP_ACT_QUIT,      // quit
};

// Initial state (APP_MENU, main menu screen)
void app_init(void);

AppState app_state(void);
void app_set_state(AppState s);

// Has the game been started at least once? (for the "Продолжить" item in the main menu)
int app_in_game(void);

// ESC: game -> pause, pause -> game, in menus — back through the screens
void app_escape(void);

// Bind recording from the "Клавиши" menu: return 1 if the key was consumed
// by the recorder (call from the main loop on KEYDOWN, BEFORE game keys).
int app_bind_key(SDL_Scancode sc);

// Draws the current menu screen (ui_begin/ui_end inside). Returns the action
// for the main loop (APP_ACT_*). Call only when app_state() != APP_PLAY.
int app_menu(int vw, int vh);

// Is the menu background img/menu.png (loaded into a texture)? If not —
// the frozen scene serves as the background (the main loop renders it).
int app_bg_has(void);

// Apply an exact graphics preset without writing config.cfg.
// Used by the reproducible benchmark before the world is loaded.
void app_apply_benchmark_preset(int level);

// Prints the map list to the console (maps command)
void app_maps_print(void);

#endif
