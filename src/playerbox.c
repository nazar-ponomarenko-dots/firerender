// ============================================================================
//  playerbox.c — character collision box in third-person mode (see playerbox.h)
// ----------------------------------------------------------------------------
//  The third-person character is drawn as the FULL box of its physical collision
//  (1 m wide, 2 m tall — see camera.h: PLAYER_HALF_W / PLAYER_HEIGHT).
//  The box is built every frame around the character's feet (feet = eye position
//  minus the current eye height, since the camera sits at eye level). Semi-transparent
//  face fill + visible edges — the collision is fully visible.
//  In first-person and in the shadow pass the node draws nothing.
// ============================================================================
#include "playerbox.h"

#include "cvars.h"
#include "gl_loader.h"
#include "resources.h"
#include "shader.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

// Vertex: world position + uv (uv is unused for lines, but the attribute is shared)
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
    if (!p) {
        resources_program_release(prog);
        return NULL;
    }
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

    SceneNode *node = node_new("PlayerBox", "PlayerBox3D", NULL, NULL, p,
                               NULL, playerbox_node_render);
    if (!node) {
        glDeleteBuffers(1, &p->vbo_fill);
        glDeleteBuffers(1, &p->vbo_line);
        glDeleteVertexArrays(1, &p->vao_fill);
        glDeleteVertexArrays(1, &p->vao_line);
        resources_program_release(p->prog);
        free(p);
    }
    return node;
}

// 8 box corners: bottom 0-3, top 4-7 (see selection.c)
// 12 edges — pairs of corner indices.
static const int s_edges[24] = {
    0,1, 1,2, 2,3, 3,0,     // bottom face
    4,5, 5,6, 6,7, 7,4,     // top face
    0,4, 1,5, 2,6, 3,7,     // vertical edges
};

void playerbox_node_render(SceneNode *n, float dt)
{
    (void)dt;
    PlayerBoxParams *p = n->params;
    if (!p || !p->cam) return;
    const RendererContext *rc = p->rc;
    if (rc->pass == RP_SHADOW) return;            // don't write into shadow maps
    // Third-person mode only: in first person the character IS the camera,
    // drawing its box onto itself is pointless.
    if (cvar_get("cam_thirdperson") < 0.5f) return;

    float half = PLAYER_HALF_W;        // 0.5 m -> 1x1 m box cross-section
    float height = PLAYER_HEIGHT;      // 2 m — collision height

    // Character's feet: the camera is at eye level (EYE_H), eye height depends
    // on crouching (cam_duck from the context).
    float eye = rc->cam_duck ? EYE_DUCK : EYE_H;
    float feet = p->cam->cam.pos[1] - eye;
    float cx = p->cam->cam.pos[0], cz = p->cam->cam.pos[2];

    // 8 box corners (Y axis — up)
    float c[8][3] = {
        {cx - half, feet,         cz - half},   // 0 bottom
        {cx + half, feet,         cz - half},   // 1
        {cx + half, feet,         cz + half},   // 2
        {cx - half, feet,         cz + half},   // 3
        {cx - half, feet + height, cz - half},  // 4 top
        {cx + half, feet + height, cz - half},  // 5
        {cx + half, feet + height, cz + half},  // 6
        {cx - half, feet + height, cz + half},  // 7
    };

    // --- Face fill: 12 triangles (6 faces x 2) --------------------
    // Face corner order: 4 indices -> 2 triangles (i0,i1,i2),(i0,i2,i3).
    static const int face[6][4] = {
        {0,1,2,3},   // bottom
        {4,5,6,7},   // top
        {0,1,5,4},   // front/back/left/right
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
    // --- Edges: 12 segments = 24 vertices --------------------------------------
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

    // Semi-transparent fill (depth test on — the box hides behind terrain, but does it
    // write depth? No: we write only color, depth is NOT touched — otherwise the
    // character would occlude itself and the world. Blend on, DepthMask off.)
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    glUniform4f(p->u_color, 0.90f, 0.30f, 0.25f, 0.35f);   // red box
    glBindVertexArray(p->vao_fill);
    glBindBuffer(GL_ARRAY_BUFFER, p->vbo_fill);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(nv * sizeof(float)), tri,
                 GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, nv / PB_STRIDE);

    // Edges — bright, on top of the fill
    glUniform4f(p->u_color, 1.0f, 0.85f, 0.30f, 0.95f);   // yellow lines
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
