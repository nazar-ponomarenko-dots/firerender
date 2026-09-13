// ============================================================================
//  sky.c — небосвод из панорамы: узел Skybox3D + цвет тумана горизонта
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
    GLuint prog, vao, vbo;         // программа и треугольник на весь экран
    GLuint tex;                    // панорама неба (img/skybox.png)
    GLuint noise_tex;              // img/noise.png (процедурные облака)
    GLint u_forward, u_right, u_up, u_tanfov, u_aspect;
    GLint u_sundir, u_suncolor, u_sunglow;
    GLint u_noise, u_camxz, u_time, u_clouds;
    const RendererContext *rc;     // базис камеры и солнце (renderer.c)
} SkyParams;
static SkyParams *s_sky = NULL;   // для hot-reload (shader_on_reshade)
static void sky_reshade(void);

static float g_fog_rgb[3] = { 0.6f, 0.65f, 0.7f };   // до загрузки неба

SceneNode *sky_make_node(const RendererContext *rc)
{
    // Панорама и тон — из map/map.json (sky = { tex, tint })
    MapSky sc;
    mapdata_sky(&sc);
    GLuint prog = make_program("shaders/sky.vs", "shaders/sky.fs");
    int sw = 0, sh = 0;
    unsigned char *img = load_img(sc.tex, &sw, &sh, STBI_rgb);
    if (!prog || !img) {
        if (prog) resources_program_release(prog);
        return NULL;
    }

    // Небо: REPEAT по X убирает шов панорамы на стыке 360 градусов.
    // LINEAR (последний аргумент 1) — без полос/пикселизации на куполе
    // (панорама 512x128 даёт ~1.4px/градус — NEAREST выглядел грязно).
    // Текстура — через кеш ресурсов (пиксели уже загружены для цвета тумана).
    GLuint tex = resources_texture(sc.tex, 3, GL_REPEAT, 1);

    // Цвет тумана берём из середины панорамы неба — горизонт сходится с небом
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
    // Пиксели во владении ресурсного кеша — не освобождаем (см. resources.h)

    // Полноэкранный треугольник
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
    // Шум для процедурных облаков (общий кеш ресурсов, REPEAT):
    // img/cloud_noise.png — гладкий тайлящийся FBM (tools/make_cloud_noise.py),
    // img/noise.png — белое зерно, облака из него получились бы в спеклу.
    p->noise_tex  = resources_texture("img/cloud_noise.png", 3, GL_REPEAT, 1);
    // Покрытие облаков — cvar (меню/консоль), применяется каждый кадр
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
    s_sky = p;                     // для hot-reload
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

// Hot-reload: пересоздать программу и uniform'ы неба.
// replace балансирует refcount (release старого + новая ссылка); если пара
// не пересобралась — 0, продолжаем рисовать прежнюю программу.
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
    if (rc->pass == RP_SHADOW) return; // в карты теней небо не пишет

    glDisable(GL_DEPTH_TEST);          // небо рисуется первым, без глубины
    glDepthMask(GL_FALSE);             // и не пишет в буфер глубины
    glUseProgram(p->prog);
    // Базис ВИДОВОЙ камеры (реальный рендер): в 1-м лице совпадает со
    // взглядом, в 3-м лице — камера стоит сзади персонажа. Иначе небо
    // разъезжалось со сценой в 3-м лице.
    glUniform3f(p->u_forward, rc->view_fwd[0], rc->view_fwd[1], rc->view_fwd[2]);
    glUniform3f(p->u_right, rc->view_right[0], rc->view_right[1], rc->view_right[2]);
    glUniform3f(p->u_up, rc->view_up[0], rc->view_up[1], rc->view_up[2]);
    glUniform1f(p->u_tanfov, tanf(rc->cam_fov_deg * 3.14159f / 360.0f));
    glUniform1f(p->u_aspect, rc->cam_aspect);
    // Солнце — первый направленный источник (индекс 0)
    glUniform3f(p->u_sundir, rc->light_dir[0][0], rc->light_dir[0][1], rc->light_dir[0][2]);
    glUniform3f(p->u_suncolor, rc->light_color[0][0], rc->light_color[0][1], rc->light_color[0][2]);
    glUniform1f(p->u_sunglow, rc->sun_glow);
    // Процедурные облака: шум на unit 1, дрейф по времени, привязка к миру
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
    if (!n) return;   // безопасно при частичном построении сцены (error-path)
    SkyParams *p = n->params;
    resources_program_release(p->prog);
    resources_texture_release(p->tex);
    if (p->noise_tex) resources_texture_release(p->noise_tex);
    glDeleteBuffers(1, &p->vbo);
    glDeleteVertexArrays(1, &p->vao);
}
