// ============================================================================
//  gi.h — dynamic global illumination (Lumen analogue)
// ----------------------------------------------------------------------------
//  Screen-space diffuse GI + ambient occlusion computed by RAY-TRACING over
//  the depth buffer, with temporal accumulation and bilateral denoising.
//  Works without precomputation: light changes — GI changes in the same frame
//  (like Lumen in UE5, but screen-space, without a world voxel/SDF cache).
//
//  Place in the frame (see renderer_frame):
//      3D scene (FBO) -> [GI] -> SSR -> bloom/composite -> fog -> HUD
//  GI runs before SSR: reflections must see the already-lit scene.
//
//  Pipeline:
//    1) gi.fs        — ray tracing (half resolution, RGBA16F: rgb = light,
//                      a = occlusion), contact refinement by interval bisection,
//                      cosine on the receiving side, distance-based LOD,
//                      temporal accumulation with history clamping;
//                      RAY MISS = light of the REAL sky (sky.c panorama, the
//                      same mapping as in sky.fs): shadows get lit by the sky
//                      color (blue zenith / warm sunset / gray overcast) —
//                      the main "Lumen-ish" ingredient. Below the horizon —
//                      ground bounce within the frame. All hit light (including
//                      scene lamps/spotlights) is re-reflected with albedo < 1;
//    2) gi_blur.fs   — bilateral denoise in 2 passes (H, V) weighted by the
//                      tangent plane: combines different ray sets of neighboring
//                      pixels (IGN) without leaking across edges;
//                      the radius grows with distance (noise is coarser far away);
//    3) gi_apply.fs  — joint bilateral upsample to full resolution and
//                      application to the scene; the cap on the addition is by
//                      the LOCAL brightness of the neighborhood, not of a single
//                      pixel: a lamp reflection in shadow is legal, global
//                      glow is not.
//
//  Cvars: r_gi, r_gi_strength, r_gi_ao, r_gi_rays, r_gi_steps, r_gi_radius,
//  r_gi_ao_radius, r_gi_sky, r_gi_skylight, r_gi_clamp, r_gi_bounce,
//  r_gi_reflect, r_gi_spread, r_gi_temporal, r_gi_blur, r_gi_scale,
//  r_gi_debug.
// ============================================================================
#ifndef GI_H
#define GI_H
#include "gl_loader.h"
#include "render_ctx.h"

typedef struct {
    GLuint prog_trace, prog_blur, prog_apply;
    GLuint vao, vbo;                 // full-screen triangle
    // Tracing + accumulation: ping-pong (write to one, read the previous)
    GLuint fbo_gi[2], tex_gi[2];
    int cur;                         // index of the current buffer
    // Denoise: intermediate target of the same size
    GLuint fbo_tmp, tex_tmp;
    // Final scene with GI (full size) — handed on to SSR/bloom
    GLuint fbo_out, tex_out;
    int gw, gh;                      // GI target size (half frame)
    int vw, vh;                      // screen size
    float prev_vp[16];               // previous frame's viewproj (reprojection)
    int have_prev;
    int frames;
    // uniform handles
    GLint t_scene, t_depth, t_prev, t_viewproj, t_invviewproj, t_prevviewproj;
    GLint t_campos, t_texel, t_params, t_params2, t_params3;
    GLint t_groundcolor, t_snowcover, t_clamp, t_blend;
    GLint t_sky, t_skytint, t_skyfog, t_skylight;   // sky on ray misses
    GLint b_gi, b_depth, b_invviewproj, b_campos, b_texel, b_dir, b_z, b_radius;
    GLint a_scene, a_gi, a_depth, a_gitexel, a_z, a_strength, a_ao, a_debug;
} GiParams;

// Creates programs and FBOs and registers cvars. vw/vh — window size.
void gi_init(GiParams *g, int vw, int vh);

// Recreates targets when the window size (or r_gi_scale) changes.
void gi_resize(GiParams *g, int vw, int vh);

// Computes GI and applies it to the scene. Returns the result texture
// (or scene_tex unchanged if GI is disabled/unavailable).
GLuint gi_render(GiParams *g, GLuint scene_tex, GLuint scene_depth,
                 const RendererContext *rc);

// 1 if GI is enabled via cvar and ready to render (for the profiler/HUD).
int gi_is_on(void);

void gi_cleanup(GiParams *g);

#endif
