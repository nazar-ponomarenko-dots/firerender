// ============================================================================
//  profiler.c — тайминги по модулям (см. profiler.h)
// ============================================================================
#include "profiler.h"

#include "cvars.h"
#include "gl_loader.h"
#include <SDL3/SDL.h>

static float s_ms[PROF_N];

// GPU-тайминги: по 2 query на слот (чередуем кадры — результат читаем
// из того, что начинался 2 кадра назад: GPU точно успел)
static GLuint s_gpu_q[PROF_N][2];
static float s_gpu_ms[PROF_N];
static int s_gpu_ok = -1;
static int s_parity = 0;
// Слот реально рисовался в кадре этой чётности? Иначе query хранит СТАРЫЙ
// результат (GL не сбрасывает query-объекты) и панель показывала бы время
// выключенного прохода (напр. туман при r_fog_mode 0).
static int s_issued[PROF_N][2];
static int s_active[PROF_N];
// Панель видима? Проверяется раз в кадр (prof_reset): при скрытой панели
// queries НЕ выпускаются вовсе — time-queries на слабом GPU не бесплатны.
static int s_panel = 0;

static void gpu_init(void)
{
    if (s_gpu_ok >= 0) return;
    // На GLES без timer_query расширения указатели NULL — не крашимся, простоCPU
    if (!p_glGenQueries || !p_glBeginQuery || !p_glEndQuery ||
        !p_glGetQueryObjectiv || !p_glGetQueryObjectui64v) {
        s_gpu_ok = 0;
        SDL_Log("profiler: GL timer queries not available (GLES) — GPU timings disabled");
        return;
    }
    s_gpu_ok = 1;
    glGenQueries((GLsizei)(PROF_N * 2), &s_gpu_q[0][0]);
    if (glGetError() != GL_NO_ERROR) {
        s_gpu_ok = 0;
        SDL_Log("profiler: glGenQueries failed — GPU timings disabled");
    }
}

void prof_reset(void)
{
    s_panel = cvar_get("profiler") > 0.5f;
    if (!s_panel) {
        // Панель скрыта: тайминги не собираем, queries чистим — после
        // включения панели первый кадр честно покажет нули, без призраков
        for (int i = 0; i < PROF_N; i++) {
            s_ms[i] = 0.0f;
            s_gpu_ms[i] = 0.0f;
            s_issued[i][0] = s_issued[i][1] = 0;
            s_active[i] = 0;
        }
        g_prof_draws = 0;
        g_prof_verts = 0;
        g_prof_tris = 0;
        s_parity ^= 1;
        return;
    }
    for (int i = 0; i < PROF_N; i++) s_ms[i] = 0.0f;
    g_prof_draws = 0;
    g_prof_verts = 0;
    g_prof_tris = 0;
    if (s_gpu_ok < 0) gpu_init();
    // читаем результат query, начатой 2 кадра назад
    int prev = s_parity ^ 1;
    if (s_gpu_ok == 1 && p_glGetQueryObjectui64v && p_glGetQueryObjectiv) {
        for (int i = 0; i < PROF_N; i++) {
            if (!s_issued[i][prev]) {
                s_gpu_ms[i] = 0.0f;
                continue;
            }
            GLint ready = 0;
            if (s_gpu_q[i][prev])
                glGetQueryObjectiv(s_gpu_q[i][prev],
                                   0x8867 /*GL_QUERY_RESULT_AVAILABLE*/, &ready);
            if (!ready) continue;
            s_issued[i][prev] = 0;
            GLuint64 v = 0;
            glGetQueryObjectui64v(s_gpu_q[i][prev], 0x8866 /*GL_QUERY_RESULT*/, &v);
            s_gpu_ms[i] = (float)((double)v / 1000000.0);
        }
    } else {
        for (int i = 0; i < PROF_N; i++) s_gpu_ms[i] = 0.0f;
    }
    s_parity ^= 1;
}

void prof_gpu_begin(ProfSlot s)
{
    if (!s_panel) return;
    if (s_gpu_ok < 0) gpu_init();
    if (s_gpu_ok == 0) return;
    if (s < 0 || s >= PROF_N || s_active[s]) return;
    if (!p_glBeginQuery) return;
    if (s_issued[s][s_parity]) return;
    s_active[s] = 1;
    s_issued[s][s_parity] = 1;
    glBeginQuery(GL_TIME_ELAPSED, s_gpu_q[s][s_parity]);
}

void prof_gpu_end(ProfSlot s)
{
    if (!s_panel || s_gpu_ok <= 0) return;
    if (s < 0 || s >= PROF_N || !s_active[s]) return;
    if (!p_glEndQuery) return;
    glEndQuery(GL_TIME_ELAPSED);
    s_active[s] = 0;
}

float prof_gpu_ms(ProfSlot s)
{
    return (s >= 0 && s < PROF_N) ? s_gpu_ms[s] : 0.0f;
}

void prof_add_ms(ProfSlot s, float ms)
{
    if (s >= 0 && s < PROF_N) s_ms[s] += ms;
}

const char *prof_name(ProfSlot s)
{
    static const char *names[PROF_N] = {
        "Sky", "Terrain", "Vegetation", "Objects", "Fauna", "Field",
        "Selection", "Minimap", "HUD", "Shadows", "Fog", "Bloom", "SSR", "GI"
    };
    return (s >= 0 && s < PROF_N) ? names[s] : "?";
}

float prof_ms(ProfSlot s)
{
    return (s >= 0 && s < PROF_N) ? s_ms[s] : 0.0f;
}

float prof_total_ms(void)
{
    float t = 0.0f;
    for (int i = 0; i < PROF_N; i++) t += s_ms[i];
    return t;
}

float prof_gpu_total_ms(void)
{
    float t = 0.0f;
    for (int i = 0; i < PROF_N; i++) t += s_gpu_ms[i];
    return t;
}
