// ============================================================================
//  cvars.c — реализация реестра CVar'ов (см. cvars.h)
// ============================================================================
#include "cvars.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Реестр растёт динамически (realloc), лимита нет. ВАЖНО: Cvar содержит
// фиксированные char[] (name/str/help), поэтому realloc лишь переносит
// структуры; снаружи не держите `const Cvar*` между двумя cvar_register.
static Cvar *s_cvars = NULL;
static int  s_cap = 0;         // вместимость массива
static int  s_count = 0;       // занято
// Идёт cvar_load_config: незнакомые имена из config.cfg создаются сразу
// АРХИВНЫМИ (иначе настройки ещё не зарегистрированных систем — плотности
// травы и пр. — терялись при выходе и возвращались к дефолтам карты).
static int  s_config_loading = 0;

void cvar_init(void)
{
    free(s_cvars);
    s_cvars = NULL;
    s_cap = 0;
    s_count = 0;
}

// Выделяет слот под новую переменную (растит массив) или NULL.
static Cvar *cvar_slot_new(void)
{
    if (s_count >= s_cap) {
        int ncap = s_cap ? s_cap * 2 : CVARS_MAX;
        Cvar *n = realloc(s_cvars, (size_t)ncap * sizeof *n);
        if (!n) return NULL;
        s_cvars = n;
        s_cap = ncap;
    }
    Cvar *c = &s_cvars[s_count++];
    // Слот из realloc — с мусором. Обнуляем целиком: иначе str/def_str
    // читаются как строка (cvar_save_config) у переменной, которая стала
    // CVAR_STRING уже после создания слота.
    memset(c, 0, sizeof *c);
    return c;
}

int cvar_index(const char *name)
{
    if (!name) return -1;
    for (int i = 0; i < s_count; i++)
        if (strcmp(s_cvars[i].name, name) == 0) return i;
    return -1;
}

void cvar_register_ex(const char *name, float def, float min, float max,
                      int flags, const char *help)
{
    if (!name || !name[0]) return;
    int existing = cvar_index(name);
    if (existing >= 0) {
        // УЖЕ есть (авто-создана из config.cfg/карты/lua): удочеряем —
        // обновляем дефолт/диапазон/флаги, но СОХРАНЯЕМ текущее значение
        // (иначе настройки пользователя затирались дефолтами при загрузке
        // мира, а незнакомые системе cvar'ы не выживали в config.cfg).
        Cvar *c = &s_cvars[existing];
        c->def = def;
        if (min <= max) { c->minv = min; c->maxv = max; }
        c->flags |= flags;
        if (help && !c->help[0]) snprintf(c->help, sizeof c->help, "%s", help);
        return;
    }
    Cvar *c = cvar_slot_new();
    if (!c) return;
    snprintf(c->name, sizeof c->name, "%s", name);
    c->def = def;
    c->value = def;
    c->minv = min;
    c->maxv = max;
    c->flags = flags;
    c->help[0] = 0;
    if (help) snprintf(c->help, sizeof c->help, "%s", help);
    if (flags & CVAR_STRING) {
        c->str[0] = 0;
        c->def_str[0] = 0;
    }
}

void cvar_register_string(const char *name, const char *def, const char *help)
{
    if (!name || !name[0]) return;
    cvar_register_ex(name, def ? (float)atof(def) : 0.0f, 1.0f, -1.0f,
                     CVAR_ARCHIVE | CVAR_STRING, help);
    int i = cvar_index(name);
    if (i < 0) return;
    Cvar *c = &s_cvars[i];
    if (def) {
        snprintf(c->def_str, sizeof c->def_str, "%s", def);
        snprintf(c->str, sizeof c->str, "%s", def);
    }
}

void cvar_register(const char *name, float def, const char *help)
{
    // Без диапазона: minv = 1, maxv = -1 (minv > maxv — нет ограничений)
    cvar_register_ex(name, def, 1.0f, -1.0f, CVAR_ARCHIVE, help);
}

float cvar_get(const char *name)
{
    int i = cvar_index(name);
    return i >= 0 ? s_cvars[i].value : 0.0f;
}

// Задать значение-умолчание (дефолт карты/системы): НЕ трогает cvar'ы,
// которые пользователь/конфиг уже задавал явно (флаг CVAR_CFGSET).
void cvar_set_default(const char *name, float v)
{
    int i = cvar_index(name);
    if (i < 0) return;
    Cvar *c = &s_cvars[i];
    if (c->flags & CVAR_CFGSET) return;
    c->value = v;
}

void cvar_set(const char *name, float v)
{
    int i = cvar_index(name);
    if (i < 0) {
        // Незнакомое имя — заводим сами (карты/Lua могут вводить свои
        // переменные), но БЕЗ CVAR_ARCHIVE: чужеродные cvar'ы не попадают
        // в config.cfg (иначе опечатка жила бы в конфиге вечно).
        if (!name || !name[0]) return;
        int fl = s_config_loading ? CVAR_ARCHIVE : 0;
        SDL_Log("cvar_set: \"%s\" не зарегистрирован — создаю%s",
                name, fl & CVAR_ARCHIVE ? " (из config.cfg — архивный)" : "");
        cvar_register_ex(name, v, 1.0f, -1.0f, fl, "map/lua-переменная");
        i = cvar_index(name);
    }
    if (i < 0) return;
    Cvar *c = &s_cvars[i];
    // Чит-переменные меняются только при sv_cheats 1
    if ((c->flags & CVAR_CHEAT) && cvar_get("sv_cheats") < 0.5f) {
        SDL_Log("cvar \"%s\" — чит: требуется sv_cheats 1", c->name);
        return;
    }
    if (c->minv <= c->maxv) {            // диапазон задан — зажимаем
        if (v < c->minv) v = c->minv;
        if (v > c->maxv) v = c->maxv;
    }
    c->value = v;
    c->flags |= CVAR_CFGSET;              // явная установка (конфиг/юзер/карта)
    if (c->flags & CVAR_STRING)           // держим строку в синхроне с числом
        snprintf(c->str, sizeof c->str, "%g", v);
}

const char *cvar_get_string(const char *name)
{
    int i = cvar_index(name);
    return (i >= 0 && (s_cvars[i].flags & CVAR_STRING))
               ? s_cvars[i].str : "";
}

void cvar_set_string(const char *name, const char *s)
{
    int i = cvar_index(name);
    if (i < 0) {
        if (!name || !name[0]) return;
        SDL_Log("cvar_set_string: \"%s\" не зарегистрирован — создаю", name);
        cvar_register_string(name, s, "");
        i = cvar_index(name);
    }
    if (i < 0) return;
    Cvar *c = &s_cvars[i];
    if (c->flags & CVAR_STRING) {
        if ((c->flags & CVAR_CHEAT) && cvar_get("sv_cheats") < 0.5f) {
            SDL_Log("cvar \"%s\" — чит: требуется sv_cheats 1", c->name);
            return;
        }
        snprintf(c->str, sizeof c->str, "%s", s ? s : "");
        c->value = (float)atof(c->str);
    } else {
        cvar_set(name, (float)atof(s ? s : "0"));
    }
}

void cvar_set_from_string(const char *name, const char *s)
{
    int i = cvar_index(name);
    if (i >= 0 && (s_cvars[i].flags & CVAR_STRING))
        cvar_set_string(name, s);
    else
        cvar_set(name, (float)atof(s));
}

int cvar_count(void)
{
    return s_count;
}

const Cvar *cvar_at(int i)
{
    return (i >= 0 && i < s_count) ? &s_cvars[i] : NULL;
}

int cvar_load_config(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    s_config_loading = 1;
    char line[128];
    while (fgets(line, sizeof line, f)) {
        char name[32];
        char value[64];
        // Строка может быть строковой: name "value" или name "vec with spaces"
        if (sscanf(line, "%31s \"%63[^\"]\"", name, value) == 2)
            cvar_set_from_string(name, value);
        else if (sscanf(line, "%31s %63s", name, value) == 2)
            cvar_set_from_string(name, value);
    }
    fclose(f);
    s_config_loading = 0;
    return 1;
}

void cvar_save_config(const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) return;
    for (int i = 0; i < s_count; i++) {
        const Cvar *c = &s_cvars[i];
        if (!(c->flags & CVAR_ARCHIVE)) continue;   // только архивные
        // Не пишем неизменённые переменные (= значению по умолчанию):
        // config.cfg хранит только отличия от дефолтов (как события в HL).
        if (c->flags & CVAR_STRING) {
            if (strcmp(c->str, c->def_str) == 0) continue;
            fprintf(f, "%s \"%s\"\n", c->name, c->str);
        } else {
            if (c->value == c->def) continue;
            fprintf(f, "%s %.4g\n", c->name, c->value);
        }
    }
    fclose(f);
}

// Сброс графических cvar'ов: все, чьё имя начинается на "r_" (рендер),
// возвращаются к значению по умолчанию. Чит-переменные пропускаются
// (сбрасывать их без sv_cheats нельзя — cvar_set их отклоняет).
int cvar_reset_graphics(void)
{
    int n = 0;
    for (int i = 0; i < s_count; i++) {
        Cvar *c = &s_cvars[i];
        if (strncmp(c->name, "r_", 2) != 0) continue;
        if (c->flags & CVAR_CHEAT) continue;
        cvar_set(c->name, c->def);
        n++;
    }
    return n;
}
