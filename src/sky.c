// ============================================================================
//  sky.c — skydome from a panorama: Skybox3D node + horizon fog color
// ============================================================================
#include "sky.h"

#include "assets.h"
#include "cvars.h"
#include "gl_loader.h"
#include "linalg.h"
#include "mapdata.h"
#include "resources.h"
#include "shader.h"

#include "stb_image.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <stdlib.h>

typedef struct {
    GLuint prog, vao, vbo;         // program and the full-screen triangle
    GLuint tex;                    // sky panorama (img/skybox.png)
    GLuint noise_tex;              // img/noise.png (procedural clouds)
    GLint u_forward, u_right, u_up, u_tanfov, u_aspect;
    GLint u_sundir, u_suncolor, u_sunglow;
    GLint u_noise, u_camxz, u_time, u_clouds;
    const RendererContext *rc;     // camera basis and sun (renderer.c)
} SkyParams;
static SkyParams *s_sky = NULL;   // for hot-reload (shader_on_reshade)
static void sky_reshade(void);

static float g_fog_rgb[3] = { 0.6f, 0.65f, 0.7f };   // until the sky is loaded

SceneNode *sky_make_node(const RendererContext *rc)
{
    // Panorama and tint come from map/map.json (sky = { tex, tint })
    MapSky sc;
    mapdata_sky(&sc);
    GLuint prog = make_program("shaders/sky.vs", "shaders/sky.fs");
    int sw = 0, sh = 0;
    unsigned char *img = load_img(sc.tex, &sw, &sh, STBI_rgb);
    if (!prog || !img) {
        if (prog) resources_program_release(prog);
        return NULL;
    }

    // Sky: REPEAT on X removes the panorama seam at the 360-degree junction.
    // LINEAR (last argument 1) — no banding/pixelation on the dome
    // (the 512x128 panorama gives ~1.4 px/degree — NEAREST looked dirty).
    // The texture goes through the resource cache (pixels already loaded for the fog color).
    GLuint tex = resources_texture(sc.tex, 3, GL_REPEAT, 1);

    // Fog color is taken from the middle of the sky panorama — the horizon meets the sky
    float fr = 0, fg = 0, fb = 0, fn = 0;
    for (int y = sh * 40 / 100; y < sh * 65 / 100; y++)
        for (int x = 0; x < sw; x++) {
            const unsigned char *p = img + ((size_t)y * sw + x) * 3;
            fr += p[0] * sc.tint[0];
            fg += p[1] * sc.tint[1];
            fb += p[2] * sc.tint[2];
            fn++;
        }
    fn = fn > 0 ? fn : 1;
    g_fog_rgb[0] = fr / fn / 255.0f;
    g_fog_rgb[1] = fg / fn / 255.0f;
    g_fog_rgb[2] = fb / fn / 255.0f;
    // Pixels are owned by the resource cache — do not free (see resources.h)

    // Full-screen triangle
    GLuint vao, vbo;
    float sky_verts[3][2] = { {-1, -1}, {3, -1}, {-1, 3} };
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof sky_verts, sky_verts, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), NULL);
    glEnableVertexAttribArray(0);

    SkyParams *p = calloc(1, sizeof *p);
    if (!p) {
        glDeleteBuffers(1, &vbo);
        glDeleteVertexArrays(1, &vao);
        resources_program_release(prog);
        resources_texture_release(tex);
        return NULL;
    }
    p->prog = prog;
    p->vao = vao;
    p->vbo = vbo;
    p->tex = tex;
    p->rc = rc;
    p->u_forward = glGetUniformLocation(prog, "uForward");
    p->u_right   = glGetUniformLocation(prog, "uRight");
    p->u_up      = glGetUniformLocation(prog, "uUp");
    p->u_tanfov  = glGetUniformLocation(prog, "uTanFov");
    p->u_aspect  = glGetUniformLocation(prog, "uAspect");
    p->u_sundir  = glGetUniformLocation(prog, "uSunDir");
    p->u_suncolor = glGetUniformLocation(prog, "uSunColor");
    p->u_sunglow  = glGetUniformLocation(prog, "uSunGlow");
    p->u_noise    = glGetUniformLocation(prog, "uNoise");
    p->u_camxz    = glGetUniformLocation(prog, "uCamXZ");
    p->u_time     = glGetUniformLocation(prog, "uTime");
    p->u_clouds   = glGetUniformLocation(prog, "uClouds");
    // Noise for procedural clouds (shared resource cache, REPEAT):
    // img/cloud_noise.png — smooth tileable FBM (tools/make_cloud_noise.py),
    // img/noise.png — white grain, clouds from it would turn out speckled.
    p->noise_tex  = resources_texture("img/cloud_noise.png", 3, GL_REPEAT, 1);
    // Cloud coverage — a cvar (menu/console), applied every frame
    cvar_register_ex("r_clouds", 0.25f, 0.0f, 1.0f, CVAR_ARCHIVE,
                     "процедурные облака: покрытие 0..1");

    glUseProgram(prog);
    glUniform1i(glGetUniformLocation(prog, "uSky"), 0);
    glUniform1i(p->u_noise, 1);
    glUniform3f(glGetUniformLocation(prog, "uFogColor"),
                g_fog_rgb[0], g_fog_rgb[1], g_fog_rgb[2]);
    glUniform3f(glGetUniformLocation(prog, "uTint"),
                sc.tint[0], sc.tint[1], sc.tint[2]);
    glUseProgram(0);
    s_sky = p;                     // for hot-reload
    shader_on_reshade(sky_reshade);

    const float size[3] = { 0, 0, 0 };
    SceneNode *node = node_new("Sky", "Skybox3D", NULL, size, p, NULL,
                               sky_node_render);
    if (!node) {
        s_sky = NULL;
        if (p->noise_tex) resources_texture_release(p->noise_tex);
        resources_texture_release(p->tex);
        resources_program_release(p->prog);
        glDeleteBuffers(1, &p->vbo);
        glDeleteVertexArrays(1, &p->vao);
        free(p);
    }
    return node;
}

// Hot-reload: recreate the sky program and uniforms.
// replace balances the refcount (release the old + new reference); if the
// pair fails to relink — 0, keep drawing the previous program.
static void sky_reshade(void)
{
    SkyParams *p = s_sky;
    if (!p) return;
    GLuint prog = resources_program_replace(p->prog,
                                            "shaders/sky.vs", "shaders/sky.fs");
    if (prog) p->prog = prog;
    p->u_forward = glGetUniformLocation(p->prog, "uForward");
    p->u_right   = glGetUniformLocation(p->prog, "uRight");
    p->u_up      = glGetUniformLocation(p->prog, "uUp");
    p->u_tanfov  = glGetUniformLocation(p->prog, "uTanFov");
    p->u_aspect  = glGetUniformLocation(p->prog, "uAspect");
    p->u_sundir  = glGetUniformLocation(p->prog, "uSunDir");
    p->u_suncolor = glGetUniformLocation(p->prog, "uSunColor");
    p->u_sunglow  = glGetUniformLocation(p->prog, "uSunGlow");
    p->u_noise    = glGetUniformLocation(p->prog, "uNoise");
    p->u_camxz    = glGetUniformLocation(p->prog, "uCamXZ");
    p->u_time     = glGetUniformLocation(p->prog, "uTime");
    p->u_clouds   = glGetUniformLocation(p->prog, "uClouds");
    glUseProgram(p->prog);
    glUniform1i(glGetUniformLocation(p->prog, "uSky"), 0);
    glUniform1i(p->u_noise, 1);
    glUniform3f(glGetUniformLocation(p->prog, "uFogColor"),
                g_fog_rgb[0], g_fog_rgb[1], g_fog_rgb[2]);
    MapSky sc;
    mapdata_sky(&sc);
    glUniform3f(glGetUniformLocation(p->prog, "uTint"),
                sc.tint[0], sc.tint[1], sc.tint[2]);
    glUseProgram(0);
}

void sky_node_render(SceneNode *n, float dt)
{
    (void)dt;
    SkyParams *p = n->params;
    const RendererContext *rc = p->rc;
    if (rc->pass == RP_SHADOW) return; // the sky does not write into shadow maps

    glDisable(GL_DEPTH_TEST);          // the sky is drawn first, without depth
    glDepthMask(GL_FALSE);             // and does not write to the depth buffer
    glUseProgram(p->prog);
    // Basis of the VIEW camera (the real render): in 1st person it matches the
    // look, in 3rd person the camera stands behind the character. Otherwise the
    // sky drifted against the scene in 3rd person.
    glUniform3f(p->u_forward, rc->view_fwd[0], rc->view_fwd[1], rc->view_fwd[2]);
    glUniform3f(p->u_right, rc->view_right[0], rc->view_right[1], rc->view_right[2]);
    glUniform3f(p->u_up, rc->view_up[0], rc->view_up[1], rc->view_up[2]);
    glUniform1f(p->u_tanfov, tanf(rc->cam_fov_deg * 3.14159f / 360.0f));
    glUniform1f(p->u_aspect, rc->cam_aspect);
    // The sun is the first directional light (index 0)
    glUniform3f(p->u_sundir, rc->light_dir[0][0], rc->light_dir[0][1], rc->light_dir[0][2]);
    glUniform3f(p->u_suncolor, rc->light_color[0][0], rc->light_color[0][1], rc->light_color[0][2]);
    glUniform1f(p->u_sunglow, rc->sun_glow);
    // Procedural clouds: noise on unit 1, drift over time, world-anchored
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, p->noise_tex);
    glUniform1i(p->u_noise, 1);
    glUniform2f(p->u_camxz, rc->view_pos[0], rc->view_pos[2]);
    glUniform1f(p->u_time, (float)(SDL_GetTicks() % 600000) / 1000.0f);
    glUniform1f(p->u_clouds, cvar_get("r_clouds"));
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, p->tex);
    glBindVertexArray(p->vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

void sky_fog_color(float out[3])
{
    out[0] = g_fog_rgb[0];
    out[1] = g_fog_rgb[1];
    out[2] = g_fog_rgb[2];
}

GLuint sky_panorama(float tint[3])
{
    if (!s_sky || !s_sky->tex) return 0;
    MapSky sc;
    mapdata_sky(&sc);
    tint[0] = sc.tint[0];
    tint[1] = sc.tint[1];
    tint[2] = sc.tint[2];
    return s_sky->tex;
}

void sky_cleanup(SceneNode *n)
{
    if (!n) return;   // safe with partially built scenes (error path)
    SkyParams *p = n->params;
    resources_program_release(p->prog);
    resources_texture_release(p->tex);
    if (p->noise_tex) resources_texture_release(p->noise_tex);
    glDeleteBuffers(1, &p->vbo);
    glDeleteVertexArrays(1, &p->vao);
}
