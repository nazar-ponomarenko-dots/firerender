// ============================================================================
//  cvars.h — engine variables (CVars) and config.cfg
// ----------------------------------------------------------------------------
//  Registry of variables driven by the console and keys: fov, cl_sens,
//  sv_gravity, r_shadows... Modules READ cvars every frame, the console
//  changes them — one-way coupling, no direct calls.
//
//  A variable: name, value, range [min, max] (set clamps into it), flags.
//  config.cfg — a "name value" file in the working folder: loaded at startup,
//  saved on exit (as in GoldSrc). Only cvars with the CVAR_ARCHIVE flag are saved.
// ============================================================================
#ifndef CVARS_H
#define CVARS_H

typedef struct {
    char name[32];       // variable name
    float value;         // current value
    float def;           // default value
    float minv, maxv;    // range (minv > maxv — no limits)
    int flags;           // CVAR_ARCHIVE etc.
    char str[64];        // string value (for string variables)
    char def_str[64];    // default string value
    char help[64];       // description (for cvarlist)
} Cvar;

// Initial registry capacity. The registry GROWS dynamically (realloc),
// there is no cap on the number of variables — CVARS_MAX is only the starting size.
#define CVARS_MAX 64

// Variable flags
#define CVAR_ARCHIVE   1  // saved to config.cfg (default for register)
#define CVAR_NOSAVE    2  // NOT saved (temporary/internal)
#define CVAR_CHEAT     4  // cheat: changeable only when sv_cheats 1
#define CVAR_USERINFO  8  // player info: sent/stored, but irrelevant to
                          // the game world (like FCVAR_USERINFO in Source)
#define CVAR_CFGSET   32  // value set explicitly (config/user/map):
                          // cvar_set_default does NOT overwrite it
#define CVAR_STRING   16  // string variable (value in c->str)

// Clears the registry (called by console_init).
void cvar_init(void);

// Registers a variable: no range, with the CVAR_ARCHIVE flag.
void cvar_register(const char *name, float def, const char *help);

// Registers with range [min, max] and flags (see CVAR_*).
// min >= max — no limits. Re-registering an existing name — no-op.
void cvar_register_ex(const char *name, float def, float min, float max,
                      int flags, const char *help);

// Registers a STRING variable (the CVAR_STRING flag is set automatically).
// The default is a string; cvar_get(name) returns atof(def_str).
// Re-registering an existing name — no-op.
void cvar_register_string(const char *name, const char *def, const char *help);

// Read/write. cvar_get of an unknown name = 0; cvar_set of an unknown
// name registers it (the console may create its own variables).
// cvar_set clamps the value into the range. For string variables cvar_set
// also writes the string value.
float cvar_get(const char *name);
void cvar_set_default(const char *name, float v);   // set only if the user hasn't set it
void cvar_set(const char *name, float v);

// String variables (CVAR_STRING). cvar_get_string returns
// "" for a non-string/unknown variable.
const char *cvar_get_string(const char *name);
void cvar_set_string(const char *name, const char *s);

// Sets a value from a STRING: if the variable is CVAR_STRING — writes the
// string, otherwise — atof() (used when loading config.cfg and by set).
void cvar_set_from_string(const char *name, const char *s);

// Access by index (for cvarlist and Tab completion).
int cvar_index(const char *name);   // -1 if not found
int cvar_count(void);
const Cvar *cvar_at(int i);

// config.cfg: "name value" lines. load returns 1 if the file opened.
// save writes only cvars with CVAR_ARCHIVE.
int cvar_load_config(const char *path);
void cvar_save_config(const char *path);

// Resets ALL graphics cvars (name starts with "r_") to their default
// values (def). Returns the number of variables reset. Cheat variables
// (CVAR_CHEAT) are left untouched.
int cvar_reset_graphics(void);

#endif
