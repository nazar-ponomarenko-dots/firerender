// ============================================================================
//  mesh.c — static meshes (prop_static): .obj loader + render node
// ============================================================================
#include "mesh.h"

#include "events.h"
#include "material.h"
#include "resources.h"
#include "shader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

// --- Mini dynarray (realloc growth) -------------------------------------
typedef struct { float *d; int n; int cap; int failed; } FArr;
static int farr_push(FArr *a, float v)
{
    if (a->failed) return 0;
    if (a->n >= a->cap) {
        int cap = a->cap ? a->cap * 2 : 64;
        float *p = realloc(a->d, (size_t)cap * sizeof(float));
        if (!p) { a->failed = 1; return 0; }
        a->d = p;
        a->cap = cap;
    }
    a->d[a->n++] = v;
    return 1;
}

// Index is 1-based; we expand per corner (face data is copied into the output).
static void push_corner(FArr *pos, FArr *nrm, FArr *uv,
                        const float *verts, int nv,
                        const float *vns, int nvn,
                        const float *vts, int nvt,
                        int vi, int ni, int ti)
{
    if (vi > 0 && vi <= nv) {
        farr_push(pos, verts[3 * (vi - 1) + 0]);
        farr_push(pos, verts[3 * (vi - 1) + 1]);
        farr_push(pos, verts[3 * (vi - 1) + 2]);
    } else { farr_push(pos, 0); farr_push(pos, 0); farr_push(pos, 0); }
    if (ni > 0 && ni <= nvn) {
        farr_push(nrm, vns[3 * (ni - 1) + 0]);
        farr_push(nrm, vns[3 * (ni - 1) + 1]);
        farr_push(nrm, vns[3 * (ni - 1) + 2]);
    } else { farr_push(nrm, 0); farr_push(nrm, 1); farr_push(nrm, 0); } // up
    if (ti > 0 && ti <= nvt) {
        farr_push(uv, vts[2 * (ti - 1) + 0]);
        farr_push(uv, vts[2 * (ti - 1) + 1]);
    } else { farr_push(uv, 0); farr_push(uv, 0); }
}

// Parses a face-corner index "v" / "v/vt" / "v//vn" / "v/vt/vn".
// All parts are 1-based, missing ones = 0.
static int parse_corner(const char *tok, int *vi, int *ti, int *ni)
{
    *vi = *ti = *ni = 0;
    char buf[96];
    if ((int)strlen(tok) >= (int)sizeof buf) return 0;
    strcpy(buf, tok);
    char *a = buf, *b = strchr(a, '/'), *c = NULL;
    if (b) { *b = 0; b++; c = strchr(b, '/'); if (c) { *c = 0; c++; } }
    if (a[0]) *vi = atoi(a);
    if (b && b[0]) *ti = atoi(b);
    if (c && c[0]) *ni = atoi(c);
    return 1;
}

Mesh *mesh_load_obj_mem(const char *name, const char *src)
{
    FArr verts = {0}, vns = {0}, vts = {0};       // sources
    FArr pos = {0}, nrm = {0}, uv = {0};          // output (per corner)

    char *buf = strdup(src);
    if (!buf) return NULL;
    char *line = buf;
    while (line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (p[0] == 'v') {
            if (p[1] == ' ') {
                float x, y, z;
                if (sscanf(p + 1, "%f %f %f", &x, &y, &z) == 3) {
                    farr_push(&verts, x); farr_push(&verts, y); farr_push(&verts, z);
                }
            } else if (p[1] == 't') {
                float u, v;
                if (sscanf(p + 2, "%f %f", &u, &v) == 2) { farr_push(&vts, u); farr_push(&vts, v); }
            } else if (p[1] == 'n') {
                float x, y, z;
                if (sscanf(p + 2, "%f %f %f", &x, &y, &z) == 3) { farr_push(&vns, x); farr_push(&vns, y); farr_push(&vns, z); }
            }
        } else if (p[0] == 'f') {
            char *q = p + 1;
            int vi[16], ti[16], ni[16];
            int ncor = 0;
            while (ncor < 16) {
                while (*q == ' ' || *q == '\t') q++;
                if (!*q) break;
                char *sp = q;
                while (*q && *q != ' ' && *q != '\t') q++;
                char save = *q; *q = 0;
                parse_corner(sp, &vi[ncor], &ti[ncor], &ni[ncor]);
                *q = save;
                ncor++;
            }
            if (ncor >= 3) {
                int nv = verts.n / 3, nvn = vns.n / 3, nvt = vts.n / 2;
                for (int t = 1; t + 1 < ncor; t++) {
                    push_corner(&pos, &nrm, &uv, verts.d, nv, vns.d, nvn, vts.d, nvt,
                                vi[0], ni[0], ti[0]);
                    push_corner(&pos, &nrm, &uv, verts.d, nv, vns.d, nvn, vts.d, nvt,
                                vi[t], ni[t], ti[t]);
                    push_corner(&pos, &nrm, &uv, verts.d, nv, vns.d, nvn, vts.d, nvt,
                                vi[t + 1], ni[t + 1], ti[t + 1]);
                }
            }
        }
        line = nl ? nl + 1 : NULL;
    }
    free(buf);
    free(verts.d); free(vns.d); free(vts.d);

    if (verts.failed || vns.failed || vts.failed ||
        pos.failed || nrm.failed || uv.failed) {
        free(pos.d); free(nrm.d); free(uv.d);
        SDL_Log("mesh: %s — out of memory", name);
        return NULL;
    }
    if (pos.n == 0) {
        free(pos.d); free(nrm.d); free(uv.d);
        SDL_Log("mesh: %s has no faces", name);
        return NULL;
    }

    Mesh *m = calloc(1, sizeof *m);
    if (!m) {
        free(pos.d); free(nrm.d); free(uv.d);
        return NULL;
    }
    m->verts = pos.d;
    m->normals = nrm.d;
    m->uvs = uv.d;
    m->vcount = pos.n / 3;
    m->tri_count = m->vcount / 3;
    SDL_Log("mesh: %s — %d verts, %d tris", name, m->vcount, m->tri_count);
    return m;
}

Mesh *mesh_load_obj(const char *path)
{
    char *src = resources_read_text(path);
    if (!src) { SDL_Log("mesh: not found %s", path); return NULL; }
    Mesh *m = mesh_load_obj_mem(path, src);
    free(src);
    return m;
}

void mesh_free(Mesh *m)
{
    if (!m) return;
    if (m->ready) {
        if (m->vbo_nrm) glDeleteBuffers(1, &m->vbo_nrm);
        if (m->vbo_uv)  glDeleteBuffers(1, &m->vbo_uv);
        glDeleteBuffers(1, &m->vbo_pos);
        glDeleteVertexArrays(1, &m->vao);
    }
    free(m->verts);
    free(m->normals);
    free(m->uvs);
    free(m);
}

void mesh_gpu_upload(Mesh *m)
{
    if (!m || m->ready) return;
    glGenVertexArrays(1, &m->vao);
    glBindVertexArray(m->vao);
    glGenBuffers(1, &m->vbo_pos);
    glBindBuffer(GL_ARRAY_BUFFER, m->vbo_pos);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(m->vcount * 3 * sizeof(float)),
                 m->verts, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, NULL);
    glEnableVertexAttribArray(0);
    if (m->normals) {
        glGenBuffers(1, &m->vbo_nrm);
        glBindBuffer(GL_ARRAY_BUFFER, m->vbo_nrm);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(m->vcount * 3 * sizeof(float)),
                     m->normals, GL_STATIC_DRAW);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 0, NULL);
        glEnableVertexAttribArray(1);
    }
    if (m->uvs) {
        glGenBuffers(1, &m->vbo_uv);
        glBindBuffer(GL_ARRAY_BUFFER, m->vbo_uv);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(m->vcount * 2 * sizeof(float)),
                     m->uvs, GL_STATIC_DRAW);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 0, NULL);
        glEnableVertexAttribArray(2);
    }
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    m->ready = 1;
}

// Shadow program for .obj meshes: one per module (mesh_shadow.vs/fs),
// hot-reload via the reshade hook.
static GLuint s_mesh_shadow_prog = 0;
static GLint s_mesh_sh_model = -1, s_mesh_sh_vp = -1;
static void mesh_shadow_reshade(void)
{
    GLuint p = resources_program_replace(s_mesh_shadow_prog,
                                         "shaders/mesh_shadow.vs",
                                         "shaders/mesh_shadow.fs");
    if (p) s_mesh_shadow_prog = p;
    s_mesh_sh_model = glGetUniformLocation(s_mesh_shadow_prog, "uModel");
    s_mesh_sh_vp    = glGetUniformLocation(s_mesh_shadow_prog, "uViewProj");
}

void mesh_node_render(SceneNode *n, float dt)
{
    (void)dt;
    MeshLocs *p = n->params;
    if (!p->mesh) return;
    if (p->rc->pass == RP_SHADOW) {
        // .obj mesh shadow: position (VAO attribute 0) x model x sun VP.
        // No alpha-test needed: obj materials are opaque (uAlphaTest = 0).
        if (!s_mesh_shadow_prog) {
            s_mesh_shadow_prog = resources_program_replace(
                0, "shaders/mesh_shadow.vs", "shaders/mesh_shadow.fs");
            mesh_shadow_reshade();
            shader_on_reshade(mesh_shadow_reshade);
        }
        glEnable(GL_DEPTH_TEST);
        glUseProgram(s_mesh_shadow_prog);
        glUniformMatrix4fv(s_mesh_sh_model, 1, GL_FALSE, n->world);
        glUniformMatrix4fv(s_mesh_sh_vp, 1, GL_FALSE, p->rc->shadow_vp);
        glBindVertexArray(p->mesh->vao);
        glDrawArrays(GL_TRIANGLES, 0, p->mesh->vcount);
        glBindVertexArray(0);
        return;
    }
    mesh_gpu_upload(p->mesh);

    // Model matrix = the node's world matrix (position + rotation + scale +
    // parent/bone chain). See scene.c: node_update_transforms.
    const float *model = n->world;

    glEnable(GL_DEPTH_TEST);
    glUseProgram(p->prog);
    scene_uniforms_bind(&p->su, p->rc);
    // Baked light: probe at the node position (extracted from the world matrix).
    scene_uniforms_probe(&p->su, model[12], model[13], model[14]);
    glUniformMatrix4fv(p->u_model, 1, GL_FALSE, model);
    material_bind(&p->mat, &p->ml, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, p->tex);
    if (p->u_tex >= 0) glUniform1i(p->u_tex, 0);

    glBindVertexArray(p->mesh->vao);
    glDrawArrays(GL_TRIANGLES, 0, p->mesh->vcount);
    glBindVertexArray(0);
}

// Release of MeshInstance3D node resources (EV_NODE_EXIT from node_free_tree).
// node_free_tree frees only the MeshLocs struct; the mesh (CPU + GPU), the
// refcounted texture and the refcounted program are returned here.
static void mesh_node_exit(const Event *e, void *user)
{
    (void)user;
    if (!e || !e->a) return;
    const SceneNode *n = e->a;
    if (!n->type || strcmp(n->type, "MeshInstance3D") != 0) return;
    MeshLocs *p = n->params;
    if (!p) return;
    if (p->tex) resources_texture_release(p->tex);
    if (p->prog) resources_program_release(p->prog);
    mesh_free(p->mesh);
}

SceneNode *mesh_make_node(const char *obj_path, const float pos[3],
                          float scale, const char *tex_path,
                          const RendererContext *rc)
{
    // Once: when a MeshInstance3D node exits the tree (node_free_tree sends
    // EV_NODE_EXIT BEFORE free(params)) the mesh, texture and program refs are
    // released. node_free_tree frees ONLY the MeshLocs struct — without this,
    // all GPU contents would leak forever (mesh_free was never called).
    static int s_registered = 0;
    if (!s_registered) { events_listen(EV_NODE_EXIT, mesh_node_exit, NULL); s_registered = 1; }

    Mesh *mesh = mesh_load_obj(obj_path);
    if (!mesh) return NULL;

    GLuint prog = make_program("shaders/mesh.vs", "shaders/mesh.fs");
    if (!prog) { mesh_free(mesh); return NULL; }

    MeshLocs *p = calloc(1, sizeof *p);
    if (!p) {
        resources_program_release(prog);
        mesh_free(mesh);
        return NULL;
    }
    p->rc = rc;
    p->mesh = mesh;
    p->prog = prog;
    p->scale = scale > 0 ? scale : 1.0f;
    p->u_model = glGetUniformLocation(prog, "uModel");
    p->u_tex = glGetUniformLocation(prog, "uTex");
    scene_uniforms_init(&p->su, prog);
    material_locs_init(&p->ml, prog);
    material_defaults(&p->mat);
    p->tex = tex_path ? resources_texture(tex_path, 4, GL_REPEAT, 0)
                      : resources_texture("img/notfound.png", 4, GL_REPEAT, 0);

    glUseProgram(prog);
    if (p->u_tex >= 0) glUniform1i(p->u_tex, 0);
    for (int c = 0; c < SHADOW_CASCADES; c++) {
        if (p->su.u_shadowmap[c] >= 0) glUniform1i(p->su.u_shadowmap[c], 3 + c);
        if (p->su.u_shadowdepth[c] >= 0) glUniform1i(p->su.u_shadowdepth[c], 3 + c);
    }
    glUseProgram(0);

    float size[3] = {scale, scale, scale};
    SceneNode *n = node_new("MeshInstance", "MeshInstance3D", pos, size, p,
                            NULL, mesh_node_render);
    if (!n) {
        if (p->tex) resources_texture_release(p->tex);
        resources_program_release(prog);
        mesh_free(mesh);
        free(p);
        return NULL;
    }
    // Scale goes into the node transform (scene.c): the mesh can be rotated and attached.
    n->scl[0] = n->scl[1] = n->scl[2] = p->scale;
    node_update_transforms(n);
    return n;
}
