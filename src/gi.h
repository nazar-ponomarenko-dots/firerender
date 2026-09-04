// ============================================================================
//  gi.h — динамическое глобальное освещение (аналог Lumen)
// ----------------------------------------------------------------------------
//  Экранное диффузное GI + ambient occlusion, посчитанные ТРАССИРОВКОЙ по
//  буферу глубины, с темпоральным накоплением и билатеральным денойзом.
//  Работает без предвычислений: свет меняется — GI меняется в том же кадре
//  (как Lumen в UE5, только в screen-space, без voxel/SDF-кэша мира).
//
//  Место в кадре (см. renderer_frame):
//      3D-сцена (FBO) -> [GI] -> SSR -> bloom/composite -> туман -> HUD
//  GI работает раньше SSR: отражения должны видеть уже подсвеченную сцену.
//
//  Пайплайн:
//    1) gi.fs        — трассировка лучей (полразрешения, RGBA16F: rgb = свет,
//                      a = затенение), уточнение контакта делением интервала,
//                      косинус на принимающей стороне, LOD по дистанции,
//                      темпоральное накопление с клампом истории;
//    2) gi_blur.fs   — билатеральный денойз в 2 прохода (H, V) с весом по
//                      касательной плоскости: складывает разные наборы лучей
//                      соседних пикселей (IGN), не протекая через изломы;
//    3) gi_apply.fs  — joint bilateral upsample на полное разрешение и
//                      наложение на сцену.
//
//  Cvar'ы: r_gi, r_gi_strength, r_gi_ao, r_gi_rays, r_gi_steps, r_gi_radius,
//  r_gi_ao_radius, r_gi_sky, r_gi_clamp, r_gi_temporal, r_gi_blur,
//  r_gi_scale, r_gi_debug.
// ============================================================================
#ifndef GI_H
#define GI_H
#include "gl_loader.h"
#include "render_ctx.h"

typedef struct {
    GLuint prog_trace, prog_blur, prog_apply;
    GLuint vao, vbo;                 // полноэкранный треугольник
    // Трассировка + накопление: ping-pong (пишем в один, читаем прошлый)
    GLuint fbo_gi[2], tex_gi[2];
    int cur;                         // индекс текущего буфера
    // Денойз: промежуточный таргет того же размера
    GLuint fbo_tmp, tex_tmp;
    // Итоговая сцена с GI (полный размер) — её отдаём дальше в SSR/bloom
    GLuint fbo_out, tex_out;
    int gw, gh;                      // размер GI-таргета (полкадра)
    int vw, vh;                      // размер экрана
    float prev_vp[16];               // viewproj прошлого кадра (репроекция)
    int have_prev;
    int frames;
    // uniform-хендлы
    GLint t_scene, t_depth, t_prev, t_viewproj, t_invviewproj, t_prevviewproj;
    GLint t_campos, t_texel, t_params, t_params2, t_params3;
    GLint t_groundcolor, t_clamp, t_blend;
    GLint b_gi, b_depth, b_invviewproj, b_campos, b_texel, b_dir, b_z, b_radius;
    GLint a_scene, a_gi, a_depth, a_gitexel, a_z, a_strength, a_ao, a_debug;
} GiParams;

// Создаёт программы, FBO и регистрирует cvar'ы. vw/vh — размер окна.
void gi_init(GiParams *g, int vw, int vh);

// Пересоздаёт таргеты при смене размера окна (или r_gi_scale).
void gi_resize(GiParams *g, int vw, int vh);

// Считает GI и накладывает его на сцену. Возвращает текстуру результата
// (или scene_tex без изменений, если GI выключен/недоступен).
GLuint gi_render(GiParams *g, GLuint scene_tex, GLuint scene_depth,
                 const RendererContext *rc);

// 1, если GI включён cvar'ом и готов рисовать (для профайлера/HUD).
int gi_is_on(void);

void gi_cleanup(GiParams *g);

#endif
