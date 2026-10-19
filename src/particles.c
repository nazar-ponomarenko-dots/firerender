// ============================================================================
//  particles.c — particle system: instanced sprites with CPU update
// ----------------------------------------------------------------------------
//  A single particle pool (PARTICLES_MAX at most) with a sprite texture
//  (img/particle.png, falls back to notfound.png if missing). Rendering is two
//  instanced draws: alpha-blended and additive; per-instance attributes
//  (position, size, color, alpha, velocity) live in a separate VBO. Particle
//  physics: velocity/gravity/air drag/wind/sway, with streak (elongated drop)
//  and bounce (off the ground via sample_height) options.
//
//  Parameters are given via ParticleParams -> particles_emit/burst.
//  Named effects and emitters live in fx.c on top of this module.
// ============================================================================
#include "particles.h"

#include "gl_loader.h"
#include "resources.h"
#include "shader.h"
#include "water.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

// Instance attributes: pos(xyz) + size(sxy) + color(rgb) + alpha + vel(xyz)
// + plain (1 = no texture, bright disc)
typedef struct {
    float pos[3];
    float size[2];   // x = width, y = streak length (for streak)
    float col[3];
    float alpha;
    float vel[3];    // velocity (for streak: stretch direction)
    float plain;     // 1 = clean disc, no texture
} ParticleInst;

typedef struct {
    float x, y, z;
    float vx, vy, vz;
    float life, max_life;   // remaining and total lifetime
    float size;             // current size (width)
    float r, g, b;
    float gravity;
    float drag;
    float sway;
    float turb;             // turbulence (swirling)
    float ph;               // random phase (each particle sways on its own)
    float wind[3];
    float alpha0, alpha1;   // alpha at the start and end of life
    float sizek;            // size growth per second (from size0/size1)
    int blend;              // 0 alpha / 1 additive
    int streak;             // stretch along the velocity
    int plain;              // no texture — bright disc (snow)
    int bounce;             // bounce off the ground
    int collide;            // 1 = dies on terrain/water, 2 = settles and fades
    int active;
} Particle;

typedef struct {
    const WorldState *world;   // heightmap (bounce)
    const RendererContext *rc;
    GLuint prog, vao, vbo, inst_vbo;
    GLint u_viewproj, u_tex, u_camright, u_camup;
    GLuint tex;
    Particle p[PARTICLES_MAX];
    ParticleInst inst[PARTICLES_MAX * 2];   // two zones: alpha/additive
} ParticlesParams;

static ParticlesParams *s_p = NULL;   // current node (one per world)

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
    // REPEAT (not CLAMP): the streak shader tiles the texture along the drop
    // (t*4); with CLAMP that "tiling" showed a filled row instead of drops.
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

    // Quad with UVs; instance attributes in a separate VBO (glVertexAttribDivisor)
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
    glVertexAttribPointer(6, 1, GL_FLOAT, GL_FALSE, sizeof(ParticleInst),
                          (const void *)(12 * sizeof(float)));
    glEnableVertexAttribArray(6);
    glVertexAttribDivisor(6, 1);
    glBindVertexArray(0);

    s_p = p;
    const float size[3] = { 100.0f, 100.0f, 100.0f };
    SceneNode *node = node_new("Particles", "MeshInstance3D", NULL, size, p,
                               particles_node_update, particles_node_render);
    if (!node) {
        s_p = NULL;
        if (p->inst_vbo) glDeleteBuffers(1, &p->inst_vbo);
        if (p->vbo) glDeleteBuffers(1, &p->vbo);
        if (p->vao) glDeleteVertexArrays(1, &p->vao);
        resources_program_release(p->prog);
        if (p->tex) resources_texture_release(p->tex);
        free(p);
    }
    return node;
}

// Node callbacks: update — from the fixed tick (100 Hz), render — from the scene traversal
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

// Finds a slot: a dead particle or the oldest of the live ones (pool full)
static int particles_alloc(ParticlesParams *p)
{
    for (int i = 0; i < PARTICLES_MAX; i++)
        if (!p->p[i].active) return i;
    // "life" is the REMAINING lifetime, so the oldest is the largest age
    // (max_life - life). Comparing "life >" evicted the FRESHEST particle, and a
    // burst into a full pool visibly dropped its own newest spawns.
    int oldest = 0;
    float oldest_age = p->p[0].max_life - p->p[0].life;
    for (int i = 1; i < PARTICLES_MAX; i++) {
        float age = p->p[i].max_life - p->p[i].life;
        if (age > oldest_age) { oldest_age = age; oldest = i; }
    }
    return oldest;
}

// --- Emission ---------------------------------------------------------------
void particles_emit(const ParticleParams *par)
{
    if (!s_p || !par) return;
    int slot = particles_alloc(s_p);
    Particle *q = &s_p->p[slot];

    float ang1 = frand() * 6.2832f;
    float ang2 = (frand() - 0.5f) * par->spread;
    float vx = sinf(ang1) * cosf(ang2);
    float vy = sinf(ang2);                       // sign allowed: spread and lob
    float vz = cosf(ang1) * cosf(ang2);
    // dir is an array inside the struct, it cannot be NULL: only check for zeros
    if (par->dir[0] || par->dir[1] || par->dir[2]) {
        vx += par->dir[0]; vy += par->dir[1]; vz += par->dir[2];
        float l = sqrtf(vx * vx + vy * vy + vz * vz);
        if (l > 0.001f) { vx /= l; vy /= l; vz /= l; }
    }
    // Speed spread (snow): multiplier weighted by bucket_w. Gravity is scaled
    // by the same multiplier — otherwise drag kills the initial speed toward
    // the terminal one (gravity/drag) within ~1 s and all particles fall alike.
    float mul = 1.0f;
    if (par->nbucket > 0) {
        float wsum = 0.0f;
        for (int i = 0; i < par->nbucket; i++) wsum += par->bucket_w[i];
        if (wsum > 0.0f) {
            float r = frand() * wsum;
            for (int i = 0; i < par->nbucket; i++) {
                r -= par->bucket_w[i];
                if (r <= 0.0f) { mul = par->bucket_mul[i]; break; }
            }
        }
    }
    float sp = (par->speed0 + frand() * (par->speed1 - par->speed0)) * mul;
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
    q->turb = par->turb;
    q->ph = frand() * 6.2832f;          // phase: own sway/swirl cycle
    if (mul != 1.0f) q->gravity *= mul; // speed spread — and gravity spread
    q->wind[0] = par->wind[0];
    q->wind[1] = par->wind[1];
    q->wind[2] = par->wind[2];
    q->alpha0 = par->alpha0;
    q->alpha1 = par->alpha1;
    q->size = par->size0;
    q->sizek = (par->size1 - par->size0) / q->max_life;
    q->blend = par->blend;
    q->streak = par->streak;
    q->plain = par->plain;
    q->bounce = par->bounce;
    q->collide = par->collide;
    q->active = 1;
}

void particles_burst(const ParticleParams *par, int count)
{
    for (int i = 0; i < count; i++)
        particles_emit(par);
}

// --- Physics update ---------------------------------------------------------
void particles_update(float dt)
{
    if (!s_p) return;
    ParticlesParams *p = s_p;
    for (int i = 0; i < PARTICLES_MAX; i++) {
        Particle *q = &p->p[i];
        if (!q->active) continue;
        q->life -= dt;
        if (q->life <= 0.0f) { q->active = 0; continue; }

        // Gravity and air drag
        q->vy -= q->gravity * dt;
        float dr = 1.0f - q->drag * dt;
        if (dr < 0.0f) dr = 0.0f;
        q->vx *= dr; q->vy *= dr; q->vz *= dr;
        // Wind and sway (snow/dust): each particle has its own phase —
        // neighbouring flakes sway out of sync.
        float t = q->max_life - q->life;
        q->vx += (q->wind[0] + q->sway * sinf(t * 2.5f + q->ph)) * dt;
        q->vy += q->wind[1] * dt;
        q->vz += (q->wind[2] + q->sway * cosf(t * 2.1f + q->ph)) * dt;
        // Turbulence: slow swirling of the trajectory (vortex):
        // circular motion in XZ at another frequency + slight Y wobble.
        if (q->turb > 0.001f) {
            q->vx += q->turb * 2.2f * cosf(t * 1.6f + q->ph * 2.0f) * dt;
            q->vz += q->turb * 2.2f * sinf(t * 1.9f + q->ph * 2.0f) * dt;
            q->vy += q->turb * 0.7f * sinf(t * 0.8f + q->ph) * dt;
        }

        q->x += q->vx * dt;
        q->y += q->vy * dt;
        q->z += q->vz * dt;
        q->size += q->sizek * dt;

        // Bounce off the ground (blood/sparks)
        if (q->bounce && p->world && q->vy < 0.0f) {
            float h = sample_height(p->world, q->x, q->z);
            if (q->y < h + 0.05f) {
                q->y = h + 0.05f;
                q->vy = -q->vy * 0.35f;
                q->vx *= 0.5f; q->vz *= 0.5f;
                if (q->vy < 0.8f) q->bounce = 0;   // damped out — lies still
            }
        }

        // Collision with the surface (rain/snow): terrain OR water level —
        // whichever of the two is higher. A raindrop dies on water/ground; a
        // snowflake settles on the surface and fades out briefly.
        if (q->collide && p->world && q->vy < 0.0f) {
            float h = sample_height(p->world, q->x, q->z);
            float ws = water_surface_at(p->world, q->x, q->z);
            if (ws > h) h = ws;                    // water above terrain — in water
            if (q->y <= h + 0.03f) {
                if (q->collide == 1) {
                    q->active = 0;                 // rain: the drop drowned
                    continue;
                }
                // snow: settles and fades (remaining life is short — fade)
                q->y = h + 0.03f;
                q->vx = q->vy = q->vz = 0.0f;
                q->gravity = 0.0f; q->sway = 0.0f;
                if (q->life > 0.5f) q->life = 0.5f;
                q->collide = 0;                    // lying — no more checks
            }
        }
    }
}

// --- Render: instanced draw of all live particles ------------------------------
// Two draw calls: first alpha-blended (normal ones), then additive (sparks,
// light). Additive needs GL_ONE, so the mode is split into passes.
void particles_draw(const RendererContext *rc)
{
    if (!s_p) return;
    if (rc->pass == RP_SHADOW) return;   // particles don't write into shadow maps
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
        it->size[1] = q->streak ? q->size * 8.0f : q->size;   // streak length
        it->col[0] = q->r; it->col[1] = q->g; it->col[2] = q->b;
        it->alpha = q->alpha1 + (q->alpha0 - q->alpha1) * k;   // fade over life
        it->vel[0] = q->vx; it->vel[1] = q->vy; it->vel[2] = q->vz;
        it->plain = q->plain ? 1.0f : 0.0f;
    }
    if (n_alpha + n_add == 0) return;

    glEnable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glUseProgram(p->prog);
    glUniformMatrix4fv(p->u_viewproj, 1, GL_FALSE, rc->viewproj);
    glUniform3f(p->u_camright, rc->view_right[0], rc->view_right[1], rc->view_right[2]);
    glUniform3f(p->u_camup, rc->view_up[0], rc->view_up[1], rc->view_up[2]);
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
    // params (p) is freed by node_free_tree(g->root)
}
