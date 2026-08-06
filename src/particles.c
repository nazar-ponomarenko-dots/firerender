// ============================================================================
//  particles.c — система частиц: instanced-спрайты с CPU-обновлением
// ----------------------------------------------------------------------------
//  Один пул частиц (максимум PARTICLES_MAX) с текстурой-спрайтом
//  (img/particle.png или процедурное пятно, если файла нет). Рендер —
//  instanced draw: per-instance атрибуты (позиция, размер, цвет, альфа,
//  режим) лежат в отдельном VBO и обновляются целиком glBufferData каждый
//  кадр (512 инстансов — ~20 КБ, дёшево). Один draw call на весь пул.
//
//  Порядок работы:
//    particles_make_node  — GL-ресурсы + шейдер, регистрирует статик-узел;
//    particles_emit       — наполнение пула (burst);
//    particles_update(dt) — физика: скорость/гравитация/жизнь;
//    particles_draw(rc)   — instanced draw всех живых частиц одним вызовом.
// ============================================================================
#include "particles.h"

#include "gl_loader.h"
#include "resources.h"
#include "shader.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

// Инстанс-атрибуты: pos(xyz) + size(sxy) + color(rgb) + alpha + mode
typedef struct {
    float pos[3];
    float size[2];
    float col[3];
    float alpha;
    float mode;        // 0 = альфа-бленд, 1 = аддитив
} ParticleInst;

typedef struct {
    float x, y, z;        // позиция
    float vx, vy, vz;     // скорость
    float life, max_life; // остаток и полная жизнь
    float size, grow;     // текущий размер и прирост размера (для дыма)
    float r, g, b;
    float gravity;
    float alpha0;         // стартовая альфа
    int mode;             // 0 альфа / 1 аддитив
    int active;
} Particle;

typedef struct {
    const RendererContext *rc;
    GLuint prog, vao, vbo, inst_vbo;
    GLint u_viewproj, u_tex, u_camright, u_camup;
    GLuint tex;
    Particle p[PARTICLES_MAX];
    ParticleInst inst[PARTICLES_MAX];
} ParticlesParams;

static ParticlesParams *s_p = NULL;   // текущий узел (один на мир)

static void particles_node_update(SceneNode *n, float dt);
static void particles_node_render(SceneNode *n, float dt);

static float frand(void)
{
    return (float)rand() / (float)RAND_MAX;
}

SceneNode *particles_make_node(const RendererContext *rc)
{
    ParticlesParams *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->rc = rc;

    // Спрайт частиц. Если img/particle.png нет — resources_texture
    // подставит img/notfound.png (Source Engine style, для дебага).
    p->tex = resources_texture("img/particle.png", 4, GL_CLAMP_TO_EDGE, 1);

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
    glGenBuffers(1, &p->inst_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, p->inst_vbo);
    // Две зоны по PARTICLES_MAX: альфа-частицы и аддитивные (два прохода)
    glBufferData(GL_ARRAY_BUFFER, sizeof(ParticleInst) * PARTICLES_MAX * 2,
                 NULL, GL_DYNAMIC_DRAW);
    // instanced: pos(1), size(2), col(3), alpha(4), mode(5)
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
    glVertexAttribPointer(5, 1, GL_FLOAT, GL_FALSE, sizeof(ParticleInst),
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

void particles_emit(float x, float y, float z,
                    const float *dir, float spread, float speed, float life,
                    float size, float r, float g, float b, float gravity)
{
    if (!s_p) return;
    int slot = particles_alloc(s_p);
    Particle *q = &s_p->p[slot];
    q->x = x; q->y = y; q->z = z;
    float ang1 = frand() * 6.2832f;
    float ang2 = (frand() - 0.5f) * spread;
    float vx = sinf(ang1) * cosf(ang2);
    float vy = fabsf(sinf(ang2));
    float vz = cosf(ang1) * cosf(ang2);
    if (dir && (dir[0] || dir[1] || dir[2])) {   // смешиваем с направлением
        vx += dir[0]; vy += dir[1]; vz += dir[2];
        float l = sqrtf(vx * vx + vy * vy + vz * vz);
        if (l > 0.001f) { vx /= l; vy /= l; vz /= l; }
    }
    float sp = speed * (0.4f + frand() * 0.8f);
    q->vx = vx * sp; q->vy = vy * sp; q->vz = vz * sp;
    q->life = q->max_life = life * (0.7f + frand() * 0.6f);
    q->size = size * (0.7f + frand() * 0.6f);
    q->grow = size * 0.4f;
    q->r = r; q->g = g; q->b = b;
    q->gravity = gravity;
    q->alpha0 = 1.0f;
    q->mode = (gravity <= 0.0f) ? 1 : 0;   // без гравитации — аддитив (искры)
    q->active = 1;
}

// Burst: count частиц из одной точки с общим направлением
void particles_burst(float x, float y, float z,
                     const float *dir, float spread, float speed, float life,
                     float size, float r, float g, float b, float gravity,
                     int count)
{
    for (int i = 0; i < count; i++)
        particles_emit(x, y, z, dir, spread, speed, life, size, r, g, b, gravity);
}

void particles_update(float dt)
{
    if (!s_p) return;
    for (int i = 0; i < PARTICLES_MAX; i++) {
        Particle *q = &s_p->p[i];
        if (!q->active) continue;
        q->life -= dt;
        if (q->life <= 0.0f) { q->active = 0; continue; }
        q->vy -= q->gravity * dt;
        q->x += q->vx * dt;
        q->y += q->vy * dt;
        q->z += q->vz * dt;
        q->size += q->grow * dt;
    }
}

// --- Рендер: instanced draw всех живых частиц ----------------------------------
// Два draw call: сначала альфа-бленд (нормальные), затем аддитивные (искры,
// без гравитации). Аддитиву нужен GL_ONE, поэтому режим делится по проходам.
void particles_draw(const RendererContext *rc)
{
    if (!s_p) return;
    if (rc->pass == RP_SHADOW) return;   // в карты теней частицы не пишут
    ParticlesParams *p = s_p;

    int n_alpha = 0, n_add = 0;
    for (int i = 0; i < PARTICLES_MAX; i++) {
        const Particle *q = &p->p[i];
        if (!q->active) continue;
        ParticleInst *it = &p->inst[q->mode ? PARTICLES_MAX + n_add++ : n_alpha++];
        it->pos[0] = q->x; it->pos[1] = q->y; it->pos[2] = q->z;
        it->size[0] = q->size; it->size[1] = q->size;
        it->col[0] = q->r; it->col[1] = q->g; it->col[2] = q->b;
        it->alpha = q->alpha0 * (q->life / q->max_life);
        it->mode = (float)q->mode;
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
    ParticlesParams *p = n->params;
    if (!p) return;
    if (s_p == p) s_p = NULL;
    resources_texture_release(p->tex);
    resources_program_release(p->prog);
    glDeleteBuffers(1, &p->vbo);
    glDeleteBuffers(1, &p->inst_vbo);
    glDeleteVertexArrays(1, &p->vao);
    free(p);
}
