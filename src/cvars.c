// ============================================================================
//  cvars.c — реализация реестра CVar'ов (см. cvars.h)
// ============================================================================
#include "cvars.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

static Cvar s_cvars[CVARS_MAX];
static int s_count = 0;

void cvar_init(void)
{
    s_count = 0;
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
    if (cvar_index(name) >= 0) return;
    if (s_count >= CVARS_MAX) return;
    Cvar *c = &s_cvars[s_count++];
    snprintf(c->name, sizeof c->name, "%s", name);
    c->def = def;
    c->value = def;
    c->minv = min;
    c->maxv = max;
    c->flags = flags;
    c->help[0] = 0;
    if (help) snprintf(c->help, sizeof c->help, "%s", help);
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

void cvar_set(const char *name, float v)
{
    int i = cvar_index(name);
    if (i < 0) {
        cvar_register(name, v, "");      // незнакомое имя — заводим сами
        i = s_count - 1;
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
    char line[128];
    while (fgets(line, sizeof line, f)) {
        char name[32];
        float v;
        if (sscanf(line, "%31s %f", name, &v) == 2)
            cvar_set(name, v);
    }
    fclose(f);
    return 1;
}

void cvar_save_config(const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) return;
    for (int i = 0; i < s_count; i++) {
        const Cvar *c = &s_cvars[i];
        if (!(c->flags & CVAR_ARCHIVE)) continue;   // только архивные
        fprintf(f, "%s %.4g\n", c->name, c->value);
    }
    fclose(f);
}
