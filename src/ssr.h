#ifndef SSR_H
#define SSR_H
#include "gl_loader.h"
#include "render_ctx.h"

typedef struct {
    GLuint fbo[2], tex[2];     // ping-pong: scene+reflection + previous frame (anti-flicker)
    int cur, last, frames;     // cur — where we write, last — where they read from, frames — counter
    GLuint vao, vbo;           // full-screen triangle
    GLuint prog;
    GLint u_scene, u_depth, u_cube, u_wetmask, u_prev;
    GLint u_viewproj, u_invviewproj, u_prevviewproj, u_campos, u_texel, u_worldsize;
    GLint u_params, u_misc, u_debug, u_wetness, u_strength, u_blend;
    int vw, vh;
    float prev_vp[16];        // previous frame's viewproj (anti-flicker reprojection)
    int have_prev;            // prev_vp is valid
} SsrParams;

// Creates the FBO/shader. vw/vh — window size.
void ssr_init(SsrParams *s, int vw, int vh);
// Recreates the texture when the window size changes.
void ssr_resize(SsrParams *s, int vw, int vh);
// Scene+reflection. Reads scene_tex+scene_depth (units 0/1),
// writes into its own FBO (ssr_tex). Call AFTER the scene render, BEFORE bloom.
void ssr_render(SsrParams *s, GLuint scene_tex, GLuint scene_depth,
                const RendererContext *rc);
// Scene+reflection texture (instead of scene_tex for bloom/fog).
GLuint ssr_tex(const SsrParams *s);
void ssr_cleanup(SsrParams *s);

#endif
