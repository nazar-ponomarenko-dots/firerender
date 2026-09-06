// ============================================================================
//  voxfog.c — воксельный объёмный туман
// ----------------------------------------------------------------------------
//  Каскады: 3D-текстуры GL_R8 (плотность на метр) вокруг камеры, центры
//  привязаны к мировой сетке вокселей (пересчёт только при смене ячейки).
//  Плотность: дымка у земли + плотный тёплый туман над полем пшеницы.
//
//  Рендер — деферред: один полноэкранный проход читает цвет+глубину сцены,
//  реконструирует мировую точку, марширует луч к ней по каскадам (шаг
//  растёт с дистанцией, джиттер убирает полосы) и складывает туман.
//  ============================================================================
#include "voxfog.h"

#include "assets.h"
#include "cvars.h"
#include "gl_loader.h"
#include "mapdata.h"
#include "resources.h"
#include "shader.h"
#include "shadow.h"
#include "water.h"
#include "world.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static GLuint s_tex3d[VF_CASCADES];
static float  s_center[VF_CASCADES][3];
static const float s_box[VF_CASCADES] = { VF_BOX0, VF_BOX1, VF_BOX2 };
static const int   s_res[VF_CASCADES] = { VF_RES0, VF_RES1, VF_RES2 };
static int    s_on = 0;
static MapFog s_cfg;   // настройки из map/map.json (fog = { ... })
static const WorldState *s_world;   // рельеф и поле (плотность тумана)
static float  s_wcol[3] = { 0.05f, 0.15f, 0.22f };   // тон тумана над водой

static GLuint s_fbo = 0, s_tex = 0;   // приёмник композита bloom (полный экран)
static GLuint s_fbo_half = 0, s_tex_half = 0;  // марш тумана в полразмера
static GLuint s_prog_comp = 0;         // композит тумана на экране
static GLint u_comp_scene = -1, u_comp_fog = -1;
static GLint u_fogonly = -1, u_fogonly2d = -1;
static GLuint s_prog = 0, s_vao = 0, s_vbo = 0;
// Аналитический туман режима «Простой» (shaders/fog2d.fs): без 3D-текстур
// и марша — высота рельефа из карты высот в 4 точках луча, замкнутые
// интегралы. Один и тот же FBO/конвейер, что у объёмного.
static GLuint s_prog2d = 0;
static GLint u2d_invvp, u2d_campos, u2d_z, u2d_sundir, u2d_fogcol,
             u2d_fogcolsun, u2d_skycolor, u2d_glare, u2d_raystr, u2d_wetness,
             u2d_heightmap, u2d_worldsize, u2d_hmin, u2d_hmax, u2d_fogp;
static GLint u_scene, u_scenedepth, u_invviewproj, u_campos, u_znear, u_zfar;
static GLint u_tex3d[VF_CASCADES], u_center[VF_CASCADES];
static GLint u_box[VF_CASCADES], u_res[VF_CASCADES];
static GLint u_sundir, u_strength, u_fogmode;
static GLint u_fogcol, u_fogcolsun, u_glare, u_raystr;
static GLint u_wetness, u_skycolor;
static GLint u_heightmap, u_worldsize, u_hmin, u_hmax, u_time;
static GLint u_haze, u_hazeh;
static GLint u_noise;   // img/noise.png (unit 12) — неравномерность дымки
static GLint u_wcol;    // цвет тумана над водой (средний цвет водоёмов)
static GLuint s_noise_tex = 0;
static GLint u_shadowmap[SHADOW_CASCADES], u_shadowmatrix[SHADOW_CASCADES], u_shadowbias[SHADOW_CASCADES];
static GLint u_shadowgrad[SHADOW_CASCADES], u_shadowon;

// Режим тумана: 0 = выкл, 1 = объёмный (полное разрешение),
// 2 = «дешёвый» объёмный в полном разрешении: марш только по грубому каскаду
static int s_mode = 1;

int voxfog_is_on(void)          { return s_on; }
void voxfog_toggle(void)        { s_on = !s_on; }
void voxfog_set(int on)         { s_on = on; if (s_mode != 1) s_mode = on ? 1 : 0; }

void voxfog_set_mode(int mode)
{
    s_mode = mode;
    s_on = mode > 0;
}
int voxfog_mode(void)           { return s_mode; }

GLuint voxfog_texture(void)     { return s_tex; }
GLuint voxfog_fbo(void)         { return s_fbo; }

// Плотность тумана в точке мира (на метр пути) — из map.json.
// Возвращает базовую плотность (земля/поле) и пишет в *wout плотность
// тумана над водоёмами (уровень воды, цвет — из water.c).
static float vf_density(float x, float y, float z, float *wout)
{
    *wout = 0.0f;
    float h = sample_height(s_world, x, z);
    float up = y - h;
    if (up < -1.0f) return 0.0f;              // под рельефом — тумана нет
    float d = 0.0f;
    // лёгкая дымка у земли (низины, склоны) — лёгкая, не стена
    if (up < s_cfg.ground_height)
        d += s_cfg.density_ground * (1.0f - up / s_cfg.ground_height);
    // тёплый туман над полем пшеницы
    if (x >= s_world->field_x0 && x <= s_world->field_x0 + s_world->field_w &&
        z >= s_world->field_z0 && z <= s_world->field_z0 + s_world->field_d)
        if (up < s_cfg.field_height)
            d += s_cfg.density_field * (1.0f - up / s_cfg.field_height);
    // туман над водоёмом: слой у поверхности воды (цвет — из водоёма,
    // подаётся в шейдер отдельно). Тоньше слоя — плотнее.
    if (s_cfg.density_water > 0.0f && s_cfg.water_height > 0.1f) {
        float ws = water_surface_at(s_world, x, z);
        if (ws > -1e8f) {
            float uw = y - ws;
            if (uw > 0.0f && uw < s_cfg.water_height) {
                float dw = s_cfg.density_water * (1.0f - uw / s_cfg.water_height);
                *wout = dw * s_cfg.strength;
            }
        }
    }
    d *= s_cfg.strength;
    if (d > 0.2f) d = 0.2f;
    return d;
}

// Перестроить каскад c (центр закреплён к мировой сетке вокселей).
// Формат RG8: R = базовая плотность, G = плотность тумана над водой.
static void vf_rebuild(int c)
{
    float cell = s_box[c] / s_res[c];
    unsigned char *buf = malloc((size_t)s_res[c] * s_res[c] * s_res[c] * 2);
    if (!buf) return;
    const float *cen = s_center[c];
    for (int z = 0; z < s_res[c]; z++)
        for (int y = 0; y < s_res[c]; y++)
            for (int x = 0; x < s_res[c]; x++) {
                float wx = cen[0] + ((float)x + 0.5f - s_res[c] * 0.5f) * cell;
                float wy = cen[1] + ((float)y + 0.5f - s_res[c] * 0.5f) * cell;
                float wz = cen[2] + ((float)z + 0.5f - s_res[c] * 0.5f) * cell;
                float dw = 0.0f;
                float d = vf_density(wx, wy, wz, &dw);
                size_t k = (size_t)((z * s_res[c] + y) * s_res[c] + x) * 2;
                buf[k]     = (unsigned char)(d * 255.0f);
                buf[k + 1] = (unsigned char)(dw * 255.0f);
            }
    glBindTexture(GL_TEXTURE_3D, s_tex3d[c]);
    glTexSubImage3D(GL_TEXTURE_3D, 0, 0, 0, 0,
                    s_res[c], s_res[c], s_res[c],
                    GL_RG, GL_UNSIGNED_BYTE, buf);
    free(buf);
}

// Проги создаются СНАРУЖИ (init / reshade: refcount балансируется там).
// Здесь — только uniform'ы для ТЕКУЩЕГО id (s_prog).
static void voxfog_prog_init(void)
{
    if (!s_prog) return;
    u_scene      = glGetUniformLocation(s_prog, "uScene");
    u_scenedepth = glGetUniformLocation(s_prog, "uSceneDepth");
    u_invviewproj = glGetUniformLocation(s_prog, "uInvViewProj");
    u_campos     = glGetUniformLocation(s_prog, "uCamPos");
    u_znear      = glGetUniformLocation(s_prog, "uZNear");
    u_zfar       = glGetUniformLocation(s_prog, "uZFar");
    u_sundir     = glGetUniformLocation(s_prog, "uSunDir");
    u_strength   = glGetUniformLocation(s_prog, "uStrength");
    u_fogmode    = glGetUniformLocation(s_prog, "uFogMode");
    u_fogcol     = glGetUniformLocation(s_prog, "uFogColor");
    u_fogcolsun  = glGetUniformLocation(s_prog, "uFogColorSun");
    u_glare      = glGetUniformLocation(s_prog, "uGlare");
    u_raystr     = glGetUniformLocation(s_prog, "uRayStrength");
    u_wetness    = glGetUniformLocation(s_prog, "uWetness");
    u_skycolor   = glGetUniformLocation(s_prog, "uSkyColor");
    u_heightmap  = glGetUniformLocation(s_prog, "uHeightmap");
    u_worldsize  = glGetUniformLocation(s_prog, "uWorldSize");
    u_hmin       = glGetUniformLocation(s_prog, "uHeightMin");
    u_hmax       = glGetUniformLocation(s_prog, "uHeightMax");
    u_time       = glGetUniformLocation(s_prog, "uTime");
    u_haze       = glGetUniformLocation(s_prog, "uHaze");
    u_hazeh      = glGetUniformLocation(s_prog, "uHazeH");
    u_noise      = glGetUniformLocation(s_prog, "uNoise");
    u_wcol       = glGetUniformLocation(s_prog, "uWColor");
    for (int c = 0; c < VF_CASCADES; c++) {
        char name[64];
        snprintf(name, sizeof name, "uFogTex%d", c);
        u_tex3d[c] = glGetUniformLocation(s_prog, name);
        snprintf(name, sizeof name, "uFogCenter%d", c);
        u_center[c] = glGetUniformLocation(s_prog, name);
        snprintf(name, sizeof name, "uFogBox%d", c);
        u_box[c] = glGetUniformLocation(s_prog, name);
        snprintf(name, sizeof name, "uFogRes%d", c);
        u_res[c] = glGetUniformLocation(s_prog, name);
    }
    for (int c = 0; c < SHADOW_CASCADES; c++) {
        char name[64];
        snprintf(name, sizeof name, "uShadowMap%d", c);
        u_shadowmap[c] = glGetUniformLocation(s_prog, name);
        snprintf(name, sizeof name, "uShadowMatrix[%d]", c);
        u_shadowmatrix[c] = glGetUniformLocation(s_prog, name);
        snprintf(name, sizeof name, "uShadowBias[%d]", c);
        u_shadowbias[c] = glGetUniformLocation(s_prog, name);
        snprintf(name, sizeof name, "uShadowGrad[%d]", c);
        u_shadowgrad[c] = glGetUniformLocation(s_prog, name);
    }
    u_shadowon   = glGetUniformLocation(s_prog, "uShadowOn");
    u_fogonly    = glGetUniformLocation(s_prog, "uFogOnly");
    glUseProgram(s_prog);
    glUniform1i(u_scene, 0);
    glUniform1i(u_scenedepth, 1);
    glUniform1i(u_tex3d[0], 2);
    glUniform1i(u_tex3d[1], 3);
    glUniform1i(u_tex3d[2], 4);
    for (int c = 0; c < SHADOW_CASCADES; c++)
        glUniform1i(u_shadowmap[c], 5 + c);      // CSM на юнитах 5-9
    glUniform1i(u_heightmap, 10);                // карта высот (дымка у земли)
    glUniform1i(u_noise, 12);                    // img/noise.png (шум дымки)
    glUniform3f(u_wcol, s_wcol[0], s_wcol[1], s_wcol[2]);   // тон тумана над водой
    glUseProgram(0);
}

// Hot-reload: replace балансирует refcount (release старого + новая ссылка);
// 0 — пара не собралась, остаётся прежний id.
static void voxfog_reshade(void)
{
    GLuint prog = resources_program_replace(s_prog, "shaders/postfx.vs",
                                            "shaders/volumetric.fs");
    if (prog) s_prog = prog;
    GLuint p2 = resources_program_replace(s_prog2d, "shaders/postfx.vs",
                                          "shaders/fog2d.fs");
    if (p2) s_prog2d = p2;
    GLuint pc = resources_program_replace(s_prog_comp, "shaders/postfx.vs",
                                          "shaders/fog_comp.fs");
    if (pc) {
        s_prog_comp = pc;
        u_comp_scene = glGetUniformLocation(s_prog_comp, "uScene");
        u_comp_fog   = glGetUniformLocation(s_prog_comp, "uFog");
        if (s_prog_comp) {
            glUseProgram(s_prog_comp);
            glUniform1i(u_comp_scene, 0);
            glUniform1i(u_comp_fog, 1);
            glUseProgram(0);
        }
    }
    voxfog_prog_init();
}

void voxfog_init(const WorldState *w, int vw, int vh)
{
    s_world = w;
    mapdata_fog(&s_cfg);
    // Тон тумана над водоёмами: средний тон всех водоёмов карты (взвешенный
    // по радиусу/ширине, чтобы море не перекрашивало туман над лужей).
    {
        float sum[3] = { 0, 0, 0 }, tw = 0.0f;
        int nw = mapdata_water_count();
        for (int i = 0; i < nw; i++) {
            const MapWater *mw = mapdata_water(i);
            float wi = mw->type == WATER_RIVER ? mw->width
                                               : (mw->radius > 1.0f ? mw->radius : 1.0f);
            if (wi <= 0.0f) wi = 1.0f;
            for (int k = 0; k < 3; k++) sum[k] += mw->color[k] * wi;
            tw += wi;
        }
        if (tw > 0.0f) {
            for (int k = 0; k < 3; k++) s_wcol[k] = sum[k] / tw;
            // туман чуть светлее и мягче чистой воды — не стена, а испарение
            float l = 0.5f * (s_wcol[0] + s_wcol[1] + s_wcol[2]) / 3.0f + 0.25f;
            for (int k = 0; k < 3; k++) s_wcol[k] = s_wcol[k] * 0.6f + 0.4f * l;
        }
    }
    // Шум дымки: img/noise.png (REPEAT), общая текстура ресурсов
    if (!s_noise_tex)
        s_noise_tex = resources_texture("img/noise.png", 3, GL_REPEAT, 1);
    // Режим — из cvar'а r_fog_mode (config.cfg/меню/консоль), а НЕ дефолт
    // карты: sync_cvars в main.c срабатывает только на ИЗМЕНЕНИЕ cvar'а,
    // поэтому при старте с r_fog_mode 0 из config.cfg туман иначе остался
    // бы включённым до первого ручного переключения.
    int mode = (int)cvar_get("r_fog_mode");
    if (mode < 0 || mode > 2) mode = s_cfg.enabled ? 1 : 0;   // запасной вариант
    s_mode = mode;
    s_on = mode > 0;
    for (int c = 0; c < VF_CASCADES; c++) {
        glGenTextures(1, &s_tex3d[c]);
        glBindTexture(GL_TEXTURE_3D, s_tex3d[c]);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
        glTexImage3D(GL_TEXTURE_3D, 0, GL_RG8, s_res[c], s_res[c], s_res[c],
                     0, GL_RG, GL_UNSIGNED_BYTE, NULL);
        memset(s_center[c], 0, sizeof s_center[c]);
    }

    // Программа тумана (replace: old==0 после cleanup => plain-загрузка)
    s_prog = resources_program_replace(s_prog, "shaders/postfx.vs",
                                       "shaders/volumetric.fs");
    // Аналитический туман «Простого» режима
    s_prog2d = resources_program_replace(s_prog2d, "shaders/postfx.vs",
                                         "shaders/fog2d.fs");
    if (s_prog2d) {
        u2d_invvp     = glGetUniformLocation(s_prog2d, "uInvViewProj");
        u2d_campos    = glGetUniformLocation(s_prog2d, "uCamPos");
        u2d_z         = glGetUniformLocation(s_prog2d, "uZ");
        u2d_sundir    = glGetUniformLocation(s_prog2d, "uSunDir");
        u2d_fogcol    = glGetUniformLocation(s_prog2d, "uFogColor");
        u2d_fogcolsun = glGetUniformLocation(s_prog2d, "uFogColorSun");
        u2d_skycolor  = glGetUniformLocation(s_prog2d, "uSkyColor");
        u2d_glare     = glGetUniformLocation(s_prog2d, "uGlare");
        u2d_raystr    = glGetUniformLocation(s_prog2d, "uRayStrength");
        u2d_wetness   = glGetUniformLocation(s_prog2d, "uWetness");
        u2d_heightmap = glGetUniformLocation(s_prog2d, "uHeightmap");
        u2d_worldsize = glGetUniformLocation(s_prog2d, "uWorldSize");
        u2d_hmin      = glGetUniformLocation(s_prog2d, "uHeightMin");
        u2d_hmax      = glGetUniformLocation(s_prog2d, "uHeightMax");
        u2d_fogp      = glGetUniformLocation(s_prog2d, "uFogP");
        u_fogonly2d   = glGetUniformLocation(s_prog2d, "uFogOnly");
        glUseProgram(s_prog2d);
        glUniform1i(glGetUniformLocation(s_prog2d, "uScene"), 0);
        glUniform1i(glGetUniformLocation(s_prog2d, "uSceneDepth"), 1);
        glUniform1i(u2d_heightmap, 10);
        glUseProgram(0);
    }
    // Композит тумана на экран (после полразмерного марша)
    if (!s_prog_comp) {
        s_prog_comp = resources_program("shaders/postfx.vs",
                                        "shaders/fog_comp.fs");
        if (s_prog_comp) {
            u_comp_scene = glGetUniformLocation(s_prog_comp, "uScene");
            u_comp_fog   = glGetUniformLocation(s_prog_comp, "uFog");
            glUseProgram(s_prog_comp);
            glUniform1i(u_comp_scene, 0);
            glUniform1i(u_comp_fog, 1);
            glUseProgram(0);
        }
    }
    voxfog_prog_init();    // uniform'ы (для hot-reload)
    if (!s_prog) return;
    u_scenedepth = glGetUniformLocation(s_prog, "uSceneDepth");
    u_invviewproj = glGetUniformLocation(s_prog, "uInvViewProj");
    u_campos     = glGetUniformLocation(s_prog, "uCamPos");
    u_znear      = glGetUniformLocation(s_prog, "uZNear");
    u_zfar       = glGetUniformLocation(s_prog, "uZFar");
    u_sundir     = glGetUniformLocation(s_prog, "uSunDir");
    u_strength   = glGetUniformLocation(s_prog, "uStrength");
    u_fogmode    = glGetUniformLocation(s_prog, "uFogMode");
    u_fogcol     = glGetUniformLocation(s_prog, "uFogColor");
    u_fogcolsun  = glGetUniformLocation(s_prog, "uFogColorSun");
    u_glare      = glGetUniformLocation(s_prog, "uGlare");
    u_raystr     = glGetUniformLocation(s_prog, "uRayStrength");
    u_wetness    = glGetUniformLocation(s_prog, "uWetness");
    u_skycolor   = glGetUniformLocation(s_prog, "uSkyColor");
    u_heightmap  = glGetUniformLocation(s_prog, "uHeightmap");
    u_worldsize  = glGetUniformLocation(s_prog, "uWorldSize");
    u_hmin       = glGetUniformLocation(s_prog, "uHeightMin");
    u_hmax       = glGetUniformLocation(s_prog, "uHeightMax");
    u_time       = glGetUniformLocation(s_prog, "uTime");
    u_haze       = glGetUniformLocation(s_prog, "uHaze");
    u_hazeh      = glGetUniformLocation(s_prog, "uHazeH");
    u_noise      = glGetUniformLocation(s_prog, "uNoise");
    for (int c = 0; c < VF_CASCADES; c++) {
        char name[64];
        snprintf(name, sizeof name, "uFogTex%d", c);
        u_tex3d[c] = glGetUniformLocation(s_prog, name);
        snprintf(name, sizeof name, "uFogCenter%d", c);
        u_center[c] = glGetUniformLocation(s_prog, name);
        snprintf(name, sizeof name, "uFogBox%d", c);
        u_box[c] = glGetUniformLocation(s_prog, name);
        snprintf(name, sizeof name, "uFogRes%d", c);
        u_res[c] = glGetUniformLocation(s_prog, name);
    }
    for (int c = 0; c < SHADOW_CASCADES; c++) {
        char name[64];
        snprintf(name, sizeof name, "uShadowMap%d", c);
        u_shadowmap[c] = glGetUniformLocation(s_prog, name);
        snprintf(name, sizeof name, "uShadowMatrix[%d]", c);
        u_shadowmatrix[c] = glGetUniformLocation(s_prog, name);
        snprintf(name, sizeof name, "uShadowBias[%d]", c);
        u_shadowbias[c] = glGetUniformLocation(s_prog, name);
        snprintf(name, sizeof name, "uShadowGrad[%d]", c);
        u_shadowgrad[c] = glGetUniformLocation(s_prog, name);
    }
    u_shadowon   = glGetUniformLocation(s_prog, "uShadowOn");
    u_fogonly    = glGetUniformLocation(s_prog, "uFogOnly");
    glUseProgram(s_prog);
    glUniform1i(u_scene, 0);
    glUniform1i(u_scenedepth, 1);
    glUniform1i(u_tex3d[0], 2);
    glUniform1i(u_tex3d[1], 3);
    glUniform1i(u_tex3d[2], 4);
    for (int c = 0; c < SHADOW_CASCADES; c++)
        glUniform1i(u_shadowmap[c], 5 + c);      // CSM на юнитах 5-9
    glUniform1i(u_heightmap, 10);                // карта высот (дымка у земли)
    glUniform1i(u_noise, 12);                    // img/noise.png (шум дымки)
    glUseProgram(0);

    shader_on_reshade(voxfog_reshade);

    // Полноэкранный треугольник
    glGenVertexArrays(1, &s_vao);
    glBindVertexArray(s_vao);
    glGenBuffers(1, &s_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
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

    voxfog_resize(vw, vh);
}

void voxfog_resize(int vw, int vh)
{
    if (vw <= 0 || vh <= 0) return;
    if (s_fbo) {
        glDeleteFramebuffers(1, &s_fbo);
        glDeleteTextures(1, &s_tex);
    }
    if (s_fbo_half) {
        glDeleteFramebuffers(1, &s_fbo_half);
        glDeleteTextures(1, &s_tex_half);
        s_fbo_half = s_tex_half = 0;
    }
    if (s_prog_comp) {
        resources_program_release(s_prog_comp);
        s_prog_comp = 0;
    }
    if (s_fbo_half) {
        glDeleteFramebuffers(1, &s_fbo_half);
        glDeleteTextures(1, &s_tex_half);
    }
    // Марш тумана в ПОЛРАЗМЕРЕ: RGBA8 (цвет тумана + trans в альфе).
    // Гладкий туман билинейный апскейл не выдаёт, а фрагментов вчетверо меньше.
    glGenTextures(1, &s_tex_half);
    glBindTexture(GL_TEXTURE_2D, s_tex_half);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (vw + 1) / 2, (vh + 1) / 2, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers(1, &s_fbo_half);
    glBindFramebuffer(GL_FRAMEBUFFER, s_fbo_half);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, s_tex_half, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    // Полный экран: сюда bloom пишет композит, туман ложится поверх
    glGenTextures(1, &s_tex);
    glBindTexture(GL_TEXTURE_2D, s_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, vw, vh, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers(1, &s_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, s_tex, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void voxfog_update(const float cam_pos[3])
{
    if (!s_on) return;
    // Режим 2 (аналитический): 3D-каскады не нужны вовсе — и память, и
    // CPU-перестройки при движении камеры не тратятся.
    if (s_mode == 2) return;
    for (int c = 0; c < VF_CASCADES; c++) {
        float cell = s_box[c] / s_res[c];
        float nc[3];
        for (int k = 0; k < 3; k++)
            nc[k] = floorf(cam_pos[k] / cell) * cell + cell * 0.5f;
        if (nc[0] != s_center[c][0] || nc[1] != s_center[c][1] ||
            nc[2] != s_center[c][2]) {
            memcpy(s_center[c], nc, sizeof nc);
            vf_rebuild(c);
        }
    }
}

// Обратная матрица 4x4 (column-major) — для реконструкции мировой точки
static void mat4_invert(const float *m, float *o)
{
    float inv[16];
    inv[0]  =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15]
             + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4]  = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15]
             - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8]  =  m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15]
             + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14]
             - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1]  = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15]
             - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5]  =  m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15]
             + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9]  = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15]
             - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] =  m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14]
             + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2]  =  m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15]
             + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
    inv[6]  = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15]
             - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
    inv[10] =  m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15]
             + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14]
             - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
    inv[3]  = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11]
             - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
    inv[7]  =  m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11]
             + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11]
             - m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
    inv[15] =  m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10]
             + m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];
    float det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
    if (det == 0.0f) { memcpy(o, m, 16 * sizeof(float)); return; }
    det = 1.0f / det;
    for (int i = 0; i < 16; i++) o[i] = inv[i] * det;
}

// Рендер объёмного тумана в заданный FBO (0 = экран) и viewport.
// Общий для режимов 1 (все каскады) и 2 (только грубый каскад) — оба
// проходят в полном разрешении экрана.
static void render_volumetric(int vw, int vh, GLuint target_fbo,
                              const RendererContext *rc,
                              GLuint scene_tex, GLuint scene_depth,
                              int fogonly)
{
    float invvp[16];
    mat4_invert(rc->viewproj, invvp);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glBindFramebuffer(GL_FRAMEBUFFER, target_fbo);
    glViewport(0, 0, vw, vh);

    // --- Режим 2 «Простой»: аналитический туман (fog2d.fs) ----------------
    // Замкнутые интегралы + 4 сэмпла карты высот вдоль луча — поведение
    // объёмного (туман в лощинах, ореол у солнца, дождевая дымка) без
    // 3D-текстур, марша и теневых запросов.
    if (s_mode == 2 && s_prog2d) {
        glUseProgram(s_prog2d);
        if (u_fogonly2d >= 0) glUniform1f(u_fogonly2d, fogonly ? 1.0f : 0.0f);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, scene_tex);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, scene_depth);
        GLuint htex = world_height_tex();
        glActiveTexture(GL_TEXTURE10);
        if (htex) glBindTexture(GL_TEXTURE_2D, htex);
        glUniformMatrix4fv(u2d_invvp, 1, GL_FALSE, invvp);
        glUniform3f(u2d_campos, rc->view_pos[0], rc->view_pos[1],
                    rc->view_pos[2]);
        glUniform2f(u2d_z, rc->znear, rc->zfar);
        glUniform3f(u2d_sundir, rc->light_dir[0][0], rc->light_dir[0][1],
                    rc->light_dir[0][2]);
        glUniform3f(u2d_fogcol, s_cfg.color[0], s_cfg.color[1], s_cfg.color[2]);
        glUniform3f(u2d_fogcolsun, s_cfg.color_sun[0], s_cfg.color_sun[1],
                    s_cfg.color_sun[2]);
        glUniform3f(u2d_skycolor, rc->sky_color[0], rc->sky_color[1],
                    rc->sky_color[2]);
        glUniform1f(u2d_glare, s_cfg.glare);
        glUniform1f(u2d_raystr, s_cfg.ray_strength);
        glUniform1f(u2d_wetness, rc->wetness);
        glUniform2f(u2d_worldsize, s_world->world_w, s_world->world_d);
        glUniform1f(u2d_hmin, s_world->min_height);
        glUniform1f(u2d_hmax, s_world->max_height);
        // x: плотность у земли, y: высота спада, z: сила, w: дальность
        // (как FOG_MAX у объёмного — 96 м)
        glUniform4f(u2d_fogp, s_cfg.density_ground, s_cfg.ground_height,
                    s_cfg.strength, 96.0f);
        glBindVertexArray(s_vao);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glEnable(GL_DEPTH_TEST);
        return;
    }

    glUseProgram(s_prog);
    if (u_fogonly >= 0) glUniform1f(u_fogonly, fogonly ? 1.0f : 0.0f);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, scene_tex);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, scene_depth);
    for (int c = 0; c < VF_CASCADES; c++) {
        glActiveTexture(GL_TEXTURE2 + c);
        glBindTexture(GL_TEXTURE_3D, s_tex3d[c]);
    }
    glUniformMatrix4fv(u_invviewproj, 1, GL_FALSE, invvp);
    // Позиция ВИДОВОЙ камеры: лучи строятся из invviewproj, а в 3-м лице
    // камера (глаз) стоит сзади персонажа — не совпадает с cam_pos.
    glUniform3f(u_campos, rc->view_pos[0], rc->view_pos[1], rc->view_pos[2]);
    glUniform1f(u_znear, rc->znear);
    glUniform1f(u_zfar, rc->zfar);
    glUniform3f(u_sundir, rc->light_dir[0][0], rc->light_dir[0][1],
                rc->light_dir[0][2]);
    glUniform1f(u_strength, 1.0f);
    glUniform1f(u_fogmode, s_mode == 2 ? 1.0f : 0.0f);
    // цвета и лучи из map/map.json (fog = { color, color_sun, glare, ... })
    glUniform3f(u_fogcol, s_cfg.color[0], s_cfg.color[1], s_cfg.color[2]);
    glUniform3f(u_fogcolsun, s_cfg.color_sun[0], s_cfg.color_sun[1],
                s_cfg.color_sun[2]);
    glUniform1f(u_glare, s_cfg.glare);
    glUniform1f(u_raystr, s_cfg.ray_strength);
    // Дождь: туман светится от неба (uSkyColor), не от солнца — мокрость
    // и цвет неба приходят из кадрового контекста.
    glUniform1f(u_wetness, rc->wetness);
    glUniform3f(u_skycolor, rc->sky_color[0], rc->sky_color[1],
                rc->sky_color[2]);
    // Дымка над землёй (дождь): серая, ~0.5 м, с шумом. Сила растёт с
    // мокротой; толщина слоя постоянна (0.5 м). Время — дрейф шума.
    glUniform2f(u_worldsize, s_world->world_w, s_world->world_d);
    glUniform1f(u_hmin, s_world->min_height);
    glUniform1f(u_hmax, s_world->max_height);
    glUniform1f(u_time, (float)(SDL_GetTicks() % 600000) / 1000.0f);
    // Приземная дымка отключена (убрана по запросу). uHaze = 0 — низ чистый.
    glUniform1f(u_haze, 0.0f);
    glUniform1f(u_hazeh, 1.0f);
    GLuint htex = world_height_tex();
    if (htex) {
        glActiveTexture(GL_TEXTURE10);
        glBindTexture(GL_TEXTURE_2D, htex);
        glActiveTexture(GL_TEXTURE12);
        glBindTexture(GL_TEXTURE_2D, s_noise_tex);
    } else {
        glUniform1f(u_haze, 0.0f);   // нет карты высот — дымку не рисуем
    }
    for (int c = 0; c < VF_CASCADES; c++) {
        glUniform3f(u_center[c], s_center[c][0], s_center[c][1], s_center[c][2]);
        glUniform1f(u_box[c], s_box[c]);
        glUniform1f(u_res[c], (float)s_res[c]);
    }
    // CSM-тени солнца для god rays: матрицы, bias и градиент — из rc
    for (int c = 0; c < SHADOW_CASCADES; c++) {
        glUniformMatrix4fv(u_shadowmatrix[c], 1, GL_FALSE, rc->shadow_matrix[c]);
        // bias ВДВОЕ меньше, чем у сцены: лучи резче (тоньше полутень)
        glUniform1f(u_shadowbias[c], rc->shadow_bias_veg[c] * 0.5f);
        glUniform1f(u_shadowgrad[c], 5.0f / rc->shadow_depth[c]);
        if (rc->shadow_tex[c]) {
            glActiveTexture(GL_TEXTURE5 + c);
            glBindTexture(GL_TEXTURE_2D, rc->shadow_tex[c]);
        }
    }
    glUniform1f(u_shadowon, rc->shadow_on ? 1.0f : 0.0f);
    glBindVertexArray(s_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glEnable(GL_DEPTH_TEST);
}

void voxfog_render(int vw, int vh, const RendererContext *rc,
                   GLuint scene_tex, GLuint scene_depth)
{
    if (!s_on) return;
    if (!s_prog || !s_fbo) return;
    // Марш тумана в ПОЛРАЗМЕРЕ (цвет тумана + trans в альфе RGBA8),
    // композит на экране — один сэмпл тумана и один сцены. Картинка та же
    // (гладкий туман), фрагментов марша вчетверо меньше.
    if (s_fbo_half && s_prog_comp) {
        GLint prev_fbo = 0;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
        render_volumetric((vw + 1) / 2, (vh + 1) / 2, s_fbo_half,
                          rc, scene_tex, scene_depth, 1);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, vw, vh);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_BLEND);
        glUseProgram(s_prog_comp);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, scene_tex);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, s_tex_half);
        glBindVertexArray(s_vao);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glEnable(GL_DEPTH_TEST);
        glActiveTexture(GL_TEXTURE0);
        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fbo);
        return;
    }
    // Один и тот же проход в ПОЛНОМ разрешении экрана. Режим 2 отличается
    // только выборкой каскада (uFogMode): марш по одному грубому каскаду.
    render_volumetric(vw, vh, 0, rc, scene_tex, scene_depth, 0);
}

void voxfog_cleanup(void)
{
    for (int c = 0; c < VF_CASCADES; c++)
        if (s_tex3d[c]) glDeleteTextures(1, &s_tex3d[c]);
    if (s_fbo) {
        glDeleteFramebuffers(1, &s_fbo);
        glDeleteTextures(1, &s_tex);
    }
    if (s_fbo_half) {
        glDeleteFramebuffers(1, &s_fbo_half);
        glDeleteTextures(1, &s_tex_half);
        s_fbo_half = s_tex_half = 0;
    }
    if (s_prog_comp) {
        resources_program_release(s_prog_comp);
        s_prog_comp = 0;
    }
    if (s_prog) resources_program_release(s_prog);
    if (s_prog2d) resources_program_release(s_prog2d);
    if (s_noise_tex) resources_texture_release(s_noise_tex);
    glDeleteBuffers(1, &s_vbo);
    glDeleteVertexArrays(1, &s_vao);
    s_fbo = s_tex = 0;
    s_prog = 0;
    s_prog2d = 0;
    s_noise_tex = 0;
    s_on = 0;
}
