// ============================================================================
//  lua_engine.h — embedded Lua: engine scripts (see lua_engine.c)
// ----------------------------------------------------------------------------
//  The engine can be customized with Lua scripts (lua/main.lua): the script
//  gets an `engine` table bound to the console, cvars and events.
//  This is "phase 4" of the original plan: events/entities were designed so
//  that Lua can listen to and register them without touching the core.
// ============================================================================
#ifndef LUA_ENGINE_H
#define LUA_ENGINE_H

// Creates the Lua state, opens the standard libraries, attaches the
// `engine` table and loads lua/main.lua (errors go to the console). Call
// in the APP_BOOT phase, AFTER console_init (console_print is required).
void lua_engine_init(void);

// Every frame: timers (engine.after) and the script's update(dt) function.
void lua_engine_update(float dt);

// Map script: loads lua/maps/<map>.lua AFTER the world is loaded.
// A missing file is not an error. Call at the end of game_load_world.
void lua_engine_map(const char *map_name);

// Map unload: removes the map script's update callbacks and timers
// (engine-level main.lua scripts stay). Call on game_unload_world.
void lua_engine_clear(void);

#endif
