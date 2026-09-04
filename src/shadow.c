// ============================================================================
//  shadow.c — каскадные карты теней: FBO, матрицы и оркестрация прохода
// ----------------------------------------------------------------------------
//  Теневой проход ЕДИН (п.13): shadow_render для каждого каскада выставляет
//  в RendererContext pass = RP_SHADOW и обходит дерево сцены — рельеф,
//  растительность и объекты рисуют СВОЮ геометрию в глубину (у каждого узла
//  своя программа и свои правила прореживания), остальные узлы в теневом
//  проходе пропускаются. Здесь остаются только FBO каскадов и матрицы
//  "свет -> NDC" (с выравниванием по текселям).
//
//  Оптимизировано под частоту кадров:
//    - растительность в тенях: прореживание делается на CPU (vegetation.c:
//      статика — каждый 2-й экземпляр, динамика — правило uThin), шейдер
//      не отбраковывает экземпляры, вершинный этап вдвое дешевле;
//    - каскады 0/1 (боксы 8/16 м): рисуются только супер-чанки/чанки
//      внутри бокса (ARB_base_instance) — вершинный этап не гоняет сотни
//      тысяч экземпляров ради нескольких видимых;
//    - каскад 3 (512 м) рисует только статику: тени густой динамики там
//      меньше текселя и не видны.
// ============================================================================
#include "shadow.h"

#include "cvars.h"
#include "linalg.h"
#include "mapdata.h"
#include "renderer.h"
#include "shader.h"
#include "world.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static GLuint g_shadow_fbo[SHADOW_CASCADES], g_shadow_tex[SHADOW_CASCADES];
static int g_shadow_on = 1;         // [ — вкл/выкл теней
int g_shadow_size[SHADOW_CASCADES] = { 2048, 2048, 2048, 2048 };
// Тестовые тумблеры замеров производительности (FR_TEST в main.c):
int g_shadow_terrain = 1;           // рельеф в картах теней
int g_shadow_cull = 1;              // culling чанков по боксу каскада
int g_shadow_veg = 1;               // растительность в картах теней
int g_shadow_obj = 1;               // объекты (деревья) в картах теней
int g_shadow_cache = 1;             // кэш статических каскадов (FR_TEST 'C' выкл)
// Матрицы каскадов последнего кадра (для renderer.c/совместимости)
static float g_last_mvp[SHADOW_CASCADES][16];

// --- Кэш статических каскадов -------------------------------------------------
// Каскады 2/3 не содержат динамики (vegetation.c рисует динамику только при
// shadow_dyn, модели в карты теней не пишутся вовсе) — их содержимое зависит
// ТОЛЬКО от снапнутой матрицы. Пока она не сменилась, перерисовывать карту
// не нужно: FBO хранит глубину с прошлого кадра. Стоя на месте — 100% попаданий,
// на ходу матрица меняется лишь через шаг сетки снапа (шаг крупнее текселя:
// 4 текселя у каскада 2, 8 — у каскада 3; подёргивание теней на ходу
// незаметно — тени там мягкие и блендятся с соседним каскадом).
static float g_cache_vp[SHADOW_CASCADES][16];
static int g_cache_valid[SHADOW_CASCADES];
static int s_shadow_frame = 0;      // для шахматного обновления динамики
// Матрицы каскадов ПРОШЛОГО кадра (все 4): «сменилась ли проекция каскада» —
// сигнал для шахматки растительности (при смене — полная перерисовка).
static float s_last_vp[SHADOW_CASCADES][16];

static void shadow_cache_invalidate(void)
{
    memset(g_cache_valid, 0, sizeof g_cache_valid);
}

void shadow_init(void)
{
    // Состояние — из cvar'а r_shadows_mode: иначе r_shadows_mode 0 из
    // config.cfg не выключит тени (sync_cvars срабатывает на изменение).
    {
        float m = cvar_get("r_shadows_mode");
        g_shadow_on = (m == 1.0f || m == 2.0f);
    }
    shadow_cache_invalidate();
    // Размер карт теней из map/map.json (shadows.size, по умолчанию 2048)
    {
        MapShadows sh;
        mapdata_shadows(&sh);
        if (sh.size >= 512 && sh.size <= 8192)
            for (int c = 0; c < SHADOW_CASCADES; c++)
                g_shadow_size[c] = sh.size;
        SDL_Log("Shadows: %dx%d per cascade", g_shadow_size[0], g_shadow_size[0]);
    }

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
    shadow_cache_invalidate();
}

void shadow_set(int on)
{
    g_shadow_on = on ? 1 : 0;
    shadow_cache_invalidate();
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
// Рисует все каскады из камеры солнца. Узлы сцены сами рисуют глубину
// (pass == RP_SHADOW в контексте); здесь — только матрицы и FBO каскадов.
void shadow_render(SceneNode *root, RendererContext *rc)
{
    // Тени рисуем, только если включены глобально (клавиша [) и сам
    // источник света отбрасывает тени (shadow.enabled из map/map.json).
    if (!rc->shadow_on) return;
    const float *sd = rc->light_dir[0];
    const float boxes[SHADOW_CASCADES] = {
        SHADOW_BOX_0, SHADOW_BOX_1, SHADOW_BOX_2, SHADOW_BOX_3
    };
    const int sizes[SHADOW_CASCADES] = {
        g_shadow_size[0], g_shadow_size[1], g_shadow_size[2], g_shadow_size[3]
    };
    // Наклон тени: насколько далеко по горизонтали уходит тень объекта
    // высотой 1 м (горизонталь / вертикаль по направлению солнца). Низкое
    // солнце — длинные тени: дальние деревья должны рисоваться в ближних
    // каскадах, иначе их тень (лежащая рядом с игроком) исчезает — узлы
    // (vegetation/objects) добавляют к радиусу culling height*shadow_slope.
    {
        float hy = sd[1] > 0.05f ? sd[1] : 0.05f;
        float hl = sqrtf(sd[0]*sd[0] + sd[2]*sd[2]);
        float slope = hl / hy;
        rc->shadow_slope = slope > 12.0f ? 12.0f : slope;   // потолок от пологих
    }

    GLint prev_fbo = 0, prev_vp[4] = { 0, 0, 0, 0 };
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    glGetIntegerv(GL_VIEWPORT, prev_vp);
    s_shadow_frame++;

    for (int c = 0; c < SHADOW_CASCADES; c++) {
        float box = boxes[c];
        float depth = box * SHADOW_DEPTH_MUL + 400.0f;
        float vp[16], mvp[16];
        // Статическим каскадам (2/3) — крупный шаг снапа: 4/8 текселей.
        // Матрица (и кэш карты) меняется в 4/8 раз реже на ходу; подёргивание
        // дальних теней при шаге 19 см / 2 м незаметно — они мягкие и на
        // границах блендятся с соседним каскадом.
        float snap_mul = (c == 2) ? 4.0f : (c == 3) ? 8.0f : 1.0f;
        shadow_matrix_for(rc->cam_pos, sd, box, depth,
                          2.0f * snap_mul / sizes[c], mvp, vp);
        // Сменилась ли матрица каскада с прошлого кадра (камера перешагнула
        // шаг сетки снапа): при смене растительность обязана перерисоваться
        // в НОВУЮ проекцию, иначе её тень разъедется с рельефом.
        int mat_same = memcmp(s_last_vp[c], vp, sizeof vp) == 0;
        // Свежие матрицы каскада — в контекст (их читают vegetation/objects/
        // voxfog в этом же кадре) и в g_last_mvp (для renderer_ctx_fill)
        memcpy(rc->shadow_matrix[c], mvp, sizeof mvp);
        memcpy(g_last_mvp[c], mvp, sizeof mvp);
        memcpy(s_last_vp[c], vp, sizeof vp);

        // Кэш статических каскадов: матрица не сменилась — карта глубины в
        // FBO уже содержит ровно этот кадр (рельеф/деревья статичны, качание
        // статичной травы вдаль нечитаемо) — пропускаем проход целиком.
        if (c >= 2 && g_shadow_cache && g_cache_valid[c] &&
            memcmp(g_cache_vp[c], vp, sizeof vp) == 0)
            continue;

        // Теневой проход: узлы рисуют глубину в этот каскад
        rc->pass = RP_SHADOW;
        memcpy(rc->shadow_vp, vp, sizeof vp);
        rc->shadow_box_cur = box;
        rc->shadow_cascade = c;
        // Каскады 0-1 (8/16 м): статика + динамика (детальные тени у ног).
        // Каскад 2 (96 м): только статика — динамика там (~200 тыс.
        // инстансов) даёт неразличимую на текселе 4.7 см тень.
        // Каскад 3 (512 м): статика пшеницы — тени поля до ~256 м.
        //
        // ШАХМАТКА РАСТИТЕЛЬНОСТИ (perf, замер FR_TEST 'q': veg-тени =
        // ~3.4 мс из ~7.7 мс кадра — 44%). Качание травы медленное (~1 Гц),
        // поэтому ОБА слоя растительности (статика+динамика) каждого
        // каскада 0/1 обновляются через кадр (каскады в противофазе).
        // Пока матрица каскада НЕ меняется (стоим/дышим камерой) — это
        // честно: тень качания запаздывает на 8-16 мс, глазу неотличимо.
        // Матрица сменилась (ходьба) — рисуем оба слоя в новую проекцию,
        // иначе тень травы разъедется с рельефом.
        int veg_frame = !mat_same || (((s_shadow_frame + c) & 1) == 0);
        rc->shadow_dyn = (c < 2) ? veg_frame : 0;
        rc->shadow_static = (c < 3) ? veg_frame : 0;
        rc->shadow_cull = (c < 2 && g_shadow_cull) ? 1 : 0;

        glBindFramebuffer(GL_FRAMEBUFFER, g_shadow_fbo[c]);
        glViewport(0, 0, sizes[c], sizes[c]);
        // ВАЖНО: маска записи глубины должна быть TRUE ДО glClear — иначе
        // glClear игнорируется (скайбокс перед этим ставит glDepthMask(FALSE),
        // и без сброса карта теней "смазывается").
        glDepthMask(GL_TRUE);
        glClear(GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);
        glCullFace(GL_BACK);         // лицевые грани — как в обычном рендере

        // Единый проход: узлы сами выбирают, что рисовать в RP_SHADOW
        for (SceneNode *n = root->first_child; n; n = n->next)
            if (n->render) n->render(n, 0.0f);

        memcpy(g_cache_vp[c], vp, sizeof vp);
        g_cache_valid[c] = 1;
    }
    rc->pass = RP_COLOR;
    // Вернуть FBO и вьюпорт цветного прохода (первый узел — небо — не
    // сбрасывает вьюпорт сам, а рисует полноэкранный треугольник)
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fbo);
    glViewport(prev_vp[0], prev_vp[1], prev_vp[2], prev_vp[3]);
}

void shadow_cleanup(void)
{
    glDeleteTextures(SHADOW_CASCADES, g_shadow_tex);
    glDeleteFramebuffers(SHADOW_CASCADES, g_shadow_fbo);
}
