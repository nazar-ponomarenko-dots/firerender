// ============================================================================
//  bloom.c — постобработка glow (bloom)
// ----------------------------------------------------------------------------
//  Пайплайн: сцена (FBO) -> bright (половинное разрешение) -> гаусс H -> V
//  -> композит на экран. Сцена LDR (8 бит), поэтому порог яркости мягкий —
//  светятся самые яркие участки (небо, колосья против солнца).
// ============================================================================
#include "bloom.h"

#include "cvars.h"
#include "fx.h"
#include "mapdata.h"
#include "resources.h"
#include "shader.h"

#include <stddef.h>

static BloomParams *s_bloom_p = NULL;   // для hot-reload (shader_on_reshade)
static void bloom_prog_init(BloomParams *b);   // uniform'ы (id уже готовы)
static void bloom_reshade(void);               // ниже: replace + prog_init

// --- Подводные эффекты (передаются в композит) ---------------------------------
// renderer обновляет состояние каждый кадр (плавный вход/выход из воды);
// bloom_render шлёт их в uniform'ы композита.
static float s_underwater = 0.0f;
static float s_water_tint[3] = { 0.08f, 0.22f, 0.30f };
static float s_under_time = 0.0f;

void bloom_set_underwater(float amount, const float tint[3], float time)
{
    if (amount < 0.0f) amount = 0.0f;
    if (amount > 1.0f) amount = 1.0f;
    s_underwater = amount;
    if (tint) {
        s_water_tint[0] = tint[0];
        s_water_tint[1] = tint[1];
        s_water_tint[2] = tint[2];
    }
    if (time > 0.0f) s_under_time = time;
}

#include <SDL3/SDL.h>
#include <stdlib.h>

static GLuint make_scene_tex(int w, int h, int ldr)
{
    // ldr (r_scene_ldr): RGBA8 вместо RGBA16F — на картошке нет bloom/GI/
    // тумана, HDR не нужен, а запись филов и чтение композита — главный
    // bandwidth на слабых iGPU
    GLuint id;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (ldr)
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    else
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0,
                     GL_RGBA, GL_HALF_FLOAT, NULL);
    return id;
}

static GLuint make_ldr_tex(int w, int h)
{
    // LDR-копия сцены для present-пути картошки: полбанды на запись/чтение
    GLuint id;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    return id;
}

static GLuint make_color_tex(int w, int h)
{
    GLuint id;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    // На LDR/GLES glow тоже RGBA8: bloom в low-end профиле выключен, но
    // валидный таргет нужен для мгновенного переключения без unsupported RT.
    if (cvar_get("r_scene_ldr") > 0.5f)
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    else
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0,
                     GL_RGBA, GL_HALF_FLOAT, NULL);
    return id;
}

static void make_fbo(GLuint *fbo, GLuint *tex, GLuint *depth,
                     int w, int h, int want_depth)
{
    glGenFramebuffers(1, fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, *fbo);
    *tex = make_color_tex(w, h);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, *tex, 0);
    if (want_depth) {
        // Глубина сцены — ТЕКСТУРА (GL_DEPTH_COMPONENT24), чтобы 3D-сцена
        // тестировалась на глубину. Фильтр NEAREST — без сравнения.
        glGenTextures(1, depth);
        glBindTexture(GL_TEXTURE_2D, *depth);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, w, h, 0,
                     GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, NULL);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                               GL_TEXTURE_2D, *depth, 0);
    }
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        SDL_Log("Bloom FBO incomplete (%dx%d)!", w, h);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// Проги создаются СНАРУЖИ (bloom_init / reshade: refcount там).
// Здесь — только uniform'ы для ТЕКУЩИХ id.
static void bloom_prog_init(BloomParams *b)
{
    b->u_btex      = glGetUniformLocation(b->prog_bright, "uTex");
    b->u_threshold = glGetUniformLocation(b->prog_bright, "uThreshold");
    b->u_intensity = glGetUniformLocation(b->prog_bright, "uIntensity");
    b->u_blurtex   = glGetUniformLocation(b->prog_blur, "uTex");
    b->u_dir       = glGetUniformLocation(b->prog_blur, "uDir");
    b->u_texel     = glGetUniformLocation(b->prog_blur, "uTexel");
    b->u_ctex      = glGetUniformLocation(b->prog_composite, "uScene");
    b->u_glowtex   = glGetUniformLocation(b->prog_composite, "uGlow");
    b->u_strength  = glGetUniformLocation(b->prog_composite, "uStrength");
    b->u_exposure  = glGetUniformLocation(b->prog_composite, "uExposure");
    b->u_gamma     = glGetUniformLocation(b->prog_composite, "uGamma");
    b->u_sat       = glGetUniformLocation(b->prog_composite, "uSat");
    b->u_contrast  = glGetUniformLocation(b->prog_composite, "uContrast");
    b->u_wetness   = glGetUniformLocation(b->prog_composite, "uWetness");
    b->u_underwater = glGetUniformLocation(b->prog_composite, "uUnderwater");
    b->u_watertint  = glGetUniformLocation(b->prog_composite, "uWaterTint");
    b->u_time       = glGetUniformLocation(b->prog_composite, "uTime");
}

// Hot-reload: replace балансирует refcount (release старой + новая ссылка);
// если пара не собралась — остаётся прежний id.
static void bloom_reshade(void)
{
    BloomParams *b = s_bloom_p;
    if (!b) return;
    GLuint p1 = resources_program_replace(b->prog_bright,
                                          "shaders/postfx.vs",
                                          "shaders/bright.fs");
    GLuint p2 = resources_program_replace(b->prog_blur,
                                          "shaders/postfx.vs",
                                          "shaders/blur.fs");
    GLuint p3 = resources_program_replace(b->prog_composite,
                                          "shaders/postfx.vs",
                                          "shaders/composite.fs");
    if (p1) b->prog_bright = p1;
    if (p2) b->prog_blur = p2;
    if (p3) b->prog_composite = p3;
    bloom_prog_init(b);
}

// --- MSAA сцены -------------------------------------------------------------
// Сцена рисуется в мультисэмпл-FBO (RBO цвет+глубина, cvar r_msaa: 0/2/4/8/16),
// затем bloom_resolve разворачивает его в обычные текстуры (цвет + глубина),
// которые читают GI/SSR/туман/композит. Кромки геометрии сглаживаются
// аппаратно, глубина для пост-эффектов остаётся резкой.
// Смена r_msaa применяется в начале кадра (bloom_msaa_sync) — без перезапуска.

static void msaa_free(BloomParams *b)
{
    if (b->fbo_ms) {
        glDeleteFramebuffers(1, &b->fbo_ms);
        glDeleteRenderbuffers(1, &b->rbo_color);
        glDeleteRenderbuffers(1, &b->rbo_depth);
        b->fbo_ms = b->rbo_color = b->rbo_depth = 0;
    }
    b->msaa = 0;
}

// Желаемые сэмплы из cvar'а: только 0/4/16 (промежуточные modes убраны —
// разница 2x/4x и 8x/16x на экране слабая, а цена заметная).
// Активен ли MSAA-таргет сцены (для alpha-to-coverage на вырезанных краях).
int bloom_msaa_active(void)
{
    return s_bloom_p ? s_bloom_p->msaa : 0;
}

static int msaa_wanted(void)
{
    // ПРИМ.: раньше при RGBA8-сцене (r_scene_ldr, «упрощённый»/КАРТОШКА) MSAA
    // принудительно выключался — из-за этого кнопка «Сглаживание» в меню на
    // КАРТОШКЕ не делала НИЧЕГО (Выкл=4x=16x по фпс). MSAA работает и с LDR:
    // мультисэмпл-буфер RGBA16F разворачивается блитом в RGBA8-текстуру сцены
    // (bloom_resolve вызывается каждый кадр, present читает уже развёрнутое).
    // Оставляем выбор игроку: по умолчанию r_msaa=0 — картошка быстрая, но
    // включённый 4x/16x теперь честно применяется (и честно стоит bandwidth).
    static const int allowed[] = { 0, 4, 16 };
    float v = cvar_get("r_msaa");
    int s = (int)(v + 0.5f);
    if (s < 0) s = 0;
    if (s > 16) s = 16;
    // ближайшее допустимое снизу
    int best = 0;
    for (unsigned i = 0; i < sizeof allowed / sizeof allowed[0]; i++)
        if (allowed[i] <= s) best = allowed[i];
    if (best > 0) {
        static int maxs_cached = -1;
        if (maxs_cached < 0) glGetIntegerv(GL_MAX_SAMPLES, &maxs_cached);
        int maxs = maxs_cached;
        if (best > maxs) {
            best = 0;
            for (unsigned i = 0; i < sizeof allowed / sizeof allowed[0]; i++)
                if (allowed[i] <= s && allowed[i] <= maxs) best = allowed[i];
        }
        // Кэп по разрешению окна: на 4К 8-16 сэмплов — это десятки мс
        // чистого bandwidth, а разница с 4x на такой плотности пикселей
        // на глаз отсутствует
        extern int renderer_vw(void);
        extern int renderer_vh(void);
        long px = (long)renderer_vw() * renderer_vh();
        if (px > 6500000 && best > 4) best = 4;    // ~4К и выше
        else if (px > 3300000 && best > 8) best = 8; // ~1440p
    }
    return best;
}

static void msaa_create(BloomParams *b, int samples)
{
    msaa_free(b);
    if (samples <= 0) return;
    glGenFramebuffers(1, &b->fbo_ms);
    glBindFramebuffer(GL_FRAMEBUFFER, b->fbo_ms);
    glGenRenderbuffers(1, &b->rbo_color);
    glBindRenderbuffer(GL_RENDERBUFFER, b->rbo_color);
    // RGBA16F — как у обычной сцены: HDR сохраняется до тонмаппинга
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_RGBA16F,
                                     b->vw, b->vh);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                              GL_RENDERBUFFER, b->rbo_color);
    glGenRenderbuffers(1, &b->rbo_depth);
    glBindRenderbuffer(GL_RENDERBUFFER, b->rbo_depth);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples,
                                     GL_DEPTH_COMPONENT24, b->vw, b->vh);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER, b->rbo_depth);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        // Драйвер не даёт мультисэмпл-RGBA16F — честный фолбэк без MSAA.
        // Запоминаем комбинацию, иначе sync пересоздавал этот FBO каждый кадр.
        SDL_Log("MSAA %dx: FBO incomplete, выключаю (без сглаживания)", samples);
        b->msaa_failed = samples;
        b->msaa_failed_ldr = b->scene_ldr;
        msaa_free(b);
    } else {
        b->msaa = samples;
        b->msaa_failed = 0;
        SDL_Log("MSAA: %dx", samples);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void bloom_msaa_sync(BloomParams *b)
{
    int want = msaa_wanted();
    if (want == b->msaa_failed && b->scene_ldr == b->msaa_failed_ldr) return;
    if (want != b->msaa) msaa_create(b, want);
}

void bloom_resolve(BloomParams *b, int want_depth)
{
    // Без MSAA единственная работа — копия глубины RBO->текстура. Если её
    // никто не читает (КАРТОШКА: GI/SSR/voxfog выкл), пропускаем весь blit.
    if (!b->msaa && (!b->rbo_depth_nm || !want_depth)) return;
    GLint readf = 0, drawf = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readf);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawf);
    GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    glDisable(GL_SCISSOR_TEST);            // blit уважает ножницы — гасим
    if (b->msaa) {
        // Цвет + глубина: пост-эффекты (GI/SSR/туман) читают развёрнутую
        glBindFramebuffer(GL_READ_FRAMEBUFFER, b->fbo_ms);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, b->fbo_res);
        glBlitFramebuffer(0, 0, b->vw, b->vh, 0, 0, b->vw, b->vh,
                          GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT,
                          GL_NEAREST);
    } else {
        // Без MSAA цвет уже в текстуре; глубину копируем из RBO в текстуру
        // (рендер в depth-текстуру отключает HiZ — см. bloom_resize).
        glBindFramebuffer(GL_READ_FRAMEBUFFER, b->fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, b->fbo_res);
        glBlitFramebuffer(0, 0, b->vw, b->vh, 0, 0, b->vw, b->vh,
                          GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)readf);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)drawf);
    if (scissor) glEnable(GL_SCISSOR_TEST);
}

void bloom_init(BloomParams *b, int vw, int vh)
{
    s_bloom_p = b;
    // Параметры постобработки — из map/map.json (bloom = { ... }), как
    // значения по умолчанию cvar'ов: меню «Графика» и консоль правят ими
    // вживую (применяются в bloom_render каждый кадр).
    MapBloom bc;
    mapdata_bloom(&bc);
    cvar_register("r_bloom", 1.0f, "свечение (bloom) вкл/выкл");
    cvar_register("r_bloom_threshold", bc.threshold, "порог яркости bloom");
    cvar_register("r_bloom_intensity", bc.intensity, "сила ярких пикселей");
    cvar_register("r_bloom_strength", bc.strength, "сила свечения в композите");
    cvar_register("r_exposure", 1.0f, "экспозиция HDR (тонмаппинг)");
    cvar_register("r_gamma", 1.0f, "гамма-коррекция (1 = без изменений)");
    cvar_register("r_saturation", 1.4f, "насыщенность (1 = без изменений)");
    cvar_register("r_contrast", 1.0f, "контраст (1 = без изменений)");
    cvar_register_ex("r_msaa", 0.0f, 0.0f, 16.0f, CVAR_ARCHIVE,
                     "сглаживание MSAA: 0/2/4/8/16 (применяется сразу)");
    b->threshold = bc.threshold;
    b->intensity = bc.intensity;
    b->strength  = bc.strength;
    // Программы постобработки (replace: old==0 после cleanup => plain-загрузка)
    b->prog_bright = resources_program_replace(b->prog_bright,
                                               "shaders/postfx.vs",
                                               "shaders/bright.fs");
    b->prog_blur = resources_program_replace(b->prog_blur,
                                             "shaders/postfx.vs",
                                             "shaders/blur.fs");
    b->prog_composite = resources_program_replace(b->prog_composite,
                                                  "shaders/postfx.vs",
                                                  "shaders/composite.fs");
    bloom_prog_init(b);    // uniform'ы (для hot-reload)

    shader_on_reshade(bloom_reshade);

    // Полноэкранный треугольник: 3 вершины покрывают весь экран
    glGenVertexArrays(1, &b->vao);
    glBindVertexArray(b->vao);
    glGenBuffers(1, &b->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, b->vbo);
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

    bloom_resize(b, vw, vh);
}

void bloom_resize(BloomParams *b, int vw, int vh)
{
    if (vw <= 0 || vh <= 0) return;
    b->vw = vw;
    b->vh = vh;
    b->msaa_failed = 0;
    int gw = vw / 2, gh = vh / 2;
    if (gw < 1) gw = 1;
    if (gh < 1) gh = 1;
    b->gw = gw;
    b->gh = gh;

    if (b->fbo) {
        glDeleteFramebuffers(1, &b->fbo);
        glDeleteTextures(1, &b->tex);
        glDeleteTextures(1, &b->depth);
        if (b->rbo_depth_nm) glDeleteRenderbuffers(1, &b->rbo_depth_nm);
        if (b->fbo_res) glDeleteFramebuffers(1, &b->fbo_res);
        b->rbo_depth_nm = 0;
        b->fbo_res = 0;
        glDeleteFramebuffers(2, b->fbo_glow);
        glDeleteTextures(2, b->tex_glow);
    }
    if (b->fbo_ldr) {
        glDeleteFramebuffers(1, &b->fbo_ldr);
        glDeleteTextures(1, &b->tex_ldr);
    }
    b->scene_ldr = cvar_get("r_scene_ldr") > 0.5f;
    // Цвет сцены: RGBA16F (HDR) или RGBA8 (картошка — HDR некому)
    b->tex = make_scene_tex(vw, vh, b->scene_ldr);
    glGenFramebuffers(1, &b->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, b->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, b->tex, 0);
    glGenTextures(1, &b->depth);
    glBindTexture(GL_TEXTURE_2D, b->depth);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, vw, vh, 0,
                 GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, NULL);
    // Глубину сцены храним в RBO, а не текстурой-аттачментом: рендер в
    // depth-текстуру лишает драйвер HiZ-оптимизаций, и перерисовка травы
    // (гигантский overdraw с discard) замедлялась ВТРОЕ (4.4 -> 1.5 мс
    // при 1080p). RBO — fast path; для пост-эффектов (GI/SSR/туман)
    // глубина копируется в текстуру одним blit в bloom_resolve.
    glGenRenderbuffers(1, &b->rbo_depth_nm);
    glBindRenderbuffer(GL_RENDERBUFFER, b->rbo_depth_nm);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, vw, vh);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER, b->rbo_depth_nm);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        SDL_Log("Bloom FBO incomplete (%dx%d)!", vw, vh);
    // Resolve-FBO: та же цветовая текстура + depth-текстура — сюда blit'ится
    // глубина (и цвет MSAA-развёртки) перед пост-проходами.
    glGenFramebuffers(1, &b->fbo_res);
    glBindFramebuffer(GL_FRAMEBUFFER, b->fbo_res);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, b->tex, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                           GL_TEXTURE_2D, b->depth, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    // LDR-копия для present: композит (grade) рисуется в масштабе сцены,
    // на экран — аппаратный blit (дешевле шейдерного фуллскрина)
    glGenFramebuffers(1, &b->fbo_ldr);
    glBindFramebuffer(GL_FRAMEBUFFER, b->fbo_ldr);
    b->tex_ldr = make_ldr_tex(vw, vh);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, b->tex_ldr, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    make_fbo(&b->fbo_glow[0], &b->tex_glow[0], NULL, gw, gh, 0);
    make_fbo(&b->fbo_glow[1], &b->tex_glow[1], NULL, gw, gh, 0);
    // MSAA-таргет под новый размер (или под сменившийся r_msaa)
    msaa_create(b, msaa_wanted());
}

GLuint bloom_scene_fbo(const BloomParams *b)
{
    return b->msaa ? b->fbo_ms : b->fbo;
}

GLuint bloom_scene_tex(const BloomParams *b)
{
    return b->tex;
}

GLuint bloom_scene_depth(const BloomParams *b)
{
    return b->depth;
}

void bloom_render(BloomParams *b, GLuint scene_tex, GLuint target_fbo,
                  int out_w, int out_h)
{
    if (!b->fbo) return;
    // Настройки из cvar'ов (меню/консоль): применяются каждый кадр.
    // r_bloom = 0 — bloom выключен: яркие пиксели и блюр НЕ считаются,
    // рисуется только композит (сцена как есть, strength = 0).
    b->threshold = cvar_get("r_bloom_threshold");
    b->intensity = cvar_get("r_bloom_intensity");
    float strength = cvar_get("r_bloom_strength");
    if (cvar_get("r_bloom") < 0.5f) strength = 0.0f;
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);   // HUD мог оставить блендинг — постобработка пишет начисто
    glBindVertexArray(b->vao);

    if (strength > 0.0f) {
        // 1) яркие пиксели сцены -> glow0 (половинное разрешение)
        glBindFramebuffer(GL_FRAMEBUFFER, b->fbo_glow[0]);
        glViewport(0, 0, b->gw, b->gh);
        glUseProgram(b->prog_bright);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, scene_tex);
        glUniform1i(b->u_btex, 0);
        glUniform1f(b->u_threshold, b->threshold);
        glUniform1f(b->u_intensity, b->intensity);
        glDrawArrays(GL_TRIANGLES, 0, 3);

        // 2) гаусс: H (glow0 -> glow1), V (glow1 -> glow0)
        glUseProgram(b->prog_blur);
        glUniform1i(b->u_blurtex, 0);
        glUniform2f(b->u_texel, 1.0f / b->gw, 1.0f / b->gh);
        glBindFramebuffer(GL_FRAMEBUFFER, b->fbo_glow[1]);
        glUniform2f(b->u_dir, 1.0f, 0.0f);
        glBindTexture(GL_TEXTURE_2D, b->tex_glow[0]);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindFramebuffer(GL_FRAMEBUFFER, b->fbo_glow[0]);
        glUniform2f(b->u_dir, 0.0f, 1.0f);
        glBindTexture(GL_TEXTURE_2D, b->tex_glow[1]);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }

    // 3) композит: сцена + glow в целевой FBO (0 = экран, иначе — FBO тумана).
    // При strength <= 0 (bloom выключен) glow умножается на ноль — проход
    // служит копией сцены на экран (один квад вместо четырёх).
    glBindFramebuffer(GL_FRAMEBUFFER, target_fbo);
    // Размер ЦЕЛИ: экран нативен (апскейл сцены r_render_scale тут),
    // fbo тумана — в масштабе сцены. b->vw тут был бы углом на экране.
    glViewport(0, 0, out_w, out_h);
    glUseProgram(b->prog_composite);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, scene_tex);
    glUniform1i(b->u_ctex, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, b->tex_glow[0]);
    glUniform1i(b->u_glowtex, 1);
    glUniform1f(b->u_strength, strength);
    // Экспозиция HDR — из cvar'а r_exposure
    if (b->u_exposure >= 0)
        glUniform1f(b->u_exposure, cvar_get("r_exposure"));
    // Гамма и цвето-коррекция (каждый кадр, из cvar'ов)
    if (b->u_gamma >= 0)
        glUniform1f(b->u_gamma, cvar_get("r_gamma"));
    if (b->u_sat >= 0)
        glUniform1f(b->u_sat, cvar_get("r_saturation"));
    if (b->u_contrast >= 0)
        glUniform1f(b->u_contrast, cvar_get("r_contrast"));
    // Мокрота (дождь): в композите снижает цветастость
    if (b->u_wetness >= 0)
        glUniform1f(b->u_wetness, fx_wetness());
    // Подводные эффекты (renderer обновляет состояние в bloom_set_underwater)
    if (b->u_underwater >= 0) {
        glUniform1f(b->u_underwater, s_underwater);
        glUniform3f(b->u_watertint, s_water_tint[0], s_water_tint[1],
                    s_water_tint[2]);
        glUniform1f(b->u_time, s_under_time);
    }
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

// Present-путь (нет тумана): grade+тонмаппинг в масштабе сцены в RGBA8,
// затем аппаратный blit-апскейл на экран — на слабых iGPU шейдерный
// фуллскрин-апскейл стоит заметно дороже фиксированного blit.
void bloom_present(BloomParams *b, GLuint scene_tex, int out_w, int out_h)
{
    if (!b->fbo_ldr) return;
    GLint prev_fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, b->fbo_ldr);
    bloom_render(b, scene_tex, b->fbo_ldr, b->vw, b->vh);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, b->fbo_ldr);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    glDisable(GL_SCISSOR_TEST);            // blit уважает ножницы — гасим
    glBlitFramebuffer(0, 0, b->vw, b->vh, 0, 0, out_w, out_h,
                      GL_COLOR_BUFFER_BIT, GL_LINEAR);
    if (scissor) glEnable(GL_SCISSOR_TEST);
    // HUD/прицел рисуются следом: вернуть viewport ЦЕЛИ (натив) и FBO
    glViewport(0, 0, out_w, out_h);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fbo);
}

void bloom_cleanup(BloomParams *b)
{
    if (s_bloom_p == b) s_bloom_p = NULL;
    resources_program_release(b->prog_bright);
    resources_program_release(b->prog_blur);
    resources_program_release(b->prog_composite);
    glDeleteBuffers(1, &b->vbo);
    glDeleteVertexArrays(1, &b->vao);
    if (b->fbo) {
        glDeleteFramebuffers(1, &b->fbo);
        glDeleteTextures(1, &b->tex);
        glDeleteTextures(1, &b->depth);
        if (b->rbo_depth_nm) glDeleteRenderbuffers(1, &b->rbo_depth_nm);
        if (b->fbo_res) glDeleteFramebuffers(1, &b->fbo_res);
        b->rbo_depth_nm = 0;
        b->fbo_res = 0;
        glDeleteFramebuffers(2, b->fbo_glow);
        glDeleteTextures(2, b->tex_glow);
        b->fbo = 0;
    }
    if (b->fbo_ldr) {
        glDeleteFramebuffers(1, &b->fbo_ldr);
        glDeleteTextures(1, &b->tex_ldr);
        b->fbo_ldr = 0;
    }
    msaa_free(b);
}
