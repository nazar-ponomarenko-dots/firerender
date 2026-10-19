// ============================================================================
//  bloom.h — post-processing glow (bloom)
// ----------------------------------------------------------------------------
//  The scene is drawn into an FBO (bloom_scene_fbo), then bloom_render:
//    1) bright pixels are extracted (bright pass, soft threshold);
//    2) blurred at half resolution (11-tap Gaussian, H+V);
//    3) composited with the scene onto the screen.
//  The minimap and HUD are drawn AFTER bloom_render — they do not glow.
// ============================================================================
#ifndef BLOOM_H
#define BLOOM_H
#include "gl_loader.h"

typedef struct {
    GLuint fbo, tex;           // scene: FBO + color (full screen)
    GLuint depth;              // scene depth buffer (texture for post-effects)
    GLuint rbo_depth_nm;       // scene depth WITHOUT MSAA: RBO attachment
    GLuint fbo_res;            // resolve-FBO (tex + depth texture) for the blit
    GLuint fbo_ms;             // MSAA scene: multisample FBO (r_msaa > 0)
    GLuint rbo_color, rbo_depth;   // its color/depth (multisample RBO)
    int msaa;                  // active samples (0 = MSAA disabled)
    int msaa_failed;           // unsupported request (don't retry every frame)
    int msaa_failed_ldr;
    GLuint fbo_ldr, tex_ldr;   // LDR copy of the scene for present (bloom_present)
    int scene_ldr;             // scene goes straight to RGBA8 (low-end: no HDR support)
    GLuint fbo_glow[2];        // glow ping-pong buffers (half size)
    GLuint tex_glow[2];
    GLuint vao, vbo;           // full-screen triangle
    GLuint prog_bright, prog_blur, prog_composite;
    GLint u_btex, u_threshold, u_intensity;
    GLint u_blurtex, u_dir, u_texel;
    GLint u_ctex, u_glowtex, u_strength;
    GLint u_exposure, u_gamma, u_sat, u_contrast, u_wetness;
    GLint u_underwater, u_watertint, u_time;     // underwater effects (composite.fs)
    int vw, vh;                // scene size (pixels)
    int gw, gh;                // glow size (half)
    float threshold;           // brightness threshold (0..1)
    float intensity;           // multiplier for bright pixels
    float strength;            // glow strength in the composite
} BloomParams;

// Creates the FBOs and post-processing shaders. vw/vh — window size in pixels.
void bloom_init(BloomParams *b, int vw, int vh);

// Recreates the textures when the window size changes.
void bloom_resize(BloomParams *b, int vw, int vh);

// The FBO into which main.c draws the 3D scene (with depth). With MSAA enabled
// (cvar r_msaa) — a multisample FBO: the scene is drawn into it, then
// bloom_resolve resolves the color and depth into plain textures.
GLuint bloom_scene_fbo(const BloomParams *b);

// Recreates the MSAA target if the cvar r_msaa changed (call BEFORE binding
// the scene FBO at the start of the frame). Applies the setting live, no restart.
void bloom_msaa_sync(BloomParams *b);

// Current sample count of the scene's MSAA target (0 — no MSAA). For
// alpha-to-coverage: classic MSAA does not smooth alpha-cutout edges (discard
// in the shader); drawing modules enable GL_SAMPLE_ALPHA_TO_COVERAGE when MSAA is active.
int bloom_msaa_active(void);

// Resolves the multisample scene into plain textures (color + depth) for
// post-processing (GI/SSR/fog read them). Without MSAA — does nothing.
// Call AFTER drawing the 3D scene, BEFORE all post passes.
// want_depth: whether the resolved depth is needed this frame. In the low-end
// preset no consumer (GI/SSR/voxfog) is active — then the depth copy from the
// RBO into a texture is skipped (a wasted full-screen blit). The MSAA path
// always resolves color+depth (medium/ULTRA presets stay bit-for-bit unchanged).
void bloom_resolve(BloomParams *b, int want_depth);

// Scene texture and depth (for volumetric fog — voxfog.c).
GLuint bloom_scene_tex(const BloomParams *b);
GLuint bloom_scene_depth(const BloomParams *b);

// Bright pixels -> blur -> composite onto screen. Called after the scene is
// drawn into the FBO, BEFORE the minimap and HUD. scene_tex — the scene
// texture: plain or already with volumetric fog (voxfog). target_fbo — where
// to write the composite: 0 = screen; otherwise an FBO (voxfog substitutes its
// own so the fog lands ON TOP of the bloom and does not glow itself).
void bloom_render(BloomParams *b, GLuint scene_tex, GLuint target_fbo,
                  int out_w, int out_h);
void bloom_present(BloomParams *b, GLuint scene_tex, int out_w, int out_h);

// Frees the GL resources.
void bloom_cleanup(BloomParams *b);

// Underwater effects (composite.fs): amount 0..1 (smooth entering/leaving the
// water), tint — the water body's tint, time — scene time (distortion wave animation).
void bloom_set_underwater(float amount, const float tint[3], float time);

#endif