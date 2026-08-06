// ============================================================================
//  fauna.c — живая карта: светлячки, бабочки, птицы (fauna[] из map.json)
// ----------------------------------------------------------------------------
//  Каждая группа — квады-биллборды с текстурами:
//    fireflies   — светящиеся точки (additive), мерцают, медленно дрейфуют;
//    butterflies — бабочки, машут крыльями (сжатие по X), зигзаг полёта;
//    birds       — силуэты, кружат в небе над областью.
//  Обновление частиц — в update-проходе сцены (fauna_node_update),
//  рендер — по одному draw на особь (их мало, до ~60 на карту).
// ============================================================================
#include "fauna.h"

#include "assets.h"
#include "gl_loader.h"
#include "mapdata.h"
#include "resources.h"
#include "shader.h"
#include "world.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#define FAUNA_MAX_PARTS 256   // потолок особей на карту
#define FAUNA_GROUPS_MAX 8

typedef struct {
    float x, y, z;       // позиция
    float ph;            // фаза (случайная)
    float sp;            // скорость
    float sx, sy, sz;    // размер квада (м)
    float hue;           // тон (светлячки)
} FaunaP;

typedef struct {
    int type;            // MapFaunaType
    int count;
    FaunaP p[FAUNA_MAX_PARTS];
    float cx, cz, hw, hd, h;   // область
    GLuint tex;
    const char *tex_path;
} FaunaGroup;

typedef struct {
    GLuint prog, vao, vbo, ibo;
    GLint u_viewproj, u_objpos, u_objsize, u_objright, u_tex;
    GLint u_tint, u_mode, u_time;
    FaunaGroup g[FAUNA_GROUPS_MAX];
    int n;
    const RendererContext *rc;   // позиция камеры (биллборды)
} FaunaParams;

static float s_time = 0.0f;

// Текстура группы
static const char *fauna_tex_path(int type)
{
    switch (type) {
    case FAUNA_FIREFLIES:   return "img/fauna_glow.png";
    case FAUNA_BUTTERFLIES: return "img/fauna_butterfly.png";
    default:                return "img/fauna_bird.png";
    }
}

static float frand(void)
{
    return (float)rand() / (float)RAND_MAX;
}

// Случайная высота: по типу
static float group_height(int type, float base)
{
    if (type == FAUNA_BIRDS) return base + frand() * 25.0f;
    if (type == FAUNA_FIREFLIES) return base * (0.6f + 0.8f * frand());
    return base + frand() * 2.5f;
}

SceneNode *fauna_make_node(const RendererContext *rc)
{
    int gn = mapdata_fauna_count();
    if (gn <= 0) return NULL;
    FaunaParams *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->rc = rc;
    if (gn > FAUNA_GROUPS_MAX) gn = FAUNA_GROUPS_MAX;

    for (int i = 0; i < gn; i++) {
        const MapFauna *f = mapdata_fauna(i);
        if (!f || f->count <= 0) continue;
        FaunaGroup *g = &p->g[p->n];
        g->type = f->type;
        g->count = f->count > FAUNA_MAX_PARTS ? FAUNA_MAX_PARTS : f->count;
        g->cx = f->area[0];
        g->cz = f->area[1];
        g->hw = f->size[0] * 0.5f;
        g->hd = f->size[1] * 0.5f;
        g->h = f->height;
        g->tex_path = fauna_tex_path(f->type);
        g->tex = resources_texture(g->tex_path, 4, GL_CLAMP_TO_EDGE, 0);
        for (int k = 0; k < g->count; k++) {
            FaunaP *q = &g->p[k];
            q->x = g->cx + (frand() * 2.0f - 1.0f) * g->hw;
            q->z = g->cz + (frand() * 2.0f - 1.0f) * g->hd;
            q->y = group_height(f->type, g->h);
            q->ph = frand() * 6.2832f;
            q->sp = 0.5f + frand() * 1.5f;
            if (f->type == FAUNA_BUTTERFLIES) {
                q->sx = 0.9f + frand() * 0.6f;
                q->sy = q->sx * 0.8f;
            } else if (f->type == FAUNA_BIRDS) {
                q->sx = 2.5f + frand() * 2.0f;
                q->sy = q->sx * 0.5f;
            } else {
                q->sx = 0.25f + frand() * 0.2f;
                q->sy = q->sx;
                q->hue = frand();
            }
        }
        p->n++;
        SDL_Log("Fauna: %s x%d at %.0f,%.0f (%.0fx%.0f m)",
                g->tex_path, g->count, g->cx, g->cz, g->hw * 2, g->hd * 2);
    }
    if (p->n == 0) { free(p); return NULL; }

    p->prog = make_program("shaders/fauna.vs", "shaders/fauna.fs");
    if (!p->prog) { free(p); return NULL; }
    p->u_viewproj = glGetUniformLocation(p->prog, "uViewProj");
    p->u_objpos   = glGetUniformLocation(p->prog, "uObjPos");
    p->u_objsize  = glGetUniformLocation(p->prog, "uObjSize");
    p->u_objright = glGetUniformLocation(p->prog, "uObjRight");
    p->u_tex      = glGetUniformLocation(p->prog, "uTex");
    p->u_tint     = glGetUniformLocation(p->prog, "uTint");
    p->u_mode     = glGetUniformLocation(p->prog, "uMode");
    p->u_time     = glGetUniformLocation(p->prog, "uTime");
    glUseProgram(p->prog);
    glUniform1i(p->u_tex, 0);
    glUseProgram(0);

    glGenVertexArrays(1, &p->vao);
    glBindVertexArray(p->vao);
    glGenBuffers(1, &p->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, p->vbo);
    const float quad[4][4] = {
        { -0.5f, -0.5f, 0.0f, 0.0f },
        {  0.5f, -0.5f, 1.0f, 0.0f },
        {  0.5f,  0.5f, 1.0f, 1.0f },
        { -0.5f,  0.5f, 0.0f, 1.0f },
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

    const float size[3] = { 1000.0f, 300.0f, 1000.0f };
    return node_new("Fauna", "MeshInstance3D", NULL, size, p,
                    fauna_node_update, fauna_node_render);
}

void fauna_node_update(SceneNode *n, float dt)
{
    FaunaParams *p = n->params;
    s_time += dt;
    for (int gi = 0; gi < p->n; gi++) {
        FaunaGroup *g = &p->g[gi];
        for (int k = 0; k < g->count; k++) {
            FaunaP *q = &g->p[k];
            switch (g->type) {
            case FAUNA_FIREFLIES:
                // медленный дрейф в области + мерцание (в шейдере)
                q->x += sinf(q->ph + s_time * q->sp * 0.4f) * dt * 0.8f;
                q->y += cosf(q->ph * 2.0f + s_time * q->sp * 0.5f) * dt * 0.5f;
                break;
            case FAUNA_BUTTERFLIES:
                // зигзаг полёта
                q->x += cosf(q->ph + s_time * q->sp) * dt * 2.5f;
                q->z += sinf(q->ph * 1.3f + s_time * q->sp * 0.8f) * dt * 2.5f;
                q->y = g->h + sinf(q->ph * 3.0f + s_time * q->sp * 1.4f) * 0.8f;
                break;
            case FAUNA_BIRDS:
                // круги в небе
                {
                    float a = q->ph + s_time * q->sp * 0.35f;
                    float r = 12.0f + q->sx * 8.0f;
                    q->x = g->cx + cosf(a) * r;
                    q->z = g->cz + sinf(a) * r * 1.4f;
                }
                break;
            }
            // не вылетать за область (кроме птиц — они над ней)
            if (g->type != FAUNA_BIRDS) {
                if (q->x < g->cx - g->hw) q->x = g->cx + g->hw;
                if (q->x > g->cx + g->hw) q->x = g->cx - g->hw;
                if (q->z < g->cz - g->hd) q->z = g->cz + g->hd;
                if (q->z > g->cz + g->hd) q->z = g->cz - g->hd;
            }
        }
    }
}

void fauna_node_render(SceneNode *n, float dt)
{
    (void)dt;
    FaunaParams *p = n->params;
    if (!p->n) return;
    if (p->rc->pass == RP_SHADOW) return;   // в карты теней фауна не пишет
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glUseProgram(p->prog);
    glUniformMatrix4fv(p->u_viewproj, 1, GL_FALSE, p->rc->viewproj);
    glUniform1f(p->u_time, s_time);
    glBindVertexArray(p->vao);

    for (int gi = 0; gi < p->n; gi++) {
        FaunaGroup *g = &p->g[gi];
        // Отсечение фрустумом на уровне группы: сфера (центр + диагональ)
        float gd = g->hw > g->hd ? g->hw : g->hd;
        if (!frustum_test_sphere(&p->rc->frustum, g->cx, g->h, g->cz,
                                 gd + 20.0f))
            continue;
        int mode = (g->type == FAUNA_FIREFLIES) ? 0 : 1;
        // светлячки — аддитивно; бабочки/птицы — альфа-тест
        if (mode == 0) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        } else {
            glDisable(GL_BLEND);
        }
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, g->tex);
        glUniform1i(p->u_mode, mode);
        for (int k = 0; k < g->count; k++) {
            FaunaP *q = &g->p[k];
            // биллборд: ось ширины перпендикулярна камере
            float dx = p->rc->cam_pos[0] - q->x;
            float dz = p->rc->cam_pos[2] - q->z;
            float d = sqrtf(dx * dx + dz * dz);
            if (d < 0.001f) d = 1.0f;
            glUniform3f(p->u_objpos, q->x, q->y, q->z);
            glUniform2f(p->u_objsize, q->sx, q->sy);
            glUniform3f(p->u_objright, -dz / d, 0.0f, dx / d);
            glUniform3f(p->u_tint, 1.0f, 1.0f, 1.0f);
            if (mode == 0)   // тёплый тон светлячка
                glUniform3f(p->u_tint, 1.0f, 0.85f + 0.1f * q->hue,
                            0.55f + 0.2f * q->hue);
            glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, NULL);
        }
    }
    glDisable(GL_BLEND);
    glUseProgram(0);
}

void fauna_cleanup(SceneNode *n)
{
    FaunaParams *p = n->params;
    if (!p) return;
    for (int i = 0; i < p->n; i++)
        resources_texture_release(p->g[i].tex);
    resources_program_release(p->prog);
    glDeleteBuffers(1, &p->vbo);
    glDeleteBuffers(1, &p->ibo);
    glDeleteVertexArrays(1, &p->vao);
}
