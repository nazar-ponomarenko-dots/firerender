// ============================================================================
//  attach.c — attaching nodes to models/bones + the Sprite3D node (see attach.h)
// ============================================================================
#include "attach.h"

#include "events.h"
#include "glb_anim.h"
#include "material.h"
#include "resources.h"
#include "shader.h"
#include "shadow.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

// --- Attaching -----------------------------------------------------------------
int attach_bone_count(SceneNode *model)
{
    return glb_anim_bone_count(model);
}

const char *attach_bone_name(SceneNode *model, int idx)
{
    return glb_anim_bone_name(model, idx);
}

int attach_bone_index(SceneNode *model, const char *bone)
{
    if (!bone || !bone[0]) return ATTACH_ORIGIN;
    return glb_anim_bone_find(model, bone);
}

int attach_node(SceneNode *parent, SceneNode *child, const char *bone,
                const float offset[3], int upright)
{
    if (!parent || !child) return -1;
    int slot = ATTACH_ORIGIN;
    int rc = 0;
    if (bone && bone[0]) {
        slot = attach_bone_index(parent, bone);
        if (slot < 0) {
            SDL_Log("attach: кость '%s' не найдена у %s — цепляю к origin",
                    bone, parent->name ? parent->name : "?");
            slot = ATTACH_ORIGIN;
            rc = -1;
        }
    }
    // Bone -> parent hook is mandatory. For non-skeletal nodes the slot stays
    // ATTACH_ORIGIN and the transform is simply computed from the parent.
    if (slot >= 0 && !parent->attach_fn) {
        SDL_Log("attach: у %s нет точек крепления — цепляю к origin",
                parent->name ? parent->name : "?");
        slot = ATTACH_ORIGIN;
        rc = -1;
    }
    child->attach_slot = slot;
    child->attach_upright = upright ? 1 : 0;
    if (offset) {
        child->pos[0] = offset[0];
        child->pos[1] = offset[1];
        child->pos[2] = offset[2];
    }
    node_add(parent, child);
    return rc;
}

void attach_detach(SceneNode *child)
{
    if (!child) return;
    child->attach_slot = ATTACH_ORIGIN;
    child->attach_upright = 0;
}

// --- Sprite3D node --------------------------------------------------------------
typedef struct {
    const RendererContext *rc;
    GLuint prog, vao, vbo, ibo, tex;
    SceneUniforms su;
    MaterialLocs ml;
    Material mat;
    GLint u_model, u_size, u_billboard, u_camright, u_camup, u_tex, u_unlit;
    float w, h;
    int billboard;
    int unlit;
    float time;
} SpriteParams;

static SpriteParams *sprite_params(SceneNode *n)
{
    if (!n || !n->type || strcmp(n->type, "Sprite3D") != 0) return NULL;
    return n->params;
}

static void sprite_prog_init(SpriteParams *p)
{
    scene_uniforms_init(&p->su, p->prog);
    material_locs_init(&p->ml, p->prog);
    p->u_model     = glGetUniformLocation(p->prog, "uModel");
    p->u_size      = glGetUniformLocation(p->prog, "uSize");
    p->u_billboard = glGetUniformLocation(p->prog, "uBillboard");
    p->u_camright  = glGetUniformLocation(p->prog, "uCamRight");
    p->u_camup     = glGetUniformLocation(p->prog, "uCamUp");
    p->u_tex       = glGetUniformLocation(p->prog, "uTex");
    p->u_unlit     = glGetUniformLocation(p->prog, "uUnlit");
    glUseProgram(p->prog);
    if (p->u_tex >= 0) glUniform1i(p->u_tex, 0);
    for (int c = 0; c < SHADOW_CASCADES; c++) {
        if (p->su.u_shadowmap[c] >= 0) glUniform1i(p->su.u_shadowmap[c], 3 + c);
        if (p->su.u_shadowdepth[c] >= 0) glUniform1i(p->su.u_shadowdepth[c], 3 + c);
    }
    glUseProgram(0);
}

static void sprite_node_exit(const Event *e, void *user)
{
    (void)user;
    if (!e || !e->a) return;
    const SceneNode *n = e->a;
    if (!n->type || strcmp(n->type, "Sprite3D") != 0) return;
    SpriteParams *p = n->params;
    if (!p) return;
    if (p->tex) resources_texture_release(p->tex);
    if (p->prog) resources_program_release(p->prog);
    glDeleteBuffers(1, &p->vbo);
    glDeleteBuffers(1, &p->ibo);
    glDeleteVertexArrays(1, &p->vao);
}

SceneNode *sprite_make_node(const char *tex_path, float w, float h,
                            int billboard, const RendererContext *rc)
{
    static int s_registered = 0;
    if (!s_registered) {
        events_listen(EV_NODE_EXIT, sprite_node_exit, NULL);
        s_registered = 1;
    }
    GLuint prog = make_program("shaders/sprite.vs", "shaders/sprite.fs");
    if (!prog) return NULL;
    SpriteParams *p = calloc(1, sizeof *p);
    if (!p) { resources_program_release(prog); return NULL; }
    p->rc = rc;
    p->prog = prog;
    p->w = w > 0.0f ? w : 1.0f;
    p->h = h > 0.0f ? h : 1.0f;
    p->billboard = billboard ? 1 : 0;
    p->tex = resources_texture(tex_path ? tex_path : "img/notfound.png", 4,
                               GL_CLAMP_TO_EDGE, 1);
    material_defaults(&p->mat);
    p->mat.blend = MAT_BLEND_ALPHA;    // sprites are mostly semi-transparent
    p->mat.cull = MAT_CULL_NONE;
    p->mat.set = 1;

    // Quad -0.5..0.5 with UV 0..1
    glGenVertexArrays(1, &p->vao);
    glBindVertexArray(p->vao);
    glGenBuffers(1, &p->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, p->vbo);
    const float quad[4][4] = {
        { -0.5f, -0.5f, 0.0f, 1.0f },
        {  0.5f, -0.5f, 1.0f, 1.0f },
        {  0.5f,  0.5f, 1.0f, 0.0f },
        { -0.5f,  0.5f, 0.0f, 0.0f },
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

    sprite_prog_init(p);
    float size[3] = { p->w, p->h, 0.0f };
    SceneNode *node = node_new("Sprite3D", "Sprite3D", NULL, size, p, NULL,
                               sprite_node_render);
    if (!node) {
        if (p->tex) resources_texture_release(p->tex);
        resources_program_release(p->prog);
        glDeleteBuffers(1, &p->vbo);
        glDeleteBuffers(1, &p->ibo);
        glDeleteVertexArrays(1, &p->vao);
        free(p);
    }
    return node;
}

void sprite_set_size(SceneNode *n, float w, float h)
{
    SpriteParams *p = sprite_params(n);
    if (!p) return;
    if (w > 0.0f) p->w = w;
    if (h > 0.0f) p->h = h;
    n->size[0] = p->w;
    n->size[1] = p->h;
}

void sprite_set_tint(SceneNode *n, float r, float g, float b, float alpha)
{
    SpriteParams *p = sprite_params(n);
    if (!p) return;
    p->mat.tint[0] = r;
    p->mat.tint[1] = g;
    p->mat.tint[2] = b;
    p->mat.alpha = alpha;
}

void sprite_set_billboard(SceneNode *n, int on)
{
    SpriteParams *p = sprite_params(n);
    if (p) p->billboard = on ? 1 : 0;
}

void sprite_set_frame(SceneNode *n, int cols, int rows, int index)
{
    SpriteParams *p = sprite_params(n);
    if (!p || cols < 1 || rows < 1) return;
    int total = cols * rows;
    if (index < 0) index = 0;
    index %= total;
    p->mat.tile[0] = 1.0f / (float)cols;
    p->mat.tile[1] = 1.0f / (float)rows;
    p->mat.offset[0] = (float)(index % cols) / (float)cols;
    p->mat.offset[1] = (float)(index / cols) / (float)rows;
}

void sprite_set_additive(SceneNode *n, int on)
{
    SpriteParams *p = sprite_params(n);
    if (!p) return;
    p->mat.blend = on ? MAT_BLEND_ADDITIVE : MAT_BLEND_ALPHA;
    // Additive sprites don't write depth: they "glow" over the scene
    p->mat.depth_write = on ? 0 : 1;
}

void sprite_set_unlit(SceneNode *n, int on)
{
    SpriteParams *p = sprite_params(n);
    if (p) p->unlit = on ? 1 : 0;
}

void sprite_node_render(SceneNode *n, float dt)
{
    SpriteParams *p = sprite_params(n);
    if (!p) return;
    const RendererContext *rc = p->rc;
    if (rc->pass == RP_SHADOW) return;    // sprites don't write into shadow maps
    p->time += dt;
    // Culling: sphere with radius along the longer side (node scale accounted for)
    float sx = n->world[0] * n->world[0] + n->world[1] * n->world[1]
             + n->world[2] * n->world[2];
    float r = (p->w > p->h ? p->w : p->h) * 0.5f * (sx > 0.0f ? sqrtf(sx) : 1.0f);
    if (!frustum_test_sphere(&rc->frustum, n->world[12], n->world[13],
                             n->world[14], r + 0.5f))
        return;

    glEnable(GL_DEPTH_TEST);
    glUseProgram(p->prog);
    scene_uniforms_bind(&p->su, rc);
    glUniformMatrix4fv(p->u_model, 1, GL_FALSE, n->world);
    if (p->u_size >= 0) glUniform2f(p->u_size, p->w, p->h);
    if (p->u_billboard >= 0)
        glUniform1f(p->u_billboard, p->billboard ? 1.0f : 0.0f);
    // Basis of the VIEW camera: the billboard faces where the frame is rendered
    // from (in third person that's the camera behind the character, not the character).
    if (p->u_camright >= 0)
        glUniform3f(p->u_camright, rc->view_right[0], rc->view_right[1],
                    rc->view_right[2]);
    if (p->u_camup >= 0)
        glUniform3f(p->u_camup, rc->view_up[0], rc->view_up[1], rc->view_up[2]);
    if (p->u_unlit >= 0) glUniform1f(p->u_unlit, p->unlit ? 1.0f : 0.0f);
    material_bind(&p->mat, &p->ml, p->time);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, p->tex);
    glBindVertexArray(p->vao);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, NULL);
    glBindVertexArray(0);
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glUseProgram(0);
}
