// ============================================================================
//  ssr.c — Screen Space Reflections
// ----------------------------------------------------------------------------
//  Post pass AFTER the scene, BEFORE bloom/fog: reads scene color+depth,
//  reconstructs world positions/normals from depth, marches the reflected
//  ray across the screen (growing step + binary refinement). On a miss — the
//  sky cubemap (renderer_refl_cubemap), gated by wetness and the terrain mask
//  (terrain_wetness_mask). Result — scene+reflection in its own FBO.
// ============================================================================
#include "ssr.h"

#include "cvars.h"
#include "game.h"
#include "linalg.h"
#include "renderer.h"
#include "resources.h"
#include "shader.h"
#include "terrain.h"

#include <stddef.h>

static SsrParams *s_ssr_p = NULL;   // for hot-reload (shader_on_reshade)
static void ssr_prog_init(SsrParams *s);
static void ssr_reshade(void);

#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>

static void ssr_prog_init(SsrParams *s)
{
    s->u_scene       = glGetUniformLocation(s->prog, "uScene");
    s->u_depth       = glGetUniformLocation(s->prog, "uDepth");
    s->u_cube        = glGetUniformLocation(s->prog, "uCube");
    s->u_wetmask     = glGetUniformLocation(s->prog, "uWetMask");
    s->u_prev        = glGetUniformLocation(s->prog, "uPrev");
    s->u_viewproj    = glGetUniformLocation(s->prog, "uViewProj");
    s->u_invviewproj = glGetUniformLocation(s->prog, "uInvViewProj");
    s->u_prevviewproj = glGetUniformLocation(s->prog, "uPrevViewProj");
    s->u_campos      = glGetUniformLocation(s->prog, "uCamPos");
    s->u_texel       = glGetUniformLocation(s->prog, "uTexel");
    s->u_worldsize   = glGetUniformLocation(s->prog, "uWorldSize");
    s->u_params      = glGetUniformLocation(s->prog, "uParams");
    s->u_misc        = glGetUniformLocation(s->prog, "uMisc");
    s->u_debug       = glGetUniformLocation(s->prog, "uDebug");
    s->u_wetness     = glGetUniformLocation(s->prog, "uWetness");
    s->u_strength    = glGetUniformLocation(s->prog, "uStrength");
    s->u_blend       = glGetUniformLocation(s->prog, "uBlend");
}

static void ssr_reshade(void)
{
    SsrParams *s = s_ssr_p;
    if (!s) return;
    GLuint p = resources_program_replace(s->prog, "shaders/postfx.vs",
                                         "shaders/ssr.fs");
    if (p) s->prog = p;
    ssr_prog_init(s);
}

void ssr_init(SsrParams *s, int vw, int vh)
{
    s_ssr_p = s;
    cvar_register_ex("r_ssr",          1.0f, 0.0f, 1.0f,  CVAR_ARCHIVE,
                     "отражения экранного пространства (SSR)");
    cvar_register_ex("r_ssr_strength", 0.7f, 0.0f, 1.0f, CVAR_ARCHIVE,
                     "сила SSR-отражений");
    cvar_register_ex("r_ssr_steps",    16.0f, 4.0f, 32.0f, CVAR_ARCHIVE,
                     "шагов марша SSR (дороже=дальше)");
    cvar_register_ex("r_ssr_maxdist",  24.0f, 4.0f, 64.0f, CVAR_ARCHIVE,
                     "дальность марша SSR (м)");
    cvar_register_ex("r_ssr_thick",    0.20f, 0.02f, 1.0f, CVAR_ARCHIVE,
                     "толщина окна хита SSR (м)");
    cvar_register_ex("r_ssr_rough",    0.06f, 0.0f, 1.0f, CVAR_ARCHIVE,
                     "шероховатость SSR-отражений (блюр)");
    cvar_register_ex("r_ssr_fallback", 1.0f, 0.0f, 1.0f,  CVAR_ARCHIVE,
                     "SSR фолбэк: кубомап неба при промахе");
    cvar_register_ex("r_ssr_mask",     1.0f, 0.0f, 1.0f,  CVAR_ARCHIVE,
                     "маска мокрости рельефа в SSR");
    cvar_register_ex("r_ssr_wet",      0.0f, 0.0f, 1.0f,  CVAR_ARCHIVE,
                     "тест: принудительная мокрость SSR (без дождя)");
    cvar_register_ex("r_ssr_debug",    0.0f, 0.0f, 4.0f,  CVAR_ARCHIVE,
                     "отладка SSR: 1 зоны, 2 хит/фолбэк, 3 вес, 4 цвет отражения");
    cvar_register_ex("r_ssr_temporal", 0.75f, 0.0f, 0.95f, CVAR_ARCHIVE,
                     "темпоральное сглаживание SSR (0 = выкл, против мерцания)");
    s->prog = resources_program_replace(0, "shaders/postfx.vs", "shaders/ssr.fs");
    ssr_prog_init(s);
    shader_on_reshade(ssr_reshade);

    glGenVertexArrays(1, &s->vao);
    glBindVertexArray(s->vao);
    glGenBuffers(1, &s->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, s->vbo);
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

    ssr_resize(s, vw, vh);
}

static void make_target(GLuint *fbo, GLuint *tex, int vw, int vh)
{
    glGenTextures(1, tex);
    glBindTexture(GL_TEXTURE_2D, *tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, vw, vh, 0,
                 GL_RGBA, GL_HALF_FLOAT, NULL);
    glGenFramebuffers(1, fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, *fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, *tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        SDL_Log("SSR FBO incomplete (%dx%d)!", vw, vh);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void ssr_resize(SsrParams *s, int vw, int vh)
{
    if (vw <= 0 || vh <= 0) return;
    s->vw = vw;
    s->vh = vh;
    if (s->fbo[0]) {
        glDeleteFramebuffers(2, s->fbo);
        glDeleteTextures(2, s->tex);
    }
    make_target(&s->fbo[0], &s->tex[0], vw, vh);
    make_target(&s->fbo[1], &s->tex[1], vw, vh);
    s->cur = s->last = 0;
    s->frames = 0;   // temporal history reset — no startup flicker
}

void ssr_render(SsrParams *s, GLuint scene_tex, GLuint scene_depth,
                const RendererContext *rc)
{
    if (!s->fbo[0]) return;
    float strength = cvar_get("r_ssr_strength");
    if (strength > 1.0f) strength = 1.0f;   // config.cfg could set >1 without clamping
    float wet = rc->wetness > cvar_get("r_ssr_wet") ? rc->wetness
                                                    : cvar_get("r_ssr_wet");
    if (cvar_get("r_ssr") < 0.5f || strength <= 0.0f || wet <= 0.001f)
        return;   // SSR off / dry — use the scene as is

    float invvp[16];
    mat4_invert(invvp, rc->viewproj);
    const WorldState *w = &game_instance()->world;
    int wetmask = cvar_get("r_ssr_mask") > 0.5f && terrain_wetness_mask_have();
    // Temporal smoothing: blend the current frame with the result one frame
    // back (uPrev) — the screen-space march is discrete, so without this step
    // the reflection "jitters" on every pixel. uBlend=0 on the first frame
    // (history is still garbage) and after a resize (s->frames==0).
    float blend = cvar_get("r_ssr_temporal");
    if (blend > 0.95f) blend = 0.95f;
    if (s->frames == 0) blend = 0.0f;
    int cur = s->cur ^ 1;           // ping-pong: write into the other buffer

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glBindFramebuffer(GL_FRAMEBUFFER, s->fbo[cur]);
    glViewport(0, 0, s->vw, s->vh);
    glUseProgram(s->prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, scene_tex);
    glUniform1i(s->u_scene, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, scene_depth);
    glUniform1i(s->u_depth, 1);
    glActiveTexture(GL_TEXTURE2);
    GLuint cube = renderer_refl_cubemap();
    // renderer always creates a cubemap at init (renderer.c:213-222 — from
    // cubemaps/ files, else a solid-color one), so in practice `cube` is never 0
    // (measured: cube=48 on map1). The old fallback still bound the 2D scene
    // texture to GL_TEXTURE_CUBE_MAP, which is INVALID_OPERATION — a texture
    // cannot be bound to a target it was not created for. If the cubemap ever
    // goes away, binding nothing is the honest fallback: the samplerCube reads
    // (0,0,0,1), so a puddle reflects a dark sky instead of the driver rejecting
    // the pass.
    glBindTexture(GL_TEXTURE_CUBE_MAP, cube);
    glUniform1i(s->u_cube, 2);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, wetmask ? terrain_wetness_mask_tex() : scene_depth);
    glUniform1i(s->u_wetmask, 3);
    glActiveTexture(GL_TEXTURE4);                    // previous frame (anti-flicker)
    glBindTexture(GL_TEXTURE_2D, s->tex[s->cur]);    // cur not written yet — read the old one
    glUniform1i(s->u_prev, 4);
    glUniformMatrix4fv(s->u_viewproj, 1, GL_FALSE, rc->viewproj);
    glUniformMatrix4fv(s->u_invviewproj, 1, GL_FALSE, invvp);
    // viewproj of the previous frame for reprojection (anti-flicker); on the first
    // frame — the current one (uBlend is 0 anyway)
    glUniformMatrix4fv(s->u_prevviewproj, 1, GL_FALSE,
                       s->have_prev ? s->prev_vp : rc->viewproj);
    // Position of the VIEW camera (matches viewproj, from which ssr.fs
    // recovers world points): in 3rd person this is the camera behind the character.
    glUniform3f(s->u_campos, rc->view_pos[0], rc->view_pos[1], rc->view_pos[2]);
    glUniform2f(s->u_texel, 1.0f / (float)s->vw, 1.0f / (float)s->vh);
    glUniform2f(s->u_worldsize, w->world_w, w->world_d);
    glUniform4f(s->u_params, cvar_get("r_ssr_steps"), cvar_get("r_ssr_maxdist"),
                cvar_get("r_ssr_thick"), cvar_get("r_ssr_rough"));
    glUniform4f(s->u_misc, rc->znear, rc->zfar,
                cvar_get("r_ssr_fallback") > 0.5f ? 1.0f : 0.0f,
                wetmask ? 1.0f : 0.0f);
    glUniform1f(s->u_wetness, wet);
    glUniform1f(s->u_strength, strength);
    glUniform1f(s->u_debug, cvar_get("r_ssr_debug"));
    glUniform1f(s->u_blend, blend);
    glBindVertexArray(s->vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glActiveTexture(GL_TEXTURE0);
    memcpy(s->prev_vp, rc->viewproj, sizeof s->prev_vp);
    s->have_prev = 1;
    s->last = s->cur;
    s->cur = cur;
    s->frames++;
    glEnable(GL_DEPTH_TEST);
}

GLuint ssr_tex(const SsrParams *s)
{
    return s->tex[s->cur];   // the most recently written frame
}

void ssr_cleanup(SsrParams *s)
{
    if (s_ssr_p == s) s_ssr_p = NULL;
    resources_program_release(s->prog);
    glDeleteBuffers(1, &s->vbo);
    glDeleteVertexArrays(1, &s->vao);
    if (s->fbo[0]) {
        glDeleteFramebuffers(2, s->fbo);
        glDeleteTextures(2, s->tex);
    }
    s->fbo[0] = s->fbo[1] = 0;
    s->tex[0] = s->tex[1] = 0;
    s->frames = 0;
}
