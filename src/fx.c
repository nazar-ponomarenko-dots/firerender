// ============================================================================
//  fx.c — эффекты из частиц: именованные пресеты и непрерывные эмиттеры
// ----------------------------------------------------------------------------
//  Пресеты — как «particle definitions» в HL2: имя -> ParticleParams + count.
//  Встроенные эффекты регистрируются в fx_init_console() (вызывается при
//  инициализации консоли). Эмиттеры спавнят частицы пресета с заданной
//  частотой в области [pos±ext]; обновляются из fx_update (тик частиц).
//
//  Консоль:
//    fx <имя> [x y z]        — burst эффекта (по умолчанию у игрока)
//    fx_burst <имя> <x> <y> <z> [count]
//    fx_emit <имя> <x> <y> <z> <rate> [life]  — эмиттер в точке
//    fx_stop <имя>           — остановить эмиттеры имени
//    fx_clear                — остановить все эмиттеры + очистить пул
//    fx_list                 — список пресетов
//  Lua: engine.fx_burst/fx_emit/fx_stop/fx_clear (см. lua_engine.c).
// ============================================================================
#include "fx.h"

#include "audio.h"
#include "camera.h"
#include "console.h"
#include "cvars.h"
#include "game.h"

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include <SDL3/SDL.h>

// --- Реестр пресетов ----------------------------------------------------------
static FxPreset s_presets[FX_MAX_PRESETS];
static int s_presets_n = 0;

static FxPreset *fx_find_slot(const char *name)
{
    for (int i = 0; i < s_presets_n; i++)
        if (strcmp(s_presets[i].name, name) == 0) return &s_presets[i];
    if (s_presets_n >= FX_MAX_PRESETS) return NULL;
    return &s_presets[s_presets_n++];
}

void fx_register(const char *name, const ParticleParams *pp, int count)
{
    if (!name || !pp) return;
    FxPreset *f = fx_find_slot(name);
    if (!f) return;
    snprintf(f->name, sizeof f->name, "%s", name);
    f->pp = *pp;
    f->count = count > 0 ? count : 1;
}

const FxPreset *fx_find(const char *name)
{
    for (int i = 0; i < s_presets_n; i++)
        if (strcmp(s_presets[i].name, name) == 0) return &s_presets[i];
    return NULL;
}

void fx_burst(const char *name, float x, float y, float z, int count)
{
    const FxPreset *f = fx_find(name);
    if (!f) return;
    ParticleParams pp = f->pp;
    pp.pos[0] = x; pp.pos[1] = y; pp.pos[2] = z;
    particles_burst(&pp, count > 0 ? count : f->count);
}

// --- Эмиттеры ------------------------------------------------------------------
typedef struct {
    char name[FX_NAME_LEN];
    int active;
    int follow;        // 1 = центр следует за камерой (погода)
    float y_off;       // вертикальное смещение центра от камеры (для follow)
    float pos[3], ext[3];
    float base_ext[3]; // исходный бокс (у дождя растёт с интенсивностью)
    float rate;        // частиц/сек (текущая; см. base_rate)
    float base_rate;   // исходная частота (ramp погоды её масштабирует)
    float accum;       // накопитель времени
    float life;        // остаток времени (<0 = бесконечно)
    FxPreset preset;   // копия (чтобы не зависеть от реестра)
} FxEmitter;

static FxEmitter s_emitters[FX_MAX_EMITTERS];

// --- Кинематографичная погода (дождь) -----------------------------------------
// Дождь НАРАСТАЕТ: интенсивность 0->1 за ~4 с (плавная s-кривая) — морось
// переходит в ливень: частота, бокс эмиттера и косой ветер растут вместе с
// ней. Ветер звучит громче с интенсивностью (audio_loop_mul_volume).
// У земли — брызги. Без молний, вспышек и грома.
static float s_weather_time = 0.0f;   // время роста дождя (ramp), с
static int s_weather_on = 0;          // дождь активен (эмиттер жив)
static float s_gust_t = 0.0f;         // время для шумов ветра
static float s_splash_acc = 0.0f;     // накопитель брызг

// Интенсивность дождя 0..1 из времени нарастания (плавный s-изгиб).
float fx_weather_intensity(void);

float fx_weather_intensity(void)
{
    if (!s_weather_on) return 0.0f;
    float k = s_weather_time / 4.0f;
    if (k > 1.0f) k = 1.0f;
    return k * k * (3.0f - 2.0f * k);   // smoothstep 0..1
}

// --- Wetness state -----------------------------------------------------------
static float s_wetness = 0.0f;         // текущий уровень мокрости 0..1
static float s_wetness_target = 0.0f;  // целевой (1 под дождём, 0 без)
static float s_wetness_ramp_up = 1.0f;   // скорость нарастания (сек^-1) — 1 сек до 1.0
static float s_wetness_ramp_down = 0.18f; // скорость высыхания (сек^-1) — ~5.5 сек до 0
static float s_wet_force = -1.0f;      // принудительная мокрость (<0 = выкл, тест)

static FxEmitter *emitter_find(const char *name)
{
    for (int i = 0; i < FX_MAX_EMITTERS; i++)
        if (s_emitters[i].active && strcmp(s_emitters[i].name, name) == 0)
            return &s_emitters[i];
    return NULL;
}

void fx_emitter_start(const char *name, const float *pos, const float *ext,
                      float rate, float life)
{
    const FxPreset *f = fx_find(name);
    if (!f || !pos) return;
    FxEmitter *e = emitter_find(name);
    if (!e) {
        for (int i = 0; i < FX_MAX_EMITTERS; i++) {
            if (!s_emitters[i].active) { e = &s_emitters[i]; break; }
        }
    }
    if (!e) return;   // лимит эмиттеров
    snprintf(e->name, sizeof e->name, "%s", name);
    e->active = 1;
    e->follow = 0;   // старый эмиттер мог быть погодным — сбрасываем,
    e->y_off = 0;    // иначе «закреплённый» эмиттер продолжит следовать за камерой
    e->pos[0] = pos[0]; e->pos[1] = pos[1]; e->pos[2] = pos[2];
    e->ext[0] = ext ? ext[0] : 0.0f;
    e->ext[1] = ext ? ext[1] : 0.0f;
    e->ext[2] = ext ? ext[2] : 0.0f;
    e->base_ext[0] = e->ext[0];
    e->base_ext[1] = e->ext[1];
    e->base_ext[2] = e->ext[2];
    e->rate = rate > 0.0f ? rate : 1.0f;
    e->base_rate = e->rate;
    e->accum = 0.0f;
    e->life = life > 0.0f ? life : -1.0f;
    e->preset = *f;
}

void fx_emitter_stop(const char *name)
{
    FxEmitter *e = emitter_find(name);
    if (e) e->active = 0;
}

// Погода: центр следует за камерой (x,z игрока), y = y камеры + y_off.
void fx_weather_start(const char *name, float y_off, const float *ext,
                      float rate, float life)
{
    Game *g = game_instance();
    if (!g || !g->cam) return;
    float pos[3] = { g->cam->cam.pos[0], g->cam->cam.pos[1] + y_off,
                     g->cam->cam.pos[2] };
    fx_emitter_start(name, pos, ext, rate, life);
    FxEmitter *e = emitter_find(name);
    if (e) { e->follow = 1; e->y_off = y_off; }
    // Дождь: запускаем кинематографичное нарастание (звук — только ветер).
    if (strcmp(name, "rain") == 0) {
        s_weather_on = 1;
        s_weather_time = 0.0f;
    }
}

void fx_emitter_stop_all(void)
{
    for (int i = 0; i < FX_MAX_EMITTERS; i++) s_emitters[i].active = 0;
}

// Порывы ветра: два синуса + шум дают живые непериодические толчки.
// Возвращает текущую силу порыва 0..1.
static float wind_gust(void)
{
    float g = 0.6f
            + 0.25f * sinf(s_gust_t * 0.55f) * sinf(s_gust_t * 0.21f + 1.7f)
            + 0.15f * sinf(s_gust_t * 1.37f + 0.4f);
    if (g < 0.0f) g = 0.0f;
    if (g > 1.0f) g = 1.0f;
    return g;
}

void fx_update(float dt)
{
    Game *g = game_instance();
    float camx = 0, camy = 0, camz = 0;
    if (g && g->cam) {
        camx = g->cam->cam.pos[0];
        camy = g->cam->cam.pos[1];
        camz = g->cam->cam.pos[2];
    }
    s_gust_t += dt;

    // Проверка активного дождя для мокрости
    int rain_active = 0;
    for (int i = 0; i < FX_MAX_EMITTERS; i++) {
        if (s_emitters[i].active && s_emitters[i].follow &&
            strcmp(s_emitters[i].name, "rain") == 0) {
            rain_active = 1;
            break;
        }
    }
    if (!rain_active) {
        // Дождь закончился: сбрасываем нарастание, возвращаем громкость ветра
        if (s_weather_on) { s_weather_on = 0; s_weather_time = 0.0f; }
        audio_loop_mul_volume(1.0f);
    }

    // Кинематографичное нарастание: интенсивность 0->1 за ~4 с
    float inten = 0.0f;
    if (rain_active) {
        if (s_weather_time < 4.0f) s_weather_time += dt;
        inten = fx_weather_intensity();
        // Ветер громче с интенсивностью ливня (без отдельного звука дождя)
        audio_loop_mul_volume(1.0f + 1.2f * inten);
    }

    // Ветер: базовый + порывы, растёт с интенсивностью дождя
    float gust = wind_gust() * inten;
    float wind_x = 3.5f + 3.0f * gust;
    float wind_z = 1.2f + 1.8f * gust;

    for (int i = 0; i < FX_MAX_EMITTERS; i++) {
        FxEmitter *e = &s_emitters[i];
        if (!e->active) continue;
        if (e->life > 0.0f) {
            e->life -= dt;
            if (e->life <= 0.0f) { e->active = 0; continue; }
        }
        // Погода следует за игроком
        if (e->follow) {
            e->pos[0] = camx;
            e->pos[1] = camy + e->y_off;
            e->pos[2] = camz;
        }
        // Дождь: ramp масштабирует частоту и бокс (морось -> ливень),
        // плюс косой ветер по текущему порыву.
        if (strcmp(e->name, "rain") == 0) {
            e->rate = e->base_rate * (0.15f + 0.85f * inten);
            float bk = 0.5f + 0.5f * inten;
            e->ext[0] = e->base_ext[0] * bk;
            e->ext[2] = e->base_ext[2] * bk;
        }
        e->accum += e->rate * dt;
        int n = (int)e->accum;
        e->accum -= n;
        // Частицы в области [pos±ext], вертикаль от верха бокса вниз
        ParticleParams pp = e->preset.pp;
        if (strcmp(e->name, "rain") == 0) {
            pp.wind[0] = wind_x;
            pp.wind[2] = wind_z;
        }
        for (int k = 0; k < n; k++) {
            float rx = e->ext[0] > 0.001f
                       ? (e->pos[0] + (rand() % 1000) / 1000.0f * 2.0f * e->ext[0] - e->ext[0])
                       : e->pos[0];
            float ry = e->ext[1] > 0.001f
                       ? (e->pos[1] + e->ext[1] * 0.5f - (rand() % 1000) / 1000.0f * e->ext[1])
                       : e->pos[1];
            float rz = e->ext[2] > 0.001f
                       ? (e->pos[2] + (rand() % 1000) / 1000.0f * 2.0f * e->ext[2] - e->ext[2])
                       : e->pos[2];
            pp.pos[0] = rx; pp.pos[1] = ry; pp.pos[2] = rz;
            particles_emit(&pp);
        }
    }

    // Брызги у земли: пока идёт дождь и земля рядом, спавним мелкие всплески
    // на поверхности вокруг игрока (только когда ливень уже сильный — в
    // морось брызг не видно).
    if (inten > 0.6f && g && g->cam && g->world_loaded) {
        s_splash_acc += dt * (18.0f * inten);
        int ns = (int)s_splash_acc;
        s_splash_acc -= ns;
        if (ns > 0) {
            const FxPreset *sp = fx_find("splash");
            if (sp) {
                ParticleParams pp = sp->pp;
                for (int k = 0; k < ns; k++) {
                    float rx = camx + ((rand() % 1000) / 1000.0f * 2.0f - 1.0f) * 6.0f;
                    float rz = camz + ((rand() % 1000) / 1000.0f * 2.0f - 1.0f) * 6.0f;
                    float h = sample_height(&g->world, rx, rz);
                    pp.pos[0] = rx; pp.pos[1] = h + 0.03f; pp.pos[2] = rz;
                    particles_emit(&pp);
                }
            }
        }
    }

    // Wetness ramp: нарастает под дождём, сохнет без.
    // s_wet_force >= 0 — принудительная мокрость (тест, без дождя).
    // Мокрость следует за интенсивностью ливня, а не за фактом запуска —
    // земля намокает постепенно вместе с усилением дождя.
    s_wetness_target = rain_active ? inten : 0.0f;
    if (s_wet_force >= 0.0f) s_wetness_target = s_wet_force;
    if (s_wetness < s_wetness_target)
        s_wetness = s_wetness + s_wetness_ramp_up * dt > s_wetness_target ? s_wetness_target : s_wetness + s_wetness_ramp_up * dt;
    else if (s_wetness > s_wetness_target)
        s_wetness = s_wetness - s_wetness_ramp_down * dt < s_wetness_target ? s_wetness_target : s_wetness - s_wetness_ramp_down * dt;
}

// --- Wetness getters ---------------------------------------------------------
float fx_wetness(void)
{
    return s_wetness;
}

void fx_force_wetness(float v)
{
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    s_wet_force = v;      // держит мокрость каждый тик (тест/консоль)
    s_wetness = v;
    s_wetness_target = v;
}

float fx_wetness_target(void)
{
    return s_wetness_target;
}

// --- Консоль и управление ------------------------------------------------------
void fx_print_list(void)
{
    console_print("fx пресеты (%d):", s_presets_n);
    for (int i = 0; i < s_presets_n; i++)
        console_print("  %s (%d)", s_presets[i].name, s_presets[i].count);
}

// Разбор "x y z" из строки (если не все — берём позицию игрока)
static void fx_pos_args(const char *args, float *out)
{
    Game *g = game_instance();
    if (g && g->cam) {
        out[0] = g->cam->cam.pos[0];
        out[1] = g->cam->cam.pos[1];
        out[2] = g->cam->cam.pos[2];
    } else {
        out[0] = out[1] = out[2] = 0.0f;
    }
    if (!args || !args[0]) return;
    float x, y, z;
    if (sscanf(args, "%f %f %f", &x, &y, &z) == 3) {
        out[0] = x; out[1] = y; out[2] = z;
    }
}

void cmd_fx_list(const char *args)
{
    (void)args;
    fx_print_list();
}

void cmd_fx(const char *args)
{
    if (!args || !args[0]) { fx_print_list(); return; }
    float pos[3];
    fx_pos_args(args, pos);
    char name[FX_NAME_LEN];
    if (sscanf(args, "%31s", name) == 1) {
        if (!fx_find(name)) {
            console_print("fx: нет пресета \"%s\" (fx_list)", name);
            return;
        }
        fx_burst(name, pos[0], pos[1], pos[2], 0);
    }
}

void cmd_fx_burst(const char *args)
{
    char name[FX_NAME_LEN];
    float x = 0, y = 0, z = 0;
    int count = 0;
    int n = sscanf(args, "%31s %f %f %f %d", name, &x, &y, &z, &count);
    if (n < 1 || !fx_find(name)) {
        console_print("usage: fx_burst <имя> <x> <y> <z> [count]");
        return;
    }
    fx_burst(name, x, y, z, count);
}

void cmd_fx_emit(const char *args)
{
    char name[FX_NAME_LEN];
    float x = 0, y = 0, z = 0, rate = 10.0f, life = 0.0f;
    if (sscanf(args, "%31s %f %f %f %f %f", name, &x, &y, &z, &rate, &life) < 4) {
        console_print("usage: fx_emit <имя> <x> <y> <z> <rate> [life]");
        return;
    }
    if (!fx_find(name)) { console_print("fx: нет пресета \"%s\"", name); return; }
    const float pos[3] = { x, y, z };
    fx_emitter_start(name, pos, NULL, rate, life);
    console_print("fx_emit: %s rate=%g", name, rate);
}

void cmd_fx_stop(const char *args)
{
    if (!args || !args[0]) { fx_emitter_stop_all(); console_print("fx: все эмиттеры остановлены"); return; }
    char name[FX_NAME_LEN];
    if (sscanf(args, "%31s", name) == 1) {
        fx_emitter_stop(name);
        console_print("fx: остановлен %s", name);
    }
}

void cmd_fx_clear(const char *args)
{
    (void)args;
    fx_emitter_stop_all();
    particles_clear();
    console_print("fx: очищено");
}

// weather <имя> [rate] — дождь/снег/... следует за игроком; weather off — стоп
void cmd_weather(const char *args)
{
    if (!args || !args[0]) {
        console_print("usage: weather <пресет> [rate] | weather off");
        return;
    }
    char name[FX_NAME_LEN];
    float rate = 0.0f;
    if (sscanf(args, "%31s %f", name, &rate) < 1) return;
    if (strcmp(name, "off") == 0 || strcmp(name, "clear") == 0) {
        fx_emitter_stop_all();
        console_print("weather: остановлено");
        return;
    }
    if (!fx_find(name)) {
        console_print("weather: нет пресета \"%s\" (fx_list)", name);
        return;
    }
    if (rate <= 0.0f) rate = 1000.0f;
    float ext[3] = { 20.0f, 10.0f, 20.0f };
    fx_weather_start(name, 12.0f, ext, rate, 0.0f);
    console_print("weather: %s rate=%g (следует за игроком)", name, rate);
}

// --- Встроенные пресеты ---------------------------------------------------------
void fx_register_builtin(void)
{
    // Кровь: красные капли, отскок, короткая жизнь
    {
        ParticleParams pp = { .dir = {0,1,0}, .spread = 0.5f,
            .speed0 = 2.0f, .speed1 = 5.0f,
            .life0 = 0.8f, .life1 = 1.6f,
            .size0 = 0.05f, .size1 = 0.02f,
            .alpha0 = 1.0f, .alpha1 = 0.3f,
            .r = 0.55f, .g = 0.02f, .b = 0.02f,
            .gravity = 8.0f, .drag = 0.6f, .bounce = 1 };
        fx_register("blood", &pp, 24);
    }
    // Дым: серый, растёт, всплывает
    {
        ParticleParams pp = { .dir = {0,1,0}, .spread = 0.3f,
            .speed0 = 0.4f, .speed1 = 1.2f,
            .life0 = 1.5f, .life1 = 3.0f,
            .size0 = 0.4f, .size1 = 2.2f,
            .alpha0 = 0.5f, .alpha1 = 0.0f,
            .r = 0.35f, .g = 0.35f, .b = 0.35f,
            .gravity = -0.5f, .drag = 1.5f, .sway = 0.4f };
        fx_register("smoke", &pp, 16);
    }
    // Взрыв: вспышка (аддитив) + искры (аддитив, отскок)
    {
        ParticleParams pp = { .dir = {0,1,0}, .spread = 0.4f,
            .speed0 = 1.0f, .speed1 = 4.0f,
            .life0 = 0.2f, .life1 = 0.6f,
            .size0 = 1.5f, .size1 = 3.0f,
            .alpha0 = 1.0f, .alpha1 = 0.0f,
            .r = 1.0f, .g = 0.85f, .b = 0.5f,
            .gravity = 0.5f, .blend = 1 };
        fx_register("explosion", &pp, 20);
    }
    {
        ParticleParams pp = { .dir = {0,1,0}, .spread = 0.9f,
            .speed0 = 4.0f, .speed1 = 10.0f,
            .life0 = 0.5f, .life1 = 1.2f,
            .size0 = 0.12f, .size1 = 0.03f,
            .alpha0 = 1.0f, .alpha1 = 0.0f,
            .r = 1.0f, .g = 0.6f, .b = 0.2f,
            .gravity = 3.0f, .drag = 0.8f, .bounce = 1, .blend = 1 };
        fx_register("sparks", &pp, 40);
    }
    // Огонь: аддитив, поднимается, мерцает
    {
        ParticleParams pp = { .dir = {0,1,0}, .spread = 0.35f,
            .speed0 = 0.6f, .speed1 = 1.6f,
            .life0 = 0.4f, .life1 = 0.9f,
            .size0 = 0.35f, .size1 = 0.1f,
            .alpha0 = 0.9f, .alpha1 = 0.0f,
            .r = 1.0f, .g = 0.6f, .b = 0.15f,
            .gravity = -1.0f, .drag = 1.2f, .sway = 0.6f, .blend = 1 };
        fx_register("fire", &pp, 6);
    }
    // Дождь: тонкие стрики вниз, быстрые, слегка голубые. Ветер (косые
    // струи + порывы) подставляется каждый кадр в fx_update из интенсивности.
    // collide=1: капля гибнет о рельеф и поверхность воды (не пролетает
    // сквозь море/озеро/склон).
    {
        ParticleParams pp = { .dir = {0,-1,0}, .spread = 0.12f,
            .speed0 = 18.0f, .speed1 = 30.0f,
            .life0 = 0.8f, .life1 = 1.4f,
            .size0 = 0.06f, .size1 = 0.04f,
            .alpha0 = 0.5f, .alpha1 = 0.0f,
            .r = 0.7f, .g = 0.8f, .b = 1.0f,
            .gravity = 6.0f, .drag = 0.1f, .streak = 1, .collide = 1 };
        fx_register("rain", &pp, 1);
    }
    // Брызги: мелкие всплески капель о землю (ливень, fx_update спавнит
    // их на рельефе вокруг игрока); растут и гаснут, аддитивные.
    {
        ParticleParams pp = { .dir = {0,1,0}, .spread = 0.6f,
            .speed0 = 0.3f, .speed1 = 1.0f,
            .life0 = 0.12f, .life1 = 0.35f,
            .size0 = 0.05f, .size1 = 0.10f,
            .alpha0 = 0.5f, .alpha1 = 0.0f,
            .r = 0.75f, .g = 0.82f, .b = 1.0f,
            .gravity = 5.0f, .drag = 1.2f };
        fx_register("splash", &pp, 1);
    }
    // Снег: медленно падает, покачивается. collide=2: ложится на рельеф
    // и поверхность воды, коротко гаснет (не уходит сквозь землю/море).
    {
        ParticleParams pp = { .dir = {0,-1,0}, .spread = 0.1f,
            .speed0 = 0.5f, .speed1 = 1.2f,
            .life0 = 4.0f, .life1 = 8.0f,
            .size0 = 0.08f, .size1 = 0.05f,
            .alpha0 = 0.9f, .alpha1 = 0.0f,
            .r = 1.0f, .g = 1.0f, .b = 1.0f,
            .gravity = 0.4f, .drag = 1.0f, .sway = 0.7f, .collide = 2 };
        fx_register("snow", &pp, 1);
    }
    // Пыль (шаги/сухость): лёгкая, серая, покачивается
    {
        ParticleParams pp = { .dir = {0,1,0}, .spread = 0.6f,
            .speed0 = 0.3f, .speed1 = 1.0f,
            .life0 = 1.0f, .life1 = 2.0f,
            .size0 = 0.15f, .size1 = 0.5f,
            .alpha0 = 0.3f, .alpha1 = 0.0f,
            .r = 0.6f, .g = 0.55f, .b = 0.5f,
            .gravity = 0.2f, .drag = 1.5f, .sway = 0.5f };
        fx_register("dust", &pp, 10);
    }
    // Вспышка выстрела (muzzle): аддитивная, короткая, вперёд
    {
        ParticleParams pp = { .dir = {0,1,0}, .spread = 0.2f,
            .speed0 = 0.5f, .speed1 = 1.5f,
            .life0 = 0.05f, .life1 = 0.12f,
            .size0 = 0.3f, .size1 = 0.6f,
            .alpha0 = 1.0f, .alpha1 = 0.0f,
            .r = 1.0f, .g = 0.9f, .b = 0.6f,
            .gravity = 0.0f, .blend = 1 };
        fx_register("muzzle_flash", &pp, 4);
    }
}

void fx_init_console(void)
{
    fx_register_builtin();
}
