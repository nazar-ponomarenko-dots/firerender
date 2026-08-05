// ============================================================================
//  render_ctx.h — единый рендер-контекст и общий uniform-набор сцены
// ----------------------------------------------------------------------------
//  Раньше каждый модуль (terrain/vegetation/objects/sky...) сам держал
//  CameraParams* и SunParams* и сам слал кадровые uniform'ы (uViewProj,
//  uCamPos, свет, туман, каскады теней) — с дублированием хендлов и кода.
//
//  Теперь всё кадровое состояние собирается ОДИН раз за кадр в
//  RendererContext (заполняет renderer.c из Game: камера, свет, туман,
//  тени; плюс флаг прохода pass для теневого рендера). Модули сцены
//  получают const RendererContext* при создании узла и читают его в
//  render — никаких cam/sun в параметрах.
//
//  SceneUniforms — хендлы ОБЩЕГО набора uniform'ов (есть во всех шейдерах
//  сцены): ищутся один раз при создании программы (необязательные = -1),
//  шлются одним вызовом scene_uniforms_bind.
// ============================================================================
#ifndef RENDER_CTX_H
#define RENDER_CTX_H
#include "gl_loader.h"
#include "shadow.h"      // SHADOW_CASCADES
#include "sun.h"         // SUN_MAX_LIGHTS

// --- Точечные и прожекторные источники (лимиты uniform-массивов) ---------------
#define SCENE_MAX_POINT 8
#define SCENE_MAX_SPOT  4

// --- Проход рендера ------------------------------------------------------------
enum {
    RP_COLOR = 0,   // обычный проход (в FBO сцены)
    RP_SHADOW,      // теневой проход (глубина в каскад) — см. shadow.c
};

// --- Кадровое состояние сцены (заполняет renderer.c каждый кадр) --------------
// Имя структуры совпадает с форвард-объявлением в shadow.h.
typedef struct RendererContext {
    // Камера
    float viewproj[16];      // вид * проекция (считает узел Camera)
    float cam_pos[3];        // позиция игрока
    float cam_fwd[3], cam_right[3], cam_up[3];   // базис взгляда
    float cam_yaw, cam_pitch;
    float cam_fov_deg, cam_aspect;
    float cam_vy;                // вертикальная скорость (HUD, физика ходьбы)
    int cam_fly;                 // режим полёта (ветер, HUD)
    float znear, zfar;
    // Свет: направленные источники (0 = солнце)
    float light_dir[SUN_MAX_LIGHTS][3];
    float light_color[SUN_MAX_LIGHTS][3];
    float light_intensity[SUN_MAX_LIGHTS];
    int light_count;
    // Точечные источники (map.json lights[]: point)
    float point_pos[SCENE_MAX_POINT][3];
    float point_color[SCENE_MAX_POINT][3];
    float point_intensity[SCENE_MAX_POINT];
    float point_range[SCENE_MAX_POINT];
    int point_count;
    // Прожекторы (spot): позиция + направление + полуугол конуса
    float spot_pos[SCENE_MAX_SPOT][3];
    float spot_dir[SCENE_MAX_SPOT][3];
    float spot_color[SCENE_MAX_SPOT][3];
    float spot_intensity[SCENE_MAX_SPOT];
    float spot_range[SCENE_MAX_SPOT];
    float spot_angle[SCENE_MAX_SPOT];
    int spot_count;
    // Цвета окружения (унифицированы для всех материалов сцены)
    float ambient[3], sky_color[3], ground_color[3];
    // Туман
    float fog_color[3], fog_start, fog_end;
    int fog_on;
    // Каскадные тени (матрицы/текстуры — из shadow.c)
    float shadow_matrix[SHADOW_CASCADES][16];
    GLuint shadow_tex[SHADOW_CASCADES];
    int shadow_on;                       // тени включены и солнце их даёт
    // Производные каскадов (bias/градиент из map.json: shadow.bias, bias_veg)
    float shadow_box[SHADOW_CASCADES];
    float shadow_depth[SHADOW_CASCADES];
    float shadow_bias[SHADOW_CASCADES];      // рельеф
    float shadow_grad[SHADOW_CASCADES];
    float shadow_bias_veg[SHADOW_CASCADES];  // растительность/объекты
    float shadow_grad_veg[SHADOW_CASCADES];
    // Теневой проход (заполняет shadow_render для каждого каскада)
    int pass;                            // RP_COLOR / RP_SHADOW
    float shadow_vp[16];                 // матрица света текущего каскада
    float shadow_box_cur;                // сторона бокса текущего каскада
    int shadow_cascade;                  // индекс каскада
    int shadow_dyn, shadow_static;       // рисовать динамику/статику
    int shadow_cull;                     // culling по боксу каскада
} RendererContext;

// --- Общий набор uniform-хендлов шейдеров сцены --------------------------------
typedef struct {
    GLint u_viewproj, u_campos;
    GLint u_lightdir, u_lightcolor, u_lightint, u_lightcount;
    GLint u_pointpos, u_pointcolor, u_pointint, u_pointrange, u_pointcount;
    GLint u_spotpos, u_spotdir, u_spotcolor, u_spotint, u_spotrange, u_spotangle, u_spotcount;
    GLint u_ambient, u_skycolor, u_groundcolor;
    GLint u_fogcolor, u_fogstart, u_fogend, u_fogon;
    GLint u_shadowmap[SHADOW_CASCADES], u_shadowmatrix[SHADOW_CASCADES];
    GLint u_shadowbias[SHADOW_CASCADES], u_shadowdepth[SHADOW_CASCADES];
    GLint u_shadowgrad[SHADOW_CASCADES], u_shadow_on;
} SceneUniforms;

// Ищет локации общего набора в программе (необязательные = -1). Теневые
// матрицы/bias/grad ищутся по ОБОИМ именам: рельеф объявляет uShadowMatrix0..3,
// растительность/объекты — массив uShadowMatrix[4].
void scene_uniforms_init(SceneUniforms *su, GLuint prog);

// Шлёт общий набор из контекста: виды, свет, окружение, туман, каскады
// (матрицы, текстуры на юнитах 3..6, bias/градиент — базовые, рельефные).
// Растительность/объекты переопределяют bias/градиент своими значениями
// (rc->shadow_bias_veg и т.п.) ПОСЛЕ вызова.
void scene_uniforms_bind(const SceneUniforms *su, const RendererContext *rc);

#endif
