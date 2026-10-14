// ============================================================================
//  objects.c — objects from map.json: billboards (trees and the like)
// ----------------------------------------------------------------------------
//  Reads objects[] from map.json: each object is a quad (or a cross of quads)
//  with a texture (model — path to a PNG, e.g. "img/tree.png"), position pos
//  and scale scale. By default — a CROSS of two static quads; billboard=1 —
//  a single quad always facing the camera. Quad height = 5 m * scale.y
//  (OBJECT_BASE_H), width — from the texture proportions. The base sits on
//  the terrain (if y <= 0). Shader: built-in default/billboard/flat or a
//  CUSTOM one (vs/fs from the map) — the uniforms are the same as in
//  object.vs/object.fs (mandatory uTex/uTint/uObjPos/uObjSize/uObjRight/uViewProj,
//  the rest need not be declared). collision=1 — the object is impenetrable
//  (a cylinder of given radius around the base; objects_collide pushes the
//  player out).
//  Lighting — same as vegetation: all directional sources from
//  map.json + CSM sun shadows + fog.
// ============================================================================
#include "objects.h"

#include "assets.h"
#include "bloom.h"
#include "cvars.h"
#include "gl_loader.h"
#include "linalg.h"
#include "mapdata.h"
#include "material.h"
#include "phys_body.h"
#include "resources.h"
#include "shader.h"
#include "shadow.h"
#include "world.h"

#include "stb_image.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#define OBJECT_MAX 256
#define OBJECT_BASE_H 9.0f     // quad height at scale.y = 1

// Uniform locations of the object shader (shared across all programs):
// the common scene set (SceneUniforms) + object geometry + material.
typedef struct {
    SceneUniforms su;            // common scene uniform set (render_ctx.h)
    GLint u_objpos, u_objsize, u_objright, u_objslope, u_tex;
    MaterialLocs mat;            // material (material.h)
} ObjLocs;

typedef struct {
    GLuint tex;
    GLuint tex_custom;       // own texture (custom wrap/filter)
    GLuint prog;             // own shader (0 = shared p->prog)
    ObjLocs locs;            // uniform locations for prog
    int billboard;           // quad faces the camera (instead of a cross)
    int src;                 // index into map.json (objects[]): instances are
                             // packed tighter than mapdata (empty model entries
                             // are skipped), so they map back only through src
    float pos[3];
    float slope[2];          // ground slope under the object (dh/dx, dh/dz):
                             // the cross base lies on the terrain, like grass
    float width, height;
    int collision;           // impenetrable cylinder
    float radius;            // collision radius, m
    Material r;              // material (material.h)
} ObjectInst;

typedef struct {
    const WorldState *world; // world data: heights (placing onto the terrain)
    const RendererContext *rc;   // per-frame data (renderer.c)
    GLuint prog, vao, vbo, ibo;
    GLuint flat_prog;        // built-in "flat" (unlit)
    GLuint shadow_prog;      // depth into shadow maps
    GLint s_viewproj, s_objpos, s_objsize, s_objright, s_objslope;
    // Blob shadows (r_shadows_mode 3): a soft patch under the object
    GLuint blob_prog, blob_tex;
    GLint b_viewproj, b_objpos, b_objsize, b_objright, b_objfwd, b_tex, b_color;
    int count;
    ObjectInst inst[OBJECT_MAX];
    ObjLocs locs;            // uniform locations of the shared programs (p->prog)
    ObjLocs flat_locs;
} ObjectsParams;

// Object collision cylinders (for camera.c — pushing the player out)
struct ObjCol { float x, z, r; float base, h; };   // base..base+h — for rigid bodies
static struct ObjCol s_col[OBJECT_MAX];
static int s_col_n = 0;
// Collision squares from collisions[] (XZ rectangles)
struct ObjBox { float x, z, hw, hz; float base, h; };
static struct ObjBox s_box[OBJECT_MAX];
static int s_box_n = 0;
static ObjectsParams *s_objs = NULL;   // for hot-reload (shader_on_reshade)
// Time (for UV animation)
static float s_time = 0.0f;

// The very same cylinders/squares the player is pushed out of also hand their
// volume to rigid bodies (phys_body): otherwise a crate flies straight
// through a tree.
static void objects_sync_phys_statics(void)
{
    for (int i = 0; i < s_col_n; i++)
        phys_static_add_cylinder(s_col[i].x, s_col[i].z, s_col[i].r,
                                 s_col[i].base, s_col[i].base + s_col[i].h);
    for (int i = 0; i < s_box_n; i++) {
        float c[3] = { s_box[i].x, s_box[i].base + s_box[i].h * 0.5f,
                       s_box[i].z };
        float e[3] = { s_box[i].hw, s_box[i].h * 0.5f, s_box[i].hz };
        phys_static_add_box(c, e);
    }
}

// Fills in a program's uniform locations (optional ones = -1)
static ObjLocs obj_locs(GLenum prog)
{
    ObjLocs l;
    memset(&l, 0, sizeof l);
    scene_uniforms_init(&l.su, prog);      // common scene uniform set
    material_locs_init(&l.mat, prog);      // material (tint/blend/UV)
    l.u_objpos   = glGetUniformLocation(prog, "uObjPos");
    l.u_objsize  = glGetUniformLocation(prog, "uObjSize");
    l.u_objright = glGetUniformLocation(prog, "uObjRight");
    l.u_objslope = glGetUniformLocation(prog, "uObjSlope");
    l.u_tex      = glGetUniformLocation(prog, "uTex");
    return l;
}

// Recreates the object programs and re-looks-up their uniforms (for hot-reload).
// replace balances the refcount (release the old one + take a new reference);
// if the pair fails to link it returns 0 and the old id is kept. We recreate
// ALL of the node's programs (including flat/shadow/blob and each instance's
// custom one), otherwise after a reload some objects would still draw with
// the old shader and its reference would leak forever (the old code did not
// even re-request flat/shadow).
static void objects_reshade(void)
{
    ObjectsParams *p = s_objs;
    if (!p) return;
    GLuint prog = resources_program_replace(p->prog,
                                            "shaders/object.vs",
                                            "shaders/object.fs");
    if (prog) p->prog = prog;
    p->locs = obj_locs(p->prog);
    if (p->flat_prog) {
        GLuint f = resources_program_replace(p->flat_prog,
                                             "shaders/object.vs",
                                             "shaders/object_flat.fs");
        if (f) p->flat_prog = f;
        p->flat_locs = obj_locs(p->flat_prog);
    }
    {
        GLuint sp = resources_program_replace(p->shadow_prog,
                                              "shaders/object_shadow.vs",
                                              "shaders/object_shadow.fs");
        if (sp) p->shadow_prog = sp;
    }
    if (p->blob_prog) {
        GLuint bp = resources_program_replace(p->blob_prog,
                                              "shaders/blob.vs",
                                              "shaders/blob.fs");
        if (bp) p->blob_prog = bp;
    }
    glUseProgram(p->prog);
    glUniform1i(p->locs.u_tex, 0);
    for (int c = 0; c < SHADOW_CASCADES; c++) {
        if (p->locs.su.u_shadowmap[c] >= 0)
            glUniform1i(p->locs.su.u_shadowmap[c], 3 + c);
        if (p->locs.su.u_shadowdepth[c] >= 0)
            glUniform1i(p->locs.su.u_shadowdepth[c], 3 + c);
    }
    glUseProgram(0);
    for (int i = 0; i < p->count; i++) {
        ObjectInst *it = &p->inst[i];
        if (!it->prog) continue;
        const MapObject *o = mapdata_object(it->src);
        if (o && o->shader_kind == OBJ_SHADER_CUSTOM) {
            const char *vs = o->shader_vs[0] ? o->shader_vs
                                             : "shaders/object.vs";
            GLuint cp = resources_program_replace(it->prog, vs, o->shader_fs);
            if (cp) it->prog = cp;
        }
        it->locs = obj_locs(it->prog);
        glUseProgram(it->prog);
        if (it->locs.u_tex >= 0) glUniform1i(it->locs.u_tex, 0);
        for (int c = 0; c < SHADOW_CASCADES; c++) {
            if (it->locs.su.u_shadowmap[c] >= 0)
                glUniform1i(it->locs.su.u_shadowmap[c], 3 + c);
            if (it->locs.su.u_shadowdepth[c] >= 0)
                glUniform1i(it->locs.su.u_shadowdepth[c], 3 + c);
        }
        glUseProgram(0);
    }
    p->s_viewproj = glGetUniformLocation(p->shadow_prog, "uViewProj");
    p->s_objpos   = glGetUniformLocation(p->shadow_prog, "uObjPos");
    p->s_objsize  = glGetUniformLocation(p->shadow_prog, "uObjSize");
    p->s_objright = glGetUniformLocation(p->shadow_prog, "uObjRight");
    p->s_objslope = glGetUniformLocation(p->shadow_prog, "uObjSlope");
}

// Per-frame uniforms (views, light, shadows, fog) — on program switch
static void set_per_frame(const ObjLocs *l, const ObjectsParams *p)
{
    const RendererContext *rc = p->rc;
    // Common set: views, light, environment, fog, shadow cascades (from rc)
    scene_uniforms_bind(&l->su, rc);
    // Objects: bias like vegetation (shadow_bias_veg from map.json),
    // gradient over ~5 m into the shadow
    for (int c = 0; c < SHADOW_CASCADES; c++) {
        if (l->su.u_shadowbias[c] >= 0)
            glUniform1f(l->su.u_shadowbias[c], rc->shadow_bias_veg[c]);
        if (l->su.u_shadowgrad[c] >= 0)
            glUniform1f(l->su.u_shadowgrad[c], 5.0f / rc->shadow_depth[c]);
    }
    // Material (tint/blend/UV animation) is applied per instance via material_bind
}

// Texture copy with other parameters (wrap/filter) — the engine cache is left alone
static GLuint texture_custom(const unsigned char *img, int w, int h,
                             int wrap, int filter)
{
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    GLint wmode = GL_CLAMP_TO_EDGE, fmag = GL_LINEAR, fmin = GL_LINEAR;
    if (wrap == MAT_WRAP_REPEAT) wmode = GL_REPEAT;
    else if (wrap == MAT_WRAP_MIRROR) wmode = GL_MIRRORED_REPEAT;
    if (filter == MAT_FILTER_NEAREST) fmag = fmin = GL_NEAREST;
    else if (filter == MAT_FILTER_MIPMAP) fmin = GL_LINEAR_MIPMAP_LINEAR;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wmode);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wmode);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, fmin);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, fmag);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, img);
    if (filter == MAT_FILTER_MIPMAP) glGenerateMipmap(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
}

static void objects_params_release(ObjectsParams *p)
{
    if (!p) return;
    if (s_objs == p) s_objs = NULL;
    s_col_n = 0;
    s_box_n = 0;
    phys_static_clear();   // objects torn down — their colliders are no longer live
    for (int i = 0; i < p->count; i++) {
        if (p->inst[i].tex_custom)
            glDeleteTextures(1, &p->inst[i].tex_custom);
        else if (p->inst[i].tex)
            resources_texture_release(p->inst[i].tex);
        if (p->inst[i].prog) resources_program_release(p->inst[i].prog);
    }
    if (p->prog) resources_program_release(p->prog);
    if (p->flat_prog) resources_program_release(p->flat_prog);
    if (p->shadow_prog) resources_program_release(p->shadow_prog);
    if (p->blob_prog) resources_program_release(p->blob_prog);
    if (p->blob_tex) resources_texture_release(p->blob_tex);
    if (p->vbo) glDeleteBuffers(1, &p->vbo);
    if (p->ibo) glDeleteBuffers(1, &p->ibo);
    if (p->vao) glDeleteVertexArrays(1, &p->vao);
}

SceneNode *objects_make_node(const WorldState *ws, const RendererContext *rc)
{
    ObjectsParams *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->world = ws;
    p->rc = rc;

    // Objects from map.json: PNG path — model, position — pos,
    // scale — scale. y <= 0 — height taken from the terrain.
    int n = mapdata_object_count();
    if (n > OBJECT_MAX) n = OBJECT_MAX;
    s_col_n = 0;
    s_box_n = 0;
    phys_static_clear();   // new map — new statics for rigid bodies
    for (int i = 0; i < n; i++) {
        const MapObject *o = mapdata_object(i);
        if (!o || !o->model[0]) continue;
        ObjectInst *it = &p->inst[p->count];
        it->src = i;             // remember the map.json index (instances are denser)
        int w = 0, h = 0;
        // Pixels are needed for the proportions and custom textures; the texture
        // goes through the resource cache (60 tree.png trees = ONE GL texture).
        unsigned char *img = load_img(o->model, &w, &h, STBI_rgb_alpha);
        if (!img || w <= 0 || h <= 0) {
            SDL_Log("Object %s: cannot load texture %s (will use notfound)", o->name, o->model);
            w = 64; h = 64;   // fallback proportions
        }
        // Material: defaults + overrides from the render block in JSON
        material_defaults(&it->r);
        if (o->render.set) it->r = o->render;
        // Own texture when non-default wrap/filter are set
        if (it->r.set &&
            (it->r.wrap != MAT_WRAP_CLAMP || it->r.filter != MAT_FILTER_LINEAR)) {
            it->tex_custom = texture_custom(img, w, h, it->r.wrap, it->r.filter);
            it->tex = it->tex_custom;
            SDL_Log("Object %s: custom texture (wrap=%d filter=%d)", o->name,
                    it->r.wrap, it->r.filter);
        } else {
            it->tex = resources_texture(o->model, 4, GL_CLAMP_TO_EDGE, 0);
        }
        it->pos[0] = o->pos[0];
        it->pos[1] = o->pos[1];
        it->pos[2] = o->pos[2];
        if (it->pos[1] <= 0.0f)   // y = 0 — place onto the terrain
            it->pos[1] = sample_height(ws, it->pos[0], it->pos[2]);
        // Ground slope under the object: the cross base lies on the terrain
        // along the slope (like the vegetation base in wheat.vs). Otherwise a
        // ~12.5 m wide tree on a hill hung one corner in the air while the
        // other sank into the ground.
        {
            const float e = 1.0f;         // slope sampling step, m
            float sdx = sample_height(ws, it->pos[0] + e, it->pos[2])
                      - sample_height(ws, it->pos[0] - e, it->pos[2]);
            float sdz = sample_height(ws, it->pos[0], it->pos[2] + e)
                      - sample_height(ws, it->pos[0], it->pos[2] - e);
            it->slope[0] = sdx / (2.0f * e);
            it->slope[1] = sdz / (2.0f * e);
        }
        float sy = o->scale[1] > 0.01f ? o->scale[1] : 1.0f;
        if (it->r.set && it->r.fixed_size) {
            it->height = it->r.height * sy;
            it->width = it->r.width * (o->scale[0] > 0.01f ? o->scale[0] : 1.0f);
        } else {
            it->height = OBJECT_BASE_H * sy;
            it->width = it->height * (float)w / (float)h;
        }
        // Billboard/collision/shader
        it->billboard = o->billboard;
        it->collision = o->collision;
        it->radius = o->radius > 0.01f ? o->radius : 1.5f;
        if (o->shader_kind == OBJ_SHADER_BILLBOARD) it->billboard = 1;
        if (it->collision && s_col_n < OBJECT_MAX) {
            s_col[s_col_n].x = it->pos[0];
            s_col[s_col_n].z = it->pos[2];
            s_col[s_col_n].r = it->radius;
            s_col[s_col_n].base = it->pos[1];
            s_col[s_col_n].h = it->height;
            s_col_n++;
        }
        p->count++;
        SDL_Log("Object: %s (%s) at %.0f,%.0f h=%.1fm%s%s%s", o->name, o->model,
                it->pos[0], it->pos[2], it->height,
                it->billboard ? " billboard" : "",
                it->collision ? " collision" : "",
                it->r.set ? " render" : "");
    }
    if (p->count == 0) {
        SDL_Log("Objects: no objects in map.json");
        free(p);
        return NULL;
    }

    // --- Collision squares from collisions[]: bound to objects by name -----
    for (int i = 0; i < mapdata_collision_count() && s_box_n < OBJECT_MAX; i++) {
        const MapCollision *c = mapdata_collision(i);
        if (!c || c->size[0] <= 0.01f || c->size[1] <= 0.01f) continue;
        float x = c->pos[0], z = c->pos[2];
        if (c->object[0]) {            // bound to an object: base = the object
            int found = 0;
            for (int k = 0; k < p->count; k++) {
                const MapObject *o = mapdata_object(p->inst[k].src);
                if (!o || strcmp(o->name, c->object) != 0) continue;
                x = p->inst[k].pos[0] + c->pos[0];
                z = p->inst[k].pos[2] + c->pos[2];
                found = 1;
                break;
            }
            if (!found) {
                SDL_Log("Collision: object \"%s\" not found, using absolute pos",
                        c->object);
            }
        }
        s_box[s_box_n].x = x;
        s_box[s_box_n].z = z;
        s_box[s_box_n].hw = c->size[0] * 0.5f;
        s_box[s_box_n].hz = c->size[1] * 0.5f;
        // The player is blocked on XZ only (height is irrelevant to it), but
        // rigid bodies need a volume: base — terrain under the square, h —
        // height from JSON (0 = stub).
        s_box[s_box_n].base = sample_height(ws, x, z);
        s_box[s_box_n].h = c->height > 0.01f ? c->height : 2.0f;
        s_box_n++;
        SDL_Log("Collision: box at %.0f,%.0f size %.0fx%.0f%s%s", x, z,
                c->size[0], c->size[1],
                c->object[0] ? " -> " : "", c->object[0] ? c->object : "");
    }

    // Map collisions → static physics bodies (after both registries).
    objects_sync_phys_statics();

    // --- Shaders ---
    p->prog = make_program("shaders/object.vs", "shaders/object.fs");
    if (!p->prog) { objects_params_release(p); free(p); return NULL; }
    p->locs = obj_locs(p->prog);
    p->flat_prog = make_program("shaders/object.vs", "shaders/object_flat.fs");
    if (p->flat_prog) p->flat_locs = obj_locs(p->flat_prog);
    glUseProgram(p->prog);
    glUniform1i(p->locs.u_tex, 0);
    // Cascades on units 3-6 (uShadowMap%d and raw depth uShadowDepth%d)
    for (int c = 0; c < SHADOW_CASCADES; c++) {
        if (p->locs.su.u_shadowmap[c] >= 0)
            glUniform1i(p->locs.su.u_shadowmap[c], 3 + c);
        if (p->locs.su.u_shadowdepth[c] >= 0)
            glUniform1i(p->locs.su.u_shadowdepth[c], 3 + c);
    }
    glUseProgram(0);
    // Objects' own shaders (custom): a separate program per object.
    // The instance index is NOT the map.json index (empty model entries are
    // skipped) — so mapdata_object is fetched via the saved src.
    for (int i = 0; i < p->count; i++) {
        const MapObject *o = mapdata_object(p->inst[i].src);
        if (!o || o->shader_kind != OBJ_SHADER_CUSTOM) continue;
        ObjectInst *it = &p->inst[i];
        const char *vs = o->shader_vs[0] ? o->shader_vs : "shaders/object.vs";
        it->prog = make_program(vs, o->shader_fs);
        if (!it->prog) {
            SDL_Log("Object %s: custom shader failed, using default", o->name);
            continue;
        }
        it->locs = obj_locs(it->prog);
        glUseProgram(it->prog);
        if (it->locs.u_tex >= 0) glUniform1i(it->locs.u_tex, 0);
        for (int c = 0; c < SHADOW_CASCADES; c++) {
            if (it->locs.su.u_shadowmap[c] >= 0)
                glUniform1i(it->locs.su.u_shadowmap[c], 3 + c);
            if (it->locs.su.u_shadowdepth[c] >= 0)
                glUniform1i(it->locs.su.u_shadowdepth[c], 3 + c);
        }
        glUseProgram(0);
    }

    // Shared quad: x -0.5..0.5 (width), y 0..1 (height), uv 0..1
    glGenVertexArrays(1, &p->vao);
    glBindVertexArray(p->vao);
    glGenBuffers(1, &p->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, p->vbo);
    const float quad[4][4] = {
        { -0.5f, 0.0f, 0.0f, 0.0f },
        {  0.5f, 0.0f, 1.0f, 0.0f },
        {  0.5f, 1.0f, 1.0f, 1.0f },
        { -0.5f, 1.0f, 0.0f, 1.0f },
    };
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), NULL);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          (const void *)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glGenBuffers(1, &p->ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, p->ibo);
    const unsigned int idx[6] = { 0, 1, 2, 0, 2, 3 };
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof idx, idx, GL_STATIC_DRAW);
    glBindVertexArray(0);

    // --- Shadow shader (depth with alpha test) ---
    p->shadow_prog = make_program("shaders/object_shadow.vs",
                                  "shaders/object_shadow.fs");
    if (!p->shadow_prog) { objects_params_release(p); free(p); return NULL; }
    p->s_viewproj = glGetUniformLocation(p->shadow_prog, "uViewProj");
    p->s_objpos   = glGetUniformLocation(p->shadow_prog, "uObjPos");
    p->s_objsize  = glGetUniformLocation(p->shadow_prog, "uObjSize");
    p->s_objright = glGetUniformLocation(p->shadow_prog, "uObjRight");
    p->s_objslope = glGetUniformLocation(p->shadow_prog, "uObjSlope");
    glUseProgram(p->shadow_prog);
    glUniform1i(glGetUniformLocation(p->shadow_prog, "uTex"), 0);
    glUseProgram(0);

    // --- Blob shadows (mode 3): a soft patch under every object ------------
    p->blob_prog = make_program("shaders/blob.vs", "shaders/blob.fs");
    if (p->blob_prog) {
        p->b_viewproj = glGetUniformLocation(p->blob_prog, "uViewProj");
        p->b_objpos   = glGetUniformLocation(p->blob_prog, "uObjPos");
        p->b_objsize  = glGetUniformLocation(p->blob_prog, "uObjSize");
        p->b_objright = glGetUniformLocation(p->blob_prog, "uObjRight");
        p->b_objfwd   = glGetUniformLocation(p->blob_prog, "uObjFwd");
        p->b_tex      = glGetUniformLocation(p->blob_prog, "uTex");
        p->b_color    = glGetUniformLocation(p->blob_prog, "uColor");
        p->blob_tex = resources_texture("img/fauna_glow.png", 4,
                                        GL_CLAMP_TO_EDGE, 1);
        glUseProgram(p->blob_prog);
        glUniform1i(p->b_tex, 0);
        glUseProgram(0);
    }

    s_objs = p;                   // for hot-reload
    shader_on_reshade(objects_reshade);

    const float size[3] = { 100.0f, OBJECT_BASE_H * 4.0f, 100.0f };
    SceneNode *node = node_new("Objects", "MeshInstance3D", NULL, size, p,
                               NULL, objects_node_render);
    if (!node) {
        objects_params_release(p);
        free(p);
    }
    return node;
}

// Shadow pass (RP_SHADOW): the same geometry and face choice as in the color
// pass, but with the object_shadow program (depth with alpha test). Only
// objects inside the cascade box. This used to be copy-paste of objects_shadow_render.
static void objects_shadow_draw(ObjectsParams *p, const RendererContext *rc)
{
    if (p->count <= 0) return;
    glDisable(GL_CULL_FACE);   // billboards — drawn from both sides
    glUseProgram(p->shadow_prog);
    glUniformMatrix4fv(p->s_viewproj, 1, GL_FALSE, rc->shadow_vp);
    glBindVertexArray(p->vao);
    for (int i = 0; i < p->count; i++) {
        ObjectInst *it = &p->inst[i];
        // Cull by the cascade box (from the camera). But an object also draws
        // when far away, if its LONG SHADOW reaches the box: the shadow
        // stretches along the sun direction by height*shadow_slope meters.
        // Otherwise the near cascades would not draw far trees even though
        // their shadow lies right next to the player.
        float reach = it->height * rc->shadow_slope + it->width * 0.5f;
        float r = rc->shadow_box_cur * 0.7071f + 6.0f + reach;
        float r2 = r * r;
        float dx = rc->cam_pos[0] - it->pos[0];
        float dz = rc->cam_pos[2] - it->pos[2];
        if (dx * dx + dz * dz > r2) continue;
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, it->tex);
        glUniform3f(p->s_objpos, it->pos[0], it->pos[1], it->pos[2]);
        glUniform2f(p->s_objsize, it->width, it->height);
        if (p->s_objslope >= 0)
            glUniform2f(p->s_objslope, it->slope[0], it->slope[1]);
        // the same face choice as in objects_node_render
        if (it->billboard) {
            float d = sqrtf(dx * dx + dz * dz);
            if (d < 0.001f) d = 1.0f;
            glUniform3f(p->s_objright, -dz / d, 0.0f, dx / d);
        } else {
            if (fabsf(dx) >= fabsf(dz))
                glUniform3f(p->s_objright, 0.0f, 0.0f, 1.0f);
            else
                glUniform3f(p->s_objright, 1.0f, 0.0f, 0.0f);
        }
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, NULL);
    }
}

void objects_node_render(SceneNode *n, float dt)
{
    ObjectsParams *p = n->params;
    if (p->count <= 0) return;
    const RendererContext *rc = p->rc;
    // Shadow pass (RP_SHADOW): depth with alpha test
    if (rc->pass == RP_SHADOW) {
        if (g_shadow_obj) objects_shadow_draw(p, rc);
        return;
    }
    s_time += dt;
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);   // two-sided by default (cull: none)
    glBindVertexArray(p->vao);

    // --- Blob shadows (mode 3): a soft patch on the ground under each object --
    // Patch offset from the sun: the shadow falls along the light direction.
    if (cvar_get("r_shadows_mode") == 3.0f && p->blob_prog) {
        const float *ld = p->rc->light_dir[0];
        float ll = sqrtf(ld[0] * ld[0] + ld[2] * ld[2]);
        float sdx = ll > 0.001f ? ld[0] / ll : 0.0f;   // horizontal toward the sun
        float sdz = ll > 0.001f ? ld[2] / ll : 0.0f;
        const float col[4] = { 0.05f, 0.05f, 0.06f, 0.5f };
        // NO depth writes: the patches are translucent, and writing depth
        // would cut off billboards drawn BEHIND the patch (holes in the grass
        // around tree shadows). Blend over the terrain, depth is still read.
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);
        glUseProgram(p->blob_prog);
        glUniformMatrix4fv(p->b_viewproj, 1, GL_FALSE, p->rc->viewproj);
        glUniform4f(p->b_color, col[0], col[1], col[2], col[3]);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, p->blob_tex);
        for (int i = 0; i < p->count; i++) {
            ObjectInst *it = &p->inst[i];
            // Patch outside the frustum — skip (and don't flood uniforms):
            // without this a weak iGPU chewed through thousands of invisible
            // quads every frame
            if (!frustum_test_sphere(&p->rc->frustum, it->pos[0],
                                     it->pos[1] + it->height * 0.5f,
                                     it->pos[2],
                                     it->height + it->width))
                continue;
            float h = it->height;
            float sz = it->width * 0.65f;
            float px = it->pos[0] - sdx * h * 0.55f;
            float pz = it->pos[2] - sdz * h * 0.55f;
            // the patch sits on the terrain BELOW ITSELF (doesn't sink on slopes)
            float base = sample_height(p->world, px, pz);
            glUniform3f(p->b_objpos, px, base + 0.25f, pz);
            glUniform2f(p->b_objsize, sz, sz);
            glUniform3f(p->b_objright, 1.0f, 0.0f, 0.0f);
            glUniform3f(p->b_objfwd, 0.0f, 0.0f, 1.0f);
            glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, NULL);
        }
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
        glUseProgram(0);
    }

    GLuint cur = 0, cur_tex = 0;
    Material mat_cache;
    int mat_valid = 0;
    // MSAA does not smooth alpha-cutout edges (discard) — for alpha-test
    // materials enable alpha-to-coverage (a semi-transparent texel edge gives
    // partial sample coverage). Leave blending materials alone: there partial
    // coverage would multiply into the blending (a*a at the edges).
    const int msaa_on = bloom_msaa_active() > 0;
    int a2c = 0;
    for (int i = 0; i < p->count; i++) {
        ObjectInst *it = &p->inst[i];
        // Frustum cull: sphere (position + maximum size)
        if (!frustum_test_sphere(&p->rc->frustum, it->pos[0], it->pos[1],
                                 it->pos[2],
                                 it->height + it->width * 0.5f))
            continue;
        GLuint prog = it->prog ? it->prog : p->prog;
        if (prog != cur) {
            cur = prog;
            const ObjLocs *l = it->prog ? &it->locs : &p->locs;
            glUseProgram(prog);
            set_per_frame(l, p);
            mat_valid = 0;
        }
        const ObjLocs *l = it->prog ? &it->locs : &p->locs;

        material_bind_cached(&it->r, &l->mat, s_time, &mat_cache, &mat_valid);

        int want_a2c = msaa_on && mat_cache.blend == MAT_BLEND_ALPHATEST;
        if (want_a2c != a2c) {
            a2c = want_a2c;
            if (a2c) glEnable(GL_SAMPLE_ALPHA_TO_COVERAGE);
            else     glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
        }

        if (it->tex != cur_tex) {
            cur_tex = it->tex;
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, it->tex);
        }
        glUniform3f(l->u_objpos, it->pos[0], it->pos[1], it->pos[2]);
        glUniform2f(l->u_objsize, it->width, it->height);
        if (l->u_objslope >= 0)
            glUniform2f(l->u_objslope, it->slope[0], it->slope[1]);
        // Baked light: sample the probe at the object's mid height (the crown).
        scene_uniforms_probe(&l->su, it->pos[0], it->pos[1] + it->height * 0.5f,
                             it->pos[2]);
        // Width axis of the quad:
        //  - billboard: perpendicular to the direction to the camera (quad faces it);
        //  - cross: static face choice by view angle.
        float dx = p->rc->cam_pos[0] - it->pos[0];
        float dz = p->rc->cam_pos[2] - it->pos[2];
        if (it->billboard) {
            float d = sqrtf(dx * dx + dz * dz);
            if (d < 0.001f) d = 1.0f;
            glUniform3f(l->u_objright, -dz / d, 0.0f, dx / d);
        } else {
            if (fabsf(dx) >= fabsf(dz))
                glUniform3f(l->u_objright, 0.0f, 0.0f, 1.0f);
            else
                glUniform3f(l->u_objright, 1.0f, 0.0f, 0.0f);
        }
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, NULL);
    }
    if (a2c) glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glDisable(GL_CULL_FACE);
    glUseProgram(0);
}

// Push a point out of the map collisions: object cylinders + collisions[]
// squares. A player with radius pr cannot get inside. Modifies *x,*z.
void objects_collide(float *x, float *z, float pr)
{
    for (int i = 0; i < s_col_n; i++) {
        float dx = *x - s_col[i].x;
        float dz = *z - s_col[i].z;
        float rr = s_col[i].r + pr;
        float d2 = dx * dx + dz * dz;
        if (d2 >= rr * rr) continue;
        if (d2 < 1e-6f) {          // standing at the center — push toward X
            *x += rr;
            continue;
        }
        float d = sqrtf(d2);
        float push = (rr - d) / d;
        *x += dx * push;
        *z += dz * push;
    }
    // Squares: push out through the nearest face
    for (int i = 0; i < s_box_n; i++) {
        float hx = s_box[i].hw + pr;
        float hz = s_box[i].hz + pr;
        float dx = *x - s_box[i].x;
        float dz = *z - s_box[i].z;
        if (fabsf(dx) >= hx || fabsf(dz) >= hz) continue;
        float ox = hx - fabsf(dx);   // overlap along X
        float oz = hz - fabsf(dz);   // overlap along Z
        if (ox < oz)                 // push along the smaller overlap
            *x += (dx >= 0.0f ? ox : -ox);
        else
            *z += (dz >= 0.0f ? oz : -oz);
    }
}


void objects_cleanup(SceneNode *n)
{
    if (!n) return;
    ObjectsParams *p = n->params;
    if (!p) return;
    objects_params_release(p);
}
