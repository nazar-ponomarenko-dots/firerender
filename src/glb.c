// ============================================================================
//  glb.c — binary glTF 2.0 (.glb): parser + GlbInstance3D node (see glb.h)
// ----------------------------------------------------------------------------
//  Two layers:
//    glb_parse_file() — CPU only: chunks, hierarchy, baking into world
//                       coordinates, grouping by material, albedo decode.
//                       Does not touch GL — called from the selftest/headless.
//    glb_make_node()   — builds GL buffers and textures, creates the instance node.
//
//  RENDER RULE: the engine is PSEUDO-PBR 2004 (like HL2/Source, Far Cry 1).
//  Real GLB PBR data is REFORMATTED into our material type (material.h):
//    baseColorFactor -> tint;      alphaMode -> blend/alpha
//    roughness/metal -> specular (smoothness);
//    emissiveFactor  -> added to tint;     normalTexture -> ignored.
// ============================================================================
#include "glb.h"

#include "events.h"
#include "json.h"
#include "render_ctx.h"
#include "frustum.h"
#include "resources.h"
#include "shader.h"
#include "snowmask.h"
#include "stb_image.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

// --- glTF components ----------------------------------------------------------
#define C_BYTE 5120
#define C_UBYTE 5121
#define C_SHORT 5122
#define C_USHORT 5123
#define C_UINT 5125
#define C_FLOAT 5126

// fourcc of the GLB chunks: little-endian "JSON" and "BIN\0".
#define GLB_CHUNK_JSON 0x4E4F534A
#define GLB_CHUNK_BIN  0x004E4942

static int comp_size(int c)
{
    switch (c) {
        case C_BYTE: case C_UBYTE: return 1;
        case C_SHORT: case C_USHORT: return 2;
        case C_UINT: case C_FLOAT: return 4;
    }
    return 0;
}

// Which TEXCOORD set the material's baseColorTexture uses (0 by default).
// glTF allows up to two UV sets per primitive: character eyes/face maps often
// live in TEXCOORD_1 — always reading only set 0 means broken textures.
static int base_uv_set(const JsonNode *mt_a, const JsonNode *tx_a, int mat)
{
    if (mat < 0 || !mt_a || mt_a->type != JSON_ARRAY) return 0;
    const JsonNode *m = json_array_get(mt_a, mat);
    const JsonNode *pbr = m ? json_find(m, "pbrMetallicRoughness") : NULL;
    const JsonNode *tex = pbr ? json_find(pbr, "baseColorTexture") : NULL;
    int ti = (int)json_num(tex, -1);
    if (ti < 0 || !tx_a || tx_a->type != JSON_ARRAY) return 0;
    const JsonNode *t = json_array_get(tx_a, ti);
    return (int)json_num(t ? json_find(t, "texCoord") : NULL, 0);
}
static int type_vec(const char *t)
{
    if (!t) return 1;
    if (strcmp(t, "VEC2") == 0) return 2;
    if (strcmp(t, "VEC3") == 0) return 3;
    if (strcmp(t, "VEC4") == 0) return 4;
    return 1;
}
static float clamp01(float x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }

// A file-supplied size as a non-negative long, or -1 if it is negative, NaN or
// larger than cap. A raw (long)json_num() cast is undefined behaviour past
// LONG_MAX, and even an in-range but huge value defeats the bounds checks below:
// they compare against the real buffer, so an out-of-range length must be
// rejected here instead of being arithmetic-verified later.
static long size_field(const JsonNode *obj, const char *key, long cap)
{
    double v = json_num(json_find(obj, key), 0);
    if (!(v >= 0) || v > (double)cap) return -1;
    return (long)v;
}

typedef struct {
    int bufferView;
    long byteOffset;
    int comp, vec, normalized;
    long count;
} GlbAcc;
typedef struct { long byteOffset, byteLength; int byteStride; } GlbBv;

// --- 4x4 matrices (column-major) -----------------------------------------------
static void mat4_identity(float m[16])
{
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}
static void mat4_mul(float out[16], const float a[16], const float b[16])
{
    float r[16];
    for (int c = 0; c < 4; c++)
        for (int rr = 0; rr < 4; rr++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a[k * 4 + rr] * b[c * 4 + k];
            r[c * 4 + rr] = s;
        }
    memcpy(out, r, sizeof r);
}
static void mat4_trs(float m[16], const JsonNode *node)
{
    const JsonNode *t = json_find(node, "translation");
    const JsonNode *q = json_find(node, "rotation");
    const JsonNode *s = json_find(node, "scale");
    float tt[3] = {0, 0, 0}, qu[4] = {0, 0, 0, 1}, sc[3] = {1, 1, 1};
    if (t && t->type == JSON_ARRAY)
        for (int i = 0; i < 3 && i < json_array_len(t); i++) tt[i] = (float)json_num(json_array_get(t, i), 0);
    if (q && q->type == JSON_ARRAY)
        for (int i = 0; i < 4 && i < json_array_len(q); i++) qu[i] = (float)json_num(json_array_get(q, i), i == 3 ? 1 : 0);
    if (s && s->type == JSON_ARRAY)
        for (int i = 0; i < 3 && i < json_array_len(s); i++) sc[i] = (float)json_num(json_array_get(s, i), 1);
    float R[16]; mat4_identity(R);
    float x = qu[0], y = qu[1], z = qu[2], w = qu[3];
    R[0] = 1 - 2 * (y * y + z * z);   R[1] = 2 * (x * y + z * w);   R[2] = 2 * (x * z - y * w);
    R[4] = 2 * (x * y - z * w);       R[5] = 1 - 2 * (x * x + z * z); R[6] = 2 * (y * z + x * w);
    R[8] = 2 * (x * z + y * w);       R[9] = 2 * (y * z - x * w);   R[10] = 1 - 2 * (x * x + y * y);
    float S[16]; mat4_identity(S); S[0] = sc[0]; S[5] = sc[1]; S[10] = sc[2];
    float RS[16]; mat4_mul(RS, R, S);
    float T[16]; mat4_identity(T); T[12] = tt[0]; T[13] = tt[1]; T[14] = tt[2];
    mat4_mul(m, T, RS);
}
static void mat4_transform_point(const float m[16], const float *p, float *o)
{
    o[0] = m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12];
    o[1] = m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13];
    o[2] = m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14];
}
static void mat4_transform_nrm(const float m[16], const float *n, float *o)
{
    float x = m[0] * n[0] + m[4] * n[1] + m[8] * n[2];
    float y = m[1] * n[0] + m[5] * n[1] + m[9] * n[2];
    float z = m[2] * n[0] + m[6] * n[1] + m[10] * n[2];
    float l = sqrtf(x * x + y * y + z * z);
    if (l > 1e-8f) { x /= l; y /= l; z /= l; } else { x = 0; y = 1; z = 0; }
    o[0] = x; o[1] = y; o[2] = z;
}

typedef struct { const JsonNode *node; float m[16]; } GlbStackItem;

static int stack_push(GlbStackItem **stack, int *n, int *cap,
                      const JsonNode *node, const float *matrix)
{
    if (*n >= *cap) {
        int next = *cap ? *cap * 2 : 64;
        GlbStackItem *p = realloc(*stack, (size_t)next * sizeof *p);
        if (!p) return 0;
        *stack = p;
        *cap = next;
    }
    (*stack)[*n].node = node;
    if (matrix) memcpy((*stack)[*n].m, matrix, 16 * sizeof(float));
    else mat4_identity((*stack)[*n].m);
    (*n)++;
    return 1;
}

// --- Vertex bucket (per material) ----------------------------------------------
typedef struct {
    int mat;                       // material index (-1 = none)
    float *verts, *nrm, *uv;
    int vcap, vn;
    unsigned int *idx;
    int icap, idxn;
} Bucket;
static Bucket *b_get(Bucket **arr, int *n, int *cap, int mat)
{
    for (int i = 0; i < *n; i++) if ((*arr)[i].mat == mat) return &(*arr)[i];
    if (*n >= *cap) {
        int next = *cap ? *cap * 2 : 16;
        Bucket *p = realloc(*arr, (size_t)next * sizeof **arr);
        if (!p) return NULL;
        *arr = p;
        *cap = next;
    }
    Bucket *b = &(*arr)[(*n)++];
    memset(b, 0, sizeof *b);
    b->mat = mat;
    return b;
}
static int b_push_v(Bucket *b, float px, float py, float pz,
                    float nx, float ny, float nz, float u, float v)
{
    if (b->vn >= b->vcap) {
        int next = b->vcap < 1 ? 4096 : (b->vcap > (1 << 28) ? 0 : b->vcap * 2);
        if (next <= 0) return 0;
        float *nv = realloc(b->verts, (size_t)next * 3 * sizeof(float));
        if (nv) b->verts = nv;
        float *nn = realloc(b->nrm, (size_t)next * 3 * sizeof(float));
        if (nn) b->nrm = nn;
        float *nu = realloc(b->uv, (size_t)next * 2 * sizeof(float));
        if (nu) b->uv = nu;
        if (!nv || !nn || !nu) return 0;
        b->vcap = next;
    }
    b->verts[3 * b->vn] = px; b->verts[3 * b->vn + 1] = py; b->verts[3 * b->vn + 2] = pz;
    b->nrm[3 * b->vn] = nx; b->nrm[3 * b->vn + 1] = ny; b->nrm[3 * b->vn + 2] = nz;
    b->uv[2 * b->vn] = u; b->uv[2 * b->vn + 1] = v;
    b->vn++;
    return 1;
}
static int b_push_i(Bucket *b, unsigned int i)
{
    if (b->idxn >= b->icap) {
        int next = b->icap < 1 ? 4096 : (b->icap > (1 << 28) ? 0 : b->icap * 2);
        if (next <= 0) return 0;
        unsigned int *p = realloc(b->idx, (size_t)next * sizeof(unsigned int));
        if (!p) return 0;
        b->idx = p;
        b->icap = next;
    }
    b->idx[b->idxn++] = i;
    return 1;
}
static void b_free(Bucket *b)
{
    free(b->verts); free(b->nrm); free(b->uv); free(b->idx);
    memset(b, 0, sizeof *b);
}

// --- Reading accessor data
// Safe index read: only unsigned integers of SCALAR type.
// Returns -1 for an invalid accessor/type, otherwise an index in [0,maxIdx].
static long read_index(const unsigned char *bin, long base, const GlbAcc *a,
                       const GlbBv *bvs, int bv_n,
                       int elem, long maxIdx)
{
    if (a->vec != 1) return -1;
    long idx = -1;
    int sz = comp_size(a->comp);
    int bv_i = a->bufferView;
    long stride = (bv_i >= 0 && bv_i < bv_n && bvs[bv_i].byteStride > 0)
        ? bvs[bv_i].byteStride : sz;
    const unsigned char *p = bin + base + (long)elem * stride;
    switch (a->comp) {
        case C_UINT:   { unsigned int v; memcpy(&v, p, 4); idx = (long)v; break; }
        case C_USHORT: { unsigned short v; memcpy(&v, p, 2); idx = (long)v; break; }
        case C_UBYTE:  { idx = (long)p[0]; break; }
        default: return -1;   // FLOAT/SHORT/BYTE are unreliable as indices → drop
    }
    if (idx < 0) return -1;
    if (maxIdx > 0 && idx >= maxIdx) return -1;   // outside the primitive's vertices
    return idx;
}
static void read_vertex(const unsigned char *bin, long base, const GlbAcc *a,
                        const GlbBv *bvs, int bv_n,
                        int elem, float *out)
{
    int sz = comp_size(a->comp);
    int bv_i = a->bufferView;
    long stride = (bv_i >= 0 && bv_i < bv_n && bvs[bv_i].byteStride > 0)
        ? bvs[bv_i].byteStride : (long)a->vec * sz;
    const unsigned char *p = bin + base + (long)elem * stride;
    float f[4] = {0};
    for (int c = 0; c < a->vec; c++) {
        switch (a->comp) {
            case C_FLOAT:  { float v; memcpy(&v, p + (long)c * 4, 4); f[c] = v; break; }
            case C_USHORT: { unsigned short v; memcpy(&v, p + (long)c * 2, 2);
                             f[c] = a->normalized ? (float)v / 65535.0f : (float)v; break; }
            case C_UBYTE:  { f[c] = a->normalized ? (float)p[(long)c * sz] / 255.0f : (float)p[(long)c * sz]; break; }
            default: f[c] = 0; break;
        }
    }
    memcpy(out, f, (size_t)a->vec * sizeof(float));
}

// Pseudo-PBR 2004: smoothness -> specular (rough 0 = shiny)
static float spec_from_rough(float rough, float metallic)
{
    float g = 1.0f - clamp01(rough);
    return 0.05f + g * (0.55f + 0.45f * clamp01(metallic));
}

// The accessor fits inside its bufferView and the BIN chunk (protection against
// a broken/malicious GLB: offsets/lengths come from JSON, checked BEFORE reading).
static int acc_fits(const GlbAcc *a, const GlbBv *bvs, int bv_n, long bin_len)
{
    if (!a || a->count <= 0) return 0;
    if (a->bufferView < 0 || a->bufferView >= bv_n) return 0;
    const GlbBv *bv = &bvs[a->bufferView];
    long sz = (long)comp_size(a->comp) * a->vec;
    if (sz <= 0) return 0;
    if (a->count > bin_len) return 0;
    if (bv->byteOffset < 0 || bv->byteLength < 0 || a->byteOffset < 0) return 0;
    // The view must lie inside BIN — by subtraction, never by addition.
    if (bv->byteOffset > bin_len) return 0;
    if (bv->byteLength > bin_len - bv->byteOffset) return 0;
    if (a->byteOffset > bv->byteLength) return 0;
    // Interleaved data: real size = (count-1)*stride + sz (stride > sz). Solved
    // as a division: the product of two file-derived longs overflows a 32-bit
    // long (Android/ILP32), and a wrapped negative passes any sum comparison.
    long stride = (bv->byteStride > 0) ? (long)bv->byteStride : sz;
    if (stride <= 0) return 0;
    long avail = bv->byteLength - a->byteOffset;
    if (avail < sz) return 0;
    return (a->count - 1) <= (avail - sz) / stride;
}
static int bv_fits(const GlbBv *bvs, int bv_n, int i, long bin_len)
{
    if (i < 0 || i >= bv_n) return 0;
    if (bvs[i].byteOffset < 0 || bvs[i].byteLength < 0) return 0;
    if (bvs[i].byteOffset > bin_len) return 0;
    if (bvs[i].byteLength > bin_len - bvs[i].byteOffset) return 0;
    // Sole caller hands the length to stbi as an int; a view longer than INT_MAX
    // would truncate to a negative length.
    return bvs[i].byteLength <= 0x7fffffffL;
}

// --- Albedo from the GLB (bufferView -> stb_image -> RGBA pixels) ------------------
static int load_albedo(GlbItem *o, int mi,
                       const JsonNode *materials, const JsonNode *textures,
                       const JsonNode *images, const JsonNode *samplers,
                       const GlbBv *bvs, int bv_n, const unsigned char *bin,
                       long bin_len)
{
    if (mi < 0 || !materials || materials->type != JSON_ARRAY ||
        !textures || textures->type != JSON_ARRAY ||
        !images || images->type != JSON_ARRAY) return 0;
    const JsonNode *mt = json_array_get(materials, mi);
    if (!mt) return 0;
    const JsonNode *bct = json_find(json_find(mt, "pbrMetallicRoughness"), "baseColorTexture");
    int tidx = (int)json_num(bct ? json_find(bct, "index") : NULL, -1);
    if (tidx < 0 || tidx >= json_array_len(textures)) return 0;
    const JsonNode *tex = json_array_get(textures, tidx);
    int src = (int)json_num(json_find(tex, "source"), -1);
    int smp = (int)json_num(json_find(tex, "sampler"), -1);
    if (src < 0 || src >= json_array_len(images)) return 0;
    int iv = (int)json_num(json_find(json_array_get(images, src), "bufferView"), -1);
    if (!bv_fits(bvs, bv_n, iv, bin_len)) return 0;
    int w, h, ch;
    unsigned char *px = stbi_load_from_memory(bin + bvs[iv].byteOffset,
                                              (int)bvs[iv].byteLength, &w, &h, &ch, 4);
    if (!px) return 0;
    o->img = px; o->iw = w; o->ih = h;
    o->wrap = MAT_WRAP_CLAMP; o->filter = MAT_FILTER_MIPMAP;
    if (smp >= 0 && samplers && samplers->type == JSON_ARRAY && smp < json_array_len(samplers)) {
        const JsonNode *sa = json_array_get(samplers, smp);
        int wS = (int)json_num(json_find(sa, "wrapS"), 33071);
        int wT = (int)json_num(json_find(sa, "wrapT"), 33071);
        // UVs of these Sketchfab models sometimes leave [0,1]; REPEAT from the sampler
        // (10497) pulled in the 2nd tile -> "torn" texture. Use CLAMP (edge) instead.
        o->wrap = (wS == 33648 || wT == 33648) ? MAT_WRAP_MIRROR : MAT_WRAP_CLAMP;
        int mag = (int)json_num(json_find(sa, "magFilter"), 9729);
        o->filter = (mag == 9728) ? MAT_FILTER_NEAREST : MAT_FILTER_MIPMAP;
    }
    return 1;
}

// --- Parser (CPU, no GL) ------------------------------------------------------
GlbModel *glb_parse_file(const char *path)
{
    unsigned int filesz = 0;
    unsigned char *file = resources_read_file(path, &filesz);
    if (!file) { SDL_Log("glb: not found %s", path); return NULL; }
    if (filesz < 20 || memcmp(file, "glTF", 4) != 0) {
        SDL_Log("glb: %s — not a GLB (header)", path);
        free(file); return NULL;
    }

    const unsigned char *json_data = NULL, *bin = NULL;
    unsigned int json_len = 0, bin_len = 0;
    unsigned int off = 12;
    while (off + 8 <= filesz) {
        unsigned int clen, ctype;
        memcpy(&clen, file + off, 4); memcpy(&ctype, file + off + 4, 4);
        off += 8;
        // clen comes from the file: off+clen can wrap around 32 bits and slip
        // past the check, after which bin_len would be falsely huge and
        // acc_fits/bv_fits would validate against it instead of the real buffer.
        if (clen > filesz - off) break;
        if (ctype == GLB_CHUNK_JSON) { json_data = file + off; json_len = clen; }
        else if (ctype == GLB_CHUNK_BIN) { bin = file + off; bin_len = clen; }
        off += clen;
    }
    if (!json_data || !bin) {
        SDL_Log("glb: %s — no JSON/BIN chunks", path);
        free(file); return NULL;
    }

    const JsonNode *root = json_parse((const char *)json_data, (size_t)json_len);
    if (!root) { SDL_Log("glb: %s — JSON not recognized", path); free(file); return NULL; }

    const JsonNode *acc_arr = json_find(root, "accessors");
    const JsonNode *bv_arr = json_find(root, "bufferViews");
    const JsonNode *nodes = json_find(root, "nodes");
    const JsonNode *meshes = json_find(root, "meshes");
    const JsonNode *materials = json_find(root, "materials");
    const JsonNode *textures = json_find(root, "textures");
    const JsonNode *images = json_find(root, "images");
    const JsonNode *samplers = json_find(root, "samplers");
    int acc_n = acc_arr && acc_arr->type == JSON_ARRAY ? json_array_len(acc_arr) : 0;
    int bv_n = bv_arr && bv_arr->type == JSON_ARRAY ? json_array_len(bv_arr) : 0;
    if (!nodes || nodes->type != JSON_ARRAY) {
        SDL_Log("glb: %s — no nodes", path);
        json_free(); free(file); return NULL;
    }

    GlbBv *bvs = calloc((size_t)(bv_n ? bv_n : 1), sizeof *bvs);
    if (!bvs) { json_free(); free(file); return NULL; }
    for (int i = 0; i < bv_n; i++) {
        const JsonNode *b = json_array_get(bv_arr, i);
        bvs[i].byteOffset = size_field(b, "byteOffset", (long)bin_len);
        bvs[i].byteLength = size_field(b, "byteLength", (long)bin_len);
        // A stride above 64 KiB is garbage; -1/0 both mean "absent", i.e. the
        // tight element packing (see acc_fits).
        long st = size_field(b, "byteStride", 65536);
        bvs[i].byteStride = st > 0 ? (int)st : 0;
    }
    GlbAcc *accs = calloc((size_t)(acc_n ? acc_n : 1), sizeof *accs);
    if (!accs) { free(bvs); json_free(); free(file); return NULL; }
    for (int i = 0; i < acc_n; i++) {
        const JsonNode *a = json_array_get(acc_arr, i);
        accs[i].bufferView = (int)json_num(json_find(a, "bufferView"), -1);
        accs[i].byteOffset = size_field(a, "byteOffset", (long)bin_len);
        accs[i].comp = (int)json_num(json_find(a, "componentType"), C_FLOAT);
        accs[i].vec = type_vec(json_str(json_find(a, "type"), "SCALAR"));
        accs[i].normalized = json_bool(json_find(a, "normalized"), 0);
        accs[i].count = size_field(a, "count", (long)bin_len);
    }

    Bucket *buckets = NULL; int bk_n = 0, bk_cap = 0;
    int ok = 1; const char *err = "";
    GlbStackItem *stack = NULL;
    int st_n = 0, st_cap = 0;

    const JsonNode *scene = json_find(root, "scenes");
    const JsonNode *rk = NULL;
    if (scene && scene->type == JSON_ARRAY && json_array_len(scene) > 0)
        rk = json_find(json_array_get(scene, 0), "nodes");
    if (rk && rk->type == JSON_ARRAY) {
        for (int i = 0; i < json_array_len(rk); i++) {
            int ni = (int)json_num(json_array_get(rk, i), -1);
            const JsonNode *nd = (ni >= 0 && ni < json_array_len(nodes)) ? json_array_get(nodes, ni) : NULL;
            if (!nd) continue;
            if (!stack_push(&stack, &st_n, &st_cap, nd, NULL)) {
                ok = 0; err = "node stack out of memory"; break;
            }
        }
    } else {
        // no scenes — walk all nodes as a flat list
        for (int i = 0; i < json_array_len(nodes); i++) {
            const JsonNode *nd = json_array_get(nodes, i);
            if (!stack_push(&stack, &st_n, &st_cap, nd, NULL)) {
                ok = 0; err = "node stack out of memory"; break;
            }
        }
    }

    while (st_n > 0 && ok) {
        GlbStackItem it = stack[--st_n];
        const JsonNode *nd = it.node;
        if (!nd) continue;
        float local[16];
        const JsonNode *mn = json_find(nd, "matrix");
        if (mn && mn->type == JSON_ARRAY && json_array_len(mn) == 16) {
            for (int c = 0; c < 16; c++) local[c] = (float)json_num(json_array_get(mn, c), 0);
        } else mat4_trs(local, nd);
        float world[16];
        mat4_mul(world, it.m, local);

        int mesh_idx = (int)json_num(json_find(nd, "mesh"), -1);
        if (mesh_idx >= 0 && meshes && mesh_idx < json_array_len(meshes)) {
            const JsonNode *prims = json_find(json_array_get(meshes, mesh_idx), "primitives");
            if (prims && prims->type == JSON_ARRAY) {
                for (int p = 0; p < json_array_len(prims) && ok; p++) {
                    const JsonNode *prim = json_array_get(prims, p);
                    if ((int)json_num(json_find(prim, "mode"), 4) != 4) continue;
                    int mat_idx = (int)json_num(json_find(prim, "material"), -1);
                    if (mat_idx >= 0 && materials && mat_idx >= json_array_len(materials)) mat_idx = -1;
                    const JsonNode *attrs = json_find(prim, "attributes");
                    int ipos = (int)json_num(attrs ? json_find(attrs, "POSITION") : NULL, -1);
                    int inrm = (int)json_num(attrs ? json_find(attrs, "NORMAL") : NULL, -1);
                    // The UV set is the one baseColorTexture of the material
                    // refers to (character eyes/face often live in TEXCOORD_1)
                    int uvset = base_uv_set(materials, textures, mat_idx);
                    char uvkey[24];
                    snprintf(uvkey, sizeof uvkey, "TEXCOORD_%d", uvset);
                    int iuv = (int)json_num(attrs ? json_find(attrs, uvkey) : NULL, -1);
                    if (iuv < 0)
                        iuv = (int)json_num(attrs ? json_find(attrs, "TEXCOORD_0") : NULL, -1);
                    int iidx = (int)json_num(json_find(prim, "indices"), -1);
                    if (ipos < 0 || ipos >= acc_n) { ok = 0; err = "no POSITION"; break; }
                    GlbAcc *ap = &accs[ipos];
                    // OOB protection: the accessor must fit inside the BIN chunk.
                    // vec is checked right here too: read_vertex copies vec
                    // floats into vp[3], so POSITION must be exactly VEC3.
                    if (!acc_fits(ap, bvs, bv_n, (long)bin_len) || ap->vec != 3) {
                        SDL_Log("glb: %s — POSITION accessor out of buffer or not VEC3, primitive skipped", path);
                        continue;
                    }
                    if (ap->count > 16L * 1024 * 1024) {
                        SDL_Log("glb: %s — POSITION count %ld too large, primitive skipped", path, ap->count);
                        continue;
                    }
                    // NORMAL exactly VEC3, UV no wider than VEC2 — else read_vertex
                    // would touch n[3]/u[2] beyond their size. If it does not fit —
                    // the attribute is simply not read (normal stays {0,1,0}, uv {0,0}).
                    GlbAcc *an = (inrm >= 0 && inrm < acc_n && accs[inrm].vec == 3 && acc_fits(&accs[inrm], bvs, bv_n, (long)bin_len)) ? &accs[inrm] : NULL;
                    GlbAcc *au = (iuv >= 0 && iuv < acc_n && accs[iuv].vec <= 2 && acc_fits(&accs[iuv], bvs, bv_n, (long)bin_len)) ? &accs[iuv] : NULL;
                    GlbAcc *ai = (iidx >= 0 && iidx < acc_n && acc_fits(&accs[iidx], bvs, bv_n, (long)bin_len)) ? &accs[iidx] : NULL;
                    long nv = ap->count;

                    Bucket *bk = b_get(&buckets, &bk_n, &bk_cap, mat_idx);
                    if (!bk) { ok = 0; err = "bucket out of memory"; break; }
                    unsigned int base = (unsigned int)bk->vn;
                    long pos_base = (ap->bufferView >= 0 && ap->bufferView < bv_n)
                                    ? bvs[ap->bufferView].byteOffset + ap->byteOffset : 0;
                    long nrm_base = an && an->bufferView >= 0 ? bvs[an->bufferView].byteOffset + an->byteOffset : 0;
                    long uv_base = au && au->bufferView >= 0 ? bvs[au->bufferView].byteOffset + au->byteOffset : 0;
                    for (long v = 0; v < nv; v++) {
                        float vp[3], n[3] = {0, 1, 0}, u[2] = {0, 0};
                        read_vertex(bin, pos_base, ap, bvs, bv_n, (int)v, vp);
                        mat4_transform_point(world, vp, vp);
                        // NORMAL/UV are indexed by the same v, but acc_fits checked
                        // them against THEIR OWN count. A broken GLB with count <
                        // POSITION.count would otherwise read past the BIN chunk
                        // (OOB) — so read the attribute only within its own count.
                        if (an && v < an->count) { read_vertex(bin, nrm_base, an, bvs, bv_n, (int)v, n); mat4_transform_nrm(world, n, n); }
                        if (au && v < au->count) read_vertex(bin, uv_base, au, bvs, bv_n, (int)v, u);
                        // NO V flip: stb_image puts the top row of the image at
                        // t=0, and glTF v=0 is also the top (see glb_anim)
                        if (!b_push_v(bk, vp[0], vp[1], vp[2], n[0], n[1], n[2], u[0], u[1])) {
                            ok = 0; err = "vertex out of memory"; break;
                        }
                    }
                    if (ok && ai) {
                        long idx_base = ai->bufferView >= 0 && ai->bufferView < bv_n
                                        ? bvs[ai->bufferView].byteOffset + ai->byteOffset : 0;
                        for (long i = 0; i < ai->count && ok; i++) {
                            long ii = read_index(bin, idx_base, ai, bvs, bv_n, (int)i, nv);
                            if (ii >= 0 && !b_push_i(bk, base + (unsigned int)ii)) {
                                ok = 0; err = "index out of memory"; break;
                            }
                        }
                    } else if (ok) {
                        for (long i = 0; i < nv && ok; i++) {
                            if (!b_push_i(bk, base + (unsigned int)i)) {
                                ok = 0; err = "index out of memory"; break;
                            }
                        }
                    }
                }
            }
        }

        const JsonNode *ch = json_find(nd, "children");
        if (ch && ch->type == JSON_ARRAY) {
            for (int i = 0; i < json_array_len(ch); i++) {
                int ci = (int)json_num(json_array_get(ch, i), -1);
                const JsonNode *cnd = (ci >= 0 && ci < json_array_len(nodes)) ? json_array_get(nodes, ci) : NULL;
                if (!cnd) continue;
                if (!stack_push(&stack, &st_n, &st_cap, cnd, world)) {
                    ok = 0; err = "node stack out of memory"; break;
                }
            }
        }
    }
    free(stack);
    free(accs);

    if (!ok || bk_n == 0) {
        SDL_Log("glb: %s — %s", path, err[0] ? err : "no primitives");
        for (int i = 0; i < bk_n; i++) b_free(&buckets[i]);
        free(buckets);
        free(bvs);
        json_free();
        free(file);
        return NULL;
    }

    // --- Materials (pseudo-PBR 2004) + albedo ----------------------------------
    GlbModel *model = calloc(1, sizeof *model);
    if (!model) {
        for (int i = 0; i < bk_n; i++) b_free(&buckets[i]);
        free(buckets); free(bvs); json_free(); free(file);
        return NULL;
    }
    model->items = calloc((size_t)bk_n, sizeof *model->items);
    if (!model->items) {
        free(model);
        for (int i = 0; i < bk_n; i++) b_free(&buckets[i]);
        free(buckets); free(bvs); json_free(); free(file);
        return NULL;
    }
    model->count = bk_n;
    for (int bi = 0; bi < bk_n; bi++) {
        GlbItem *o = &model->items[bi];
        Bucket *bk = &buckets[bi];
        o->verts = bk->verts; o->normals = bk->nrm; o->uvs = bk->uv;
        o->vcount = bk->vn; o->indices = bk->idx; o->index_count = bk->idxn;
        o->tint[0] = o->tint[1] = o->tint[2] = 1.0f;
        o->alpha = 1.0f; o->blend = MAT_BLEND_OPAQUE; o->cull = MAT_CULL_NONE;
        o->tile[0] = o->tile[1] = 1.0f;
        o->specular = 0.2f; o->wrap = MAT_WRAP_CLAMP; o->filter = MAT_FILTER_MIPMAP;

        int mi = bk->mat;
        if (mi >= 0 && materials && mi < json_array_len(materials)) {
            const JsonNode *mt = json_array_get(materials, mi);
            const JsonNode *pbr = json_find(mt, "pbrMetallicRoughness");
            float rough = 0.6f, metallic = 0.0f;
            if (pbr) {
                const JsonNode *bcf = json_find(pbr, "baseColorFactor");
                if (bcf && bcf->type == JSON_ARRAY && json_array_len(bcf) >= 4) {
                    o->tint[0] = (float)json_num(json_array_get(bcf, 0), 1);
                    o->tint[1] = (float)json_num(json_array_get(bcf, 1), 1);
                    o->tint[2] = (float)json_num(json_array_get(bcf, 2), 1);
                    o->alpha = (float)json_num(json_array_get(bcf, 3), 1);
                }
                rough = (float)json_num(json_find(pbr, "roughnessFactor"), 0.6f);
                metallic = (float)json_num(json_find(pbr, "metallicFactor"), 0.0f);
            }
            o->specular = spec_from_rough(rough, metallic);
            const char *am = json_str(json_find(mt, "alphaMode"), "OPAQUE");
            if (strcmp(am, "BLEND") == 0) o->blend = MAT_BLEND_ALPHA;
            else if (strcmp(am, "MASK") == 0) o->blend = MAT_BLEND_ALPHATEST;
            else o->blend = MAT_BLEND_OPAQUE;
            // glTF props are rendered two-sided (MAT_CULL_NONE): otherwise thin
            // one-sided panels and the cabin (seen through the glass from outside) vanish.
            o->cull = MAT_CULL_NONE;
            const JsonNode *em = json_find(mt, "emissiveFactor");
            if (em && em->type == JSON_ARRAY && json_array_len(em) >= 3) {
                o->tint[0] += (float)json_num(json_array_get(em, 0), 0);
                o->tint[1] += (float)json_num(json_array_get(em, 1), 0);
                o->tint[2] += (float)json_num(json_array_get(em, 2), 0);
            }
            if (json_find(mt, "normalTexture"))
                SDL_Log("glb: %s — normalTexture ignored (2004-style pseudo-PBR)", path);
            load_albedo(o, mi, materials, textures, images, samplers, bvs, bv_n,
                        bin, (long)bin_len);
            // "Glass" in gltf models is often painted with an OPAQUE texture
            // (alpha=255). A BLEND material has no real alpha to mix with, so the
            // panel stays solid and hides the interior/innards. If the
            // texture has not a single translucent pixel, treat the material as
            // glass and lower the overall opacity.
            if (o->blend == MAT_BLEND_ALPHA && o->img && o->alpha >= 0.95f) {
                int has_trans = 0;
                int npix = (int)((size_t)o->iw * o->ih);
                const unsigned char *pa = o->img + 3;
                for (int k = 0; k < npix; k++, pa += 4)
                    if (*pa < 250) { has_trans = 1; break; }
                if (!has_trans) o->alpha = 0.35f;
            }
        }
        // Data handed over to GlbItem — do not free it again in b_free
        bk->verts = bk->nrm = bk->uv = (float *)NULL;
        bk->idx = NULL;
        bk->vn = bk->idxn = 0;
    }
    for (int i = 0; i < bk_n; i++) b_free(&buckets[i]);
    free(buckets);
    free(bvs);

    json_free();
    free(file);
    return model;
}

// --- Instance node (GL) ---------------------------------------------------------
typedef struct {
    GLuint vao, vbo, ebo;
    GLuint snow_vbo;         // per-vertex "open to the sky above" (for snow), 0 if none
    int index_count;
    GLenum index_type;
    GLuint tex;
    int tex_owned;           // 1 — glGenTextures (from img), 0 — resource cache
    Material mat;
    MaterialLocs ml;
    float specular;
} DrawItem;
typedef struct {
    const RendererContext *rc;
    DrawItem *items;
    int count;
    GLuint prog;
    SceneUniforms su;
    GLint u_model, u_tex, u_specular;
    float scale;
    float radius;                  // model bounding sphere (in world scale)
} GlbParams;

static DrawItem *build_item(const GlbItem *gi, GLuint prog, const float *snow)
{
    DrawItem *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    glGenVertexArrays(1, &d->vao);
    glBindVertexArray(d->vao);
    glGenBuffers(1, &d->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, d->vbo);
    const size_t elem = 8 * sizeof(float);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)((size_t)gi->vcount * elem), NULL, GL_STATIC_DRAW);
    float *cpu = malloc((size_t)gi->vcount * 8 * sizeof(float));
    if (!cpu) { SDL_Log("glb: out of memory for CPU vertex buffer"); glDeleteVertexArrays(1, &d->vao);
                glDeleteBuffers(1, &d->vbo); free(d); return NULL; }
    for (int i = 0; i < gi->vcount; i++) {
        cpu[8 * i + 0] = gi->verts[3 * i + 0];
        cpu[8 * i + 1] = gi->verts[3 * i + 1];
        cpu[8 * i + 2] = gi->verts[3 * i + 2];
        cpu[8 * i + 3] = gi->normals ? gi->normals[3 * i + 0] : 0;
        cpu[8 * i + 4] = gi->normals ? gi->normals[3 * i + 1] : 1;
        cpu[8 * i + 5] = gi->normals ? gi->normals[3 * i + 2] : 0;
        cpu[8 * i + 6] = gi->uvs ? gi->uvs[2 * i + 0] : 0;
        cpu[8 * i + 7] = gi->uvs ? gi->uvs[2 * i + 1] : 0;
    }
    glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)((size_t)gi->vcount * elem), cpu);
    free(cpu);
    GLint a_pos = glGetAttribLocation(prog, "aPos");
    GLint a_nrm = glGetAttribLocation(prog, "aNormal");
    GLint a_uv = glGetAttribLocation(prog, "aUV");
    glVertexAttribPointer(a_pos, 3, GL_FLOAT, GL_FALSE, (GLsizei)elem, NULL);
    glEnableVertexAttribArray(a_pos);
    if (a_nrm >= 0) { glVertexAttribPointer(a_nrm, 3, GL_FLOAT, GL_FALSE, (GLsizei)elem, (void *)(3 * sizeof(float))); glEnableVertexAttribArray(a_nrm); }
    if (a_uv >= 0) { glVertexAttribPointer(a_uv, 2, GL_FLOAT, GL_FALSE, (GLsizei)elem, (void *)(6 * sizeof(float))); glEnableVertexAttribArray(a_uv); }
    if (snow) {
        glGenBuffers(1, &d->snow_vbo);
        glBindBuffer(GL_ARRAY_BUFFER, d->snow_vbo);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)((size_t)gi->vcount * sizeof(float)), snow, GL_STATIC_DRAW);
        GLint a_snow = glGetAttribLocation(prog, "aSnow");
        if (a_snow >= 0) {
            glVertexAttribPointer(a_snow, 1, GL_FLOAT, GL_FALSE, 0, NULL);
            glEnableVertexAttribArray(a_snow);
        }
    }
    glGenBuffers(1, &d->ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, d->ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)((size_t)gi->index_count * sizeof(unsigned int)),
                 gi->indices, GL_STATIC_DRAW);
    d->index_count = gi->index_count;
    d->index_type = GL_UNSIGNED_INT;
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    material_defaults(&d->mat);
    memcpy(d->mat.tint, gi->tint, sizeof gi->tint);
    d->mat.alpha = gi->alpha;
    d->mat.blend = gi->blend;
    d->mat.cull = gi->cull;
    d->mat.tile[0] = gi->tile[0]; d->mat.tile[1] = gi->tile[1];
    d->mat.set = 1;
    material_locs_init(&d->ml, prog);
    d->specular = gi->specular;
    if (gi->img) {
        glGenTextures(1, &d->tex);
        d->tex_owned = 1;
        glBindTexture(GL_TEXTURE_2D, d->tex);
        GLint wm = GL_REPEAT, fm = GL_LINEAR_MIPMAP_LINEAR, fmag = GL_LINEAR;
        if (gi->wrap == MAT_WRAP_CLAMP) wm = GL_CLAMP_TO_EDGE;
        else if (gi->wrap == MAT_WRAP_MIRROR) wm = GL_MIRRORED_REPEAT;
        if (gi->filter == MAT_FILTER_NEAREST) { fm = GL_NEAREST; fmag = GL_NEAREST; }
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wm);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wm);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, fm);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, fmag);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, gi->iw, gi->ih, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, gi->img);
        glGenerateMipmap(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, 0);
    } else {
        d->tex = resources_texture("img/notfound.png", 4, GL_REPEAT, 0);
    }
    return d;
}

static void glb_node_exit(const Event *e, void *user);   // forward decl (see below)
static void glb_free_resources(GlbParams *p);

SceneNode *glb_make_node(const char *path, const float pos[3], float scale,
                         const RendererContext *rc)
{
    // Once: a GlbInstance3D node releases its resources when the tree exits.
    static int s_registered = 0;
    if (!s_registered) { events_listen(EV_NODE_EXIT, glb_node_exit, NULL); s_registered = 1; }

    GlbModel *gm = glb_parse_file(path);
    if (!gm) return NULL;
    GLuint prog = make_program("shaders/mesh.vs", "shaders/mesh.fs");
    if (!prog) { glb_model_free(gm); return NULL; }

    GlbParams *p = calloc(1, sizeof *p);
    if (!p) {
        resources_program_release(prog);
        glb_model_free(gm);
        return NULL;
    }
    p->rc = rc;
    p->scale = scale > 0 ? scale : 1.0f;
    p->prog = prog;
    scene_uniforms_init(&p->su, prog);
    p->u_model = glGetUniformLocation(prog, "uModel");
    p->u_tex = glGetUniformLocation(prog, "uTex");
    p->u_specular = glGetUniformLocation(prog, "uSpecular");

    p->items = calloc((size_t)gm->count, sizeof *p->items);
    if (!p->items) {
        resources_program_release(prog);
        glb_model_free(gm);
        free(p);
        return NULL;
    }
    p->count = 0;
    p->radius = 0.0f;
    // Per-vertex "open to the sky above": snow will settle on the roof/hood/trunk,
    // but not inside the cabin (under its own roof). Computed over the whole model.
    const float **vp = malloc((size_t)gm->count * sizeof *vp);
    int *vc = malloc((size_t)gm->count * sizeof *vc);
    const unsigned **ip = malloc((size_t)gm->count * sizeof *ip);
    int *ic = malloc((size_t)gm->count * sizeof *ic);
    if (!vp || !vc || !ip || !ic) {
        free(vp); free(vc); free(ip); free(ic);
        glb_free_resources(p);
        glb_model_free(gm);
        free(p);
        return NULL;
    }
    for (int i = 0; i < gm->count; i++) {
        vp[i] = gm->items[i].verts; vc[i] = gm->items[i].vcount;
        ip[i] = gm->items[i].indices; ic[i] = gm->items[i].index_count;
    }
    int snow_total = 0;
    float *snow_mask = snow_mask_compute(vp, vc, ip, ic, gm->count, &snow_total);
    free(vp); free(vc); free(ip); free(ic);
    static int s_dump_glb = 1;
    int snow_off = 0;
    for (int i = 0; i < gm->count; i++) {
        DrawItem *bi = build_item(&gm->items[i], prog, snow_mask ? snow_mask + snow_off : NULL);
        snow_off += gm->items[i].vcount;
        if (!bi) continue;                 // OOM skip, p->items[i] stays zeroed
        DrawItem *d = &p->items[p->count++];
        *d = *bi; free(bi);                // struct copied — free the wrapper
        // bounding sphere from bucket geometry (verts already in GLB world coords)
        float mn[3] = {1e30f,1e30f,1e30f}, mx[3] = {-1e30f,-1e30f,-1e30f};
        for (int v = 0; v < gm->items[i].vcount; v++) {
            float dx = gm->items[i].verts[3*v+0], dy = gm->items[i].verts[3*v+1],
                  dz = gm->items[i].verts[3*v+2];
            float r2 = dx*dx + dy*dy + dz*dz;
            if (r2 > p->radius) p->radius = r2;
            if (dx < mn[0]) mn[0] = dx;
            if (dy < mn[1]) mn[1] = dy;
            if (dz < mn[2]) mn[2] = dz;
            if (dx > mx[0]) mx[0] = dx;
            if (dy > mx[1]) mx[1] = dy;
            if (dz > mx[2]) mx[2] = dz;
        }
        if (s_dump_glb)
            SDL_Log("glb bkt %d: nv=%d x[%.2f..%.2f] y[%.2f..%.2f] z[%.2f..%.2f]",
                i, gm->items[i].vcount, mn[0],mx[0], mn[1],mx[1], mn[2],mx[2]);
    }
    if (s_dump_glb) s_dump_glb = 0;
    free(snow_mask);
    p->radius = sqrtf(p->radius) * p->scale;
    glb_model_free(gm);

    glUseProgram(prog);
    if (p->u_tex >= 0) glUniform1i(p->u_tex, 0);
    for (int c = 0; c < SHADOW_CASCADES; c++) {
        if (p->su.u_shadowmap[c] >= 0) glUniform1i(p->su.u_shadowmap[c], 3 + c);
        if (p->su.u_shadowdepth[c] >= 0) glUniform1i(p->su.u_shadowdepth[c], 3 + c);
    }
    glUseProgram(0);

    float size[3] = {scale, scale, scale};
    SceneNode *n = node_new("GlbInstance", "GlbInstance3D", pos, size, p, NULL, glb_node_render);
    if (!n) {
        glb_free_resources(p);
        free(p);
        return NULL;
    }
    // The scale goes into the node transform: scene.c assembles the world matrix, so
    // the model can be rotated (n->rot) and attached to bones of other models.
    n->scl[0] = n->scl[1] = n->scl[2] = p->scale;
    node_update_transforms(n);
    return n;
}

// --- Shadows: GLB models cast a shadow ----------------------------------------
// Depth pass (mesh_shadow.vs/fs): position × node world matrix ×
// sun VP, alpha-test against the baguette texture (foliage/grass quads in glTF
// must not cast a shadow with the whole quad). One program for all glb nodes
// (module static, hot-reload via the reshade hook).
static GLuint s_glb_shadow_prog = 0;
static GLint s_glb_sh_model = -1, s_glb_sh_vp = -1, s_glb_sh_tex = -1,
             s_glb_sh_at = -1;

static void glb_shadow_prog_init(void)
{
    s_glb_sh_model = glGetUniformLocation(s_glb_shadow_prog, "uModel");
    s_glb_sh_vp    = glGetUniformLocation(s_glb_shadow_prog, "uViewProj");
    s_glb_sh_tex   = glGetUniformLocation(s_glb_shadow_prog, "uTex");
    s_glb_sh_at    = glGetUniformLocation(s_glb_shadow_prog, "uAlphaTest");
}

static void glb_shadow_reshade(void)
{
    GLuint p = resources_program_replace(s_glb_shadow_prog,
                                         "shaders/mesh_shadow.vs",
                                         "shaders/mesh_shadow.fs");
    if (p) s_glb_shadow_prog = p;
    glb_shadow_prog_init();
}

void glb_node_render(SceneNode *n, float dt)
{
    (void)dt;
    GlbParams *p = n->params;
    if (!p) return;
    if (p->rc->pass == RP_SHADOW) {
        // Shadow pass: model depth from the sun's point of view. We do not check the
        // frustum — clipping against the cascade box is done by the calling code.
        if (!s_glb_shadow_prog) {
            s_glb_shadow_prog = resources_program_replace(
                0, "shaders/mesh_shadow.vs", "shaders/mesh_shadow.fs");
            glb_shadow_prog_init();
            shader_on_reshade(glb_shadow_reshade);
        }
        glEnable(GL_DEPTH_TEST);
        glUseProgram(s_glb_shadow_prog);
        glUniformMatrix4fv(s_glb_sh_model, 1, GL_FALSE, n->world);
        glUniformMatrix4fv(s_glb_sh_vp, 1, GL_FALSE, p->rc->shadow_vp);
        glUniform1i(s_glb_sh_at, 1);
        glActiveTexture(GL_TEXTURE0);
        if (s_glb_sh_tex >= 0) glUniform1i(s_glb_sh_tex, 0);
        for (int i = 0; i < p->count; i++) {
            DrawItem *d = &p->items[i];
            glBindTexture(GL_TEXTURE_2D, d->tex);
            glBindVertexArray(d->vao);
            glDrawElements(GL_TRIANGLES, d->index_count, d->index_type, NULL);
        }
        glBindVertexArray(0);
        return;
    }
    if (!frustum_test_sphere(&p->rc->frustum, n->world[12], n->world[13],
                             n->world[14], p->radius))
        return;

    // The model matrix = the node WORLD matrix (scene.c): position, rotation,
    // scale and — if the node is attached to bones/another node — the whole
    // parent chain. It used to be assembled by hand from pos and the scale
    // scalar, so the model could be neither rotated nor attached.
    const float *model = n->world;
    glEnable(GL_DEPTH_TEST);
    glUseProgram(p->prog);
    scene_uniforms_bind(&p->su, p->rc);
    scene_uniforms_probe(&p->su, model[12], model[13], model[14]);
    glUniformMatrix4fv(p->u_model, 1, GL_FALSE, model);
    if (p->u_tex >= 0) glUniform1i(p->u_tex, 0);

    for (int i = 0; i < p->count; i++) {
        DrawItem *d = &p->items[i];
        if (p->u_specular >= 0) glUniform1f(p->u_specular, d->specular);
        material_bind(&d->mat, &d->ml, 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, d->tex);
        glBindVertexArray(d->vao);
        glDrawElements(GL_TRIANGLES, d->index_count, d->index_type, NULL);
    }
    glBindVertexArray(0);
}

// Release of GlbInstance3D node resources (via EV_NODE_EXIT from node_free_tree).
// node_free_tree frees only the GlbParams struct; the VAO/VBO/EBO of every
// bucket, the textures (own + cache) and the program refcount are released here.
static void glb_free_resources(GlbParams *p)
{
    if (!p) return;
    if (p->items) {
        for (int i = 0; i < p->count; i++) {
            DrawItem *d = &p->items[i];
            if (d->vao) glDeleteVertexArrays(1, &d->vao);
            if (d->vbo) glDeleteBuffers(1, &d->vbo);
            if (d->snow_vbo) glDeleteBuffers(1, &d->snow_vbo);
            if (d->ebo) glDeleteBuffers(1, &d->ebo);
            if (d->tex_owned) glDeleteTextures(1, &d->tex);
            else if (d->tex) resources_texture_release(d->tex);
        }
        free(p->items); p->items = NULL;
    }
    if (p->prog) { resources_program_release(p->prog); p->prog = 0; }
}

static void glb_node_exit(const Event *e, void *user)
{
    (void)user;
    if (!e || !e->a) return;
    const SceneNode *n = e->a;
    if (!n->type || strcmp(n->type, "GlbInstance3D") != 0) return;
    glb_free_resources(n->params);
}

// --- Release ---------------------------------------------------------------
void glb_model_free(GlbModel *m)
{
    if (!m) return;
    for (int i = 0; i < m->count; i++) {
        free(m->items[i].verts);
        free(m->items[i].normals);
        free(m->items[i].uvs);
        free(m->items[i].indices);
        stbi_image_free(m->items[i].img);
    }
    free(m->items);
    free(m);
}