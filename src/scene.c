// ============================================================================
//  scene.c — дерево сцены: создание узлов, трансформы и обход
// ============================================================================
#include "scene.h"

#include "events.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

// Локальные помощники матриц (linalg.c даёт mat4_mul, но здесь нужен ещё
// TRS-сбор; держим его рядом с узлом, чтобы scene.c не тянул лишнего).
static void m4_identity(float m[16])
{
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void m4_mul(float out[16], const float a[16], const float b[16])
{
    float r[16];
    for (int c = 0; c < 4; c++)
        for (int rr = 0; rr < 4; rr++) {
            float s = 0.0f;
            for (int k = 0; k < 4; k++) s += a[k * 4 + rr] * b[c * 4 + k];
            r[c * 4 + rr] = s;
        }
    memcpy(out, r, sizeof r);
}

SceneNode *node_new(const char *name, const char *type,
                    const float *pos, const float *size,
                    void *params, NodeFn update, NodeFn render)
{
    SceneNode *n = calloc(1, sizeof *n);
    n->name = name;
    n->type = type;
    if (pos)  memcpy(n->pos, pos, sizeof n->pos);
    if (size) memcpy(n->size, size, sizeof n->size);
    n->scl[0] = n->scl[1] = n->scl[2] = 1.0f;
    n->attach_slot = -1;
    m4_identity(n->world);
    n->params = params;
    n->update = update;
    n->render = render;
    return n;
}

void node_add(SceneNode *parent, SceneNode *child)
{
    if (!parent || !child) return;
    if (child->parent) node_remove(child);   // не даём двойного родителя
    child->parent = parent;
    if (parent->last_child) parent->last_child->next = child;
    else                    parent->first_child = child;
    parent->last_child = child;
    events_emit(EV_NODE_INIT, child, NULL);  // узел вошёл в дерево
}

void node_remove(SceneNode *n)
{
    SceneNode *p = n ? n->parent : NULL;
    if (!p) return;
    if (p->first_child == n) p->first_child = n->next;
    SceneNode *prev = NULL;
    for (SceneNode *c = p->first_child; c && c != n; c = c->next) prev = c;
    if (prev) prev->next = n->next;
    if (p->last_child == n) p->last_child = n->next ? n->next : prev;
    n->parent = NULL;
    n->next = NULL;
}

SceneNode *node_find(SceneNode *root, const char *name)
{
    if (!root) return NULL;
    if (name && root->name && strcmp(root->name, name) == 0) return root;
    for (SceneNode *c = root->first_child; c; c = c->next) {
        SceneNode *r = node_find(c, name);
        if (r) return r;
    }
    return NULL;
}

// Локальная матрица: T * Ry * Rx * Rz * S. Порядок поворотов как в Godot
// (yaw вокруг Y — главный, поэтому применяется последним).
void node_local_matrix(const SceneNode *n, float out[16])
{
    float sy = sinf(n->rot[1]), cy = cosf(n->rot[1]);
    float sx = sinf(n->rot[0]), cx = cosf(n->rot[0]);
    float sz = sinf(n->rot[2]), cz = cosf(n->rot[2]);
    // R = Ry * Rx * Rz (столбцовый порядок GL: m[col*4 + row])
    float r00 =  cy * cz + sy * sx * sz;
    float r01 = -cy * sz + sy * sx * cz;
    float r02 =  sy * cx;
    float r10 =  cx * sz;
    float r11 =  cx * cz;
    float r12 = -sx;
    float r20 = -sy * cz + cy * sx * sz;
    float r21 =  sy * sz + cy * sx * cz;
    float r22 =  cy * cx;
    float sxs = n->scl[0] != 0.0f ? n->scl[0] : 1.0f;
    float sys = n->scl[1] != 0.0f ? n->scl[1] : 1.0f;
    float szs = n->scl[2] != 0.0f ? n->scl[2] : 1.0f;
    out[0]  = r00 * sxs;  out[1]  = r10 * sxs;  out[2]  = r20 * sxs;  out[3]  = 0;
    out[4]  = r01 * sys;  out[5]  = r11 * sys;  out[6]  = r21 * sys;  out[7]  = 0;
    out[8]  = r02 * szs;  out[9]  = r12 * szs;  out[10] = r22 * szs;  out[11] = 0;
    out[12] = n->pos[0];  out[13] = n->pos[1];  out[14] = n->pos[2];  out[15] = 1;
}

void node_world_pos(const SceneNode *n, float out[3])
{
    if (!n) { out[0] = out[1] = out[2] = 0.0f; return; }
    out[0] = n->world[12];
    out[1] = n->world[13];
    out[2] = n->world[14];
}

void node_update_transforms(SceneNode *n)
{
    if (!n) return;
    float local[16];
    node_local_matrix(n, local);
    if (n->parent) {
        // Точка крепления родителя (кость скелета и т.п.): вставляется
        // МЕЖДУ мировой матрицей родителя и локальной матрицей ребёнка.
        if (n->attach_slot >= 0 && n->parent->attach_fn) {
            float att[16];
            m4_identity(att);
            n->parent->attach_fn(n->parent, n->attach_slot, att);
            if (n->attach_upright) {
                // Оставляем только позицию точки: спрайт/иконка не должны
                // крутиться вместе с костью (маркеры, метки над головой).
                float t[3] = { att[12], att[13], att[14] };
                m4_identity(att);
                att[12] = t[0]; att[13] = t[1]; att[14] = t[2];
            }
            float pa[16];
            m4_mul(pa, n->parent->world, att);
            m4_mul(n->world, pa, local);
        } else {
            m4_mul(n->world, n->parent->world, local);
        }
    } else {
        memcpy(n->world, local, sizeof local);
    }
    for (SceneNode *c = n->first_child; c; c = c->next)
        node_update_transforms(c);
}

void node_update_tree(SceneNode *n, float dt)
{
    if (n->update) n->update(n, dt);
    for (SceneNode *c = n->first_child; c; c = c->next)
        node_update_tree(c, dt);
}

void node_render_tree(SceneNode *n, float dt)
{
    if (n->render) n->render(n, dt);
    for (SceneNode *c = n->first_child; c; c = c->next)
        node_render_tree(c, dt);
}

void node_free_tree(SceneNode *n)
{
    if (!n) return;
    events_emit(EV_NODE_EXIT, n, NULL);      // узел уходит из дерева
    for (SceneNode *c = n->first_child; c;) {
        SceneNode *tmp = c->next;
        node_free_tree(c);
        c = tmp;
    }
    free(n->params);
    free(n);
}

void scene_print(SceneNode *n, int depth)
{
    char indent[64] = "";
    for (int i = 0; i < depth && (size_t)i < sizeof indent - 3; i++)
        strcat(indent, "  ");
    SDL_Log("%s%s (%s)  pos=(%.0f, %.0f, %.0f) size=(%.0f, %.0f, %.0f)%s",
            indent, n->name, n->type,
            n->pos[0], n->pos[1], n->pos[2],
            n->size[0], n->size[1], n->size[2],
            n->attach_slot >= 0 ? "  [attached]" : "");
    for (SceneNode *c = n->first_child; c; c = c->next)
        scene_print(c, depth + 1);
}
