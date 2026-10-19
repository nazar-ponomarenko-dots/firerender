// ============================================================================
//  shadow.c — cascaded shadow maps: FBOs, matrices and pass orchestration
// ----------------------------------------------------------------------------
//  The shadow pass is UNIFIED (item 13): for each cascade shadow_render sets
//  pass = RP_SHADOW in RendererContext and walks the scene tree — terrain,
//  vegetation and objects draw THEIR OWN geometry into depth (each node has
//  its own program and its own thinning rules), the remaining nodes are
//  skipped in the shadow pass. Only the cascade FBOs and the
//  "light -> NDC" matrices (with texel alignment) stay here.
//
//  Optimized for frame rate:
//    - vegetation in shadows: thinning is done on the CPU (vegetation.c:
//      statics — every 2nd instance, dynamics — the uThin rule), the shader
//      does not reject instances, the vertex stage is twice cheaper;
//    - cascades 0/1 (8/16 m boxes): only super-chunks/chunks inside the box
//      are drawn (ARB_base_instance) — the vertex stage does not run hundreds
//      of thousands of instances for a few visible ones;
//    - cascade 3 (512 m) draws statics only: shadows of dense dynamics there
//      are smaller than a texel and invisible.
// ============================================================================
#include "shadow.h"

#include "cvars.h"
#include "linalg.h"
#include "mapdata.h"
#include "renderer.h"
#include "shader.h"
#include "world.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static GLuint g_shadow_fbo[SHADOW_CASCADES], g_shadow_tex[SHADOW_CASCADES];
static int g_shadow_on = 1;         // [ — enable/disable shadows
int g_shadow_size[SHADOW_CASCADES] = { 2048, 2048, 2048, 2048 };
// Test toggles for performance measurements (FR_TEST in main.c):
int g_shadow_terrain = 1;           // terrain into shadow maps
int g_shadow_cull = 1;              // chunk culling by cascade box
int g_shadow_veg = 1;               // vegetation into shadow maps
int g_shadow_obj = 1;               // objects (trees) into shadow maps
int g_shadow_cache = 1;             // static cascade cache (FR_TEST 'C' disables)
// Cascade matrices of the last frame (for renderer.c/compatibility)
static float g_last_mvp[SHADOW_CASCADES][16];

// --- Static cascade cache -------------------------------------------------
// Cascades 2/3 contain no dynamics (vegetation.c draws dynamics only when
// shadow_dyn, models never write into the shadow maps at all) — their content
// depends ONLY on the snapped matrix. While it has not changed, there is no
// need to redraw the map: the FBO keeps last frame's depth. Standing still —
// 100% hits; while moving the matrix changes only once per snap-grid step
// (the step is coarser than a texel: 4 texels for cascade 2, 8 — for cascade
// 3; shadow jitter while moving is unnoticed — shadows there are soft and
// blended with the neighbouring cascade).
// Content: terrain, trees, static grass/wheat (culled by super-chunks —
// a handful of chunks per cascade, see vegetation.c).
static float g_cache_vp[SHADOW_CASCADES][16];
static int g_cache_valid[SHADOW_CASCADES];

static void shadow_cache_invalidate(void)
{
    memset(g_cache_valid, 0, sizeof g_cache_valid);
}

void shadow_init(void)
{
    // State comes from the r_shadows_mode cvar: otherwise r_shadows_mode 0 in
    // config.cfg would not disable shadows (sync_cvars fires on change).
    {
        float m = cvar_get("r_shadows_mode");
        g_shadow_on = (m == 1.0f || m == 2.0f);
    }
    shadow_cache_invalidate();
    // Shadow map size from map/map.json (shadows.size, default 2048)
    {
        MapShadows sh;
        mapdata_shadows(&sh);
        if (sh.size >= 512 && sh.size <= 8192)
            for (int c = 0; c < SHADOW_CASCADES; c++)
                g_shadow_size[c] = sh.size;
        SDL_Log("Shadows: %dx%d per cascade", g_shadow_size[0], g_shadow_size[0]);
    }

    // Cascade shadow maps: colorless FBO (depth only)
    for (int c = 0; c < SHADOW_CASCADES; c++) {
        int size = g_shadow_size[c];
        glGenTextures(1, &g_shadow_tex[c]);
        glBindTexture(GL_TEXTURE_2D, g_shadow_tex[c]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, size, size,
                     0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, NULL);
        // Hardware PCF: comparison against reference + bilinear filter —
        // one texture() in the shader gives a smooth penumbra without bands.
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE,
                        GL_COMPARE_REF_TO_TEXTURE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
        glGenFramebuffers(1, &g_shadow_fbo[c]);
        glBindFramebuffer(GL_FRAMEBUFFER, g_shadow_fbo[c]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D,
                               g_shadow_tex[c], 0);
        if (p_glDrawBuffer) glDrawBuffer(GL_NONE);
        if (p_glReadBuffer) glReadBuffer(GL_NONE);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            SDL_Log("Shadow FBO %d incomplete!", c);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
}

int shadow_is_on(void)
{
    return g_shadow_on;
}

void shadow_toggle(void)
{
    g_shadow_on = !g_shadow_on;
    shadow_cache_invalidate();
}

void shadow_set(int on)
{
    g_shadow_on = on ? 1 : 0;
    shadow_cache_invalidate();
}

GLuint shadow_texture(int c)
{
    return g_shadow_tex[c];
}

const float *shadow_matrix(int c)
{
    return g_last_mvp[c];
}

// Shadow matrix from the sun position: view+projection (vp) and with [0..1] bias (mvp).
// center — the box center (player position).
// ndc_texel — the NDC-square size of one texel (0 = no alignment):
// the map is aligned to the texel grid so that while the camera moves
// shadows do not "crawl" back and forth by one texel.
static void shadow_matrix_for(const float *center, const float *sun_dir,
                              float area, float depth, float ndc_texel,
                              float mvp[16], float vp[16])
{
    float e[3] = { center[0] + sun_dir[0] * depth,
                   center[1] + sun_dir[1] * depth,
                   center[2] + sun_dir[2] * depth };
    float up[3] = { 0, 1, 0 }, view[16], proj[16];
    mat4_lookat(view, e, center, up);
    mat4_ortho(proj, -area / 2, area / 2, -area / 2, area / 2, 1.0f, 2.0f * depth);
    mat4_mul(vp, proj, view);
    if (ndc_texel > 0.0f) {
        // Shadow stabilization: the texel grid is anchored to the WORLD (anchor —
        // the coordinate origin), not to the camera. Otherwise while walking the
        // grid re-snaps by a texel and shadow edges flicker/jump.
        float pc[4] = { 0.0f, 0.0f, 0.0f, 1.0f };   // fixed world point
        float pv[4];
        for (int r = 0; r < 4; r++)
            pv[r] = vp[0*4+r]*pc[0] + vp[1*4+r]*pc[1]
                  + vp[2*4+r]*pc[2] + vp[3*4+r]*pc[3];
        float nx = pv[0] / pv[3], ny = pv[1] / pv[3];
        vp[12] += floorf(nx / ndc_texel + 0.5f) * ndc_texel - nx;
        vp[13] += floorf(ny / ndc_texel + 0.5f) * ndc_texel - ny;
    }
    float bias[16] = { 0.5f,0,0,0,  0,0.5f,0,0,  0,0,0.5f,0,  0.5f,0.5f,0.5f,1 };
    mat4_mul(mvp, bias, vp);
}
// Draws all cascades from the sun camera. Scene nodes draw depth themselves
// (pass == RP_SHADOW in the context); here — only matrices and cascade FBOs.
void shadow_render(SceneNode *root, RendererContext *rc)
{
    // Shadows are drawn only if enabled globally (key [) and the light
    // source itself casts shadows (shadow.enabled from map/map.json).
    if (!rc->shadow_on) return;
    const float *sd = rc->light_dir[0];
    const float boxes[SHADOW_CASCADES] = {
        SHADOW_BOX_0, SHADOW_BOX_1, SHADOW_BOX_2, SHADOW_BOX_3
    };
    const int sizes[SHADOW_CASCADES] = {
        g_shadow_size[0], g_shadow_size[1], g_shadow_size[2], g_shadow_size[3]
    };
    // Shadow slope: how far horizontally the shadow of a 1 m tall object
    // stretches (horizontal / vertical along the sun direction). Low sun —
    // long shadows: distant trees must be drawn into the near cascades,
    // otherwise their shadow (lying next to the player) disappears — nodes
    // (vegetation/objects) add height*shadow_slope to the culling radius.
    {
        float hy = sd[1] > 0.05f ? sd[1] : 0.05f;
        float hl = sqrtf(sd[0]*sd[0] + sd[2]*sd[2]);
        float slope = hl / hy;
        rc->shadow_slope = slope > 12.0f ? 12.0f : slope;   // cap for shallow angles
    }

    GLint prev_fbo = 0, prev_vp[4] = { 0, 0, 0, 0 };
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    glGetIntegerv(GL_VIEWPORT, prev_vp);

    for (int c = 0; c < SHADOW_CASCADES; c++) {
        float box = boxes[c];
        float depth = box * SHADOW_DEPTH_MUL + 400.0f;
        float vp[16], mvp[16];
        shadow_matrix_for(rc->cam_pos, sd, box, depth,
                          2.0f / sizes[c], mvp, vp);
        // Fresh cascade matrices — into the context (vegetation/objects/
        // voxfog read them in this same frame) and into g_last_mvp (for renderer_ctx_fill)
        memcpy(rc->shadow_matrix[c], mvp, sizeof mvp);
        memcpy(g_last_mvp[c], mvp, sizeof mvp);

        // Static cascade cache: the matrix has not changed — the depth map in
        // the FBO already holds exactly this frame. Cascades 2/3 contain terrain
        // and trees (statics); static grass sway there is < 1 texel
        // (9.4/50 cm), so the frozen map does not flicker. Skip the whole
        // pass. Matrices are snapped to the grid — while walking, distant
        // shadows shift in smooth small steps, without jitter.
        if (c >= 2 && g_shadow_cache && g_cache_valid[c] &&
            memcmp(g_cache_vp[c], vp, sizeof vp) == 0)
            continue;

        // Shadow pass: nodes draw depth into this cascade
        rc->pass = RP_SHADOW;
        memcpy(rc->shadow_vp, vp, sizeof vp);
        rc->shadow_box_cur = box;
        rc->shadow_cascade = c;
        // Cascades 0-1 (8/16 m): statics + dynamics (detailed shadows at the feet).
        // Cascades 2/3 (96/512 m): statics only — dynamics there (~200 thousand
        // instances) cast a shadow smaller than a texel. Statics are now in ALL
        // cascades (in the 3rd, vegetation.c thins them down to a single wheat):
        // field and tree shadows reach the horizon, while super-chunk culling
        // + the matrix cache make these passes almost free.
        // NOTE: vegetation here is drawn EVERY frame — trying to update it
        // every other frame (checkerboard) gives visible sway flicker:
        // at a cascade-0 texel (~4 mm) the 7 cm grass swing is ~20 texels.
        rc->shadow_dyn = (c < 2) ? 1 : 0;
        rc->shadow_static = 1;
        rc->shadow_cull = g_shadow_cull ? 1 : 0;

        glBindFramebuffer(GL_FRAMEBUFFER, g_shadow_fbo[c]);
        glViewport(0, 0, sizes[c], sizes[c]);
        // IMPORTANT: the depth write mask must be TRUE BEFORE glClear — otherwise
        // glClear is ignored (the skybox just set glDepthMask(FALSE), and without
        // the reset the shadow map "smears").
        glDepthMask(GL_TRUE);
        glClear(GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);
        glCullFace(GL_BACK);         // back faces — as in the regular render

        // Unified pass: nodes choose themselves what to draw in RP_SHADOW
        for (SceneNode *n = root->first_child; n; n = n->next)
            if (n->render) n->render(n, 0.0f);

        memcpy(g_cache_vp[c], vp, sizeof vp);
        g_cache_valid[c] = 1;
    }
    rc->pass = RP_COLOR;
    // Restore the color pass's FBO and viewport (the first node — sky — does
    // not reset the viewport itself, and draws a fullscreen triangle)
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fbo);
    glViewport(prev_vp[0], prev_vp[1], prev_vp[2], prev_vp[3]);
}

void shadow_cleanup(void)
{
    glDeleteTextures(SHADOW_CASCADES, g_shadow_tex);
    glDeleteFramebuffers(SHADOW_CASCADES, g_shadow_fbo);
}
