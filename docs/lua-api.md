# Lua API (`engine.*`)

The engine embeds Lua 5.4 (vendored, `third_party/lua`). Scripts are a
data-driven layer on top of the console: they see the same cvars and commands as
the player, plus a few direct engine calls. Implementation — `src/lua_engine.c`,
surface — the `engine_lib` table (`lua_engine.c:863-905`).

## Load order

1. `boot_init` calls `lua_engine_init()` **after** the console, cvars, binds
   and FX presets (`main.c:215-227`).
2. `lua/main.lua` is loaded — the engine script (through the resource system,
   it may live in a map pak). If the file is missing, the log
   shows `lua: нет lua/main.lua — скрипты выключены`, and no further
   API calls happen.
3. After the world loads (at the end of `game_load_world`) the engine loads
   `lua/maps/<map_name>.lua` (`lua_engine_map`, `lua_engine.c:818-844`).
   A missing file is not an error. The script already sees the spawned entities.
4. On map unload `lua_engine_clear()` removes **only** the state added by the map
   script: update callbacks, timers and event subscriptions, starting from the
   indices recorded at step 3. The engine's `main.lua` stays.

Compile and runtime errors do not crash the game: they are printed to the console
(`console_print`), and `pcall` pops the message off the stack.

## Sandbox

Only `base` (without `dofile`/`loadfile`/`load` — they are nulled out,
`lua_engine.c:920-922`), `string`, `table`, `math` are available. `os`, `io`,
`package`, `debug` are not opened: a map script cannot read files or spawn
processes. Access to the engine is only through `engine.*`.

## Limits

| What | Limit |
|---|---|
| Commands from Lua (`engine.cmd`) | 16 (`LUA_CMD_MAX`) |
| Lua entity classes | 16 (`LUACLS_MAX`) |
| Subscriptions per event | 4 (`LUA_EV_MAX`) |
| `engine.after` timers | 32 (`LUA_TIMERS_MAX`) |
| Update callbacks | 8 (`LUA_UPDATE_MAX`) |
| `engine.material_register` | up to `MATERIALS_MAX` in the `material.c` registry |

Re-registering a command or class (map reload) reuses the slot by name — there is
no reference leak.

---

## Console and variables

| Call | Returns | Note |
|---|---|---|
| `engine.log(msg)` | — | Print to the engine console |
| `engine.exec(line)` | — | `console_exec`: the same `;` and `wait` semantics as in the console |
| `engine.cmd(name, fn)` | — | New console command; `fn(args)` gets the whole argument string |
| `engine.cvar_get(name)` | number | 0 for a non-existent variable |
| `engine.cvar_set(name, v)` | — | Clamped to the range; cheats (`CVAR_CHEAT`) are not changed while `sv_cheats 0` |
| `engine.cvar_get_string(name)` | string | For `CVAR_STRING` (e.g. `name`) |
| `engine.cvar_set_string(name, s)` | — | |
| `engine.bind(key, cmd)` / `engine.unbind(key)` | — | Key name — an SDL scancode name or `mouse_wheel_up`/`mouse_wheel_down`; unknown key → message to the console |
| `engine.msg(text)` | — | Map message in the center of the screen, 4 seconds (as with triggers) |
| `engine.time()` | number | Seconds since engine start |

## Events and loop

| Call | What |
|---|---|
| `engine.on("event", fn)` | Subscription; `fn(type, a, b)` gets the event code and two `lightuserdata` parameters |
| `engine.update(fn)` | Called every frame: `fn(dt)` |
| `engine.after(sec, fn)` | One-shot timer |

Events: `entity_spawn`, `entity_kill`, `console_open`, `console_close`
(`ev_type_from_name`, `lua_engine.c:139-146`). An unknown name →
`engine.on: неизвестное событие`.

Callbacks added during an `update`/timer are not called in the same tick — they
shift to the next frame (`lua_engine.c:748-812`).

## Entities

An entity is passed to Lua as a **handle** (`int`), not a pointer: a dead entity
gives `nil`/an empty answer, no dangling reference exists.

| Call | Returns |
|---|---|
| `engine.entity_class(name, { spawn=fn, think=fn, kill=fn })` | — ; callbacks are called as `fn(handle, dt)` |
| `engine.entity_spawn(class, [x, y, z])` | handle or `nil` |
| `engine.entity_kill(h)` | — |
| `engine.entity_find(class, [n])` | handle of the n-th entity of the class (0 — the first) or `nil` |
| `engine.entity_count()` | number |
| `engine.entity_pos(h)` | `x, y, z` (zeros for a dead one) |
| `engine.entity_set_pos(h, x, y, z)` | — |
| `engine.entity_classname(h)` | string (empty for a dead one) |
| `engine.world_loaded()` | bool |

## World, camera, player

| Call | Returns / does |
|---|---|
| `engine.camera()` | `x, y, z, yaw, pitch` (the player view; zeros outside a game) |
| `engine.terrain_height(x, z)` | terrain height, 0 before the world loads |
| `engine.teleport(x, y, z, [yaw], [pitch])` | teleport the player (angles in radians) |
| `engine.player_hp()` | `hp, max, dead` |
| `engine.player_damage(n)` | damage to the player |
| `engine.player_heal([n])` | healing; without n — to full |
| `engine.player_set_max(n)` | new maximum (tops the player up to full) |

## Models, materials, effects

| Call | Returns / does |
|---|---|
| `engine.mesh(path, [x, y, z], [scale], [tex])` | bool — a static `.obj` as a scene node |
| `engine.glb(path, [x, y, z], [scale])` | bool — `.glb`; if it has animation the node becomes a skinned one (`AnimInstance3D`) |
| `engine.material_register(name, { … })` | true; table fields: `tint={r,g,b}`, `alpha`, `blend` (0/1/2/3), `tile={u,v}` |
| `engine.material_list()` | space-separated string of names (for debugging) |
| `engine.fx_burst(name, [x, y, z], [count])` | particle burst |
| `engine.fx_emit(name, [x, y, z], [rate], [life])` | emitter (rate defaults to 10) |
| `engine.fx_stop([name])` | stop emitters |
| `engine.fx_clear()` | all emitters + the pool |
| `engine.fx_weather([name], [rate])` | weather behind the player; `""`/`"off"`/`"clear"` — stop. The area is fixed: ext 20×10×20, height 12 m |
| `engine.fx_register(name, { … })` | custom particle preset (see below) |

Fields of `engine.fx_register` (`lua_engine.c:644-711`) and their defaults:

| Field | Default | Meaning |
|---|---|---|
| `dir` | `{0, 1, 0}` | Emission direction |
| `spread` | 0.5 | Spread, radians |
| `speed` | `{1, 2}` | min/max, m/s |
| `life` | `{0.5, 1.0}` | min/max, seconds |
| `size` | `{0.2, 0.2}` | min/max, meters |
| `alpha` | `{1.0, 0.0}` | from/to |
| `color` | `{1, 1, 1}` | RGB |
| `gravity` | 0 | m/s² |
| `drag` | 0 | Drag |
| `sway` | 0 | Sway |
| `wind` | `{0, 0, 0}` | Wind |
| `blend` | 0 | 1 = additive blending |
| `streak` | 0 | 1 = stretched along velocity |
| `plain` | 0 | 1 = no lighting/texture |
| `bounce` | 0 | 1 = bounce off the terrain |
| `collide` | 0 | 1 = dies on terrain/water, 2 = lands and fades out (snow) |
| `count` | 8 | Particles per burst |

## Example (`lua/main.lua`, abridged)

```lua
engine.cmd("lua_greet", function(args)
    engine.log("[Lua] привет, " .. ((args ~= "" and args) or "мир") .. "!")
end)

engine.on("console_open",  function() engine.log("[Lua] консоль открыта")  end)
engine.on("console_close", function() engine.log("[Lua] консоль закрыта") end)

engine.entity_class("beacon", {
    spawn = function(e) engine.log("[Lua] маяк: " .. engine.entity_classname(e)) end,
    think = function(e, dt)
        local x, y, z = engine.entity_pos(e)
        engine.entity_set_pos(e, x, 5 + math.sin(engine.time()) * 1.5, z)
    end,
    kill  = function() engine.log("[Lua] маяк исчез") end,
})

engine.update(function(dt)
    if not world_up and engine.world_loaded() then
        world_up = true
        engine.entity_spawn("beacon", 1000, 5, 562)
    end
end)
```

## See also

- [commands.md](commands.md) — what `engine.exec` invokes
- [cvars.md](cvars.md) — what `engine.cvar_set` tunes
- [map-json.md](map-json.md) — the `cvars`/`commands` sections inside a map
