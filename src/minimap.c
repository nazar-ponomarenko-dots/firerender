// ============================================================================
//  minimap.c — minimap: map + player dot + view direction
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
    if (!p) {
        glDeleteBuffers(1, &vbo);
        glDeleteVertexArrays(1, &vao);
        resources_program_release(prog);
        return NULL;
    }
    p->prog = prog;
    p->vao = vao;
    p->vbo = vbo;
    p->tex_base = tex_base;                 // map base color (for the minimap)
    p->world = w;
    p->rc = rc;
    p->u_basemap = glGetUniformLocation(prog, "uBasemap");
    p->u_color   = glGetUniformLocation(prog, "uColor");
    p->u_usetex  = glGetUniformLocation(prog, "uUseTex");
    p->u_usemarker = glGetUniformLocation(prog, "uUseMarker");
    p->visible = 1;
    // Player marker (img/player.png): an arrow, rotated by yaw every frame.
    // The texture comes through the resource cache.
    p->tex_marker = resources_texture("img/player.png", 4,
                                      GL_CLAMP_TO_EDGE, 1);
    glUseProgram(prog);
    glUniform1i(p->u_basemap, 0);        // map bound to texture unit 0
    glUniform1i(glGetUniformLocation(prog, "uMarker"), 1);  // marker — unit 1
    glUseProgram(0);
    const float pos[3] = { 16, 16, 0 };         // inset from the window edge
    const float size[3] = { 220, 220 * w->world_d / w->world_w, 0 };
    SceneNode *node = node_new("Minimap", "CanvasItem2D", pos, size, p,
                               NULL, minimap_node_render);
    if (!node) {
        if (p->tex_marker) resources_texture_release(p->tex_marker);
        glDeleteBuffers(1, &p->vbo);
        glDeleteVertexArrays(1, &p->vao);
        resources_program_release(p->prog);
        free(p);
    }
    return node;
}

void minimap_node_render(SceneNode *n, float dt)
{
    (void)dt;   // the update is tied to the frame, not to time
    // Draw EVERY frame: the whole scene is redrawn, and skipping frames
    // (it used to be "once every 3") caused flickering — the map vanished on
    // skipped frames. Re-uploading a 16-vertex quad costs nothing, no point
    // saving on it.
    MinimapParams *p = n->params;
    if (!p->visible) return;        // G key — map hidden
    const RendererContext *rc = p->rc;
    if (rc->pass == RP_SHADOW) return;      // 2D must not write into shadow maps
    int vw = p->vw, vh = p->vh;
    if (vw <= 0 || vh <= 0) return;
    glDisable(GL_DEPTH_TEST);   // 2D on top of the 3D scene

    // Screen quad in the top-left corner. NDC coordinates are computed
    // directly from the window size — on resize the minimap scales by itself.
    float mm_w = n->size[0];
    float mm_h = n->size[1];
    float mx0 = 16.0f, my0 = vh - 16.0f - mm_h;
    float nx0 = -1.0f + 2.0f * mx0 / vw;
    float nx1 = -1.0f + 2.0f * (mx0 + mm_w) / vw;
    float ny1 =  1.0f - 2.0f * my0 / vh;              // top (v=0)
    float ny0 =  1.0f - 2.0f * (my0 + mm_h) / vh;     // bottom  (v=1)

    // Player position in map uv (x/WORLD_W, z/WORLD_D), clamped to the edges
    const WorldState *w = p->world;
    float u = rc->cam_pos[0] / w->world_w;
    float v = rc->cam_pos[2] / w->world_d;
    if (u < 0) u = 0;
    if (u > 1) u = 1;
    if (v < 0) v = 0;
    if (v > 1) v = 1;
    float px = nx0 + u * (nx1 - nx0);
    float py = ny1 - v * (ny1 - ny0);   // z grows downwards on the map

    // View-direction tick (12 m ahead) + yellow line pointing at the sun
    // (from the player, 60 m).
    float lx = sinf(rc->cam_yaw) * 12.0f / w->world_w * (nx1 - nx0);
    float ly = -cosf(rc->cam_yaw) * 12.0f / w->world_d * (ny1 - ny0);
    // Direction towards the sun (the first directional light)
    float sx = sinf(atan2f(rc->light_dir[0][0], rc->light_dir[0][2])) * 60.0f / w->world_w * (nx1 - nx0);
    float sy = -cosf(atan2f(rc->light_dir[0][0], rc->light_dir[0][2])) * 60.0f / w->world_d * (ny1 - ny0);

    // Player marker: img/player.png rotated by yaw — the arrow points where
    // the player looks. forward = (sin yaw, 0, cos yaw); on the map
    // +x is right, +z is down -> the view direction on screen is (sin yaw, -cos yaw),
    // texture rotation angle θ = yaw + π (the texture points "north").
    float rh = 11.0f;                       // half-marker size, px
    float ux = sinf(rc->cam_yaw) * 2.0f * rh / vw;
    float uy = -cosf(rc->cam_yaw) * 2.0f * rh / vh;
    float px2 = cosf(rc->cam_yaw) * 2.0f * rh / vw;
    float py2 = sinf(rc->cam_yaw) * 2.0f * rh / vh;
    // Vertices: 0-5 map, 6-11 marker, 12-13 view line, 14-15 sun
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
    glDrawArrays(GL_TRIANGLES, 0, 6);       // the map itself
    // player marker: texture with its own alpha (blending — only here)
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUniform1f(p->u_usetex, 0.0f);
    glUniform1f(p->u_usemarker, 1.0f);
    glDrawArrays(GL_TRIANGLES, 6, 6);       // marker
    glDisable(GL_BLEND);
    glUniform1f(p->u_usemarker, 0.0f);
    glUniform3f(p->u_color, 1.0f, 0.25f, 0.15f); // orange — view line
    glDrawArrays(GL_LINES, 12, 2);          // view direction
    glUniform3f(p->u_color, 1.0f, 0.85f, 0.2f); // yellow — sun
    glDrawArrays(GL_LINES, 14, 2);

}

void minimap_cleanup(SceneNode *n)
{
    if (!n) return;   // error-path scene_build: the node may not have been created
    MinimapParams *p = n->params;
    resources_program_release(p->prog);
    resources_texture_release(p->tex_marker);   // the base map is shared, don't delete it
    glDeleteBuffers(1, &p->vbo);
    glDeleteVertexArrays(1, &p->vao);
}
