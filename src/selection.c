// ============================================================================
//  selection.c — selection wireframes around 3D scene nodes (see selection.h)
// ----------------------------------------------------------------------------
//  Draws through the RHI (gpu.h) — never touches GL directly.
// ============================================================================
#include "selection.h"

#include "gpu.h"

#include <stdlib.h>
#include <string.h>

#define SEL_MAX_NODES 32   // cap on boxes (~5 in the scene)

static int s_on = 1;       // on/off (key G, together with the HUD)

static const GpuDevice *s_gpu = NULL;

SceneNode *selection_make_node(SceneNode *root, const RendererContext *rc)
{
    if (!s_gpu) s_gpu = gpu_create();
    // Same shader as the field wireframe: world position + color. No new
    // files needed — that is enough for lines.
    GpuProg prog = gpu_program_new(s_gpu, "shaders/field.vs", "shaders/field.fs");
    if (!prog) return NULL;

    GpuVao vao = gpu_vao_gen(s_gpu);
    gpu_vao_bind(s_gpu, vao);
    GpuBuf vbo = gpu_buf_gen(s_gpu);
    gpu_buf_bind_array(s_gpu, vbo);
    gpu_buf_upload_array(s_gpu, NULL,
                         (unsigned)(SEL_MAX_NODES * 24 * 3 * sizeof(float)), 1);
    gpu_vertex_attr_3f(s_gpu, 0, 3 * (int)sizeof(float), NULL);
    gpu_vao_bind(s_gpu, 0);

    SelParams *p = calloc(1, sizeof *p);
    if (!p) {
        gpu_program_delete(s_gpu, &prog);
        gpu_buf_delete(s_gpu, &vbo);
        gpu_vao_delete(s_gpu, &vao);
        return NULL;
    }
    p->prog = prog;
    p->vao = vao;
    p->vbo = vbo;
    p->rc = rc;
    p->root = root;
    p->u_viewproj = gpu_program_loc(s_gpu, prog, "uViewProj");
    p->u_color    = gpu_program_loc(s_gpu, prog, "uColor");
    SceneNode *node = node_new("Selection", "Node3D", NULL, NULL, p,
                               NULL, selection_node_render);
    if (!node) {
        gpu_program_delete(s_gpu, &p->prog);
        gpu_buf_delete(s_gpu, &p->vbo);
        gpu_vao_delete(s_gpu, &p->vao);
        free(p);
    }
    return node;
}

void selection_set_on(int on)
{
    s_on = on;
}

int selection_is_on(void)
{
    return s_on;
}

// 12 box edges: vertex-index pairs from the 8 corners (bottom 0-3, top 4-7)
static const int s_edges[24] = {
    0,1, 1,2, 2,3, 3,0,     // bottom face
    4,5, 5,6, 6,7, 7,4,     // top face
    0,4, 1,5, 2,6, 3,7,     // vertical edges
};

void selection_node_render(SceneNode *n, float dt)
{
    (void)dt;
    if (!s_on) return;
    SelParams *p = n->params;
    if (p->rc->pass == RP_SHADOW) return;   // wireframes must not write into shadow maps

    // Collect the box corners of every 3D node (skip 2D and zero-size ones)
    float v[SEL_MAX_NODES * 24 * 3];
    int nv = 0;
    for (SceneNode *c = p->root->first_child; c; c = c->next) {
        if (strcmp(c->type, "CanvasItem2D") == 0) continue;
        if (c->size[0] == 0.0f && c->size[1] == 0.0f && c->size[2] == 0.0f)
            continue;
        if (nv + 24 * 3 > SEL_MAX_NODES * 24 * 3) break;
        // 8 corners: pos ± size/2 (per axis)
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

    gpu_cap(s_gpu, GPU_CAP_DEPTH_TEST, 1);   // edges get occluded by the terrain
    gpu_use_program(s_gpu, p->prog);
    gpu_uniform_mat4(s_gpu, p->u_viewproj, p->rc->viewproj);
    gpu_uniform_float3(s_gpu, p->u_color, 1.0f, 0.6f, 0.15f);   // orange, like in Godot
    gpu_vao_bind(s_gpu, p->vao);
    gpu_buf_bind_array(s_gpu, p->vbo);
    gpu_buf_upload_array(s_gpu, v, (unsigned)(nv * sizeof(float)), 1);
    gpu_draw_arrays(s_gpu, GPU_PRIM_LINES, nv / 3);
}

void selection_cleanup(SceneNode *n)
{
    if (!n) return;   // scene_build error path: the node may never have been created
    SelParams *p = n->params;
    gpu_program_delete(s_gpu, &p->prog);
    gpu_buf_delete(s_gpu, &p->vbo);
    gpu_vao_delete(s_gpu, &p->vao);
}