// ============================================================================
//  binds.h — key bindings to commands (Half-Life style)
// ----------------------------------------------------------------------------
//  bind <key> <command> — bind a key. On press the command runs
//  (or a ";"-separated combination), on release — "-command"
//  (for commands with a "+" prefix). Binds are stored in config.cfg
//  ("bind key command" lines) and survive restarts.
//  Default binds are installed only if config.cfg has no binds at all.
// ============================================================================
#ifndef BINDS_H
#define BINDS_H
#include <SDL3/SDL.h>
#include <stdio.h>

#define BINDS_MAX 64

typedef struct {
    SDL_Scancode sc;     // key (by scancode — works under any layout)
    char cmd[96];        // command (may contain ";" — a combination)
} Bind;

// Virtual mouse wheel "keys" (they have no real scancode)
#define BIND_WHEEL_UP   ((SDL_Scancode)SDL_SCANCODE_COUNT)
#define BIND_WHEEL_DOWN ((SDL_Scancode)(SDL_SCANCODE_COUNT + 1))

// Default binds (if config.cfg does not define its own)
void binds_init(void);

// Reset to defaults (bind_reset command, "Сброс управления" menu)
void binds_reset(void);

// Execute a key's bind: down=1 — the command, down=0 — "-command" for "+"
void binds_key(SDL_Scancode sc, int down);

// Mouse wheel: up=1 — up, 0 — down (impulse action)
void binds_wheel(int up);

// Key name for the console/menu (incl. mouse_wheel_up/down)
const char *binds_key_name(SDL_Scancode sc);

// Name -> key (incl. mouse_wheel_up/down)
SDL_Scancode binds_key_from_name(const char *name);

// Command bound to the key (NULL — none)
const char *binds_cmd(SDL_Scancode sc);

// Bind/unbind
void binds_set(SDL_Scancode sc, const char *cmd);
void binds_clear(SDL_Scancode sc);
void binds_clearall(void);

// First key the command is bound to (for the settings menu)
SDL_Scancode binds_key_for_cmd(const char *cmd);

// Access by index (bindlist, menus)
int binds_count(void);
const Bind *binds_at(int i);

// config.cfg: loading bind lines and saving all binds
void binds_load_config(void);
void binds_save_config(FILE *f);

#endif
