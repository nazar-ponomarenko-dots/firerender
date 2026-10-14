// ============================================================================
//  cmdreg.h — the SINGLE registry of console commands (see cmdreg.c)
// ----------------------------------------------------------------------------
//  Commands used to be spread across 4 places: boot_init in main.c (28 of
//  them), actions.c (+/- movement commands), fx.c (effect commands) and
//  dynamically from Lua. Every new module called console_add_command in its
//  own init — the order and the limits were hard to track, and an ordering
//  slip could "lose" a command (the table is capped at CON_MAX_COMMANDS,
//  extras are silently not added).
//
//  Now ALL static commands are described ONCE in the s_cmds table in
//  cmdreg.c (name + help + handler) and registered by a single
//  cmdreg_register_all() call. Dynamic commands (engine.cmd from Lua) still
//  go through console_add_command — the user adds them at runtime.
// ============================================================================
#ifndef CMDREG_H
#define CMDREG_H
#include "console.h"   // ConsoleCmdFn

// A command entry in the central registry.
typedef struct {
    const char *name;      // command name ("map", "+forward", ...)
    const char *help;      // help text shown by `help`
    ConsoleCmdFn fn;       // handler (const char *args)
} ConCmdDesc;

// Registers ALL static commands from the central s_cmds table.
// Call once after console_init (in boot_init). This is the ONLY registration
// point for static commands of the whole engine.
void cmdreg_register_all(void);

#endif