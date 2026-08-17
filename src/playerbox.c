// ============================================================================
//  playerbox.c — бокс коллизии персонажа в режиме от 3-го лица (см. playerbox.h)
// ----------------------------------------------------------------------------
//  Персонаж в 3-м лице рисуется как ПОЛНЫЙ бокс его физической коллизии
//  (ширина 1 м, высота 2 м — см. camera.h: PLAYER_HALF_W / PLAYER_HEIGHT).
//  Бокс строится каждый кадр вокруг ног персонажа (ноги = позиция глаз минус
//  текущая высота глаз, т.к. камера стоит на уровне глаз). Полупрозрачная
//  закраска граней + видимые рёбра — коллизия видна целиком.
//  В 1-м лице и в теневом проходе узел ничего не рисует.
// ============================================================================
#include "playerbox.h"

#include "cvars.h"
#include "gl_loader.h"
#include "resources.h"
#include "shader.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

// Вершина: позиция в мире + uv (для линий uv не используется, но атрибут общий)
#define PB_STRIDE 5

static void pb_setup_vao(GLuint vao, GLuint vbo)
{
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, PB_STRIDE * sizeof(float), NULL);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, PB_STRIDE * sizeof(float),
                          (const void *)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);
}

SceneNode *playerbox_make_node(CameraParams *cam, const RendererContext *rc)
{
    if (!cam || !rc) return NULL;
    GLuint prog = make_program("shaders/playerbox.vs", "shaders/playerbox.fs");
    if (!prog) return NULL;

    PlayerBoxParams *p = calloc(1, sizeof *p);
    p->rc = rc;
    p->cam = cam;
    p->prog = prog;
    p->u_color = glGetUniformLocation(prog, "uColor");
    p->u_viewproj = glGetUniformLocation(prog, "uViewProj");

    glGenVertexArrays(1, &p->vao_fill);
    glGenBuffers(1, &p->vbo_fill);
    glGenVertexArrays(1, &p->vao_line);
    glGenBuffers(1, &p->vbo_line);
    pb_setup_vao(p->vao_fill, p->vbo_fill);
    pb_setup_vao(p->vao_line, p->vbo_line);

    return node_new("PlayerBox", "PlayerBox3D", NULL, NULL, p,
                    NULL, playerbox_node_render);
}

// 8 углов бокса: низ 0-3, верх 4-7 (см. selection.c)
// 12 рёбер — пары индексов углов.
static const int s_edges[24] = {
    0,1, 1,2, 2,3, 3,0,     // нижняя грань
    4,5, 5,6, 6,7, 7,4,     // верхняя грань
    0,4, 1,5, 2,6, 3,7,     // вертикальные рёбра
};

void playerbox_node_render(SceneNode *n, float dt)
{
    (void)dt;
    PlayerBoxParams *p = n->params;
    if (!p || !p->cam) return;
    const RendererContext *rc = p->rc;
    if (rc->pass == RP_SHADOW) return;            // в карты теней не пишем
    // Только в режиме от 3-го лица: в 1-м лице персонаж = сама камера,
    // рисовать его бокс на себя бессмысленно.
    if (cvar_get("cam_thirdperson") < 0.5f) return;

    float half = PLAYER_HALF_W;        // 0.5 м -> бокс 1x1 м в сечении
    float height = PLAYER_HEIGHT;      // 2 м — высота коллизии

    // Ноги персонажа: камера на уровне глаз (EYE_H), высота глаз зависит
    // от приседа (cam_duck из контекста).
    float eye = rc->cam_duck ? EYE_DUCK : EYE_H;
    float feet = p->cam->cam.pos[1] - eye;
    float cx = p->cam->cam.pos[0], cz = p->cam->cam.pos[2];

    // 8 углов бокса (ось Y — вверх)
    float c[8][3] = {
        {cx - half, feet,         cz - half},   // 0 низ
        {cx + half, feet,         cz - half},   // 1
        {cx + half, feet,         cz + half},   // 2
        {cx - half, feet,         cz + half},   // 3
        {cx - half, feet + height, cz - half},  // 4 верх
        {cx + half, feet + height, cz - half},  // 5
        {cx + half, feet + height, cz + half},  // 6
        {cx - half, feet + height, cz + half},  // 7
    };

    // --- Закраска граней: 12 треугольников (6 граней по 2) --------------------
    // Порядок углов грани: 4 индекса -> 2 треугольника (i0,i1,i2),(i0,i2,i3).
    static const int face[6][4] = {
        {0,1,2,3},   // низ
        {4,5,6,7},   // верх
        {0,1,5,4},   // перед/зад/лево/право
        {2,3,7,6},
        {1,2,6,5},
        {3,0,4,7},
    };
    float tri[6 * 6 * PB_STRIDE];
    int nv = 0;
    for (int f = 0; f < 6; f++) {
        int q0 = face[f][0], q1 = face[f][1], q2 = face[f][2], q3 = face[f][3];
        const int idx[6] = { q0, q1, q2, q0, q2, q3 };
        for (int k = 0; k < 6; k++) {
            tri[nv++] = c[idx[k]][0];
            tri[nv++] = c[idx[k]][1];
            tri[nv++] = c[idx[k]][2];
            tri[nv++] = 0.0f;
            tri[nv++] = 0.0f;
        }
    }
    // --- Рёбра: 12 отрезков = 24 вершины --------------------------------------
    float lin[24 * PB_STRIDE];
    int nl = 0;
    for (int e = 0; e < 24; e++) {
        const float *a = c[s_edges[e]];
        lin[nl++] = a[0]; lin[nl++] = a[1]; lin[nl++] = a[2];
        lin[nl++] = 0.0f; lin[nl++] = 0.0f;
    }

    glDisable(GL_CULL_FACE);
    glUseProgram(p->prog);
    glUniformMatrix4fv(p->u_viewproj, 1, GL_FALSE, rc->viewproj);

    // Полупрозрачная закраска (depth test вкл — бокс за рельефом, но пишет
    // глубину? Нет: пишем только цвет, глубину НЕ трогаем — иначе персонаж
    // закрывал бы сам себя и мир. Blend вкл, DepthMask off.)
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    glUniform4f(p->u_color, 0.90f, 0.30f, 0.25f, 0.35f);   // красный бокс
    glBindVertexArray(p->vao_fill);
    glBindBuffer(GL_ARRAY_BUFFER, p->vbo_fill);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(nv * sizeof(float)), tri,
                 GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, nv / PB_STRIDE);

    // Рёбра — яркие, поверх закраски
    glUniform4f(p->u_color, 1.0f, 0.85f, 0.30f, 0.95f);   // жёлтые линии
    glBindVertexArray(p->vao_line);
    glBindBuffer(GL_ARRAY_BUFFER, p->vbo_line);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(nl * sizeof(float)), lin,
                 GL_STREAM_DRAW);
    glDrawArrays(GL_LINES, 0, nl / PB_STRIDE);

    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glEnable(GL_CULL_FACE);
    glUseProgram(0);
}

void playerbox_cleanup(SceneNode *n)
{
    if (!n) return;
    PlayerBoxParams *p = n->params;
    if (!p) return;
    resources_program_release(p->prog);
    glDeleteBuffers(1, &p->vbo_fill);
    glDeleteBuffers(1, &p->vbo_line);
    glDeleteVertexArrays(1, &p->vao_fill);
    glDeleteVertexArrays(1, &p->vao_line);
}
