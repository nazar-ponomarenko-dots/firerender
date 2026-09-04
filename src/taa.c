// ============================================================================
//  taa.c — временное сглаживание (см. taa.h)
// ============================================================================
#include "taa.h"

#include "cvars.h"
#include "linalg.h"
#include "resources.h"
#include "shader.h"

#include <SDL3/SDL.h>
#include <string.h>

static TaaParams *s_taa_p = NULL;    // для hot-reload (shader_on_reshade)

static void taa_prog_init(TaaParams *t)
{
    t->t_scene   = glGetUniformLocation(t->prog, "uScene");
    t->t_depth   = glGetUniformLocation(t->prog, "uDepth");
    t->t_hist    = glGetUniformLocation(t->prog, "uHistory");
    t->t_invvp   = glGetUniformLocation(t->prog, "uInvViewProj");
    t->t_prevvp  = glGetUniformLocation(t->prog, "uPrevViewProj");
    t->t_campos  = glGetUniformLocation(t->prog, "uCamPos");
    t->t_texel   = glGetUniformLocation(t->prog, "uTexel");
    t->t_z       = glGetUniformLocation(t->prog, "uZ");
    t->t_blend   = glGetUniformLocation(t->prog, "uBlend");
}

static void taa_reshade(void)
{
    TaaParams *t = s_taa_p;
    if (!t) return;
    GLuint p = resources_program_replace(t->prog, "shaders/postfx.vs",
                                         "shaders/taa.fs");
    if (p) t->prog = p;
    taa_prog_init(t);
    t->frames = 0;
    t->have_prev = 0;
}

static void make_target(GLuint *fbo, GLuint *tex, int w, int h)
{
    glGenTextures(1, tex);
    glBindTexture(GL_TEXTURE_2D, *tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA,
                 GL_HALF_FLOAT, NULL);
    glGenFramebuffers(1, fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, *fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, *tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        SDL_Log("TAA FBO incomplete (%dx%d)!", w, h);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void taa_init(TaaParams *t, int vw, int vh)
{
    s_taa_p = t;
    cvar_register_ex("r_taa", 1.0f, 0.0f, 1.0f, CVAR_ARCHIVE,
                     "временное сглаживание (TAA): гладкие края, мягкий шум");
    memset(t, 0, sizeof *t);
    t->prog = resources_program_replace(0, "shaders/postfx.vs", "shaders/taa.fs");
    taa_prog_init(t);
    shader_on_reshade(taa_reshade);

    glGenVertexArrays(1, &t->vao);
    glBindVertexArray(t->vao);
    glGenBuffers(1, &t->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, t->vbo);
    const float tri[3][4] = {
        { -1, -1, 0, 0 },
        {  3, -1, 2, 0 },
        { -1,  3, 0, 2 },
    };
    glBufferData(GL_ARRAY_BUFFER, sizeof tri, tri, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), NULL);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          (const void *)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);

    taa_resize(t, vw, vh);
}

void taa_resize(TaaParams *t, int vw, int vh)
{
    if (vw <= 0 || vh <= 0) return;
    if (t->tex_hist[0]) {
        glDeleteFramebuffers(2, t->fbo_hist);
        glDeleteTextures(2, t->tex_hist);
    }
    t->vw = vw;
    t->vh = vh;
    make_target(&t->fbo_hist[0], &t->tex_hist[0], vw, vh);
    make_target(&t->fbo_hist[1], &t->tex_hist[1], vw, vh);
    t->cur = 0;
    t->frames = 0;
    t->have_prev = 0;
}

// 8-фазный Halton(2,3): смещения в пикселях, покрывают субпиксельную сетку.
static const float s_halton2[8] = { 0.0f, 0.5f, 0.25f, 0.75f,
                                    0.125f, 0.625f, 0.375f, 0.875f };
static const float s_halton3[8] = { 0.0f, 0.3333f, 0.6667f, 0.1111f,
                                    0.4444f, 0.7778f, 0.2222f, 0.5556f };

void taa_jitter(TaaParams *t, RendererContext *rc)
{
    if (!taa_is_on()) return;
    int i = t->frames & 7;
    // Сдвиг в NDC через матрицу поверх viewproj: смещение одно и то же для
    // всех глубин (в отличие от правки столбца трансляции).
    float J[16] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,
                    (s_halton2[i] - 0.5f) * 2.0f / (float)t->vw,
                    (s_halton3[i] - 0.5f) * 2.0f / (float)t->vh,
                    0, 1 };
    float vp[16];
    memcpy(vp, rc->viewproj, sizeof vp);
    mat4_mul(rc->viewproj, J, vp);
}

int taa_is_on(void)
{
    return cvar_get("r_taa") >= 0.5f;
}

GLuint taa_render(TaaParams *t, GLuint scene_tex, GLuint scene_depth,
                  const RendererContext *rc)
{
    if (!t->tex_hist[0] || !taa_is_on()) {
        // TAA выключен — матрица прошлого кадра не нужна, но фазу двигаем,
        // чтобы при включении не начинать с застарелой истории
        return scene_tex;
    }
    int cur = t->cur ^ 1;
    float blend = 0.9f;
    if (t->frames == 0 || !t->have_prev) blend = 0.0f;

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glBindVertexArray(t->vao);

    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo_hist[cur]);
    glViewport(0, 0, t->vw, t->vh);
    glUseProgram(t->prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, scene_tex);
    glUniform1i(t->t_scene, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, scene_depth);
    glUniform1i(t->t_depth, 1);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, t->tex_hist[t->cur]);
    glUniform1i(t->t_hist, 2);
    float invvp[16];
    mat4_invert(invvp, rc->viewproj);
    glUniformMatrix4fv(t->t_invvp, 1, GL_FALSE, rc->viewproj);
    glUniformMatrix4fv(t->t_prevvp, 1, GL_FALSE,
                       t->have_prev ? t->prev_vp : rc->viewproj);
    glUniform3f(t->t_campos, rc->view_pos[0], rc->view_pos[1], rc->view_pos[2]);
    glUniform2f(t->t_texel, 1.0f / (float)t->vw, 1.0f / (float)t->vh);
    glUniform2f(t->t_z, rc->znear, rc->zfar);
    glUniform1f(t->t_blend, blend);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glBindVertexArray(0);
    glActiveTexture(GL_TEXTURE0);
    glEnable(GL_DEPTH_TEST);

    memcpy(t->prev_vp, rc->viewproj, sizeof t->prev_vp);
    t->have_prev = 1;
    t->cur = cur;
    t->frames++;
    return t->tex_hist[cur];
}

void taa_cleanup(TaaParams *t)
{
    if (s_taa_p == t) s_taa_p = NULL;
    if (t->prog) resources_program_release(t->prog);
    t->prog = 0;
    glDeleteBuffers(1, &t->vbo);
    glDeleteVertexArrays(1, &t->vao);
    if (t->tex_hist[0]) {
        glDeleteFramebuffers(2, t->fbo_hist);
        glDeleteTextures(2, t->tex_hist);
        memset(t->fbo_hist, 0, sizeof t->fbo_hist);
        memset(t->tex_hist, 0, sizeof t->tex_hist);
    }
    t->frames = 0;
    t->have_prev = 0;
}
