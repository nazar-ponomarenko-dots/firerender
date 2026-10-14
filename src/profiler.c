// ============================================================================
//  profiler.c — per-module timings (see profiler.h)
// ============================================================================
#include "profiler.h"

#include "cvars.h"
#include "gl_loader.h"
#include <SDL3/SDL.h>

static float s_ms[PROF_N];

// GPU timings: 2 queries per slot (alternating frames — we read the result
// of the one that started 2 frames ago: the GPU has definitely finished)
static GLuint s_gpu_q[PROF_N][2];
static float s_gpu_ms[PROF_N];
static int s_gpu_ok = -1;
static int s_parity = 0;
// Did the slot actually draw in a frame of this parity? Otherwise the query
// holds a STALE result (GL does not reset query objects) and the panel would
// show the time of a disabled pass (e.g. fog with r_fog_mode 0).
static int s_issued[PROF_N][2];
static int s_active[PROF_N];
// Is the panel visible? Checked once per frame (prof_reset): with the panel
// hidden queries are not issued at all — time queries are not free on weak GPUs.
static int s_panel = 0;

static void gpu_init(void)
{
    if (s_gpu_ok >= 0) return;
    // On GLES without the timer_query extension the pointers are NULL — don't crash, CPU timings only
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
        // Panel hidden: don't collect timings, clear the queries — after
        // re-enabling the panel the first frame honestly shows zeros, no ghosts
        for (int i = 0; i < PROF_N; i++) {
            s_ms[i] = 0.0f;
            s_gpu_ms[i] = 0.0f;
            s_issued[i][0] = s_issued[i][1] = 0;
            s_active[i] = 0;
        }
        g_prof_draws = 0;
        g_prof_verts = 0;
        g_prof_tris = 0;
        g_prof_state_skipped = 0;
        s_parity ^= 1;
        return;
    }
    for (int i = 0; i < PROF_N; i++) s_ms[i] = 0.0f;
    g_prof_draws = 0;
    g_prof_verts = 0;
    g_prof_tris = 0;
    g_prof_state_skipped = 0;
    if (s_gpu_ok < 0) gpu_init();
    // read the result of the query started 2 frames ago
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
