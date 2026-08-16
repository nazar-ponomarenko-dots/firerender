// ============================================================================
//  bloom.h — постобработка glow (bloom)
// ----------------------------------------------------------------------------
//  Сцена рисуется в FBO (bloom_scene_fbo), потом bloom_render:
//    1) яркие пиксели вырезаются (bright, мягкий порог);
//    2) размываются на половинном разрешении (гаусс 11 тапов, H+V);
//    3) складываются со сценой на экран (композит).
//  Миникарта и HUD рисуются ПОСЛЕ bloom_render — они не светятся.
// ============================================================================
#ifndef BLOOM_H
#define BLOOM_H
#include "gl_loader.h"

typedef struct {
    GLuint fbo, tex;           // сцена: FBO + цвет (полный экран)
    GLuint depth;              // буфер глубины сцены
    GLuint fbo_glow[2];        // ping-pong буферы glow (половинный размер)
    GLuint tex_glow[2];
    GLuint vao, vbo;           // полноэкранный треугольник
    GLuint prog_bright, prog_blur, prog_composite;
    GLint u_btex, u_threshold, u_intensity;
    GLint u_blurtex, u_dir, u_texel;
    GLint u_ctex, u_glowtex, u_strength;
    GLint u_exposure;
    int vw, vh;                // размер сцены (пиксели)
    int gw, gh;                // размер glow (половина)
    float threshold;           // порог яркости (0..1)
    float intensity;           // множитель ярких пикселей
    float strength;            // сила свечения в композите
} BloomParams;

// Создаёт FBO и шейдеры постобработки. vw/vh — размер окна в пикселях.
void bloom_init(BloomParams *b, int vw, int vh);

// Пересоздаёт текстуры при изменении размера окна.
void bloom_resize(BloomParams *b, int vw, int vh);

// FBO, в который main.c рисует 3D-сцену (с глубиной).
GLuint bloom_scene_fbo(const BloomParams *b);

// Текстура и глубина сцены (для объёмного тумана — voxfog.c).
GLuint bloom_scene_tex(const BloomParams *b);
GLuint bloom_scene_depth(const BloomParams *b);

// Яркие пиксели -> блюр -> композит на экран. Вызывается после отрисовки
// сцены в FBO, ДО миникарты и HUD. scene_tex — текстура сцены: обычная
// или уже с объёмным туманом (voxfog). target_fbo — куда писать композит:
// 0 = экран; иначе FBO (voxfog подставляет свой, чтобы туман лёг ПОВЕРХ
// bloom'а и сам не светился).
void bloom_render(BloomParams *b, GLuint scene_tex, GLuint target_fbo);

// Освобождает GL-ресурсы.
void bloom_cleanup(BloomParams *b);

#endif