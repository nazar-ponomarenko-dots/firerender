# Console commands

Command reference. The registry is the `s_cmds` table in `src/cmdreg.c:46-140`
(one table for all static engine commands) plus the console's own commands
(`src/console.c:984-1004`) and commands registered from Lua (`engine.cmd`).
In game: `help` — the list, `help <command>` — help for one.

## Syntax

| Rule | Behavior | Source |
|---|---|---|
| Arguments | The command name is everything up to the first space/tab, the rest is passed to the handler as a single argument string | `console.c:173-184` |
| `;` | Command separator within one line, **except** inside quotes (the body of `alias`/`bind` may contain `;`) | `console.c:228-260` |
| Quotes | String arguments in `"…"` (player name, alias body, path with spaces) | same as above |
| alias recursion | Expanded no deeper than 8 levels — protection against `alias a "a"` | `console.c:170-199` |
| Frame queue | `wait [N]` defers **all subsequent commands in the line** by N frames (1 by default); the queue is bounded by `CON_PEND_MAX` | `console.c:95-105, 214-260` |
| Unknown command | `Unknown command "<имя>" (help — список команд)` | `console.c:201` |

## Console core

| Command | Syntax | What it does |
|---|---|---|
| `help` | `help [command]` | List of commands, or help for one |
| `cvarlist` | — | List of variables with current values |
| `echo` | `echo <text>` | Print text to the console |
| `set` | `set <name> [value]` | Set a cvar (same as `<name> <value>`) |
| `clear` | — | Clear the console log |
| `cfg` | `cfg load` / `cfg save` | Read/write `config.cfg` |
| `exec` | `exec <file>` | Execute commands from a file (looked up via the resource system) |
| `quit` | — | Quit the game |
| `version` | — | Engine version and build number |
| `toggle` | `toggle <cvar>` | Invert 0/1 |
| `bind` | `bind <key> <command>` | Bind a key |
| `unbind` | `unbind <key>` | Remove a binding |
| `bindlist` | — | List of bindings |
| `unbindall` | — | Reset all bindings |
| `bind_reset` | — | Restore the default bindings |
| `alias` | `alias <имя> "<команда[; команда]>"` | Macro |
| `unalias` | `unalias <имя>` | Delete a macro |
| `aliaslist` | — | List of macros |
| `snd_debug` | `snd_debug [0/1]` | Playing sounds in the HUD |
| `profiler` | `profiler [0/1]` | Performance panel |
| `m_smooth` | `m_smooth [0..1]` | Mouse view interpolation |

The key name for `bind` is an SDL scancode name (`A`, `W`, `F5`, `SPACE`,
`LEFTBRACKET`, `LCTRL`, `LSHIFT`, …) plus two virtual ones:
`mouse_wheel_up` / `mouse_wheel_down` (`binds.c:47-52`).

## Movement (`+`/`−` pairs)

The action flag is set from a bind; the `-` command clears exactly the same
flag (`cmdreg.c:23-37, 44-61`). Everything is read by `camera.c`.

| Pair | Action |
|---|---|
| `+forward` / `-forward` | Forward |
| `+back` / `-back` | Backward |
| `+left` / `-left` | Left |
| `+right` / `-right` | Right |
| `+jump` / `-jump` | Jump / up in flight |
| `+down` / `-down` | Down in flight |
| `+speed` / `-speed` | Run |
| `+duck` / `-duck` | Crouch |
| `+use` / `-use` | Interact with an object |

## Map, view, game flow

| Command | Syntax | What it does |
|---|---|---|
| `map` | `map <имя>` | Select and start a map |
| `maps` | — | List of available maps |
| `changelevel` | `changelevel <карта>` | Change the map |
| `restart` | — | Restart the current map |
| `demo` | — | Demo scene: automatic camera flythrough + FPS measurement |
| `pause` | — | Pause / back to the menu |
| `noclip` | — | Fly through the world (cheat) |
| `thirdperson` / `firstperson` | — | Third-person / first-person view |
| `setpos` | `setpos x y z [yaw] [pitch]` | Teleport the player |
| `save` / `load` | — | Save / load position (`save.cfg`) |
| `screenshot` | — | Frame to PNG into `screenshots/` |
| `toggle_map` | — | Minimap on/off |
| `toggle_debug` | — | Debug panel on/off |
| `toggle_crosshair` | — | Crosshair on/off |
| `toggle_shadows` | — | Shadows on/off |
| `toggle_fog` | — | Volumetric fog on/off |
| `r_drawdebug` | `r_drawdebug [0/1]` | Debug panel (by value) |
| `mat_wireframe` | `mat_wireframe [0/1]` | Wireframe mode |
| `reload_shaders` | — | Recompile changed shaders |

## Effects and weather

| Command | Syntax | What it does |
|---|---|---|
| `fx` | `fx <имя> [x y z]` | One-shot effect at a point (without coordinates — at the view point) |
| `fx_burst` | `fx_burst <имя> <x> <y> <z> [count]` | Particle burst |
| `fx_emit` | `fx_emit <имя> <x> <y> <z> <rate> [life]` | Continuous emitter |
| `fx_stop` | `fx_stop [имя]` | Stop an emitter (without a name — all of them) |
| `fx_clear` | — | All emitters + the particle pool |
| `fx_list` | — | List of presets |
| `fx_sparks` / `fx_smoke` / `fx_blood` | — | Sparks / smoke / blood at the view point (bound to `M`, `,`, `.`) |
| `weather` | `weather <пресет> [rate]` or `weather off` | Weather following the player |

Built-in presets (`fx.c:483-595`): `sparks`, `smoke`, `fire`, `blood`,
`explosion`, `rain`, `splash`, `snow`, `dust`, `muzzle_flash`. Custom presets
are registered from Lua — `engine.fx_register`
([lua-api.md](lua-api.md)).

## Models, scene, animation

| Command | Syntax | What it does |
|---|---|---|
| `mesh` | `mesh <obj> [x y z] [scale] [tex]` | Static `.obj` as a scene node |
| `glb` | `glb <glb> [x y z] [scale]` | `.glb`; with a skin + animation — autonomous AnimInstance selection |
| `glbanim` | `glbanim <glb> [x y z] [scale]` | Animated model (skinning) |
| `glbclips` | `glbclips [имя-узла]` | List of animation clips |
| `glbclip` | `glbclip <индекс\|имя> [имя-узла]` | Switch a clip |
| `glbanim_play` / `glbanim_pause` | `[имя-узла]` | Resume / pause the animation |
| `glbanim_speed` | `glbanim_speed <x> [имя-узла]` | Playback speed |
| `glbanim_loop` | `glbanim_loop <0/1> [имя-узла]` | Loop it |
| `glbanim_seek` | `glbanim_seek <сек> [имя-узла]` | Set the clip time |
| `glbanim_frame` | `glbanim_frame <кадр> [fps] [имя-узла]` | Set a frame (fps defaults to 24) |
| `anim` | `anim [list\|help]` \| `anim <N> pos\|move x y z [spd]` \| `rot yaw pitch roll` \| `clip\|play\|pause\|loop\|speed\|seek\|frame\|on\|off\|scale\|clips\|bones\|stop\|remove` | General-purpose node animation operator |
| `attach` | `attach sprite\|glb\|mesh <N> <кость\|-> <файл> […]` \| `attach list\|bones <N>` | Attach a file to a bone (or to a node) |
| `sprite` | `sprite <png> [x y z] [w h] [flat]` | Sprite in the world |
| `matlist` | — | List of registered materials |
| `cubemap_capture` | `cubemap_capture [x y z] [size]` | 6 face PNGs into `cubemaps/` (static water reflections) |

## Entities

| Command | Syntax | What it does |
|---|---|---|
| `ent_create` | `ent_create <класс> [x y z]` | Spawn an entity |
| `ent_list` | — | List of live entities |

Classes can be registered from Lua — `engine.entity_class`.

## Physics (mini Verlet)

| Command | Syntax | What it does |
|---|---|---|
| `phys_rope` | `phys_rope [звеньев]` | A rope made of links |
| `phys_push` | `phys_push [сила]` | Push the rope end |
| `phys_ragdoll` | `phys_ragdoll [hang] [kick]` | Ragdoll on an animated model |
| `phys_clear` | — | Remove the physics world |
| `phys_sphere` | `phys_sphere [радиус]` | A sphere body in front of the camera (turns on `phys_bodies`) |
| `phys_box` | `phys_box [размер]` | A box body (8 corners + links) in front of the camera |
| `phys_props` | `phys_props [n]` | A stack of boxes (<=32) in front of the camera |
| `phys_bodies` | — | How many bodies, where the centers are, who is resting, how much static geometry |

## ECS (data layer over the scene)

| Command | Syntax | What it does |
|---|---|---|
| `ecs_list` | — | List of ECS entities, components and systems |
| `ecs_stats` | — | Counters: entities, components, systems, bridge pairs |
| `ecs_test` | — | Core selftest (failures -> FAIL in the console and log) |
| `ecs_verify` | — | Scene<->ECS bridge consistency check (nodes/pairs/divergences) |

## Player health

| Command | Syntax | What it does |
|---|---|---|
| `hp` | — | Current HP status |
| `hurt` | `hurt [N]` | Damage (25 by default) |
| `heal` | `heal [N]` | Heal; without N — to full |
| `sethp` | `sethp <N>` | Set HP (requires `sv_cheats 1`) |
| `maxhp` | `maxhp <N>` | Set the HP maximum |
| `god` | `god [0/1]` | Immortality (requires `sv_cheats 1`) |
| `kill` | — | Instant death |

## Sound

| Command | Syntax | What it does |
|---|---|---|
| `snd_test` | — | Play all sounds one after another |
| `snd_occlude` | `snd_occlude <x> <y> <z>` | Occlusion multiplier from the listener to a point (terrain occlusion) |

## Default bindings

`binds.c:14-36` (restored by the `bind_reset` command):

| Key | Command |
|---|---|
| `W` / `A` / `S` / `D` | `+forward` / `+left` / `+back` / `+right` |
| `SPACE` | `+jump` |
| `LCTRL` | `+down` |
| `LSHIFT` | `+speed` |
| `C` | `+duck` |
| `E` | `+use` |
| `F` | `noclip` |
| `[` | `toggle_shadows` |
| `P` | `toggle_fog` |
| `G` | `toggle_map` |
| `H` | `toggle_debug` |
| `Q` | `demo` |
| `M` / `,` / `.` | `fx_sparks` / `fx_smoke` / `fx_blood` |
| `F5` / `F9` | `save` / `load` |
| `F12` | `screenshot` |

The mouse (view) and the `` ` `` / `~` key (`SDL_SCANCODE_GRAVE`) for the
console are handled outside the bind system (`console.c:526-527`).
The `FR_CONSOLE` environment variable opens the console right at startup.
