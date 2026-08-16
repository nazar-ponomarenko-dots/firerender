#ifndef SSR_H
#define SSR_H
#include "gl_loader.h"
#include "render_ctx.h"

typedef struct {
    GLuint fbo[2], tex[2];     // ping-pong: сцена+отражение + прошлый кадр (антимерцание)
    int cur, last, frames;     // cur — куда пишем, last — откуда читают, frames — счётчик
    GLuint vao, vbo;           // полноэкранный треугольник
    GLuint prog;
    GLint u_scene, u_depth, u_cube, u_wetmask, u_prev;
    GLint u_viewproj, u_invviewproj, u_prevviewproj, u_campos, u_texel, u_worldsize;
    GLint u_params, u_misc, u_debug, u_wetness, u_strength, u_blend;
    int vw, vh;
    float prev_vp[16];        // viewproj прошлого кадра (репроекция антимерцания)
    int have_prev;            // prev_vp валиден
} SsrParams;

// Создаёт FBO/шейдер. vw/vh — размер окна.
void ssr_init(SsrParams *s, int vw, int vh);
// Пересоздаёт текстуру при изменении размера окна.
void ssr_resize(SsrParams *s, int vw, int vh);
// Сцена+отражение. Читает scene_tex+scene_depth (единицы 0/1),
// пишет в свой FBO (ssr_tex). Вызывать ПОСЛЕ рендера сцены, ДО bloom.
void ssr_render(SsrParams *s, GLuint scene_tex, GLuint scene_depth,
                const RendererContext *rc);
// Текстура сцены+отражения (вместо scene_tex для bloom/тумана).
GLuint ssr_tex(const SsrParams *s);
void ssr_cleanup(SsrParams *s);

#endif
