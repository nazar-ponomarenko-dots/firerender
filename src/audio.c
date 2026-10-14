// ============================================================================
//  audio.c — реализация звука (см. audio.h)
// ----------------------------------------------------------------------------
//  Устройство + N каналов (SDL_AudioStream, привязаны к устройству —
//  микширует SDL). Каждый Sound хранит wav-данные и spec; канал
//  конвертирует spec файла в spec устройства автоматически.
// ============================================================================
#include "audio.h"

#include "cvars.h"
#include "console.h"
#include "mapdata.h"
#include "resources.h"
#include "world.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AUDIO_CHANNELS 8     // одновременных звуков (шаги, клики, ...)

struct Sound {
    char path[128];
    SDL_AudioSpec spec;      // формат wav-файла
    Uint8 *data;             // pcm-данные (владеем)
    Uint32 len;              // размер в байтах
};

static SDL_AudioDeviceID s_dev = 0;
static SDL_AudioSpec s_dev_spec;           // формат устройства
static SDL_AudioStream *s_chan[AUDIO_CHANNELS];
static Sound *s_chan_snd[AUDIO_CHANNELS];  // звук, играющий на канале

// Фоновая петля
static SDL_AudioStream *s_loop = NULL;
static Sound *s_loop_snd = NULL;
static float s_loop_vol = 0.0f;
static Uint32 s_loop_sent = 0;
// Множитель громкости петли (погода: в дождь ветер громче). 1 = без изменений
static float s_loop_mul = 1.0f;

// Музыка карты (вторая петля, независимая от ambient)
static SDL_AudioStream *s_music = NULL;
static Sound *s_music_snd = NULL;
static float s_music_vol = 0.0f;

// Позиционные (3D) источники: до 8 каналов, громкость от дистанции.
// У каждого — два эхо-потока (реверб): с дистанцией звук тише, а эхо
// с задержкой ~90/180 мс — громче (признак "издалека").
#define AUDIO_3D_CHANNELS 8
#define AUDIO_ECHO_N 2          // число эхо-повторов
#define AUDIO_ECHO_DELAY_MS 90
typedef struct {
    SDL_AudioStream *stream;
    SDL_AudioStream *echo[AUDIO_ECHO_N];   // эхо-потоки
    Sound *snd;
    float pos[3];
    float vol;
    float radius;
    int loop;              // 1 — петля, 0 — один раз (канал освобождается)
    int active;
} Audio3D;
static Audio3D s_3d[AUDIO_3D_CHANNELS];
static float s_listener[3] = { 0, 0, 0 };
static const struct WorldState *s_world = NULL;   // окклюзия: карта высот

// Кеш звуков
#define AUDIO_CACHE 16
static Sound *s_cache[AUDIO_CACHE];
static int s_cache_n = 0;

// Итоговая громкость: общая (snd_master) x категория (sfx/music/dialog).
// Читается каждый кадр — ползунки меню действуют мгновенно.
static float snd_gain(const char *cat)
{
    return cvar_get("snd_master") * cvar_get(cat);
}

void audio_init(void)
{
    // Просим моно 16-bit 22050 (или что даст устройство)
    SDL_AudioSpec want = { 0 };
    want.format = SDL_AUDIO_S16;
    want.channels = 1;
    want.freq = 22050;
    s_dev = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &want);
    if (!s_dev) {
        SDL_Log("Audio init failed: %s", SDL_GetError());
        return;
    }
    SDL_GetAudioDeviceFormat(s_dev, &s_dev_spec, NULL);
    SDL_Log("Audio device: %d Hz, %d ch", s_dev_spec.freq, s_dev_spec.channels);

    for (int i = 0; i < AUDIO_CHANNELS; i++) {
        s_chan[i] = SDL_CreateAudioStream(NULL, &s_dev_spec);
        if (s_chan[i]) SDL_BindAudioStream(s_dev, s_chan[i]);
        s_chan_snd[i] = NULL;
    }
    s_loop = SDL_CreateAudioStream(NULL, &s_dev_spec);
    if (s_loop) SDL_BindAudioStream(s_dev, s_loop);
    s_music = SDL_CreateAudioStream(NULL, &s_dev_spec);
    if (s_music) SDL_BindAudioStream(s_dev, s_music);
    for (int i = 0; i < AUDIO_3D_CHANNELS; i++) {
        s_3d[i].stream = SDL_CreateAudioStream(NULL, &s_dev_spec);
        if (s_3d[i].stream) SDL_BindAudioStream(s_dev, s_3d[i].stream);
        for (int e = 0; e < AUDIO_ECHO_N; e++) {
            s_3d[i].echo[e] = SDL_CreateAudioStream(NULL, &s_dev_spec);
            if (s_3d[i].echo[e]) SDL_BindAudioStream(s_dev, s_3d[i].echo[e]);
        }
    }
}

Sound *audio_load(const char *path)
{
    if (!s_dev) return NULL;
    for (int i = 0; i < s_cache_n; i++)
        if (strcmp(s_cache[i]->path, path) == 0) return s_cache[i];

    // Через ресурсы: пак карты -> диск -> вшитые (карта может переопределить)
    unsigned int sz = 0;
    unsigned char *buf = resources_read_file(path, &sz);
    if (!buf) return NULL;

    SDL_AudioSpec spec;
    Uint8 *data = NULL;
    Uint32 len = 0;
    SDL_IOStream *io = SDL_IOFromMem(buf, sz);
    bool ok = io && SDL_LoadWAV_IO(io, true, &spec, &data, &len);
    free(buf);
    if (!ok) {
        SDL_Log("audio_load: bad wav %s", path);
        return NULL;
    }

    if (s_cache_n >= AUDIO_CACHE) {
        SDL_free(data);
        return NULL;
    }
    Sound *s = calloc(1, sizeof *s);
    if (!s) { SDL_free(data); return NULL; }
    snprintf(s->path, sizeof s->path, "%s", path);
    s->spec = spec;
    s->data = data;
    s->len = len;
    s_cache[s_cache_n++] = s;
    SDL_Log("audio: loaded %s (%.1f s)", path, (float)len / spec.freq /
            (spec.channels * (SDL_AUDIO_BITSIZE(spec.format) / 8)));
    return s;
}

// Канал: свободен, если очередь пуста и он не занят другим звуком
static SDL_AudioStream *channel_take(Sound *s, int *idx)
{
    int oldest = 0;
    for (int i = 0; i < AUDIO_CHANNELS; i++) {
        if (s_chan_snd[i] == s && SDL_GetAudioStreamQueued(s_chan[i]) <= 0)
            { *idx = i; return s_chan[i]; }
        if (s_chan_snd[i] == NULL || SDL_GetAudioStreamQueued(s_chan[i]) <= 0) {
            *idx = i;
            return s_chan[i];
        }
        // "самый старый" — где очередь меньше всех (грубая оценка)
        if (SDL_GetAudioStreamQueued(s_chan[i]) <
            SDL_GetAudioStreamQueued(s_chan[oldest]))
            oldest = i;
    }
    *idx = oldest;
    return s_chan[oldest];
}

void audio_play(Sound *s, float volume, float pitch)
{
    if (!s_dev || !s) return;
    if (volume <= 0.0f) return;
    // pitch = множитель частоты входа: поток пересэмплирует в формат
    // устройства, сдвигая и тональность, и скорость (0.5..2 = октава вниз/вверх).
    if (pitch < 0.5f) pitch = 0.5f;
    if (pitch > 2.0f) pitch = 2.0f;
    int idx = 0;
    SDL_AudioStream *ch = channel_take(s, &idx);
    if (!ch) return;
    SDL_ClearAudioStream(ch);
    // Задаём формат входа (wav) и выхода (устройство): иначе поток не
    // конвертирует 22050 моно -> 44100 стерео и звук молчит.
    SDL_AudioSpec src = s->spec;
    src.freq = (int)(src.freq * pitch + 0.5f);
    SDL_SetAudioStreamFormat(ch, &src, &s_dev_spec);
    SDL_SetAudioStreamGain(ch, volume * snd_gain("snd_sfx"));
    SDL_PutAudioStreamData(ch, s->data, (int)s->len);
    s_chan_snd[idx] = s;
}

void audio_loop_start(Sound *s, float volume)
{
    if (!s_dev || !s || !s_loop) return;
    s_loop_snd = s;
    s_loop_vol = volume;
    s_loop_sent = 0;
    SDL_ClearAudioStream(s_loop);
    SDL_SetAudioStreamFormat(s_loop, &s->spec, &s_dev_spec);  // как в audio_play
    SDL_SetAudioStreamGain(s_loop, volume * snd_gain("snd_sfx"));
    SDL_PutAudioStreamData(s_loop, s->data, (int)s->len);
    s_loop_sent = s->len;
}

void audio_loop_stop(void)
{
    if (!s_loop) return;
    SDL_ClearAudioStream(s_loop);
    s_loop_snd = NULL;
    s_loop_sent = 0;
}

void audio_loop_mul_volume(float mul)
{
    if (mul < 0.0f) mul = 0.0f;
    if (mul > 4.0f) mul = 4.0f;
    s_loop_mul = mul;   // gain (vol * mul) применяет audio_update каждый кадр
}

void audio_music_start(Sound *s, float volume)
{
    if (!s_dev || !s || !s_music) return;
    s_music_snd = s;
    s_music_vol = volume;
    SDL_ClearAudioStream(s_music);
    SDL_SetAudioStreamFormat(s_music, &s->spec, &s_dev_spec);
    SDL_SetAudioStreamGain(s_music, volume * snd_gain("snd_music"));
    SDL_PutAudioStreamData(s_music, s->data, (int)s->len);
}

void audio_music_stop(void)
{
    if (!s_music) return;
    SDL_ClearAudioStream(s_music);
    s_music_snd = NULL;
}

void audio_set_listener(const float pos[3])
{
    s_listener[0] = pos[0];
    s_listener[1] = pos[1];
    s_listener[2] = pos[2];
}

void audio_set_world(const struct WorldState *w)
{
    s_world = w;
}

// Множитель громкости из-за перекрытия линии слушатель->источник.
// 1 = прямо слышно; ниже — часть пути под землёй. snd_occlusion — сила
// эффекта (0 = выключено, поведение старших сборок), snd_occlusion_min —
// пол: даже полностью заслонённый источник остаётся едва слышным (дифракция
// вокруг холма), иначе за холмом наступала бы абсолютная тишина.
float audio_occlusion_at(const float to[3])
{
    float k = cvar_get("snd_occlusion");
    if (k <= 0.0f || !s_world || !to) return 1.0f;
    float open = world_line_open(s_world, s_listener, to);
    float fmin = cvar_get("snd_occlusion_min");
    if (fmin < 0.0f) fmin = 0.0f;
    if (fmin > 1.0f) fmin = 1.0f;
    float f = 1.0f - k * (1.0f - open) * (1.0f - fmin);
    return f < fmin ? fmin : f;
}

void cmd_snd_occlude(const char *args)
{
    float p[3];
    if (!args || sscanf(args, "%f %f %f", &p[0], &p[1], &p[2]) != 3) {
        console_print("использование: snd_occlude <x> <y> <z>");
        return;
    }
    float f = audio_occlusion_at(p);
    console_print("snd_occlude: %.2f (snd_occlusion %.2f, пол %.2f)",
                  f, cvar_get("snd_occlusion"), cvar_get("snd_occlusion_min"));
    // console_print виден только на экране — лог даёт то же число из headless.
    SDL_Log("snd_occlude: (%.0f %.0f %.0f) -> %.2f", p[0], p[1], p[2], f);
}

// --- Самопроверка (FR_TEST 'Z') ------------------------------------------------
// Рельеф синтетический (сетка 8x8 м со стеной 10 м в столбце x=4): тест не
// должен зависеть от высот конкретной карты.
int audio_selftest(void)
{
    static float h[8 * 8];
    for (int i = 0; i < 8 * 8; i++) h[i] = (i % 8 == 4) ? 10.0f : 0.0f;
    WorldState w;
    memset(&w, 0, sizeof w);
    w.heights = h;
    w.tex_w = w.tex_h = 8;
    w.world_w = w.world_d = 7.0f;

    const float from[3]   = { 0.0f, 0.0f, 3.0f };
    const float behind[3] = { 7.0f, 0.0f, 3.0f };    // за стеной, на уровне земли
    const float above[3]  = { 7.0f, 20.0f, 3.0f };   // та же точка, но над стеной
    int fails = 0;
    if (world_line_open(&w, from, behind) > 0.1f) {
        SDL_Log("audio selftest FAIL: стена не перекрыла прямую");
        fails++;
    }
    if (world_line_open(&w, from, above) < 0.99f) {
        SDL_Log("audio selftest FAIL: путь над стеной считается закрытым");
        fails++;
    }
    // Громкость: выключенная окклюзия = 1.0, включённая — до самого пола.
    float k0 = cvar_get("snd_occlusion"), m0 = cvar_get("snd_occlusion_min");
    audio_set_world(&w);
    audio_set_listener(from);
    cvar_set("snd_occlusion", 0.0f);
    if (audio_occlusion_at(behind) < 0.99f) {
        SDL_Log("audio selftest FAIL: snd_occlusion 0 всё равно глушит");
        fails++;
    }
    cvar_set("snd_occlusion", 1.0f);
    cvar_set("snd_occlusion_min", 0.25f);
    float full = audio_occlusion_at(behind);
    if (full > 0.26f || full < 0.24f) {
        SDL_Log("audio selftest FAIL: за стеной %.2f, ожидался пол 0.25", full);
        fails++;
    }
    if (audio_occlusion_at(above) < 0.99f) {
        SDL_Log("audio selftest FAIL: над стеной глуше, чем нужно");
        fails++;
    }
    cvar_set("snd_occlusion", k0);
    cvar_set("snd_occlusion_min", m0);
    audio_set_world(NULL);
    return fails;
}

// То же на реальной карте: множитель для каждого sounds[] из map.json.
// Без этого синтетический тест не доказывает, что на Ферме между игроком
// и источником вообще есть холм.
void audio_selftest_world(const WorldState *w, const float listener[3])
{
    if (!w || !listener) return;
    audio_set_world(w);
    audio_set_listener(listener);
    for (int i = 0; i < mapdata_sound_count(); i++) {
        const MapSound3D *s = mapdata_sound(i);
        if (!s) continue;
        SDL_Log("FR_TEST Z: звук %d (%.0f %.0f %.0f) -> %.2f", i,
                s->pos[0], s->pos[1], s->pos[2], audio_occlusion_at(s->pos));
    }
    // Контрольная точка: источник в 50 м ПОД ногами обязан быть заглушён —
    // иначе высоты не читаются (пустой мир), и все «1.00» выше ничего не значат.
    float under[3] = { listener[0], listener[1] - 50.0f, listener[2] };
    SDL_Log("FR_TEST Z: контрольно, под ногами -> %.2f", audio_occlusion_at(under));
    audio_set_world(NULL);
}

// Заливает в эхо-поток: тишина (задержка) + звук — паттерн реверба
static void echo_fill(SDL_AudioStream *st, Sound *s, int delay_ms)
{
    if (!st || !s) return;
    Uint32 srate = s->spec.freq;
    Uint32 ch = s->spec.channels ? s->spec.channels : 1;
    Uint32 bytes = (SDL_AUDIO_BITSIZE(s->spec.format) / 8) * ch;
    Uint32 silence = (Uint32)(srate * (Uint64)delay_ms / 1000) * bytes;
    if (silence > 0) {
        unsigned char *z = calloc(silence, 1);
        if (z) {
            SDL_PutAudioStreamData(st, z, (int)silence);
            free(z);
        }
    }
    SDL_PutAudioStreamData(st, s->data, (int)s->len);
}

// Внутренняя: play/play_once = play_ex с loop 1/0
static int audio_3d_play_ex(Sound *s, float volume, const float pos[3],
                            float radius, int loop);

int audio_3d_play(Sound *s, float volume, const float pos[3], float radius)
{
    return audio_3d_play_ex(s, volume, pos, radius, 1);
}

int audio_3d_play_once(Sound *s, float volume, const float pos[3], float radius)
{
    return audio_3d_play_ex(s, volume, pos, radius, 0);
}

static int audio_3d_play_ex(Sound *s, float volume, const float pos[3],
                            float radius, int loop)
{
    if (!s_dev || !s || radius <= 0.1f) return -1;
    // Петлевой источник: занятый канал с тем же звуком — двигаем позицию.
    // РАЗОВЫЙ звук так не делаем: каждый вызов обязан перезапуститься
    // (иначе частые шаги/шелест замолкают после первого раза).
    if (loop) {
        for (int i = 0; i < AUDIO_3D_CHANNELS; i++) {
            if (s_3d[i].active && s_3d[i].snd == s && s_3d[i].loop) {
                s_3d[i].pos[0] = pos[0];
                s_3d[i].pos[1] = pos[1];
                s_3d[i].pos[2] = pos[2];
                s_3d[i].vol = volume;
                s_3d[i].radius = radius;
                return i;
            }
        }
    }
    // Ищем свободный канал; если все заняты — перезапускаем самый старый
    // (с наименьшей очередью), чтобы звук не пропадал.
    int slot = -1;
    int oldest = -1;
    Uint32 minq = 0xFFFFFFFF;
    for (int i = 0; i < AUDIO_3D_CHANNELS; i++) {
        if (!s_3d[i].active) { slot = i; break; }
        Uint32 q = (Uint32)SDL_GetAudioStreamQueued(s_3d[i].stream);
        if (q < minq) { minq = q; oldest = i; }
    }
    if (slot < 0) slot = oldest;
    if (slot < 0) return -1;
    Audio3D *a = &s_3d[slot];
    a->active = 1;
    a->loop = loop;
    a->snd = s;
    a->pos[0] = pos[0];
    a->pos[1] = pos[1];
    a->pos[2] = pos[2];
    a->vol = volume;
    a->radius = radius;
    SDL_ClearAudioStream(a->stream);
    SDL_SetAudioStreamFormat(a->stream, &s->spec, &s_dev_spec);
    SDL_SetAudioStreamGain(a->stream, 0.0f);
    SDL_PutAudioStreamData(a->stream, s->data, (int)s->len);
    // эхо: задержки 90 и 180 мс
    for (int e = 0; e < AUDIO_ECHO_N; e++) {
        SDL_ClearAudioStream(a->echo[e]);
        SDL_SetAudioStreamFormat(a->echo[e], &s->spec, &s_dev_spec);
        SDL_SetAudioStreamGain(a->echo[e], 0.0f);
        echo_fill(a->echo[e], s, AUDIO_ECHO_DELAY_MS * (e + 1));
    }
    return slot;
}

void audio_3d_stop(int id)
{
    if (id < 0 || id >= AUDIO_3D_CHANNELS || !s_3d[id].active) return;
    SDL_ClearAudioStream(s_3d[id].stream);
    for (int e = 0; e < AUDIO_ECHO_N; e++)
        SDL_ClearAudioStream(s_3d[id].echo[e]);
    s_3d[id].active = 0;
    s_3d[id].snd = NULL;
}

void audio_3d_stop_all(void)
{
    for (int i = 0; i < AUDIO_3D_CHANNELS; i++) audio_3d_stop(i);
}

void audio_update(void)
{
    if (!s_dev) return;
    // Ползунки громкости действуют сразу: переустанавливаем gain каждый кадр
    float sfx = snd_gain("snd_sfx");
    float mus = snd_gain("snd_music");
    if (s_loop && s_loop_snd) {
        SDL_SetAudioStreamGain(s_loop, s_loop_vol * s_loop_mul * sfx);
        int queued = SDL_GetAudioStreamQueued(s_loop);
        if (queued < (int)s_loop_snd->len / 2) {
            SDL_PutAudioStreamData(s_loop, s_loop_snd->data,
                                   (int)s_loop_snd->len);
            s_loop_sent += s_loop_snd->len;
        }
        (void)s_loop_sent;
    }
    if (s_music && s_music_snd) {
        SDL_SetAudioStreamGain(s_music, s_music_vol * mus);
        int queued = SDL_GetAudioStreamQueued(s_music);
        if (queued < (int)s_music_snd->len / 2)
            SDL_PutAudioStreamData(s_music, s_music_snd->data,
                                   (int)s_music_snd->len);
    }
    // 3D-источники: громкость от дистанции, эхо усиливается вдали
    for (int i = 0; i < AUDIO_3D_CHANNELS; i++) {
        Audio3D *a = &s_3d[i];
        if (!a->active) continue;
        float dx = a->pos[0] - s_listener[0];
        float dy = a->pos[1] - s_listener[1];
        float dz = a->pos[2] - s_listener[2];
        float dist = sqrtf(dx * dx + dy * dy + dz * dz);
        float t = dist / a->radius;
        // Разовый звук отыграл — канал свободен (вне зависимости от t)
        if (!a->loop && SDL_GetAudioStreamQueued(a->stream) <= 0) {
            audio_3d_stop(i);
            continue;
        }
        float gain = 0.0f;
        float occ = 1.0f;
        if (t < 1.0f) {
            // Окклюзия считается только для слышимого канала: это ~160
            // выборок высоты на источник, а не на все 8 каналов.
            occ = audio_occlusion_at(a->pos);
            gain = a->vol * sfx * (1.0f - t) * (1.0f - t) * occ;   // квадратичное затухание
            if (a->loop) {
                int queued = SDL_GetAudioStreamQueued(a->stream);
                if (queued < (int)a->snd->len / 2)
                    SDL_PutAudioStreamData(a->stream, a->snd->data,
                                           (int)a->snd->len);
            }
        }
        SDL_SetAudioStreamGain(a->stream, gain);
        // Реверб: лёгкий хвост есть ВСЕГДА, с дистанцией усиливается
        // (звук издалека = тише + отчётливое эхо).
        Uint32 bps = (Uint32)(SDL_AUDIO_BITSIZE(a->snd->spec.format) / 8) *
                     (a->snd->spec.channels ? a->snd->spec.channels : 1);
        for (int e = 0; e < AUDIO_ECHO_N; e++) {
            float eg = 0.0f;
            if (t < 1.0f)
                eg = a->vol * sfx * (0.10f + t * (e == 0 ? 0.25f : 0.12f)) * occ;
            SDL_SetAudioStreamGain(a->echo[e], eg);
            if (eg > 0.0f && a->loop) {
                Uint32 delay_bytes = (Uint32)(a->snd->spec.freq * bps *
                                              (AUDIO_ECHO_DELAY_MS * (e + 1)) / 1000);
                Uint32 total = delay_bytes + a->snd->len;
                if ((Uint32)SDL_GetAudioStreamQueued(a->echo[e]) < total)
                    echo_fill(a->echo[e], a->snd, AUDIO_ECHO_DELAY_MS * (e + 1));
            }
        }
    }
}

// Отладка: какие звуки сейчас на каналах (имя без audio/ и .wav)
int audio_active_names(char *buf, int size)
{
    int n = 0;
    if (size <= 0) return 0;
    buf[0] = 0;
    for (int i = 0; i < AUDIO_CHANNELS; i++) {
        if (s_chan_snd[i] && SDL_GetAudioStreamQueued(s_chan[i]) > 0) {
            const char *p = strrchr(s_chan_snd[i]->path, '/');
            p = p ? p + 1 : s_chan_snd[i]->path;
            if (n < size - 1)
                n += snprintf(buf + n, (size_t)(size - n), "%s ", p);
        }
    }
    if (s_loop_snd && n < size - 1)
        n += snprintf(buf + n, (size_t)(size - n), "LOOP ");
    if (s_music_snd && n < size - 1)
        n += snprintf(buf + n, (size_t)(size - n), "MUSIC ");
    return n;
}

void audio_cleanup(void)
{
    if (!s_dev) return;
    for (int i = 0; i < AUDIO_CHANNELS; i++) {
        if (s_chan[i]) {
            SDL_UnbindAudioStream(s_chan[i]);
            SDL_DestroyAudioStream(s_chan[i]);
            s_chan[i] = NULL;
        }
    }
    if (s_loop) {
        SDL_UnbindAudioStream(s_loop);
        SDL_DestroyAudioStream(s_loop);
        s_loop = NULL;
    }
    if (s_music) {
        SDL_UnbindAudioStream(s_music);
        SDL_DestroyAudioStream(s_music);
        s_music = NULL;
    }
    for (int i = 0; i < AUDIO_3D_CHANNELS; i++) {
        if (s_3d[i].stream) {
            SDL_UnbindAudioStream(s_3d[i].stream);
            SDL_DestroyAudioStream(s_3d[i].stream);
            s_3d[i].stream = NULL;
        }
        for (int e = 0; e < AUDIO_ECHO_N; e++) {
            if (s_3d[i].echo[e]) {
                SDL_UnbindAudioStream(s_3d[i].echo[e]);
                SDL_DestroyAudioStream(s_3d[i].echo[e]);
                s_3d[i].echo[e] = NULL;
            }
        }
        s_3d[i].active = 0;
        s_3d[i].snd = NULL;
    }
    for (int i = 0; i < s_cache_n; i++) {
        if (s_cache[i]->data) SDL_free(s_cache[i]->data);
        free(s_cache[i]);
    }
    s_cache_n = 0;
    if (s_dev) {
        SDL_CloseAudioDevice(s_dev);
        s_dev = 0;
    }
    SDL_Log("audio: closed");
}
