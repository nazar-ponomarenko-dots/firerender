#include "lowend_profile.h"

#include "cvars.h"

#include <SDL3/SDL.h>

static LowendProfile s_active;
static float s_avg_ms;
static float s_cooldown;

static LowendProfile select_profile(void)
{
    int requested = (int)cvar_get("r_lowend_profile");
    if (requested >= LOWEND_MOBILE_LEGACY && requested <= LOWEND_IGPU_LEGACY)
        return (LowendProfile)requested;
#ifdef USE_GLES_PATH
    return LOWEND_MOBILE_LEGACY;
#else
    return LOWEND_BALANCED;
#endif
}

void lowend_profile_init(void)
{
    cvar_register_ex("r_lowend_profile", 0.0f, 0.0f, 2.0f, CVAR_ARCHIVE,
                     "low-end профиль: 0=balanced, 1=старый смартфон, 2=Celeron iGPU");
    cvar_register_ex("r_dynamic_resolution", 0.0f, 0.0f, 1.0f, CVAR_ARCHIVE,
                     "динамический масштаб сцены по времени кадра");
    cvar_register_ex("r_dynamic_target_ms", 33.3f, 16.6f, 50.0f, CVAR_ARCHIVE,
                     "целевое время кадра dynamic resolution");
    s_active = select_profile();
}

void lowend_profile_apply(void)
{
    s_active = select_profile();
    /* Профиль выбирает backend policy, но не имеет права менять картинку
       выбранного пользователем пресета. Масштаб сцены всегда нативный. */
    cvar_set("r_render_scale", 1.0f);
    cvar_set("r_dynamic_resolution", 0.0f);
    if (s_active != LOWEND_BALANCED)
        SDL_Log("Low-end profile: %s (native quality contract)",
                lowend_profile_name());
}

void lowend_profile_frame(float frame_ms)
{
    s_active = select_profile();
    if (s_active == LOWEND_BALANCED || cvar_get("r_dynamic_resolution") < 0.5f)
        return;
    if (frame_ms <= 0.0f || frame_ms > 250.0f) return;
    s_avg_ms = s_avg_ms > 0.0f ? s_avg_ms * 0.96f + frame_ms * 0.04f : frame_ms;
    if (s_cooldown > 0.0f) { s_cooldown -= frame_ms; return; }
    float target = cvar_get("r_dynamic_target_ms");
    float scale = cvar_get("r_render_scale");
    float min_scale = s_active == LOWEND_MOBILE_LEGACY ? 0.50f : 0.60f;
    float max_scale = s_active == LOWEND_MOBILE_LEGACY ? 0.75f : 0.90f;
    float next = scale;
    if (s_avg_ms > target * 1.10f) next -= 0.05f;
    else if (s_avg_ms < target * 0.82f) next += 0.05f;
    if (next < min_scale) next = min_scale;
    if (next > max_scale) next = max_scale;
    next = (float)((int)(next * 20.0f + 0.5f)) / 20.0f;
    if (next != scale) {
        cvar_set("r_render_scale", next);
        s_cooldown = 1500.0f;
    }
}

LowendProfile lowend_profile_active(void) { return s_active; }

const char *lowend_profile_name(void)
{
    if (s_active == LOWEND_MOBILE_LEGACY) return "mobile_legacy";
    if (s_active == LOWEND_IGPU_LEGACY) return "igpu_legacy";
    return "balanced";
}

float lowend_profile_scale(void) { return cvar_get("r_render_scale"); }
