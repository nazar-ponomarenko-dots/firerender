# ⚠️ PROJECT TEMPORARILY FROZEN

**This project is currently frozen.** Development is focused on a specific game by the engine's author. After the game is complete, all new features and fixes will be merged back into the public firerender repository.

**The project is highly unstable and contains many bugs**, but it already produces a visually pleasing result.

---

# firerender

A **3D engine and sandbox game** written in **pure C** using **SDL3** (window, input, audio) + **OpenGL**. Hybrid architecture combining a scene tree and render passes (Godot-style) with a console, cvars, and binds (GoldSrc/Source-style). All GPU code uses the engine's own RHI (`sr_*`, `src/srender.h`), making graphics API swaps transparent to render logic.

> **Repository:** <https://github.com/nazar-ponomarenko-dots/firerender>
> **License:** MIT ([LICENSE](LICENSE))

## Key Features

- **Open-world terrain:** Heightmap-based, procedural wheat/grass fields, billboard objects, water (puddles/lakes/rivers/sea), sky with panorama and procedural clouds
- **Rendering:** CSM shadows, screen-space GI with temporal accumulation, SSR with wetness mask, volumetric fog (3 R8 3D cascades), bloom + tonemapping, procedural clouds
- **Lighting:** Forward+ clusters (directional/point/spot/rect/tube), baked lightmaps, sunvis and probe baking
- **Vegetation:** GPU-driven compute-cull + `glDrawElementsIndirect`, CPU instanced fallback
- **World:** Surface splat masks, roads, triggers, positional sounds, fauna (fireflies/butterflies/birds)
- **Models:** `.obj` and `.glb` (static, skinning, skeletal animations), bone attachments, sprites
- **Physics:** Verlet mini-world (ropes, ragdolls)
- **Player:** Walk/run/crouch/jump/fly, HP system with death and killcam, position saves
- **UI:** Immediate-mode menu (UTF-8 Russian, TTF atlas), console with history and autocompletion, HUD, minimap, debug panel, profiler
- **Input:** Keyboard/mouse, gamepad, touch (joystick + buttons), Android lifecycle
- **Scripting:** Lua 5.4 sandbox with commands, events, entities, materials, FX
- **Performance:** Dynamic resolution, low-end profiles, PO-TATO/MEDIUM/ULTRA presets, chunk regeneration budget, grass z-prepass

Full references: [cvars](docs/cvars.md) · [commands](docs/commands.md) · [map.json](docs/map-json.md) · [Lua API](docs/lua-api.md)

## Quick Start

```sh
make BAKE=0            # build engine only (skip light baking)
./build/firerender      # run
FR_CONSOLE=1 ./build/firerender  # start with console open
```

Requirements: `gcc`/`clang`, SDL3 **3.x**, `python3`. OpenGL functions loaded at runtime via `SDL_GL_GetProcAddress`. Lua 5.4 and miniz vendored in `third_party/`.

> **Note:** `make` bakes light for all maps by default (`BAKE ?= 1`). For code iteration, use `make BAKE=0`.

## Platforms & Backends

| Backend | Status |
|---|---|
| OpenGL (desktop, 4.5 CORE) | **works** — `make` |
| OpenGL ES 3.0-3.2 | **works** — `make gles` (Android, Raspberry Pi) |
| Vulkan | **groundwork** — device only, render pipeline not ported — `make vulkan` |
| Metal | **skeleton** — `src/srender_metal.c`, macOS only — `make metal` |
| Web / Emscripten | **none** |

| Platform | Build Command |
|---|---|
| Linux | `make` or `make linux` |
| Windows | `make win` / `make winpkg` |
| Android | `make apk` |
| macOS | `make` |

## Testing (Headless)

```sh
SDL_VIDEODRIVER=offscreen FR_TEST=S ./build/firerender    # screenshot at frame 450
SDL_VIDEODRIVER=offscreen FR_TEST=SlS ./build/firerender  # + glb + skeletal animation
SDL_VIDEODRIVER=offscreen FR_TEST=Sx ./build/firerender   # + console self-check
SDL_VIDEODRIVER=offscreen FR_BENCH=1 ./build/firerender   # benchmark
```

See [docs/testing.md](docs/testing.md) for full harness details.

## Documentation

- [Architecture](docs/architecture.md)
- [Headless Testing](docs/testing.md)
- [Code Conventions](docs/conventions.md)
- [Pitfalls & Limitations](docs/pitfalls.md)
- [AGENTS.md](AGENTS.md) — AI agent working rules

## Legacy Documentation

For the full original README, see [README-LEGACY.md](README-LEGACY.md).
