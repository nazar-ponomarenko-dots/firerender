# Pitfalls: proven NOT-bugs and known limitations

This section exists so that an agent **does not spend edits on defects that do not
exist** and does not treat a deliberate decision as a bug. Every item was verified
against the code (and, where required, with sanitizers).

---

## 1. Do not "fix" — these are not bugs

| Observation that "smells like a bug" | Why it is not |
|---|---|
| `events.c:50-64` — ring buffer of deferred events, "overflow silently drops an event" | `events_emit_deferred()` **has no callers at all** — only the declaration in `events.h:49` and the mention in the comment at `main.c:857-860`, where the `flush` happens. A dead path, not an event leak. |
| `game.c:244-247, 276-279` — "pointer to world/camera outlives `console_exec` (UAF)" | The code **itself** accounts for this: `console_exec` can unload the world (`map X`), so the position is passed **by value**, the camera is touched only while `g->cam` is alive (`game.c:276-279` and the comment there), and map action data are static arrays (`MAP_ACTIONS_MAX`), not heap. There is no real UAF; what remains is a cosmetic effect of switching maps from a trigger — it is known and out of scope. |
| `main.c:727-741` — "cvars are force-resynchronized every frame, odd" | **Deliberate:** without the forced `sync_cvars` resync, which does not look at "changes", `g_shadow_on`/fog were not brought in line with the mode — CSM was rendered needlessly in blob/off mode, fog stayed stuck in its initialization mode. The `FR_TEST=s/o` letters are applied **after** this resync exactly for that reason (`main.c:742-747`). |
| `entity.c` — "serial overflows 16 bits, the handle wraps" | Serial wrap-around is unreachable: 256 slots, serial grows per spawn on a slot. The real defect was **signed** packing/comparison (the handle went negative at `serial >= 32768`, while `entity_get` rejected `handle <= 0` — a live entity "died" for Lua). Fixed: packing through `unsigned`, an invalid handle is strictly `0`. |
| `loading.c` — "the loading screen does not pump events → hang/reentrancy" | The loading screen calls no handlers that could re-enter loading. There is no reentrancy. |
| `shader.c` — "cyclic shader `#include`s will hang" | Include depth is capped (8): a cycle produces a **GLSL compile error** with a message, not a hang. |
| `config/menu.json` — "the file is never read, delete it" | The engine really does not read it, but the file may be used by the APK build. Do not delete; the status is noted in the README. |
| `renderer.c` — "the MSAA resolve happens even when post passes are off" | The `need_depth` condition (`renderer.c:945-950`): the fullscreen depth blit is skipped when GI / wet SSR / voxel fog are not needed. This is a saving, not an oversight. |

---

## 2. Real defects, already fixed (do not re-apply a "correction")

| Where | What it was | What it is now |
|---|---|---|
| `glb.c`, `glb_anim.c` — reading GLB chunks | `off + clen > filesz` on `unsigned` wrapped around, a bogus `bin_len ~4 GB` passed the checks → read past the heap buffer | check by subtraction: `clen > filesz - off` |
| `glb.c:read_vertex`, `glb_anim.c:rvv` | a `VEC4` accessor wrote 4 float into `float p[3]` → stack corruption | accessors are matched against the expected component count (`ap->vec != 3`, `au vec==2`, `aj/aw vec==4`); a mismatch — "primitive skipped" + log |
| `glb.c`, `glb_anim.c`, `audio.c`, `hud.c` | unchecked `malloc`/`SDL_LoadWAV_IO`/`stbtt_MakeGlyphBitmap` (incl. writing `out->nodes` before checking `out`) | guard right after the alloc, free what belongs to the function, zero the counters |
| `cvars.c:cvar_slot_new` | a slot from `realloc` with garbage; "adopting" an existing slot set `CVAR_STRING` without zeroing → `strcmp(c->str, c->def_str)` in `cvar_save_config` read garbage and injected it into `config.cfg` | `memset(c, 0, sizeof *c)` when the slot is handed out |
| `render_ctx.h` + `renderer.c` | dead field `refl_cubemap_dirty`, which was only ever zeroed | removed (the water cubemap is **static**, see §3) |

---

## 3. Known limitations — accept as fact

- **`SDL_VIDEODRIVER=dummy` has no GL support.** `SDL_CreateWindow failed …
  driver (dummy)`, `exit 1`. CI tolerates this via `|| true`, so the CI "basic
  test" covers only the path up to window creation. Headless rendering =
  `offscreen` (works without `DISPLAY`).
- **The water reflection cubemap is static.** Faces are written by the
  `cubemap_capture` command into `cubemaps/` and loaded at startup; the cubemap is
  **not re-rendered** per frame. Do not "fix" it as outdated — it is a design.
- **Vulkan — device bring-up only** (`srender_vk.c`), the rest are stubs.
  **Metal — a skeleton** (`srender_metal.c`). **Web/Emscripten — no Makefile target.**
- **There are no unit tests and no test framework**; `console_selftest` is the only
  built-in set of checks. Do not create `tests/`, `make test`, gtest scaffolding.
- **There is no `LICENSE` in the repository** — rights are not declared. Do not add
  the file and do not invent a license without an explicit user decision.
- **`objects[].rot` is parsed (`mapdata.c:678`) but not applied by the renderer** —
  billboard objects are oriented toward the camera/terrain. Documented in
  docs/map-json.md; do not silently "fix" it, this is a note, not a task.
- **Object collision is a cylinder** (`objects.c:311-317`, `x/z/radius`): no height
  and no tilt. For exact shapes — `collisions[]`.
- **SDL3 itself leaks ~256 bytes at exit** (internal threads) — which is why CI uses
  `detect_leaks=0`. This is not project code.
- **`make` re-bakes the light of every map by default** (`BAKE ?= 1`); every `make`
  also increments the build number → `buildnum.txt`/`src/buildnum.h` change
  (both are in `.gitignore`).
- **A headless run overwrites `config.cfg`** and writes PNGs into `screenshots/` —
  a side effect, not a "dirty tree".
- **Two cosmetic header anomalies:** `linalg.c` is named `math.c` in its banner,
  `lowend_profile.c` has an empty second banner line. They need no fixing.
- **`veg_gpu_driven_todo.txt` is the user's plan.** STAGE 1 items are already done
  (the desktop context is raised to GL 4.5 CORE in `main.c:485`, `g_gl_gpu_driven`
  and GPU-driven grass work); STAGE 5 and 6.1/6.3/6.4 are open. Do not rewrite the
  file's contents without asking.

---

## 4. Typical regressions that are easy to introduce

1. **A new `gl*` call without a `p_glXxx == NULL` check** → crash on GLES/Android.
   It must be in the `gl_loader.c` table and have a fallback.
2. **A bound by addition** (`off + len > size`) — unsigned overflow cancels the
   check. Subtraction only.
3. **`char buf[N]` + `snprintf` in a loop without checking the returned length** —
   silently truncated paths (`resources_*`, `map%d.zip`).
4. **A partially filled structure after a failed alloc** — zero the counters,
   otherwise `*_free` and the following loops will walk garbage.
5. **Allocation/reading inside the frame loop** — `p95/p99` degradation at the same mean.
6. **A new fullscreen pass "for consistency"** — tens of fps lost; always a
   `*_is_on()`/cvar condition, as in `need_depth`.
7. **A pointer to the camera/world outliving `console_exec`** — the `map X` command
   unloads the scene in the middle of your function. Value copy + liveness check, as
   in `game.c:276-279`.
8. **Wiping someone's `config.cfg` / committing `screenshots/`** — these are run
   artifacts and do not go into git.

---

## 5. How to correctly "find a bug"

Before editing:

1. Read the **whole** data path (parser → structure → consumer), not the line that
   "looks suspicious".
2. Check whether the suspicious code has **callers**. A dead path is not a bug, it
   is a removal candidate (removal — only on request).
3. Reproduce: a minimal corrupt file / command / `FR_TEST` letter, and read the
   **log**, not just the exit code.
4. A sanitizer as in docs/testing.md §4 — it separates real memory corruption from
   "ugly code".
5. If there is no defect but the purpose of the code is unclear — **ask the user or
   document it**, do not "simplify". Examples of such code are in §1.
