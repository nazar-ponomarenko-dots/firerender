# AGENTS.md — working rules for AI agents

This is a working contract, not an advertisement. If a rule conflicts with the
task — the rule first, then the task. If you are unsure whether a rule applies,
check the fact in the code, not in this file: a reference to a `file:line`
always outweighs a retelling.

Agent-facing documentation referenced from here:

| File | About |
|---|---|
| [docs/architecture.md](docs/architecture.md) | map of `src/` modules, application phases, frame pass order |
| [docs/testing.md](docs/testing.md) | `FR_*` variables, the headless loop, CI, sanitizer recipes, A/B screenshots |
| [docs/conventions.md](docs/conventions.md) | C style, comments, naming, where to register a cvar/command/`map.json` key |
| [docs/pitfalls.md](docs/pitfalls.md) | proven NOT-bugs (do not "fix") and known limitations |
| [docs/cvars.md](docs/cvars.md) · [docs/commands.md](docs/commands.md) · [docs/map-json.md](docs/map-json.md) · [docs/lua-api.md](docs/lua-api.md) | references, must match the code |

---

## 0. Priority hierarchy

Every decision is weighed in this order, top down:

1. **Engine stability.** No crashes, UB, stack/heap corruption, "hanging on
   load", black frames. The engine must survive a broken asset.
2. **FPS and frame time.** A change must not silently eat performance.
3. **Image correctness.**
4. **Nice code / refactoring / new features.**

"Nicier" never outweighs "does not crash" and "does not lag". A cosmetic
refactor of 45k lines of working render code is risk with no profit — don't do
it.

---

## 1. Always test. No exceptions

No edit counts as done until there is proof. The minimal loop is three commands
(CI only checks the build and the run up to window creation, see
docs/testing.md §1):

```sh
make BAKE=0                                                       # builds clean
SDL_VIDEODRIVER=offscreen FR_TEST=SlS ./build/firerender          # render + glb + animation
SDL_VIDEODRIVER=offscreen FR_TEST=Sx ./build/firerender           # console and cvar selftest
```

`offscreen` draws without `X`/`Wayland`; the trailing `S` is mandatory — it
takes an autoscreenshot at frame 450 and exits at 480, otherwise a headless run
never finishes on its own and you will need `timeout`.

What must be checked by task type:

| Touched | Prove it with |
|---|---|
| asset parsers (`glb*.c`, `mesh.c`, `mapdata.c`, `json.c`, `audio.c`, `world.c`) | build + `FR_TEST=Sl` + a sanitizer run on a **broken** file (docs/testing.md §4) |
| render pipeline, shaders, post-processes | `FR_TEST=S` before/after, compare `screenshots/*.png` by eye + `FR_BENCH` |
| performance | `FR_BENCH=1` on **real** output (x11/wayland/device) — under `offscreen` a frame costs almost nothing and the numbers are not comparable (docs/testing.md §5). Report p50/p95/p99, not "it got faster" |
| console, cvars, commands | `FR_TEST=Sx`, then the new command/valued variable actually from the console |
| Lua | `FR_TEST=S` (loads `lua/main.lua` + `lua/maps/<map>.lua`) and a call from the console |
| physics/player/HP | `FR_TEST=SH` (ragdoll), `SR` (rope), `SK` (killcam) |
| GLES path | `make gles` + `FR_TEST=S`, not only desktop GL |

Proof rules:

- **Read the log, not just the exit code.** A successful `exit 0` with a new
  `primitive skipped`, `Shader compile error` / `Program link error`,
  `cvar_set: ... not registered` or a compiler warning is a failure, not a
  success.
- **Do not trust CI smoke claims.** `SDL_VIDEODRIVER=dummy` does not create a GL
  window (`SDL_CreateWindow failed ... driver (dummy)`, `exit 1`); CI tolerates
  this via `|| true`. Only `offscreen` actually draws.
- **`-Wall -Wextra` warnings must not increase.** Ideally there are none to
  begin with; do not "fix" someone else's noise along the way.
- **Sanitize anything that touches other people's bytes** (parsers, `memcpy`,
  casts, indexing). Recipe — docs/testing.md §4.

---

## 2. Code must be testable from the CLI

The engine lives in headless mode: if a feature cannot be enabled, measured and
screenshotted with environment variables and the console, the feature cannot be
verified. Therefore:

- **Any new quality knob = a cvar.** Registered in `src/cvars.c` via
  `cvar_register`/`cvar_register_ex`/`cvar_register_string`, with `min/max`,
  `CVAR_ARCHIVE` and a Russian `help` (player-visible text stays Russian). Then
  it is verifiable from the console, lands in `config.cfg` and in
  [docs/cvars.md](docs/cvars.md). A flag constant in the code is bad;
  `r_ssr 0..1` is good.
- **Any new action = a console command** (a row in `s_cmds`, `src/cmdreg.c:46`;
  the handler lives in its own module), not only a menu button. A command is
  callable from Lua, from the config, from an A/B test wrapper script.
- **A new visual mode = a letter in `FR_TEST`** (`fr_test_apply`,
  `src/main.c:250`) following the existing ones: `s`/`o`/`r`/`v`/`p`/`q`/`c`/`e`/
  `f`/`t`/`k` are subsystem toggles "on top of the mode", exactly what A/B frames
  need.
- **The viewpoint is set from outside**: `FR_MAP=<name>`,
  `FR_POS="x,z[,yaw,pitch]"`, `FR_W`/`FR_H`, `FR_MSAA`. Do not hardcode a camera
  in test code.
- **Keep state resettable:** a node/subsystem must survive being switched off on
  the fly (`shadow_toggle`, `voxfog_set_mode`, `r_baked_light 0/1`) — that is
  both a feature for the user and a test switch.
- **Performance is CLI too:** `profiler` (timings per slot, GPU queries),
  `matlist`, `FR_BENCH=1`, `FR_BENCH_GPU=1`, `FR_BENCH_PRESET=N`. A new expensive
  pass must show up in the profile, otherwise there is nothing to measure it with.
- **Fail soft with a log, not with `assert`.** A bad asset is a log message and a
  skipped element; in a headless test you see it on stdout. `abort()`/silence is
  unverifiable.

---

## 3. Split work into subtasks

Rule: one subtask = one verifiable hypothesis.

- Before starting, cut the list into pieces (`TaskCreate`), each with **what I do
  and how I prove it is ready**. Dependency order, if part A blocks part B.
- Do not mix in one pass: a memory edit + a rename + a new feature flag.
  Each category gets its own step and its own check, otherwise a regression is
  indistinguishable from a coincidence.
- **A minimal edit in the existing style** beats an architecturally correct
  rewrite. Fix an overflow with one condition line, not by rewriting the parser.
- Size: if a subtask is not verifiable with one command from §1, it is too big.
- After each subtask — a build and a smoke run, not "I'll do everything, then fix
  it". No half-finished states in the tree: either the node works or it honestly
  is not in the build paths.
- Do not "tidy up" after a previous step when you notice an unrelated defect:
  open a task and tell the user. Side edits are the source of unplanned
  regressions.

---

## 4. Stability: concrete obligations

Verified on this code (see the edit history and docs/pitfalls.md):

- **Every `malloc`/`realloc`/`SDL_malloc` is checked right after the call.**
  Never `out->x = malloc(...)` before `if (!out)`. On failure — free everything
  the function already owns (`json_free(); free(bvs); free(file); return NULL;`)
  and zero the counter so later loops do not spin on garbage.
- **Bounds coming from a file are checked by subtraction, never by
  addition.** `clen > filesz - off`, not `off + clen > filesz`: the unsigned sum
  wraps and the check passes.
- **Any size from an external file is validated against the actual buffer and
  against the expected `vec` component** (`acc_fits` + `ap->vec != 3`; 4 floats
  into `float p[3]` is stack corruption, not a "weird model").
- **Slots from `realloc` come with garbage:** zero the whole struct if part of
  the fields are strings read later (`cvar_slot_new`, `strcmp` in
  `cvar_save_config`).
- **Handles/UIDs are unsigned.** Packing `serial<<16|num` through an `int` dies
  at `serial >= 32768`; the contract "0 = invalid" is held explicitly
  (`src/entity.c`).
- **Pointers through a world unload:** `console_exec` can execute `map X` and
  remove the scene. Therefore — a copy of the value instead of a pointer, and a
  liveness check of the node/camera before use (sample: `src/game.c:276-279`).
- **GL state:** a GLES context ≠ desktop. A new call must be in the
  `gl_loader.c` table and must not be called when `p_glXxx == NULL` (the
  fallback path) — otherwise it dies on Android/old drivers. Checking
  `GL_NO_ERROR` is not "for beauty", it is where a buffer is read afterwards.
- **Never invent facts about data.** A missing file is a normal situation:
  `img/notfound.png`, a log message, continue working.

---

## 5. FPS: what the engine pays for

Before adding a pass, understand what it costs in fill-rate. What is expensive
in this project (descending by typical cost):

- **fullscreen passes in the post chain** (GI with temporal accumulation, SSR,
  voxel fog 3×R8 3D, bloom). One extra screen = tens of fps gone.
- **Grass/vegetation** — overdraw. There is a grass z-prepass
  (`r_veg_zprepass`), GPU-driven compute-cull (`shaders/veg_cull.cs`) +
  `glDrawElementsIndirect`, a chunk regeneration budget (`r_veg_regen_budget`),
  `r_veg_falloff_mult`. Do not add instances "just for now".
- **MSAA on GLES** — inversion: a depth RBO instead of a depth texture (HiZ).
  Do not "improve" multisampling without a measurement on GLES.
- **Shadows**: CSM, static/baked shadow modes, "blobs". Every cascade is a full
  scene pass.
- **Resolution**: dynamic — `r_dynamic_resolution` + `r_dynamic_target_ms`
  (`src/lowend_profile.c`), presets `POTATO/MEDIUM/ULTRA` (a 21-cvar matrix —
  docs/cvars.md), global scale `r_render_scale`. The right answer to "it lags"
  is to switch off/lower via a cvar, not to rewrite the pass.

Obligations:

- **Any "improved the image" comes with `FR_BENCH` numbers** before and after —
  taken on real output, not `offscreen` (docs/testing.md §5). No numbers — no change.
- **Timing work outside the critical path:** heavy tasks in `src/jobs.c` (small
  thread pool) or in loading phases with a budget; not in the frame.
- **Minimize per-frame allocations** and GPU↔CPU transfer; `glReadPixels`,
  `glMapBuffer` in the frame loop is a red flag.
- **Do not introduce new synchronization "just in case".** The fixed logic tick
  (100 Hz desktop / 60 Hz Android, max 8/4 ticks) is a contract; slow logic is
  caught by `sim_acc` clipping, not by a new thread.

---

## 6. Technology boundaries

- **Pure C only** (project style), no C++/STL/RTTI. No new dependencies:
  vendor into `third_party/` only (`lua`, `miniz`) and only with the user's
  explicit consent.
- **SDL3 and OpenGL are the only system calls** for input/window/audio/GL. GL
  functions are loaded at runtime via `SDL_GL_GetProcAddress`; linking
  `-lGL`/GLEW/EGL is not allowed.
- **All GPU code goes through the `sr_*` RHI (`src/srender.h`).** Direct `gl*`
  is allowed where it already exists (`gpu_gl.c`, GLES specifics), but a new
  subsystem goes through `sr_*`, otherwise it cannot be ported.
- **Vulkan/Metal are not "finish along the way"**: `srender_vk.c` brings up a
  device, the rest are stubs; `srender_metal.c` is a skeleton. These are
  separate large tasks.
- **Resources are read only through `resources.c`** (`resources_read_*`, order
  pak → disk → `base_dir` → `../`). A direct `fopen(path)` breaks
  Android/zip packs.
- **Shaders — one source for all backends**: `shader.c` substitutes `#version`
  (300es/310es/330core/430core). Do not multiply `.glsl`/`.glessl` variants.
- **Lua is a sandbox** (`src/lua_engine.c`): no `dofile`/`loadfile`/`load`,
  limits on object counts; a new `engine.*` is registered + documented in
  docs/lua-api.md.
- **Text language:** code comments and `SDL_Log` dev output are English;
  everything the player sees (menu/HUD, `console_print`, cvar `help`, command
  descriptions) is Russian. Details — docs/conventions.md §1, §8.

---

## 7. Build and git

- `make` **re-bakes the light of every map by default** (`BAKE ?= 1`, stamp
  `build/.baked.stamp`) — minutes of work. For code iteration: `make BAKE=0`.
- Build targets: `make gles`, `make linux`, `make win`, `make apk`,
  `make mapzip MAP=…`, `make mapbake MAP=…`, `make bake-maps`, `make backends`.
- **`buildnum` is incremented by every `make`** → `src/buildnum.h` and the
  version string change. That is an expected diff, do not edit it by hand.
- **Commit only when the user explicitly asks.** Do not push, open PRs or create
  branches without being asked.
- Before `git add` — `git status` and look at what got in. Do not stage:
  `config.cfg`, `save.cfg`, `screenshots/`, `models/`, `reference/`, `build/`,
  `buildnum.txt`, `src/buildnum.h` (the list is in `.gitignore`).
- **A headless run overwrites `config.cfg` and drops PNGs into
  `screenshots/`** — a side effect of testing; these files do not go into git,
  and you neither "accidentally" commit them nor delete someone else's.
- Never `--no-verify`, never `reset --hard`/`checkout --`/`clean -f` without an
  explicit request; before them — `git status` and preserve other people's
  uncommitted work.
- Commit message — in the repository's style: a `feat:`/`fix:`/`perf:`/`ui:`
  prefix + what and **why**, in Russian (`git log -8` as the sample).

---

## 8. Documentation is part of the task

- Every new **cvar/command/`map.json` key/`engine.*` function** must appear in
  the corresponding docs file in the same change. A reference line without code
  and code without a reference line are the same defect.
- Do not claim a feature works if it is a stub in the code. The backend status
  (GL/GLES — works, Vulkan — device only, Metal — skeleton, Web — none) must
  stay honest in README.
- Read your own claim out loud and find a `file:line` for it. Not "apparently",
  not "usually in engines like this".
- Comments in code are in **English** and only about the **why** (a hidden
  constraint, an invariant, a workaround for a specific bug). What the code
  **does** is already visible from the names. Do not add `// TODO for the
  future` without a task.

---

## 9. Forbidden without an explicit user request

- Refactoring "for cleanliness", codebase-wide renames, style changes.
- Deleting or editing `veg_gpu_driven_todo.txt` — that is the user's plan
  (discuss it, but do not decide for them).
- Creating a `LICENSE` and any "would be good to have" files (`.editorconfig`,
  CI, a test framework) — ask first.
- Installing/updating system packages, downloading dependencies from the
  internet.
- A new dependency, a new language, a new render backend, a new `src/` folder.
- "Fixing" proven NOT-bugs — the list is in docs/pitfalls.md.

---

## 10. Handover checklist

1. `make BAKE=0` — builds, **no new warnings**.
2. Headless smoke: `SDL_VIDEODRIVER=offscreen FR_TEST=SlS ./build/firerender` —
   `exit 0`, no new `failed`/`skipped` in the log.
3. Targeted check from the table in §1 (A/B screenshot or `FR_BENCH` numbers).
4. Sanitizer run if foreign bytes were touched.
5. `config.cfg`/`screenshots/`/`build*` — not in the diff; temp files removed.
6. Docs updated in the same commit; every link to a feature leads into the code.
7. Report to the user: what changed, **how it is proven** (commands +
   numbers/files), what is left unverified. If a check was not done — say so
   plainly.
