// ============================================================================
//  sky.c — небосвод из панорамы: узел Skybox3D + цвет тумана горизонта
// ============================================================================
#include "sky.h"

#include "assets.h"
#include "gl_loader.h"
#include "linalg.h"
#include "mapdata.h"
#include "resources.h"
#include "shader.h"

#include "stb_image.h"
#include <math.h>
#include <stdlib.h>

typedef struct {
    GLuint prog, vao, vbo;         // программа и треугольник на весь экран
    GLuint tex;                    // панорама неба (img/skybox.png)
    GLint u_forward, u_right, u_up, u_tanfov, u_aspect;
    GLint u_sundir, u_suncolor;
    const RendererContext *rc;     // базис камеры и солнце (renderer.c)
} SkyParams;

static float g_fog_rgb[3] = { 0.6f, 0.65f, 0.7f };   // до загрузки неба

SceneNode *sky_make_node(const RendererContext *rc)
{
    // Панорама и тон — из map/map.json (sky = { tex, tint })
    MapSky sc;
    mapdata_sky(&sc);
    GLuint prog = make_program("shaders/sky.vs", "shaders/sky.fs");
    int sw = 0, sh = 0;
    unsigned char *img = load_img(sc.tex, &sw, &sh, STBI_rgb);
    if (!prog || !img) return NULL;

    // Небо: REPEAT по X убирает шов панорамы на стыке 360 градусов.
    // Текстура — через кеш ресурсов (пиксели уже загружены для цвета тумана).
    GLuint tex = resources_texture(sc.tex, 3, GL_REPEAT, 0);

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

    glUseProgram(prog);
    glUniform1i(glGetUniformLocation(prog, "uSky"), 0);
    glUniform3f(glGetUniformLocation(prog, "uFogColor"),
                g_fog_rgb[0], g_fog_rgb[1], g_fog_rgb[2]);
    glUniform3f(glGetUniformLocation(prog, "uTint"),
                sc.tint[0], sc.tint[1], sc.tint[2]);
    glUseProgram(0);

    const float size[3] = { 0, 0, 0 };
    return node_new("Sky", "Skybox3D", NULL, size, p, NULL, sky_node_render);
}

void sky_node_render(SceneNode *n, float dt)
{
    (void)dt;
    SkyParams *p = n->params;
    const RendererContext *rc = p->rc;

    glDisable(GL_DEPTH_TEST);          // небо рисуется первым, без глубины
    glDepthMask(GL_FALSE);             // и не пишет в буфер глубины
    glUseProgram(p->prog);
    glUniform3f(p->u_forward, rc->cam_fwd[0], rc->cam_fwd[1], rc->cam_fwd[2]);
    glUniform3f(p->u_right, rc->cam_right[0], rc->cam_right[1], rc->cam_right[2]);
    glUniform3f(p->u_up, rc->cam_up[0], rc->cam_up[1], rc->cam_up[2]);
    glUniform1f(p->u_tanfov, tanf(rc->cam_fov_deg * 3.14159f / 360.0f));
    glUniform1f(p->u_aspect, rc->cam_aspect);
    // Солнце — первый направленный источник (индекс 0)
    glUniform3f(p->u_sundir, rc->light_dir[0][0], rc->light_dir[0][1], rc->light_dir[0][2]);
    glUniform3f(p->u_suncolor, rc->light_color[0][0], rc->light_color[0][1], rc->light_color[0][2]);
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

void sky_cleanup(SceneNode *n)
{
    SkyParams *p = n->params;
    resources_program_release(p->prog);
    resources_texture_release(p->tex);
    glDeleteBuffers(1, &p->vbo);
    glDeleteVertexArrays(1, &p->vao);
}
