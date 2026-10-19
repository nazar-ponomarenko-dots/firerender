# Testing firerender with the headless loop

Document for agents and CI. Everything below has been verified on this tree; if it
disagrees with the code, trust the code and fix the document.

---

## 1. The three commands to run after any edit

```sh
make BAKE=0                                                       # build without re-baking the maps
SDL_VIDEODRIVER=offscreen FR_TEST=SlS ./build/firerender          # GL pipeline + glb + animation
SDL_VIDEODRIVER=offscreen FR_TEST=Sx ./build/firerender           # console and cvar selftest
```

The letter `S` is needed in the second run as well: `FR_TEST=x` without it only
prints `console selftest: OK` and keeps spinning frames — the headless run will not
finish on its own (a CI-style `timeout 5` is then mandatory).

`make` without `BAKE=0` re-bakes the light of **all** maps (`BAKE ?= 1`,
`Makefile:208-211`, stamp `build/.baked.stamp`) — minutes instead of seconds. The
stamp prevents a repeated re-bake only if shaders/maps have not changed.

### Why `dummy` ≠ headless render

| `SDL_VIDEODRIVER` | What happens |
|---|---|
| `offscreen` | A GL context is created, the frame is drawn, `FR_TEST=S` writes a PNG. Works **without** `DISPLAY` — verified. |
| `dummy` | `SDL_CreateWindow failed: OpenGL support is either not configured in SDL or not available in current SDL video driver (dummy)`, **exit 1**. Not a single frame. |
| `x11`/`wayland` | normal launch, the window is visible to the user. |

CI (`.github/workflows/ci.yml`) uses `dummy` and tolerates `exit 1` via `|| true`.
That means **"build with ASAN/UBSAN → run basic test" in CI covers only the path up
to window creation** (`boot_init` before `SDL_CreateWindow`) and nothing
related to render logic. The agent must run the real headless pass on `offscreen`
itself.

---

## 2. Environment variables (`FR_*`)

| Variable | Meaning | Where it is read |
|---|---|---|
| `FR_TEST` | string of letters — a set of tests, all of them are applied | `main.c:250` `fr_test_apply`, then in the game loop |
| `FR_MAP` | map name overriding the choice in `config.cfg` | `game.c:729` |
| `FR_POS` | `"x,z[,yaw_deg[,pitch]]"` — teleport after the world loads; enables `fly`, eye = height + 2.4 m | `main.c:298` |
| `FR_W`, `FR_H` | window size (screenshots at a different resolution) | `main.c:592` |
| `FR_SHOT` | `<frame>` — dump that frame to `/tmp/fr_shot.ppm`; `<from>-<to>` — dump **every** frame of the range to `/tmp/fr_shot_<n>.ppm`. Consecutive frames of one run differ by a single logic tick, so the range form is how temporal instability (flicker, z-fighting) is measured headlessly | `main.c:505` `loop_fr_shot` |
| `FR_MSAA` | number of MSAA samples overriding the preset | `app.c:456` |
| `FR_GL_PROBE_FAIL` | forces the GLSL dialect probe to report "unsupported" — exercises the boot path of a driver that cannot compile the engine's shaders (log + window title + native dialog, map load refused). Under `FR_TEST` the dialog is skipped, and the run exits `1` with `FR_TEST: map load refused` | `shader.c:227` `shader_dialect_supported` |
| `FR_CONSOLE` | open the console at startup | `main.c:770` |
| `FR_BAKE` | offline light-compilation mode (writes to `map/baked/`) | `main.c:938`, target `make mapbake` |
| `FR_BENCH` | enable frame timing and apply the benchmark quality preset (the window is NOT resized) | `main.c:697,715` |
| `FR_BENCH_PRESET` | `0..3` — which quality preset to use for the measurement | `main.c:765` (`app_apply_benchmark_preset`) |
| `FR_BENCH_GPU` | measure with GPU elapsed queries (`GL_TIME_ELAPSED`, ring of 8) | `main.c:716` |

---

## 3. `FR_TEST` letters

See `src/main.c:250-400` (`fr_test_apply`) and the frame loop at `main.c:722` (`while (running)`).

### What to launch/enable

| Letter | Action |
|---|---|
| `x` | `console_selftest()` — set/clamp of cvars, text input, Left/Backspace/Delete, aliases, history (`console.c:1177`) |
| `S` | automatic screenshot on **frame 450**, exit on **frame 480** (without `FR_BENCH`) — `main.c:924-932` |
| `D` | camera demo flight (`game_demo_toggle`) — a nice viewpoint for a shot: `FR_TEST=SD` |
| `g` | `cmd_glb("models/vaz-2105.glb")` — static mesh with automatic placement on the terrain |
| `l` | `cmd_glbanim` animated model + camera looking down — skinning, face UV |
| `R` | `cmd_phys_rope("16")` — Verlet rope in front of the camera |
| `H` | female + ragdoll with a push, plus automatic camera aim every frame |
| `K` | kill the player ~1.2 s before the shot (killcam, `health`) |
| `m` | `"fire"` particle burst at the player |
| `d` | decal under the player (`decal_place`) |
| `a` | rain (`fx_weather_start("rain", ...)`) |
| `n` | snow |
| `y` | explosion |
| `w` | forced wetness 1.0 without rain particles — clean SSR A/B |
| `E` | `ecs_enable 1` + `ecs_selftest()` (ECS core) + a clean generation after the test |
| `V` | `ecs_enable 1` + `ecs_scene_verify_once()` — scene<->ECS bridge consistency on the first tick |
| `u` | `phys_bodies 1` + `phys_body_selftest()` — bodies: fall onto static, stack, impulse, box, cylinder, limit, kill switch, push |
| `U` | `phys_bodies 1` + `cmd_phys_props("8")` — stack of crates in front of the camera (after load_world and `FR_POS`) |
| `Z` | `audio_selftest()` (occlusion on a synthetic wall) + after load — the muffling multiplier for each map `sounds[]` entry and a reference point "under the feet" |

### What to disable (subsystem toggles "overriding the mode" — for A/B frames)

| Letter | What gets switched off |
|---|---|
| `r` | SSR (`r_ssr 0`) |
| `s` | shadows (`shadow_toggle()`) — applied **after** the cvar resync, otherwise `shadow_init` overwrites it (`main.c:788`) |
| `o` | voxel fog (`voxfog_set(1)`) — same story with the ordering |
| `C` | static cascade cache (`g_shadow_cache = 0`) |
| `p` | terrain shadows (`g_shadow_terrain = 0`) |
| `q` | vegetation shadows (`g_shadow_veg = 0`) |
| `c` | shadow cull checks (`g_shadow_cull = 0`) |
| `v` | all vegetation (`veg_set_visible_test(0)`) |
| `e` | static vegetation layer (`veg_set_static_main(0)`) |
| `f` | dynamic vegetation layer (`veg_set_dyn_main(0)`) |
| `t` | terrain node (`g->terr_node->render = NULL`) — requires an already loaded world, hence applied after load |
| `k` | sky node (`g->sky_node->render = NULL`) |
| `O` | positional audio occlusion (`snd_occlusion 0`) — A/B pair for `Z` |

Letters combine in a single string: `FR_TEST=SlS` = load the world + animation +
screenshot + automatic exit. `FR_TEST=SqS` — a shot without grass shadows.

---

## 4. Sanitizers

As in CI (and the same must be run locally on the parsers):

```sh
make clean
make BAKE=0 EXTRA_CFLAGS="-fsanitize=address,undefined -O1 -g -fno-omit-frame-pointer"
ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 \
UBSAN_OPTIONS=print_stacktrace=1:abort_on_error=1 \
SDL_VIDEODRIVER=offscreen FR_TEST=SlS ./build/firerender
```

- `detect_leaks=0` — SDL3 itself leaks ~256 bytes at shutdown (internal threads);
  that is not project code. Look for local leaks separately, without CI mode.
- On asset parsers run **broken files**: a truncated `.glb`, a chunk with
  `chunk_len ≈ 0xFFFFFFFF`, a model with `"POSITION"` of type `VEC4`, a `map.json`
  with indices beyond array bounds. Expected behaviour — a message in the log and
  the element skipped, not SEGV/stack-buffer-overflow.
- `EXTRA_CFLAGS` is the only Makefile hook for flags; `CFLAGS` overwrites the whole
  set and breaks the `-I` paths.

---

## 5. Performance measurement

`FR_BENCH=1` (`main.c:129-153`): 600 frames **and** 10 s of warm-up, then 30 s of
sampling (up to 60000 frames), then one line in the log:

```
BENCH native wall-clock: N frames 30.0x s | NN.N fps | mean X.XXX ms p50 ... p95 ... p99 ... | GPU X.XXX ms
```

Details one must know in order not to fool oneself with the numbers:

- `FR_BENCH` does **not** resize the window: it only warns when the drawable ended up
  a different size than requested (`main.c:697`). It applies the benchmark quality
  preset (`main.c:765`) and **disables hot-reload of shaders/textures** (`main.c:798`)
  — otherwise the 1 Hz `stat()` wakes the disk inside the measurement.
- `GPU` is present only with `FR_BENCH_GPU=1` and if GL timer queries exist; without
  them it is `0.000`.
- **`FR_MSAA` can silently do nothing.** It is applied inside the benchmark preset
  (`app.c:456`), so it is read only with `FR_BENCH=1`, and anything loaded later —
  `config.cfg` (`r_msaa` is `CVAR_ARCHIVE`) or a map's own cvars — overwrites it.
  Measured: `FR_MSAA=0` still logged `MSAA: 4x`. To force a value use
  `FR_CVAR="r_msaa 0"` (applied after the preset, `main.c:860`) and confirm it in the
  log — otherwise an "MSAA on/off" A/B measures the same configuration twice.
- `p95/p99` matter more than the mean: a regression in this engine looks like "the
  mean is the same, p99 grew" (chunk regeneration budget, allocations, grass
  rebuild).
- Always compare with identical `FR_POS`/`FR_MAP`/`FR_W`/`FR_H`/`FR_MSAA` and on the
  same preset. Different points on the map — non-comparable numbers.
- Sampling ends at `min(30 s, 60000 frames)` — on a very fast rig the frame limit,
  not the time, determines the report.
- **Numbers under `offscreen` are not comparable with real gameplay.** Verified:
  `FR_BENCH=1 FR_BENCH_PRESET=2` on the offscreen rig gives
  `BENCH native wall-clock: 60000 frames 1.11 s | 54035.8 fps | mean 0.019 ms …`
  — a frame costs almost nothing there. For FPS A/B you need a real output
  (`x11`/`wayland` on the same GPU and resolution, or the device), while `offscreen`
  is only for "did not crash / did render". CI has no GPU measurements at all.
- Per-module in-frame timings: the `profiler` command (`src/profiler.h`, 14 slots,
  CPU time + GPU time for the previous frame; the vertex/triangle/draw-call counters
  are maintained by `gl_loader.c` automatically).

---

## 6. Screenshots and A/B images

- Automatic shot: `FR_TEST=S` → `screenshots/shot_<YYYYMMDD_HHMMSS>.png`
  (`commands.c:305-361`, the folder is created by itself). A manual shot — the
  `screenshot` command or `F12`.
- Viewpoint — `FR_POS="300,500,180,-0.2"` (x, z, yaw°, pitch°).
- A/B: the same `FR_POS` + `FR_MAP`, two toggle letters, two files, side-by-side
  comparison. That is exactly the proof that "the image did not get worse".
- **Never A/B two runs of the engine.** The simulation is driven by wall-clock
  time, so frame N is not reproducible between processes: two runs of the SAME
  binary at `FR_SHOT=900` differ in ~50% of pixels (measured 2026-10-15, 640x360,
  `FR_TEST=Sv`, HEAD vs HEAD). Compare frames **inside one run** — that is what
  the `FR_SHOT=<from>-<to>` form is for.
- `screenshots/` is in `.gitignore`; **do not delete other people's shots** — the
  folder is shared with the user's manual runs.

---

## 7. Side effects of runs (know them, do not panic)

- Any engine exit **overwrites `config.cfg`** (archived cvars, diff-against-default).
  The file is in `.gitignore`; if the user kept manual settings there — do not commit
  it and do not "fix" its contents.
- Every `make` increments the build counter → `buildnum.txt` and `src/buildnum.h`
  change (both in `.gitignore`), the version string becomes
  `firerender 0.4 build #N`.
- A headless run may drop a PNG into `screenshots/`.
- The first `FR_TEST=S` after a shader change takes longer: re-bake/warm-up, then the
  numbers stabilize.

---

## 8. What is not here

- **There are no unit tests and no test framework.** `console_selftest` is the only
  built-in set of assertions. Do not invent `tests/`, `make test`, a pytest wrapper;
  the loop is "built, ran headless, read the log and the numbers".
- **GL regression is not checked pixel-wise.** Image comparison is done by a human
  (or by your own script outside the repository). Do not add binary references to git.
- **`make apk`, `make vulkan`, `make metal`** are separate builds; if the task does
  not concern them, desktop + `make gles` is enough.
- **Web/Emscripten — there is no Makefile target for it**, do not "finish it along
  the way".
