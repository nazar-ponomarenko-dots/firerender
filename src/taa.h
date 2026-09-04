// ============================================================================
//  taa.h — временное сглаживание (TAA)
// ----------------------------------------------------------------------------
//  Кадр рендерится с субпиксельным джиттером проекции (8-фазный Halton),
//  taa_render совмещает его с историей (перепроекция по глубине + variance
//  clamp) — края геометрии гладкие, временные шумы (джиттер тумана, GI)
//  усредняются. Работает поверх MSAA: сначала resolve, потом TAA.
//
//  Место в кадре (renderer_frame):
//      3D-сцена (MSAA-FBO) -> resolve -> [TAA] -> GI -> SSR -> bloom -> HUD
//  Джиттер ставит taa_jitter ПРЯМО ПЕРЕД render_3d (после кубомапа, чтобы
//  грани отражений остались без сдвига).
//
//  Cvar: r_taa (вкл/выкл).
// ============================================================================
#ifndef TAA_H
#define TAA_H
#include "gl_loader.h"
#include "render_ctx.h"

typedef struct {
    GLuint prog;
    GLuint vao, vbo;            // полноэкранный треугольник
    GLuint fbo_hist[2], tex_hist[2];   // ping-pong истории (полный размер)
    int cur;                    // текущий буфер истории
    int vw, vh;
    float prev_vp[16];          // viewproj прошлого кадра (с его джиттером)
    int have_prev;
    int frames;
    GLint t_scene, t_depth, t_hist, t_invvp, t_prevvp, t_campos, t_texel,
          t_z, t_blend;
} TaaParams;

// Создаёт программу, FBO истории, регистрирует cvar r_taa.
void taa_init(TaaParams *t, int vw, int vh);

// Пересоздаёт таргеты при смене размера окна (история сбрасывается).
void taa_resize(TaaParams *t, int vw, int vh);

// Субпиксельный джиттер проекции текущего кадра: правит rc->viewproj
// (сдвиг в NDC, одинаковый для всех глубин) и запоминает фазу. Вызывать
// ПЕРЕД render_3d, только если TAA включён.
void taa_jitter(TaaParams *t, RendererContext *rc);

// Совмещает текущий кадр с историей. Возвращает текстуру результата
// (или scene_tex без изменений, если TAA выключен).
GLuint taa_render(TaaParams *t, GLuint scene_tex, GLuint scene_depth,
                  const RendererContext *rc);

// 1, если TAA включён (для HUD/профайлера).
int taa_is_on(void);

void taa_cleanup(TaaParams *t);

#endif
