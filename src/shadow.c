// ============================================================================
//  shadow.c — каскадные карты теней: камеры на солнце, глубина земли и травы
// ----------------------------------------------------------------------------
//  Оптимизировано под частоту кадров:
//    - растительность в тенях: прореживание делается на CPU (vegetation.c:
//      статика — каждый 2-й экземпляр, динамика — правило uThin), шейдер
//      не отбраковывает экземпляры, вершинный этап вдвое дешевле;
//    - каскады 0/1 (боксы 8/16 м): рисуются только супер-чанки/чанки
//      внутри бокса (ARB_base_instance) — вершинный этап не гоняет сотни
//      тысяч экземпляров ради нескольких видимых; карты глубины байт-
//      в-байт те же (проверено оффскрин-сравнением кадров);
//    - каскад 3 (512 м, тексель 25 см) рисует только статику: тени густой
//      динамики там меньше текселя и не видны;
//    - локации uniform'ов кэшируются при инициализации.
// ============================================================================
#include "shadow.h"

#include "cvars.h"
#include "linalg.h"
#include "mapdata.h"
#include "objects.h"
#include "resources.h"
#include "shader.h"
#include "terrain.h"
#include "vegetation.h"
#include "world.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
static GLuint g_shadow_fbo[SHADOW_CASCADES], g_shadow_tex[SHADOW_CASCADES];
static GLuint g_shadow_prog = 0, g_shadowveg_prog = 0;
static int g_shadow_on = 1;         // [ — вкл/выкл теней
int g_shadow_size[SHADOW_CASCADES] = { 2048, 2048, 2048, 2048 };
// Тестовые тумблеры замеров производительности (FR_TEST в main.c):
int g_shadow_terrain = 1;           // рельеф в картах теней
int g_shadow_cull = 1;              // culling чанков по боксу каскада
int g_shadow_veg = 1;               // растительность в картах теней
int g_shadow_obj = 1;               // объекты (деревья) в картах теней
// Матрицы каскадов последнего кадра — для растительности (приём теней)
static float g_last_mvp[SHADOW_CASCADES][16];

// Кэш локаций uniform'ов (обновляются каждый кадр)
static GLint u_shadow_viewproj, u_shadow_heightmap, u_shadow_hmin, u_shadow_hmax;
static GLint u_veg_viewproj, u_veg_veg, u_veg_thin, u_veg_time, u_veg_campos;
static GLint u_veg_dyn;      // 1 = динамический слой (только lod0/lod1)

void shadow_init(void)
{
    // Состояние — из cvar'а r_shadows_mode: иначе r_shadows_mode 0 из
    // config.cfg не выключит тени (sync_cvars срабатывает на изменение).
    {
        float m = cvar_get("r_shadows_mode");
        g_shadow_on = (m == 1.0f || m == 2.0f);
    }
    // Размер карт теней из map/map.json (shadows.size, по умолчанию 2048)
    {
        MapShadows sh;
        mapdata_shadows(&sh);
        if (sh.size >= 512 && sh.size <= 8192)
            for (int c = 0; c < SHADOW_CASCADES; c++)
                g_shadow_size[c] = sh.size;
        SDL_Log("Shadows: %dx%d per cascade", g_shadow_size[0], g_shadow_size[0]);
    }
    g_shadow_prog = make_program("shaders/shadow.vs", "shaders/shadow.fs");
    g_shadowveg_prog = make_program("shaders/shadowveg.vs", "shaders/shadowveg.fs");
    u_shadow_viewproj = glGetUniformLocation(g_shadow_prog, "uViewProj");
    u_shadow_heightmap = glGetUniformLocation(g_shadow_prog, "uHeightmap");
    u_shadow_hmin = glGetUniformLocation(g_shadow_prog, "uHeightMin");
    u_shadow_hmax = glGetUniformLocation(g_shadow_prog, "uHeightMax");
    u_veg_viewproj = glGetUniformLocation(g_shadowveg_prog, "uViewProj");
    u_veg_veg = glGetUniformLocation(g_shadowveg_prog, "uVeg");
    u_veg_thin = glGetUniformLocation(g_shadowveg_prog, "uThin");
    u_veg_time = glGetUniformLocation(g_shadowveg_prog, "uTime");
    u_veg_campos = glGetUniformLocation(g_shadowveg_prog, "uCamPos");
    u_veg_dyn = glGetUniformLocation(g_shadowveg_prog, "uDyn");

    // Карты теней каскадов: FBO без цвета (только глубина)
    for (int c = 0; c < SHADOW_CASCADES; c++) {
        int size = g_shadow_size[c];
        glGenTextures(1, &g_shadow_tex[c]);
        glBindTexture(GL_TEXTURE_2D, g_shadow_tex[c]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, size, size,
                     0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, NULL);
        // Аппаратный PCF: сравнение с референсом + билинейный фильтр —
        // один texture() в шейдере даёт гладкую полутень без полос.
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE,
                        GL_COMPARE_REF_TO_TEXTURE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
        glGenFramebuffers(1, &g_shadow_fbo[c]);
        glBindFramebuffer(GL_FRAMEBUFFER, g_shadow_fbo[c]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D,
                               g_shadow_tex[c], 0);
        glDrawBuffer(GL_NONE);
        glReadBuffer(GL_NONE);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            SDL_Log("Shadow FBO %d incomplete!", c);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
}

int shadow_is_on(void)
{
    return g_shadow_on;
}

void shadow_toggle(void)
{
    g_shadow_on = !g_shadow_on;
}

void shadow_set(int on)
{
    g_shadow_on = on ? 1 : 0;
}

GLuint shadow_texture(int c)
{
    return g_shadow_tex[c];
}

const float *shadow_matrix(int c)
{
    return g_last_mvp[c];
}


// Матрица теней от позиции солнца: вид+проекция (vp) и с bias'ом [0..1] (mvp).
// center — центр бокса (позиция игрока).
// ndc_texel — размер NDC-квадрата одного текселя (0 = без выравнивания):
// карта выравнивается по сетке текселей, чтобы при движении камеры
// тени не "плыли" по одному текселю туда-сюда.
static void shadow_matrix_for(const float *center, const float *sun_dir,
                              float area, float depth, float ndc_texel,
                              float mvp[16], float vp[16])
{
    float e[3] = { center[0] + sun_dir[0] * depth,
                   center[1] + sun_dir[1] * depth,
                   center[2] + sun_dir[2] * depth };
    float up[3] = { 0, 1, 0 }, view[16], proj[16];
    mat4_lookat(view, e, center, up);
    mat4_ortho(proj, -area / 2, area / 2, -area / 2, area / 2, 1.0f, 2.0f * depth);
    mat4_mul(vp, proj, view);
    if (ndc_texel > 0.0f) {
        // Стабилизация теней: сетка текселей привязывается к МИРУ (якорь —
        // начало координат), а не к камере. Иначе при ходьбе сетка
        // перещёлкивается на тексель и края теней мерцают/прыгают.
        float pc[4] = { 0.0f, 0.0f, 0.0f, 1.0f };   // фиксированная точка мира
        float pv[4];
        for (int r = 0; r < 4; r++)
            pv[r] = vp[0*4+r]*pc[0] + vp[1*4+r]*pc[1]
                  + vp[2*4+r]*pc[2] + vp[3*4+r]*pc[3];
        float nx = pv[0] / pv[3], ny = pv[1] / pv[3];
        vp[12] += floorf(nx / ndc_texel + 0.5f) * ndc_texel - nx;
        vp[13] += floorf(ny / ndc_texel + 0.5f) * ndc_texel - ny;
    }
    float bias[16] = { 0.5f,0,0,0,  0,0.5f,0,0,  0,0,0.5f,0,  0.5f,0.5f,0.5f,1 };
    mat4_mul(mvp, bias, vp);
}

// Рисует землю + растительность в карту глубины (fbo, size, vp).
// draw_static/draw_dyn — какие слои растительности рисовать; оба слоя уже
// предпрорежены на CPU (vegetation.c), в шейдере отбраковки нет.
// cull_mode: 0 = весь буфер, 1 = только супер-чанки/чанки внутри бокса
// каскада (карта глубины идентична: остальное всё равно отсекалось
// аппаратным клипом, только ПОСЛЕ вершинного шейдера).
static void shadow_render_into(GLuint fbo, int size, float box,
                               const float vp[16], TerrainParams *p,
                               int cull_mode,
                               int draw_static, int draw_dyn)
{
    GLint prev_fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, size, size);
    // ВАЖНО: маска записи глубины должна быть TRUE ДО glClear — иначе glClear
    // игнорируется (скайбокс перед этим ставит glDepthMask(GL_FALSE), и без
    // сброса карта теней "смазывается": старые глубины не стираются и
    // накапливаются, тени чернеют и плывут при движении камеры).
    glDepthMask(GL_TRUE);
    glClear(GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glCullFace(GL_BACK);         // лицевые грани — как в обычном рендере
    glUseProgram(g_shadow_prog);
    glUniformMatrix4fv(u_shadow_viewproj, 1, GL_FALSE, vp);
    glUniform1i(u_shadow_heightmap, 2);
    glUniform1f(u_shadow_hmin, p->world->min_height);
    glUniform1f(u_shadow_hmax, p->world->max_height);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, p->tex_height);
    glBindVertexArray(p->vao);
    if (g_shadow_terrain)
        glDrawElements(GL_TRIANGLES, p->vert_count, GL_UNSIGNED_INT, NULL);

    // растительность. Густую (динамика) прореживаем: иначе 100 шт/м2
    // перекрывают каждый тексель и земля уходит в сплошную черноту.
    // Прореживание делается на CPU (vegetation.c: статика — каждый 2-й,
    // динамика — по правилу uThin из shadowveg.vs), поэтому здесь
    // uThin = 0 всегда и вершинный этап не тратит время на отбраковку.
    glDisable(GL_CULL_FACE);     // биллборды — с двух сторон
    glUseProgram(g_shadowveg_prog);
    glUniformMatrix4fv(u_veg_viewproj, 1, GL_FALSE, vp);
    glUniform1i(u_veg_veg, 0);
    glUniform1f(u_veg_time, veg_time());
    glUniform3f(u_veg_campos, p->cam->cam.pos[0], p->cam->cam.pos[1],
                p->cam->cam.pos[2]);
    glUniform1f(glGetUniformLocation(g_shadowveg_prog, "uWalk"),
                p->cam->cam.fly ? 0.0f : 1.0f);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, veg_texture());
    glUniform1i(u_veg_thin, 0);
    glUniform1f(u_veg_dyn, 0.0f);   // статика: полный LOD (в т.ч. lod2)
    if (draw_static && g_shadow_veg) {
        glBindVertexArray(veg_shadow_vao(0));
        if (cull_mode && veg_has_base_instance()) {
            // только супер-чанки внутри бокса каскада: бокс + полудиагональ
            // супер-чанка (64 м: 32*1.414 ≈ 45.3) — остальные вне бокса всё
            // равно отсекались бы аппаратным клипом
            float r2 = (45.3f + box * 0.7071f) * (45.3f + box * 0.7071f);
            float pcx = p->cam->cam.pos[0], pcz = p->cam->cam.pos[2];
            for (int s = 0; s < veg_shadow_super_n(); s++) {
                int cnt = veg_shadow_super_cnt(0, s);
                if (cnt <= 0) continue;
                float dx = veg_shadow_super_cx(s) - pcx;
                float dz = veg_shadow_super_cz(s) - pcz;
                if (dx*dx + dz*dz > r2) continue;
                glDrawElementsInstancedBaseInstance(GL_TRIANGLES, 6,
                        GL_UNSIGNED_INT, NULL, cnt,
                        veg_shadow_super_base(0, s));
            }
        } else {
            glDrawElementsInstanced(GL_TRIANGLES, 6, GL_UNSIGNED_INT, NULL,
                                    veg_shadow_count(0));
        }
        glBindVertexArray(veg_shadow_vao(1));
        if (cull_mode && veg_has_base_instance()) {
            float r2 = (45.3f + box * 0.7071f) * (45.3f + box * 0.7071f);
            float pcx = p->cam->cam.pos[0], pcz = p->cam->cam.pos[2];
            for (int s = 0; s < veg_shadow_super_n(); s++) {
                int cnt = veg_shadow_super_cnt(1, s);
                if (cnt <= 0) continue;
                float dx = veg_shadow_super_cx(s) - pcx;
                float dz = veg_shadow_super_cz(s) - pcz;
                if (dx*dx + dz*dz > r2) continue;
                glDrawElementsInstancedBaseInstance(GL_TRIANGLES, 6,
                        GL_UNSIGNED_INT, NULL, cnt,
                        veg_shadow_super_base(1, s));
            }
        } else {
            glDrawElementsInstanced(GL_TRIANGLES, 6, GL_UNSIGNED_INT, NULL,
                                    veg_shadow_count(1));
        }
    }
    glUniform1f(u_veg_dyn, 1.0f);   // динамика: только lod0/lod1 (без lod2)
    if (draw_dyn && g_shadow_veg) {
        if (cull_mode && veg_has_base_instance()) {
            // чанки внутри бокса: бокс + полудиагональ чанка (16 м: 11.3)
            float r2 = (11.3f + box * 0.7071f) * (11.3f + box * 0.7071f);
            float pcx = p->cam->cam.pos[0], pcz = p->cam->cam.pos[2];
            for (int k = 0; k < veg_dyn_shadow_chunks(); k++) {
                float cx, cz;
                GLuint gbase, wbase;
                int gcnt, wcnt;
                veg_dyn_shadow_chunk(k, &cx, &cz, &gbase, &gcnt, &wbase, &wcnt);
                float dx = cx - pcx, dz = cz - pcz;
                if (dx*dx + dz*dz > r2) continue;
                if (gcnt > 0) {
                    glBindVertexArray(veg_shadow_vao(2));
                    glDrawElementsInstancedBaseInstance(GL_TRIANGLES, 6,
                            GL_UNSIGNED_INT, NULL, gcnt, gbase);
                }
                if (wcnt > 0) {
                    glBindVertexArray(veg_shadow_vao(3));
                    glDrawElementsInstancedBaseInstance(GL_TRIANGLES, 6,
                            GL_UNSIGNED_INT, NULL, wcnt, wbase);
                }
            }
        } else {
            glBindVertexArray(veg_shadow_vao(2));
            glDrawElementsInstanced(GL_TRIANGLES, 6, GL_UNSIGNED_INT, NULL,
                                    veg_shadow_count(2));
            glBindVertexArray(veg_shadow_vao(3));
            glDrawElementsInstanced(GL_TRIANGLES, 6, GL_UNSIGNED_INT, NULL,
                                    veg_shadow_count(3));
        }
    }
    // Объекты (деревья): крест из двух квадов с альфа-тестом,
    // отсечение по боксу каскада (см. objects.c)
    if (g_shadow_obj)
        objects_shadow_render(vp, box);
    glEnable(GL_CULL_FACE);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fbo);
}

void shadow_render(TerrainParams *p)
{
    // Тени рисуем, только если включены глобально (клавиша [) и сам
    // источник света отбрасывает тени (shadow.enabled из map/map.json).
    if (!g_shadow_on || !p->sun->shadow_on) return;
    const float *sd = p->sun->dir;
    const float boxes[SHADOW_CASCADES] = { SHADOW_BOX_0, SHADOW_BOX_1, SHADOW_BOX_2, SHADOW_BOX_3 };
    const int sizes[SHADOW_CASCADES] = { g_shadow_size[0], g_shadow_size[1], g_shadow_size[2], g_shadow_size[3] };
    for (int c = 0; c < SHADOW_CASCADES; c++) {
        float box = boxes[c];
        float depth = box * SHADOW_DEPTH_MUL + 400.0f;
        float vp[16];
        shadow_matrix_for(p->cam->cam.pos, sd, box, depth,
                          2.0f / sizes[c], p->shadow_mvp[c], vp);
        memcpy(g_last_mvp[c], p->shadow_mvp[c], sizeof g_last_mvp[c]);
        // Каскады 0-1 (8/16 м): статика + динамика (детальные тени у ног).
        // Каскад 2 (96 м): только статика — динамика там (~200 тыс.
        // инстансов) даёт неразличимую на текселе 4.7 см тень.
        // Каскад 3 (512 м): статика пшеницы — тени поля до ~256 м.
        shadow_render_into(g_shadow_fbo[c], sizes[c], boxes[c], vp, p,
                           (c < 2 && g_shadow_cull) ? 1 : 0,
                           c < 3, c < 2);
    }
}

void shadow_cleanup(void)
{
    resources_program_release(g_shadow_prog);
    resources_program_release(g_shadowveg_prog);
    glDeleteTextures(SHADOW_CASCADES, g_shadow_tex);
    glDeleteFramebuffers(SHADOW_CASCADES, g_shadow_fbo);
    g_shadow_prog = g_shadowveg_prog = 0;
}
