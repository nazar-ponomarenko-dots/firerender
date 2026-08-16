// ============================================================================
//  decals.c — декали на рельефе
// ----------------------------------------------------------------------------
//  Каждая декаль — квад, проецируемый на рельеф: 9 сэмплов высоты (3x3)
//  для плавного следования за склонами. Полигон-оффсет вместо ручного
//  смещения — без z-fighting и парения.
// ============================================================================
#include "decals.h"

#include "gl_loader.h"
#include "linalg.h"
#include "resources.h"
#include "shader.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

typedef struct {
    float cx, cz;          // центр
    float hw, hd;          // полуширина/полувысота
    float rot;             // поворот вокруг Y
    float offset;          // смещение от рельефа (м)
    GLuint tex;
    float r, g, b, a;
} Decal;

typedef struct {
    const WorldState *world;      // карта высот (посадка на рельеф)
    const RendererContext *rc;
    GLuint prog, vao, vbo, ibo;
    GLint u_viewproj, u_tex, u_color, u_alpha;
    Decal d[DECALS_MAX];
    int n;
    int head;        // следующий слот кольца (при n == DECALS_MAX)
} DecalsParams;

static DecalsParams *s_d = NULL;

static void decals_node_render(SceneNode *n, float dt);

SceneNode *decals_make_node(const WorldState *w, const RendererContext *rc)
{
    DecalsParams *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->world = w;
    p->rc = rc;

    p->prog = make_program("shaders/decal.vs", "shaders/decal.fs");
    if (!p->prog) { free(p); return NULL; }
    p->u_viewproj = glGetUniformLocation(p->prog, "uViewProj");
    p->u_tex      = glGetUniformLocation(p->prog, "uTex");
    p->u_color    = glGetUniformLocation(p->prog, "uColor");
    p->u_alpha    = glGetUniformLocation(p->prog, "uAlpha");
    glUseProgram(p->prog);
    glUniform1i(p->u_tex, 0);
    glUseProgram(0);

    // Плоский квад XZ: -0.5..0.5, UV 0..1
    glGenVertexArrays(1, &p->vao);
    glBindVertexArray(p->vao);
    glGenBuffers(1, &p->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, p->vbo);
    const float quad[4][5] = {
        { -0.5f, 0.0f, -0.5f, 0.0f, 0.0f },
        {  0.5f, 0.0f, -0.5f, 1.0f, 0.0f },
        {  0.5f, 0.0f,  0.5f, 1.0f, 1.0f },
        { -0.5f, 0.0f,  0.5f, 0.0f, 1.0f },
    };
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), NULL);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float),
                          (const void *)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glGenBuffers(1, &p->ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, p->ibo);
    const unsigned int idx[6] = { 0, 1, 2, 0, 2, 3 };
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof idx, idx, GL_STATIC_DRAW);
    glBindVertexArray(0);

    s_d = p;
    const float size[3] = { 100.0f, 10.0f, 100.0f };
    return node_new("Decals", "MeshInstance3D", NULL, size, p, NULL,
                    decals_node_render);
}

// Колбэк узла: рендер всех декалей (из обхода сцены)
static void decals_node_render(SceneNode *n, float dt)
{
    (void)dt;
    DecalsParams *p = n->params;
    if (!p) return;
    decals_draw(p->rc);
}

// --- Внутренний хелпер: сэмпл 3x3 для плавного склона -----------------------
static float decal_sample_terrain(const WorldState *w, float x, float z,
                                  float hw, float hd, float u, float v)
{
    // 3x3 сетка сэмплов (u,v в 0..1): центр + 4 угла + 4 середины сторон
    const float du[3] = { -1.0f, 0.0f, 1.0f };
    const float dv[3] = { -1.0f, 0.0f, 1.0f };
    float sum = 0.0f;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            float sx = x + (du[i] * hw);
            float sz = z + (dv[j] * hd);
            sum += sample_height(w, sx, sz);
        }
    }
    return sum / 9.0f;
}

// --- Публичный API -----------------------------------------------------------
void decal_place(float x, float z, float half_w, float half_d, float rot,
                 const char *tex_path, float r, float g, float b, float alpha)
{
    decal_place_ex(x, z, half_w, half_d, rot, tex_path, r, g, b, alpha, 0.03f);
}

void decal_place_ex(float x, float z, float half_w, float half_d, float rot,
                    const char *tex_path, float r, float g, float b,
                    float alpha, float offset)
{
    if (!s_d) return;
    DecalsParams *p = s_d;
    int slot;
    if (p->n < DECALS_MAX) {
        slot = p->n++;
        p->d[slot].tex = 0;
    } else {
        // Настоящее кольцо: раньше всегда переписывался slot 0 — самая
        // старая декаль вытеснялась каждым новым штампением, остальные жили
        // вечно (кольцо не работало).
        slot = p->head;
        p->head = (p->head + 1) % DECALS_MAX;
        if (p->d[slot].tex) { resources_texture_release(p->d[slot].tex); p->d[slot].tex = 0; }
    }
    Decal *d = &p->d[slot];
    d->cx = x; d->cz = z;
    d->hw = half_w; d->hd = half_d;
    d->rot = rot;
    d->offset = offset;
    d->r = r; d->g = g; d->b = b; d->a = alpha;
    d->tex = resources_texture(tex_path, 4, GL_CLAMP_TO_EDGE, 1);
}

void decals_clear(void)
{
    if (!s_d) return;
    for (int i = 0; i < s_d->n; i++)
        if (s_d->d[i].tex) resources_texture_release(s_d->d[i].tex);
    s_d->n = 0;
}

// --- Рендер: один draw per decal, polygon offset вместо смещения -----------
void decals_draw(const RendererContext *rc)
{
    if (!s_d || s_d->n == 0) return;
    if (rc->pass == RP_SHADOW) return;   // декали не пишут в карты теней
    DecalsParams *p = s_d;

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(-1.0f, -1.0f);   // притягиваем к поверхности
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);           // декали друг на друге без глубины
    glDisable(GL_CULL_FACE);
    glUseProgram(p->prog);
    glUniformMatrix4fv(p->u_viewproj, 1, GL_FALSE, rc->viewproj);
    glBindVertexArray(p->vao);
    glBindBuffer(GL_ARRAY_BUFFER, p->vbo);

    for (int i = 0; i < p->n; i++) {
        const Decal *d = &p->d[i];
        glUniform3f(p->u_color, d->r, d->g, d->b);
        glUniform1f(p->u_alpha, d->a);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, d->tex);

        // Квад с поворотом и высотой по рельефу (3x3 сэмплирование)
        float ct = cosf(d->rot), st = sinf(d->rot);
        float off[4][2] = {
            { -d->hw, -d->hd }, {  d->hw, -d->hd },
            {  d->hw,  d->hd }, { -d->hw,  d->hd },
        };
        float tmp[4][5];   // x, y, z, u, v
        for (int k = 0; k < 4; k++) {
            tmp[k][0] = d->cx + off[k][0] * ct - off[k][1] * st;
            tmp[k][2] = d->cz + off[k][0] * st + off[k][1] * ct;
            // 3x3 сэмплирование для плавного склона
            tmp[k][1] = decal_sample_terrain(p->world,
                                             tmp[k][0], tmp[k][2],
                                             d->hw, d->hd,
                                             (k == 0 || k == 3) ? 0.0f : 1.0f,
                                             (k < 2) ? 0.0f : 1.0f)
                        + d->offset;
            tmp[k][3] = (k == 0 || k == 3) ? 0.0f : 1.0f;   // u
            tmp[k][4] = (k < 2) ? 0.0f : 1.0f;              // v
        }
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof tmp, tmp);
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, NULL);
    }
    glBindVertexArray(0);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glUseProgram(0);
}

void decals_cleanup(SceneNode *n)
{
    if (!n) return;
    DecalsParams *p = n->params;
    if (!p) return;
    if (s_d == p) s_d = NULL;
    for (int i = 0; i < p->n; i++)
        if (p->d[i].tex) resources_texture_release(p->d[i].tex);
    resources_program_release(p->prog);
    glDeleteBuffers(1, &p->vbo);
    glDeleteBuffers(1, &p->ibo);
    glDeleteVertexArrays(1, &p->vao);
    // params (p) освобождает node_free_tree(g->root)
}