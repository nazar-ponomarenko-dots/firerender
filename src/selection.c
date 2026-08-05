// ============================================================================
//  selection.c — рамки выделения вокруг 3D-узлов сцены (см. selection.h)
// ============================================================================
#include "selection.h"

#include "gl_loader.h"
#include "resources.h"
#include "shader.h"

#include <stdlib.h>
#include <string.h>

#define SEL_MAX_NODES 32   // потолок боксов (в сцене их ~5)

static int s_on = 1;       // вкл/выкл (клавиша G, вместе с HUD)

SceneNode *selection_make_node(SceneNode *root, const RendererContext *rc)
{
    // Тот же шейдер, что у рамки поля: позиция в мире + цвет. Новые файлы
    // не нужны — для линий этого хватает.
    GLuint prog = make_program("shaders/field.vs", "shaders/field.fs");
    if (!prog) return NULL;

    GLuint vao, vbo;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 (GLsizeiptr)(SEL_MAX_NODES * 24 * 3 * sizeof(float)),
                 NULL, GL_STREAM_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), NULL);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);

    SelParams *p = calloc(1, sizeof *p);
    p->prog = prog;
    p->vao = vao;
    p->vbo = vbo;
    p->rc = rc;
    p->root = root;
    p->u_viewproj = glGetUniformLocation(prog, "uViewProj");
    p->u_color    = glGetUniformLocation(prog, "uColor");
    return node_new("Selection", "Node3D", NULL, NULL, p,
                    NULL, selection_node_render);
}

void selection_set_on(int on)
{
    s_on = on;
}

int selection_is_on(void)
{
    return s_on;
}

// 12 рёбер бокса: пары индексов вершин из 8 углов (низ 0-3, верх 4-7)
static const int s_edges[24] = {
    0,1, 1,2, 2,3, 3,0,     // нижняя грань
    4,5, 5,6, 6,7, 7,4,     // верхняя грань
    0,4, 1,5, 2,6, 3,7,     // рёбра
};

void selection_node_render(SceneNode *n, float dt)
{
    (void)dt;
    if (!s_on) return;
    SelParams *p = n->params;

    // Собираем углы боксов всех 3D-узлов (пропускаем 2D и нулевые размеры)
    float v[SEL_MAX_NODES * 24 * 3];
    int nv = 0;
    for (SceneNode *c = p->root->first_child; c; c = c->next) {
        if (strcmp(c->type, "CanvasItem2D") == 0) continue;
        if (c->size[0] == 0.0f && c->size[1] == 0.0f && c->size[2] == 0.0f)
            continue;
        if (nv + 24 * 3 > SEL_MAX_NODES * 24 * 3) break;
        // 8 углов: pos ± size/2 (по каждой оси)
        float corner[8][3];
        for (int i = 0; i < 8; i++) {
            corner[i][0] = c->pos[0] + ((i & 1) ? 1.0f : -1.0f) * c->size[0] * 0.5f;
            corner[i][1] = c->pos[1] + ((i & 2) ? 1.0f : -1.0f) * c->size[1] * 0.5f;
            corner[i][2] = c->pos[2] + ((i & 4) ? 1.0f : -1.0f) * c->size[2] * 0.5f;
        }
        for (int e = 0; e < 24; e++) {
            const float *a = corner[s_edges[e]];
            v[nv++] = a[0]; v[nv++] = a[1]; v[nv++] = a[2];
        }
    }
    if (nv == 0) return;

    glEnable(GL_DEPTH_TEST);   // рёбра закрываются рельефом, как положено
    glUseProgram(p->prog);
    glUniformMatrix4fv(p->u_viewproj, 1, GL_FALSE, p->rc->viewproj);
    glUniform3f(p->u_color, 1.0f, 0.6f, 0.15f);   // оранжевый, как в Godot
    glBindVertexArray(p->vao);
    glBindBuffer(GL_ARRAY_BUFFER, p->vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(nv * sizeof(float)), v,
                 GL_STREAM_DRAW);
    glDrawArrays(GL_LINES, 0, nv / 3);
}

void selection_cleanup(SceneNode *n)
{
    SelParams *p = n->params;
    resources_program_release(p->prog);
    glDeleteBuffers(1, &p->vbo);
    glDeleteVertexArrays(1, &p->vao);
}
