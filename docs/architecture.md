# firerender architecture

Source tree map and the main flows. For agents: before editing, find your module
here and read its header banner (the first 5–8 lines of each `src/*.c` — an exact
description of its purpose).

---

## 1. Three architectural ideas

1. **A Godot-style scene tree.** `SceneNode` (`src/scene.h:33-52`) — name, type
   as a string, transform (`pos/rot/scl` + world `world[16]`), `params`, and two
   function pointers: `update(node, dt)` and `render(node)`. Traversal —
   `node_update_tree` + `node_update_transforms` before rendering.
2. **A GoldSrc/Source-style console and cvars.** `src/cvars.c` (registry, clamping,
   `CVAR_ARCHIVE`/`CVAR_CHEAT`/`CVAR_STRING`), `src/console.c` (input, history,
   autocompletion), a single command registry `src/cmdreg.c:46-140`, key binds
   `src/binds.c:14-36`, `+/-` actions in `src/actions.c`.
3. **A Source-style resource resolution.** `src/resources.c:85-164`: map pack
   (zip) → disk → `base_dir` → `../`, a missing image — `img/notfound.png`.
   No embedded data, everything is read from disk or from a zip pack.

All GPU work goes through the in-house `sr_*` RHI (`src/srender.h`) — swapping the
API does not touch the render logic.

---

## 2. Application skeleton

```
main.c: SDL init -> window/GL context -> boot_init() -> main loop
```

**`boot_init` (`src/main.c:216-237`)** — the order matters:

| Step | What is brought up |
|---|---|
| 1 | `resources_init()` — resource cache **before** any loads |
| 2 | `console_init()` — cvars, commands, `SDL_Log` output into the console |
| 3 | `audio_init()`, `binds_init()`, `fx_init_console()` |
| 4 | `ecs_scene_init()` (bridge components) + cvar `ecs_enable`, `phys_body_init()` (body cvars, empty static-collider registry) |
| 5 | `cmdreg_register_all()` — all static commands from the single registry |
| 6 | `hud_shared_init()` — font/atlas (needed by the menu and the console) |
| 7 | `app_init()` — `img/menu.png` background |
| 8 | `console_autoload()` — `config.cfg` (user cvars and binds) |
| 9 | `game_map_config_load()` — the selected map |
| 10 | `lua_engine_init()` — `lua/main.lua` |
| 11 | `app_set_state(APP_MENU)` (or `APP_PLAY` under `FR_TEST`) |

**States (`src/app.h:13-18`):** `APP_BOOT → APP_MENU ⇄ APP_PLAY ⇄ APP_PAUSE`.
The world is frozen outside `APP_PLAY`; `AppState` hands actions to the main loop
(`APP_ACT_START/RESUME/MAIN/QUIT`).

**Tick:** a fixed logic step — 100 Hz on desktop (at most 8 ticks per frame),
60 Hz on Android (at most 4) — `src/main.c:812-821`. Rendering runs as often as
the GPU allows; logic does not depend on FPS.

The order inside a tick is also fixed: `game_update` — player, entities,
triggers (`game.c:764-802`), then `ecs_scene_frame` (`game.c:817`: pulls scene
transforms, runs ECS systems, pushes back only the changed ones; with
`ecs_enable 0` the layer does nothing), then `node_update_transforms`
(`game.c:827`). After `game_update` — `phys_body_step` (`main.c:827`: Verlet step
plus collisions; it also steps the global world, which is why ropes and ragdolls
work even with bodies disabled) and `glb_anim_ragdoll_step_all` (`main.c:829`).

---

## 3. World loading and rollback

`game_load_world` (`src/game.c:589`) is assembled from phases, each with its own
partial rollback; unloading (`game_unload_world`, `src/game.c:705`) is a strict
inverse of the creation order (`src/game.c:15`):

| Phase | Function | What happens |
|---|---|---|
| 1. Geometry | `world_load_geometry` (`src/game.c:329`) | opens the zip pack (`maps/<name>.zip` or `../maps/…`), `mapdata_load("map.json")`, `world_load` — heights/normals/textures, the wheat field |
| 2. Scene | `scene_build` (`:410`) | `shadow_init`, scene node tree, entities, render FBO, first frame |
| 3. Map overrides | `map_apply` (`:529`) | cvars and commands from `map.json` — **on top of** `config.cfg` |
| 4. Map audio | `audio_load_map` (`:549`) | music, ambient/wind, positional sources (wav from the map zip) |

Failure of any phase returns `-1` and frees only what the previous steps created.
Therefore parsers must be fail-soft and must not leave dangling nodes in the tree.

---

## 4. Per-frame pass order

The orchestrator is `renderer_frame` (`src/renderer.c:886-1037`); the single
per-frame context is filled once: `renderer_ctx_fill(g)` (`:888`) puts camera,
world, time, sun, fog, water/underwater, wetness into `RendererContext`
(`src/render_ctx.h`).

```
1. underwater tint / wave time                            (renderer.c:892-911)
2. CSM shadows: shadow_render(world, &rc)                (:926)  — shadow.c
3. scene into FBO (r_render_scale, MSAA if enabled)      (:764)
4. bloom_resolve(need_depth) — resolve + is depth needed (:945-951)
5. GI: gi_render(...) -> own color texture               (:957-965)  — gi.c
6. SSR: ssr_render(...) -> ssr_tex                       (:971-985)  — ssr.c
7. voxel fog + bloom composite                           (:990-1008)  — voxfog.c, bloom.c
8. present: bloom_present (r_present_blit) or
   fullscreen grade into the backbuffer (ES/EGL)         (:1009-1024)
9. 2D overlays: minimap, HUD, crosshair, console, menu   (renderer.c, hud.c, ui.c)
10. lowend_profile_frame(dt) — adaptive resolution       (:1090)     — lowend_profile.c
```

`need_depth` (`:945-950`) is a lazy condition: depth is resolved only if GI,
wet/forced SSR, or voxel fog is enabled (not the "cheap" `r_cheap_fog`). This is
the typical project pattern: **a pass is skipped when it is turned off**, rather
than always run "for consistency".

---

## 5. Map of `src/` modules

Purposes are taken from the file header banners (line 2 of each `.c`).

### Core and loop

| Module | What it is |
|---|---|
| `main.c` | entry point, `boot_init`, main loop, all `FR_TEST`/`FR_BENCH` scaffolding |
| `game.c` | game state: world load/unload (phases + rollback), update |
| `app.c` | state manager and menu screens, quality presets, `FR_MSAA` |
| `loading.c` | level loading screen (per-phase progress) |
| `jobs.c` | small worker-thread pool |
| `events.c` | event bus |
| `entity.c` | entities: slots, `serial<<16|num` handles |
| `ecs.c` | ECS core: id + stable slots, sparse POD components, systems by `all` mask, deferred kill |
| `ecs_scene.c` | scene<->ECS bridge: generations against dangling pointers, transform pull/systems/push |
| `scene.c` | scene tree: node creation, transforms, traversal |
| `selection.c` | selection outlines for 3D nodes |
| `profiler.c` | per-module timings + GPU queries; the counters live in `gl_loader.c` |
| `lowend_profile.c` | low-end profile: a package of tricks, adaptive resolution |

### Console, input, UI

| Module | What it is |
|---|---|
| `cvars.c` | cvar registry (registration, clamping, flags, config.cfg) |
| `console.c` | developer console: input, history, autocompletion, `console_selftest` |
| `cmdreg.c` | single table of static commands |
| `commands.c` | action command handlers (incl. `screenshot`) |
| `binds.c` | key bindings, defaults |
| `actions.c` | `+/-` actions (`g_act[]`) |
| `input.c` | main-loop events, mouse mode |
| `gamepad.c`, `touch.c` | gamepad and touch (joystick + buttons) |
| `ui.c` | immediate-mode UI |
| `hud.c` | debug HUD (position, FPS, speed), font atlases |
| `minimap.c` | minimap |
| `json.c` | mini JSON parser (node tree in an arena) |
| `png.c` | saving an RGB frame to PNG via miniz |

### Renderer and RHI

| Module | What it is |
|---|---|
| `render_ctx.h` | `RendererContext` — the single set of scene data per frame |
| `renderer.c` | orchestration of per-frame passes, `r_render_scale`, scale/FBO |
| `srender.h` + `srender_gl.c` | the `sr_*` RHI on OpenGL/GLES (the only complete backend) |
| `srender_vk.c` | Vulkan skeleton: device bring-up only, the rest are stubs (`make vulkan`) |
| `srender_metal.c` | Metal skeleton for macOS (`make metal`) |
| `srender_stub.c` | render API selection and stubs for future backends |
| `gl_loader.c` | loads GL functions via `SDL_GL_GetProcAddress`; `g_gl_gpu_driven` |
| `gpu_gl.c` | thin wrappers of the old RHI (`gpu.h`) over `sr_*` |
| `shader.c` | reading shader files, `#version` substitution, program linking, hot-reload |
| `assets.c` | texture creation (loading goes through `resources.c`) |
| `material.c` | surface material: GL state and uniforms |
| `bloom.c` | glow postprocessing + resolve/composite/present |
| `gi.c` | screen-space global illumination with temporal accumulation |
| `ssr.c` | Screen Space Reflections (wetness mask) |
| `voxfog.c` | voxel volumetric fog (3 R8 3D cascades + history) |
| `shadow.c` | cascaded shadow maps, static/baked/"blob" modes |
| `frustum.c` | frustum culling |

### World and content

| Module | What it is |
|---|---|
| `world.c` | heightmap, normal computation, chunks |
| `mapdata.c` | `map.json`: field, lights, objects, triggers, waters, fauna (:769-1056) |
| `resources.c` | resource manager: pak → disk → `base_dir` → `../`, caching |
| `terrain.c` | terrain: grid from the heightmap, ground material, shadowed render |
| `splat.c` | surfaces: splat mask, roads, occupancy map |
| `snowmask.c` | marks vertices "open to the sky from above" (snow on GLB) |
| `field.c` | wheat field outline (a closed loop on the terrain) |
| `vegetation.c` | vegetation: static map layer + dynamic layer |
| `objects.c` | objects from `map.json`: billboards (trees, etc.) |
| `mesh.c` | static meshes (`prop_static`): `.obj` loader + node |
| `glb.c` | binary glTF 2.0: parser + `GlbInstance3D` node |
| `glb_anim.c` | skeletal animation (skinning), clips, instances |
| `attach.c` | attaching nodes to models/bones + the `Sprite3D` node |
| `fauna.c` | living map: fireflies, butterflies, birds |
| `water.c` | water: puddles, lakes, rivers, seas; underwater tint |
| `sky.c` | sky from a panorama: the `Skybox3D` node + horizon fog color |
| `sun.c` | sun: a frozen directional light |
| `lights.c` | Forward+: light layout into froxels (clusters) |
| `particles.c` | particle system: instanced sprites with CPU updates |
| `fx.c` | effects: named presets and continuous emitters, weather |
| `decals.c` | decals on the terrain |

### Player, physics, scripts, platform

| Module | What it is |
|---|---|
| `player.c` | player state |
| `movement.c` | physics/movement (walk, run, crouch, jump, fly) |
| `camera.c` | camera: WASD + mouse, walk physics, view/projection matrices |
| `playerbox.c` | third-person character collision box |
| `health.c` | HP, death, killcam |
| `physics.c` | mini body engine (Verlet): ropes, ragdoll |
| `phys_body.c` | layer over the same Verlet world: rigid bodies (sphere/box), static map colliders, particle-particle collisions, push by the player |
| `linalg.c` | vectors and matrices for the camera and projection |
| `lua_engine.c` | engine Lua scripts: sandbox, `engine.*`, events |
| `audio.c` | sound: device, channels, positional sources |
| `android.c` | Android environment setup (lifecycle) |

Two known "cosmetic" header defects, not to be touched without reason:
`src/linalg.c` is called `math.c` in its header, and `src/lowend_profile.c` has an
empty second banner line.

---

## 6. Maps and pack resources

- **Shipped form:** `maps/<name>.zip` — the whole map (`map.json`, `map.png`,
  `img/`, `shaders/`, `audio/`, `lua/maps/`, baked `baked/`).
- **Dev mode:** a `map/` folder next to the sources.
- The lookup chain — `src/resources.c:85-164`. Hence the rule: every new resource
  is addressed by a **relative path** through `resources_*`, otherwise it will not
  be found in a zip or on Android.
- Packing/baking: `make mapzip MAP=<name>` (rebuild from `map/`),
  `make mapbake MAP=<name>` (light → `map/baked/` → into the zip), `make bake-maps`
  (all `maps/*.zip`, via `tools/inject_baked.py`, geometry untouched).

---

## 7. Where to look for a specific question

| Question | File |
|---|---|
| why a setting did not persist | `cvars.c` (flags, `CVAR_CFGSET`) + `console_autoload` |
| why a command is "not found" | `cmdreg.c:46-140` + `console.c:988-1008` |
| why a map key is ignored | `mapdata.c:753-1078` (unknown enum → skip + log) |
| why a black frame / no post effect | `renderer.c:886-1037` (`need_depth`, `*_is_on()` conditions) |
| why grass did not appear | `vegetation.c` + `r_veg_*` (docs/cvars.md), chunk budget |
| why FPS dropped after an edit | `profiler.c` (slots), `FR_BENCH`, `lowend_profile.c` |
| why an asset failed to load | `resources.c` (chain) + `img/notfound.png` |
| how to add a backend | `srender.h` + `srender_stub.c` (API selection) |

Value references: [docs/cvars.md](cvars.md) ·
[docs/commands.md](commands.md) · [docs/map-json.md](map-json.md) ·
[docs/lua-api.md](lua-api.md). Agent working rules:
[../AGENTS.md](../AGENTS.md).
