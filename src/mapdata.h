// ============================================================================
//  mapdata.h — map data from map/map.json: field, lights, objects
// ----------------------------------------------------------------------------
//  Everything map-related lives in the map/ folder and is pulled by the game
//  from there:
//    map/map.png   — height map (brightness = height, color = ground color);
//    map/map.json  — metadata: height map path, field rectangle,
//                    light list, object (model) list.
//  The JSON is parsed once at startup by the mini parser in json.h
//  (mapdata_load), after that scene modules read values through the functions
//  below. If a block is missing from the JSON — modules use their own
//  defaults.
// ============================================================================
#ifndef MAPDATA_H
#define MAPDATA_H
#include "material.h"

// Light types (for future point/spot lights)
typedef enum {
    LIGHT_DIRECTIONAL,   // directional (sun)
    LIGHT_POINT,         // point
    LIGHT_SPOT           // spotlight
} MapLightType;

typedef struct {
    MapLightType type;
    char name[64];       // light name (for lookup from code)
    float pos[3];        // position (point/spot)
    float dir[3];        // direction (directional/spot)
    float color[3];      // light color
    float intensity;     // light intensity
    float range;         // range (point/spot), m
    float angle;         // cone half-angle (spot), rad
    int shadow_on;       // does the light cast shadows? (shadow/enabled)
    float shadow_bias;   // terrain shadow bias (shadow/bias, in lateral texels)
    float shadow_bias_veg; // vegetation shadow bias (shadow/bias_veg)
    int attached;        // 1 = follows the player every frame (point light)
} MapLight;

// Object material — unified type (see material.h): blend/cull/wrap/filter/
// tint/alpha/UV animation. Parsed from the "render" block in map.json.
// --- Map collisions: collisions[] — arbitrary rectangles ---------------------
typedef struct {
    char object[64];     // object this collision is attached to ("tree1") or ""
    float pos[3];        // center (x, y, z) or offset from the object
    float size[2];       // [w, d] — rectangle size in meters
    float height;        // collision height, m (placeholder; block is on XZ now)
} MapCollision;

// --- Positional map sounds: sounds[] -----------------------------------------
typedef struct {
    float pos[3];        // source position, m
    char sound[256];     // path to wav
    float volume;        // volume 0..1
    float radius;        // audible radius, m
} MapSound3D;

// --- Commands/message/sound — shared by use and triggers ---------------------
#define MAP_ACTIONS_MAX  8     // max commands per action
#define MAP_CMD_LEN      256

// Object "use" block: what happens when E is pressed nearby
typedef struct {
    int set;
    char commands[MAP_ACTIONS_MAX][MAP_CMD_LEN];
    int cmd_n;
    char message[256];
    char sound[256];
    int once;            // fire only once
    int used;
} MapUse;

// Trigger: rectangular area, fires when the player enters it
typedef struct {
    char name[64];
    float pos[3];        // area center
    float size[2];       // [w, d] in meters
    int once;            // fire only once
    int entered;         // player was inside last frame (engine state)
    int fired;           // already fired (engine state)
    char commands[MAP_ACTIONS_MAX][MAP_CMD_LEN];
    int cmd_n;
    char message[256];
    char sound[256];
    float teleport[3];   // teleport on entry (if has_tp)
    int has_tp;
} MapTrigger;

// --- Objects: objects[] -------------------------------------------------------
// Built-in object shaders (MapObject.shader_kind)
typedef enum {
    OBJ_SHADER_DEFAULT,   // cross of two quads, full lighting
    OBJ_SHADER_BILLBOARD, // one quad always facing the camera, full lighting
    OBJ_SHADER_FLAT,      // unlit: texture * tint (icons, UI)
    OBJ_SHADER_CUSTOM     // custom shader (vs/fs from the map)
} MapObjectShader;

typedef struct {
    char name[64];       // object name
    char model[256];     // sprite texture path (PNG, relative to the map)
    char class[64];      // entity class (entity system, defaults to "prop_static")
    float pos[3];        // position
    float rot[3];        // rotation (radians)
    float scale[3];      // scale
    int billboard;       // 1 — quad always faces the camera (instead of a cross)
    int collision;       // 1 — object is solid (cylinder of radius around its base)
    float radius;        // collision radius, m (default 1.5)
    MapObjectShader shader_kind; // built-in shader
    char shader_vs[256]; // custom vertex shader (if custom)
    char shader_fs[256]; // custom fragment shader (if custom)
    Material render;         // material (render = { ... }, see material.h)
    MapUse use;              // interaction via E (use = { ... })
} MapObject;

// --- Voxel volumetric fog: fog = { ... } --------------------------------------
typedef struct {
    int enabled;              // 1 — fog enabled (key P)
    float strength;           // global density multiplier
    float density_ground;     // ground haze (per meter of path)
    float ground_height;      // ground haze height, m
    float density_field;      // extra fog over the wheat field
    float field_height;       // field fog height, m
    float density_water;      // fog over water bodies (per meter of path)
    float water_height;       // fog height above the water level, m
    float color[3];           // BASE fog color (everywhere)
    float color_sun[3];       // sun scattering color (warm, near the sun)
    float glare;              // halo right at the sun
    float ray_strength;       // god rays contrast (0 = none, 2 = very sharp)
} MapFog;

// --- Terrain: terrain = { ... } -----------------------------------------------
typedef struct {
    float min_height, max_height;   // height curve: world min/max
    float band_t0, band_t1;         // gameplay height band (by brightness)
    float band_h0, band_h1;         // heights of the band
    int blur;                       // height map smoothing (passes)
    int blur_radius;                // smoothing radius, meters
    int erosion;                    // steepness-clamp passes (0 = off)
    float erosion_slope;            // max steepness after erosion, degrees
    char ground_tex[256];           // ground texture (img/ground.png)
    float tex_scale;                // meters per texture tile
    float field_color[3];           // soil tint of the wheat field
    float field_blend;              // blend strength (0..1)
    float fog_start, fog_end;       // fog distance (world size multipliers)
} MapTerrain;

// --- Terrain surfaces: surfaces = { ... } -------------------------------------
//  Splat map layers (see src/splat.h). The base layer is grass/dirt from
//  terrain.ground_tex; 4 materials blend over it by mask.
//  Indices are fixed: 0 dirt, 1 sand, 2 gravel (path), 3 asphalt.
//  tex — texture, scale — meters per tile.
#define SPLAT_LAYERS 4
enum { SPLAT_DIRT = 0, SPLAT_SAND, SPLAT_GRAVEL, SPLAT_ASPHALT };

typedef struct {
    char tex[SPLAT_LAYERS][256];   // layer textures
    float scale[SPLAT_LAYERS];     // meters per texture tile
    float grass[SPLAT_LAYERS];     // grass density multiplier on the layer (0 = none)
    float resolution;              // meters per splat map texel
    float slope_rock;              // from which slope (deg) gravel shows through
    int set;                       // the surfaces block was present in JSON
} MapSurfaces;

// --- Surface painting: paints[] -----------------------------------------------
//  Material patches: circle (pos + radius), rectangle (pos + size) or
//  a strip along a centerline (points + width). edge — edge softness, m.
typedef enum { PAINT_CIRCLE = 0, PAINT_RECT, PAINT_PATH } MapPaintShape;

#define MAP_MAX_PAINTS       64
#define MAP_PAINT_MAX_POINTS 64

typedef struct {
    int material;        // SPLAT_DIRT..SPLAT_ASPHALT
    int shape;           // MapPaintShape
    float pos[2];        // center (circle/rect)
    float radius;        // radius (circle)
    float size[2];       // [w, d] (rect)
    float width;         // strip width (path)
    float points[MAP_PAINT_MAX_POINTS][2];
    int point_n;
    float edge;          // edge softness, m
    float strength;      // layer strength 0..1
} MapPaint;

// --- Roads: roads[] -----------------------------------------------------------
//  A road is a centerline points[] + width. type selects a preset:
//    "auto"   asphalt highway (gravel shoulder, strong flattening)
//    "dirt"   dirt road (brown earth, light flattening, ruts)
//    "gravel" gravel road
//    "path"   footpath (narrow, trampled earth, almost no flattening)
//  Any preset field can be overridden in JSON.
typedef enum { ROAD_AUTO = 0, ROAD_DIRT, ROAD_GRAVEL, ROAD_PATH } MapRoadType;

#define MAP_MAX_ROADS       32
#define MAP_ROAD_MAX_POINTS 64

typedef struct {
    char name[64];
    int type;                 // MapRoadType (preset)
    int material;             // SPLAT_* — road surface
    int shoulder_material;    // SPLAT_* — shoulder surface
    float width;              // carriageway width, m
    float shoulder;           // shoulder width on each side, m (0 = none)
    float edge;               // carriageway edge softness, m
    float flatten;            // 0..1 — how much the road flattens the terrain
    float flatten_extra;      // beyond the road, flattening tapers off, m
    float ruts;               // ruts 0..1 (dirt road): depression strength
    int veg_clear;            // 1 — vegetation does not grow on the carriageway
    float points[MAP_ROAD_MAX_POINTS][2];
    int point_n;
} MapRoad;

// --- Vegetation: veg = { ... } ------------------------------------------------
typedef struct {
    float grass_density, wheat_density;  // stems/m2 right next to the camera
    float grass_floor, wheat_floor;      // minimum across the whole map
    float dandelion_density;             // dandelions stems/m2 near the camera (rare,
                                         // added ON TOP OF the grass)
    float dandelion_floor;               // minimum dandelions across the map
    float dyn_radius, vis_radius;        // dynamics/visibility radii
    float falloff;                       // density falloff radius
} MapVeg;

// --- Sky: sky = { ... } -------------------------------------------------------
typedef struct {
    float tint[3];              // panorama tint
    char tex[256];              // sky texture (img/skybox.png)
} MapSky;

// --- Post-processing: bloom = { ... } -----------------------------------------
typedef struct {
    float threshold, intensity, strength;   // brightness threshold, strength, glow
} MapBloom;

// --- Shadows: shadows = { ... } -----------------------------------------------
typedef struct {
    int size;                   // shadow map size (1024/2048/4096)
} MapShadows;

// --- Map info: info = { name, author, description } ---------------------------
typedef struct {
    char name[128];             // title (for menus/logs)
    char author[128];           // author
    char description[256];      // description
} MapInfo;

// --- Map sound: ambient = { sound, volume }, music = { sound, volume } --------
typedef struct {
    char sound[256];            // path to wav (relative to root: audio/x.wav)
    float volume;               // volume 0..1
} MapSound;

// Loads map/map.json: height map path, field rectangle, lights and objects.
// Call BEFORE world_load (the map name is needed).
// 0 — success, -1 — file unreadable or JSON broken.
int mapdata_load(const char *path);

// Height map name relative to map/ (default "map.png")
const char *mapdata_image(void);

// Wheat field rectangle in meters: center[2] = (x, z), size[2] = (w, d).
// Returns 1 if the field is defined in JSON, otherwise 0 (use world center).
int mapdata_field(float center[2], float size[2]);

// Field attached to the player (field/attached): 1 — wheat grows across the
// whole map (infinite field), the frame follows the camera.
int mapdata_field_attached(void);

// Player start position (player/pos [x,y,z]) and view
// (player/yaw, player/pitch). Returns 1 if player is defined in JSON.
// y <= 0 means "take the height from the terrain at the spawn point".
// fov — field of view (player.fov, default 60).
int mapdata_player(float pos[3], float *yaw, float *pitch, float *fov);

// --- Light sources ------------------------------------------------------------
int mapdata_light_count(void);
const MapLight *mapdata_light(int i);

// --- Voxel volumetric fog (voxfog.c) ------------------------------------------
void mapdata_fog(MapFog *f);

// --- Terrain / vegetation / sky / bloom / shadows -----------------------------
void mapdata_terrain(MapTerrain *t);
void mapdata_veg(MapVeg *v);
void mapdata_sky(MapSky *s);
void mapdata_bloom(MapBloom *b);
void mapdata_shadows(MapShadows *s);

// --- Terrain surfaces (splat) / painting / roads ------------------------------
void mapdata_surfaces(MapSurfaces *s);
int mapdata_paint_count(void);
const MapPaint *mapdata_paint(int i);
int mapdata_road_count(void);
const MapRoad *mapdata_road(int i);

// --- Objects (models) ---------------------------------------------------------
int mapdata_object_count(void);
const MapObject *mapdata_object(int i);

// --- Collision rectangles: collisions[] ---------------------------------------
int mapdata_collision_count(void);
const MapCollision *mapdata_collision(int i);

// --- Positional sounds: sounds[] ----------------------------------------------
#define MAP_MAX_SOUNDS 16
int mapdata_sound_count(void);
const MapSound3D *mapdata_sound(int i);

// --- Triggers: triggers[] -------------------------------------------------------
#define MAP_MAX_TRIGGERS 64
int mapdata_trigger_count(void);
const MapTrigger *mapdata_trigger(int i);
int mapdata_trigger_fired(int i);
void mapdata_trigger_set_fired(int i, int v);
void mapdata_object_set_used(const char *name, int v);

// --- Water: waters[] ----------------------------------------------------------
// Four water body types. Each block sets the water level (level), the shape
// (circle pos/radius for a puddle/lake/sea, or a centerline points + width
// for a river) and the look (color, transparency, waves, flow, shore foam).
typedef enum {
    WATER_PUDDLE,   // puddle: small, nearly still, dries out fast
    WATER_LAKE,     // lake: calm, gentle wave
    WATER_RIVER,    // river: flow along the centerline (flow_speed)
    WATER_SEA       // sea: global water level, large waves
} MapWaterType;

#define MAP_MAX_WATERS        32   // cap on water bodies from JSON
#define MAP_WATER_MAX_POINTS  16   // max points in a river centerline

typedef struct {
    int type;              // MapWaterType
    char name[64];         // water body name (for lookup from code)
    float level;           // surface level (meters); for the sea — global
    float center[2];       // circle center [x, z] (puddle/lake/sea)
    float radius;          // circle radius, m (sea: 0 = whole world)
    float width;           // channel width, m (river)
    float points[MAP_WATER_MAX_POINTS][2]; // river centerline [x, z]
    int point_n;
    float depth;           // depth: how far the bottom shows through water, m
    float color[3];        // water color (deep tint)
    float alpha;           // base opacity 0..1
    float wave_amp;        // wave amplitude, m
    float wave_speed;      // wave speed (time multiplier)
    float flow[2];         // flow direction (river: derived from points on XZ)
    float flow_speed;      // flow speed, m/s (river pushes the player)
    float foam;            // shore foam strength 0..1
} MapWater;

// --- Fauna: fauna[] -----------------------------------------------------------
typedef enum {
    FAUNA_FIREFLIES,   // fireflies (glowing points, flickering)
    FAUNA_BUTTERFLIES, // butterflies (flapping wings, flying within an area)
    FAUNA_BIRDS        // birds (circling in the sky above an area)
} MapFaunaType;

typedef struct {
    int type;            // MapFaunaType
    float area[2];       // area center [x, z]
    float size[2];       // [w, d]
    int count;           // number of individuals
    float height;        // flight height, m
} MapFauna;

#define MAP_MAX_FAUNA 8
int mapdata_fauna_count(void);
const MapFauna *mapdata_fauna(int i);

// --- Map information ----------------------------------------------------------
void mapdata_info(MapInfo *i);

// --- Map sound: ambient/music. 1 — defined in JSON (play on loop). ------------
int mapdata_ambient(MapSound *a);
int mapdata_music(MapSound *m);

// --- Water: waters[] ----------------------------------------------------------
int mapdata_water_count(void);
const MapWater *mapdata_water(int i);

// --- Map commands: commands = [ "command", ... ] ------------------------------
// Executed through the console on map load (after the scene is built);
// can change cvars, binds, run scripts.
#define MAP_MAX_COMMANDS 64    // cap on commands per map
#define MAP_CMD_LEN      256    // max per command
int mapdata_command_count(void);
const char *mapdata_command(int i);

// --- Map cvars: cvars = { "name": value, ... } --------------------------------
// Applied on map load (after the scene, before commands).
#define MAP_MAX_CVARS 64
int mapdata_cvar_count(void);
const char *mapdata_cvar_name(int i);
float mapdata_cvar_value(int i);

// Frees the JSON arena. Call on shutdown.
void mapdata_free(void);

#endif
