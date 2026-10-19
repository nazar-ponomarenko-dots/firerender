# `map.json` schema

Every map is described by a single JSON file `map/map.json` — inside the zip pak
`maps/<map>.zip` or in the dev folder `map/` (resource resolution order:
pak → disk → `base_dir` → `../`). Parser — `src/mapdata.c:753-1056`;
structures and limits — `src/mapdata.h`.

Parsing rules:

- **Every map is parsed from a clean state.** The built-in defaults
  snapshot is restored at the start (`mapdata_reset_defaults`,
  `mapdata.c:132-151`), so a missing section never inherits
  parameters from the previous map.
- **An unknown key name is silently ignored**; an unknown *value*
  of an enumerated field (light/water/road/material type) — the
  record is skipped with a message in the log.
- **Array `[x, y, z]`** is read element by element; a missing
  element = the field's default (`parse_vec3`, `mapdata.c:167-172`).
- Numbers are always `float` (JSON number), flags are `true`/`false`.

There are 24 root keys: `map`, `field`, `player`, `fog`, `terrain`, `veg`, `sky`,
`bloom`, `shadows`, `surfaces`, `paints[]`, `roads[]`, `info`, `ambient`,
`music`, `cvars`, `commands`, `lights[]`, `objects[]`, `collisions[]`,
`sounds[]`, `triggers[]`, `fauna[]`, `waters[]`.

---

## `map` — height map

| Field | Type | Default | Comment |
|---|---|---|---|
| `image` | string | `map.png` | Path to the height PNG, relative to the map folder. Field missing — `map.png` is used with a warning |

## `field` — vegetation field

| Field | Type | Default | Comment |
|---|---|---|---|
| `center` | `[x, z]` | world center | Both fields (`center` and `size`) are required, otherwise the section is not considered specified |
| `size` | `[w, d]` | — | Meters |
| `attached` | bool | `false` | The field follows the player (infinite field) |

## `player` — start

| Field | Type | Default |
|---|---|---|
| `pos` | `[x, y, z]` | — (the section applies only if `pos` is present) |
| `yaw` | float | `3.14159` (radians) |
| `pitch` | float | `-0.5` (radians) |
| `fov` | float | `60` |

## `fog` — volumetric voxel fog

| Field | Default | Meaning |
|---|---|---|
| `enabled` | 1 | Fog on |
| `strength` | 0.9 | Overall strength |
| `density_ground` / `ground_height` | 0.020 / 5.0 | Ground layer |
| `density_field` / `field_height` | 0.032 / 8.0 | Layer above the field |
| `density_water` / `water_height` | 0.030 / 4.0 | Layer above water |
| `color` | `[0.52, 0.56, 0.60]` | Base haze color |
| `color_sun` | `[0.85, 0.62, 0.45]` | Warm tint toward the sun |
| `glare` | 0.20 | Glow around the sun |
| `ray_strength` | 1.4 | Rays (god rays) |

The mode toggle is the cvar `r_fog_mode`
([cvars.md](cvars.md#fog)).

## `terrain` — terrain

| Field | Default | Meaning |
|---|---|---|
| `min_height` / `max_height` | -100 / 400 | Height range, m |
| `band_t0` / `band_t1` | 0.16 / 0.40 | Normalized PNG height band (0..1) that is mapped to the whole playable range |
| `band_h0` / `band_h1` | -50 / 200 | Meters at the ends of that band (`world.c:37-50`) |
| `blur` | 2 | Smoothing passes |
| `blur_radius` | 4 | Smoothing radius, texels |
| `erosion` | 40 | Erosion iterations |
| `erosion_slope` | 35.0 | Angle from which erosion starts working, ° |
| `ground_tex` | `img/ground.png` | Base ground texture |
| `tex_scale` | 6.4 | Meters per tile |
| `field_color` | `[0.68, 0.57, 0.33]` | Field color |
| `field_blend` | 0.85 | Blend of the field color into the ground |
| `fog_start` / `fog_end` | 0.75 / 1.60 | Fog distances (fractions of world size) |

## `veg` — vegetation

| Field | Default | Meaning |
|---|---|---|
| `grass_density` | 1.2 | items/m² |
| `wheat_density` | 140 | items/m² |
| `dandelion_density` | 0.05 | items/m² on top of grass |
| `grass_floor` / `wheat_floor` / `dandelion_floor` | 0.012 / 1.0 / 0.002 | Minimum density (items/m²) outside the field: below it the field is not clipped, so the map is not bare |
| `dyn_radius` | 48 | Radius of dynamic (around the player) chunks, m |
| `vis_radius` | 220 | Draw radius, m |
| `falloff` | 16 | Smoothness of the density falloff toward the edge |

These are the section defaults; user multipliers are the cvars `r_*_density`,
`r_veg_*`.

## `sky`, `bloom`, `shadows`

| Section | Field | Default | Meaning |
|---|---|---|---|
| `sky` | `tint` | `[1, 1, 1]` | Panorama tint |
| `sky` | `tex` | `img/skybox.png` | Panorama (equirect) |
| `bloom` | `threshold` | 0.75 | Bright-pass threshold |
| `bloom` | `intensity` | 1.0 | Strength of bright pixels |
| `bloom` | `strength` | 0.425 | Glow strength in the composite |
| `shadows` | `size` | 2048 | Shadow map side, texels |

## `surfaces` — splat map layers

The layer order is fixed: `dirt`, `sand`, `gravel`, `asphalt`
(`SPLAT_DIRT/SAND/GRAVEL/ASPHALT`). For each layer:

| Field | Default dirt / sand / gravel / asphalt |
|---|---|
| `tex` | `img/dirt.png` / `img/sand.png` / `img/gravel.png` / `img/asphalt.png` |
| `scale` | 3.0 / 2.5 / 2.0 / 4.0 (meters per tile) |
| `grass` | 0.25 / 0 / 0 / 0 (grass density multiplier; 0 = no grass) |

Section-wide fields: `resolution` (1.0 — splat map texels per meter) and
`slope_rock` (42.0° — above which rock shows through).

## `paints[]` — surface painting

Element:

| Field | Default | Meaning |
|---|---|---|
| `material` | required | `dirt`/`soil`, `sand`, `gravel`/`rock`, `asphalt`/`road`. Unknown name → record skipped |
| `shape` | auto | `circle` / `rect` / `path`. Without the field: ≥2 points → `path`, `size` > 0 → `rect`, otherwise `circle` |
| `pos` | `[0, 0]` | Center (x, z) |
| `radius` | `path`/`rect`: 0; `circle`: 8 if ≤ 0.01 | Circle radius |
| `size` | `[0, 0]` | Rectangle half-extents |
| `points` | — | `[[x, z], …]`, up to 64 points (`MAP_PAINT_MAX_POINTS`) |
| `width` | 4.0 | Stroke width of the `path` |
| `edge` | 2.0 | Edge softness |
| `strength` | 1.0 | Stroke opacity |

Maximum records — 64 (`MAP_MAX_PAINTS`).

## `roads[]` — roads

Element: `type` (or default `dirt`) and `points` (≥2 points, up to 64) required.

| Field | Meaning |
|---|---|
| `name` | Name for the log |
| `type` | `auto`/`asphalt` → asphalt with a gravel shoulder, `dirt`/`ground` → dirt road with ruts, `gravel` → gravel, `path`/`trail` → trail. Unknown type → skip |
| `material`, `shoulder_material` | Layer override (names as in `paints`) |
| `width`, `shoulder`, `edge` | Geometry: strip, shoulder, edge softness |
| `flatten` / `flatten_extra` | How much the road flattens the terrain and how much wider than the strip |
| `ruts` | Ruts |
| `veg_clear` | true — remove grass from the road |
| `points` | `[[x, z], …]` centerline |

Presets per type (`mapdata.c:586-607`):

| type | material / shoulder | width | shoulder | edge | flatten | flatten_extra | ruts |
|---|---|---|---|---|---|---|---|
| `auto` | asphalt / gravel | 7.0 | 1.6 | 0.5 | 1.00 | 6.0 | 0.0 |
| `gravel` | gravel / dirt | 5.0 | 1.2 | 0.9 | 0.85 | 4.0 | 0.25 |
| `dirt` | dirt / dirt | 4.0 | 1.0 | 1.2 | 0.65 | 3.0 | 0.6 |
| `path` | dirt / dirt | 1.3 | 0.4 | 0.6 | 0.30 | 1.5 | 0.15 |

Maximum roads — 32 (`MAP_MAX_ROADS`).

## `lights[]` — light sources

Up to 16 records (`MAP_MAX_LIGHTS`).

| Field | Default | Meaning |
|---|---|---|
| `type` | `directional` | `directional` / `point` / `spot`; unknown → skip |
| `name` | `""` | Name (for `attached` lights and debugging) |
| `pos` | `[0, 0, 0]` | Position (unused for a directional light) |
| `dir` | `[0, 0, 0]` | Direction |
| `color` | `[1, 1, 1]` | Color |
| `intensity` | 1.0 | Strength |
| `range` | 20.0 | Radius for point/spot, m |
| `angle` | 0.6 | Cone half-angle, radians |
| `attached` | false | Light follows the player (flashlight) |
| `shadow.enabled` | true | Shadows from this source |
| `shadow.bias` | 4.0 | Bias, lateral texels |
| `shadow.bias_veg` | 2.0 | Same for grass |

Point/spot lights are drawn through Forward+ clusters (`lights.c`, cvar `r_clustered`).

## `objects[]` — models and props

Up to 256 records (`MAP_MAX_OBJECTS`).

| Field | Default | Meaning |
|---|---|---|
| `name` | `""` | Object name; `collisions[]` attach by it |
| `model` | `""` | Path to a PNG (billboard) / `.obj` / `.glb` |
| `class` | `prop_static` | Entity class for the entity system |
| `pos` | `[0, 0, 0]` | Position, meters |
| `rot` | `[0, 0, 0]` | Parsed and stored in radians, but the object renderer does not apply the rotation (`objects.c:299-322`) |
| `scale` | `[1, 1, 1]` | Only an array of 3 numbers, otherwise identity; a value ≤ 0.01 is treated as 1 |
| `billboard` | false | Always faces the camera (otherwise — a cross of two quads) |
| `collision` | false | Impassable cylinder around the object base (player and solid bodies) |
| `radius` | 1.5 | Collision radius |
| `shader` | `default` | String `default`/`billboard`/`flat` **or** object `{ "vs": "shaders/x.vs", "fs": "shaders/x.fs" }`; `fs` is required for a custom one |
| `render` | — | Display block (see below) |
| `use` | — | Interaction via `+use` (see below) |

### `render = { … }`

Defaults when the block is present: tint `[1,1,1]`, alpha 1,
`blend` = alphatest, `cull` = none, `depth_write` = true, `wrap` = clamp,
`filter` = linear, tile `[1,1]`, offset/scroll `[0,0]`.

| Field | Meaning |
|---|---|
| `tint` | `[r, g, b]` |
| `alpha` | Transparency |
| `blend` | `alphatest` (default) / `alpha` / `additive` / `opaque` |
| `cull` | `none` / `back` / `front` |
| `depth_write` | bool |
| `texture.wrap` | `clamp` / `repeat` / `mirror` |
| `texture.filter` | `linear` / `nearest` / `mipmap` |
| `texture.tile` / `texture.offset` / `texture.scroll` | `[u, v]` — animating UV scrolls the texture |
| `size` | `{ "width": N, "height": M }` — fixed size in meters (default 5×5) |

### `use = { … }`

| Field | Meaning |
|---|---|
| `commands` | Array of console commands (up to 8, `MAP_ACTIONS_MAX`) |
| `message` | Message in the center of the screen |
| `sound` | Sound |
| `once` | Fire only once |

## `collisions[]` — AABB colliders

Up to 256 (`MAP_MAX_COLLISIONS`). Fields: `object` (owning object name),
`pos` `[x, y, z]`, `size` `[w, d]` (1×1 by default), `height` (4.0).

The player is blocked on XZ (`objects_collide`), while `height` sets the top of
the solid box collider (`phys_body`): without it a box would fall through the
structure. `height <= 0` → 2 m.

## `sounds[]` — positional sounds

Up to 16 (`MAP_MAX_SOUNDS`). An empty `sound` → the record is ignored.
Fields: `sound` (path), `pos`, `volume` (0.7), `radius` (25).

## `triggers[]` — triggers

Up to 64 (`MAP_MAX_TRIGGERS`).

| Field | Default | Meaning |
|---|---|---|
| `name` | `""` | Name |
| `pos` | `[0, 0, 0]` | Center |
| `size` | `[1, 1]` | Half-extents along X/Z |
| `once` | false | One-shot |
| `on_enter` | — | Array of commands (up to 8) |
| `message` | `""` | Message shown on screen |
| `sound` | `""` | Sound on enter |
| `teleport` | — | `[x, y, z]` — teleport the player |

## `fauna[]` — creatures

Up to 8 (`MAP_MAX_FAUNA`).

| Field | Default | Meaning |
|---|---|---|
| `type` | required | `fireflies` / `butterflies` / `birds`; unknown → skip |
| `area` | `[0, 0]` | Zone center (x, z) |
| `size` | `[50, 50]` | Zone size |
| `count` | 40 (fireflies) / 12 (the rest) | Individuals |
| `height` | 2.5 (40 for birds) | Flight height, m |

## `waters[]` — water

Up to 32 (`MAP_MAX_WATERS`). The type sets the look preset, then any field can
be overridden.

| Field | Meaning |
|---|---|
| `type` | `puddle` / `lake` / `river` / `sea`; unknown → skip |
| `name` | Name for the log |
| `pos` | `[x, z]` — center of the circle (puddle/lake) |
| `radius` | 3.0 puddle / 40.0 lake / otherwise 0 |
| `points` | River centerline, `[[x, z], …]` up to 16 points (`MAP_WATER_MAX_POINTS`); a river without ≥2 points is skipped |
| `width` | 8.0 — channel width |
| `level` | -60.0 — water level; for `sea` this is the global level |
| `depth` / `color` / `alpha` / `wave_amp` / `wave_speed` / `flow_speed` / `flow` / `foam` | Water look |

Look presets per type (`mapdata.c:391-425`):

| type | depth | color | alpha | wave_amp | wave_speed | flow_speed | foam |
|---|---|---|---|---|---|---|---|
| `puddle` | 0.6 | 0.36/0.38/0.34 | 0.82 | 0.02 | 1.5 | 0 | 0 |
| `lake` | 3.0 | 0.10/0.22/0.24 | 0.88 | 0.06 | 1.0 | 0 | 0.35 |
| `river` | 2.0 | 0.10/0.24/0.26 | 0.85 | 0.05 | 1.4 | 1.2 | 0.5 |
| `sea` | 6.0 | 0.04/0.16/0.22 | 0.94 | 0.18 | 0.8 | 0 | 0.6 |

If a river has no `flow`, the direction is computed from the chain of points.

## `info`, `ambient`, `music`

| Section | Field | Default | Meaning |
|---|---|---|---|
| `info` | `name` / `author` / `description` | `""` | Shown in the map selection menu |
| `ambient` | `sound` | `""` (the section does not apply) | Looping background sound |
| `ambient` | `volume` | 0.5 | |
| `music` | `sound` / `volume` | as above | Map track |

## `cvars` and `commands` — map-local settings

```json
"cvars":    { "r_gi": 0, "fov": 75 },
"commands": [ "weather rain 120", "matlist" ]
```

- `cvars` — an object of "name: number", up to 64 pairs (`MAP_MAX_CVARS`).
  Applied on world load.
- `commands` — an array of strings, up to 64 (`MAP_MAX_COMMANDS`, 256 characters
  per command). Executed after the map loads.
- An unregistered name in `cvars` creates the variable as a
  "map/lua variable" without `CVAR_ARCHIVE`; when a module later registers the
  same name, the slot is adopted (default/range/flags are updated, the value is
  kept).

## Minimal example

```json
{
  "info":  { "name": "Поле", "author": "—", "description": "—" },
  "map":   { "image": "map.png" },
  "field": { "center": [1000, 562], "size": [500, 300] },
  "player":{ "pos": [1000, 0, 562], "yaw": 3.14, "pitch": -0.5 },
  "lights":[ { "type": "directional", "dir": [0.25, 0.6, 3.0],
               "color": [1.0, 0.75, 0.65], "intensity": 1.0 } ],
  "objects":[ { "name": "tree1", "model": "img/tree.png",
                "pos": [880, 0, 380], "billboard": true, "collision": true } ],
  "waters":[ { "type": "lake", "pos": [400, 700], "radius": 60, "level": -20 } ]
}
```

## See also

- [cvars.md](cvars.md) — what can be tuned from the map
- [lua-api.md](lua-api.md) — the map script `lua/maps/<name>.lua`
