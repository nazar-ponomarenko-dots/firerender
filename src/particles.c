// ============================================================================
//  particles.c — система частиц: instanced-спрайты с CPU-обновлением
// ----------------------------------------------------------------------------
//  Один пул частиц (максимум PARTICLES_MAX) с текстурой-спрайтом
//  (img/particle.png, при отсутствии — notfound.png). Рендер — два
//  instanced draw: альфа-бленд и аддитив, per-instance атрибуты (позиция,
//  размер, цвет, альфа, скорость) в отдельном VBO. Физика частицы:
//  скорость/гравитация/сопротивление воздуха/ветер/покачивание, опции
//  streak (вытянутая капля) и bounce (отскок от земли через sample_height).
//
//  Параметры задаются структурой ParticleParams -> particles_emit/burst.
//  Именованные эффекты и эмиттеры — в fx.c поверх этого модуля.
// ============================================================================
#include "particles.h"

#include "gl_loader.h"
#include "resources.h"
#include "shader.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

// Инстанс-атрибуты: pos(xyz) + size(sxy) + color(rgb) + alpha + vel(xyz)
typedef struct {
    float pos[3];
    float size[2];   // x = ширина, y = длина стрика (для streak)
    float col[3];
    float alpha;
    float vel[3];    // скорость (для стрика: направление растяжения)
} ParticleInst;

typedef struct {
    float x, y, z;
    float vx, vy, vz;
    float life, max_life;   // остаток и полная жизнь
    float size;             // текущий размер (ширина)
    float r, g, b;
    float gravity;
    float drag;
    float sway;
    float wind[3];
    float alpha0, alpha1;   // альфа в начале и конце жизни
    float sizek;            // прирост размера в секунду (из size0/size1)
    int blend;              // 0 альфа / 1 аддитив
    int streak;             // вытянуть вдоль скорости
    int bounce;             // отскок от земли
    int active;
} Particle;

typedef struct {
    const WorldState *world;   // карта высот (отскок bounce)
    const RendererContext *rc;
    GLuint prog, vao, vbo, inst_vbo;
    GLint u_viewproj, u_tex, u_camright, u_camup;
    GLuint tex;
    Particle p[PARTICLES_MAX];
    ParticleInst inst[PARTICLES_MAX * 2];   // две зоны: альфа/аддитив
} ParticlesParams;

static ParticlesParams *s_p = NULL;   // текущий узел (один на мир)

static void particles_node_update(SceneNode *n, float dt);
static void particles_node_render(SceneNode *n, float dt);

static float frand(void)
{
    return (float)rand() / (float)RAND_MAX;
}

SceneNode *particles_make_node(const WorldState *w, const RendererContext *rc)
{
    ParticlesParams *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->world = w;
    p->rc = rc;
    // REPEAT (не CLAMP): стрик-шейдер тайлит текстуру вдоль капли (t*4),
    // с CLAMP тот "тайлинг" показывал залитую строку вместо капель.
    p->tex = resources_texture("img/particle.png", 4, GL_REPEAT, 1);

    p->prog = make_program("shaders/particle.vs", "shaders/particle.fs");
    if (!p->prog) {
        resources_texture_release(p->tex);
        free(p);
        return NULL;
    }
    p->u_viewproj = glGetUniformLocation(p->prog, "uViewProj");
    p->u_tex      = glGetUniformLocation(p->prog, "uTex");
    p->u_camright = glGetUniformLocation(p->prog, "uCamRight");
    p->u_camup    = glGetUniformLocation(p->prog, "uCamUp");
    glUseProgram(p->prog);
    glUniform1i(p->u_tex, 0);
    glUseProgram(0);

    // Квад с UV; инстанс-атрибуты в отдельном VBO (glVertexAttribDivisor)
    glGenVertexArrays(1, &p->vao);
    glBindVertexArray(p->vao);
    glGenBuffers(1, &p->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, p->vbo);
    const float quad[6][2] = {
        { -0.5f, -0.5f }, { 0.5f, -0.5f }, { 0.5f,  0.5f },
        { -0.5f, -0.5f }, { 0.5f,  0.5f }, { -0.5f,  0.5f },
    };
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), NULL);
    glEnableVertexAttribArray(0);
    // instanced: 1=pos(3) 2=size(2) 3=col(3) 4=alpha(1) 5=vel(3)
    glGenBuffers(1, &p->inst_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, p->inst_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(ParticleInst) * PARTICLES_MAX * 2,
                 NULL, GL_DYNAMIC_DRAW);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(ParticleInst), NULL);
    glEnableVertexAttribArray(1);
    glVertexAttribDivisor(1, 1);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(ParticleInst),
                          (const void *)(3 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribDivisor(2, 1);
    glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, sizeof(ParticleInst),
                          (const void *)(5 * sizeof(float)));
    glEnableVertexAttribArray(3);
    glVertexAttribDivisor(3, 1);
    glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, sizeof(ParticleInst),
                          (const void *)(8 * sizeof(float)));
    glEnableVertexAttribArray(4);
    glVertexAttribDivisor(4, 1);
    glVertexAttribPointer(5, 3, GL_FLOAT, GL_FALSE, sizeof(ParticleInst),
                          (const void *)(9 * sizeof(float)));
    glEnableVertexAttribArray(5);
    glVertexAttribDivisor(5, 1);
    glBindVertexArray(0);

    s_p = p;
    const float size[3] = { 100.0f, 100.0f, 100.0f };
    return node_new("Particles", "MeshInstance3D", NULL, size, p,
                    particles_node_update, particles_node_render);
}

// Колбэки узла: update — из фикс-тика (100 Гц), render — из обхода сцены
static void particles_node_update(SceneNode *n, float dt)
{
    (void)n;
    particles_update(dt);
}

static void particles_node_render(SceneNode *n, float dt)
{
    (void)dt;
    ParticlesParams *p = n->params;
    if (!p) return;
    particles_draw(p->rc);
}

// Находит слот: мёртвая частица или самая старая из живых (пул полон)
static int particles_alloc(ParticlesParams *p)
{
    for (int i = 0; i < PARTICLES_MAX; i++)
        if (!p->p[i].active) return i;
    int oldest = 0;
    for (int i = 1; i < PARTICLES_MAX; i++)
        if (p->p[i].life > p->p[oldest].life) oldest = i;
    return oldest;
}

// --- Эмиссия ---------------------------------------------------------------
void particles_emit(const ParticleParams *par)
{
    if (!s_p || !par) return;
    int slot = particles_alloc(s_p);
    Particle *q = &s_p->p[slot];

    float ang1 = frand() * 6.2832f;
    float ang2 = (frand() - 0.5f) * par->spread;
    float vx = sinf(ang1) * cosf(ang2);
    float vy = sinf(ang2);                       // знак разрешён: разброс и навес
    float vz = cosf(ang1) * cosf(ang2);
    // dir — массив внутри struct, NULL быть не может: проверяем только нули
    if (par->dir[0] || par->dir[1] || par->dir[2]) {
        vx += par->dir[0]; vy += par->dir[1]; vz += par->dir[2];
        float l = sqrtf(vx * vx + vy * vy + vz * vz);
        if (l > 0.001f) { vx /= l; vy /= l; vz /= l; }
    }
    float sp = par->speed0 + frand() * (par->speed1 - par->speed0);
    q->x = par->pos[0]; q->y = par->pos[1]; q->z = par->pos[2];
    q->vx = vx * sp + par->wind[0];
    q->vy = vy * sp + par->wind[1];
    q->vz = vz * sp + par->wind[2];
    q->life = q->max_life = par->life0 + frand() * (par->life1 - par->life0);
    if (q->max_life <= 0.01f) q->max_life = q->life = 0.5f;
    q->r = par->r; q->g = par->g; q->b = par->b;
    q->gravity = par->gravity;
    q->drag = par->drag;
    q->sway = par->sway;
    q->wind[0] = par->wind[0];
    q->wind[1] = par->wind[1];
    q->wind[2] = par->wind[2];
    q->alpha0 = par->alpha0;
    q->alpha1 = par->alpha1;
    q->size = par->size0;
    q->sizek = (par->size1 - par->size0) / q->max_life;
    q->blend = par->blend;
    q->streak = par->streak;
    q->bounce = par->bounce;
    q->active = 1;
}

void particles_burst(const ParticleParams *par, int count)
{
    for (int i = 0; i < count; i++)
        particles_emit(par);
}

// --- Обновление физики ------------------------------------------------------
void particles_update(float dt)
{
    if (!s_p) return;
    ParticlesParams *p = s_p;
    for (int i = 0; i < PARTICLES_MAX; i++) {
        Particle *q = &p->p[i];
        if (!q->active) continue;
        q->life -= dt;
        if (q->life <= 0.0f) { q->active = 0; continue; }

        // Гравитация и сопротивление воздуха
        q->vy -= q->gravity * dt;
        float dr = 1.0f - q->drag * dt;
        if (dr < 0.0f) dr = 0.0f;
        q->vx *= dr; q->vy *= dr; q->vz *= dr;
        // Ветер и покачивание (снег/пыль)
        float t = q->max_life - q->life;
        q->vx += (q->wind[0] + q->sway * sinf(t * 2.5f)) * dt;
        q->vy += q->wind[1] * dt;
        q->vz += (q->wind[2] + q->sway * cosf(t * 2.1f)) * dt;

        q->x += q->vx * dt;
        q->y += q->vy * dt;
        q->z += q->vz * dt;
        q->size += q->sizek * dt;

        // Отскок от земли (кровь/искры)
        if (q->bounce && p->world && q->vy < 0.0f) {
            float h = sample_height(p->world, q->x, q->z);
            if (q->y < h + 0.05f) {
                q->y = h + 0.05f;
                q->vy = -q->vy * 0.35f;
                q->vx *= 0.5f; q->vz *= 0.5f;
                if (q->vy < 0.8f) q->bounce = 0;   // затухло — лежит
            }
        }
    }
}

// --- Рендер: instanced draw всех живых частиц ----------------------------------
// Два draw call: сначала альфа-бленд (нормальные), затем аддитивные (искры,
// свет). Аддитиву нужен GL_ONE, поэтому режим делится по проходам.
void particles_draw(const RendererContext *rc)
{
    if (!s_p) return;
    if (rc->pass == RP_SHADOW) return;   // в карты теней частицы не пишут
    ParticlesParams *p = s_p;

    int n_alpha = 0, n_add = 0;
    for (int i = 0; i < PARTICLES_MAX; i++) {
        const Particle *q = &p->p[i];
        if (!q->active) continue;
        int dst = q->blend ? PARTICLES_MAX + n_add++ : n_alpha++;
        ParticleInst *it = &p->inst[dst];
        it->pos[0] = q->x; it->pos[1] = q->y; it->pos[2] = q->z;
        float k = q->max_life > 0.001f ? (q->life / q->max_life) : 0.0f;
        it->size[0] = q->size;
        it->size[1] = q->streak ? q->size * 8.0f : q->size;   // длина стрика
        it->col[0] = q->r; it->col[1] = q->g; it->col[2] = q->b;
        it->alpha = q->alpha1 + (q->alpha0 - q->alpha1) * k;   // fade по жизни
        it->vel[0] = q->vx; it->vel[1] = q->vy; it->vel[2] = q->vz;
    }
    if (n_alpha + n_add == 0) return;

    glEnable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glUseProgram(p->prog);
    glUniformMatrix4fv(p->u_viewproj, 1, GL_FALSE, rc->viewproj);
    glUniform3f(p->u_camright, rc->cam_right[0], rc->cam_right[1], rc->cam_right[2]);
    glUniform3f(p->u_camup, rc->cam_up[0], rc->cam_up[1], rc->cam_up[2]);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, p->tex);
    glBindVertexArray(p->vao);
    glBindBuffer(GL_ARRAY_BUFFER, p->inst_vbo);

    if (n_alpha > 0) {
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glBufferData(GL_ARRAY_BUFFER, sizeof(ParticleInst) * PARTICLES_MAX,
                     p->inst, GL_DYNAMIC_DRAW);
        glDrawArraysInstanced(GL_TRIANGLES, 0, 6, n_alpha);
    }
    if (n_add > 0) {
        glBlendFunc(GL_ONE, GL_ONE);
        glBufferData(GL_ARRAY_BUFFER, sizeof(ParticleInst) * PARTICLES_MAX,
                     p->inst + PARTICLES_MAX, GL_DYNAMIC_DRAW);
        glDrawArraysInstanced(GL_TRIANGLES, 0, 6, n_add);
    }
    glBindVertexArray(0);

    glDisable(GL_BLEND);
    glUseProgram(0);
}

void particles_clear(void)
{
    if (!s_p) return;
    for (int i = 0; i < PARTICLES_MAX; i++) s_p->p[i].active = 0;
}

void particles_cleanup(SceneNode *n)
{
    if (!n) return;
    ParticlesParams *p = n->params;
    if (!p) return;
    if (s_p == p) s_p = NULL;
    resources_texture_release(p->tex);
    resources_program_release(p->prog);
    glDeleteBuffers(1, &p->vbo);
    glDeleteBuffers(1, &p->inst_vbo);
    glDeleteVertexArrays(1, &p->vao);
    // params (p) освобождает node_free_tree(g->root)
}
