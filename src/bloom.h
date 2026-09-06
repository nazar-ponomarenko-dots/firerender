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
    GLuint fbo_ms;             // MSAA-сцена: мультисэмпл-FBO (r_msaa > 0)
    GLuint rbo_color, rbo_depth;   // его цвет/глубина (мультисэмпл-RBO)
    int msaa;                  // активные сэмплы (0 = MSAA выключен)
    GLuint fbo_ldr, tex_ldr;   // LDR-копия сцены для present (bloom_present)
    int scene_ldr;             // сцена сразу в RGBA8 (картошка: HDR некому)
    GLuint fbo_glow[2];        // ping-pong буферы glow (половинный размер)
    GLuint tex_glow[2];
    GLuint vao, vbo;           // полноэкранный треугольник
    GLuint prog_bright, prog_blur, prog_composite;
    GLint u_btex, u_threshold, u_intensity;
    GLint u_blurtex, u_dir, u_texel;
    GLint u_ctex, u_glowtex, u_strength;
    GLint u_exposure, u_gamma, u_sat, u_contrast, u_wetness;
    GLint u_underwater, u_watertint, u_time;     // подводные эффекты (composite.fs)
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

// FBO, в который main.c рисует 3D-сцену (с глубиной). При включённом MSAA
// (cvar r_msaa) — мультисэмпл-FBO: сцена рисуется в него, затем
// bloom_resolve разворачивает (resolve) цвет и глубину в обычные текстуры.
GLuint bloom_scene_fbo(const BloomParams *b);

// Пересоздаёт MSAA-таргет, если cvar r_msaa изменился (вызывать ДО привязки
// FBO сцены в начале кадра). Живое применение настройки без перезапуска.
void bloom_msaa_sync(BloomParams *b);

// Разворачивает мультисэмпл-сцену в обычные текстуры (цвет + глубина) для
// постобработки (GI/SSR/туман читают их). Без MSAA — ничего не делает.
// Вызывать ПОСЛЕ отрисовки 3D-сцены, ДО всех пост-проходов.
void bloom_resolve(BloomParams *b);

// Текстура и глубина сцены (для объёмного тумана — voxfog.c).
GLuint bloom_scene_tex(const BloomParams *b);
GLuint bloom_scene_depth(const BloomParams *b);

// Яркие пиксели -> блюр -> композит на экран. Вызывается после отрисовки
// сцены в FBO, ДО миникарты и HUD. scene_tex — текстура сцены: обычная
// или уже с объёмным туманом (voxfog). target_fbo — куда писать композит:
// 0 = экран; иначе FBO (voxfog подставляет свой, чтобы туман лёг ПОВЕРХ
// bloom'а и сам не светился).
void bloom_render(BloomParams *b, GLuint scene_tex, GLuint target_fbo,
                  int out_w, int out_h);
void bloom_present(BloomParams *b, GLuint scene_tex, int out_w, int out_h);

// Освобождает GL-ресурсы.
void bloom_cleanup(BloomParams *b);

// Подводные эффекты (composite.fs): amount 0..1 (плавный вход/выход из воды),
// tint — тон водоёма, time — время сцены (анимация волн искажения).
void bloom_set_underwater(float amount, const float tint[3], float time);

#endif