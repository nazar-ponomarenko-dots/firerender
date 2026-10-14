// ============================================================================
//  cvars.c — CVar registry implementation (see cvars.h)
// ============================================================================
#include "cvars.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The registry grows dynamically (realloc), no limit. IMPORTANT: Cvar contains
// fixed char[] arrays (name/str/help), so realloc merely relocates the
// structs; do not hold a `const Cvar*` across two cvar_register calls.
static Cvar *s_cvars = NULL;
static int  s_cap = 0;         // array capacity
static int  s_count = 0;       // in use
// cvar_load_config is running: unrecognized names from config.cfg are created
// as ARCHIVE right away (otherwise settings of not-yet-registered systems —
// grass density etc. — were lost on exit and fell back to map defaults).
static int  s_config_loading = 0;

void cvar_init(void)
{
    free(s_cvars);
    s_cvars = NULL;
    s_cap = 0;
    s_count = 0;
}

// Allocates a slot for a new variable (grows the array) or NULL.
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
    // A realloc'd slot comes with garbage. Zero the whole struct: otherwise
    // str/def_str are read as strings (cvar_save_config) for a variable that
    // became CVAR_STRING only after the slot was created.
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
        // ALREADY exists (auto-created from config.cfg/map/lua): adopt it —
        // update default/range/flags but KEEP the current value (otherwise
        // user settings were overwritten with defaults on world load, and
        // cvars unknown to the system never survived into config.cfg).
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
    // No range: minv = 1, maxv = -1 (minv > maxv — no limits)
    cvar_register_ex(name, def, 1.0f, -1.0f, CVAR_ARCHIVE, help);
}

float cvar_get(const char *name)
{
    int i = cvar_index(name);
    return i >= 0 ? s_cvars[i].value : 0.0f;
}

// Set the default value (map/system default): does NOT touch cvars that the
// user/config already set explicitly (CVAR_CFGSET flag).
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
        // Unknown name — create it ourselves (maps/Lua may introduce their
        // own variables), but WITHOUT CVAR_ARCHIVE: foreign cvars do not land
        // in config.cfg (otherwise a typo would live in the config forever).
        if (!name || !name[0]) return;
        int fl = s_config_loading ? CVAR_ARCHIVE : 0;
        SDL_Log("cvar_set: \"%s\" not registered — creating%s",
                name, fl & CVAR_ARCHIVE ? " (from config.cfg — archived)" : "");
        cvar_register_ex(name, v, 1.0f, -1.0f, fl, "map/lua-переменная");
        i = cvar_index(name);
    }
    if (i < 0) return;
    Cvar *c = &s_cvars[i];
    // Cheat variables only change when sv_cheats 1
    if ((c->flags & CVAR_CHEAT) && cvar_get("sv_cheats") < 0.5f) {
        SDL_Log("cvar \"%s\" is a cheat: sv_cheats 1 required", c->name);
        return;
    }
    if (c->minv <= c->maxv) {            // range set — clamp
        if (v < c->minv) v = c->minv;
        if (v > c->maxv) v = c->maxv;
    }
    c->value = v;
    c->flags |= CVAR_CFGSET;              // explicitly set (config/user/map)
    if (c->flags & CVAR_STRING)           // keep the string in sync with the number
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
        SDL_Log("cvar_set_string: \"%s\" not registered — creating", name);
        cvar_register_string(name, s, "");
        i = cvar_index(name);
    }
    if (i < 0) return;
    Cvar *c = &s_cvars[i];
    if (c->flags & CVAR_STRING) {
        if ((c->flags & CVAR_CHEAT) && cvar_get("sv_cheats") < 0.5f) {
            SDL_Log("cvar \"%s\" is a cheat: sv_cheats 1 required", c->name);
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
        // The line may be a string one: name "value" or name "vec with spaces"
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
        if (!(c->flags & CVAR_ARCHIVE)) continue;   // archive ones only
        // Don't write unchanged variables (= default value): config.cfg keeps
        // only the diffs from defaults (like events in HL).
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

// Reset graphics cvars: everything whose name starts with "r_" (renderer)
// returns to its default value. Cheat variables are skipped
// (resetting them without sv_cheats is not allowed — cvar_set rejects it).
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
