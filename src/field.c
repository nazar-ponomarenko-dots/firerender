// ============================================================================
//  field.c — рамка поля пшеницы: замкнутая линия на поверхности рельефа
// ============================================================================
#include "field.h"

#include "gl_loader.h"
#include "resources.h"
#include "shader.h"
#include "world.h"

#include <stdlib.h>

SceneNode *field_make_node(const WorldState *w, const float *center,
                           const float *size, const RendererContext *rc)
{
    GLuint prog = make_program("shaders/field.vs", "shaders/field.fs");
    if (!prog) return NULL;

    GLuint vao, vbo;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, 5 * 3 * sizeof(float), NULL, GL_STREAM_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), NULL);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);

    FieldParams *p = calloc(1, sizeof *p);
    p->prog = prog;
    p->vao = vao;
    p->vbo = vbo;
    p->world = w;
    p->rc = rc;
    p->u_viewproj = glGetUniformLocation(prog, "uViewProj");
    p->u_color    = glGetUniformLocation(prog, "uColor");
    return node_new("FieldArea", "FieldArea3D", center, size, p,
                    NULL, field_node_render);
}

void field_node_render(SceneNode *n, float dt)
{
    (void)dt;
    FieldParams *p = n->params;
    float x0 = n->pos[0] - n->size[0] * 0.5f, z0 = n->pos[2] - n->size[2] * 0.5f;
    float x1 = n->pos[0] + n->size[0] * 0.5f, z1 = n->pos[2] + n->size[2] * 0.5f;
    float v[5][3] = {
        {x0, 0, z0}, {x1, 0, z0}, {x1, 0, z1}, {x0, 0, z1}, {x0, 0, z0},
    };
    for (int i = 0; i < 5; i++)
        v[i][1] = sample_height(p->world, v[i][0], v[i][2]) + 0.5f;
    glEnable(GL_DEPTH_TEST);
    glUseProgram(p->prog);
    glUniformMatrix4fv(p->u_viewproj, 1, GL_FALSE, p->rc->viewproj);
    glUniform3f(p->u_color, 0.95f, 0.75f, 0.15f);   // жёлтая рамка поля
    glBindVertexArray(p->vao);
    glBindBuffer(GL_ARRAY_BUFFER, p->vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof v, v, GL_STREAM_DRAW);
    glDrawArrays(GL_LINE_STRIP, 0, 5);
}

void field_cleanup(SceneNode *n)
{
    FieldParams *p = n->params;
    resources_program_release(p->prog);
    glDeleteBuffers(1, &p->vbo);
    glDeleteVertexArrays(1, &p->vao);
}
