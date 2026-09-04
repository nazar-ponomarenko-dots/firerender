// ============================================================================
//  gi.c — динамическое глобальное освещение (см. gi.h)
// ============================================================================
#include "gi.h"

#include "cvars.h"
#include "linalg.h"
#include "resources.h"
#include "shader.h"

#include <SDL3/SDL.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

static GiParams *s_gi_p = NULL;      // для hot-reload (shader_on_reshade)

static void gi_prog_init(GiParams *g)
{
    g->t_scene         = glGetUniformLocation(g->prog_trace, "uScene");
    g->t_depth         = glGetUniformLocation(g->prog_trace, "uDepth");
    g->t_prev          = glGetUniformLocation(g->prog_trace, "uPrev");
    g->t_viewproj      = glGetUniformLocation(g->prog_trace, "uViewProj");
    g->t_invviewproj   = glGetUniformLocation(g->prog_trace, "uInvViewProj");
    g->t_prevviewproj  = glGetUniformLocation(g->prog_trace, "uPrevViewProj");
    g->t_campos        = glGetUniformLocation(g->prog_trace, "uCamPos");
    g->t_texel         = glGetUniformLocation(g->prog_trace, "uTexel");
    g->t_params        = glGetUniformLocation(g->prog_trace, "uParams");
    g->t_params2       = glGetUniformLocation(g->prog_trace, "uParams2");
    g->t_params3       = glGetUniformLocation(g->prog_trace, "uParams3");
    g->t_groundcolor   = glGetUniformLocation(g->prog_trace, "uGroundColor");
    g->t_clamp         = glGetUniformLocation(g->prog_trace, "uClamp");
    g->t_blend         = glGetUniformLocation(g->prog_trace, "uBlend");

    g->b_gi          = glGetUniformLocation(g->prog_blur, "uGI");
    g->b_depth       = glGetUniformLocation(g->prog_blur, "uDepth");
    g->b_invviewproj = glGetUniformLocation(g->prog_blur, "uInvViewProj");
    g->b_campos      = glGetUniformLocation(g->prog_blur, "uCamPos");
    g->b_texel       = glGetUniformLocation(g->prog_blur, "uTexel");
    g->b_dir         = glGetUniformLocation(g->prog_blur, "uDir");
    g->b_z           = glGetUniformLocation(g->prog_blur, "uZ");
    g->b_radius      = glGetUniformLocation(g->prog_blur, "uRadius");

    g->a_scene    = glGetUniformLocation(g->prog_apply, "uScene");
    g->a_gi       = glGetUniformLocation(g->prog_apply, "uGI");
    g->a_depth    = glGetUniformLocation(g->prog_apply, "uDepth");
    g->a_gitexel  = glGetUniformLocation(g->prog_apply, "uGiTexel");
    g->a_z        = glGetUniformLocation(g->prog_apply, "uZ");
    g->a_strength = glGetUniformLocation(g->prog_apply, "uStrength");
    g->a_ao       = glGetUniformLocation(g->prog_apply, "uAoStrength");
    g->a_debug    = glGetUniformLocation(g->prog_apply, "uDebug");
}

static void gi_reshade(void)
{
    GiParams *g = s_gi_p;
    if (!g) return;
    GLuint p1 = resources_program_replace(g->prog_trace, "shaders/postfx.vs",
                                          "shaders/gi.fs");
    GLuint p2 = resources_program_replace(g->prog_blur, "shaders/postfx.vs",
                                          "shaders/gi_blur.fs");
    GLuint p3 = resources_program_replace(g->prog_apply, "shaders/postfx.vs",
                                          "shaders/gi_apply.fs");
    if (p1) g->prog_trace = p1;
    if (p2) g->prog_blur = p2;
    if (p3) g->prog_apply = p3;
    gi_prog_init(g);
    g->frames = 0;              // история могла стать несовместимой
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
        SDL_Log("GI FBO incomplete (%dx%d)!", w, h);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void gi_init(GiParams *g, int vw, int vh)
{
    s_gi_p = g;
    cvar_register_ex("r_gi", 1.0f, 0.0f, 1.0f, CVAR_ARCHIVE,
                     "динамическое глобальное освещение (GI, аналог Lumen)");
    // Силы уменьшены втрое против предыдущей ревизии (0.85 / 0.85 / 1.05 /
    // 0.55): GI подсвечивал сам себя и половина сцены светилась. Плюс в
    // gi_apply.fs добавка теперь физически ограничена яркостью пикселя.
    cvar_register_ex("r_gi_strength", 0.28f, 0.0f, 3.0f, CVAR_ARCHIVE,
                     "сила непрямого света GI");
    cvar_register_ex("r_gi_ao", 0.3f, 0.0f, 1.0f, CVAR_ARCHIVE,
                     "сила затенения GI (контактные тени + закрытость неба)");
    cvar_register_ex("r_gi_rays", 8.0f, 2.0f, 32.0f, CVAR_ARCHIVE,
                     "лучей на пиксель (дороже = чище)");
    cvar_register_ex("r_gi_steps", 12.0f, 4.0f, 32.0f, CVAR_ARCHIVE,
                     "шагов марша луча GI");
    cvar_register_ex("r_gi_radius", 12.0f, 1.0f, 64.0f, CVAR_ARCHIVE,
                     "радиус сбора непрямого света, м");
    cvar_register_ex("r_gi_ao_radius", 2.5f, 0.2f, 16.0f, CVAR_ARCHIVE,
                     "радиус ambient occlusion, м");
    cvar_register_ex("r_gi_sky", 0.25f, 0.0f, 1.0f, CVAR_ARCHIVE,
                     "крупное затенение по закрытости небосвода (игра света)");
    cvar_register_ex("r_gi_clamp", 1.0f, 0.1f, 8.0f, CVAR_ARCHIVE,
                     "потолок энергии непрямого света на пиксель");
    cvar_register_ex("r_gi_bounce", 1.15f, 1.0f, 2.5f, CVAR_ARCHIVE,
                     "насыщенность отражённого света (цветные рефлексы)");
    cvar_register_ex("r_gi_reflect", 0.35f, 0.0f, 1.0f, CVAR_ARCHIVE,
                     "зеркальный отблеск GI (отражения света на поверхностях)");
    // Максимум 1.0 безопасен: hit_radiance делит на (1 + spread), поэтому
    // цепочка переотражений перераспределяет энергию, а не добавляет её.
    cvar_register_ex("r_gi_spread", 0.18f, 0.0f, 1.0f, CVAR_ARCHIVE,
                     "распространение света: переотражения по цепочке");
    cvar_register_ex("r_gi_temporal", 0.88f, 0.0f, 0.96f, CVAR_ARCHIVE,
                     "темпоральное накопление GI (0 = выкл, шумно)");
    cvar_register_ex("r_gi_blur", 1.6f, 0.0f, 4.0f, CVAR_ARCHIVE,
                     "радиус денойза GI (тексели)");
    cvar_register_ex("r_gi_scale", 0.5f, 0.25f, 1.0f, CVAR_ARCHIVE,
                     "разрешение GI от экрана (0.5 = половина, дешевле)");
    cvar_register_ex("r_gi_debug", 0.0f, 0.0f, 2.0f, CVAR_ARCHIVE,
                     "отладка GI: 1 только непрямой свет, 2 только AO");

    g->prog_trace = resources_program_replace(0, "shaders/postfx.vs",
                                              "shaders/gi.fs");
    g->prog_blur  = resources_program_replace(0, "shaders/postfx.vs",
                                              "shaders/gi_blur.fs");
    g->prog_apply = resources_program_replace(0, "shaders/postfx.vs",
                                              "shaders/gi_apply.fs");
    gi_prog_init(g);
    shader_on_reshade(gi_reshade);

    glGenVertexArrays(1, &g->vao);
    glBindVertexArray(g->vao);
    glGenBuffers(1, &g->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g->vbo);
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

    gi_resize(g, vw, vh);
}

void gi_resize(GiParams *g, int vw, int vh)
{
    if (vw <= 0 || vh <= 0) return;
    float sc = cvar_get("r_gi_scale");
    if (sc < 0.25f) sc = 0.25f;
    if (sc > 1.0f) sc = 1.0f;
    g->vw = vw;
    g->vh = vh;
    int gw = (int)(vw * sc), gh = (int)(vh * sc);
    if (gw < 8) gw = 8;
    if (gh < 8) gh = 8;
    if (g->fbo_gi[0]) {
        glDeleteFramebuffers(2, g->fbo_gi);
        glDeleteTextures(2, g->tex_gi);
        glDeleteFramebuffers(1, &g->fbo_tmp);
        glDeleteTextures(1, &g->tex_tmp);
        glDeleteFramebuffers(1, &g->fbo_out);
        glDeleteTextures(1, &g->tex_out);
    }
    g->gw = gw;
    g->gh = gh;
    make_target(&g->fbo_gi[0], &g->tex_gi[0], gw, gh);
    make_target(&g->fbo_gi[1], &g->tex_gi[1], gw, gh);
    make_target(&g->fbo_tmp, &g->tex_tmp, gw, gh);
    make_target(&g->fbo_out, &g->tex_out, vw, vh);
    g->cur = 0;
    g->frames = 0;
    g->have_prev = 0;
}

int gi_is_on(void)
{
    return cvar_get("r_gi") >= 0.5f &&
           (cvar_get("r_gi_strength") > 0.001f || cvar_get("r_gi_ao") > 0.001f);
}

GLuint gi_render(GiParams *g, GLuint scene_tex, GLuint scene_depth,
                 const RendererContext *rc)
{
    if (!g->fbo_gi[0] || !gi_is_on()) return scene_tex;
    // Смена r_gi_scale на ходу: пересобрать таргеты (дешевле, чем ветвление)
    {
        float sc = cvar_get("r_gi_scale");
        int want_w = (int)(g->vw * (sc < 0.25f ? 0.25f : (sc > 1.0f ? 1.0f : sc)));
        if (want_w > 8 && abs(want_w - g->gw) > 2) gi_resize(g, g->vw, g->vh);
    }

    float invvp[16];
    mat4_invert(invvp, rc->viewproj);
    int cur = g->cur ^ 1;                       // пишем в свободный буфер
    float blend = cvar_get("r_gi_temporal");
    if (blend > 0.96f) blend = 0.96f;
    if (g->frames == 0 || !g->have_prev) blend = 0.0f;

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glBindVertexArray(g->vao);

    // --- 1) Трассировка + темпоральное накопление ---------------------------
    glBindFramebuffer(GL_FRAMEBUFFER, g->fbo_gi[cur]);
    glViewport(0, 0, g->gw, g->gh);
    glUseProgram(g->prog_trace);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, scene_tex);
    glUniform1i(g->t_scene, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, scene_depth);
    glUniform1i(g->t_depth, 1);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, g->tex_gi[g->cur]);    // прошлый кадр
    glUniform1i(g->t_prev, 2);
    glUniformMatrix4fv(g->t_viewproj, 1, GL_FALSE, rc->viewproj);
    glUniformMatrix4fv(g->t_invviewproj, 1, GL_FALSE, invvp);
    glUniformMatrix4fv(g->t_prevviewproj, 1, GL_FALSE,
                       g->have_prev ? g->prev_vp : rc->viewproj);
    glUniform3f(g->t_campos, rc->view_pos[0], rc->view_pos[1], rc->view_pos[2]);
    glUniform2f(g->t_texel, 1.0f / (float)g->gw, 1.0f / (float)g->gh);
    glUniform4f(g->t_params, cvar_get("r_gi_rays"), cvar_get("r_gi_steps"),
                cvar_get("r_gi_radius"), rc->znear);
    // Фаза джиттера: меняется каждый кадр — накопление усредняет разные лучи
    glUniform4f(g->t_params2, rc->zfar, cvar_get("r_gi_sky"),
                (float)(g->frames % 64) * 1.618f, cvar_get("r_gi_ao_radius"));
    glUniform4f(g->t_params3, cvar_get("r_gi_bounce"),
                cvar_get("r_gi_reflect"), cvar_get("r_gi_spread"), 0.0f);
    glUniform3f(g->t_groundcolor, rc->ground_color[0], rc->ground_color[1],
                rc->ground_color[2]);
    glUniform1f(g->t_clamp, cvar_get("r_gi_clamp"));
    glUniform1f(g->t_blend, blend);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    // --- 2) Билатеральный денойз (H, затем V) -------------------------------
    float br = cvar_get("r_gi_blur");
    GLuint denoised = g->tex_gi[cur];
    if (br > 0.01f) {
        glUseProgram(g->prog_blur);
        glUniform1i(g->b_gi, 0);
        glUniform1i(g->b_depth, 1);
        glUniformMatrix4fv(g->b_invviewproj, 1, GL_FALSE, invvp);
        glUniform3f(g->b_campos, rc->view_pos[0], rc->view_pos[1],
                    rc->view_pos[2]);
        glUniform2f(g->b_texel, 1.0f / (float)g->gw, 1.0f / (float)g->gh);
        glUniform2f(g->b_z, rc->znear, rc->zfar);
        glUniform1f(g->b_radius, br);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, scene_depth);
        // H: gi[cur] -> tmp
        glBindFramebuffer(GL_FRAMEBUFFER, g->fbo_tmp);
        glViewport(0, 0, g->gw, g->gh);
        glUniform2f(g->b_dir, 1.0f, 0.0f);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, g->tex_gi[cur]);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        // V: tmp -> gi[g->cur] (старый буфер: он уже прочитан как история).
        // ВАЖНО: пишем НЕ в gi[cur] — он остаётся «сырым» накопителем для
        // следующего кадра (иначе размытие копилось бы экспоненциально).
        glBindFramebuffer(GL_FRAMEBUFFER, g->fbo_gi[g->cur]);
        glUniform2f(g->b_dir, 0.0f, 1.0f);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, g->tex_tmp);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        denoised = g->tex_gi[g->cur];
    }

    // --- 3) Наложение на сцену ---------------------------------------------
    glBindFramebuffer(GL_FRAMEBUFFER, g->fbo_out);
    glViewport(0, 0, g->vw, g->vh);
    glUseProgram(g->prog_apply);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, scene_tex);
    glUniform1i(g->a_scene, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, denoised);
    glUniform1i(g->a_gi, 1);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, scene_depth);
    glUniform1i(g->a_depth, 2);
    glUniform2f(g->a_gitexel, 1.0f / (float)g->gw, 1.0f / (float)g->gh);
    glUniform2f(g->a_z, rc->znear, rc->zfar);
    glUniform1f(g->a_strength, cvar_get("r_gi_strength"));
    glUniform1f(g->a_ao, cvar_get("r_gi_ao"));
    glUniform1f(g->a_debug, cvar_get("r_gi_debug"));
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glBindVertexArray(0);
    glActiveTexture(GL_TEXTURE0);
    glEnable(GL_DEPTH_TEST);

    memcpy(g->prev_vp, rc->viewproj, sizeof g->prev_vp);
    g->have_prev = 1;
    // История для следующего кадра — СЫРОЙ накопитель (gi[cur]); denoised
    // писался в gi[g->cur], поэтому свапаем так, чтобы cur стал историей.
    g->cur = cur;
    g->frames++;
    return g->tex_out;
}

void gi_cleanup(GiParams *g)
{
    if (s_gi_p == g) s_gi_p = NULL;
    if (g->prog_trace) resources_program_release(g->prog_trace);
    if (g->prog_blur) resources_program_release(g->prog_blur);
    if (g->prog_apply) resources_program_release(g->prog_apply);
    g->prog_trace = g->prog_blur = g->prog_apply = 0;
    glDeleteBuffers(1, &g->vbo);
    glDeleteVertexArrays(1, &g->vao);
    if (g->fbo_gi[0]) {
        glDeleteFramebuffers(2, g->fbo_gi);
        glDeleteTextures(2, g->tex_gi);
        glDeleteFramebuffers(1, &g->fbo_tmp);
        glDeleteTextures(1, &g->tex_tmp);
        glDeleteFramebuffers(1, &g->fbo_out);
        glDeleteTextures(1, &g->tex_out);
    }
    memset(g->fbo_gi, 0, sizeof g->fbo_gi);
    memset(g->tex_gi, 0, sizeof g->tex_gi);
    g->fbo_tmp = g->tex_tmp = g->fbo_out = g->tex_out = 0;
    g->frames = 0;
    g->have_prev = 0;
}
