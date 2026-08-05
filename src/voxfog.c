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

static GLuint s_fbo = 0, s_tex = 0;   // приёмник композита bloom (полный экран)
static GLuint s_prog = 0, s_vao = 0, s_vbo = 0;
static GLint u_scene, u_scenedepth, u_invviewproj, u_campos, u_znear, u_zfar;
static GLint u_tex3d[VF_CASCADES], u_center[VF_CASCADES];
static GLint u_box[VF_CASCADES], u_res[VF_CASCADES];
static GLint u_sundir, u_strength, u_fogmode;
static GLint u_fogcol, u_fogcolsun, u_glare, u_raystr;
static GLint u_shadowmap[SHADOW_CASCADES], u_shadowmatrix[SHADOW_CASCADES], u_shadowbias[SHADOW_CASCADES];
static GLint u_shadowgrad[SHADOW_CASCADES], u_shadowon;

// Режим тумана: 0 = выкл, 1 = объёмный (полное разрешение),
// 2 = объёмный в 1/4 разрешения (дешёвый, для слабых машин)
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

// Плотность тумана в точке мира (на метр пути) — из map.json
static float vf_density(float x, float y, float z)
{
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
    d *= s_cfg.strength;
    if (d > 0.2f) d = 0.2f;
    return d;
}

// Перестроить каскад c (центр закреплён к мировой сетке вокселей)
static void vf_rebuild(int c)
{
    float cell = s_box[c] / s_res[c];
    unsigned char *buf = malloc((size_t)s_res[c] * s_res[c] * s_res[c]);
    if (!buf) return;
    const float *cen = s_center[c];
    for (int z = 0; z < s_res[c]; z++)
        for (int y = 0; y < s_res[c]; y++)
            for (int x = 0; x < s_res[c]; x++) {
                float wx = cen[0] + ((float)x + 0.5f - s_res[c] * 0.5f) * cell;
                float wy = cen[1] + ((float)y + 0.5f - s_res[c] * 0.5f) * cell;
                float wz = cen[2] + ((float)z + 0.5f - s_res[c] * 0.5f) * cell;
                float d = vf_density(wx, wy, wz);
                buf[(z * s_res[c] + y) * s_res[c] + x] =
                    (unsigned char)(d * 255.0f);
            }
    glBindTexture(GL_TEXTURE_3D, s_tex3d[c]);
    glTexSubImage3D(GL_TEXTURE_3D, 0, 0, 0, 0,
                    s_res[c], s_res[c], s_res[c],
                    GL_RED, GL_UNSIGNED_BYTE, buf);
    free(buf);
}

void voxfog_init(const WorldState *w, int vw, int vh)
{
    s_world = w;
    mapdata_fog(&s_cfg);
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
        glTexImage3D(GL_TEXTURE_3D, 0, GL_R8, s_res[c], s_res[c], s_res[c],
                     0, GL_RED, GL_UNSIGNED_BYTE, NULL);
        memset(s_center[c], 0, sizeof s_center[c]);
    }

    s_prog = make_program("shaders/postfx.vs", "shaders/volumetric.fs");
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
    glUseProgram(s_prog);
    glUniform1i(u_scene, 0);
    glUniform1i(u_scenedepth, 1);
    glUniform1i(u_tex3d[0], 2);
    glUniform1i(u_tex3d[1], 3);
    glUniform1i(u_tex3d[2], 4);
    for (int c = 0; c < SHADOW_CASCADES; c++)
        glUniform1i(u_shadowmap[c], 5 + c);      // CSM на юнитах 5-9
    glUseProgram(0);

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
    // Режим 2: только грубый каскад 2 (дешевле CPU) — ближние не нужны
    int first = (s_mode == 2) ? 2 : 0;
    for (int c = first; c < VF_CASCADES; c++) {
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
// Общий для режимов 1 (полное разрешение) и 2 (1/4 + растяжка).
static void render_volumetric(int vw, int vh, GLuint target_fbo,
                              const RendererContext *rc,
                              GLuint scene_tex, GLuint scene_depth)
{
    float invvp[16];
    mat4_invert(rc->viewproj, invvp);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glBindFramebuffer(GL_FRAMEBUFFER, target_fbo);
    glViewport(0, 0, vw, vh);
    glUseProgram(s_prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, scene_tex);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, scene_depth);
    for (int c = 0; c < VF_CASCADES; c++) {
        glActiveTexture(GL_TEXTURE2 + c);
        glBindTexture(GL_TEXTURE_3D, s_tex3d[c]);
    }
    glUniformMatrix4fv(u_invviewproj, 1, GL_FALSE, invvp);
    glUniform3f(u_campos, rc->cam_pos[0], rc->cam_pos[1], rc->cam_pos[2]);
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
    // Один и тот же проход в ПОЛНОМ разрешении экрана. Режим 2 отличается
    // только выборкой каскада (uFogMode): марш по одному грубому каскаду.
    render_volumetric(vw, vh, 0, rc, scene_tex, scene_depth);
}

void voxfog_cleanup(void)
{
    for (int c = 0; c < VF_CASCADES; c++)
        if (s_tex3d[c]) glDeleteTextures(1, &s_tex3d[c]);
    if (s_fbo) {
        glDeleteFramebuffers(1, &s_fbo);
        glDeleteTextures(1, &s_tex);
    }
    if (s_prog) resources_program_release(s_prog);
    glDeleteBuffers(1, &s_vbo);
    glDeleteVertexArrays(1, &s_vao);
    s_fbo = s_tex = 0;
    s_prog = 0;
    s_on = 0;
}
