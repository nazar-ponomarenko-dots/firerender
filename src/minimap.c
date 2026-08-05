// ============================================================================
//  minimap.c — мини-карта: карта + точка игрока + направление взгляда
// ============================================================================
#include "minimap.h"

#include "assets.h"
#include "gl_loader.h"
#include "linalg.h"
#include "resources.h"
#include "shader.h"
#include "world.h"
#include "shadow.h"

#include <math.h>
#include <stdlib.h>

#include <SDL3/SDL.h>

SceneNode *minimap_make_node(const WorldState *w, GLuint tex_base,
                             const RendererContext *rc)
{
    GLuint prog = make_program("shaders/minimap.vs", "shaders/minimap.fs");
    if (!prog) return NULL;

    GLuint vao, vbo;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, 16 * 4 * sizeof(float), NULL, GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), NULL);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          (const void *)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);

    MinimapParams *p = calloc(1, sizeof *p);
    p->prog = prog;
    p->vao = vao;
    p->vbo = vbo;
    p->tex_base = tex_base;                 // цвет карты (для мини-карты)
    p->world = w;
    p->rc = rc;
    p->u_basemap = glGetUniformLocation(prog, "uBasemap");
    p->u_color   = glGetUniformLocation(prog, "uColor");
    p->u_usetex  = glGetUniformLocation(prog, "uUseTex");
    p->u_usemarker = glGetUniformLocation(prog, "uUseMarker");
    p->visible = 1;
    // Маркер игрока (img/player.png): стрелка, крутится на yaw каждый кадр.
    // Текстура — через кеш ресурсов.
    p->tex_marker = resources_texture("img/player.png", 4,
                                      GL_CLAMP_TO_EDGE, 1);
    glUseProgram(prog);
    glUniform1i(p->u_basemap, 0);        // карта — на текстурном юните 0
    glUniform1i(glGetUniformLocation(prog, "uMarker"), 1);  // маркер — юнит 1
    glUseProgram(0);
    const float pos[3] = { 16, 16, 0 };         // отступ от края окна
    const float size[3] = { 220, 220 * w->world_d / w->world_w, 0 };
    return node_new("Minimap", "CanvasItem2D", pos, size, p,
                    NULL, minimap_node_render);
}

void minimap_node_render(SceneNode *n, float dt)
{
    (void)dt;
    MinimapParams *p = n->params;
    if (!p->visible) return;        // клавиша G — карта скрыта
    const RendererContext *rc = p->rc;
    if (rc->pass == RP_SHADOW) return;      // в карты теней 2D не пишет
    int vw = p->vw, vh = p->vh;
    if (vw <= 0 || vh <= 0) return;
    glDisable(GL_DEPTH_TEST);   // 2D поверх 3D-сцены

    // Экранный квад в верхнем левом углу. Координаты в NDC считаем прямо
    // из размеров окна — при ресайзе мини-карта масштабируется сама.
    float mm_w = n->size[0];
    float mm_h = n->size[1];
    float mx0 = 16.0f, my0 = vh - 16.0f - mm_h;
    float nx0 = -1.0f + 2.0f * mx0 / vw;
    float nx1 = -1.0f + 2.0f * (mx0 + mm_w) / vw;
    float ny1 =  1.0f - 2.0f * my0 / vh;              // верх (v=0)
    float ny0 =  1.0f - 2.0f * (my0 + mm_h) / vh;     // низ  (v=1)

    // Позиция игрока в uv карты (x/WORLD_W, z/WORLD_D), прижата к краям
    const WorldState *w = p->world;
    float u = rc->cam_pos[0] / w->world_w;
    float v = rc->cam_pos[2] / w->world_d;
    if (u < 0) u = 0;
    if (u > 1) u = 1;
    if (v < 0) v = 0;
    if (v > 1) v = 1;
    float px = nx0 + u * (nx1 - nx0);
    float py = ny1 - v * (ny1 - ny0);   // z растёт вниз по карте

    // Штрих направления взгляда (12 м вперёд) + жёлтая линия направления
    // на солнце (от игрока, 60 м).
    float lx = sinf(rc->cam_yaw) * 12.0f / w->world_w * (nx1 - nx0);
    float ly = -cosf(rc->cam_yaw) * 12.0f / w->world_d * (ny1 - ny0);
    // Направление на солнце (первый направленный источник)
    float sx = sinf(atan2f(rc->light_dir[0][0], rc->light_dir[0][2])) * 60.0f / w->world_w * (nx1 - nx0);
    float sy = -cosf(atan2f(rc->light_dir[0][0], rc->light_dir[0][2])) * 60.0f / w->world_d * (ny1 - ny0);

    // Маркер игрока: img/player.png, повёрнут на yaw — стрелка смотрит
    // туда, куда смотрит игрок. forward = (sin yaw, 0, cos yaw); на карте
    // +x вправо, +z вниз -> направление взгляда на экране (sin yaw, -cos yaw),
    // угол поворота текстуры θ = yaw + π (текстура смотрит "на север").
    float rh = 11.0f;                       // полмаркера, px
    float ux = sinf(rc->cam_yaw) * 2.0f * rh / vw;
    float uy = -cosf(rc->cam_yaw) * 2.0f * rh / vh;
    float px2 = cosf(rc->cam_yaw) * 2.0f * rh / vw;
    float py2 = sinf(rc->cam_yaw) * 2.0f * rh / vh;
    // Вершины: 0-5 карта, 6-11 маркер, 12-13 линия взгляда, 14-15 солнце
    float verts[16][4] = {
        {nx0, ny0, 0, 1}, {nx1, ny0, 1, 1}, {nx0, ny1, 0, 0},
        {nx0, ny1, 0, 0}, {nx1, ny0, 1, 1}, {nx1, ny1, 1, 0},
        {px + ux - px2, py + uy - py2, 0, 0},
        {px + ux + px2, py + uy + py2, 1, 0},
        {px - ux - px2, py - uy - py2, 0, 1},
        {px + ux + px2, py + uy + py2, 1, 0},
        {px - ux + px2, py - uy + py2, 1, 1},
        {px - ux - px2, py - uy - py2, 0, 1},
        {px, py, 0, 0}, {px + lx, py + ly, 0, 0},
        {px, py, 0, 0}, {px + sx, py + sy, 0, 0},
    };
    glBindVertexArray(p->vao);
    glBindBuffer(GL_ARRAY_BUFFER, p->vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof verts, verts, GL_DYNAMIC_DRAW);
    glUseProgram(p->prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, p->tex_base);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, p->tex_marker);
    glUniform3f(p->u_color, 1.0f, 1.0f, 1.0f);
    glUniform1f(p->u_usetex, 1.0f);
    glUniform1f(p->u_usemarker, 0.0f);
    glDrawArrays(GL_TRIANGLES, 0, 6);       // сама карта
    // маркер игрока: текстура со своей альфой (блендинг — только здесь)
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUniform1f(p->u_usetex, 0.0f);
    glUniform1f(p->u_usemarker, 1.0f);
    glDrawArrays(GL_TRIANGLES, 6, 6);       // маркер
    glDisable(GL_BLEND);
    glUniform1f(p->u_usemarker, 0.0f);
    glUniform3f(p->u_color, 1.0f, 0.25f, 0.15f); // оранжевая — линия взгляда
    glDrawArrays(GL_LINES, 12, 2);          // направление взгляда
    glUniform3f(p->u_color, 1.0f, 0.85f, 0.2f); // жёлтое — солнце
    glDrawArrays(GL_LINES, 14, 2);

}

void minimap_cleanup(SceneNode *n)
{
    MinimapParams *p = n->params;
    resources_program_release(p->prog);
    resources_texture_release(p->tex_marker);   // карта — общая, её не удаляем
    glDeleteBuffers(1, &p->vbo);
    glDeleteVertexArrays(1, &p->vao);
}
