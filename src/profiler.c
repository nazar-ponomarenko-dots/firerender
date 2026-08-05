// ============================================================================
//  profiler.c — тайминги по модулям (см. profiler.h)
// ============================================================================
#include "profiler.h"

#include "gl_loader.h"   // g_prof_draws / g_prof_verts / g_prof_tris

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

static void gpu_init(void)
{
    if (s_gpu_ok >= 0) return;
    s_gpu_ok = 1;
    glGenQueries((GLsizei)(PROF_N * 2), &s_gpu_q[0][0]);
}

void prof_reset(void)
{
    for (int i = 0; i < PROF_N; i++) s_ms[i] = 0.0f;
    g_prof_draws = 0;
    g_prof_verts = 0;
    g_prof_tris = 0;
    if (s_gpu_ok < 0) gpu_init();
    // читаем результат query, начатой 2 кадра назад
    int prev = s_parity ^ 1;
    for (int i = 0; i < PROF_N; i++) {
        if (!s_issued[i][prev]) {          // проход не рисовался — время 0
            s_gpu_ms[i] = 0.0f;
            continue;
        }
        GLuint64 v = 0;
        if (s_gpu_q[i][prev])
            glGetQueryObjectui64v(s_gpu_q[i][prev], 0x8866 /*GL_QUERY_RESULT*/,
                                  &v);
        s_gpu_ms[i] = (float)((double)v / 1000000.0);
    }
    s_parity ^= 1;
}

void prof_gpu_begin(ProfSlot s)
{
    if (s_gpu_ok < 0) gpu_init();
    if (s < 0 || s >= PROF_N) return;
    s_issued[s][s_parity] = 1;
    glBeginQuery(GL_TIME_ELAPSED, s_gpu_q[s][s_parity]);
}

void prof_gpu_end(ProfSlot s)
{
    if (s < 0 || s >= PROF_N) return;
    glEndQuery(GL_TIME_ELAPSED);
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
        "Selection", "Minimap", "HUD", "Shadows", "Fog", "Bloom"
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
