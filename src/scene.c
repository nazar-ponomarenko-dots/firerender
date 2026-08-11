// ============================================================================
//  scene.c — дерево сцены: создание узлов и обход (update/render/очистка)
// ============================================================================
#include "scene.h"

#include "events.h"

#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>

SceneNode *node_new(const char *name, const char *type,
                    const float *pos, const float *size,
                    void *params, NodeFn update, NodeFn render)
{
    SceneNode *n = calloc(1, sizeof *n);
    n->name = name;
    n->type = type;
    if (pos)  memcpy(n->pos, pos, sizeof n->pos);
    if (size) memcpy(n->size, size, sizeof n->size);
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
    SDL_Log("%s%s (%s)  pos=(%.0f, %.0f, %.0f) size=(%.0f, %.0f, %.0f)",
            indent, n->name, n->type,
            n->pos[0], n->pos[1], n->pos[2],
            n->size[0], n->size[1], n->size[2]);
    for (SceneNode *c = n->first_child; c; c = c->next)
        scene_print(c, depth + 1);
}
