# Console variables (cvars)

Reference for all engine variables. The source of truth is the
`cvar_register` / `cvar_register_ex` / `cvar_register_string` calls
(`src/cvars.c:56-107`); in game the full list with current values is given by
the `cvarlist` command, help for a specific one — `help <name>`.

## How it works

| Mechanic | Behavior |
|---|---|
| Archiving | Variables with the `CVAR_ARCHIVE` flag are written to `config.cfg` **only if they differ from the default** (`console.c:1018-1039`). `cfg save` / `cfg load` — manual control. |
| Range | `cvar_register_ex(name, def, min, max, …)`; `min > max` means "no limits" (`cvar_register` and `cvar_register_string` register exactly that way). On set, the value is clamped (`cvars.c:147-150`). |
| Cheats | `CVAR_CHEAT` flag: the variable does not change while `sv_cheats 0` (`cvars.c:143-146`). |
| Re-registration | If the name already exists (created by `config.cfg`, `map.json` or Lua), the slot is **adopted**: default/range/flags are updated, but the current value is kept (`cvars.c:60-72`). That is why module init order matters more than the "second" declaration. |
| Unknown name | `cvar <name> <value>` for a nonexistent variable creates it tagged `map/lua-переменная` and **without** `CVAR_ARCHIVE` — a typo will not settle into the config (`cvars.c:126-139`). |
| String | `CVAR_STRING`: `name` (player name). The number and the string are kept in sync. |
| Map defaults | The `cvars = { … }` section in `map.json` is applied on world load and overrides engine defaults (up to 64 names, `mapdata.c:965-975`). |

---

## Core, input, simulation

| Name | Default | Range | Purpose | File |
|---|---|---|---|---|
| `developer` | 1 | 0…1 | Print `SDL_Log` to the console | `console.c:910` |
| `fov` | 60 | 5…120 | Field of view, degrees | `console.c:911` |
| `cl_sens` | 0.0025 | 0.0001…0.1 | Mouse sensitivity | `console.c:912` |
| `m_invert` | 0 | 0…1 | Mouse Y inversion | `console.c:964` |
| `m_smooth` | 0 | 0…1 | View interpolation (1 = heavy smoothing) | `console.c:965` |
| `cl_walkspeed` | 2.5 | 0…100 | Walk speed, m/s | `console.c:913` |
| `cl_flightspeed` | 25 | 0…500 | Flight speed (noclip), m/s | `console.c:915` |
| `sv_maxspeed` | 0 | 0…100 | Player speed cap; 0 = `cl_walkspeed` | `console.c:914` |
| `sv_gravity` | 20 | 0…100 | Gravity, m/s² | `console.c:916` |
| `sv_maxslope` | 60 | 0…89 | Steepest climbable slope (0 = off) | `console.c:917` |
| `sv_jumpspeed` | 7 | 0…50 | Initial jump velocity, m/s | `console.c:918` |
| `sv_cheats` | 1 | 0…1 | Allow cheats (flight `+` commands, `noclip`, `r_wireframe`) | `console.c:972` |
| `sv_timescale` | 1 | 0.01…10 | Time scale (cheat) | `console.c:973` |
| `host_framerate` | 0 | 0…1000 | FPS limit; 0 = no limit | `console.c:974` |
| `name` | `"player"` | string | Player name | `console.c:982` |
| `cam_thirdperson` | 0 | 0…1 | Third-person view | `console.c:975` |
| `cam_tpd` | 4 | 1…12 | Third-person camera distance, m | `console.c:976` |
| `r_touch_ui` | auto | 0…1 | Touch controls (joystick + buttons); auto = enabled on a touch device | `touch.c:97` |
| `r_fullscreen` | 0 | 0/1 | Fullscreen mode | `console.c:967` |
| `r_vsync` | 0 | 0/1 | Vertical synchronization | `console.c:968` |
| `profiler` | 0 | 0/1 | Performance panel (polygons/time) | `console.c:971` |
| `snd_debug` | 0 | 0/1 | Playing sounds in the HUD (debug) | `console.c:970` |

## Sound

| Name | Default | Range | Purpose | File |
|---|---|---|---|---|
| `snd_master` | 1 | 0…1 | Master volume | `console.c:977` |
| `snd_sfx` | 1 | 0…1 | Footsteps and effects | `console.c:978` |
| `snd_dialog` | 1 | 0…1 | Dialogs | `console.c:979` |
| `snd_music` | 1 | 0…1 | Music | `console.c:980` |
| `snd_occlusion` | 1 | 0…1 | Terrain occlusion of positional sound: effect strength, 0 — off | `console.c:981`, `audio.c:audio_occlusion_at` |
| `snd_occlusion_min` | 0.25 | 0…1 | Occlusion floor: minimum volume of a fully blocked source | `console.c:983` |

## Body physics and the ECS layer

| Name | Default | Range | Purpose | File |
|---|---|---|---|---|
| `phys_bodies` | 0 | 0…1 | Rigid bodies (spheres/boxes on top of the Verlet world); 0 — bodies stay where they landed, ropes and ragdolls keep stepping | `phys_body.c:533` |
| `phys_bounce` | 0.25 | 0…0.9 | Restitution: fraction of normal velocity kept when a particle hits a particle/static geometry | `phys_body.c:535` |
| `phys_push_speed` | 4 | 0…12 | Speed (m/s) at which the player pushes bodies around; 0 = don't push | `phys_body.c:537`, `movement.c` |
| `ecs_enable` | 0 | — | ECS layer: syncs the scene tree with components and runs systems | `main.c:221` |

## Pipeline and post-processing

| Name | Default | Range | Purpose | File |
|---|---|---|---|---|
| `r_msaa` | 0 | 0…16 | MSAA; applies immediately | `console.c:922`, `bloom.c:324` |
| `r_render_scale` | 1 | 0.4…1 | Scene render scale (1 = window resolution) | `console.c:943` |
| `r_scene_ldr` | 0 | 0/1 | Scene in RGBA8 without HDR — faster on weak GPUs | `console.c:959` |
| `r_present_blit` | 1 | 0/1 | Present: grade+blit in two passes (1) / a single composite into the native target (0) | `renderer.c:597` |
| `r_exposure` | 1 | 0.1…5 | HDR exposure (tone mapping) | `bloom.c:320`, `console.c:925` |
| `r_gamma` | 1 | 0.5…2.5 | Gamma correction | `bloom.c:321`, `console.c:924` |
| `r_saturation` | 1.4 | 0…3 | Saturation | `bloom.c:322`, `console.c:926` |
| `r_contrast` | 1 | 0.5…2 | Contrast | `bloom.c:323`, `console.c:927` |
| `r_bloom` | 1 | 0/1 | Bloom on/off | `bloom.c:316`, `console.c:923,962` |
| `r_bloom_threshold` | 0.75 | none | Brightness threshold for the bright-pass | `bloom.c:317` |
| `r_bloom_intensity` | 1.0 | none | Strength of bright pixels | `bloom.c:318` |
| `r_bloom_strength` | 0.425 | none | Bloom strength in the composite | `bloom.c:319` |
| `r_crosshair` | 1 | 0/1 | Crosshair (dot at center) | `console.c:963` |
| `r_wireframe` | 0 | 0/1 | Wireframe mode (cheat) | `console.c:969` |

The `r_bloom_*` defaults are taken from the map's built-in values
(`mapdata.c:99-101`), i.e. before `map.json` loads these are per-map numbers,
not a code constant.

## Shadows and baked light

| Name | Default | Range | Purpose | File |
|---|---|---|---|---|
| `r_shadows_mode` | 1 | 0…3 | 0 = off, 1 = CSM, 2 = static (baked), 3 = blobs | `console.c:919` |
| `r_baked_light` | 1 | 0/1 | Read the map's baked light (`map/baked/`), if it exists | `console.c:947` |

## Global illumination (screen-space, Lumen-like)

| Name | Default | Range | Purpose | File |
|---|---|---|---|---|
| `r_gi` | 1 | 0/1 | Dynamic GI | `gi.c:99`, `console.c:921` |
| `r_gi_scale` | 0.5 | 0.25…1 | GI resolution relative to the screen | `gi.c:141`, `console.c:940` |
| `r_gi_rays` | 6 | 2…32 | Rays per pixel | `gi.c:113`, `console.c:941` |
| `r_gi_steps` | 10 | 4…32 | Ray-march steps | `gi.c:115`, `console.c:942` |
| `r_gi_strength` | 0.38 | 0…3 | Strength of indirect light | `gi.c:104` |
| `r_gi_ao` | 0.8 | 0…1.5 | Contact shadows + sky occlusion | `gi.c:108` |
| `r_gi_radius` | 12 | 1…64 | Indirect-light gather radius, m | `gi.c:117` |
| `r_gi_ao_radius` | 3 | 0.2…16 | Ambient occlusion radius, m | `gi.c:119` |
| `r_gi_sky` | 0.45 | 0…1 | Large-scale shading by sky openness | `gi.c:121` |
| `r_gi_skylight` | 1 | 0…3 | Sky light in shadows (color from the panorama) | `gi.c:125` |
| `r_gi_clamp` | 1 | 0.1…8 | Per-pixel cap on indirect light energy | `gi.c:127` |
| `r_gi_bounce` | 1.15 | 1…2.5 | Saturation of reflected light (colored bounces) | `gi.c:129` |
| `r_gi_reflect` | 0.35 | 0…1 | Specular GI sheen | `gi.c:131` |
| `r_gi_spread` | 0.18 | 0…1 | Chained re-reflections | `gi.c:135` |
| `r_gi_temporal` | 0.92 | 0…0.96 | Temporal accumulation (0 = noisy) | `gi.c:137` |
| `r_gi_blur` | 1.35 | 0…4 | Denoise radius (texels), grows with distance | `gi.c:139` |
| `r_gi_debug` | 0 | 0…2 | 1 = indirect light only, 2 = AO only | `gi.c:143` |

## Screen-space reflections (SSR)

| Name | Default | Range | Purpose | File |
|---|---|---|---|---|
| `r_ssr` | 1 | 0/1 | SSR on/off | `ssr.c:64` |
| `r_ssr_strength` | 0.7 | 0…1 | Reflection strength | `ssr.c:66` |
| `r_ssr_steps` | 16 | 4…32 | March steps | `ssr.c:68` |
| `r_ssr_maxdist` | 24 | 4…64 | March distance, m | `ssr.c:70` |
| `r_ssr_thick` | 0.2 | 0.02…1 | Hit window thickness, m | `ssr.c:72` |
| `r_ssr_rough` | 0.06 | 0…1 | Reflection roughness (blur) | `ssr.c:74` |
| `r_ssr_fallback` | 1 | 0/1 | Sky cubemap on a miss | `ssr.c:76` |
| `r_ssr_mask` | 1 | 0/1 | Terrain wetness mask | `ssr.c:78` |
| `r_ssr_wet` | 0 | 0…1 | Test: forced wetness without rain | `ssr.c:80` |
| `r_ssr_temporal` | 0.75 | 0…0.95 | Temporal smoothing (against flicker) | `ssr.c:84` |
| `r_ssr_debug` | 0 | 0…4 | 1 zones, 2 hit/fallback, 3 weight, 4 color | `ssr.c:82` |

## Light sources

| Name | Default | Range | Purpose | File |
|---|---|---|---|---|
| `r_clustered` | 1 | 0/1 | Forward+ clusters: many point/spot light sources | `lights.c:63` |
| `r_light_debug` | 0 | 0…2 | 1 = light-source heatmap | `lights.c:65` |

## Fog

| Name | Default | Range | Purpose | File |
|---|---|---|---|---|
| `r_fog_mode` | 1 | 0…2 | 0 = off, 1 = volumetric (voxel), 2 = simple (1 mix) | `console.c:961` |
| `r_cheap_fog` | 0 | 0/1 | Fog in shaders only, without voxfog passes | `console.c:957` |
| `r_fog_accum` | 1 | 0/1 | Temporal accumulation of volumetric fog (anti-flicker) | `voxfog.c:365` |
| `r_fog_accum_blend` | 0.8 | 0…0.95 | History weight (0.8 ≈ 5 frames of averaging) | `voxfog.c:367` |

The fog parameters themselves (per-layer densities, colors, sun rays) are not
cvars but the `fog` section in `map.json`: see [map-json.md](map-json.md).

## Sky

| Name | Default | Range | Purpose | File |
|---|---|---|---|---|
| `r_clouds` | 0.25 | 0…1 | Procedural cloud coverage | `sky.c:109` |

## Terrain

| Name | Default | Range | Purpose | File |
|---|---|---|---|---|
| `r_terrain_lod1` | 200 | 10…2000 | Detailed mesh distance, m | `terrain.c:690` |
| `r_terrain_lod2` | 500 | 20…4000 | Mid mesh distance, m | `terrain.c:692` |
| `r_terrain_lod3` | 1200 | 30…8000 | Coarse mesh distance, m | `terrain.c:694` |
| `r_terr_detail_dist` | 0 | 0…500 | Detail LOD: farther — base map (0 = off) | `console.c:932` |

The LOD defaults come from the module's static variables (`terrain.c:264-266`).

## Vegetation

| Name | Default | Range | Purpose | File |
|---|---|---|---|---|
| `r_grass_density` | 1.2 | 0…10 | Grass, stems/m² | `vegetation.c:1472`, `console.c:938` |
| `r_wheat_density` | 140 | 0…200 | Wheat, stems/m² | `vegetation.c:1471`, `console.c:937` |
| `r_dandelion_density` | 0.05 | 0…2 | Dandelions, stems/m² (on top of grass) | `vegetation.c:1473`, `console.c:939` |
| `r_veg_vis_mult` | 1 | 0.25…1 | Draw-radius multiplier (1 = whole map) | `console.c:928` |
| `r_veg_dyn_mult` | 1 | 0.4…1 | Dynamic chunk radius multiplier | `vegetation.c:1492` |
| `r_veg_minpix` | 1 | 0…8 | Min on-screen footprint of a grass blade in px (0 = don't cull) | `vegetation.c:1654`, `console.c:930` |
| `r_veg_falloff_mult` | 1 | 0.3…1 | Field density falloff: <1 — sparser already toward the middle ring | `console.c:953` |
| `r_veg_lod_shift` | 0 | 0…2 | Wheat LOD shift toward the camera (1-2 = wider sooner) | `vegetation.c:1487` |
| `r_veg_regen_budget` | 4096 | 1…4096 | Chunks regenerated per frame (lower = fewer CPU hitches) | `vegetation.c:1489`, `console.c:955` |
| `r_veg_zprepass` | 1 | 0/1 | Grass Z-prepass: cuts overdraw (only when MSAA is off) | `vegetation.c:1656` |
| `r_veg_gpudriven` | 1 | none | GPU-driven compute-cull for grass (0 = instanced fallback) | `vegetation.c:1653` |

The default densities are the module's static values (`vegetation.c:55-59`);
the map overrides them via the `veg` section.

## Low-end GPUs and dynamic resolution

| Name | Default | Range | Purpose | File |
|---|---|---|---|---|
| `r_simple_render` | 0 | 0/1 | "Simplified render" bundle (`«Упрощённый рендер»`, see below) | `console.c:945` |
| `r_lowend_profile` | 0 | 0…2 | 0 = balanced, 1 = old smartphone, 2 = Celeron iGPU | `lowend_profile.c:25`, `console.c:949` |
| `r_dynamic_resolution` | 0 | 0/1 | Dynamic scene scale based on frame time | `lowend_profile.c:27`, `console.c:951` |
| `r_dynamic_target_ms` | 33.3 | 16.6…50 | Target frame time for dynamic resolution | `lowend_profile.c:29` |

---

## Quality presets

The «КАРТОШКА / СРЕДНИЕ / УЛЬТРА» buttons in the «Графика» menu
(`app.c:502-507`) set 21 variables at once — `app.c:350-421`; there is no
separate console command for a preset.

| # | Variable | КАРТОШКА | СРЕДНИЕ | УЛЬТРА |
|---|---|---|---|---|
| 1 | `r_msaa` | 0 | 4 | 8 |
| 2 | `r_gi` | 0 | 1 | 1 |
| 3 | `r_shadows_mode` | 3 (blobs) | 1 (CSM) | 1 (CSM) |
| 4 | `r_wheat_density` | 15 | 80 | 140 |
| 5 | `r_grass_density` | 1.0 | 5.0 | 10.0 |
| 6 | `r_dandelion_density` | 0 | 0.05 | 0.10 |
| 7 | `r_ssr` | 0 | 0 | 1 |
| 8 | `r_bloom` | 0 | 1 | 1 |
| 9 | `r_clouds` | 0 | 0.15 | 0.30 |
| 10 | `r_fog_mode` | 2 (simple) | 1 (volumetric) | 1 (volumetric) |
| 11 | `r_gi_scale` | 0.25 | 0.5 | 0.5 |
| 12 | `r_gi_rays` | 4 | 6 | 8 |
| 13 | `r_gi_steps` | 6 | 10 | 12 |
| 14 | `r_render_scale` | 1.0 | 1.0 | 1.0 |
| 15 | `r_veg_vis_mult` | 0.45 | 1.0 | 1.0 |
| 16 | `r_veg_minpix` | 4 | 1 | 1 |
| 17 | `r_terr_detail_dist` | 60 | 0 | 0 |
| 18 | `r_simple_render` | 1 | 0 | 0 |
| 19 | `r_veg_falloff_mult` | 0.5 | 1.0 | 1.0 |
| 20 | `r_cheap_fog` | 1 | 0 | 0 |
| 21 | `r_scene_ldr` | 1 | 0 | 0 |

After the table `simple_render_apply()` runs (`app.c:360-405`) — it is the same
thing the "Simplified render" (`«Упрощённый рендер»`) checkbox in the menu does,
and it **does not touch** the densities and effects (those are separate settings):

| Variable | `r_simple_render 1` (desktop) | `…1` (Android) | `…0` |
|---|---|---|---|
| `r_render_scale` | 1.0 | 0.60 | 1.0 |
| `r_scene_ldr` | 1 | 1 | 0 |
| `r_veg_zprepass` | 1 | 1 | 0 |
| `r_veg_vis_mult` | 0.45 | 0.28 | 1.0 |
| `r_veg_minpix` | 4 | 6 | 1.0 |
| `r_terr_detail_dist` | 60 | 30 | 0 |
| `r_veg_falloff_mult` | 0.5 | 0.32 | 1.0 |
| `r_veg_regen_budget` | 6 | 3 | 2 |
| `r_veg_dyn_mult` | 0.7 | 0.6 | 1.0 |
| `r_veg_lod_shift` | 0 | 1.5 | 0 |

On Android the «КАРТОШКА» preset additionally lowers the densities
(`r_wheat_density` 6, `r_grass_density` 0.6, `r_dandelion_density` 0) and
turns off GI/SSR/SSR/clouds/bloom — `app.c:423-439`.

`app_apply_benchmark_preset()` (`app.c:443-459`) — the same preset for
benchmarking (the level number comes from `FR_BENCH_PRESET`,
`main.c:595-596`), but with `r_render_scale 1` forced, dynamic resolution /
low-end profile / vsync / FPS limit / profiler turned off, and with `r_msaa`
overridden from the `FR_MSAA` environment variable.
