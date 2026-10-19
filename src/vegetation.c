// ============================================================================
//  vegetation.c — vegetation: static layer (whole map) + dynamic layer
//  (around the camera)
// ----------------------------------------------------------------------------
//  Layer 1 — STATIC: the whole map is covered with grass/wheat at the minimum
//  density (s_grass_floor / s_wheat_floor). Generated ONCE at startup,
//  uploaded to a VBO and NEVER touched again — no flying around or "chunk
//  streaming" needed: vegetation exists on the entire map from the first second.
//  Layer 2 — DYNAMIC: around the camera (radius s_dyn_r) density grows along
//  an LOD curve: at 5 m from the player — like now (100/m2 wheat, 50/m2 grass),
//  further out it thins smoothly, converging to the floor at the radius edge.
//  When the camera moves, only chunks whose density changed noticeably are
//  recreated (VEG_REGEN_TOL) — that's a couple of chunks, no hitch and no
//  "streaming".
//
//  Plants are ORIENTED BY THE GROUND NORMAL: they tilt with the terrain
//  instead of standing strictly vertical. The normal is computed from the same
//  quantized heights (g_hq), so plants sit exactly on the surface.
//
//  Sprites — layers of a 256x178 2D array (0/1/2=grass.png/grass1.png/grass2.png,
//  3=dandelions.png, 4-6=wheat_lod0/1/2.png); the wheat LOD is chosen in the
//  shader by distance. View, rotation, size, tint, lean — instance attributes
//  (12 floats). The wheat field is set up by main.c via g_field_*. Drawing —
//  4 instanced calls (2 static + 2 dynamic), a single shader.
// ============================================================================
#include "vegetation.h"

#include "assets.h"
#include "bloom.h"
#include "cvars.h"
#include "gl_loader.h"
#include "linalg.h"
#include "mapdata.h"
#include "resources.h"
#include "shader.h"
#include "shadow.h"
#include "sky.h"
#include "splat.h"
#include "water.h"
#include "world.h"
#include "fx.h"
#include "jobs.h"

#include "stb_image.h"
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>   // SDL_Log

// Densities and radii — from map/map.json (veg = { ... }); the defaults
// match the previous constants.
static float s_grass_density = 1.2f;     // grass: items/m2 right next to the camera
static float s_wheat_density = 140.0f;  // wheat: items/m2 right next to the camera
static float s_grass_floor = 0.012f;     // grass minimum across the WHOLE map
static float s_wheat_floor = 1.0f;      // wheat minimum inside the field
static float s_dandelion_density = 0.05f;   // dandelions items/m2 at the camera (rare)
static float s_dandelion_floor   = 0.002f;  // dandelion minimum across the map
static float s_falloff_r = 16.0f;       // density falloff radius (LOD)
static float s_dyn_r = 48.0f;           // dynamic layer radius
static float s_vis_r = 220.0f;          // static layer radius, meters (at ground level)

// --- Sprite 2D-array layers (load order = index) ----------------------------
#define VEG_LAY_GRASS0  0   // img/grass.png    — grass, view 1 (tall)
#define VEG_LAY_GRASS1  1   // img/grass1.png   — grass, view 2 (medium)
#define VEG_LAY_GRASS2  2   // img/grass2.png   — grass, view 3 (short)
#define VEG_LAY_DANDE   3   // img/dandelions.png — dandelion
#define VEG_LAY_WHEAT0  4   // img/wheat_lod0.png
#define VEG_LAY_WHEAT1  5   // img/wheat_lod1.png
#define VEG_LAY_WHEAT2  6   // img/wheat_lod2.png
#define VEG_LAYERS      7
// "wheat" class boundary in the layer attribute (iAttr.w): everything >= 3.5
// is drawn with the 4..6 LOD ladder; smaller values (0/1/2 — grass,
// 3 — dandelion) are sampled as-is.
#define VEG_WHEAT_BASE  4.0f
#define VEG_WHEAT_MARK  3.5f

// --- Flexible vegetation kind table -----------------------------------------
// Each kind is one row: scale, tint, lean, appearance weight and the layer
// index into the sprite 2D-array. Grass (3 kinds) and the dandelion are drawn
// by a single instance stream; the kind is picked by weight. A new kind = one
// texture row in the array + one row here. The whole "grass" group shares one
// density (s_grass_density) — the weights split it among the kinds.
//
// The sink field was REMOVED: plants are placed with their BASE EXACTLY ON THE
// GROUND. The sprite's transparent areas are compensated by the shader
// (veg_base_v in wheat.vs), while sinking by 36..46% of the height hid the
// grass bottom under the terrain.
typedef struct {
    float s0, s1;      // min/max scale (m)
    float t0, t1;      // min/max tint
    float lean;        // max sideways lean (amplitude, rad)
    float weight;      // share of this kind within the whole grass group (0..~1)
    float layer;       // sprite 2D-array layer (see VEG_LAY_*)
} VegKind;
#define VEG_KINDS 4
#define VEG_KIND_DANDELION 3   // dandelion index in the s_kinds table
static const VegKind s_kinds[VEG_KINDS] = {
    //             s0    s1    t0    t1   lean  weight layer
    { 0.32f, 0.62f, 0.78f, 1.06f, 0.26f, 0.30f, VEG_LAY_GRASS0 },  // tall dense
    { 0.22f, 0.46f, 0.85f, 1.12f, 0.25f, 0.28f, VEG_LAY_GRASS1 },  // medium
    { 0.18f, 0.38f, 0.72f, 0.98f, 0.25f, 0.26f, VEG_LAY_GRASS2 },  // short sparse
    { 0.30f, 0.55f, 0.95f, 1.15f, 0.15f, 0.0f,  VEG_LAY_DANDE  },  // dandelion (outside the weights, rare by density)
};
// Sum of GRASS weights (the dandelion is picked separately via p_dande, its
// weight is excluded when normalizing the kind pick).
static float s_grass_total = 0.0f;
static float veg_grass_total(void)
{
    if (s_grass_total > 0.0f) return s_grass_total;
    float t = 0.0f;
    for (int i = 0; i < VEG_KINDS; i++)
        if (i != VEG_KIND_DANDELION) t += s_kinds[i].weight;
    s_grass_total = t > 0.0f ? t : 1.0f;
    return s_grass_total;
}
// Pick a GRASS kind by weight (the dandelion does not participate — it is
// rare and picked by its own probability). pick ∈ [0..grass_total) → kind index.
static int veg_pick_kind(unsigned int *rng)
{
    float total = veg_grass_total();
    float pick = frnd(rng) * total;
    float acc = 0.0f;
    for (int i = 0; i < VEG_KINDS; i++) {
        if (i == VEG_KIND_DANDELION) continue;   // dandelion — handled separately
        acc += s_kinds[i].weight;
        if (pick < acc) return i;
    }
    return 0;
}
#define VEG_CHUNK       16.0f     // chunk side in meters
#define VEG_REGEN_TOL    3.0f     // recreate a chunk if density changed by more
#define VEG_REGEN_REL    0.25f    // ...or by this FRACTION of density (wheat 140 vs grass 1.2)
#define VEG_REGEN_MIN    0.30f    // lower bound of the relative tolerance (items/m²)
// Visibility radius of the STATIC layer around the camera: beyond it sprites
// are smaller than a pixel and invisible; their "look" is carried by the
// terrain texture (stamped grass) and the field tint. The radius grows with
// camera altitude (a mountaintop view).
#define VEG_VIS_H       50.0f     // above this altitude the radius grows
#define VEG_VIS_F        0.6f     // ...by this many meters per meter of altitude
// The slope limit for vegetation was removed: grass/wheat grow on any tilted
// surfaces (oriented by the ground normal), density is compensated by surface
// area — no "holes" on slopes.
#define VEG_SLOPE_E     0.4f      // slope-check step, m
#define VEG_CAND_CAP  51200       // max candidates per chunk (wheat 140/m2)

// Dynamic chunk cache (around the camera)
typedef struct {
    float *g, *w;                // instances: 12 float (see below)
    int gn, wn;
    float dg_d, dw_d;            // density the chunk was generated with
} VegChunk;

static VegChunk *g_veg_chunks;   // [g_map_w * g_map_d]
static int g_map_w, g_map_d;
static float *g_static_g, *g_static_w;   // static layer (whole map)
static int g_static_gc, g_static_wc;
static float *g_dyn_g, *g_dyn_w;         // dynamic layer (around the camera)
static int g_dyn_gc, g_dyn_wc, g_dyn_cap;
// Shared generation scratch buffers: previously each dense chunk temporarily
// allocated two VEG_CAND_CAP arrays (~4.7 MiB in total) right inside the frame.
static float *g_gen_scratch_g, *g_gen_scratch_w;
static float *g_shadow_scratch_g, *g_shadow_scratch_w;
static int g_shadow_scratch_gcap, g_shadow_scratch_wcap;
// INT_MAX: the first veg_update MUST run (otherwise, if the camera starts in
// chunk (0,0), the dynamic layer would never be generated)
static int g_chunk_cam[2] = { INT_MAX, INT_MAX };
// Regen budget (r_veg_regen_budget): at most N chunks per veg_update call,
// the rest — in following frames. On 2-core CPUs a batch of recreations in one
// frame = a freeze; the budget drip removes the jumps. >0 = there is a tail.
static int g_regen_pending = 0;
// Predictive preloading: chunks thicken up not when the camera CROSSES their
// border (then grass/field "pop up right under the nose") but in advance —
// density is computed from the distance to the path SEGMENT [camera .. lead
// point ahead along the movement]. While standing still (offset=0) — behavior
// as before. Updated in veg_update from the camera velocity.
static float g_look_bx = 0.0f, g_look_bz = 0.0f;  // lead point (world)
static float g_prev_cam[3] = { 0, 0, 0 };
static int   g_prev_cam_ok = 0;
// Distance from point (px,pz) to the segment [(ax,az)..(bx,bz)] in the XZ plane.
// Grass along the movement path thickens in advance, at the feet (a) and to
// the sides — as is.
static float veg_path_dist(float px, float pz, float ax, float az,
                           float bx, float bz)
{
    float vx = bx - ax, vz = bz - az;
    float len2 = vx * vx + vz * vz;
    if (len2 < 1e-4f) {   // standing still: plain distance to the camera
        float dx = px - ax, dz = pz - az;
        return sqrtf(dx * dx + dz * dz);
    }
    float t = ((px - ax) * vx + (pz - az) * vz) / len2;
    if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
    float cx = ax + vx * t, cz = az + vz * t;
    float dx = px - cx, dz = pz - cz;
    return sqrtf(dx * dx + dz * dz);
}

// The vegetation VAO and sprite array — needed by the shadow pass (shadow.c)
static GLuint g_veg_vao[4];
static GLuint g_veg_tex = 0;
static GLuint g_veg_mesh_vbo, g_veg_mesh_ibo, g_veg_vb[4];
// Pre-thinned vegetation layer for shadows: static — every 2nd instance;
// dynamic — by the shader's uThin rule ((id*2654435761)%100<50), so the set in
// the shadow map is EXACTLY what shader-side thinning gives, but the vertex
// stage processes half as many instances.
static GLuint g_veg_shadow_vao[4], g_veg_shadow_vbo[4];
static int g_veg_shadow_cnt[4];

// --- GPU-driven (compute-cull + indirect draw, GL 4.3+) ---------------------
//  Context-independent buffers: all instances live in the Src SSBO, the
//  compute pass culls by frustum + screen footprint and COMPACTLY writes the
//  survivors into the Cull SSBO, accumulating their count into the instanceCount
//  of the Indirect command. Drawing — glDrawElementsIndirect: the vertex count
//  is bounded by visible pixels, not by density ("a billion" ≈ "one blade" in
//  cost). Fallback — the instanced path below (when g_veg_gpudriven == 0 or
//  cvar r_veg_gpudriven == 0).
static int g_veg_gpudriven = 0;       // path active (capability && cvar)
static int g_veg_src_cnt[4];          // instance count in Src per layer
static GLuint g_veg_src_ssbo[4];      // all instances (read in compute)
static GLuint g_veg_cull_ssbo[4];     // compact survivors (-> instance VAO)
static GLuint g_veg_indirect[4];      // DrawElementsIndirectCommand
static GLuint g_veg_vao_cull[4];      // VAO with instances from the Cull SSBO
static GLuint s_cull_prog = 0;        // culling compute program
static GLint u_cull_vp, u_cull_viewh, u_cull_tan, u_cull_minpix, u_cull_src, u_cull_off;
static GLint u_cull_planes, u_cull_campos, u_cull_capacity;

// GPU-driven prototypes (defined below, used higher up in the file).
static void veg_gpu_init(VegParams *p);
static void veg_gpu_src_upload(int q, const float *src, int count);
static void veg_gpu_draw(VegParams *p, const RendererContext *rc);

// Vegetation shadow shader (depth with CPU-side thinning) —
// used to live in shadow.c, now belongs to vegetation.
static GLuint s_shadowveg_prog = 0;
static GLint u_veg_viewproj, u_veg_veg, u_veg_thin, u_veg_time;
static GLint u_veg_campos, u_veg_dyn, u_veg_walk, u_veg_windboost;
static GLint u_veg_grassfall = -1, u_veg_grassfloor = -1;
static GLint u_cull_lodshift = -1;
static VegParams *s_veg = NULL;

// Programs are created OUTSIDE (make_node / reshade: the refcount is balanced
// there). Here — only uniforms for the CURRENT ids (p->prog and s_shadowveg_prog).
static void veg_prog_init(VegParams *p)
{
    GLuint prog = p->prog;
    u_veg_viewproj = glGetUniformLocation(s_shadowveg_prog, "uViewProj");
    u_veg_veg      = glGetUniformLocation(s_shadowveg_prog, "uVeg");
    u_veg_thin     = glGetUniformLocation(s_shadowveg_prog, "uThin");
    u_veg_time     = glGetUniformLocation(s_shadowveg_prog, "uTime");
    u_veg_campos   = glGetUniformLocation(s_shadowveg_prog, "uCamPos");
    u_veg_dyn      = glGetUniformLocation(s_shadowveg_prog, "uDyn");
    u_veg_walk     = glGetUniformLocation(s_shadowveg_prog, "uWalk");
    u_veg_windboost = glGetUniformLocation(s_shadowveg_prog, "uWindBoost");
    u_veg_grassfall = glGetUniformLocation(s_shadowveg_prog, "uGrassFall");
    u_veg_grassfloor = glGetUniformLocation(s_shadowveg_prog, "uGrassFloorFrac");

    scene_uniforms_init(&p->su, prog);
    p->u_dyn      = glGetUniformLocation(prog, "uDyn");
    p->u_time     = glGetUniformLocation(prog, "uTime");
    p->u_walk     = glGetUniformLocation(prog, "uWalk");
    p->u_windboost = glGetUniformLocation(prog, "uWindBoost");
    p->u_lodshift = glGetUniformLocation(prog, "uLodShift");
    p->u_depthonly = glGetUniformLocation(prog, "uDepthOnly");
    p->u_simple   = glGetUniformLocation(prog, "uSimple");
    p->u_grassfall = glGetUniformLocation(prog, "uGrassFall");
    p->u_grassfloor = glGetUniformLocation(prog, "uGrassFloorFrac");

    // Static vegetation uniforms
    glUseProgram(prog);
    glUniform1i(glGetUniformLocation(prog, "uVeg"), 0);
    // Cascades on units 3-6 (uShadowMap%d and raw depth uShadowDepth%d)
    for (int c = 0; c < SHADOW_CASCADES; c++) {
        if (p->su.u_shadowmap[c] >= 0)
            glUniform1i(p->su.u_shadowmap[c], 3 + c);
        if (p->su.u_shadowdepth[c] >= 0)
            glUniform1i(p->su.u_shadowdepth[c], 3 + c);
    }
    // Environment/fog is sent by scene_uniforms_bind every frame from rc
    glUseProgram(0);
}

// Hot-reload: recreate the programs and uniforms of the current node
// (replace balances the refcount; 0 — the pair failed to build, the old id stays).
static void veg_reshade(void)
{
    if (!s_veg) return;
    GLuint prog = resources_program_replace(s_veg->prog,
                                            "shaders/wheat.vs", "shaders/wheat.fs");
    if (prog) s_veg->prog = prog;
    else SDL_Log("veg: reshade failed, keeping old program");
    GLuint sp = resources_program_replace(s_shadowveg_prog,
                                          "shaders/shadowveg.vs",
                                          "shaders/shadowveg.fs");
    if (sp) s_shadowveg_prog = sp;
    else SDL_Log("veg: shadow program reshade failed, keeping old");
    veg_prog_init(s_veg);
}
static float g_veg_time;         // simulation time (wind)
static int g_veg_visible = 1;    // test toggle for drawing (perf harness)
static int g_draw_static_main = 1, g_draw_dyn_main = 1;   // FR_TEST: e/f

void veg_set_visible_test(int v) { g_veg_visible = v; }
void veg_set_static_main(int v) { g_draw_static_main = v; }
void veg_set_dyn_main(int v) { g_draw_dyn_main = v; }

// --- Frustum culling of super-chunks ----------------------------------------
// 6 sawtooth-cone planes from viewproj (Gribb-Hartmann). Matrices are
// column-major (m[c*4+r]): row r = (m[r], m[4+r], m[8+r], m[12+r]).
// Planes: left=row3+row0, right=row3-row0, bottom=row3+row1,
// top=row3-row1, near=row3+row2, far=row3-row2.
static float g_fr[6][4];

static void frustum_extract(const float vp[16])
{
    float r0[4] = { vp[0], vp[4], vp[8],  vp[12] };
    float r1[4] = { vp[1], vp[5], vp[9],  vp[13] };
    float r2[4] = { vp[2], vp[6], vp[10], vp[14] };
    float r3[4] = { vp[3], vp[7], vp[11], vp[15] };
    for (int i = 0; i < 6; i++) {
        const float *a = r3, *b;
        float s;
        switch (i) {
            case 0: b = r0; s =  1.0f; break;
            case 1: b = r0; s = -1.0f; break;
            case 2: b = r1; s =  1.0f; break;
            case 3: b = r1; s = -1.0f; break;
            case 4: b = r2; s =  1.0f; break;
            default: b = r2; s = -1.0f; break;
        }
        float n[4] = { a[0] + s*b[0], a[1] + s*b[1], a[2] + s*b[2],
                       a[3] + s*b[3] };
        float len = sqrtf(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]);
        if (len > 1e-8f)
            for (int k = 0; k < 4; k++) g_fr[i][k] = n[k] / len;
        else
            g_fr[i][0] = g_fr[i][1] = g_fr[i][2] = 0, g_fr[i][3] = 1.0f;
    }
}

// Is the box (XZ extent, height = the whole terrain) touched by the cone at all?
static int frustum_box(const WorldState *w, float x0, float z0, float x1, float z1)
{
    for (int i = 0; i < 6; i++) {
        const float *pl = g_fr[i];
        // box vertex "most towards" the plane
        float p[3] = {
            pl[0] > 0.0f ? x1 : x0,
            pl[1] > 0.0f ? w->max_height + 10.0f : w->min_height - 10.0f,
            pl[2] > 0.0f ? z1 : z0
        };
        if (pl[0]*p[0] + pl[1]*p[1] + pl[2]*p[2] + pl[3] < 0.0f)
            return 0;
    }
    return 1;
}

// Density at distance r from the camera: at the camera — d0 (as now), further
// out it falls smoothly along a smoothstep to floor at the dynamic layer edge
// (s_dyn_r). The static layer uses that same floor density — so there is no
// jump at the dynamic/static seam, the transition from the player to the "end"
// has no pops.
static float veg_density(float r, float d0, float floor)
{
    // r_veg_falloff_mult compresses the falloff curve: at 0.5 density reaches
    // floor twice as close to the camera (potato mode: the middle ring is
    // sparser, at the feet nothing changes)
    float t = r / (s_dyn_r * cvar_get("r_veg_falloff_mult"));
    if (t >= 1.0f) return floor;
    t = t * t * (3.0f - 2.0f * t);   // smoothstep: slow start, smooth end
    return d0 + (floor - d0) * t;
}

// Generates the plants of chunk (cx, cy). Density — by distance to the camera
// (camx, camz); if static_run == true, only the floor is used (whole map).
//
// The grass GROUP = 3 grass kinds + dandelion. Group density = grass density
// + the dandelions' OWN (small) density — they are added ON TOP of the grass
// and do not dilute it. Each group candidate becomes a dandelion with
// probability d_dande / (d_grass + d_dande) — rare spots. The remaining
// candidates are split among the 3 grass kinds by the s_kinds weights.
//
// An instance is 12 floats: x y z, nx nz (ground normal), yaw, scale, tint,
// layer, lean, dh/dx, dh/dz (terrain slope — for seating on the relief without
// sampling the height texture in the shader). The seed depends on the chunk
// coordinates — rerunning gives the same grass.
static void veg_gen_chunk(const WorldState *w, int cx, int cy, float camx, float camz,
                          bool static_run, float *gdst, float *wdst,
                          int *gn, int *wn, float *dgo, float *dwo)
{
    float x0 = (float)cx * VEG_CHUNK, z0 = (float)cy * VEG_CHUNK;
    // Distance to the movement path (camera→lead point), not just to the camera —
    // chunks ahead thicken up in advance, before arrival.
    float r = static_run ? 1e9f
              : veg_path_dist(x0 + 8.0f, z0 + 8.0f, camx, camz,
                              g_look_bx, g_look_bz);
    // Densities at the camera — from cvars (Graphics menu). Dandelions have
    // their own small density, added ON TOP of the grass (not eating its share).
    // GRASS: in the dynamic layer the density is FULL across the whole radius —
    // the falloff toward the edge is done by the shader (wheat.vs, smooth
    // hash-keep by distance). So a chunk is not rebuilt when the camera
    // approaches (only on load/state change), and no "streaming steps" are
    // visible. Static (static_run) — as before (floor).
    float dg_g = static_run
        ? veg_density(r, cvar_get("r_grass_density"), s_grass_floor)
        : cvar_get("r_grass_density");
    float dg_d = static_run
        ? veg_density(r, cvar_get("r_dandelion_density"), s_dandelion_floor)
        : cvar_get("r_dandelion_density");
    float dw = veg_density(r, cvar_get("r_wheat_density"), s_wheat_floor);
    float dg = dg_g + dg_d;                      // the whole grass group
    if (dwo) *dwo = dw;
    if (dgo) *dgo = dg;
    float wmax = dw > 2.0f * dg ? dw : 2.0f * dg;
    int cands = (int)(VEG_CHUNK * VEG_CHUNK * wmax) + 1;
    if (cands > VEG_CAND_CAP) cands = VEG_CAND_CAP;
    unsigned int rng = 0x9e3779b9u ^ (unsigned)((unsigned)cx * 73856093u
                                                ^ (unsigned)cy * 19349663u);
    // Static and dynamic use DIFFERENT seeds: otherwise their plant positions
    // coincide (the dynamic layer draws over the static one at the same spots —
    // "double stacking").
    if (static_run) rng ^= 0x5bd1e995u;
    // Dandelion probability within the grass group (rare spots).
    float p_dande = dg > 1e-4f ? dg_d / dg : 0.0f;
    if (p_dande > 1.0f) p_dande = 1.0f;
    int gc = 0, wc = 0;
    float e = VEG_SLOPE_E;
    for (int i = 0; i < cands; i++) {
        float x = x0 + frnd(&rng) * VEG_CHUNK;
        float z = z0 + frnd(&rng) * VEG_CHUNK;
        if (x < 1.5f || z < 1.5f || x > w->world_w - 1.5f || z > w->world_d - 1.5f)
            continue;                            // not past the world edge
        int wheat = (x >= w->field_x0 && x <= w->field_x0 + w->field_w &&
                     z >= w->field_z0 && z <= w->field_z0 + w->field_d);
        float gy = sample_height(w, x, z);
        if (gy < w->min_height + 4.0f) continue;    // extreme lowlands — same
        // No vegetation underwater: the sea/river/puddle bottom stays clean
        if (water_surface_at(w, x, z) > gy) continue;
        // OCCUPANCY AND COVERAGE (splat.c): grass does not grow inside static
        // 3D objects/models or on roads; none on sand/gravel/asphalt, sparse on
        // soil (surfaces.grass). One multiplier per candidate.
        float cover = splat_grass_factor(x, z);
        if (cover <= 0.001f) continue;
        // terrain slope and normal (signed differences — like in a normal map)
        float sdx = sample_height(w, x + e, z)
                  - sample_height(w, x - e, z);
        float sdz = sample_height(w, x, z + e)
                  - sample_height(w, x, z - e);
        float slope = sqrtf(sdx*sdx + sdz*sdz) / (2.0f * e);
        // Density is computed per SURFACE AREA, not horizontally: on a slope a
        // horizontal square meter is smaller, and without the correction
        // grass/wheat thin out — leaving "holes". Correction = 1/cos = sqrt(1+s²).
        float surf = sqrtf(1.0f + slope * slope);
        float want = (wheat ? dw : dg) * surf * cover;
        if (frnd(&rng) >= want / wmax) continue; // thinning down to the density
        float nx = -sdx, nz = -sdz;              // normal: (-dh/dx, 2e, -dh/dz)
        float nl = sqrtf(nx*nx + nz*nz + 4.0f * e * e);
        nx /= nl; nz /= nl;

        float *dst = wheat ? wdst : gdst;
        dst[0] = x;
        // BASE EXACTLY ON THE GROUND: neither a hop nor sinking. The sprite
        // bottom matches the terrain surface (the image's transparent areas are
        // removed by the shader, see veg_base_v in wheat.vs).
        dst[1] = gy;
        dst[2] = z;
        dst[3] = nx;
        dst[4] = nz;
        dst[5] = frnd(&rng) * 6.2831853f;        // random rotation around Y
        if (wheat) {
            dst[6] = 0.18f + 0.14f * frnd(&rng); // ear: narrow, 0.18..0.32 m
            dst[7] = 0.90f + 0.20f * frnd(&rng); // tint
            dst[8] = VEG_WHEAT_BASE;             // kind: wheat (layer 4)
            dst[9] = 0.0f;                       // no lean
        } else {
            // Grass group: a dandelion with probability p_dande (rare, does not
            // dilute the grass), otherwise one of the 3 kinds by weights.
            int kind = frnd(&rng) < p_dande ? VEG_KIND_DANDELION
                                            : veg_pick_kind(&rng);
            const VegKind *k = &s_kinds[kind];
            dst[6] = k->s0 + (k->s1 - k->s0) * frnd(&rng);   // scale (m)
            dst[7] = k->t0 + (k->t1 - k->t0) * frnd(&rng);   // tint
            dst[8] = k->layer;                    // sprite layer (view)
            dst[9] = (frnd(&rng) - 0.5f) * k->lean;  // sideways lean, like in real life
        }
        dst[10] = sdx / (2.0f * e);              // slope: dh/dx, dh/dz
        dst[11] = sdz / (2.0f * e);
        if (wheat) { wdst += 12; wc++; }
        else       { gdst += 12; gc++; }
    }
    if (gn) *gn = gc;
    if (wn) *wn = wc;
}

// Configures the vegetation VAO with the shared mesh and the instance buffer
// (see the definition below — used by the static layer and the shadow buffers).
static void veg_setup_vao(GLuint vao, GLuint inst_vbo);

static void veg_instance_offset(GLuint inst_vbo, int first)
{
    const GLsizei stride = 12 * (GLsizei)sizeof(float);
    const uintptr_t base = (uintptr_t)first * (uintptr_t)stride;
    glBindBuffer(GL_ARRAY_BUFFER, inst_vbo);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, stride, (const void *)(base));
    glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, stride,
                          (const void *)(base + 3 * sizeof(float)));
    glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, stride,
                          (const void *)(base + 5 * sizeof(float)));
    glVertexAttribPointer(5, 1, GL_FLOAT, GL_FALSE, stride,
                          (const void *)(base + 9 * sizeof(float)));
    glVertexAttribPointer(6, 2, GL_FLOAT, GL_FALSE, stride,
                          (const void *)(base + 10 * sizeof(float)));
}

// --- Static super-chunks (64 m = 4x4 chunks of 16 m) ------------------------
// The static layer is drawn NOT as a whole, but only by super-chunks within the
// camera's visibility radius (glDrawElementsInstancedBaseInstance). Beyond it
// sprites are smaller than a pixel — the "look" is carried by the terrain
// texture. Saving: on the ground ~10% of the static layer is drawn instead of 100%.
#define VEG_SUPER 4          // chunks per super-chunk side
static int g_sc_gw, g_sc_gh, g_sc_n;   // grid and super-chunk count
static int *g_sc_cnt_g, *g_sc_cnt_w;   // plants per super-chunk
static int *g_sc_off_g, *g_sc_off_w;   // prefix sums (instance bases)
static float *g_sc_cx, *g_sc_cz;       // super-chunk centers (meters)
static int g_has_base_instance;        // is ARB_base_instance available

// Parallel static-layer generation (jobs): super-chunks are independent, so
// both passes — counting and filling — are split by super-chunk index across
// all cores. The instance order inside a super-chunk (dy,dx) and the bases from
// the prefix sums are the same as in the sequential version => the buffer is
// BYTE-FOR-BYTE identical. Each fn call keeps its own scratch (its range is
// processed sequentially inside fn).
typedef struct { const WorldState *world; } VegGenCtx;

static void veg_static_count_range(int s0, int s1, void *ud)
{
    VegGenCtx *ctx = (VegGenCtx *)ud;
    float *sg = malloc((size_t)VEG_CAND_CAP * 12 * sizeof(float));
    float *sw = malloc((size_t)VEG_CAND_CAP * 12 * sizeof(float));
    if (!sg || !sw) { free(sg); free(sw); return; }   // counters will stay 0
    for (int s = s0; s < s1; s++) {
        int sx = s % g_sc_gw, sy = s / g_sc_gw;
        g_sc_cx[s] = (float)(sx * VEG_SUPER) * VEG_CHUNK + VEG_SUPER * VEG_CHUNK * 0.5f;
        g_sc_cz[s] = (float)(sy * VEG_SUPER) * VEG_CHUNK + VEG_SUPER * VEG_CHUNK * 0.5f;
        int g = 0, w = 0;
        for (int dy = 0; dy < VEG_SUPER; dy++)
            for (int dx = 0; dx < VEG_SUPER; dx++) {
                int i = sx * VEG_SUPER + dx, j = sy * VEG_SUPER + dy;
                if (i >= g_map_w || j >= g_map_d) continue;
                int gn = 0, wn = 0;
                veg_gen_chunk(ctx->world, i, j, 0, 0, true, sg, sw, &gn, &wn, NULL, NULL);
                g += gn; w += wn;
            }
        g_sc_cnt_g[s] = g;
        g_sc_cnt_w[s] = w;
    }
    free(sg); free(sw);
}

static void veg_static_fill_range(int s0, int s1, void *ud)
{
    VegGenCtx *ctx = (VegGenCtx *)ud;
    float *sg = malloc((size_t)VEG_CAND_CAP * 12 * sizeof(float));
    float *sw = malloc((size_t)VEG_CAND_CAP * 12 * sizeof(float));
    if (!sg || !sw) { free(sg); free(sw); return; }
    for (int s = s0; s < s1; s++) {
        int sx = s % g_sc_gw, sy = s / g_sc_gw;
        int pos_g = g_sc_off_g[s], pos_w = g_sc_off_w[s];   // bases from prefix sums
        for (int dy = 0; dy < VEG_SUPER; dy++)
            for (int dx = 0; dx < VEG_SUPER; dx++) {
                int i = sx * VEG_SUPER + dx, j = sy * VEG_SUPER + dy;
                if (i >= g_map_w || j >= g_map_d) continue;
                int gn = 0, wn = 0;
                veg_gen_chunk(ctx->world, i, j, 0, 0, true, sg, sw, &gn, &wn, NULL, NULL);
                if (gn) memcpy(g_static_g + (size_t)pos_g * 12, sg,
                               (size_t)gn * 12 * sizeof(float));
                if (wn) memcpy(g_static_w + (size_t)pos_w * 12, sw,
                               (size_t)wn * 12 * sizeof(float));
                pos_g += gn;
                pos_w += wn;
            }
    }
    free(sg); free(sw);
}


// Static layer: the whole map, minimal density. Once at startup.
// Instances are laid out PER SUPER-CHUNK (64 m) so the renderer can draw only
// the visible super-chunks (see veg_node_render).
static int veg_static_init(VegParams *p)
{
    g_sc_gw = (g_map_w + VEG_SUPER - 1) / VEG_SUPER;
    g_sc_gh = (g_map_d + VEG_SUPER - 1) / VEG_SUPER;
    g_sc_n = g_sc_gw * g_sc_gh;
    g_sc_cnt_g = calloc((size_t)g_sc_n, sizeof(int));
    g_sc_cnt_w = calloc((size_t)g_sc_n, sizeof(int));
    g_sc_off_g = malloc((size_t)(g_sc_n + 1) * sizeof(int));
    g_sc_off_w = malloc((size_t)(g_sc_n + 1) * sizeof(int));
    g_sc_cx = malloc((size_t)g_sc_n * sizeof(float));
    g_sc_cz = malloc((size_t)g_sc_n * sizeof(float));
    g_gen_scratch_g = malloc((size_t)VEG_CAND_CAP * 12 * sizeof(float));
    g_gen_scratch_w = malloc((size_t)VEG_CAND_CAP * 12 * sizeof(float));
    if (!g_sc_cnt_g || !g_sc_cnt_w || !g_sc_off_g || !g_sc_off_w ||
        !g_sc_cx || !g_sc_cz || !g_gen_scratch_g || !g_gen_scratch_w)
        return 0;

    // pass 1: count plants per super-chunk (in parallel)
    VegGenCtx gctx = { p->world };
    jobs_parallel_for(0, g_sc_n, veg_static_count_range, &gctx);
    // prefix sums: instance bases for glDrawElementsInstancedBaseInstance
    g_sc_off_g[0] = g_sc_off_w[0] = 0;
    for (int s = 0; s < g_sc_n; s++) {
        g_sc_off_g[s + 1] = g_sc_off_g[s] + g_sc_cnt_g[s];
        g_sc_off_w[s + 1] = g_sc_off_w[s] + g_sc_cnt_w[s];
    }
    g_static_gc = g_sc_off_g[g_sc_n];
    g_static_wc = g_sc_off_w[g_sc_n];
    g_static_g = g_static_gc ? malloc((size_t)g_static_gc * 12 * sizeof(float)) : NULL;
    g_static_w = g_static_wc ? malloc((size_t)g_static_wc * 12 * sizeof(float)) : NULL;
    if ((g_static_gc && !g_static_g) || (g_static_wc && !g_static_w)) return 0;

    // pass 2: fill the buffers (generation is deterministic — same counters).
    // Each super-chunk writes into ITS OWN range [off, off+cnt) => no races.
    jobs_parallel_for(0, g_sc_n, veg_static_fill_range, &gctx);
    glBindBuffer(GL_ARRAY_BUFFER, p->vbo_sg);
    glBufferData(GL_ARRAY_BUFFER, (size_t)g_static_gc * 12 * sizeof(float),
                 g_static_g, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, p->vbo_sw);
    glBufferData(GL_ARRAY_BUFFER, (size_t)g_static_wc * 12 * sizeof(float),
                 g_static_w, GL_STATIC_DRAW);
    SDL_Log("Veg static: %d grass, %d wheat (whole map, %d super-chunks)",
            g_static_gc, g_static_wc, g_sc_n);
    return 1;
}

// Pre-thinned static layer for shadows: every 2nd instance.
// Shadows must match the visible plants (otherwise it looks "crooked"), so the
// thinning is minimal — by half.
// The buffer is built PER SUPER-CHUNK (in the same order as the main static
// layer): the shadow pass draws only super-chunks inside the cascade box and
// does not push 318k instances through the vertex shader for the sake of 10
// visible ones.
static int *g_sc_shcnt[2], *g_sc_shoff[2];   // pre-thinned ranges
static void veg_shadow_build(void)
{
    g_sc_shcnt[0] = calloc((size_t)g_sc_n, sizeof(int));
    g_sc_shcnt[1] = calloc((size_t)g_sc_n, sizeof(int));
    g_sc_shoff[0] = malloc((size_t)(g_sc_n + 1) * sizeof(int));
    g_sc_shoff[1] = malloc((size_t)(g_sc_n + 1) * sizeof(int));
    // Every one of these is indexed in the loop below and read by the shadow
    // pass. veg_cleanup frees all four unconditionally, so a partial set is safe
    // to leave — what must not happen is a draw reading a NULL range.
    if (!g_sc_shcnt[0] || !g_sc_shcnt[1] || !g_sc_shoff[0] || !g_sc_shoff[1]) {
        SDL_Log("Vegetation shadow: out of memory for the super-chunk ranges");
        g_shadow_veg = 0;
        return;
    }
    for (int q = 0; q < 2; q++) {
        const float *src = q == 0 ? g_static_g : g_static_w;
        int srcn = q == 0 ? g_static_gc : g_static_wc;
        int n = (srcn + 1) / 2;
        float *thin = n > 0 ? malloc((size_t)n * 12 * sizeof(float)) : NULL;
        if (n > 0 && !thin) {
            SDL_Log("Vegetation shadow: out of memory (%d %s thins)",
                    n, q ? "wheat" : "grass");
            g_shadow_veg = 0;   // gates both the static and the dynamic read
            break;
        }
        int k = 0;
        g_sc_shoff[q][0] = 0;
        for (int s = 0; s < g_sc_n; s++) {
            int a = q == 0 ? g_sc_off_g[s] : g_sc_off_w[s];
            int b = q == 0 ? g_sc_off_g[s + 1] : g_sc_off_w[s + 1];
            int start = a + (a & 1);   // first EVEN index in [a,b): the rule
            // of global thinning i%2==0 must be preserved exactly
            g_sc_shcnt[q][s] = start < b ? (b - start + 1) / 2 : 0;
            for (int i = start; i < b; i += 2)
                memcpy(thin + (size_t)k++ * 12, src + (size_t)i * 12,
                       12 * sizeof(float));
            g_sc_shoff[q][s + 1] = k;
        }
        g_veg_shadow_cnt[q] = n;
        glGenVertexArrays(1, &g_veg_shadow_vao[q]);
        glGenBuffers(1, &g_veg_shadow_vbo[q]);
        veg_setup_vao(g_veg_shadow_vao[q], g_veg_shadow_vbo[q]);
        glBindBuffer(GL_ARRAY_BUFFER, g_veg_shadow_vbo[q]);
        glBufferData(GL_ARRAY_BUFFER, (size_t)n * 12 * sizeof(float),
                     thin, GL_STATIC_DRAW);
        free(thin);
    }
    // Dynamic shadow VAOs (thinned copies of vbo_dg/vbo_dw — filled in
    // veg_update when the buffers are rebuilt).
    for (int q = 2; q < 4; q++) {
        glGenVertexArrays(1, &g_veg_shadow_vao[q]);
        glGenBuffers(1, &g_veg_shadow_vbo[q]);
        veg_setup_vao(g_veg_shadow_vao[q], g_veg_shadow_vbo[q]);
        glBindBuffer(GL_ARRAY_BUFFER, g_veg_shadow_vbo[q]);
        glBufferData(GL_ARRAY_BUFFER, 1, NULL, GL_STREAM_DRAW);
    }
}

// Dynamic layer: on camera movement it recreates only the chunks whose density
// changed noticeably, then uploads them to the VBO.
static void veg_dyn_thin_build(int need[][2], int nn, VegParams *p);
static void veg_update(const float cam_pos[3], float dt, VegParams *p)
{
    int cx = (int)floorf(cam_pos[0] / VEG_CHUNK);
    int cy = (int)floorf(cam_pos[2] / VEG_CHUNK);
    // The occupancy map may have changed (a model was placed via the glb/mesh
    // console command — splat_occupy_* bumps the version): dynamic chunks must
    // be rebuilt so the grass retreats from under the new model. Compare the
    // version against the one we last saw.
    static unsigned int s_occ_seen = 0;
    unsigned int occ_now = splat_occupancy_version();
    int occ_dirty = (occ_now != s_occ_seen);
    if (occ_dirty) s_occ_seen = occ_now;
    // Camera velocity (XZ) for predictive preloading. The lead distance is
    // ~0.5 s of travel along the movement direction, 6..40 m. While standing
    // still — the point equals the camera (as before).
    float vx = 0.0f, vz = 0.0f;
    if (g_prev_cam_ok && dt > 1e-4f) {
        vx = (cam_pos[0] - g_prev_cam[0]) / dt;
        vz = (cam_pos[2] - g_prev_cam[2]) / dt;
    }
    float speed = sqrtf(vx * vx + vz * vz);
    float look = 0.0f;
    if (speed > 1.0f) {
        look = speed * 0.5f;
        if (look < 6.0f) look = 6.0f; else if (look > 40.0f) look = 40.0f;
        g_look_bx = cam_pos[0] + vx / speed * look;
        g_look_bz = cam_pos[2] + vz / speed * look;
    } else {
        g_look_bx = cam_pos[0];
        g_look_bz = cam_pos[2];
    }
    // Movement since the last frame: if the camera shifted noticeably, update
    // EVEN inside the same chunk cell — otherwise grass/field "switch" only
    // when a chunk border is crossed (popping up under the nose). Regeneration
    // is still budget-limited (2/frame), and the VBO is re-uploaded only on a
    // real change (dirty flag) — standing still is nearly free.
    int moving = g_prev_cam_ok &&
                 (fabsf(cam_pos[0] - g_prev_cam[0]) +
                  fabsf(cam_pos[2] - g_prev_cam[2])) > 0.05f;
    g_prev_cam[0] = cam_pos[0]; g_prev_cam[1] = cam_pos[1];
    g_prev_cam[2] = cam_pos[2]; g_prev_cam_ok = 1;
    if (!occ_dirty && !moving && g_regen_pending <= 0 &&
        cx == g_chunk_cam[0] && cy == g_chunk_cam[1]) return;
    g_chunk_cam[0] = cx;
    g_chunk_cam[1] = cy;

    // r_veg_dyn_mult compresses the LOAD RADIUS of dynamic chunks (not the
    // density curve — that one is falloff_mult): in "simplified" mode we load
    // chunks at a shorter distance, less generation/drawing. Default 1.0 (no
    // change).
    float dyn_r = s_dyn_r * cvar_get("r_veg_dyn_mult");
    if (dyn_r < 8.0f) dyn_r = 8.0f;
    float dr2 = dyn_r * dyn_r;
    // list of needed chunks (within the camera→lead-point path radius)
    int need[512][2], nn = 0;
    for (int j = 0; j < g_map_d && nn < 512; j++)
        for (int i = 0; i < g_map_w && nn < 512; i++) {
            float pr = veg_path_dist((float)(i * 16 + 8), (float)(j * 16 + 8),
                                     cam_pos[0], cam_pos[2], g_look_bx, g_look_bz);
            if (pr * pr <= dr2) { need[nn][0] = i; need[nn][1] = j; nn++; }
        }
    // unload chunks that left the radius
    int dirty = 0;
    for (int idx = 0; idx < g_map_w * g_map_d; idx++) {
        VegChunk *c = &g_veg_chunks[idx];
        if (!c->g && !c->w) continue;
        bool keep = false;
        for (int k = 0; k < nn && !keep; k++)
            if (need[k][1] * g_map_w + need[k][0] == idx) keep = true;
        if (!keep) {
            free(c->g); free(c->w);
            c->g = NULL; c->w = NULL; c->gn = c->wn = 0;
            dirty = 1;
        }
    }
    // Chunks closest to the path (camera→lead point) come first: the budget is
    // spent on the corridor ahead of the movement, so vegetation there loads in
    // time before arrival; at the feet it is already dense and needs no regen.
    for (int a = 1; a < nn; a++) {
        int bi = need[a][0], bj = need[a][1];
        float br2 = veg_path_dist((float)(bi * 16 + 8), (float)(bj * 16 + 8),
                                  cam_pos[0], cam_pos[2], g_look_bx, g_look_bz);
        br2 = br2 * br2;
        int b2 = a - 1;
        while (b2 >= 0) {
            float dd = veg_path_dist((float)(need[b2][0] * 16 + 8),
                                     (float)(need[b2][1] * 16 + 8),
                                     cam_pos[0], cam_pos[2], g_look_bx, g_look_bz);
            if (dd * dd <= br2) break;
            need[b2 + 1][0] = need[b2][0];
            need[b2 + 1][1] = need[b2][1];
            b2--;
        }
        need[b2 + 1][0] = bi;
        need[b2 + 1][1] = bj;
    }
    // recreate the chunks whose density changed noticeably
    int changed = 0;
    int budget = (int)cvar_get("r_veg_regen_budget");
    if (budget < 1) budget = 1;
    if (budget > 2) budget = 2;
    g_regen_pending = 0;
    for (int k = 0; k < nn; k++) {
        int i = need[k][0], j = need[k][1];
        VegChunk *c = &g_veg_chunks[j * g_map_w + i];
        float r = veg_path_dist((float)(i * 16 + 8), (float)(j * 16 + 8),
                                cam_pos[0], cam_pos[2], g_look_bx, g_look_bz);
        // The density TARGET for DYNAMIC grass does not depend on distance (the
        // shader thins by distance itself): the chunk is not rebuilt when the
        // camera approaches — only on load/occupancy change. Wheat — as before.
        float tgd = cvar_get("r_grass_density") + cvar_get("r_dandelion_density");
        float twd = veg_density(r, cvar_get("r_wheat_density"), s_wheat_floor);
        // IMPORTANT: the same density formula as written by veg_gen_chunk (grass
        // group = grass + dandelions, from cvars). Previously the target was
        // computed from the map.json constants (s_*_density) while the chunk
        // remembered the density from the cvars — due to this mismatch EVERY
        // chunk failed the tolerance, and ALL vegetation was recreated on every
        // chunk transition (regen storm).
        // RELATIVE tolerance: wheat at the camera is 140 items/m², grass 1.2 —
        // the absolute TOL=3 made wheat recreate on almost every chunk transition
        // (dozens of crossings of the 140→1 threshold), while grass (Δ<3) never
        // updated and "unloaded/loaded" with a pop at one ring.
        // Threshold = a fraction of the density (lower bound VEG_REGEN_MIN):
        // wheat updates in rare big steps (the budget keeps up — not "crooked"),
        // grass in soft steps as the camera approaches, both kinds coherent.
        float tol_g = tgd * VEG_REGEN_REL; if (tol_g < VEG_REGEN_MIN) tol_g = VEG_REGEN_MIN;
        float tol_w = twd * VEG_REGEN_REL; if (tol_w < VEG_REGEN_MIN) tol_w = VEG_REGEN_MIN;
        bool need_gen = (!c->g && !c->w) || occ_dirty ||
                        fabsf(c->dg_d - tgd) > tol_g ||
                        fabsf(c->dw_d - twd) > tol_w;
        if (need_gen) {
            if (changed >= budget) {   // the tail — in following frames
                g_regen_pending = 1;
                continue;
            }
            if (!g_gen_scratch_g)
                g_gen_scratch_g = malloc((size_t)VEG_CAND_CAP * 12 * sizeof(float));
            if (!g_gen_scratch_w)
                g_gen_scratch_w = malloc((size_t)VEG_CAND_CAP * 12 * sizeof(float));
            if (!g_gen_scratch_g || !g_gen_scratch_w) {
                SDL_Log("Vegetation: generation scratch allocation failed");
                g_regen_pending = 1;
                return;
            }
            int gn = 0, wn = 0;
            float dg_d, dw_d;
            veg_gen_chunk(p->world, i, j, cam_pos[0], cam_pos[2], false,
                          g_gen_scratch_g, g_gen_scratch_w, &gn, &wn,
                          &dg_d, &dw_d);
            float *gb = gn ? malloc((size_t)gn * 12 * sizeof(float)) : NULL;
            float *wb = wn ? malloc((size_t)wn * 12 * sizeof(float)) : NULL;
            if ((gn && !gb) || (wn && !wb)) {
                free(gb); free(wb);
                SDL_Log("Vegetation: chunk allocation failed");
                g_regen_pending = 1;
                return;
            }
            if (gn) memcpy(gb, g_gen_scratch_g, (size_t)gn * 12 * sizeof(float));
            if (wn) memcpy(wb, g_gen_scratch_w, (size_t)wn * 12 * sizeof(float));
            free(c->g); free(c->w);
            c->g = gb; c->w = wb; c->gn = gn; c->wn = wn;
            c->dg_d = dg_d; c->dw_d = dw_d;
            changed++;
            dirty = 1;
        }
    }
    // With a budget tail there are frames with no newly ready chunk. Previously
    // they still copied the whole dynamic layer and redefined the VBO/SSBO/shadows.
    if (!dirty) return;
    // flat dynamic buffer -> VBO
    int gc = 0, wc = 0;
    for (int k = 0; k < nn; k++) {
        VegChunk *c = &g_veg_chunks[need[k][1] * g_map_w + need[k][0]];
        gc += c->gn; wc += c->wn;
    }
    if (gc > g_dyn_cap || wc > g_dyn_cap) {
        int cap = (int)((gc > wc ? gc : wc) * 1.2f) + 1;
        float *ng = realloc(g_dyn_g, (size_t)cap * 12 * sizeof(float));
        if (!ng) {
            SDL_Log("Vegetation: dynamic grass buffer allocation failed");
            return;
        }
        g_dyn_g = ng;
        float *nw = realloc(g_dyn_w, (size_t)cap * 12 * sizeof(float));
        if (!nw) {
            SDL_Log("Vegetation: dynamic wheat buffer allocation failed");
            return;
        }
        g_dyn_w = nw;
        g_dyn_cap = cap;
    }
    float *gd = g_dyn_g, *wd = g_dyn_w;
    for (int k = 0; k < nn; k++) {
        VegChunk *c = &g_veg_chunks[need[k][1] * g_map_w + need[k][0]];
        if (c->gn) { memcpy(gd, c->g, (size_t)c->gn * 12 * sizeof(float)); gd += (size_t)c->gn * 12; }
        if (c->wn) { memcpy(wd, c->w, (size_t)c->wn * 12 * sizeof(float)); wd += (size_t)c->wn * 12; }
    }
    g_dyn_gc = gc;
    g_dyn_wc = wc;
    glBindBuffer(GL_ARRAY_BUFFER, p->vbo_dg);
    glBufferData(GL_ARRAY_BUFFER, (size_t)gc * 12 * sizeof(float), g_dyn_g, GL_STREAM_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, p->vbo_dw);
    glBufferData(GL_ARRAY_BUFFER, (size_t)wc * 12 * sizeof(float), g_dyn_w, GL_STREAM_DRAW);
    // GPU-driven: the same dynamic layers — into the Src SSBO for compute-cull.
    veg_gpu_src_upload(2, g_dyn_g, g_dyn_gc);
    veg_gpu_src_upload(3, g_dyn_w, g_dyn_wc);
    // Thinned copies for shadows: exactly the uThin rule from shadowveg.vs
    // ((flat_index*2654435761)%100<50) — the shadow maps are byte-for-byte the
    // same, but the vertex stage of the shadow pass handles half as many
    // instances. Ranges are remembered PER CHUNK (in need-list order): cascades
    // 0/1 (8/16 m boxes) draw only the chunks inside the box, not all 512.
    veg_dyn_thin_build(need, nn, p);
    if (changed)
        SDL_Log("Veg dyn: %d grass, %d wheat (%d chunks regen)", gc, wc, changed);
}

// Centers and pre-thinned ranges of dynamic chunks (for shadows)
static float g_dyn_sh_x[512], g_dyn_sh_z[512];
static int g_dyn_sh_go[512], g_dyn_sh_gc[512];
static int g_dyn_sh_wo[512], g_dyn_sh_wc[512];
static int g_dyn_sh_n;

// The uThin thinning rule from shadowveg.vs for the buffer's GLOBAL index
// (the same for grass and wheat): hash(id*2654435761)%100 < 50
static int g_dyn_sh_keep(int idx)
{
    return (unsigned)idx * 2654435761u % 100u < 50u;
}

static void veg_dyn_thin_build(int need[][2], int nn, VegParams *p)
{
    (void)p;
    g_dyn_sh_n = nn;
    if (nn == 0) return;
    int main_g0[512], main_w0[512];
    // 1) flat starts of chunks in the MAIN buffer (the hash rule — by these)
    int sgo = 0, swo = 0;
    for (int k = 0; k < nn; k++) {
        VegChunk *c = &g_veg_chunks[need[k][1] * g_map_w + need[k][0]];
        main_g0[k] = sgo;  main_w0[k] = swo;
        sgo += c->gn;      swo += c->wn;
    }
    // 2) how many instances remain after thinning, thin starts, chunk centers
    int og = 0, ow = 0;
    for (int k = 0; k < nn; k++) {
        VegChunk *c = &g_veg_chunks[need[k][1] * g_map_w + need[k][0]];
        int g0 = main_g0[k], w0 = main_w0[k];
        int gc2 = 0, wc2 = 0;
        for (int t = 0; t < c->gn; t++) if (g_dyn_sh_keep(g0 + t)) gc2++;
        for (int t = 0; t < c->wn; t++) if (g_dyn_sh_keep(w0 + t)) wc2++;
        g_dyn_sh_go[k] = og;  og += gc2;
        g_dyn_sh_wo[k] = ow;  ow += wc2;
        g_dyn_sh_gc[k] = gc2;
        g_dyn_sh_wc[k] = wc2;
        g_dyn_sh_x[k] = (float)need[k][0] * VEG_CHUNK + 8.0f;
        g_dyn_sh_z[k] = (float)need[k][1] * VEG_CHUNK + 8.0f;
    }
    g_veg_shadow_cnt[2] = og;
    g_veg_shadow_cnt[3] = ow;
    // 3) fill the thin buffers in need-list order (same instance order as
    //    before: the shadow maps are identical)
    if (og > g_shadow_scratch_gcap) {
        int cap = og + og / 4 + 1;
        float *ng = realloc(g_shadow_scratch_g, (size_t)cap * 12 * sizeof(float));
        if (!ng) {
            // The old block is too small for this thin list — the memcpy below
            // would write past it. The draws are gated by g_shadow_veg, so
            // switching it off leaves the stale counts unread.
            SDL_Log("Vegetation shadow: out of memory (%d grass thins)", og);
            g_shadow_veg = 0;
            return;
        }
        g_shadow_scratch_g = ng;
        g_shadow_scratch_gcap = cap;
    }
    if (ow > g_shadow_scratch_wcap) {
        int cap = ow + ow / 4 + 1;
        float *nw = realloc(g_shadow_scratch_w, (size_t)cap * 12 * sizeof(float));
        if (!nw) {
            SDL_Log("Vegetation shadow: out of memory (%d wheat thins)", ow);
            g_shadow_veg = 0;
            return;
        }
        g_shadow_scratch_w = nw;
        g_shadow_scratch_wcap = cap;
    }
    float *tg = g_shadow_scratch_g;
    float *tw = g_shadow_scratch_w;
    int kg = 0, kw = 0;
    for (int k = 0; k < nn; k++) {
        VegChunk *c = &g_veg_chunks[need[k][1] * g_map_w + need[k][0]];
        int g0 = main_g0[k], w0 = main_w0[k];
        for (int t = 0; t < c->gn; t++)
            if (g_dyn_sh_keep(g0 + t)) {
                memcpy(tg + (size_t)kg * 12, c->g + (size_t)t * 12,
                       12 * sizeof(float));
                kg++;
            }
        for (int t = 0; t < c->wn; t++)
            if (g_dyn_sh_keep(w0 + t)) {
                memcpy(tw + (size_t)kw * 12, c->w + (size_t)t * 12,
                       12 * sizeof(float));
                kw++;
            }
    }
    glBindBuffer(GL_ARRAY_BUFFER, g_veg_shadow_vbo[2]);
    glBufferData(GL_ARRAY_BUFFER, (size_t)og * 12 * sizeof(float), tg, GL_STREAM_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, g_veg_shadow_vbo[3]);
    glBufferData(GL_ARRAY_BUFFER, (size_t)ow * 12 * sizeof(float), tw, GL_STREAM_DRAW);
}

// Shadow pass (RP_SHADOW): thinned buffers, ranges per super-chunk/chunk
// inside the cascade box (the logic moved here from shadow.c).
static void veg_shadow_draw(const RendererContext *rc)
{
    // Vegetation is written into ALL cascades. Cascades 2/3 are cheap: culling
    // by super-chunks (below) leaves only a few chunks inside the box, and the
    // static matrix cache (shadow.c) redraws them only on a snap step.
    glDisable(GL_CULL_FACE);     // billboards — from both sides
    glUseProgram(s_shadowveg_prog);
    glUniformMatrix4fv(u_veg_viewproj, 1, GL_FALSE, rc->shadow_vp);
    glUniform1i(u_veg_veg, 0);
    glUniform1f(u_veg_time, g_veg_time);
    glUniform3f(u_veg_campos, rc->cam_pos[0], rc->cam_pos[1], rc->cam_pos[2]);
    glUniform1f(u_veg_walk, rc->cam_fly ? 0.0f : 1.0f);
    glUniform1f(u_veg_windboost, rc->wetness);   // shadows stay in sync with the wind
    // The grass shadow is thinned by distance exactly like the visible grass —
    // otherwise the shadow is denser than the plant. Same values as in the main pass.
    if (u_veg_grassfall >= 0)
        glUniform1f(u_veg_grassfall, s_dyn_r * cvar_get("r_veg_falloff_mult"));
    if (u_veg_grassfloor >= 0) {
        float d0 = cvar_get("r_grass_density") + cvar_get("r_dandelion_density");
        float ff = d0 > 1e-4f ? (s_grass_floor + s_dandelion_floor) / d0 : 0.0f;
        glUniform1f(u_veg_grassfloor, ff);
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, g_veg_tex);
    glUniform1i(u_veg_thin, 0);    // thinning already done on the CPU

    // long-shadow overshoot: a plant up to ~1 m tall under a low sun casts its
    // shadow sideways by height*slope meters — a chunk just outside the box may
    // still throw one (like objects in objects.c)
    float reach = rc->shadow_slope * 1.5f;

    // Static: full LOD (incl. lod2). Cascade 3 (512 m, 25 cm texel) — wheat
    // only: a grass tuft there is under two texels and turns into shimmer when
    // the map is rebuilt, while the wide lod2 field quads read up to the
    // horizon (field shadows — out to ~256 m).
    glUniform1f(u_veg_dyn, 0.0f);
    if (rc->shadow_static && g_shadow_veg) {
        // q: 0=grass (g_static_g), 1=wheat (g_static_w). Cascade 3 — WHEAT ONLY
        // (qstart=1): a grass tuft there is < two texels and shimmers; the near
        // cascades draw both kinds.
        int qstart = rc->shadow_cascade >= 3 ? 1 : 0;
        for (int q = qstart; q < 2; q++) {
            if (rc->shadow_cull && g_has_base_instance) {
                // box + half-diagonal of a super-chunk (64 m: 32*1.414 ≈ 45.3)
                float r = 45.3f + rc->shadow_box_cur * 0.7071f + reach;
                float r2 = r * r;
                float pcx = rc->cam_pos[0], pcz = rc->cam_pos[2];
                for (int s = 0; s < g_sc_n; s++) {
                    int cnt = g_sc_shcnt[q][s];
                    if (cnt <= 0) continue;
                    float dx = g_sc_cx[s] - pcx;
                    float dz = g_sc_cz[s] - pcz;
                    if (dx*dx + dz*dz > r2) continue;
                    glBindVertexArray(g_veg_shadow_vao[q]);
                    glDrawElementsInstancedBaseInstance(GL_TRIANGLES, 6,
                            GL_UNSIGNED_INT, NULL, cnt,
                            (GLuint)g_sc_shoff[q][s]);
                }
            } else if (q == 0) {
                glBindVertexArray(g_veg_shadow_vao[0]);
                glDrawElementsInstanced(GL_TRIANGLES, 6, GL_UNSIGNED_INT, NULL,
                                        g_veg_shadow_cnt[0]);
            } else {
                glBindVertexArray(g_veg_shadow_vao[1]);
                glDrawElementsInstanced(GL_TRIANGLES, 6, GL_UNSIGNED_INT, NULL,
                                        g_veg_shadow_cnt[1]);
            }
        }
    }
    // Dynamic: lod0/lod1 only (no lod2). Chunks inside the box.
    glUniform1f(u_veg_dyn, 1.0f);
    if (rc->shadow_dyn && g_shadow_veg) {
        if (rc->shadow_cull && g_has_base_instance) {
            // box + half-diagonal of a chunk (16 m: 11.3) + long-shadow overshoot
            float r = 11.3f + rc->shadow_box_cur * 0.7071f + reach;
            float r2 = r * r;
            float pcx = rc->cam_pos[0], pcz = rc->cam_pos[2];
            for (int k = 0; k < g_dyn_sh_n; k++) {
                float dx = g_dyn_sh_x[k] - pcx;
                float dz = g_dyn_sh_z[k] - pcz;
                if (dx*dx + dz*dz > r2) continue;
                if (g_dyn_sh_gc[k] > 0) {
                    glBindVertexArray(g_veg_shadow_vao[2]);
                    glDrawElementsInstancedBaseInstance(GL_TRIANGLES, 6,
                            GL_UNSIGNED_INT, NULL, g_dyn_sh_gc[k],
                            (GLuint)g_dyn_sh_go[k]);
                }
                if (g_dyn_sh_wc[k] > 0) {
                    glBindVertexArray(g_veg_shadow_vao[3]);
                    glDrawElementsInstancedBaseInstance(GL_TRIANGLES, 6,
                            GL_UNSIGNED_INT, NULL, g_dyn_sh_wc[k],
                            (GLuint)g_dyn_sh_wo[k]);
                }
            }
        } else {
            glBindVertexArray(g_veg_shadow_vao[2]);
            glDrawElementsInstanced(GL_TRIANGLES, 6, GL_UNSIGNED_INT, NULL,
                                    g_veg_shadow_cnt[2]);
            glBindVertexArray(g_veg_shadow_vao[3]);
            glDrawElementsInstanced(GL_TRIANGLES, 6, GL_UNSIGNED_INT, NULL,
                                    g_veg_shadow_cnt[3]);
        }
    }
    glEnable(GL_CULL_FACE);
}

// Instanced fallback pass: static (super-chunks in radius + frustum) and
// dynamic. Extracted so it can be called TWICE for the z-prepass (depth, then
// color with GL_EQUAL) without duplicating code. Uniforms/texture/frustum are
// already set up by the caller (veg_node_render).
static void veg_draw_main_instances(VegParams *p, const RendererContext *rc)
{
    // Static: only super-chunks within the visibility radius (instead of the
    // whole map). Beyond it sprites are smaller than a pixel — the terrain
    // texture carries their look. When the camera climbs, the radius grows so
    // the mountaintop view doesn't go empty.
    glUniform1f(p->u_dyn, 0.0f);   // static: full LOD (incl. lod2)
    if (g_draw_static_main) {
        float vis = s_vis_r * cvar_get("r_veg_vis_mult");
        if (rc->cam_pos[1] > VEG_VIS_H)
            vis += (rc->cam_pos[1] - VEG_VIS_H) * VEG_VIS_F;
        float vr2 = vis * vis;
        float half = VEG_SUPER * VEG_CHUNK * 0.5f;
        for (int pass = 0; pass < 2; pass++) {
            GLuint vao = pass ? p->vao_sw : p->vao_sg;
            GLuint vbo = pass ? p->vbo_sw : p->vbo_sg;
            glBindVertexArray(vao);
            for (int s = 0; s < g_sc_n; s++) {
                int cnt = pass ? g_sc_cnt_w[s] : g_sc_cnt_g[s];
                if (cnt <= 0) continue;
                if (!frustum_box(p->world, g_sc_cx[s] - half, g_sc_cz[s] - half,
                                 g_sc_cx[s] + half, g_sc_cz[s] + half)) continue;
                float dx = g_sc_cx[s] - rc->cam_pos[0];
                float dz = g_sc_cz[s] - rc->cam_pos[2];
                if (dx*dx + dz*dz > vr2) continue;
                GLuint first = (GLuint)(pass ? g_sc_off_w[s] : g_sc_off_g[s]);
                if (g_has_base_instance) {
                    glDrawElementsInstancedBaseInstance(GL_TRIANGLES, 6,
                            GL_UNSIGNED_INT, NULL, cnt, first);
                } else {
                    veg_instance_offset(vbo, (int)first);
                    glDrawElementsInstanced(GL_TRIANGLES, 6,
                                            GL_UNSIGNED_INT, NULL, cnt);
                }
            }
            if (!g_has_base_instance) veg_instance_offset(vbo, 0);
        }
    }
    glUniform1f(p->u_dyn, 1.0f);   // dynamic: lod0/lod1 only (no lod2)
    if (g_draw_dyn_main) {
        glBindVertexArray(p->vao_dg);
        glDrawElementsInstanced(GL_TRIANGLES, 6, GL_UNSIGNED_INT, NULL, g_dyn_gc);
        glBindVertexArray(p->vao_dw);
        glDrawElementsInstanced(GL_TRIANGLES, 6, GL_UNSIGNED_INT, NULL, g_dyn_wc);
    }
}

void veg_node_render(SceneNode *n, float dt)
{
    (void)dt;
    // Switching grass off completely with a single checkbox — previously
    // floor=1.0 kept the field even at density 0
    if (cvar_get("r_wheat_density") < 0.01f && cvar_get("r_grass_density") < 0.01f &&
        cvar_get("r_dandelion_density") < 0.01f) {
        return;
    }
    VegParams *p = n->params;
    const RendererContext *rc = p->rc;
    if (rc->pass == RP_SHADOW) {
        veg_shadow_draw(rc);
        return;
    }
    g_veg_time += dt * (1.0f + fx_wetness());
    // Densities from cvars changed (Graphics menu) — regenerate chunks:
    // resetting the camera chunk position forces veg_update to recreate everything.
    {
        static float s_prev_w = -1.0f, s_prev_g = -1.0f, s_prev_d = -1.0f,
                     s_prev_fo = -1.0f;
        float w = cvar_get("r_wheat_density");
        float g = cvar_get("r_grass_density");
        float d = cvar_get("r_dandelion_density");
        float fo = cvar_get("r_veg_falloff_mult");
        if (w != s_prev_w || g != s_prev_g || d != s_prev_d ||
            fo != s_prev_fo) {
            s_prev_w = w; s_prev_g = g; s_prev_d = d; s_prev_fo = fo;
            g_chunk_cam[0] = g_chunk_cam[1] = INT_MAX;
            SDL_Log("Veg density changed: wheat %.0f, grass %.1f, dandelion %.2f",
                    w, g, d);
        }
    }
    veg_update(rc->cam_pos, dt, p);
    if (g_static_gc + g_static_wc + g_dyn_gc + g_dyn_wc <= 0) return;
    if (!g_veg_visible) return;
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);             // billboards are visible from both sides
    // The sprite is a HARD cutout (texels with alpha < 0.5 discard), but the
    // survivors keep alpha 0.5..1.0. With blending on (main.c enables it for the
    // whole frame) overlapping blades would mix in DRAW order — and the
    // GPU-driven cull emits survivors in whatever order the atomics return, so
    // the distant field shimmers frame to frame. Alpha-to-coverage edges do not
    // need blending either: coverage still limits the write.
    GLboolean had_blend = glIsEnabled(GL_BLEND);
    glDisable(GL_BLEND);
    glUseProgram(p->prog);
    // MSAA does not smooth alpha-cutout sprite edges (discard) — enable
    // alpha-to-coverage: a semi-transparent texel edge gives partial sample
    // coverage and the edge antialiases. Does not affect the shadow map.
    int a2c = bloom_msaa_active() > 0;
    if (a2c) glEnable(GL_SAMPLE_ALPHA_TO_COVERAGE);
    // Common set: kinds, light, environment, fog, shadow cascades (from rc)
    scene_uniforms_bind(&p->su, rc);
    // Plants: their own bias/gradient (shadow_bias_veg from map.json — shadows
    // hugging the plants, no normal-term: a billboard normal is meaningless)
    for (int c = 0; c < SHADOW_CASCADES; c++) {
        if (p->su.u_shadowbias[c] >= 0)
            glUniform1f(p->su.u_shadowbias[c], rc->shadow_bias_veg[c]);
        if (p->su.u_shadowgrad[c] >= 0)
            glUniform1f(p->su.u_shadowgrad[c], rc->shadow_grad_veg[c]);
    }
    glUniform1f(p->u_time, g_veg_time);
    glUniform1f(p->u_walk, rc->cam_fly ? 0.0f : 1.0f);
    glUniform1f(p->u_windboost, rc->wetness);
    if (p->u_lodshift >= 0) glUniform1f(p->u_lodshift, cvar_get("r_veg_lod_shift"));
    if (p->u_simple >= 0) glUniform1f(p->u_simple, cvar_get("r_simple_render"));
    // GRASS is thinned by distance in the shader (smoothly), not by streaming
    // chunks: the dynamic layer is generated at full density and the shader
    // removes the excess toward the edge. uGrassFall — the same falloff
    // distance as in veg_density (C).
    if (p->u_grassfall >= 0)
        glUniform1f(p->u_grassfall, s_dyn_r * cvar_get("r_veg_falloff_mult"));
    if (p->u_grassfloor >= 0) {
        float d0 = cvar_get("r_grass_density") + cvar_get("r_dandelion_density");
        float ff = d0 > 1e-4f ? (s_grass_floor + s_dandelion_floor) / d0 : 0.0f;
        glUniform1f(p->u_grassfloor, ff);
    }
    frustum_extract(rc->viewproj);
    glBindTexture(GL_TEXTURE_2D_ARRAY, p->tex);

    // GPU-driven (compute-cull + indirect): vertices are bounded by visible
    // pixels, not by density. The "billion" cost ≈ the "one blade" cost.
    if (g_veg_gpudriven && (int)cvar_get("r_veg_gpudriven") > 0.5f) {
        veg_gpu_draw(p, rc);
        glEnable(GL_CULL_FACE);
        if (a2c) glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
        if (had_blend) glEnable(GL_BLEND);
        return;
    }

    // Otherwise — the instanced fallback (the old path).
    // Z-PREPASS (only when MSAA/alpha-to-coverage is OFF): first a cheap depth
    // pass (alpha-test only, no lighting), then the color pass with
    // glDepthFunc(GL_EQUAL) and depthMask off — every visible pixel is shaded
    // EXACTLY ONCE, overdraw in the heavy shader is cut. Same quad and same
    // discard => the depth matches exactly, the picture is identical. With a2c
    // the edges smooth via partial sample coverage — no prepass there, to avoid
    // fighting with GL_EQUAL (MEDIUM/ULTRA run on strong hardware anyway).
    int zprepass = !a2c && p->u_depthonly >= 0 &&
                   cvar_get("r_veg_zprepass") > 0.5f;
    if (zprepass) {
        // 1) Depth: write Z, color masked, regular GL_LESS.
        glUniform1f(p->u_depthonly, 1.0f);
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        glDepthMask(GL_TRUE);
        glDepthFunc(GL_LESS);
        veg_draw_main_instances(p, rc);
        // 2) Color: shade only the nearest fragment (Z is already final),
        // depthMask off — the early test discards losers before the shader.
        glUniform1f(p->u_depthonly, 0.0f);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDepthMask(GL_FALSE);
        glDepthFunc(GL_EQUAL);
        veg_draw_main_instances(p, rc);
        // Restore the regular depth state for subsequent passes.
        glDepthFunc(GL_LESS);
        glDepthMask(GL_TRUE);
    } else {
        if (p->u_depthonly >= 0) glUniform1f(p->u_depthonly, 0.0f);
        veg_draw_main_instances(p, rc);
    }
    glEnable(GL_CULL_FACE);
    if (a2c) glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
    if (had_blend) glEnable(GL_BLEND);
}

int veg_count(int q)
{
    const int cnt[4] = { g_static_gc, g_static_wc, g_dyn_gc, g_dyn_wc };
    return cnt[q];
}

// Shadow pass: draw only super-chunks inside the cascade box —
// returns the super-chunk range [*first, *last) to reset the global counter
// (super-chunks go in order, culling works over them).
// Dynamic layer in shadows: chunks and their pre-thinned ranges.
// Configures the vegetation VAO with the shared mesh (a quad: 4 vertices, 6 indices)
// and the instance buffer (12 float: pos3, normal2, yaw/scale/tint/layer, lean,
// slope2 — see veg_gen_chunk). Used for all VAOs: regular and shadow ones.
static void veg_setup_vao(GLuint vao, GLuint inst_vbo)
{
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_veg_mesh_vbo);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), NULL);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float),
                          (const void *)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_veg_mesh_ibo);
    glBindBuffer(GL_ARRAY_BUFFER, inst_vbo);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, 12 * sizeof(float), NULL);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, 12 * sizeof(float),
                          (const void *)(3 * sizeof(float)));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, 12 * sizeof(float),
                          (const void *)(5 * sizeof(float)));
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(5, 1, GL_FLOAT, GL_FALSE, 12 * sizeof(float),
                          (const void *)(9 * sizeof(float)));
    glEnableVertexAttribArray(5);
    glVertexAttribPointer(6, 2, GL_FLOAT, GL_FALSE, 12 * sizeof(float),
                          (const void *)(10 * sizeof(float)));
    glEnableVertexAttribArray(6);
    glVertexAttribDivisor(2, 1);
    glVertexAttribDivisor(3, 1);
    glVertexAttribDivisor(4, 1);
    glVertexAttribDivisor(5, 1);
    glVertexAttribDivisor(6, 1);
    glBindVertexArray(0);
}

// --- GPU-driven: compute-cull + indirect draw -------------------------------
// Creates the SSBO/indirect/VAO objects and compiles the culling compute
// shader. On any error it disables the path (g_veg_gpudriven = 0) — the
// instanced fallback remains. Called from veg_make_node only when the context
// is >= GL 4.3.
static void veg_gpu_init(VegParams *p)
{
    (void)p;
    s_cull_prog = make_compute_program("shaders/veg_cull.cs");
    if (!s_cull_prog) { g_veg_gpudriven = 0; return; }
    u_cull_vp     = glGetUniformLocation(s_cull_prog, "uViewProj");
    u_cull_viewh  = glGetUniformLocation(s_cull_prog, "uViewH");
    u_cull_tan    = glGetUniformLocation(s_cull_prog, "uTanHalfFov");
    u_cull_minpix = glGetUniformLocation(s_cull_prog, "uMinPix");
    u_cull_lodshift = glGetUniformLocation(s_cull_prog, "uLodShift");
    u_cull_src    = glGetUniformLocation(s_cull_prog, "uSrcCount");
    u_cull_off    = glGetUniformLocation(s_cull_prog, "uOffset");
    u_cull_planes = glGetUniformLocation(s_cull_prog, "uPlanes");
    u_cull_campos = glGetUniformLocation(s_cull_prog, "uCamPos");
    u_cull_capacity = glGetUniformLocation(s_cull_prog, "uDstCapacity");
    // Bind the compute program's SSBO blocks to binding points 0/1/2.
    GLuint bsrc = (p_glGetProgramResourceIndex)
                  ? p_glGetProgramResourceIndex(s_cull_prog, GL_SHADER_STORAGE_BLOCK, "Src")
                  : GL_INVALID_INDEX;
    GLuint bdst = (p_glGetProgramResourceIndex)
                  ? p_glGetProgramResourceIndex(s_cull_prog, GL_SHADER_STORAGE_BLOCK, "Dst")
                  : GL_INVALID_INDEX;
    GLuint bind = (p_glGetProgramResourceIndex)
                  ? p_glGetProgramResourceIndex(s_cull_prog, GL_SHADER_STORAGE_BLOCK, "Indirect")
                  : GL_INVALID_INDEX;
    if (bsrc == GL_INVALID_INDEX || bdst == GL_INVALID_INDEX || bind == GL_INVALID_INDEX) {
        SDL_Log("veg: SSBO block binding failed, GPU-driven off");
        glDeleteProgram(s_cull_prog);
        s_cull_prog = 0; g_veg_gpudriven = 0; return;
    }
    glShaderStorageBlockBinding(s_cull_prog, bsrc, 0);
    glShaderStorageBlockBinding(s_cull_prog, bdst, 1);
    glShaderStorageBlockBinding(s_cull_prog, bind, 2);
    for (int q = 0; q < 4; q++) {
        g_veg_src_cnt[q] = 0;
        glGenBuffers(1, &g_veg_src_ssbo[q]);
        glGenBuffers(1, &g_veg_cull_ssbo[q]);
        // The Cull SSBO must exist BEFORE the VAO (instance buffer): 1 float.
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_veg_cull_ssbo[q]);
        glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(float), NULL, GL_DYNAMIC_COPY);
        glGenBuffers(1, &g_veg_indirect[q]);
        GLuint cmd[5] = { 6, 0, 0, 0, 0 };   // count=6 (quad), instanceCount=0
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, g_veg_indirect[q]);
        glBufferData(GL_DRAW_INDIRECT_BUFFER, sizeof(cmd), cmd, GL_DYNAMIC_DRAW);
        glGenVertexArrays(1, &g_veg_vao_cull[q]);
        veg_setup_vao(g_veg_vao_cull[q], g_veg_cull_ssbo[q]);
        glBindVertexArray(0);
    }
    SDL_Log("Veg GPU-driven: compute-cull ENABLED (4 layers)");
}

// Uploads layer q (0=static grass, 1=static wheat, 2=dynamic grass, 3=dynamic
// wheat) into the Src SSBO and resizes the Cull SSBO to match it (worst case —
// all survived). Called after static-layer generation and after every dynamic rebuild.
static void veg_gpu_src_upload(int q, const float *src, int count)
{
    g_veg_src_cnt[q] = count;
    if (!g_veg_gpudriven || count <= 0) return;
    size_t bytes = (size_t)count * 12 * sizeof(float);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_veg_src_ssbo[q]);
    glBufferData(GL_SHADER_STORAGE_BUFFER, bytes, src, GL_DYNAMIC_COPY);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_veg_cull_ssbo[q]);
    glBufferData(GL_SHADER_STORAGE_BUFFER, bytes, NULL, GL_DYNAMIC_COPY);
}

// GPU-driven pass: cull + indirect draw of the 4 layers. The vertex cost is
// bounded by visible pixels (far sub-pixel blades are culled in compute even
// before the vertex shader). The STATIC layer is pre-culled by super-chunks
// EXACTLY like the instanced fallback (frustum + visibility radius) — the
// compute dispatch runs only over visible super-chunks, the resulting set is
// visually the same, but far fewer instances are iterated. ONE barrier for all
// layers.
static void veg_gpu_draw(VegParams *p, const RendererContext *rc)
{
    float fov = rc->cam_fov_deg * 0.0174532925f;
    float tanhf = tanf(0.5f * fov);
    glUseProgram(s_cull_prog);
    glUniformMatrix4fv(u_cull_vp, 1, GL_FALSE, rc->viewproj);
    glUniform1f(u_cull_viewh, (float)rc->vh);
    glUniform1f(u_cull_tan, tanhf);
    // Visibility threshold — a cvar: 1 px by default. Culling runs on a SPHERE
    // around the whole sprite (see veg_cull.cs), so plants at the screen edge
    // stay visible — previously the point test clipped them entirely.
    glUniform1f(u_cull_minpix, cvar_get("r_veg_minpix"));
    if (u_cull_lodshift >= 0) glUniform1f(u_cull_lodshift, cvar_get("r_veg_lod_shift"));
    glUniform4fv(u_cull_planes, 6, &rc->frustum.p[0][0]);
    glUniform3f(u_cull_campos, rc->cam_pos[0], rc->cam_pos[1], rc->cam_pos[2]);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, p->tex);

    // --- PASS 1: compute-cull of all layers (accumulate into indirect) -----
    // WAR guard: last frame's draws consumed Dst as a vertex attribute array and
    // the indirect block as a command stream; the dispatches below write both.
    glMemoryBarrier(GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT | GL_COMMAND_BARRIER_BIT
                    | GL_BUFFER_UPDATE_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);
    for (int q = 0; q < 4; q++) {
        if (g_veg_src_cnt[q] <= 0) continue;
        if ((q < 2 && !g_draw_static_main) || (q >= 2 && !g_draw_dyn_main)) continue;
        // zero the instanceCount before accumulating this layer
        GLuint zero = 0;
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, g_veg_indirect[q]);
        glBufferSubData(GL_DRAW_INDIRECT_BUFFER, 4, 4, &zero);
        // the CPU write above has to land before the compute atomics touch the
        // same buffer, and the new command stream before the draw consumes it
        glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT
                        | GL_SHADER_STORAGE_BARRIER_BIT);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, g_veg_src_ssbo[q]);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, g_veg_cull_ssbo[q]);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, g_veg_indirect[q]);
        if (u_cull_capacity >= 0)
            glUniform1i(u_cull_capacity, g_veg_src_cnt[q]);

        if (q < 2) {
            // STATIC: only visible super-chunks (like the fallback). This is the
            // main saving: don't run compute over the WHOLE map every frame.
            float vis = s_vis_r * cvar_get("r_veg_vis_mult");
            if (rc->cam_pos[1] > VEG_VIS_H)
                vis += (rc->cam_pos[1] - VEG_VIS_H) * VEG_VIS_F;
            float vr2 = vis * vis;
            float half = VEG_SUPER * VEG_CHUNK * 0.5f;
            const int *off = q == 0 ? g_sc_off_g : g_sc_off_w;
            const int *cnt = q == 0 ? g_sc_cnt_g : g_sc_cnt_w;
            for (int s = 0; s < g_sc_n; s++) {
                if (cnt[s] <= 0) continue;
                if (!frustum_box(p->world, g_sc_cx[s] - half, g_sc_cz[s] - half,
                                 g_sc_cx[s] + half, g_sc_cz[s] + half)) continue;
                float dx = g_sc_cx[s] - rc->cam_pos[0];
                float dz = g_sc_cz[s] - rc->cam_pos[2];
                if (dx*dx + dz*dz > vr2) continue;
                glUniform1i(u_cull_off, off[s]);
                glUniform1i(u_cull_src, cnt[s]);
                glDispatchCompute((GLuint)((cnt[s] + 63) / 64), 1, 1);
            }
        } else {
            // DYNAMIC: the whole layer (already limited to the radius around the camera)
            glUniform1i(u_cull_off, 0);
            glUniform1i(u_cull_src, g_veg_src_cnt[q]);
            glDispatchCompute((GLuint)((g_veg_src_cnt[q] + 63) / 64), 1, 1);
        }
    }
    // ONE barrier: each layer writes its own buffer, we read only after all of them.
    // The survivors are fetched by the vertex stage through a GL_ARRAY_BUFFER
    // binding (veg_setup_vao), not through an SSBO in the shader, so the bit that
    // makes compute writes visible to that fetch is
    // GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT.
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT
                    | GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, 0);

    // --- PASS 2: indirect draw of the survivors ----------------------------
    glUseProgram(p->prog);
    if (p->u_lodshift >= 0) glUniform1f(p->u_lodshift, cvar_get("r_veg_lod_shift"));
    if (p->u_simple >= 0) glUniform1f(p->u_simple, cvar_get("r_simple_render"));
    for (int q = 0; q < 4; q++) {
        if (g_veg_src_cnt[q] <= 0) continue;
        if ((q < 2 && !g_draw_static_main) || (q >= 2 && !g_draw_dyn_main)) continue;
        glUniform1f(p->u_dyn, q >= 2 ? 1.0f : 0.0f);
        glBindVertexArray(g_veg_vao_cull[q]);
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, g_veg_indirect[q]);
        glDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT, (const void *)0);
    }
    glBindVertexArray(0);
}

SceneNode *veg_make_node(const WorldState *w, const RendererContext *rc)
{
    // Densities and radii — from map/map.json (veg = { ... })
    {
        MapVeg v;
        mapdata_veg(&v);
        s_grass_density = v.grass_density;
        s_wheat_density = v.wheat_density;
        s_grass_floor = v.grass_floor;
        s_wheat_floor = v.wheat_floor;
        s_dandelion_density = v.dandelion_density;
        s_dandelion_floor   = v.dandelion_floor;
        s_falloff_r = v.falloff;
        s_dyn_r = v.dyn_radius;
        s_vis_r = v.vis_radius;
        // Densities as cvars (Graphics menu): defaults come from map.json,
        // applied when chunks are regenerated.
        cvar_register("r_wheat_density", s_wheat_density, "пшеница шт/м2");
        cvar_register("r_grass_density", s_grass_density, "трава шт/м2");
        cvar_register("r_dandelion_density", s_dandelion_density,
                      "одуванчики шт/м2 (сверх травы)");
        // The MAP's values are applied only to cvars the user/preset did not
        // explicitly set (CVAR_CFGSET): user settings outrank the map, and on
        // the first run the field looks exactly as the map author intended.
        cvar_set_default("r_wheat_density", s_wheat_density);
        cvar_set_default("r_grass_density", s_grass_density);
        cvar_set_default("r_dandelion_density", s_dandelion_density);
        // Radius multiplier for static wheat: <1 — the far field stays only in
        // the terrain texture (POTATO mode, less overdraw)
        cvar_register_ex("r_veg_vis_mult", 1.0f, 0.25f, 1.0f, CVAR_ARCHIVE,
                         "множитель радиуса прорисовки пшеницы (1 = карта)");
        cvar_register_ex("r_veg_falloff_mult", 1.0f, 0.3f, 1.0f, CVAR_ARCHIVE,
                         "спад плотности поля: <1 — реже уже к среднему кольцу");
        cvar_register_ex("r_veg_lod_shift", 0.0f, 0.0f, 2.0f, CVAR_ARCHIVE,
                         "сдвиг LOD пшеницы к камере (0=как на карте, 1-2=шире раньше)");
        cvar_register_ex("r_veg_regen_budget", 4096.0f, 1.0f, 4096.0f,
                         CVAR_ARCHIVE, "чанков регенерации за кадр (меньше = "
                         "меньше рывков CPU, дорисовывается в следующих)");
        cvar_register_ex("r_veg_dyn_mult", 1.0f, 0.4f, 1.0f, CVAR_ARCHIVE,
                         "множитель радиуса динамических чанков (<1 = грузим "
                         "траву ближе, меньше генерации/отрисовки)");
        SDL_Log("Veg cfg: wheat %.0f/m2, grass %.1f/m2, dandelion %.2f/m2, "
                "dyn %.0fm, vis %.0fm",
                s_wheat_density, s_grass_density, s_dandelion_density,
                s_dyn_r, s_vis_r);
    }
    GLuint prog = make_program("shaders/wheat.vs", "shaders/wheat.fs");
    // Plant sprites packed into one 2D array (canvas = the largest layer,
    // the rest pinned to the top-left corner):
    //   layer 0  grass view 1 (tall)     img/grass.png      128x128
    //   layer 1  grass view 2 (medium)   img/grass1.png     128x128
    //   layer 2  grass view 3 (short)    img/grass2.png     128x128
    //   layer 3  dandelion               img/dandelions.png 128x128
    //   layer 4  wheat lod0              img/wheat_lod0.png 128x178
    //   layer 5  wheat lod1              img/wheat_lod1.png 152x178
    //   layer 6  wheat lod2              img/wheat_lod2.png 256x178
    // The index order MUST match VEG_LAY_* (see the top of the file).
    int ww[7] = {0}, hh[7] = {0};
    unsigned char *img[7];
    static const char *const path[7] = {
        "img/grass.png", "img/grass1.png", "img/grass2.png",
        "img/dandelions.png",
        "img/wheat_lod0.png", "img/wheat_lod1.png", "img/wheat_lod2.png",
    };
    for (int i = 0; i < 7; i++)
        img[i] = load_img(path[i], &ww[i], &hh[i], STBI_rgb_alpha);
    if (!prog) return NULL;
    for (int i = 0; i < 7; i++)
        if (!img[i]) { SDL_Log("veg: texture %s missing", path[i]); return NULL; }
    // Canvas: max along each dimension (wheat lod2 determines it).
    int cw = 1, ch = 1;
    for (int i = 0; i < 7; i++) {
        if (ww[i] > cw) cw = ww[i];
        if (hh[i] > ch) ch = hh[i];
    }
    // Sprites live in a 2D array: all vegetation is drawn in one draw call,
    // and the layer (instance layer attribute) picks which sprite to show.
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D_ARRAY, tex);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, cw, ch, VEG_LAYERS, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    for (int i = 0; i < 7; i++)
        glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, i, ww[i], hh[i], 1,
                        GL_RGBA, GL_UNSIGNED_BYTE, img[i]);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    // The pixels are owned by the resource cache — do not free them (see resources.h)
    g_veg_tex = tex;

    // Grass is a single slightly tilted quad, wheat is a cross of two quads:
    // that way grass looks natural while the wheat ear looks volumetric.
    const float veg_mesh[4][5] = {
        {-0.5f, 0, 0, 0, 1}, { 0.5f, 0, 0, 1, 1},
        {-0.5f, 1, 0, 0, 0}, { 0.5f, 1, 0, 1, 0},
    };
    const GLuint veg_idx[6] = { 0,1,2, 1,3,2 };
    glGenBuffers(1, &g_veg_mesh_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g_veg_mesh_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof veg_mesh, veg_mesh, GL_STATIC_DRAW);
    glGenBuffers(1, &g_veg_mesh_ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_veg_mesh_ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof veg_idx, veg_idx, GL_STATIC_DRAW);

    // 4 vegetation VAOs: static/dynamic x grass/wheat. The base mesh is one
    // quad, the instance is 12 float (see veg_gen_chunk).
    GLuint veg_vbo[4];
    for (int q = 0; q < 4; q++) {
        glGenVertexArrays(1, &g_veg_vao[q]);
        glGenBuffers(1, &veg_vbo[q]);
        veg_setup_vao(g_veg_vao[q], veg_vbo[q]);
        glBindBuffer(GL_ARRAY_BUFFER, veg_vbo[q]);
        glBufferData(GL_ARRAY_BUFFER, 1, NULL, GL_STREAM_DRAW);
    }
    g_veg_vb[0] = veg_vbo[0];
    g_veg_vb[1] = veg_vbo[1];
    g_veg_vb[2] = veg_vbo[2];
    g_veg_vb[3] = veg_vbo[3];
    glBindVertexArray(0);

    VegParams *p = calloc(1, sizeof *p);
    if (!p) {
        resources_program_release(prog);
        glDeleteTextures(1, &tex);
        glDeleteBuffers(1, &g_veg_mesh_vbo);
        glDeleteBuffers(1, &g_veg_mesh_ibo);
        for (int q = 0; q < 4; q++) {
            glDeleteBuffers(1, &g_veg_vb[q]);
            glDeleteVertexArrays(1, &g_veg_vao[q]);
            g_veg_vb[q] = g_veg_vao[q] = 0;
        }
        g_veg_mesh_vbo = g_veg_mesh_ibo = 0;
        return NULL;
    }
    p->world = w;
    p->rc = rc;
    p->prog = prog;
    p->vao_sg = g_veg_vao[0];
    p->vao_sw = g_veg_vao[1];
    p->vao_dg = g_veg_vao[2];
    p->vao_dw = g_veg_vao[3];
    p->vbo_sg = g_veg_vb[0];
    p->vbo_sw = g_veg_vb[1];
    p->vbo_dg = g_veg_vb[2];
    p->vbo_dw = g_veg_vb[3];
    p->tex = tex;
    // Shadow shader (depth with thinning): equals 0 after veg_cleanup(),
    // so replace here is equivalent to a plain load.
    s_shadowveg_prog = resources_program_replace(s_shadowveg_prog,
                                                 "shaders/shadowveg.vs",
                                                 "shaders/shadowveg.fs");
    if (!s_shadowveg_prog) {
        SceneNode partial = {0};
        partial.params = p;
        veg_cleanup(&partial);
        free(p);
        return NULL;
    }
    veg_prog_init(p);              // uniforms + static ones
    s_veg = p;                     // for hot-reload
    shader_on_reshade(veg_reshade);

    // Dynamic-layer chunks (around the camera)
    g_map_w = (int)(w->world_w / VEG_CHUNK) + 1;
    g_map_d = (int)(w->world_d / VEG_CHUNK) + 1;
    g_veg_chunks = calloc((size_t)g_map_w * g_map_d, sizeof(VegChunk));
    g_dyn_g = g_dyn_w = NULL;
    g_dyn_cap = 0;

    // Static vegetation layer: the whole map, once at startup
    // (g_veg_chunks is dereferenced by every dynamic-layer pass, so an absent
    // chunk grid goes through the same "no vegetation node" path as a failed
    // static layer — veg_cleanup tolerates either being NULL.)
    if (!g_veg_chunks || !veg_static_init(p)) {
        SDL_Log("Vegetation: static layer allocation failed");
        SceneNode partial = {0};
        partial.params = p;
        veg_cleanup(&partial);
        free(p);
        return NULL;
    }

    // GLES has no core base-instance. Even if a desktop driver exports the
    // extension address in an ES context, use the guaranteed emulation.
#ifdef USE_GLES_PATH
    g_has_base_instance = 0;
#else
    g_has_base_instance = p_glDrawElementsInstancedBaseInstance != NULL;
#endif
    SDL_Log("Vegetation draw: %s",
            g_has_base_instance ? "instanced super-chunks (base-instance)"
                                : "instanced super-chunks (offset emulation)");

    // Pre-thinned layer for shadows (every 2nd instance)
    veg_shadow_build();

    // GPU-driven (compute-cull + indirect): context >= GL 4.3. The static
    // layers are already generated — upload them into the Src SSBOs. The
    // dynamic layer will fill in during the first camera movement in veg_update.
    g_veg_gpudriven = g_gl_gpu_driven;
    cvar_register("r_veg_gpudriven", 1.0f, "GPU-driven compute-cull травы (0=фолбэк)");
    cvar_register_ex("r_veg_minpix", 1.0f, 0.0f, 8.0f, CVAR_ARCHIVE,
                     "мин. экранный след травинки, px (0 = не отсекать)");
    cvar_register_ex("r_veg_zprepass", 1.0f, 0.0f, 1.0f, CVAR_ARCHIVE,
                     "z-prepass травы: срезать overdraw (только при MSAA off)");
    if (g_veg_gpudriven) {
        veg_gpu_init(p);
        veg_gpu_src_upload(0, g_static_g, g_static_gc);
        veg_gpu_src_upload(1, g_static_w, g_static_wc);
    }

    const float size[3] = { w->world_w, w->max_height - w->min_height,
                            w->world_d };
    SceneNode *node = node_new("Vegetation", "MeshInstance3D", NULL, size, p,
                               NULL, veg_node_render);
    if (!node) {
        SceneNode partial = {0};
        partial.params = p;
        veg_cleanup(&partial);
        free(p);
    }
    return node;
}

void veg_cleanup(SceneNode *n)
{
    if (!n) return;   // safe during partial scene construction (error path)
    VegParams *p = n->params;
    if (!p) return;
    if (g_veg_chunks) {
        for (int i = 0; i < g_map_w * g_map_d; i++) {
            free(g_veg_chunks[i].g);
            free(g_veg_chunks[i].w);
        }
        free(g_veg_chunks);
        g_veg_chunks = NULL;
    }
    // Reset the camera chunk position: on a reloaded map the dynamic layer is
    // regenerated immediately (otherwise the field stays half-empty until the
    // chunk changes).
    g_chunk_cam[0] = g_chunk_cam[1] = INT_MAX;
    free(g_static_g); free(g_static_w);
    free(g_dyn_g); free(g_dyn_w);
    free(g_gen_scratch_g); free(g_gen_scratch_w);
    free(g_shadow_scratch_g); free(g_shadow_scratch_w);
    g_static_g = g_static_w = g_dyn_g = g_dyn_w = NULL;
    g_gen_scratch_g = g_gen_scratch_w = NULL;
    g_shadow_scratch_g = g_shadow_scratch_w = NULL;
    g_shadow_scratch_gcap = g_shadow_scratch_wcap = 0;
    free(g_sc_cnt_g); free(g_sc_cnt_w);
    free(g_sc_off_g); free(g_sc_off_w);
    free(g_sc_cx); free(g_sc_cz);
    free(g_sc_shcnt[0]); free(g_sc_shcnt[1]);
    free(g_sc_shoff[0]); free(g_sc_shoff[1]);
    g_sc_cnt_g = g_sc_cnt_w = NULL;
    g_sc_off_g = g_sc_off_w = NULL;
    g_sc_cx = g_sc_cz = NULL;
    g_sc_shcnt[0] = g_sc_shcnt[1] = NULL;
    g_sc_shoff[0] = g_sc_shoff[1] = NULL;
    glDeleteBuffers(1, &g_veg_mesh_vbo);
    glDeleteBuffers(1, &g_veg_mesh_ibo);
    for (int q = 0; q < 4; q++) {
        glDeleteBuffers(1, &g_veg_vb[q]);
        glDeleteVertexArrays(1, &g_veg_vao[q]);
    }
    if (g_veg_gpudriven) {
        for (int q = 0; q < 4; q++) {
            glDeleteBuffers(1, &g_veg_src_ssbo[q]);
            glDeleteBuffers(1, &g_veg_cull_ssbo[q]);
            glDeleteBuffers(1, &g_veg_indirect[q]);
            glDeleteVertexArrays(1, &g_veg_vao_cull[q]);
        }
        if (s_cull_prog) glDeleteProgram(s_cull_prog);
        s_cull_prog = 0;
        g_veg_gpudriven = 0;
    }
    for (int q = 0; q < 4; q++) {
        glDeleteBuffers(1, &g_veg_shadow_vbo[q]);
        glDeleteVertexArrays(1, &g_veg_shadow_vao[q]);
    }
    glDeleteTextures(1, &p->tex);
    resources_program_release(s_shadowveg_prog);
    s_shadowveg_prog = 0;
    if (s_veg == p) s_veg = NULL;
    resources_program_release(p->prog);
}
