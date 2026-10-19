# firerender code and documentation conventions

What an agent should read before editing so the diff looks like it was written
by a local. Every item describes the actual state of `src/` (72 modules,
~32k lines of `.c`), not a wish.

---

## 1. Language and boundaries

- **C only.** No C++/STL/RTTI/templates. `/* */` comments are rare (8 files),
  the style is `//`.
- **Text language.** Code comments, header blocks and `SDL_Log` dev output are
  **English**. Everything the player can see stays **Russian**: menu/HUD labels,
  `console_print()` output, cvar `help` strings, command descriptions.
  Documentation in `docs/` and `README.md` is English; commit messages are
  Russian (repo history). Do not translate a Russian runtime string and do not
  write a new player-visible string in English.
- **No new dependencies.** Vendoring goes exclusively into `third_party/`
  (`lua`, `miniz`) and only with the user's explicit consent.
  `third_party/SDL/` is a downloaded source tree, not in git (`.gitignore`).
- **The build is a single gcc command** over `src/*.c` + vendored files
  (`CFLAGS` — `Makefile:41`, build rule — `Makefile:77`):
  `-O2 -Wall -Wextra -Ithird_party -Ithird_party/lua -Ithird_party/miniz -Isrc
  $(pkg-config --cflags sdl3)`. No new flags, no `-lGL`, GLEW or EGL — GL is
  loaded through `SDL_GL_GetProcAddress` (`gl_loader.c`).
  Your own flag hook is `EXTRA_CFLAGS`.
- **`-Wall -Wextra` warnings must not increase.** Someone else's existing noise
  is not "fixed" along the way.

---

## 2. File format

```c
// ============================================================================
//  renderer.c — frame render orchestration (see renderer.h)
// ----------------------------------------------------------------------------
//  Extracted from main.c: profiled scene-tree traversal, the post-processing
//  cascade (scene -> bloom -> voxel fog) and 2D overlays
//  (minimap, HUD, crosshair, map message, debug panels).
// ============================================================================
#include "renderer.h"

#include "app.h"
#include "console.h"
```

- Header: a `// ===` frame, one line `module.c — what it does (see module.h)`,
  then `// ---` and 1–5 lines saying **why this module exists and why it is the
  way it is** (usually — what it was split out of, what it replaces, which
  driver workaround it carries).
- Own header first, blank line, then the rest alphabetically.
- Indentation — **4 spaces, no tabs** (in all `src/*.c`). K&R braces;
  `if (x) return NULL;` on one line is fine for trivial guards.
- Headers: `#ifndef NAME_H` / `#define NAME_H`, no `#pragma once`; do not
  expose declarations nobody outside needs; a short English comment per
  function.
- One function — one responsibility; long blocks are commented with the
  **reason**, not a retelling of the code.

---

## 3. Naming

| What | Style | Example |
|---|---|---|
| functions | `snake_case`, verb | `shadow_toggle()`, `resources_resolve_path()` |
| file-statics | `s_` + noun | `static Cvar *s_cvars;`, `static int s_n;` |
| global state | `g_` | `g_gl_gpu_driven`, `g_act[A_COUNT]` |
| struct types | `PascalCase`, often self-typedef | `typedef struct Entity Entity;` |
| fields | `snake_case` (or short `x, y, z`) | `c->minv`, `cam->yaw` |
| limit constants | `*_MAX` in the module header | `CVARS_MAX`, `DECALS_MAX`, `MAP_MAX_ROADS` |
| enum values | `UPPER_SNAKE` with an area prefix | `PROF_SKY`, `APP_PLAY`, `CVAR_ARCHIVE` |
| GL pointers | `p_gl*` | `p_glDrawElementsIndirect` |
| cvars | domain_name | `r_veg_falloff_mult`, `sv_cheats`, `cam_sensitivity` |
| commands | lowercase, verb | `toggle_shadows`, `cubemap_capture` |

Domain prefixes: `r_` render, `sv_` simulation / network groundwork, `cam_`
camera, `cl_` client, `snd_` sound, `ui_`/`hud_` interface, `host_` main loop.

---

## 4. What gets registered where

| Adding | The single place | Reference |
|---|---|---|
| cvar | `cvar_register*()` in the subsystem init | `src/cvars.c`; sample — `console.c:910-986` |
| console command | `s_cmds` table + handler | `src/cmdreg.c:46-140`, handlers in `commands.c`/`fx.c` |
| default bind | `s_defaults[]` | `src/binds.c:14-36` |
| "+action" (`+forward`) | action table + `g_act[]` | `src/actions.c` |
| `map.json` key | parser in `mapdata.c` | `src/mapdata.c:753-1078` |
| `engine.*` for Lua | function registration + sandbox | `src/lua_engine.c` |
| `FR_TEST` letter | `fr_test_apply()` / loop | `src/main.c:250-400` (applied at `src/main.c:768`) |
| profiler slot | `ProfSlot` enum | `src/profiler.h:12-29` |
| scene node | `node_new()` + `node_add()`, `update`/`render` hooks, `params` | `src/scene.h:25-70` |
| resource path | `resources.c` (pak → disk → `base_dir` → `../` chain) | `src/resources.c:85-164` |
| GL function | loader table | `src/gl_loader.c` |
| quality preset | cvar matrix of presets | `src/lowend_profile.c`, `app.c:350-419` |

Registration order = table order; in `s_cmds` new rows go into their own group,
not at the end.

---

## 5. Resources, files, assets

- **File reads go through `resources.c` only** (`resources_read_*`,
  `resources_resolve_path`). A direct `fopen(path)` breaks zip map packs and
  Android (`assets` are read-only).
- **A missing file is normal:** return `NULL`, log it, continue; a missing
  texture is `img/notfound.png`. Never invent facts about data.
- **All external bytes are untrusted.** Check the size before reading, bounds by
  subtraction (`clen > filesz - off`), the expected `vec` component, indices
  inside the buffer. A broken file must produce a log line and a skipped
  element.
- **`malloc`/`free` are the standard ones** (535 call sites); `SDL_free` only
  where SDL allocated (`SDL_LoadWAV_IO` → `SDL_free` in `audio.c`).
  After every alloc an immediate guard; on failure free **everything** the
  function already owns and zero the counters.
- **JSON is read through an arena** (`src/json.c`, `json_free`) — no `strdup`
  per node.
- **There is no `assert()` or `exit()` in `src/`** — and none should appear: a
  headless run must return a code, not kill the process.

---

## 6. GL and RHI

- New GPU code goes through `sr_*` (`src/srender.h`); `srender_gl.c` is the only
  full backend. Do not touch Vulkan/Metal without a separate task.
- Any direct `gl*` call must exist in `gl_loader.c` and **must not be called
  when `p_glXxx == NULL`** (the fallback path for GLES/old drivers).
- GLES ≠ desktop: caveats from `GL_INVALID_OPERATION` due to a depth texture
  instead of a depth RBO (MSAA/HiZ), missing desktop formats. Verify with
  `make gles` + a headless run.
- **Shaders — one source for all backends**, `shader.c:70-76` substitutes
  `#version` (300es/310es/330core/430core). Do not multiply file variants.
  Shader/texture hot-reload runs in the desktop loop once per second
  (`main.c:794-804`).
- GL state goes through `material.c` (uniforms, blend, depth); do not "set and
  forget": any global mode must be restored before the end of the frame.

---

## 7. Performance as part of the style

- Heavy work stays out of the frame: `src/jobs.c` (small thread pool) or loading
  phases with a budget. In-frame code does no per-object allocations and no
  `glReadPixels`/`glMapBuffer`.
- An expensive visual mode must have a cvar switch and (preferably) a `profiler`
  slot, otherwise it cannot be measured and cannot be turned off on the
  POTATO preset.
- Before/after numbers belong in the report (`FR_BENCH`, docs/testing.md §5).
  "It looks better" without numbers is not accepted.

---

## 8. Logs and user-facing messages

- `SDL_Log("…")` — technical events and errors, **English** text, prefixed with
  the subsystem name: `cvar_set: "%s" not registered — creating%s`,
  `primitive skipped …`. Player-facing console text uses `console_print()` and
  stays Russian.
- A warning about an abnormal situation must be writable as a test check
  (a stable substring, the name of the offending path).
- No per-frame spam: a log inside a loop fires 60 times a second, turns into
  noise and costs time; log the fact / the first occurrence.

---

## 9. Documentation

- References: `docs/cvars.md`, `docs/commands.md`, `docs/map-json.md`,
  `docs/lua-api.md`. The rule: **every reference line has a `file:line` in the
  code, and every new cvar/command/key/`engine.*` appears in the reference in
  the same change.**
- Overview and launch — `README.md`; agent rules — `AGENTS.md`;
  `docs/architecture.md`, `docs/testing.md`, `docs/conventions.md`,
  `docs/pitfalls.md`.
- Honest status: do not call a stub working (Vulkan — device creation only,
  Metal — a skeleton, Web — absent, the engine does not read
  `config/menu.json`).
- Documentation language — **English**, engine terms as they are in the code
  (`cvar`, `bind`, chunk, billboard). Do not translate file names or
  identifiers. Quoted player-visible strings stay in the original Russian,
  because that is what the engine prints.
- Do not invent: before writing "the engine does X", find X in the code.

---

## 10. Git

- Commit **only on an explicit request**. Do not push, branch or open PRs on
  your own.
- Message style: `feat:`/`fix:`/`perf:`/`ui:` + what and why, in Russian, one or
  two lines, `git log -8` as the sample.
- Keep out of the index: `config.cfg`, `save.cfg`, `screenshots/`, `models/`,
  `reference/`, `build/`, `buildnum.txt`, `src/buildnum.h` (`.gitignore`),
  `third_party/SDL/`.
- Do not rewrite history (published `amend`, `reset --hard`) and do not bypass
  hooks without a request.
- Before a broad `git add` — `git status` and a check that no secrets or run
  artifacts slipped in.
