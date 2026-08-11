// ============================================================================
//  glb.c — бинарный glTF 2.0 (.glb): парсер + GlbInstance3D-узел (см. glb.h)
// ----------------------------------------------------------------------------
//  Два слоя:
//    glb_parse_file() — только CPU: чанки, иерархия, запекание в мировые
//                       координаты, группировка по материалам, декод альбедо.
//                       Не трогает GL — вызывается в селфтесте/headless.
//    glb_make_node()   — строит GL-буферы и текстуры, создаёт узел-инстанс.
//
//  ПРАВИЛО РЕНДЕРА: движок — ПСЕВДО-PBR 2004 (как HL2/Source, Far Cry 1).
//  Настоящие PBR-данные GLB ПЕРЕФОРМАТИРУЮТСЯ в наш материал (material.h):
//    baseColorFactor -> tint;      alphaMode -> blend/alpha
//    roughness/металл -> specular (гладкость);
//    emissiveFactor  -> прибавка к tint;     normalTexture -> игнор.
// ============================================================================
#include "glb.h"

#include "json.h"
#include "render_ctx.h"
#include "resources.h"
#include "shader.h"
#include "stb_image.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

// --- Компоненты glTF ----------------------------------------------------------
#define C_BYTE 5120
#define C_UBYTE 5121
#define C_SHORT 5122
#define C_USHORT 5123
#define C_UINT 5125
#define C_FLOAT 5126

static int comp_size(int c)
{
    switch (c) {
        case C_BYTE: case C_UBYTE: return 1;
        case C_SHORT: case C_USHORT: return 2;
        case C_UINT: case C_FLOAT: return 4;
    }
    return 0;
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

typedef struct {
    int bufferView;
    long byteOffset;
    int comp, vec, normalized;
    long count;
} GlbAcc;
typedef struct { long byteOffset, byteLength; int byteStride; } GlbBv;

// --- Матрицы 4x4 (column-major) -----------------------------------------------
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

// --- Бакет вершин (по материалу) ----------------------------------------------
typedef struct {
    int mat;                       // индекс материала (-1 = без)
    float *verts, *nrm, *uv;
    int vcap, vn;
    unsigned int *idx;
    int icap, idxn;
} Bucket;
static Bucket *b_get(Bucket **arr, int *n, int *cap, int mat)
{
    for (int i = 0; i < *n; i++) if ((*arr)[i].mat == mat) return &(*arr)[i];
    if (*n >= *cap) {
        *cap = *cap ? *cap * 2 : 16;
        *arr = realloc(*arr, (size_t)*cap * sizeof **arr);
    }
    Bucket *b = &(*arr)[(*n)++];
    memset(b, 0, sizeof *b);
    b->mat = mat;
    return b;
}
static void b_push_v(Bucket *b, float px, float py, float pz,
                     float nx, float ny, float nz, float u, float v)
{
    if (b->vn >= b->vcap) {
        b->vcap = b->vcap ? b->vcap * 2 : 4096;
        b->verts = realloc(b->verts, (size_t)b->vcap * 3 * sizeof(float));
        b->nrm = realloc(b->nrm, (size_t)b->vcap * 3 * sizeof(float));
        b->uv = realloc(b->uv, (size_t)b->vcap * 2 * sizeof(float));
    }
    b->verts[3 * b->vn] = px; b->verts[3 * b->vn + 1] = py; b->verts[3 * b->vn + 2] = pz;
    b->nrm[3 * b->vn] = nx; b->nrm[3 * b->vn + 1] = ny; b->nrm[3 * b->vn + 2] = nz;
    b->uv[2 * b->vn] = u; b->uv[2 * b->vn + 1] = v;
    b->vn++;
}
static void b_push_i(Bucket *b, unsigned int i)
{
    if (b->idxn >= b->icap) {
        b->icap = b->icap ? b->icap * 2 : 4096;
        b->idx = realloc(b->idx, (size_t)b->icap * sizeof(unsigned int));
    }
    b->idx[b->idxn++] = i;
}
static void b_free(Bucket *b)
{
    free(b->verts); free(b->nrm); free(b->uv); free(b->idx);
    memset(b, 0, sizeof *b);
}

// --- Чтение данных accessor ---------------------------------------------------
static float read_comp(const unsigned char *bin, long base, const GlbAcc *a,
                       int elem, int comp)
{
    int sz = comp_size(a->comp);
    const unsigned char *p = bin + base + (long)elem * a->vec * sz + (long)comp * sz;
    switch (a->comp) {
        case C_FLOAT:  { float v; memcpy(&v, p, 4); return v; }
        case C_UINT:   { unsigned int v; memcpy(&v, p, 4); return (float)v; }
        case C_USHORT: { unsigned short v; memcpy(&v, p, 2); return (float)v; }
        case C_SHORT:  { short v; memcpy(&v, p, 2); return (float)v; }
        case C_UBYTE:  return (float)p[0];
        case C_BYTE:   return (float)((signed char)p[0]);
    }
    return 0;
}
static void read_vertex(const unsigned char *bin, long base, const GlbAcc *a,
                        int elem, float *out)
{
    long stride = (long)a->vec * comp_size(a->comp);
    const unsigned char *p = bin + base + (long)elem * stride;
    float f[4] = {0};
    for (int c = 0; c < a->vec; c++) {
        switch (a->comp) {
            case C_FLOAT:  { float v; memcpy(&v, p + (long)c * 4, 4); f[c] = v; break; }
            case C_USHORT: { unsigned short v; memcpy(&v, p + (long)c * 2, 2);
                             f[c] = a->normalized ? (float)v / 65535.0f : (float)v; break; }
            case C_UBYTE:  { f[c] = a->normalized ? (float)p[c] / 255.0f : (float)p[c]; break; }
            default: f[c] = 0; break;
        }
    }
    memcpy(out, f, (size_t)a->vec * sizeof(float));
}

// Псевдо-PBR 2004: гладкость -> спекуляр (rough 0 = блеск)
static float spec_from_rough(float rough, float metallic)
{
    float g = 1.0f - clamp01(rough);
    return 0.05f + g * (0.55f + 0.45f * clamp01(metallic));
}

// --- Альбедо из GLB (bufferView -> stb_image -> RGBA пиксели) ------------------
static int load_albedo(GlbItem *o, int mi,
                       const JsonNode *materials, const JsonNode *textures,
                       const JsonNode *images, const JsonNode *samplers,
                       const GlbBv *bvs, int bv_n, const unsigned char *bin)
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
    if (iv < 0 || iv >= bv_n) return 0;
    int w, h, ch;
    unsigned char *px = stbi_load_from_memory(bin + bvs[iv].byteOffset,
                                              (int)bvs[iv].byteLength, &w, &h, &ch, 4);
    if (!px) return 0;
    o->img = px; o->iw = w; o->ih = h;
    o->wrap = MAT_WRAP_REPEAT; o->filter = MAT_FILTER_MIPMAP;
    if (smp >= 0 && samplers && samplers->type == JSON_ARRAY && smp < json_array_len(samplers)) {
        const JsonNode *sa = json_array_get(samplers, smp);
        int wS = (int)json_num(json_find(sa, "wrapS"), 33071);
        o->wrap = (wS == 33072) ? MAT_WRAP_CLAMP
                 : (wS == 33648) ? MAT_WRAP_MIRROR : MAT_WRAP_REPEAT;
        int wT = (int)json_num(json_find(sa, "wrapT"), 33071);
        if (wT == 33648) o->wrap = MAT_WRAP_MIRROR;
        int mag = (int)json_num(json_find(sa, "magFilter"), 9729);
        o->filter = (mag == 9728) ? MAT_FILTER_NEAREST : MAT_FILTER_MIPMAP;
    }
    return 1;
}

// --- Парсер (CPU, без GL) ------------------------------------------------------
GlbModel *glb_parse_file(const char *path)
{
    unsigned int filesz = 0;
    unsigned char *file = resources_read_file(path, &filesz);
    if (!file) { SDL_Log("glb: не найден %s", path); return NULL; }
    if (filesz < 20 || memcmp(file, "glTF", 4) != 0) {
        SDL_Log("glb: %s — не GLB (заголовок)", path);
        free(file); return NULL;
    }

    const unsigned char *json_data = NULL, *bin = NULL;
    unsigned int json_len = 0;
    unsigned int off = 12;
    while (off + 8 <= filesz) {
        unsigned int clen, ctype;
        memcpy(&clen, file + off, 4); memcpy(&ctype, file + off + 4, 4);
        off += 8;
        if (off + clen > filesz) break;
        if (ctype == 0x4E4F534A) { json_data = file + off; json_len = clen; }
        else if (ctype == 0x004E4942) bin = file + off;
        off += clen;
    }
    if (!json_data || !bin) {
        SDL_Log("glb: %s — нет чанков JSON/BIN", path);
        free(file); return NULL;
    }

    const JsonNode *root = json_parse((const char *)json_data, (size_t)json_len);
    if (!root) { SDL_Log("glb: %s — JSON не распознан", path); free(file); return NULL; }

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
        SDL_Log("glb: %s — нет nodes", path);
        json_free(); free(file); return NULL;
    }

    GlbBv *bvs = calloc((size_t)(bv_n ? bv_n : 1), sizeof *bvs);
    for (int i = 0; i < bv_n; i++) {
        const JsonNode *b = json_array_get(bv_arr, i);
        bvs[i].byteOffset = (long)json_num(json_find(b, "byteOffset"), 0);
        bvs[i].byteLength = (long)json_num(json_find(b, "byteLength"), 0);
        bvs[i].byteStride = (int)json_num(json_find(b, "byteStride"), 0);
    }
    GlbAcc *accs = calloc((size_t)(acc_n ? acc_n : 1), sizeof *accs);
    for (int i = 0; i < acc_n; i++) {
        const JsonNode *a = json_array_get(acc_arr, i);
        accs[i].bufferView = (int)json_num(json_find(a, "bufferView"), -1);
        accs[i].byteOffset = (long)json_num(json_find(a, "byteOffset"), 0);
        accs[i].comp = (int)json_num(json_find(a, "componentType"), C_FLOAT);
        accs[i].vec = type_vec(json_str(json_find(a, "type"), "SCALAR"));
        accs[i].normalized = json_bool(json_find(a, "normalized"), 0);
        accs[i].count = (long)json_num(json_find(a, "count"), 0);
    }

    Bucket *buckets = NULL; int bk_n = 0, bk_cap = 0;
    int ok = 1; const char *err = "";
    struct StackItem { const JsonNode *node; float m[16]; } *stack = NULL;
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
            if (st_n >= st_cap) { st_cap = st_cap ? st_cap * 2 : 64; stack = realloc(stack, (size_t)st_cap * sizeof *stack); }
            stack[st_n].node = nd; mat4_identity(stack[st_n].m); st_n++;
        }
    } else {
        // нет сцен — обходим все nodes как плоский список
        for (int i = 0; i < json_array_len(nodes); i++) {
            if (st_n >= st_cap) { st_cap = st_cap ? st_cap * 2 : 64; stack = realloc(stack, (size_t)st_cap * sizeof *stack); }
            stack[st_n].node = json_array_get(nodes, i); mat4_identity(stack[st_n].m); st_n++;
        }
    }

    while (st_n > 0 && ok) {
        struct StackItem it = stack[--st_n];
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
                    int iuv = (int)json_num(attrs ? json_find(attrs, "TEXCOORD_0") : NULL, -1);
                    int iidx = (int)json_num(json_find(prim, "indices"), -1);
                    if (ipos < 0 || ipos >= acc_n) { ok = 0; err = "нет POSITION"; break; }
                    GlbAcc *ap = &accs[ipos];
                    GlbAcc *an = (inrm >= 0 && inrm < acc_n) ? &accs[inrm] : NULL;
                    GlbAcc *au = (iuv >= 0 && iuv < acc_n) ? &accs[iuv] : NULL;
                    GlbAcc *ai = (iidx >= 0 && iidx < acc_n) ? &accs[iidx] : NULL;
                    long nv = ap->count;

                    Bucket *bk = b_get(&buckets, &bk_n, &bk_cap, mat_idx);
                    unsigned int base = (unsigned int)bk->vn;
                    long pos_base = (ap->bufferView >= 0 && ap->bufferView < bv_n)
                                    ? bvs[ap->bufferView].byteOffset + ap->byteOffset : 0;
                    long nrm_base = an && an->bufferView >= 0 ? bvs[an->bufferView].byteOffset + an->byteOffset : 0;
                    long uv_base = au && au->bufferView >= 0 ? bvs[au->bufferView].byteOffset + au->byteOffset : 0;
                    for (long v = 0; v < nv; v++) {
                        float p[3], n[3] = {0, 1, 0}, u[2] = {0, 0};
                        read_vertex(bin, pos_base, ap, (int)v, p);
                        mat4_transform_point(world, p, p);
                        if (an) { read_vertex(bin, nrm_base, an, (int)v, n); mat4_transform_nrm(world, n, n); }
                        if (au) read_vertex(bin, uv_base, au, (int)v, u);
                        b_push_v(bk, p[0], p[1], p[2], n[0], n[1], n[2], u[0], u[1]);
                    }
                    if (ai) {
                        long idx_base = ai->bufferView >= 0 && ai->bufferView < bv_n
                                        ? bvs[ai->bufferView].byteOffset + ai->byteOffset : 0;
                        for (long i = 0; i < ai->count; i++)
                            b_push_i(bk, base + (unsigned int)read_comp(bin, idx_base, ai, (int)i, 0));
                    } else {
                        for (long i = 0; i < nv; i++) b_push_i(bk, base + (unsigned int)i);
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
                if (st_n >= st_cap) { st_cap = st_cap ? st_cap * 2 : 64; stack = realloc(stack, (size_t)st_cap * sizeof *stack); }
                stack[st_n].node = cnd; memcpy(stack[st_n].m, world, sizeof world); st_n++;
            }
        }
    }
    free(stack);
    free(accs);

    if (!ok || bk_n == 0) {
        SDL_Log("glb: %s — %s", path, err[0] ? err : "нет примитивов");
        for (int i = 0; i < bk_n; i++) b_free(&buckets[i]);
        free(buckets);
        free(bvs);
        json_free();
        free(file);
        return NULL;
    }

    // --- Материалы (псевдо-PBR 2004) + альбедо ----------------------------------
    GlbModel *model = calloc(1, sizeof *model);
    model->items = calloc((size_t)bk_n, sizeof *model->items);
    model->count = bk_n;
    for (int bi = 0; bi < bk_n; bi++) {
        GlbItem *o = &model->items[bi];
        Bucket *bk = &buckets[bi];
        o->verts = bk->verts; o->normals = bk->nrm; o->uvs = bk->uv;
        o->vcount = bk->vn; o->indices = bk->idx; o->index_count = bk->idxn;
        o->tint[0] = o->tint[1] = o->tint[2] = 1.0f;
        o->alpha = 1.0f; o->blend = MAT_BLEND_OPAQUE; o->cull = MAT_CULL_BACK;
        o->tile[0] = o->tile[1] = 1.0f;
        o->specular = 0.2f; o->wrap = MAT_WRAP_REPEAT; o->filter = MAT_FILTER_MIPMAP;

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
            o->cull = json_bool(json_find(mt, "doubleSided"), 0) ? MAT_CULL_NONE : MAT_CULL_BACK;
            const JsonNode *em = json_find(mt, "emissiveFactor");
            if (em && em->type == JSON_ARRAY && json_array_len(em) >= 3) {
                o->tint[0] += (float)json_num(json_array_get(em, 0), 0);
                o->tint[1] += (float)json_num(json_array_get(em, 1), 0);
                o->tint[2] += (float)json_num(json_array_get(em, 2), 0);
            }
            if (json_find(mt, "normalTexture"))
                SDL_Log("glb: %s — normalTexture игнор (псевдо-PBR 2004)", path);
            load_albedo(o, mi, materials, textures, images, samplers, bvs, bv_n, bin);
        }
        // Данные переданы GlbItem'у — не освобождать повторно в b_free
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

// --- Узел-инстанс (GL) ---------------------------------------------------------
typedef struct {
    GLuint vao, vbo, ebo;
    int index_count;
    GLenum index_type;
    GLuint tex;
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
} GlbParams;

static DrawItem *build_item(const GlbItem *gi, GLuint prog)
{
    DrawItem *d = calloc(1, sizeof *d);
    glGenVertexArrays(1, &d->vao);
    glBindVertexArray(d->vao);
    glGenBuffers(1, &d->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, d->vbo);
    const size_t elem = 8 * sizeof(float);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)((size_t)gi->vcount * elem), NULL, GL_STATIC_DRAW);
    float *cpu = malloc((size_t)gi->vcount * 8 * sizeof(float));
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

SceneNode *glb_make_node(const char *path, const float pos[3], float scale,
                         const RendererContext *rc)
{
    GlbModel *gm = glb_parse_file(path);
    if (!gm) return NULL;
    GLuint prog = make_program("shaders/mesh.vs", "shaders/mesh.fs");
    if (!prog) { glb_model_free(gm); return NULL; }

    GlbParams *p = calloc(1, sizeof *p);
    p->rc = rc;
    p->scale = scale > 0 ? scale : 1.0f;
    p->prog = prog;
    scene_uniforms_init(&p->su, prog);
    p->u_model = glGetUniformLocation(prog, "uModel");
    p->u_tex = glGetUniformLocation(prog, "uTex");
    p->u_specular = glGetUniformLocation(prog, "uSpecular");

    p->items = calloc((size_t)gm->count, sizeof *p->items);
    p->count = gm->count;
    for (int i = 0; i < gm->count; i++) p->items[i] = *build_item(&gm->items[i], prog);
    glb_model_free(gm);

    glUseProgram(prog);
    if (p->u_tex >= 0) glUniform1i(p->u_tex, 0);
    for (int c = 0; c < SHADOW_CASCADES; c++) {
        if (p->su.u_shadowmap[c] >= 0) glUniform1i(p->su.u_shadowmap[c], 3 + c);
        if (p->su.u_shadowdepth[c] >= 0) glUniform1i(p->su.u_shadowdepth[c], 3 + c);
    }
    glUseProgram(0);

    float size[3] = {scale, scale, scale};
    return node_new("GlbInstance", "GlbInstance3D", pos, size, p, NULL, glb_node_render);
}

void glb_node_render(SceneNode *n, float dt)
{
    (void)dt;
    GlbParams *p = n->params;
    if (!p || p->rc->pass == RP_SHADOW) return;

    float model[16] = {
        p->scale, 0, 0, 0,
        0, p->scale, 0, 0,
        0, 0, p->scale, 0,
        n->pos[0], n->pos[1], n->pos[2], 1
    };
    glEnable(GL_DEPTH_TEST);
    glUseProgram(p->prog);
    scene_uniforms_bind(&p->su, p->rc);
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

// --- Освобождение ---------------------------------------------------------------
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