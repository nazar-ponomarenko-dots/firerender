// ============================================================================
//  terrain.c — terrain: heightmap grid, ground material, shadowed rendering
// ============================================================================
#include "terrain.h"

#include "assets.h"
#include "cvars.h"
#include "jobs.h"
#include "profiler.h"
#include "gl_loader.h"
#include "linalg.h"
#include "mapdata.h"
#include "png.h"
#include "resources.h"
#include "shader.h"
#include "sky.h"
#include "splat.h"

#include "stb_image.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#ifdef _WIN32
#  include <direct.h>
#endif

#include <SDL3/SDL.h>

// --- Shadow pass (pass == RP_SHADOW) ----------------------------------------
// Terrain depth into a cascade: the same grid, height taken from the heightmap in the shader.
// The program used to live in shadow.c — now it belongs to the terrain.
static GLuint s_shadow_prog = 0;
static GLint u_sh_viewproj, u_sh_heightmap, u_sh_hmin, u_sh_hmax;
static TerrainParams *s_terr = NULL;   // for hot-reload (shader_on_reshade)

// Recreates the programs, looks up uniform locations and static uniforms. Called
// when the node is created and on shader hot-reload (shader_hot_reload).
// Programs are created OUTSIDE (make_node / reshade: refcount is balanced there).
// Here — only the uniform locations for the CURRENT ids (p->prog and s_shadow_prog).
static void terrain_prog_init(TerrainParams *p, const WorldState *w)
{
    GLuint prog = p->prog;
    u_sh_viewproj = glGetUniformLocation(s_shadow_prog, "uViewProj");
    u_sh_heightmap = glGetUniformLocation(s_shadow_prog, "uHeightmap");
    u_sh_hmin = glGetUniformLocation(s_shadow_prog, "uHeightMin");
    u_sh_hmax = glGetUniformLocation(s_shadow_prog, "uHeightMax");

    scene_uniforms_init(&p->su, prog);    // shared scene set
    p->u_specpow  = glGetUniformLocation(prog, "uSpecPower");
    p->u_specint  = glGetUniformLocation(prog, "uSpecIntensity");
    p->u_heightmin = glGetUniformLocation(prog, "uHeightMin");
    p->u_heightmax = glGetUniformLocation(prog, "uHeightMax");
    p->u_texscale  = glGetUniformLocation(prog, "uTexScale");
    p->u_statictex = glGetUniformLocation(prog, "uStaticShadow");
    p->u_static_on = glGetUniformLocation(prog, "uStaticOn");
    p->u_worldsize = glGetUniformLocation(prog, "uWorldSize");
    p->u_baked_lm  = glGetUniformLocation(prog, "uBakedLM");
    p->u_baked_on  = glGetUniformLocation(prog, "uBakedOn");
    p->u_sunvis    = glGetUniformLocation(prog, "uSunVis");
    p->u_sunvis_on = glGetUniformLocation(prog, "uSunVisOn");
    p->u_wetness_mask = glGetUniformLocation(prog, "uWetnessMask");
    // Surfaces (splat): mask + 4 layers. Units: 10 — mask, 12..15 — layers
    // (0..9 and 11 are taken by base/normals/heights/cascades/static/
    // wetness/cubemap/noise — see below).
    p->u_splat = glGetUniformLocation(prog, "uSplat");
    p->u_splat_on = glGetUniformLocation(prog, "uSplatOn");
    p->u_detail_dist = glGetUniformLocation(prog, "uDetailDist");
    p->u_simple = glGetUniformLocation(prog, "uSimple");
    p->u_layer_scale = glGetUniformLocation(prog, "uLayerScale");
    for (int i = 0; i < SPLAT_LAYERS; i++) {
        char name[16];
        snprintf(name, sizeof name, "uLayer%d", i);
        p->u_layer[i] = glGetUniformLocation(prog, name);
    }

    // Terrain material: static shader parameters
    MapTerrain tc;
    mapdata_terrain(&tc);
    glUseProgram(prog);
    glUniform1i(glGetUniformLocation(prog, "uBasemap"), 0);   // ground
    glUniform1i(glGetUniformLocation(prog, "uNormalmap"), 1); // normals
    glUniform1i(glGetUniformLocation(prog, "uHeightmap"), 2); // heights
    glUniform1i(p->u_wetness_mask, 8);             // wetness mask (unit 8)
    // Shadow cascades on units 3-6 (uShadowMap%d and raw depth uShadowDepth%d —
    // the SAME textures: when sampled via sampler2D the comparison mode is
    // ignored, depth is read directly).
    for (int c = 0; c < SHADOW_CASCADES; c++) {
        if (p->su.u_shadowmap[c] >= 0)
            glUniform1i(p->su.u_shadowmap[c], 3 + c);
        if (p->su.u_shadowdepth[c] >= 0)
            glUniform1i(p->su.u_shadowdepth[c], 3 + c);
    }
    glUniform1i(p->u_statictex, 7);               // static shadows (unit 7)
    glUniform1i(p->u_static_on, 0);
    if (p->u_baked_lm >= 0) glUniform1i(p->u_baked_lm, 11); // luxel (unit 11:
                                                            // 9 taken by the reflection
                                                            // cubemap uReflection)
    if (p->u_baked_on >= 0) glUniform1f(p->u_baked_on, 0.0f);
    if (p->u_sunvis >= 0) glUniform1i(p->u_sunvis, 16);     // sun visibility map
    if (p->u_sunvis_on >= 0) glUniform1f(p->u_sunvis_on, 0.0f);
    glUniform2f(p->u_worldsize, w->world_w, w->world_d);
    // Surface layers: samplers and tile scales (meters per tile).
    if (p->u_splat >= 0) glUniform1i(p->u_splat, 10);
    for (int i = 0; i < SPLAT_LAYERS; i++)
        if (p->u_layer[i] >= 0) glUniform1i(p->u_layer[i], 12 + i);
    {
        const MapSurfaces *sf = splat_surfaces();
        if (p->u_layer_scale >= 0)
            glUniform4f(p->u_layer_scale, sf->scale[0], sf->scale[1],
                        sf->scale[2], sf->scale[3]);
        if (p->u_splat_on >= 0)
            glUniform1f(p->u_splat_on, splat_have() ? 1.0f : 0.0f);
    }
    // Environment/fog is pushed by scene_uniforms_bind every frame from rc
    glUniform1f(p->u_specpow, 40.0f);
    glUniform1f(p->u_specint, 0.0f);   // ground is matte — no sun specular
    glUniform1f(p->u_heightmin, w->min_height);
    glUniform1f(p->u_heightmax, w->max_height);
    // Ground tile: one texture per tex_scale meters (from map.json)
    glUniform2f(glGetUniformLocation(prog, "uTexScale"),
                w->world_w / tc.tex_scale, w->world_d / tc.tex_scale);
    // Wheat field: soil tint inside the rectangle (instead of grass)
    glUniform4f(glGetUniformLocation(prog, "uField"),
                w->field_x0, w->field_x0 + w->field_w,
                w->field_z0, w->field_z0 + w->field_d);
    glUniform3f(glGetUniformLocation(prog, "uFieldColor"),
                tc.field_color[0], tc.field_color[1], tc.field_color[2]);
    glUniform1f(glGetUniformLocation(prog, "uFieldBlend"), tc.field_blend);
    // Fog (uFogStart/uFogEnd) is pushed by scene_uniforms_bind from rc
    glUseProgram(0);
}

// Hot-reload: recreate the current node's programs and uniforms.
// We release the old programs: make_program refcounts them, and without release
// each reload would leak both p->prog and the shadow program (refs never dropped,
// so deferred deletion in resources.c never fired).
// replace releases the old reference and takes a fresh one. If the pair fails to
// rebuild, it returns 0 — we do NOT change the id and keep drawing the old one (still alive until release).
static void terrain_reshade(void)
{
    if (!s_terr) return;
    GLuint prog = resources_program_replace(s_terr->prog,
                                            "shaders/terrain.vs",
                                            "shaders/terrain.fs");
    if (prog) s_terr->prog = prog;
    else SDL_Log("terrain: reshade failed, keeping old program");
    GLuint sp = resources_program_replace(s_shadow_prog,
                                          "shaders/shadow.vs",
                                          "shaders/shadow.fs");
    if (sp) s_shadow_prog = sp;
    else SDL_Log("terrain: shadow program reshade failed, keeping old");
    terrain_prog_init(s_terr, s_terr->world);
}

// --- Static shadows (r_shadows_mode 2) ---------------------------------
// Shadow map resolution: ~1 texel per 1 m (2000x1125 -> 2048x1152).
#define STATIC_SHADOW_W 2048
#define STATIC_SHADOW_H 1152
#define STATIC_SHADOW_STEP 12.0f  // terrain ray step, m
#define STATIC_SHADOW_STEPS 40    // ray length: 480 m
#define STATIC_OBJ_STEP 4.0f      // object ray step, m (finer: canopies are narrow)
#define STATIC_OBJ_STEPS 60       // length: 240 m

// Object cylinders for tracing (gathered and sorted by x once)
#define STATIC_OBJ_MAX 256
typedef struct {
    float x, z;      // position
    float r;         // canopy radius, m
    float h;         // canopy height, m
    float base;      // ground height at the object
} ShadowObj;
static ShadowObj s_shobjs[STATIC_OBJ_MAX];
static int s_shobjs_n = 0;

static int shobj_cmp(const void *a, const void *b)
{
    float ax = ((const ShadowObj *)a)->x;
    float bx = ((const ShadowObj *)b)->x;
    return (ax > bx) - (ax < bx);
}

static void shobjs_build(const WorldState *w)
{
    s_shobjs_n = 0;
    for (int i = 0; i < mapdata_object_count() && s_shobjs_n < STATIC_OBJ_MAX; i++) {
        const MapObject *o = mapdata_object(i);
        if (!o) continue;
        ShadowObj *s = &s_shobjs[s_shobjs_n];
        float sy = o->scale[1] > 0.01f ? o->scale[1] : 1.0f;
        s->x = o->pos[0];
        s->z = o->pos[2];
        s->r = 4.5f * sy;                     // canopy radius
        s->h = 9.0f * sy + 2.0f;              // canopy height
        s->base = sample_height(w, s->x, s->z);
        s_shobjs_n++;
    }
    qsort(s_shobjs, (size_t)s_shobjs_n, sizeof s_shobjs[0], shobj_cmp);
}

// Ray to the sun: does it hit a tree canopy? A binary search on x prunes
// almost all objects — the small step (4 m) leaves no "holes" in the shadow.
static int ray_hits_object(float x, float y, float z)
{
    if (s_shobjs_n == 0) return 0;
    // object range with |ox| < 120 m (distant tree shadows are negligible)
    int lo = 0, hi = s_shobjs_n - 1, first = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (s_shobjs[mid].x >= x - 120.0f) { first = mid; hi = mid - 1; }
        else lo = mid + 1;
    }
    if (first < 0) return 0;
    for (int k = first; k < s_shobjs_n; k++) {
        const ShadowObj *o = &s_shobjs[k];
        if (o->x > x + 120.0f) break;
        // the ray is nearly horizontal (dz from the sun's z component); a ray
        // point at dist2d from the tree: the tree shadow stretches out to dist
        float ddx = x - o->x;
        float ddz = z - o->z;
        if (ddx * ddx + ddz * ddz > o->r * o->r) continue;
        if (y < o->base - 1.0f || y > o->base + o->h + 1.0f) continue;
        return 1;
    }
    return 0;
}

static GLuint s_static_tex = 0;
static int s_static_baked = 0;

// --- Baked light: GL luxel texture + 3D probe grid --------------------
static GLuint s_baked_lm_tex = 0;    // RGB8 terrain luxel (unit 11)
static GLuint s_baked_sunvis_tex = 0;// R8 (grayscale) sun visibility (unit 16):
                                     // distant static shadows in mode 1 (CSM)
static int    s_baked_have = 0;      // 1 = luxel loaded/baked
// Irradiance probe grid for dynamic objects (ambient RGB + sunvis).
static float *s_probes = NULL;       // nx*ny*nz*4 float (RGB + sunvis)
static int    s_pnx = 0, s_pny = 0, s_pnz = 0;
static float  s_py0 = 0.0f, s_py1 = 0.0f;   // grid vertical range
static float  s_pwx = 0.0f, s_pwz = 0.0f;   // world size (for the grid UV)

// --- Tiled geometry with 4 LOD -------------------------------------------------
// The map is cut into 128x128 m tiles. Each tile is built at 4 LOD levels
// (quad step 2/8/32/128 m) — the geometry does NOT depend on texel density:
// a vertex takes its height from the heightmap in the shader (GL_R16 LINEAR), so
// large quads at far LODs give smooth terrain with a minimum of polygons.
//
// A tile is a flat, even grid (no stitching or slabs) so the terrain looks
// smooth rather than "blocky". The seams between LODs are hidden by a SHORT skirt —
// a vertical wall along the tile border, dropped only a few meters
// (aDrop in the shader): no gaps, and from below the tile doesn't look like a massive block.
// We draw ALL tiles every frame (no frustum culling) — distance-based LOD
// keeps the polygon count low.
//
// Vertex: (x, z, u, v, drop) — 5 float. drop = 0 at the surface, at the bottom
// edge of the skirt drop = TERRAIN_SKIRT_DEPTH (shader: y = h - drop).

#define TERRAIN_SKIRT_DEPTH 4.0f    // skirt depth, m (hides LOD seams)
// LOD switch distances (to the nearest tile point), m.
// Implemented as cvars r_terrain_lod1..3: draw distance can be tuned
// without a rebuild (e.g. on weak machines shrink LOD3 = coarser background).
static float s_lod1 = 200.0f;
static float s_lod2 = 500.0f;
static float s_lod3 = 1200.0f;
static const float s_lod_step[TERRAIN_LODS] = { 2.0f, 8.0f, 32.0f, 128.0f };

// Growing vertex/index buffers (one VBO+IBO for the whole terrain)
typedef struct {
    float *v;    size_t nv, cv;    // 5 float per vertex
    GLuint *idx; size_t ni, ci;
    int failed;
} TerrMesh;

static GLuint tm_push_vert(TerrMesh *m, float x, float z,
                           float u, float v, float drop)
{
    if (m->failed) return 0;
    if (m->nv + 5 > m->cv) {
        size_t cap = m->cv ? m->cv * 2 : 1 << 18;
        float *p = realloc(m->v, cap * sizeof(float));
        if (!p) { m->failed = 1; return 0; }
        m->v = p;
        m->cv = cap;
    }
    GLuint id = (GLuint)(m->nv / 5);
    m->v[m->nv++] = x;  m->v[m->nv++] = z;
    m->v[m->nv++] = u;  m->v[m->nv++] = v;
    m->v[m->nv++] = drop;
    return id;
}

static void tm_push_tri(TerrMesh *m, GLuint a, GLuint b, GLuint c)
{
    if (m->failed) return;
    if (m->ni + 3 > m->ci) {
        size_t cap = m->ci ? m->ci * 2 : 1 << 19;
        GLuint *p = realloc(m->idx, cap * sizeof(GLuint));
        if (!p) { m->failed = 1; return; }
        m->idx = p;
        m->ci = cap;
    }
    m->idx[m->ni++] = a;  m->idx[m->ni++] = b;  m->idx[m->ni++] = c;
}

// Surface quad (xa,za)-(xb,zb), front face up (CCW from above)
static void tm_push_quad(TerrMesh *m, const WorldState *w,
                         float xa, float za, float xb, float zb)
{
    float tex_w = (float)w->tex_w, tex_h = (float)w->tex_h;
    GLuint v00 = tm_push_vert(m, xa, za, (xa + 0.5f) / tex_w, (za + 0.5f) / tex_h, 0);
    GLuint v01 = tm_push_vert(m, xa, zb, (xa + 0.5f) / tex_w, (zb + 0.5f) / tex_h, 0);
    GLuint v10 = tm_push_vert(m, xb, za, (xb + 0.5f) / tex_w, (za + 0.5f) / tex_h, 0);
    GLuint v11 = tm_push_vert(m, xb, zb, (xb + 0.5f) / tex_w, (zb + 0.5f) / tex_h, 0);
    tm_push_tri(m, v00, v01, v10);
    tm_push_tri(m, v01, v11, v10);
}

// Skirt vertex: same point but dropped by drop (0 = top of skirt)
static GLuint tm_push_skirt_vert(TerrMesh *m, const WorldState *w,
                                 float x, float z)
{
    return tm_push_vert(m, x, z, (x + 0.5f) / (float)w->tex_w,
                        (z + 0.5f) / (float)w->tex_h, TERRAIN_SKIRT_DEPTH);
}

// Skirt along a tile edge: nodes xs[0..n-1] on a line z=const (or x=const).
// The wall faces OUTWARD from the tile; the bottom edge is dropped only by
// TERRAIN_SKIRT_DEPTH meters (short: hides neighbouring-LOD seams, but
// the tile doesn't look like a massive block down to the world floor).
static void tm_push_skirt_edge(TerrMesh *m, const WorldState *w,
                               const float *xs, int n,
                               float fixed, int along_x, int outward_pos)
{
    for (int i = 0; i + 1 < n; i++) {
        float xa = xs[i], xb = xs[i + 1];
        // the top edge coincides with the tile surface border
        GLuint ta, tb, ba, bb;
        if (along_x) {   // edge along X: z = fixed (south/north)
            ta = tm_push_vert(m, xa, fixed, (xa + 0.5f) / (float)w->tex_w,
                              (fixed + 0.5f) / (float)w->tex_h, 0);
            tb = tm_push_vert(m, xb, fixed, (xb + 0.5f) / (float)w->tex_w,
                              (fixed + 0.5f) / (float)w->tex_h, 0);
        } else {         // edge along Z: x = fixed (west/east)
            ta = tm_push_vert(m, fixed, xa, (fixed + 0.5f) / (float)w->tex_w,
                              (xa + 0.5f) / (float)w->tex_h, 0);
            tb = tm_push_vert(m, fixed, xb, (fixed + 0.5f) / (float)w->tex_w,
                              (xb + 0.5f) / (float)w->tex_h, 0);
        }
        ba = tm_push_skirt_vert(m, w, along_x ? xa : fixed, along_x ? fixed : xa);
        bb = tm_push_skirt_vert(m, w, along_x ? xb : fixed, along_x ? fixed : xb);
        // Outward normal: south(-z), west(-x) -> (ta,tb,ba); north(+z), east(+x) -> (ta,ba,tb)
        if ((along_x && !outward_pos) || (!along_x && outward_pos)) {
            tm_push_tri(m, ta, tb, ba);
            tm_push_tri(m, tb, bb, ba);
        } else {
            tm_push_tri(m, ta, ba, tb);
            tm_push_tri(m, tb, ba, bb);
        }
    }
}

// One tile LOD: flat even grid with step spacing + short skirts.
// Returns the index range [first, first+count) in the shared IBO.
// Flat grid WITHOUT merging into slabs — the terrain is smooth, chunks don't
// look like stepped blocks.
static void terrain_tile_lod(const WorldState *w, TerrMesh *m,
                             float x0, float z0, float x1, float z1,
                             float step,
                             GLuint *first, GLsizei *count)
{
    *first = (GLuint)m->ni;
    float wt = x1 - x0, dt = z1 - z0;
    int ncx = (int)(wt / step + 0.5f);           // whole cells along X
    if ((float)ncx * step < wt - 0.01f) ncx++;   // + remainder cell (larger/smaller)
    if (ncx < 1) ncx = 1;
    int ncy = (int)(dt / step + 0.5f);
    if ((float)ncy * step < dt - 0.01f) ncy++;
    if (ncy < 1) ncy = 1;
    int nnx = ncx + 1, nny = ncy + 1;

    float *xs = malloc((size_t)nnx * sizeof(float));
    float *zs = malloc((size_t)nny * sizeof(float));
    if (!xs || !zs) {
        free(xs); free(zs);
        *count = 0;
        return;
    }
    for (int i = 0; i <= ncx; i++) xs[i] = (i < ncx) ? x0 + i * step : x1;
    for (int j = 0; j <= ncy; j++) zs[j] = (j < ncy) ? z0 + j * step : z1;

    // Flat grid: each cell is one quad (no merging into slabs)
    for (int j = 0; j < ncy; j++)
        for (int i = 0; i < ncx; i++)
            tm_push_quad(m, w, xs[i], zs[j], xs[i + 1], zs[j + 1]);

    // Short skirts along the four tile borders
    tm_push_skirt_edge(m, w, xs, nnx, z0, 1, 0);              // south
    tm_push_skirt_edge(m, w, xs, nnx, z1, 1, 1);              // north
    tm_push_skirt_edge(m, w, zs, nny, x0, 0, 0);              // west
    tm_push_skirt_edge(m, w, zs, nny, x1, 0, 1);              // east
    free(xs); free(zs);
    *count = (GLsizei)(m->ni - *first);
}

// Splits the map into 128x128 m tiles, builds 4 LODs per tile (flat quads
// plus skirts) and puts everything into one VBO/IBO. Vertex format:
// (x, z, u, v, drop) — 5 float; UV at texel centers of the map (GL_LINEAR
// between them gives bilinear height, like the old 1-quad-per-meter grid).
static int terrain_geometry_create(WorldState *w,
                                   GLuint *vao, GLuint *vbo, GLuint *ibo,
                                   int *vert_count,
                                   TerrainTile **out_tiles, int *out_tiles_n)
{
    int tiles_x = (int)ceilf(w->world_w / TERRAIN_TILE);
    int tiles_z = (int)ceilf(w->world_d / TERRAIN_TILE);
    if (tiles_x < 1) tiles_x = 1;
    if (tiles_z < 1) tiles_z = 1;
    TerrainTile *tiles = malloc((size_t)tiles_x * tiles_z * sizeof *tiles);
    if (!tiles) return 0;
    TerrMesh m = {0};
    Uint64 t0 = SDL_GetTicks();
    for (int tj = 0; tj < tiles_z; tj++) {
        for (int ti = 0; ti < tiles_x; ti++) {
            TerrainTile *t = &tiles[tj * tiles_x + ti];
            t->xmin = ti * TERRAIN_TILE;
            t->zmin = tj * TERRAIN_TILE;
            t->xmax = t->xmin + TERRAIN_TILE; if (t->xmax > w->world_w) t->xmax = w->world_w;
            t->zmax = t->zmin + TERRAIN_TILE; if (t->zmax > w->world_d) t->zmax = w->world_d;
            // Tile height range (for the AABB frustum test): scan with a 4 m step
            float ymin = 1e9f, ymax = -1e9f;
            for (float z = t->zmin; z <= t->zmax + 0.5f; z += 4.0f) {
                float zz = z > t->zmax ? t->zmax : z;
                for (float x = t->xmin; x <= t->xmax + 0.5f; x += 4.0f) {
                    float xx = x > t->xmax ? t->xmax : x;
                    float h = sample_height(w, xx, zz);
                    if (h < ymin) ymin = h;
                    if (h > ymax) ymax = h;
                }
            }
            t->ymin = ymin; t->ymax = ymax;
            for (int l = 0; l < TERRAIN_LODS; l++)
                terrain_tile_lod(w, &m, t->xmin, t->zmin, t->xmax, t->zmax,
                                 s_lod_step[l],
                                 &t->first[l], &t->count[l]);
        }
    }
    if (m.failed || m.nv == 0 || m.ni == 0) {
        SDL_Log("Terrain: geometry allocation failed");
        free(m.v);
        free(m.idx);
        free(tiles);
        return 0;
    }
    SDL_Log("Terrain: %dx%d tiles (%d), 4 LODs: %d verts / %d indices in %llu ms",
            tiles_x, tiles_z, tiles_x * tiles_z,
            (int)(m.nv / 5), (int)m.ni,
            (unsigned long long)(SDL_GetTicks() - t0));

    glGenVertexArrays(1, vao);
    glBindVertexArray(*vao);
    glGenBuffers(1, vbo);
    glBindBuffer(GL_ARRAY_BUFFER, *vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(m.nv * sizeof(float)),
                 m.v, GL_STATIC_DRAW);
    glGenBuffers(1, ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, *ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(m.ni * sizeof(GLuint)),
                 m.idx, GL_STATIC_DRAW);
    free(m.v);
    free(m.idx);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), NULL);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float),
                          (const void *)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, 5 * sizeof(float),
                          (const void *)(4 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glBindVertexArray(0);

    *vert_count = (int)m.ni;
    w->vert_count = *vert_count;      // for the HUD (GRID/TRIS)
    *out_tiles = tiles;
    *out_tiles_n = tiles_x * tiles_z;
    return 1;
}

// Squared distance from a point to the tile AABB (0 = inside)
static float terrain_tile_dist2(const TerrainTile *t, const float *p)
{
    float dx = p[0] < t->xmin ? t->xmin - p[0] : (p[0] > t->xmax ? p[0] - t->xmax : 0);
    float dy = p[1] < t->ymin ? t->ymin - p[1] : (p[1] > t->ymax ? p[1] - t->ymax : 0);
    float dz = p[2] < t->zmin ? t->zmin - p[2] : (p[2] > t->zmax ? p[2] - t->zmax : 0);
    return dx * dx + dy * dy + dz * dz;
}

// Draws the tiles. Color pass: frustum culling + distance-based LOD.
// Shadow pass: culling against the cascade's ortho box (rc->shadow_vp).
static void terrain_draw_tiles(const TerrainParams *p,
                               const RendererContext *rc)
{
    if (!p->tiles) return;               // OOM during build — nothing to draw
    int shadow = (rc->pass == RP_SHADOW);
    // LOD distances from cvars (r_terrain_lod1..3) — applied immediately
    float lod1 = cvar_get("r_terrain_lod1");
    float lod2 = cvar_get("r_terrain_lod2");
    float lod3 = cvar_get("r_terrain_lod3");
    s_lod1 = lod1; s_lod2 = lod2; s_lod3 = lod3;
    const float lod1_2 = lod1 * lod1;
    const float lod2_2 = lod2 * lod2;
    const float lod3_2 = lod3 * lod3;
    Frustum lf;                              // light frustum (for shadows)
    if (shadow) frustum_build(&lf, rc->shadow_vp);
    glBindVertexArray(p->vao);
    for (int i = 0; i < p->tiles_n; i++) {
        const TerrainTile *t = &p->tiles[i];
        float mn[3] = { t->xmin, t->ymin, t->zmin };
        float mx[3] = { t->xmax, t->ymax, t->zmax };
        // Frustum culling: the color pass uses the camera, the shadow pass — the light
        if (shadow) {
            if (!frustum_test_aabb(&lf, mn, mx))
                continue;
        } else {
            if (!frustum_test_aabb(&rc->frustum, mn, mx))
                continue;
        }
        float d2 = terrain_tile_dist2(t, rc->cam_pos);
        int lod;
        if (shadow) {
            float box = rc->shadow_box_cur;
            lod = box <= 16.0f ? (d2 < 1024.0f ? 0 : 1)
                : box <= 96.0f ? (d2 < 9216.0f ? 1 : 2)
                :                (d2 < 65536.0f ? 2 : 3);
        } else {
            lod = d2 < lod1_2 ? 0
                : d2 < lod2_2 ? 1
                : d2 < lod3_2 ? 2 : 3;
        }
        // Removed: reporting to the console "Terrain/frame: ..." every 240 frames (noise).
        if (!t->count[lod]) continue;
        glDrawElements(GL_TRIANGLES, t->count[lod], GL_UNSIGNED_INT,
                       (const void *)(uintptr_t)((size_t)t->first[lod] *
                                                 sizeof(GLuint)));
    }
}

// Ground material: ground.png with STAMPED-IN grass tufts (grass.png):
// distant grass is "baked" into the texture, so the WHOLE map looks covered
// in grass even where the volumetric tufts are no longer visible (an LOD texture —
// a standard trick, cheaper than millions of distant sprites).
static GLuint terrain_ground_texture(const unsigned char *ground_img,
                                     const unsigned char *grass_img)
{
    int cw = 512, ch = 512;
    unsigned char *cover = malloc((size_t)cw * ch * 3);
    if (!cover) return 0;
    memcpy(cover, ground_img, (size_t)cw * ch * 3);   // background = ground 1:1
    unsigned int rng2 = 0x51f15e;
    // Many tufts (2200): the distant vegetation look is carried by THIS
    // texture — the static sprite layer is drawn only around the camera.
    for (int s = 0; s < 2200; s++) {
        float cx = frnd(&rng2) * cw, cy = frnd(&rng2) * ch;
        float sc = 0.12f + 0.16f * frnd(&rng2);   // tuft 15..36 px
        float ang = frnd(&rng2) * 6.2831853f;
        float tint = 0.80f + 0.25f * frnd(&rng2);
        float ca = cosf(ang), sa = sinf(ang);
        int hs = (int)(64 * sc) + 4;
        for (int dy = -hs; dy <= hs; dy++)
          for (int dx = -hs; dx <= hs; dx++) {
            float ux = ( dx * ca + dy * sa) / sc;
            float uy = (-dx * sa + dy * ca) / sc;
            int sx = (int)(ux + 64), sy = (int)(uy + 64);
            if ((unsigned)sx > 127 || (unsigned)sy > 127) continue;
            const unsigned char *sp = grass_img + ((size_t)sy * 128 + sx) * 4;
            if (sp[3] < 128) continue;
            int px = (int)cx + dx, py = (int)cy + dy;
            if ((unsigned)px >= (unsigned)cw || (unsigned)py >= (unsigned)ch) continue;
            unsigned char *d = cover + ((size_t)py * cw + px) * 3;
            float a = sp[3] / 255.0f * 0.55f;   // semi-transparent tufts
            d[0] = (unsigned char)(d[0] * (1 - a) + sp[0] * tint * a);
            d[1] = (unsigned char)(d[1] * (1 - a) + sp[1] * tint * a);
            d[2] = (unsigned char)(d[2] * (1 - a) + sp[2] * tint * a);
        }
    }
    GLuint id = make_texture(cw, ch, cover, GL_RGB, GL_REPEAT, 0);
    free(cover);
    return id;
}

SceneNode *terrain_make_node(WorldState *w, const WorldTextures *wt,
                             const RendererContext *rc)
{
    // Ground material: texture from map.json (terrain.ground_tex), grass is
    // always img/grass.png (stamped into the ground)
    MapTerrain tc;
    mapdata_terrain(&tc);
    int gw = 0, gh = 0, aw = 0, ah = 0;
    unsigned char *ground_img = load_img(tc.ground_tex, &gw, &gh, STBI_rgb);
    unsigned char *grass_img  = load_img("img/grass.png", &aw, &ah, STBI_rgb_alpha);
    if (!ground_img || !grass_img) return NULL;
    GLuint tex_ground = terrain_ground_texture(ground_img, grass_img);
    if (!tex_ground) return NULL;
    // Pixels are owned by the resource cache — we don't free them (see resources.h)

    GLuint prog = make_program("shaders/terrain.vs", "shaders/terrain.fs");
    if (!prog) { glDeleteTextures(1, &tex_ground); return NULL; }
    // Shadow program (terrain depth): after cleanup() s_shadow_prog == 0,
    // so replace here is equivalent to a plain load.
    s_shadow_prog = resources_program_replace(s_shadow_prog,
                                              "shaders/shadow.vs",
                                              "shaders/shadow.fs");
    if (!s_shadow_prog) {
        resources_program_release(prog);
        glDeleteTextures(1, &tex_ground);
        return NULL;
    }

    GLuint vao = 0, vbo = 0, ibo = 0;
    int vert_count = 0;
    TerrainTile *tiles = NULL;
    int tiles_n = 0;
    if (!terrain_geometry_create(w, &vao, &vbo, &ibo, &vert_count,
                                 &tiles, &tiles_n)) {
        resources_program_release(s_shadow_prog);
        s_shadow_prog = 0;
        resources_program_release(prog);
        glDeleteTextures(1, &tex_ground);
        return NULL;
    }

    TerrainParams *p = calloc(1, sizeof *p);
    if (!p) {
        glDeleteBuffers(1, &vbo);
        glDeleteBuffers(1, &ibo);
        glDeleteVertexArrays(1, &vao);
        free(tiles);
        resources_program_release(s_shadow_prog);
        s_shadow_prog = 0;
        resources_program_release(prog);
        glDeleteTextures(1, &tex_ground);
        return NULL;
    }
    p->world = w;
    p->rc = rc;                           // per-frame data (renderer.c)
    p->prog = prog;
    p->vao = vao;    p->vbo = vbo;
    p->ibo = ibo;
    p->tex_base = tex_ground;             // ground (unit 0)
    p->tex_norm = wt->tex_norm;           // normals (unit 1)
    p->tex_height = wt->tex_height;       // heights (unit 2)
    p->tiles = tiles;
    p->tiles_n = tiles_n;
    p->vert_count = vert_count;
    // Surface layers (splat): textures tile (REPEAT) with mipmapping —
    // asphalt/sand don't shimmer at distance. Paths and scales come from map.json
    // (surfaces = { ... }), defaults are img/dirt|sand|gravel|asphalt.png.
    {
        const MapSurfaces *sf = splat_surfaces();
        for (int i = 0; i < SPLAT_LAYERS; i++)
            p->tex_splat_layer[i] = resources_texture(sf->tex[i], 3,
                                                      GL_REPEAT, 1);
    }
    terrain_prog_init(p, w);              // uniforms (+ static)
    s_terr = p;                           // for hot-reload
    shader_on_reshade(terrain_reshade);

    const float size[3] = { w->world_w, w->max_height - w->min_height,
                            w->world_d };
    SceneNode *node = node_new("Terrain", "MeshInstance3D", NULL, size, p,
                               NULL, terrain_node_render);
    if (!node) {
        SceneNode partial = {0};
        partial.params = p;
        terrain_cleanup(&partial);
        free(p);
    }
    return node;
}

void terrain_node_render(SceneNode *n, float dt)
{
    (void)dt;
    TerrainParams *p = n->params;
    const RendererContext *rc = p->rc;

    static int s_cvar_reg = 0;
    if (!s_cvar_reg) {
        cvar_register_ex("r_terrain_lod1", s_lod1, 10.0f, 2000.0f, CVAR_ARCHIVE,
                         "рельеф LOD1: дальность детальной сетки, м");
        cvar_register_ex("r_terrain_lod2", s_lod2, 20.0f, 4000.0f, CVAR_ARCHIVE,
                         "рельеф LOD2: дальность средней сетки, м");
        cvar_register_ex("r_terrain_lod3", s_lod3, 30.0f, 8000.0f, CVAR_ARCHIVE,
                         "рельеф LOD3: дальность грубой сетки, м");
        cvar_register_ex("r_terr_detail_dist", 0.0f, 0.0f, 500.0f, CVAR_ARCHIVE,
                         "LOD деталей террейна: дальше — базовая карта (0 = выкл)");
        s_cvar_reg = 1;
    }

    // Shadow pass (RP_SHADOW): terrain depth into the current cascade.
    // Orchestration is done by shadow.c (see shadow_render in renderer_frame).
    // No frustum culling: distant tiles cast long shadows into the cascade.
    if (rc->pass == RP_SHADOW) {
        if (g_shadow_terrain) {
            glUseProgram(s_shadow_prog);
            glUniformMatrix4fv(u_sh_viewproj, 1, GL_FALSE, rc->shadow_vp);
            glUniform1i(u_sh_heightmap, 2);
            glUniform1f(u_sh_hmin, p->world->min_height);
            glUniform1f(u_sh_hmax, p->world->max_height);
            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_2D, p->tex_height);
            terrain_draw_tiles(p, rc);   // cull by cascade volume + LOD
        }
        return;
    }

    // normal pass (the viewport is set by the render context: window or cubemap face)
    glViewport(rc->view_vp[0], rc->view_vp[1], rc->view_vp[2], rc->view_vp[3]);
    glEnable(GL_DEPTH_TEST);   // 3D: near slopes hide far ones
    glDepthMask(GL_TRUE);      // (the skybox disables it)
    glUseProgram(p->prog);
    // Shared set: views, light, environment, fog, shadow cascades (from rc)
    scene_uniforms_bind(&p->su, p->rc);
    // Static shadows (mode 2): a multiplier instead of CSM
    {
        float on = 0.0f;
        if (cvar_get("r_shadows_mode") == 2.0f && terrain_shadows_have())
            on = 1.0f;
        glUniform1f(p->u_static_on, on);
        if (on > 0.5f) {
            glActiveTexture(GL_TEXTURE7);
            glBindTexture(GL_TEXTURE_2D, s_static_tex);
        }
    }
    // Baked light (unit 11): the RGB luxel replaces ambient+sun+shadow.
    // This is a STATIC lighting solution (occluder shadows are baked offline), so it
    // turns on ONLY in static-shadow mode (r_shadows_mode==2). In
    // modes 0/1/3 the terrain takes the dynamic path (ambient+sun+CSM):
    // that is "dynamics INSTEAD OF statics", not on top of them. Produced only offline
    // (make mapbake) and gated by cvar r_baked_light.
    // IMPORTANT: not unit 9 — that's the reflection cubemap uReflection (samplerCube).
    // Two samplers of different types on one unit = INVALID_OPERATION on strict
    // desktop drivers, and the whole terrain pass silently vanishes (llvmpipe
    // tolerated it, real GL did not: the terrain wasn't drawn at all).
    {
        float on = 0.0f;
        if (cvar_get("r_shadows_mode") == 2.0f &&
            cvar_get("r_baked_light") > 0.5f && terrain_baked_light_have())
            on = 1.0f;
        if (p->u_baked_on >= 0) glUniform1f(p->u_baked_on, on);
        if (on > 0.5f) {
            glActiveTexture(GL_TEXTURE11);
            glBindTexture(GL_TEXTURE_2D, s_baked_lm_tex);
        }
    }
    // Sun visibility map (unit 16): ONLY in mode 1 (CSM). Cascades cover
    // the near zone; beyond them there were no shadows (see shadow_at → 1.0 past 250 m).
    // Here, at FAR distances we blend in the baked static shadow — distant
    // trees/field cast a shadow on the horizon again.
    {
        float on = 0.0f;
        if (cvar_get("r_shadows_mode") == 1.0f && terrain_baked_sunvis_have())
            on = 1.0f;
        if (p->u_sunvis_on >= 0) glUniform1f(p->u_sunvis_on, on);
        if (on > 0.5f) {
            glActiveTexture(GL_TEXTURE16);
            glBindTexture(GL_TEXTURE_2D, s_baked_sunvis_tex);
        }
    }
    // Wetness mask (under a roof): unit 8
    if (terrain_wetness_mask_have()) {
        glActiveTexture(GL_TEXTURE8);
        glBindTexture(GL_TEXTURE_2D, terrain_wetness_mask_tex());
    }
    // Detail LOD: 0 = full splatting (default), >0 = detail radius (m).
    // Beyond that — the plain base map: we save 6-8 texture samples per pixel
    // over a LARGE part of the screen (the "POTATO" mode).
    if (p->u_detail_dist >= 0)
        glUniform1f(p->u_detail_dist, cvar_get("r_terr_detail_dist"));
    if (p->u_simple >= 0)
        glUniform1f(p->u_simple, cvar_get("r_simple_render"));
    // Surfaces (splat): mask on unit 10, layers on 12..15
    if (p->u_splat_on >= 0)
        glUniform1f(p->u_splat_on, splat_have() ? 1.0f : 0.0f);
    if (splat_have()) {
        glActiveTexture(GL_TEXTURE10);
        glBindTexture(GL_TEXTURE_2D, splat_texture());
        for (int i = 0; i < SPLAT_LAYERS; i++) {
            if (!p->tex_splat_layer[i]) continue;
            glActiveTexture(GL_TEXTURE12 + i);
            glBindTexture(GL_TEXTURE_2D, p->tex_splat_layer[i]);
        }
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, p->tex_base);     // ground (unit 0)
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, p->tex_norm);     // normals (unit 1)
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, p->tex_height);   // heights (unit 2)
    glActiveTexture(GL_TEXTURE0);
    terrain_draw_tiles(p, rc);           // distance-based LOD + frustum culling
}

void terrain_cleanup(SceneNode *n)
{
    if (!n) return;   // safe during partial scene construction (error path)
    TerrainParams *p = n->params;
    resources_program_release(s_shadow_prog);
    s_shadow_prog = 0;
    if (s_terr == p) s_terr = NULL;
    resources_program_release(p->prog);
    glDeleteBuffers(1, &p->vbo);
    glDeleteBuffers(1, &p->ibo);
    glDeleteVertexArrays(1, &p->vao);
    glDeleteTextures(1, &p->tex_base);   // computed texture — not from the cache
    for (int i = 0; i < SPLAT_LAYERS; i++)
        if (p->tex_splat_layer[i]) {
            resources_texture_release(p->tex_splat_layer[i]);
            p->tex_splat_layer[i] = 0;
        }
    free(p->tiles);
    if (s_static_tex) {
        glDeleteTextures(1, &s_static_tex);
        s_static_tex = 0;
        s_static_baked = 0;
    }
}

int terrain_tile_count(void)
{
    return s_terr ? s_terr->tiles_n : 0;
}

// --- Static shadows (r_shadows_mode 2) ---------------------------------
// (constants and statics are at the top of the file)
static float static_ground(const WorldState *w, float x, float z)
{
    return sample_height(w, x, z);
}

void terrain_shadows_bake(const WorldState *w, SunParams *sun)
{
    if (!sun || sun->light_count < 1) return;
    // Ray to the sun (the first directional light)
    const float *ld = sun->light_dir[0];
    float len = sqrtf(ld[0] * ld[0] + ld[1] * ld[1] + ld[2] * ld[2]);
    if (len < 0.001f) return;
    float dx = ld[0] / len, dy = ld[1] / len, dz = ld[2] / len;

    unsigned char *tex = malloc((size_t)STATIC_SHADOW_W * STATIC_SHADOW_H);
    if (!tex) return;
    shobjs_build(w);   // object cylinders: once, sorted by x
    Uint64 t0 = SDL_GetTicks();
    int shadow_px = 0, total_px = 0;
    for (int j = 0; j < STATIC_SHADOW_H; j++) {
        for (int i = 0; i < STATIC_SHADOW_W; i++) {
            float x = ((float)i + 0.5f) / STATIC_SHADOW_W * w->world_w;
            float z = ((float)j + 0.5f) / STATIC_SHADOW_H * w->world_d;
            float y = static_ground(w, x, z);
            int shadow = 0;
            // 1) terrain: coarse step up to 480 m
            for (int s = 1; s <= STATIC_SHADOW_STEPS; s++) {
                float tt = s * STATIC_SHADOW_STEP;
                float px = x + dx * tt;
                float pz = z + dz * tt;
                float py = y + dy * tt;
                if (px < 0 || pz < 0 || px > w->world_w || pz > w->world_d)
                    break;                       // left the map — lit
                if (static_ground(w, px, pz) > py + 1.0f) { shadow = 1; break; }
            }
            // 2) objects: FINE step (4 m), up to 240 m — continuous shadows
            //    from canopies (the coarse step "stepped over" narrow trees)
            if (!shadow) {
                for (int s = 1; s <= STATIC_OBJ_STEPS; s++) {
                    float tt = s * STATIC_OBJ_STEP;
                    float px = x + dx * tt;
                    float pz = z + dz * tt;
                    float py = y + dy * tt;
                    if (px < 0 || pz < 0 || px > w->world_w || pz > w->world_d)
                        break;
                    if (ray_hits_object(px, py, pz)) { shadow = 1; break; }
                }
            }
            // In OpenGL the first data row of glTexImage2D is v=0 (the BOTTOM of the texture).
            // We want z=0 to land in v=0 (UV in the shader = z/world_d),
            // so we write WITHOUT inversion (inversion here mirrored the shadows
            // along Z — "toward the sun").
            tex[j * STATIC_SHADOW_W + i] = shadow ? 64 : 255;   // shadow ~25%
            total_px++;
            if (shadow) shadow_px++;
        }
    }
    SDL_Log("Static shadows baked (%dx%d) in %llu ms: %d/%d px in shadow (%.1f%%)",
            STATIC_SHADOW_W, STATIC_SHADOW_H,
            (unsigned long long)(SDL_GetTicks() - t0),
            shadow_px, total_px,
            total_px ? 100.0 * shadow_px / total_px : 0.0);

    // Debug: dump the shadow map to a PPM (FR_SHADOW_DUMP=1)
    if (getenv("FR_SHADOW_DUMP")) {
        FILE *f = fopen("/tmp/static_shadow.ppm", "wb");
        if (f) {
            fprintf(f, "P6\n%d %d\n255\n", STATIC_SHADOW_W, STATIC_SHADOW_H);
            for (int j = STATIC_SHADOW_H - 1; j >= 0; j--)
                for (int i = 0; i < STATIC_SHADOW_W; i++) {
                    unsigned char v = tex[j * STATIC_SHADOW_W + i];
                    fputc(v, f); fputc(v, f); fputc(v, f);
                }
            fclose(f);
            SDL_Log("Static shadow dump: /tmp/static_shadow.ppm");
        }
    }

    // Texture: R8, nearest, clamp
    if (!s_static_tex)
        glGenTextures(1, &s_static_tex);
    glBindTexture(GL_TEXTURE_2D, s_static_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, STATIC_SHADOW_W, STATIC_SHADOW_H,
                 0, GL_RED, GL_UNSIGNED_BYTE, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    free(tex);
    s_static_baked = 1;
}

// ===========================================================================
//  Baked light (offline). Computed ONCE in make mapbake (FR_BAKE),
//  written to map/baked/, the engine only reads it. Dynamics (fauna/particles/
//  glb_anim) do NOT participate in baking — occluders are static only
//  (ShadowObj cylinders from mapdata_object, same as for shadows).
// ===========================================================================
// Offline lightmap resolution: 2 texels/m (was 1) — sharper tree/field shadows.
// Decoupled from STATIC_SHADOW_W/H (the runtime R8 fallback stays 1 texel/m).
#define BAKE_LM_W 4096
#define BAKE_LM_H 2304

// Terrain normal from heightmap finite differences (matches the geometry).
static void bl_normal(const WorldState *w, float x, float z, float N[3])
{
    float e = 1.0f;
    float hl = sample_height(w, x - e, z), hr = sample_height(w, x + e, z);
    float hd = sample_height(w, x, z - e), hu = sample_height(w, x, z + e);
    float nx = hl - hr, ny = 2.0f * e, nz = hd - hu;
    float l = sqrtf(nx * nx + ny * ny + nz * nz);
    if (l < 1e-6f) { N[0] = 0; N[1] = 1; N[2] = 0; return; }
    N[0] = nx / l; N[1] = ny / l; N[2] = nz / l;
}

// Ray from a point along (dx,dy,dz): does it hit terrain/a static object
// within maxd? Leaving the map or going above the world means sky (0, not blocked).
static int bl_ray_blocked(const WorldState *w, float x, float y, float z,
                          float dx, float dy, float dz,
                          float maxd, float step)
{
    int n = (int)(maxd / step);
    for (int s = 1; s <= n; s++) {
        float tt = s * step;
        float px = x + dx * tt, py = y + dy * tt, pz = z + dz * tt;
        if (px < 0 || pz < 0 || px > w->world_w || pz > w->world_d) return 0;
        if (py > w->max_height + 6.0f) return 0;
        if (static_ground(w, px, pz) > py + 0.5f) return 1;
        if (ray_hits_object(px, py, pz)) return 1;
    }
    return 0;
}

// Wheat field (world rectangle) — a low semi-transparent occluder layer.
#define BAKE_WHEAT_H     1.4f    // ear height above the ground, m
#define BAKE_WHEAT_TRANS 0.72f   // light transmission per march step through the ears
                                 // (the sun is low ~11°, the ray passes through the ears
                                 //  along a long slanted path ~5 steps → field V ~0.3;
                                 //  otherwise the field shadow drowns in the clamp of a bright lightmap)
static int bl_in_field(const WorldState *w, float x, float z)
{
    return w->field_w > 1.0f && w->field_d > 1.0f &&
           x >= w->field_x0 && x <= w->field_x0 + w->field_w &&
           z >= w->field_z0 && z <= w->field_z0 + w->field_d;
}

// Sun ray for BAKING: two-phase step, returns TRANSMITTANCE [0..1].
// Previously a single 8 m step skipped tree canopies (radius ~4.5 m) between
// samples — hence "holes" in the shadow. Now near the point (up to 160 m, where objects
// and local terrain live) the step is 1.5 m, farther — 10 m up to 480 m (distant ridges).
// The start offset (t0) removes self-hits and stitches neighbouring texels'
// samples, killing "banding". Terrain/canopy is a hard block (0); wheat is a
// soft filter (the field's shadow on the ground), a product of transmittances over the layer.
static float bl_sun_ray_vis(const WorldState *w, float x, float y, float z,
                            float dx, float dy, float dz, float t0)
{
    float tt = 1.0f + t0;
    const float breaks[2] = { 160.0f, 480.0f };
    const float steps[2]  = { 1.5f, 10.0f };
    float trans = 1.0f;
    for (int phase = 0; phase < 2; phase++) {
        for (; tt <= breaks[phase]; tt += steps[phase]) {
            float px = x + dx * tt, py = y + dy * tt, pz = z + dz * tt;
            if (px < 0 || pz < 0 || px > w->world_w || pz > w->world_d) return trans;
            if (py > w->max_height + 6.0f) return trans;
            float g = static_ground(w, px, pz);
            if (g > py + 0.5f) return 0.0f;                 // terrain — opaque
            if (ray_hits_object(px, py, pz)) return 0.0f;   // canopy — opaque
            // Wheat: a point inside the field and BELOW the ear tops filters the
            // light (a soft field shadow). Only in the near phase (1.5 m step).
            if (phase == 0 && py < g + BAKE_WHEAT_H && bl_in_field(w, px, pz)) {
                trans *= BAKE_WHEAT_TRANS;
                if (trans < 0.03f) return 0.0f;
            }
        }
    }
    return trans;
}

// Fractional sun visibility [0..1]: supersampled rays in a narrow cone around
// the sun direction (a soft penumbra) + a different start offset per ray.
// Binary 0/1 gave jagged "brush-stroke" edges; averaging over the cone +
// post-blurring the field yield a smooth stylized shadow matching the geometry.
#define BAKE_SUN_SAMPLES 8
#define BAKE_SUN_CONE    0.055f   // cone half-angle (~3.1°) — penumbra width
static float bl_sun_vis(const WorldState *w, float x, float y, float z,
                        float dx, float dy, float dz)
{
    // Orthonormal basis (u,v) perpendicular to the sun direction.
    float ux, uy, uz;
    if (fabsf(dy) < 0.99f) { ux = dz; uy = 0.0f; uz = -dx; }
    else { ux = 1.0f; uy = 0.0f; uz = 0.0f; }
    float ul = sqrtf(ux*ux + uy*uy + uz*uz); ux/=ul; uy/=ul; uz/=ul;
    float vx = dy*uz - dz*uy, vy = dz*ux - dx*uz, vz = dx*uy - dy*ux;
    float acc = 0.0f;
    for (int s = 0; s < BAKE_SUN_SAMPLES; s++) {
        // Cone disk via Vogel sampling (golden angle) — uniform, no clumping.
        float ang = 2.399963f * (float)s;
        float rr  = BAKE_SUN_CONE * sqrtf(((float)s + 0.5f) / BAKE_SUN_SAMPLES);
        float jx = cosf(ang) * rr, jy = sinf(ang) * rr;
        float rx = dx + ux*jx + vx*jy;
        float ry = dy + uy*jx + vy*jy;
        float rz = dz + uz*jx + vz*jy;
        float rl = sqrtf(rx*rx + ry*ry + rz*rz); rx/=rl; ry/=rl; rz/=rl;
        acc += bl_sun_ray_vis(w, x, y, z, rx, ry, rz, (float)s * 0.3f);
    }
    return acc / (float)BAKE_SUN_SAMPLES;
}

// Sky AO: the fraction of upper-hemisphere rays (biased toward N) that reach the sky.
// Local occlusion under canopies / near slopes (radius ~26 m). 1 = open.
static float bl_sky_ao(const WorldState *w, float x, float y, float z,
                       const float N[3])
{
    // 9 fixed directions: zenith + 8 around a ring at 45°.
    static const float ring[8][2] = {
        { 1, 0 }, { 0.707f, 0.707f }, { 0, 1 }, { -0.707f, 0.707f },
        { -1, 0 }, { -0.707f, -0.707f }, { 0, -1 }, { 0.707f, -0.707f }
    };
    int open = 0, total = 0;
    float dirs[9][3];
    dirs[0][0] = 0; dirs[0][1] = 1; dirs[0][2] = 0;            // zenith
    for (int k = 0; k < 8; k++) {
        dirs[k + 1][0] = ring[k][0] * 0.6f;
        dirs[k + 1][1] = 0.8f;                                 // 45°+ tilt
        dirs[k + 1][2] = ring[k][1] * 0.6f;
    }
    for (int k = 0; k < 9; k++) {
        float dx = dirs[k][0], dy = dirs[k][1], dz = dirs[k][2];
        if (dx * N[0] + dy * N[1] + dz * N[2] <= 0.05f) continue; // below the horizon
        total++;
        if (!bl_ray_blocked(w, x, y + 0.4f, z, dx, dy, dz, 26.0f, 2.0f)) open++;
    }
    return total ? (float)open / (float)total : 1.0f;
}

// Per-frame light state for baking (from RendererContext).
typedef struct {
    const WorldState *w;
    float *E;                    // BAKE_LM_W*BAKE_LM_H*3 float (luxel)
    const float *V;              // BAKE_LM_W*BAKE_LM_H — fractional sun visibility
    float amb[3], sky[3], grd[3];
    float ldir[SUN_MAX_LIGHTS][3], lcol[SUN_MAX_LIGHTS][3], lint[SUN_MAX_LIGHTS];
    int lcount;
} BakeLight;

// Sun visibility pass: fractional shadow [0..1] with supersampling (see bl_sun_vis).
// Computed SEPARATELY from lighting so the field can be blurred (a soft
// penumbra) before compositing — otherwise the shadow comes out jagged ("brush-stroke").
static void bake_sunvis_rows(int j0, int j1, void *ud)
{
    BakeLight *b = (BakeLight *)ud;
    const WorldState *w = b->w;
    float *V = (float *)b->V;
    float dx = 0, dy = 1, dz = 0, ln = 0;
    if (b->lcount > 0) {
        dx = b->ldir[0][0]; dy = b->ldir[0][1]; dz = b->ldir[0][2];
        ln = sqrtf(dx*dx + dy*dy + dz*dz);
        if (ln > 1e-4f) { dx/=ln; dy/=ln; dz/=ln; }
    }
    for (int j = j0; j < j1; j++) {
        for (int i = 0; i < BAKE_LM_W; i++) {
            float vis = 1.0f;
            if (b->lcount > 0 && ln > 1e-4f) {
                float x = ((float)i + 0.5f) / BAKE_LM_W * w->world_w;
                float z = ((float)j + 0.5f) / BAKE_LM_H * w->world_d;
                float y = static_ground(w, x, z);
                vis = bl_sun_vis(w, x, y, z, dx, dy, dz);
            }
            V[(size_t)j * BAKE_LM_W + i] = vis;
        }
    }
}

// Direct pass: ambient (hemisphere) * AO + direct light from each directional
// source; sun visibility (0) applies only to source 0 (as in the shader).
static void bake_direct_rows(int j0, int j1, void *ud)
{
    BakeLight *b = (BakeLight *)ud;
    const WorldState *w = b->w;
    for (int j = j0; j < j1; j++) {
        for (int i = 0; i < BAKE_LM_W; i++) {
            float x = ((float)i + 0.5f) / BAKE_LM_W * w->world_w;
            float z = ((float)j + 0.5f) / BAKE_LM_H * w->world_d;
            float y = static_ground(w, x, z);
            float N[3]; bl_normal(w, x, z, N);
            float ao = bl_sky_ao(w, x, y, z, N);
            // Hemispheric ambient (like terrain.fs), dimmed by AO.
            float t = N[1] * 0.5f + 0.5f;
            float aok = 0.35f + 0.65f * ao;
            float light[3];
            for (int c = 0; c < 3; c++) {
                float amb = (b->grd[c] + (b->sky[c] - b->grd[c]) * t) * b->amb[c];
                light[c] = amb * aok;
            }
            // Sun visibility — fractional, from the blurred field (soft penumbra).
            float shadow = b->V ? b->V[(size_t)j * BAKE_LM_W + i] : 1.0f;
            // shadow chokes ambient down to 1/4 (like mix(0.25,1,shadow) in the shader)
            float ambmul = 0.25f + 0.75f * shadow;
            for (int c = 0; c < 3; c++) light[c] *= ambmul;
            // Direct light: each directional source.
            for (int li = 0; li < b->lcount; li++) {
                float dx = b->ldir[li][0], dy = b->ldir[li][1], dz = b->ldir[li][2];
                float ln = sqrtf(dx*dx + dy*dy + dz*dz);
                if (ln < 1e-4f) continue;
                dx/=ln; dy/=ln; dz/=ln;
                float diff = N[0]*dx + N[1]*dy + N[2]*dz; if (diff < 0) diff = 0;
                float sh = (li == 0) ? shadow : 1.0f;
                float k = b->lint[li] * (0.55f + 0.45f * diff) * sh;
                for (int c = 0; c < 3; c++) light[c] += b->lcol[li][c] * k;
            }
            float *o = &b->E[((size_t)j * BAKE_LM_W + i) * 3];
            o[0] = light[0]; o[1] = light[1]; o[2] = light[2];
        }
    }
}

// Diffuse bounce: gather light from a ring of neighbours (a src snapshot),
// tint it with the ground color (bounce tint) and add it to the receiver (dst). One
// pass = "the ground color bleeds into shadow" — stylized GI without tracing.
typedef struct {
    const float *src;   // luxel snapshot before the bounce
    float *dst;         // receiver (= a copy of src + indirect)
    float tint[3];      // bounced-light tint (ground color * gain)
} BounceJob;

static void bake_bounce_rows(int j0, int j1, void *ud)
{
    BounceJob *b = (BounceJob *)ud;
    // Sample rings (in texels): 32 neighbours, weight falls off with distance.
    static const int rad[4] = { 5, 11, 23, 45 };
    static const float ring[8][2] = {
        { 1, 0 }, { 0.707f, 0.707f }, { 0, 1 }, { -0.707f, 0.707f },
        { -1, 0 }, { -0.707f, -0.707f }, { 0, -1 }, { 0.707f, -0.707f }
    };
    for (int j = j0; j < j1; j++) {
        for (int i = 0; i < BAKE_LM_W; i++) {
            float acc[3] = { 0, 0, 0 }, wsum = 0.0f;
            for (int r = 0; r < 4; r++) {
                float wgt = 1.0f / (1.0f + rad[r] / 12.0f);
                for (int k = 0; k < 8; k++) {
                    int sx = i + (int)(ring[k][0] * rad[r]);
                    int sy = j + (int)(ring[k][1] * rad[r]);
                    if (sx < 0) sx = 0; else if (sx >= BAKE_LM_W) sx = BAKE_LM_W - 1;
                    if (sy < 0) sy = 0; else if (sy >= BAKE_LM_H) sy = BAKE_LM_H - 1;
                    const float *s = &b->src[((size_t)sy * BAKE_LM_W + sx) * 3];
                    acc[0] += s[0] * wgt; acc[1] += s[1] * wgt; acc[2] += s[2] * wgt;
                    wsum += wgt;
                }
            }
            const float *self = &b->src[((size_t)j * BAKE_LM_W + i) * 3];
            float *o = &b->dst[((size_t)j * BAKE_LM_W + i) * 3];
            for (int c = 0; c < 3; c++)
                o[c] = self[c] + b->tint[c] * (wsum > 0 ? acc[c] / wsum : 0.0f);
        }
    }
}

// Create the map/baked directory (idempotent).
static void bake_ensure_dir(void)
{
#ifdef _WIN32
    _mkdir("map"); _mkdir("map\\baked");
#else
    mkdir("map", 0755); mkdir("map/baked", 0755);
#endif
}

// --- Irradiance probe grid (for dynamic objects) ------------------
#define BAKE_PROBE_NX 64
#define BAKE_PROBE_NZ 36
#define BAKE_PROBE_NY 8

// Writes probes.bin: header + nx*ny*nz*4 float (ambient RGB + sun
// visibility). Each probe is a 3D point; an object takes a trilinear sample.
static void bake_probes(const WorldState *w, const BakeLight *b)
{
    int nx = BAKE_PROBE_NX, ny = BAKE_PROBE_NY, nz = BAKE_PROBE_NZ;
    float y0 = w->min_height, y1 = w->max_height + 30.0f;
    float *P = calloc((size_t)nx * ny * nz * 4, sizeof(float));
    if (!P) return;
    float sdx = 0, sdy = 1, sdz = 0;
    if (b->lcount > 0) {
        float ln = sqrtf(b->ldir[0][0]*b->ldir[0][0] + b->ldir[0][1]*b->ldir[0][1]
                       + b->ldir[0][2]*b->ldir[0][2]);
        if (ln > 1e-4f) { sdx = b->ldir[0][0]/ln; sdy = b->ldir[0][1]/ln;
                          sdz = b->ldir[0][2]/ln; }
    }
    float upN[3] = { 0, 1, 0 };
    for (int iz = 0; iz < nz; iz++)
    for (int iy = 0; iy < ny; iy++)
    for (int ix = 0; ix < nx; ix++) {
        float x = (nx > 1) ? (float)ix / (nx - 1) * w->world_w : 0;
        float z = (nz > 1) ? (float)iz / (nz - 1) * w->world_d : 0;
        float y = y0 + (ny > 1 ? (float)iy / (ny - 1) : 0) * (y1 - y0);
        float g = static_ground(w, x, z);
        if (y < g + 0.5f) y = g + 0.5f;            // probe not underground
        float ao = bl_sky_ao(w, x, y, z, upN);
        float aok = 0.35f + 0.65f * ao;
        float sunv = (b->lcount > 0) ? bl_sun_vis(w, x, y, z, sdx, sdy, sdz) : 1.0f;
        float *o = &P[(((size_t)iz * ny + iy) * nx + ix) * 4];
        for (int c = 0; c < 3; c++)               // omnidirectional ambient (t=0.5)
            o[c] = (b->grd[c] + (b->sky[c] - b->grd[c]) * 0.5f) * b->amb[c] * aok;
        o[3] = sunv;
    }
    FILE *f = fopen("map/baked/probes.bin", "wb");
    if (f) {
        int32_t hdr[4] = { 0x504C5246 /*'FRLP'*/, 1, 0, 0 };  // magic, ver
        int32_t dims[3] = { nx, ny, nz };
        float bounds[4] = { w->world_w, w->world_d, y0, y1 };
        fwrite(hdr, sizeof(int32_t), 2, f);
        fwrite(dims, sizeof(int32_t), 3, f);
        fwrite(bounds, sizeof(float), 4, f);
        fwrite(P, sizeof(float), (size_t)nx * ny * nz * 4, f);
        fclose(f);
        SDL_Log("Baked probes: %dx%dx%d -> map/baked/probes.bin", nx, ny, nz);
    }
    // Keep the grid in memory for a preview in this same process.
    free(s_probes);
    s_probes = P; s_pnx = nx; s_pny = ny; s_pnz = nz;
    s_py0 = y0; s_py1 = y1; s_pwx = w->world_w; s_pwz = w->world_d;
}

void terrain_light_bake(const WorldState *w, SunParams *sun,
                        const RendererContext *rc)
{
    if (!w || !rc) return;
    shobjs_build(w);   // static occluders (dynamics excluded)

    BakeLight b; memset(&b, 0, sizeof b);
    b.w = w;
    for (int c = 0; c < 3; c++) {
        b.amb[c] = rc->ambient[c];
        b.sky[c] = rc->sky_color[c];
        b.grd[c] = rc->ground_color[c];
    }
    b.lcount = rc->light_count < SUN_MAX_LIGHTS ? rc->light_count : SUN_MAX_LIGHTS;
    for (int i = 0; i < b.lcount; i++) {
        for (int c = 0; c < 3; c++) {
            b.ldir[i][c] = rc->light_dir[i][c];
            b.lcol[i][c] = rc->light_color[i][c];
        }
        b.lint[i] = rc->light_intensity[i];
    }
    (void)sun;

    size_t n = (size_t)BAKE_LM_W * BAKE_LM_H * 3;
    size_t np = (size_t)BAKE_LM_W * BAKE_LM_H;
    float *E = malloc(n * sizeof(float));
    float *E2 = malloc(n * sizeof(float));
    float *V = malloc(np * sizeof(float));      // fractional sun visibility
    float *V2 = malloc(np * sizeof(float));     // same, after blurring
    unsigned char *rgb = malloc((size_t)BAKE_LM_W * BAKE_LM_H * 3);
    if (!E || !E2 || !V || !V2 || !rgb) {
        free(E); free(E2); free(V); free(V2); free(rgb); return;
    }

    Uint64 t0 = SDL_GetTicks();

    // 1) Fractional sun visibility (cone supersampling) — as a separate field.
    b.V = V;
    jobs_parallel_for(0, BAKE_LM_H, bake_sunvis_rows, &b);
    // 2) Separable blur of the shadow field (gauss R=3) — a soft penumbra, removes
    //    the "brush-stroke" steps. Horizontal V->V2, vertical V2->V.
    {
        const int R = 3;
        float wt[4] = { 0.383f, 0.242f, 0.061f, 0.006f };  // gauss sigma~1.5
        for (int j = 0; j < BAKE_LM_H; j++)
            for (int i = 0; i < BAKE_LM_W; i++) {
                float acc = V[(size_t)j*BAKE_LM_W + i] * wt[0], ws = wt[0];
                for (int r = 1; r <= R; r++) {
                    int a = i - r < 0 ? 0 : i - r;
                    int c = i + r >= BAKE_LM_W ? BAKE_LM_W - 1 : i + r;
                    acc += (V[(size_t)j*BAKE_LM_W + a] + V[(size_t)j*BAKE_LM_W + c]) * wt[r];
                    ws += 2.0f * wt[r];
                }
                V2[(size_t)j*BAKE_LM_W + i] = acc / ws;
            }
        for (int j = 0; j < BAKE_LM_H; j++)
            for (int i = 0; i < BAKE_LM_W; i++) {
                float acc = V2[(size_t)j*BAKE_LM_W + i] * wt[0], ws = wt[0];
                for (int r = 1; r <= R; r++) {
                    int a = j - r < 0 ? 0 : j - r;
                    int c = j + r >= BAKE_LM_H ? BAKE_LM_H - 1 : j + r;
                    acc += (V2[(size_t)a*BAKE_LM_W + i] + V2[(size_t)c*BAKE_LM_W + i]) * wt[r];
                    ws += 2.0f * wt[r];
                }
                V[(size_t)j*BAKE_LM_W + i] = acc / ws;
            }
    }
    // 3) Direct light with a fractional shadow from the blurred field V.
    b.E = E;
    b.V = V;
    jobs_parallel_for(0, BAKE_LM_H, bake_direct_rows, &b);

    // One diffuse bounce (the ground color bleeds into shadow).
    BounceJob bj;
    bj.src = E; bj.dst = E2;
    for (int c = 0; c < 3; c++) bj.tint[c] = rc->ground_color[c] * 0.85f;
    jobs_parallel_for(0, BAKE_LM_H, bake_bounce_rows, &bj);

    // Clamp the result to [0,1] (terrain.fs does min(light,1) anyway) -> RGB8.
    for (size_t i = 0; i < n; i++) {
        float v = E2[i]; if (v < 0) v = 0; if (v > 1) v = 1;
        rgb[i] = (unsigned char)(v * 255.0f + 0.5f);
    }
    bake_ensure_dir();
    if (png_save("map/baked/lightmap.png", BAKE_LM_W, BAKE_LM_H, rgb) == 0)
        SDL_Log("Baked lightmap: %dx%d -> map/baked/lightmap.png in %llu ms",
                BAKE_LM_W, BAKE_LM_H,
                (unsigned long long)(SDL_GetTicks() - t0));

    // Upload to GL immediately (preview in this same process).
    if (!s_baked_lm_tex) glGenTextures(1, &s_baked_lm_tex);
    glBindTexture(GL_TEXTURE_2D, s_baked_lm_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, BAKE_LM_W, BAKE_LM_H, 0,
                 GL_RGB, GL_UNSIGNED_BYTE, rgb);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    // Sun visibility map (sunvis): the fractional field V, but at 2× lower
    // resolution (2048x1152). Mode 1 (dynamic) needs it for FAR shadows beyond the
    // CSM cascades — static tree/field shadows "load in" on the horizon.
    // Grayscale (R=G=B), written via RGB-png (grayscale is enough for the distance).
    {
        int sw = BAKE_LM_W / 2, sh = BAKE_LM_H / 2;
        unsigned char *sv = malloc((size_t)sw * sh * 3);
        if (sv) {
            for (int j = 0; j < sh; j++)
                for (int i = 0; i < sw; i++) {
                    // 2x2 box filter from the full-resolution V.
                    float a = V[(size_t)(2*j)  * BAKE_LM_W + 2*i]
                            + V[(size_t)(2*j)  * BAKE_LM_W + 2*i+1]
                            + V[(size_t)(2*j+1)* BAKE_LM_W + 2*i]
                            + V[(size_t)(2*j+1)* BAKE_LM_W + 2*i+1];
                    unsigned char c = (unsigned char)(a * 0.25f * 255.0f + 0.5f);
                    size_t o = ((size_t)j * sw + i) * 3;
                    sv[o] = sv[o+1] = sv[o+2] = c;
                }
            png_save("map/baked/sunvis.png", sw, sh, sv);
            if (!s_baked_sunvis_tex) glGenTextures(1, &s_baked_sunvis_tex);
            glBindTexture(GL_TEXTURE_2D, s_baked_sunvis_tex);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, sw, sh, 0,
                         GL_RGB, GL_UNSIGNED_BYTE, sv);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glBindTexture(GL_TEXTURE_2D, 0);
            free(sv);
        }
    }

    bake_probes(w, &b);
    free(E); free(E2); free(V); free(V2); free(rgb);
    s_baked_have = 1;
}

void terrain_baked_light_free(void)
{
    if (s_baked_lm_tex) { glDeleteTextures(1, &s_baked_lm_tex); s_baked_lm_tex = 0; }
    if (s_baked_sunvis_tex) { glDeleteTextures(1, &s_baked_sunvis_tex); s_baked_sunvis_tex = 0; }
    free(s_probes); s_probes = NULL;
    s_pnx = s_pny = s_pnz = 0;
    s_baked_have = 0;
}

int terrain_baked_light_have(void) { return s_baked_have && s_baked_lm_tex; }

int terrain_baked_sunvis_have(void) { return s_baked_sunvis_tex != 0; }

int terrain_baked_probes_have(void) { return s_probes != NULL; }

int terrain_baked_light_load(const WorldState *w)
{
    terrain_baked_light_free();
    unsigned int sz = 0;
    unsigned char *png = resources_read_file("baked/lightmap.png", &sz);
    if (!png) return 0;
    int lw, lh, ch;
    unsigned char *px = stbi_load_from_memory(png, (int)sz, &lw, &lh, &ch, 3);
    free(png);
    if (!px) return 0;
    glGenTextures(1, &s_baked_lm_tex);
    glBindTexture(GL_TEXTURE_2D, s_baked_lm_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, lw, lh, 0,
                 GL_RGB, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    stbi_image_free(px);
    s_baked_have = 1;
    SDL_Log("Baked lightmap loaded (%dx%d)", lw, lh);

    // Sun visibility map (for distant shadows in mode 1). Optional.
    unsigned int svsz = 0;
    unsigned char *svpng = resources_read_file("baked/sunvis.png", &svsz);
    if (svpng) {
        int vw, vh, vch;
        unsigned char *vpx = stbi_load_from_memory(svpng, (int)svsz, &vw, &vh, &vch, 3);
        free(svpng);
        if (vpx) {
            glGenTextures(1, &s_baked_sunvis_tex);
            glBindTexture(GL_TEXTURE_2D, s_baked_sunvis_tex);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, vw, vh, 0,
                         GL_RGB, GL_UNSIGNED_BYTE, vpx);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glBindTexture(GL_TEXTURE_2D, 0);
            stbi_image_free(vpx);
            SDL_Log("Baked sunvis loaded (%dx%d)", vw, vh);
        }
    }

    // Probe grid (non-critical: without it objects are just lit as before).
    unsigned int psz = 0;
    unsigned char *pb = resources_read_file("baked/probes.bin", &psz);
    if (pb && psz >= 36) {
        const unsigned char *p = pb;
        int32_t magic, ver, dims[3]; float bounds[4];
        memcpy(&magic, p, 4); p += 4;
        memcpy(&ver, p, 4); p += 4;
        memcpy(dims, p, 12); p += 12;
        memcpy(bounds, p, 16); p += 16;
        size_t need = (size_t)dims[0] * dims[1] * dims[2] * 4;
        if (magic == 0x504C5246 && ver == 1 &&
            dims[0] > 0 && dims[0] <= 4096 &&
            dims[1] > 0 && dims[1] <= 4096 &&
            dims[2] > 0 && dims[2] <= 4096 &&
            psz >= 36 + need * sizeof(float)) {
            s_probes = malloc(need * sizeof(float));
            if (s_probes) {
                memcpy(s_probes, p, need * sizeof(float));
                s_pnx = dims[0]; s_pny = dims[1]; s_pnz = dims[2];
                s_pwx = bounds[0]; s_pwz = bounds[1];
                s_py0 = bounds[2]; s_py1 = bounds[3];
                SDL_Log("Baked probes loaded (%dx%dx%d)", s_pnx, s_pny, s_pnz);
            }
        }
    }
    free(pb);
    (void)w;
    return 1;
}

void terrain_baked_probe(float x, float y, float z, float out[4])
{
    out[0] = out[1] = out[2] = 0.5f; out[3] = 1.0f;   // neutral without a grid
    if (!s_probes || s_pnx < 2 || s_pnz < 2) return;
    float fx = (x / (s_pwx > 0 ? s_pwx : 1)) * (s_pnx - 1);
    float fz = (z / (s_pwz > 0 ? s_pwz : 1)) * (s_pnz - 1);
    float fy = (s_py1 > s_py0) ? (y - s_py0) / (s_py1 - s_py0) * (s_pny - 1) : 0;
    if (fx < 0) fx = 0;
    if (fx > s_pnx - 1) fx = s_pnx - 1;
    if (fz < 0) fz = 0;
    if (fz > s_pnz - 1) fz = s_pnz - 1;
    if (fy < 0) fy = 0;
    if (fy > s_pny - 1) fy = s_pny - 1;
    int x0 = (int)fx, y0i = (int)fy, z0 = (int)fz;
    int x1 = x0 < s_pnx - 1 ? x0 + 1 : x0;
    int y1i = y0i < s_pny - 1 ? y0i + 1 : y0i;
    int z1 = z0 < s_pnz - 1 ? z0 + 1 : z0;
    float tx = fx - x0, ty = fy - y0i, tz = fz - z0;
    for (int c = 0; c < 4; c++) {
        #define PB(ix,iy,iz) s_probes[(((size_t)(iz)*s_pny+(iy))*s_pnx+(ix))*4+c]
        float c00 = PB(x0,y0i,z0) + (PB(x1,y0i,z0) - PB(x0,y0i,z0)) * tx;
        float c10 = PB(x0,y1i,z0) + (PB(x1,y1i,z0) - PB(x0,y1i,z0)) * tx;
        float c01 = PB(x0,y0i,z1) + (PB(x1,y0i,z1) - PB(x0,y0i,z1)) * tx;
        float c11 = PB(x0,y1i,z1) + (PB(x1,y1i,z1) - PB(x0,y1i,z1)) * tx;
        #undef PB
        float c0 = c00 + (c10 - c00) * ty;
        float c1 = c01 + (c11 - c01) * ty;
        out[c] = c0 + (c1 - c0) * tz;
    }
}


// sky visibility texture: RG8. R = 255 = open sky
// (rain/snow reaches), G = 255 = under a roof (dry).
#define WETNESS_MASK_W 512
#define WETNESS_MASK_H 512
#define WETNESS_MASK_MAX_H 500.0f   // max ray height upward
#define WETNESS_MASK_STEPS 256      // ray steps

static GLuint s_wetness_mask_tex = 0;
static int s_wetness_mask_baked = 0;

// Ray upward: does it hit a tree canopy? Same as ray_hits_object, but the ray
// points straight up (0, 1, 0).
static int ray_hits_object_up(float x, float y, float z)
{
    if (s_shobjs_n == 0) return 0;
    // object range with |ox| < 120 m
    int lo = 0, hi = s_shobjs_n - 1, first = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (s_shobjs[mid].x >= x - 120.0f) { first = mid; hi = mid - 1; }
        else lo = mid + 1;
    }
    if (first < 0) return 0;
    for (int k = first; k < s_shobjs_n; k++) {
        const ShadowObj *o = &s_shobjs[k];
        if (o->x > x + 120.0f) break;
        float ddx = x - o->x;
        float ddz = z - o->z;
        if (ddx * ddx + ddz * ddz > o->r * o->r) continue;
        if (y < o->base - 1.0f || y > o->base + o->h + 1.0f) continue;
        return 1;
    }
    return 0;
}

// One block of wetness-mask rows for the job pool (see jobs.h). Data is read-only
// (s_shobjs is built, w->heights ready) — safe to parallelize.
typedef struct {
    const WorldState *w;
    unsigned char *tex;
    SDL_AtomicInt dry, total;
} WetnessJob;

static void wetness_rows(int i0, int i1, void *ud)
{
    WetnessJob *j = (WetnessJob *)ud;
    const WorldState *w = j->w;
    unsigned char *tex = j->tex;
    int dry_local = 0, total_local = 0;
    for (int jj = i0; jj < i1; jj++) {
        for (int i = 0; i < WETNESS_MASK_W; i++) {
            float x = ((float)i + 0.5f) / WETNESS_MASK_W * w->world_w;
            float z = ((float)jj + 0.5f) / WETNESS_MASK_H * w->world_d;
            float y = static_ground(w, x, z) + 0.1f; // slightly above the ground
            int dry = 0;

            // Ray upward: look for overhanging objects
            for (int s = 1; s <= WETNESS_MASK_STEPS; s++) {
                float tt = s * (WETNESS_MASK_MAX_H / WETNESS_MASK_STEPS);
                float py = y + tt;
                if (ray_hits_object_up(x, py, z)) { dry = 1; break; }
            }

            // In OpenGL the first row = v=0 (texture BOTTOM), z=0 -> v=0.
            // R = open sky (rain/snow falls), G = under a roof (dry).
            size_t k = ((size_t)jj * WETNESS_MASK_W + i) * 2;
            tex[k]     = dry ? 0 : 255;
            tex[k + 1] = dry ? 255 : 0;
            total_local++;
            if (dry) dry_local++;
        }
    }
    SDL_AddAtomicInt(&j->dry, dry_local);
    SDL_AddAtomicInt(&j->total, total_local);
}

void terrain_wetness_mask_bake(const WorldState *w)
{
    if (s_wetness_mask_baked) return; // already baked this session
    shobjs_build(w);   // build the object list (canopy cylinders)

    unsigned char *tex = malloc((size_t)WETNESS_MASK_W * WETNESS_MASK_H * 2);
    if (!tex) return;

    Uint64 t0 = SDL_GetTicks();

    // Parallel by rows: 512 rows * 256 ray steps — pure CPU,
    // no GL and no shared writes (the texture is split by rows without overlap)
    WetnessJob j;
    j.w = w;
    j.tex = tex;
    SDL_SetAtomicInt(&j.dry, 0);
    SDL_SetAtomicInt(&j.total, 0);
    jobs_parallel_for(0, WETNESS_MASK_H, wetness_rows, &j);
    int dry_px = SDL_GetAtomicInt(&j.dry), total_px = SDL_GetAtomicInt(&j.total);
    SDL_Log("Wetness mask baked (%dx%d) in %llu ms: %d/%d px dry (%.1f%%)",
            WETNESS_MASK_W, WETNESS_MASK_H,
            (unsigned long long)(SDL_GetTicks() - t0),
            dry_px, total_px,
            total_px ? 100.0 * dry_px / total_px : 0.0);

    if (!s_wetness_mask_tex)
        glGenTextures(1, &s_wetness_mask_tex);
    glBindTexture(GL_TEXTURE_2D, s_wetness_mask_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, WETNESS_MASK_W, WETNESS_MASK_H,
                 0, GL_RG, GL_UNSIGNED_BYTE, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    free(tex);
    s_wetness_mask_baked = 1;
}

int terrain_wetness_mask_have(void)
{
    return s_wetness_mask_baked && s_wetness_mask_tex;
}

GLuint terrain_wetness_mask_tex(void)
{
    return s_wetness_mask_tex;
}

int terrain_snow_mask_have(void)
{
    return terrain_wetness_mask_have();
}

GLuint terrain_snow_mask_tex(void)
{
    return s_wetness_mask_tex;
}

int terrain_shadows_have(void)
{
    return s_static_baked && s_static_tex;
}
