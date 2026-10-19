// ============================================================================
//  bloom.c — glow postprocessing (bloom)
// ----------------------------------------------------------------------------
//  Pipeline: scene (FBO) -> bright (half resolution) -> Gaussian H -> V
//  -> composite to screen. The scene is LDR (8-bit), so the brightness threshold
//  is soft — the brightest spots glow (sky, ears of grain against the sun).
// ============================================================================
#include "bloom.h"

#include "cvars.h"
#include "fx.h"
#include "mapdata.h"
#include "resources.h"
#include "shader.h"

#include <stddef.h>

static BloomParams *s_bloom_p = NULL;   // for hot-reload (shader_on_reshade)
static void bloom_prog_init(BloomParams *b);   // uniforms (ids already ready)
static void bloom_reshade(void);               // below: replace + prog_init

// --- Underwater effects (passed into the composite) ---------------------------
// The renderer updates the state every frame (smooth water entry/exit);
// bloom_render forwards it to the composite uniforms.
static float s_underwater = 0.0f;
static float s_water_tint[3] = { 0.08f, 0.22f, 0.30f };
static float s_under_time = 0.0f;

void bloom_set_underwater(float amount, const float tint[3], float time)
{
    if (amount < 0.0f) amount = 0.0f;
    if (amount > 1.0f) amount = 1.0f;
    s_underwater = amount;
    if (tint) {
        s_water_tint[0] = tint[0];
        s_water_tint[1] = tint[1];
        s_water_tint[2] = tint[2];
    }
    if (time > 0.0f) s_under_time = time;
}

#include <SDL3/SDL.h>
#include <stdlib.h>

static GLuint make_scene_tex(int w, int h, int ldr)
{
    // ldr (r_scene_ldr): RGBA8 instead of RGBA16F — on low-end there is no bloom/GI/
    // fog, HDR is unneeded, and writing fragments + reading the composite are the main
    // bandwidth cost on weak iGPUs
    GLuint id;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (ldr)
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    else
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0,
                     GL_RGBA, GL_HALF_FLOAT, NULL);
    return id;
}

static GLuint make_ldr_tex(int w, int h)
{
    // LDR copy of the scene for the low-end present path: half the bandwidth on write/read
    GLuint id;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    return id;
}

static GLuint make_color_tex(int w, int h)
{
    GLuint id;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    // On LDR/GLES glow is RGBA8 too: bloom is off in the low-end profile, but
    // a valid target is still needed for instant toggling without an unsupported RT.
    if (cvar_get("r_scene_ldr") > 0.5f)
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    else
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0,
                     GL_RGBA, GL_HALF_FLOAT, NULL);
    return id;
}

static void make_fbo(GLuint *fbo, GLuint *tex, GLuint *depth,
                     int w, int h, int want_depth)
{
    glGenFramebuffers(1, fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, *fbo);
    *tex = make_color_tex(w, h);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, *tex, 0);
    if (want_depth) {
        // Scene depth as a TEXTURE (GL_DEPTH_COMPONENT24) so the 3D scene
        // can be depth-tested against it. NEAREST filter — no comparison.
        glGenTextures(1, depth);
        glBindTexture(GL_TEXTURE_2D, *depth);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, w, h, 0,
                     GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, NULL);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                               GL_TEXTURE_2D, *depth, 0);
    }
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        SDL_Log("Bloom FBO incomplete (%dx%d)!", w, h);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// Programs are created OUTSIDE (bloom_init / reshade: refcount lives there).
// Here — only uniforms for the CURRENT ids.
static void bloom_prog_init(BloomParams *b)
{
    b->u_btex      = glGetUniformLocation(b->prog_bright, "uTex");
    b->u_threshold = glGetUniformLocation(b->prog_bright, "uThreshold");
    b->u_intensity = glGetUniformLocation(b->prog_bright, "uIntensity");
    b->u_blurtex   = glGetUniformLocation(b->prog_blur, "uTex");
    b->u_dir       = glGetUniformLocation(b->prog_blur, "uDir");
    b->u_texel     = glGetUniformLocation(b->prog_blur, "uTexel");
    b->u_ctex      = glGetUniformLocation(b->prog_composite, "uScene");
    b->u_glowtex   = glGetUniformLocation(b->prog_composite, "uGlow");
    b->u_strength  = glGetUniformLocation(b->prog_composite, "uStrength");
    b->u_exposure  = glGetUniformLocation(b->prog_composite, "uExposure");
    b->u_gamma     = glGetUniformLocation(b->prog_composite, "uGamma");
    b->u_sat       = glGetUniformLocation(b->prog_composite, "uSat");
    b->u_contrast  = glGetUniformLocation(b->prog_composite, "uContrast");
    b->u_wetness   = glGetUniformLocation(b->prog_composite, "uWetness");
    b->u_underwater = glGetUniformLocation(b->prog_composite, "uUnderwater");
    b->u_watertint  = glGetUniformLocation(b->prog_composite, "uWaterTint");
    b->u_time       = glGetUniformLocation(b->prog_composite, "uTime");
}

// Hot-reload: replace balances the refcount (release old + new reference);
// if the pair fails to link — the previous id stays.
static void bloom_reshade(void)
{
    BloomParams *b = s_bloom_p;
    if (!b) return;
    GLuint p1 = resources_program_replace(b->prog_bright,
                                          "shaders/postfx.vs",
                                          "shaders/bright.fs");
    GLuint p2 = resources_program_replace(b->prog_blur,
                                          "shaders/postfx.vs",
                                          "shaders/blur.fs");
    GLuint p3 = resources_program_replace(b->prog_composite,
                                          "shaders/postfx.vs",
                                          "shaders/composite.fs");
    if (p1) b->prog_bright = p1;
    if (p2) b->prog_blur = p2;
    if (p3) b->prog_composite = p3;
    bloom_prog_init(b);
}

// --- Scene MSAA -------------------------------------------------------------
// The scene is drawn into a multisample FBO (RBO color+depth, cvar r_msaa: 0/2/4/8/16),
// then bloom_resolve expands it into plain textures (color + depth)
// that GI/SSR/fog/composite read. Geometry edges are smoothed
// in hardware, while depth for post effects stays crisp.
// Changing r_msaa applies at the start of the frame (bloom_msaa_sync) — no restart needed.

static void msaa_free(BloomParams *b)
{
    if (b->fbo_ms) {
        glDeleteFramebuffers(1, &b->fbo_ms);
        glDeleteRenderbuffers(1, &b->rbo_color);
        glDeleteRenderbuffers(1, &b->rbo_depth);
        b->fbo_ms = b->rbo_color = b->rbo_depth = 0;
    }
    b->msaa = 0;
}

// Desired sample count from the cvar: only 0/4/16 (intermediate modes removed —
// the 2x/4x and 8x/16x difference on screen is weak while the cost is noticeable).
// Whether the scene MSAA target is active (for alpha-to-coverage on clipped edges).
int bloom_msaa_active(void)
{
    return s_bloom_p ? s_bloom_p->msaa : 0;
}

static int msaa_wanted(void)
{
    // NOTE: previously, with an RGBA8 scene (r_scene_ldr, low-end preset) MSAA
    // was force-disabled — which made the menu "antialiasing" button do NOTHING
    // on the low-end preset (Off=4x=16x identical fps). MSAA works with LDR too:
    // the multisample buffer is then allocated as RGBA8, the same format as the
    // scene texture, so the resolve blit stays legal (see msaa_create) — and
    // bloom_resolve runs every frame, present reads the already-resolved one.
    // The choice is left to the player: r_msaa defaults to 0 — low-end stays fast, but
    // enabled 4x/16x now applies honestly (and honestly costs bandwidth).
    static const int allowed[] = { 0, 4, 16 };
    float v = cvar_get("r_msaa");
    int s = (int)(v + 0.5f);
    if (s < 0) s = 0;
    if (s > 16) s = 16;
    // nearest allowed value from below
    int best = 0;
    for (unsigned i = 0; i < sizeof allowed / sizeof allowed[0]; i++)
        if (allowed[i] <= s) best = allowed[i];
    if (best > 0) {
        static int maxs_cached = -1;
        if (maxs_cached < 0) glGetIntegerv(GL_MAX_SAMPLES, &maxs_cached);
        int maxs = maxs_cached;
        if (best > maxs) {
            best = 0;
            for (unsigned i = 0; i < sizeof allowed / sizeof allowed[0]; i++)
                if (allowed[i] <= s && allowed[i] <= maxs) best = allowed[i];
        }
        // Window-resolution cap: at 4K, 8-16 samples mean tens of ms
        // of pure bandwidth, and the difference vs 4x at that pixel density
        // is invisible to the eye
        extern int renderer_vw(void);
        extern int renderer_vh(void);
        long px = (long)renderer_vw() * renderer_vh();
        if (px > 6500000 && best > 4) best = 4;    // ~4K and above
        else if (px > 3300000 && best > 8) best = 8; // ~1440p
    }
    return best;
}

static void msaa_create(BloomParams *b, int samples)
{
    msaa_free(b);
    if (samples <= 0) return;
    glGenFramebuffers(1, &b->fbo_ms);
    glBindFramebuffer(GL_FRAMEBUFFER, b->fbo_ms);
    glGenRenderbuffers(1, &b->rbo_color);
    glBindRenderbuffer(GL_RENDERBUFFER, b->rbo_color);
    // Same internal format as the resolve target (b->tex). When SAMPLE_BUFFERS of
    // the read framebuffer is > 0, glBlitFramebuffer requires IDENTICAL formats
    // (GL ES 3.x and desktop GL 4.x: "the formats of read and draw are not
    // identical" -> INVALID_OPERATION and the blit is dropped). NVIDIA 4.5 core
    // tolerates RGBA16F->RGBA8 here (measured: no error, AA visible), a conforming
    // GLES driver (Adreno/Mali) drops the resolve -> the scene stays black/stale.
    // So LDR (r_scene_ldr) multisamples as RGBA8, HDR as RGBA16F.
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples,
                                     b->scene_ldr ? GL_RGBA8 : GL_RGBA16F,
                                     b->vw, b->vh);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                              GL_RENDERBUFFER, b->rbo_color);
    glGenRenderbuffers(1, &b->rbo_depth);
    glBindRenderbuffer(GL_RENDERBUFFER, b->rbo_depth);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples,
                                     GL_DEPTH_COMPONENT24, b->vw, b->vh);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER, b->rbo_depth);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        // The driver won't provide multisampled RGBA16F — honest fallback without MSAA.
        // Remember the combination, otherwise sync would recreate this FBO every frame.
        SDL_Log("MSAA %dx: FBO incomplete, disabling (no antialiasing)", samples);
        b->msaa_failed = samples;
        b->msaa_failed_ldr = b->scene_ldr;
        msaa_free(b);
    } else {
        b->msaa = samples;
        b->msaa_failed = 0;
        SDL_Log("MSAA: %dx", samples);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void bloom_msaa_sync(BloomParams *b)
{
    int want = msaa_wanted();
    if (want == b->msaa_failed && b->scene_ldr == b->msaa_failed_ldr) return;
    if (want != b->msaa) msaa_create(b, want);
}

void bloom_resolve(BloomParams *b, int want_depth)
{
    // Without MSAA the only job is the RBO->texture depth copy. If nobody
    // reads it (low-end: GI/SSR/voxfog off), skip the whole blit.
    if (!b->msaa && (!b->rbo_depth_nm || !want_depth)) return;
    GLint readf = 0, drawf = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readf);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawf);
    GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    glDisable(GL_SCISSOR_TEST);            // blit respects the scissor — disable it
    if (b->msaa) {
        // Color + depth: post effects (GI/SSR/fog) read the resolved
        glBindFramebuffer(GL_READ_FRAMEBUFFER, b->fbo_ms);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, b->fbo_res);
        glBlitFramebuffer(0, 0, b->vw, b->vh, 0, 0, b->vw, b->vh,
                          GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT,
                          GL_NEAREST);
    } else {
        // Without MSAA the color is already in a texture; copy depth from RBO to texture
        // (rendering into a depth texture disables HiZ — see bloom_resize).
        glBindFramebuffer(GL_READ_FRAMEBUFFER, b->fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, b->fbo_res);
        glBlitFramebuffer(0, 0, b->vw, b->vh, 0, 0, b->vw, b->vh,
                          GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)readf);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)drawf);
    if (scissor) glEnable(GL_SCISSOR_TEST);
}

void bloom_init(BloomParams *b, int vw, int vh)
{
    s_bloom_p = b;
    // Postprocessing parameters — from map/map.json (bloom = { ... }), like
    // cvar defaults: the "Graphics" menu and console edit them
    // live (applied in bloom_render every frame).
    MapBloom bc;
    mapdata_bloom(&bc);
    cvar_register("r_bloom", 1.0f, "свечение (bloom) вкл/выкл");
    cvar_register("r_bloom_threshold", bc.threshold, "порог яркости bloom");
    cvar_register("r_bloom_intensity", bc.intensity, "сила ярких пикселей");
    cvar_register("r_bloom_strength", bc.strength, "сила свечения в композите");
    cvar_register("r_exposure", 1.0f, "экспозиция HDR (тонмаппинг)");
    cvar_register("r_gamma", 1.0f, "гамма-коррекция (1 = без изменений)");
    cvar_register("r_saturation", 1.4f, "насыщенность (1 = без изменений)");
    cvar_register("r_contrast", 1.0f, "контраст (1 = без изменений)");
    cvar_register_ex("r_msaa", 0.0f, 0.0f, 16.0f, CVAR_ARCHIVE,
                     "сглаживание MSAA: 0/2/4/8/16 (применяется сразу)");
    b->threshold = bc.threshold;
    b->intensity = bc.intensity;
    b->strength  = bc.strength;
    // Postprocessing programs (replace: old==0 after cleanup => plain load)
    b->prog_bright = resources_program_replace(b->prog_bright,
                                               "shaders/postfx.vs",
                                               "shaders/bright.fs");
    b->prog_blur = resources_program_replace(b->prog_blur,
                                             "shaders/postfx.vs",
                                             "shaders/blur.fs");
    b->prog_composite = resources_program_replace(b->prog_composite,
                                                  "shaders/postfx.vs",
                                                  "shaders/composite.fs");
    bloom_prog_init(b);    // uniforms (for hot-reload)

    shader_on_reshade(bloom_reshade);

    // Fullscreen triangle: 3 vertices cover the whole screen
    glGenVertexArrays(1, &b->vao);
    glBindVertexArray(b->vao);
    glGenBuffers(1, &b->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, b->vbo);
    const float tri[3][4] = {
        { -1, -1, 0, 0 },
        {  3, -1, 2, 0 },
        { -1,  3, 0, 2 },
    };
    glBufferData(GL_ARRAY_BUFFER, sizeof tri, tri, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), NULL);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          (const void *)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);

    bloom_resize(b, vw, vh);
}

void bloom_resize(BloomParams *b, int vw, int vh)
{
    if (vw <= 0 || vh <= 0) return;
    b->vw = vw;
    b->vh = vh;
    b->msaa_failed = 0;
    int gw = vw / 2, gh = vh / 2;
    if (gw < 1) gw = 1;
    if (gh < 1) gh = 1;
    b->gw = gw;
    b->gh = gh;

    if (b->fbo) {
        glDeleteFramebuffers(1, &b->fbo);
        glDeleteTextures(1, &b->tex);
        glDeleteTextures(1, &b->depth);
        if (b->rbo_depth_nm) glDeleteRenderbuffers(1, &b->rbo_depth_nm);
        if (b->fbo_res) glDeleteFramebuffers(1, &b->fbo_res);
        b->rbo_depth_nm = 0;
        b->fbo_res = 0;
        glDeleteFramebuffers(2, b->fbo_glow);
        glDeleteTextures(2, b->tex_glow);
    }
    if (b->fbo_ldr) {
        glDeleteFramebuffers(1, &b->fbo_ldr);
        glDeleteTextures(1, &b->tex_ldr);
    }
    b->scene_ldr = cvar_get("r_scene_ldr") > 0.5f;
    // Scene color: RGBA16F (HDR) or RGBA8 (low-end — nobody needs HDR)
    b->tex = make_scene_tex(vw, vh, b->scene_ldr);
    glGenFramebuffers(1, &b->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, b->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, b->tex, 0);
    glGenTextures(1, &b->depth);
    glBindTexture(GL_TEXTURE_2D, b->depth);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, vw, vh, 0,
                 GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, NULL);
    // Store scene depth in an RBO rather than a texture attachment: rendering
    // into a depth texture strips the driver of HiZ optimizations, and grass
    // redraw (giant overdraw with discard) slowed THREEFOLD (4.4 -> 1.5 ms
    // at 1080p). RBO is the fast path; for post effects (GI/SSR/fog)
    // depth is copied into a texture with a single blit in bloom_resolve.
    glGenRenderbuffers(1, &b->rbo_depth_nm);
    glBindRenderbuffer(GL_RENDERBUFFER, b->rbo_depth_nm);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, vw, vh);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER, b->rbo_depth_nm);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        SDL_Log("Bloom FBO incomplete (%dx%d)!", vw, vh);
    // Resolve-FBO: same color texture + depth texture — depth (and the MSAA color
    // resolve) is blitted here before the post passes.
    glGenFramebuffers(1, &b->fbo_res);
    glBindFramebuffer(GL_FRAMEBUFFER, b->fbo_res);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, b->tex, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                           GL_TEXTURE_2D, b->depth, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    // LDR copy for present: the composite (grade) renders at scene scale,
    // to screen — a hardware blit (cheaper than a shader fullscreen pass)
    glGenFramebuffers(1, &b->fbo_ldr);
    glBindFramebuffer(GL_FRAMEBUFFER, b->fbo_ldr);
    b->tex_ldr = make_ldr_tex(vw, vh);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, b->tex_ldr, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    make_fbo(&b->fbo_glow[0], &b->tex_glow[0], NULL, gw, gh, 0);
    make_fbo(&b->fbo_glow[1], &b->tex_glow[1], NULL, gw, gh, 0);
    // MSAA target for the new size (or for a changed r_msaa)
    msaa_create(b, msaa_wanted());
}

GLuint bloom_scene_fbo(const BloomParams *b)
{
    return b->msaa ? b->fbo_ms : b->fbo;
}

GLuint bloom_scene_tex(const BloomParams *b)
{
    return b->tex;
}

GLuint bloom_scene_depth(const BloomParams *b)
{
    return b->depth;
}

void bloom_render(BloomParams *b, GLuint scene_tex, GLuint target_fbo,
                  int out_w, int out_h)
{
    if (!b->fbo) return;
    // Settings from cvars (menu/console): applied every frame.
    // r_bloom = 0 — bloom off: bright pixels and blur are NOT computed,
    // only the composite is drawn (scene as-is, strength = 0).
    b->threshold = cvar_get("r_bloom_threshold");
    b->intensity = cvar_get("r_bloom_intensity");
    float strength = cvar_get("r_bloom_strength");
    if (cvar_get("r_bloom") < 0.5f) strength = 0.0f;
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);   // the HUD may have left blending on — postprocessing writes fresh
    glBindVertexArray(b->vao);

    if (strength > 0.0f) {
        // 1) bright scene pixels -> glow0 (half resolution)
        glBindFramebuffer(GL_FRAMEBUFFER, b->fbo_glow[0]);
        glViewport(0, 0, b->gw, b->gh);
        glUseProgram(b->prog_bright);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, scene_tex);
        glUniform1i(b->u_btex, 0);
        glUniform1f(b->u_threshold, b->threshold);
        glUniform1f(b->u_intensity, b->intensity);
        glDrawArrays(GL_TRIANGLES, 0, 3);

        // 2) Gaussian: H (glow0 -> glow1), V (glow1 -> glow0)
        glUseProgram(b->prog_blur);
        glUniform1i(b->u_blurtex, 0);
        glUniform2f(b->u_texel, 1.0f / b->gw, 1.0f / b->gh);
        glBindFramebuffer(GL_FRAMEBUFFER, b->fbo_glow[1]);
        glUniform2f(b->u_dir, 1.0f, 0.0f);
        glBindTexture(GL_TEXTURE_2D, b->tex_glow[0]);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindFramebuffer(GL_FRAMEBUFFER, b->fbo_glow[0]);
        glUniform2f(b->u_dir, 0.0f, 1.0f);
        glBindTexture(GL_TEXTURE_2D, b->tex_glow[1]);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }

    // 3) composite: scene + glow into the target FBO (0 = screen, otherwise — the fog FBO).
    // With strength <= 0 (bloom off) glow is multiplied by zero — the pass
    // acts as a copy of the scene to screen (one quad instead of four).
    glBindFramebuffer(GL_FRAMEBUFFER, target_fbo);
    // Size of the TARGET: the screen is native (scene upscale r_render_scale happens here),
    // the fog fbo — in scene scale. b->vw here would be wrong on screen.
    glViewport(0, 0, out_w, out_h);
    glUseProgram(b->prog_composite);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, scene_tex);
    glUniform1i(b->u_ctex, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, b->tex_glow[0]);
    glUniform1i(b->u_glowtex, 1);
    glUniform1f(b->u_strength, strength);
    // HDR exposure — from cvar r_exposure
    if (b->u_exposure >= 0)
        glUniform1f(b->u_exposure, cvar_get("r_exposure"));
    // Gamma and color correction (every frame, from cvars)
    if (b->u_gamma >= 0)
        glUniform1f(b->u_gamma, cvar_get("r_gamma"));
    if (b->u_sat >= 0)
        glUniform1f(b->u_sat, cvar_get("r_saturation"));
    if (b->u_contrast >= 0)
        glUniform1f(b->u_contrast, cvar_get("r_contrast"));
    // Wetness (rain): lowers saturation in the composite
    if (b->u_wetness >= 0)
        glUniform1f(b->u_wetness, fx_wetness());
    // Underwater effects (the renderer updates the state via bloom_set_underwater)
    if (b->u_underwater >= 0) {
        glUniform1f(b->u_underwater, s_underwater);
        glUniform3f(b->u_watertint, s_water_tint[0], s_water_tint[1],
                    s_water_tint[2]);
        glUniform1f(b->u_time, s_under_time);
    }
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

// Present path (no fog): grade+tonemapping at scene scale into RGBA8,
// then a hardware blit upscale to screen — on weak iGPUs a shader-based
// fullscreen upscale costs noticeably more than a fixed blit.
void bloom_present(BloomParams *b, GLuint scene_tex, int out_w, int out_h)
{
    if (!b->fbo_ldr) return;
    GLint prev_fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, b->fbo_ldr);
    bloom_render(b, scene_tex, b->fbo_ldr, b->vw, b->vh);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, b->fbo_ldr);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    glDisable(GL_SCISSOR_TEST);            // blit respects the scissor — disable it
    glBlitFramebuffer(0, 0, b->vw, b->vh, 0, 0, out_w, out_h,
                      GL_COLOR_BUFFER_BIT, GL_LINEAR);
    if (scissor) glEnable(GL_SCISSOR_TEST);
    // HUD/crosshair are drawn next: restore the TARGET viewport (native) and FBO
    glViewport(0, 0, out_w, out_h);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fbo);
}

void bloom_cleanup(BloomParams *b)
{
    if (s_bloom_p == b) s_bloom_p = NULL;
    resources_program_release(b->prog_bright);
    resources_program_release(b->prog_blur);
    resources_program_release(b->prog_composite);
    glDeleteBuffers(1, &b->vbo);
    glDeleteVertexArrays(1, &b->vao);
    if (b->fbo) {
        glDeleteFramebuffers(1, &b->fbo);
        glDeleteTextures(1, &b->tex);
        glDeleteTextures(1, &b->depth);
        if (b->rbo_depth_nm) glDeleteRenderbuffers(1, &b->rbo_depth_nm);
        if (b->fbo_res) glDeleteFramebuffers(1, &b->fbo_res);
        b->rbo_depth_nm = 0;
        b->fbo_res = 0;
        glDeleteFramebuffers(2, b->fbo_glow);
        glDeleteTextures(2, b->tex_glow);
        b->fbo = 0;
    }
    if (b->fbo_ldr) {
        glDeleteFramebuffers(1, &b->fbo_ldr);
        glDeleteTextures(1, &b->tex_ldr);
        b->fbo_ldr = 0;
    }
    msaa_free(b);
}
