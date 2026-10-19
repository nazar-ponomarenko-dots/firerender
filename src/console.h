// ============================================================================
//  console.h — developer console (GoldSrc/X-Ray style)
// ----------------------------------------------------------------------------
//  Key ` (~) — open/close. While open: the mouse cursor is visible,
//  text input works (SDL text input), the game ignores keys.
//
//  Line editing: ←/→ — cursor, Home/End, Delete, Backspace (UTF-8 aware),
//  Ctrl+V — paste from clipboard, Ctrl+C — copy, Ctrl+A/E — line
//  start/end, Ctrl+U/K — erase after/before the cursor, Ctrl+W — erase a word.
//  Tab — completes command/variable names (cycles through matches).
//  Scrolling output: mouse wheel, PgUp/PgDn, Home/End.
//  Enter — execute, ↑/↓ — history, Esc — close.
//
//  SDL_Log output lands in the console (cvar "developer"). Commands are
//  registered via console_add_command — later Lua will register its own the
//  same way. Built-in commands: help, cvarlist, set, exec,
//  clear, cfg, echo, quit, version.
// ============================================================================
#ifndef CONSOLE_H
#define CONSOLE_H
#include <SDL3/SDL.h>

typedef void (*ConsoleCmdFn)(const char *args);

#define CON_MAX_COMMANDS 128

// "quit requested" flag — set by the quit command, read by the main loop.
extern int g_quit_request;

// Initialization: default cvars, built-in commands, interception of
// SDL_Log into the console ring buffer. Call once after creating the GL context.
void console_init(void);

// Open/close (key `). Emits EV_CONSOLE_OPENED/EV_CONSOLE_CLOSED.
void console_toggle(void);
int console_active(void);

// Feed an SDL event to the console (text input, keys). Returns 1 if the
// event was consumed by the console — the main loop must not process it further.
int console_event(const SDL_Event *ev);

// Print a line to the console (printf-style).
void console_print(const char *fmt, ...);

// Register a command. name — the command name, help — the hint text,
// fn — the handler (args — the rest of the line after the command name).
int console_add_command(const char *name, const char *help, ConsoleCmdFn fn);

// Execute a command line (from Enter or from code). `wait` / `wait N`
// in the line defer everything after them by 1 / N frames.
void console_exec(const char *line);

// Deferred execution: run cmd after `frames` frames (wait/console_later).
// The main loop calls console_drain() every frame.
void console_later(int frames, const char *cmd);
void console_drain(void);

// Draws the console (panel, output lines with a scrollbar, input line
// with a cursor). Call from the main loop AFTER rendering the UI.
void console_render(int vw, int vh);

// Autoload config.cfg at startup and save it on exit.
void console_autoload(void);
void console_save_config(void);

// Self-test: commands, cvars, input (SDL events), history, Tab.
// Started by FR_TEST=x; prints the result via SDL_Log.
void console_selftest(void);

#endif
