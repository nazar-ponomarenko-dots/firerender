// ============================================================================
//  bloom.c — постобработка glow (bloom)
// ----------------------------------------------------------------------------
//  Пайплайн: сцена (FBO) -> bright (половинное разрешение) -> гаусс H -> V
//  -> композит на экран. Сцена LDR (8 бит), поэтому порог яркости мягкий —
//  светятся самые яркие участки (небо, колосья против солнца).
// ============================================================================
#include "bloom.h"

#include "cvars.h"
#include "mapdata.h"
#include "resources.h"
#include "shader.h"

#include <stddef.h>

static BloomParams *s_bloom_p = NULL;   // для hot-reload (shader_on_reshade)
static void bloom_prog_init(BloomParams *b);
static void bloom_reshade(void) { if (s_bloom_p) bloom_prog_init(s_bloom_p); }

#include <SDL3/SDL.h>
#include <stdlib.h>

static GLuint make_color_tex(int w, int h)
{
    GLuint id;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    return id;
}

static void make_fbo(GLuint *fbo, GLuint *tex, GLuint *depth,
                     int w, int h, int want_depth)
{
    glGenFramebuffers(1, fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, *fbo);
    *tex = make_color_tex(w, h);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, *tex, 0);
    if (want_depth) {
        // Глубина сцены — ТЕКСТУРА (GL_DEPTH_COMPONENT24), чтобы 3D-сцена
        // тестировалась на глубину. Фильтр NEAREST — без сравнения.
        glGenTextures(1, depth);
        glBindTexture(GL_TEXTURE_2D, *depth);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, w, h, 0,
                     GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, NULL);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                               GL_TEXTURE_2D, *depth, 0);
    }
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        SDL_Log("Bloom FBO incomplete (%dx%d)!", w, h);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// Пересоздаёт программы постобработки и переищет uniform'ы (для hot-reload)
static void bloom_prog_init(BloomParams *b)
{
    b->prog_bright = make_program("shaders/postfx.vs", "shaders/bright.fs");
    b->prog_blur    = make_program("shaders/postfx.vs", "shaders/blur.fs");
    b->prog_composite = make_program("shaders/postfx.vs", "shaders/composite.fs");
    b->u_btex      = glGetUniformLocation(b->prog_bright, "uTex");
    b->u_threshold = glGetUniformLocation(b->prog_bright, "uThreshold");
    b->u_intensity = glGetUniformLocation(b->prog_bright, "uIntensity");
    b->u_blurtex   = glGetUniformLocation(b->prog_blur, "uTex");
    b->u_dir       = glGetUniformLocation(b->prog_blur, "uDir");
    b->u_texel     = glGetUniformLocation(b->prog_blur, "uTexel");
    b->u_ctex      = glGetUniformLocation(b->prog_composite, "uScene");
    b->u_glowtex   = glGetUniformLocation(b->prog_composite, "uGlow");
    b->u_strength  = glGetUniformLocation(b->prog_composite, "uStrength");
}

void bloom_init(BloomParams *b, int vw, int vh)
{
    s_bloom_p = b;
    // Параметры постобработки — из map/map.json (bloom = { ... }), как
    // значения по умолчанию cvar'ов: меню «Графика» и консоль правят ими
    // вживую (применяются в bloom_render каждый кадр).
    MapBloom bc;
    mapdata_bloom(&bc);
    cvar_register("r_bloom", 1.0f, "свечение (bloom) вкл/выкл");
    cvar_register("r_bloom_threshold", bc.threshold, "порог яркости bloom");
    cvar_register("r_bloom_intensity", bc.intensity, "сила ярких пикселей");
    cvar_register("r_bloom_strength", bc.strength, "сила свечения в композите");
    b->threshold = bc.threshold;
    b->intensity = bc.intensity;
    b->strength  = bc.strength;
    bloom_prog_init(b);    // программы + uniform'ы (для hot-reload)

    shader_on_reshade(bloom_reshade);

    // Полноэкранный треугольник: 3 вершины покрывают весь экран
    glGenVertexArrays(1, &b->vao);
    glBindVertexArray(b->vao);
    glGenBuffers(1, &b->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, b->vbo);
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

    bloom_resize(b, vw, vh);
}

void bloom_resize(BloomParams *b, int vw, int vh)
{
    if (vw <= 0 || vh <= 0) return;
    b->vw = vw;
    b->vh = vh;
    int gw = vw / 2, gh = vh / 2;
    if (gw < 1) gw = 1;
    if (gh < 1) gh = 1;
    b->gw = gw;
    b->gh = gh;

    if (b->fbo) {
        glDeleteFramebuffers(1, &b->fbo);
        glDeleteTextures(1, &b->tex);
        glDeleteTextures(1, &b->depth);
        glDeleteFramebuffers(2, b->fbo_glow);
        glDeleteTextures(2, b->tex_glow);
    }
    make_fbo(&b->fbo, &b->tex, &b->depth, vw, vh, 1);
    make_fbo(&b->fbo_glow[0], &b->tex_glow[0], NULL, gw, gh, 0);
    make_fbo(&b->fbo_glow[1], &b->tex_glow[1], NULL, gw, gh, 0);
}

GLuint bloom_scene_fbo(const BloomParams *b)
{
    return b->fbo;
}

GLuint bloom_scene_tex(const BloomParams *b)
{
    return b->tex;
}

GLuint bloom_scene_depth(const BloomParams *b)
{
    return b->depth;
}

void bloom_render(BloomParams *b, GLuint scene_tex, GLuint target_fbo)
{
    if (!b->fbo) return;
    // Настройки из cvar'ов (меню/консоль): применяются каждый кадр.
    // r_bloom = 0 — bloom выключен: яркие пиксели и блюр НЕ считаются,
    // рисуется только композит (сцена как есть, strength = 0).
    b->threshold = cvar_get("r_bloom_threshold");
    b->intensity = cvar_get("r_bloom_intensity");
    float strength = cvar_get("r_bloom_strength");
    if (cvar_get("r_bloom") < 0.5f) strength = 0.0f;
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);   // HUD мог оставить блендинг — постобработка пишет начисто
    glBindVertexArray(b->vao);

    if (strength > 0.0f) {
        // 1) яркие пиксели сцены -> glow0 (половинное разрешение)
        glBindFramebuffer(GL_FRAMEBUFFER, b->fbo_glow[0]);
        glViewport(0, 0, b->gw, b->gh);
        glUseProgram(b->prog_bright);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, scene_tex);
        glUniform1i(b->u_btex, 0);
        glUniform1f(b->u_threshold, b->threshold);
        glUniform1f(b->u_intensity, b->intensity);
        glDrawArrays(GL_TRIANGLES, 0, 3);

        // 2) гаусс: H (glow0 -> glow1), V (glow1 -> glow0)
        glUseProgram(b->prog_blur);
        glUniform1i(b->u_blurtex, 0);
        glUniform2f(b->u_texel, 1.0f / b->gw, 1.0f / b->gh);
        glBindFramebuffer(GL_FRAMEBUFFER, b->fbo_glow[1]);
        glUniform2f(b->u_dir, 1.0f, 0.0f);
        glBindTexture(GL_TEXTURE_2D, b->tex_glow[0]);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindFramebuffer(GL_FRAMEBUFFER, b->fbo_glow[0]);
        glUniform2f(b->u_dir, 0.0f, 1.0f);
        glBindTexture(GL_TEXTURE_2D, b->tex_glow[1]);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }

    // 3) композит: сцена + glow в целевой FBO (0 = экран, иначе — FBO тумана).
    // При strength <= 0 (bloom выключен) glow умножается на ноль — проход
    // служит копией сцены на экран (один квад вместо четырёх).
    glBindFramebuffer(GL_FRAMEBUFFER, target_fbo);
    glViewport(0, 0, b->vw, b->vh);
    glUseProgram(b->prog_composite);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, scene_tex);
    glUniform1i(b->u_ctex, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, b->tex_glow[0]);
    glUniform1i(b->u_glowtex, 1);
    glUniform1f(b->u_strength, strength);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

void bloom_cleanup(BloomParams *b)
{
    if (s_bloom_p == b) s_bloom_p = NULL;
    resources_program_release(b->prog_bright);
    resources_program_release(b->prog_blur);
    resources_program_release(b->prog_composite);
    glDeleteBuffers(1, &b->vbo);
    glDeleteVertexArrays(1, &b->vao);
    if (b->fbo) {
        glDeleteFramebuffers(1, &b->fbo);
        glDeleteTextures(1, &b->tex);
        glDeleteTextures(1, &b->depth);
        glDeleteFramebuffers(2, b->fbo_glow);
        glDeleteTextures(2, b->tex_glow);
        b->fbo = 0;
    }
}
