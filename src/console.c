// ============================================================================
//  console.c — developer console implementation (see console.h)
// ============================================================================
#include "console.h"

#include "actions.h"
#include "binds.h"
#include "cvars.h"
#include "events.h"
#include "hud.h"
#include "resources.h"
#include "utf8.h"
#include "buildnum.h"
#include "fx.h"
#include "mesh.h"
#include "glb.h"
#include "glb_anim.h"
#include "material.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

#define CON_LINES      400     // output lines kept in the ring (scrollback)
#define CON_LINE_LEN   160     // output line length
#define CON_INPUT_LEN  128     // input line length
#define CON_HISTORY    32      // command history depth
#define CON_VIS_MAX    60      // cap on visible output lines (from panel height)

int g_quit_request = 0;

// --- Output line ring ---------------------------------------------------------
static char s_lines[CON_LINES][CON_LINE_LEN];
static int  s_first = 0;      // index of the oldest line in the ring
static int  s_count = 0;      // total lines held in the ring

static void ring_push(const char *s)
{
    snprintf(s_lines[(s_first + s_count) % CON_LINES], CON_LINE_LEN, "%s", s);
    if (s_count < CON_LINES) s_count++;
    else s_first = (s_first + 1) % CON_LINES;
}

static const char *ring_at(int i)   // i: 0 = oldest line
{
    return s_lines[(s_first + i) % CON_LINES];
}

// --- Input and history --------------------------------------------------------
static char s_input[CON_INPUT_LEN];
static int  s_inlen = 0;      // input line length (bytes)
static int  s_cur = 0;        // cursor position (bytes, may be < inlen)
static char s_history[CON_HISTORY][CON_INPUT_LEN];
static int  s_hist_n = 0, s_hist_pos = -1;

// --- State --------------------------------------------------------------------
static int s_active = 0;
static int s_scroll = 0;      // lines scrolled up (0 = bottom)
static int s_vis_last = 14;   // output lines that fit the panel (for PgUp/PgDn)

// --- Commands -----------------------------------------------------------------
static struct {
    const char *name;
    const char *help;
    ConsoleCmdFn fn;
} s_cmds[CON_MAX_COMMANDS];
static int s_cmds_n = 0;

// --- Aliases (macros, like in HL): alias <name> <command; command> -------------
#define ALIAS_MAX 32
typedef struct { char name[32]; char body[192]; } Alias;
static Alias s_aliases[ALIAS_MAX];
static int s_alias_n = 0;

static const char *alias_find(const char *name)
{
    if (!name) return NULL;
    for (int i = 0; i < s_alias_n; i++)
        if (strcmp(s_aliases[i].name, name) == 0) return s_aliases[i].body;
    return NULL;
}

// --- Deferred execution (wait): command queue delayed by frames ---------------
// Like GoldSrc: `wait` in a line delays ALL commands after it by 1 frame
// (wait N — by N frames). Each frame console_drain (main loop) runs those
// whose counter reached zero. console_later — same mechanism for code/Lua.
#define CON_PEND_MAX 64
static struct { char cmd[192]; int frames; } s_pend[CON_PEND_MAX];
static int s_pend_n = 0;

static void console_exec_one(const char *line);   // forward (see below)

void console_later(int frames, const char *cmd)
{
    if (!cmd || !cmd[0]) return;
    if (s_pend_n >= CON_PEND_MAX) {
        console_print("console: очередь wait полна (%d)", CON_PEND_MAX);
        return;
    }
    snprintf(s_pend[s_pend_n].cmd, sizeof s_pend[0].cmd, "%s", cmd);
    s_pend[s_pend_n].frames = frames > 0 ? frames : 1;
    s_pend_n++;
}

// Called every frame from the main loop: runs the deferred commands.
// A command with frames=N runs on frame N (wait -> one frame later).
// Order MATTERS (FIFO): commands maturing in the same frame run in the same
// order they were added. We compact the list by shifting, preserving order
// (swap-with-last didn't work here — it shuffled the queue and lost nested
// wait). Commands added by console_later DURING drain (nested wait) are
// appended past the end snapshot: this frame they do NOT run, and the
// compaction moves them to the front of the queue — for the next frame.
// (Previously the tail was just dropped by s_pend_n = kept — commands lost.)
void console_drain(void)
{
    int end = s_pend_n;
    if (end == 0) return;
    int kept = 0;
    for (int i = 0; i < end; i++) {
        if (s_pend[i].frames > 0) s_pend[i].frames--;
        if (s_pend[i].frames <= 0) {
            // Copy the line BEFORE exec: a nested wait's console_later may
            // append to the tail, and the compaction below may rewrite the slot.
            char cmd[sizeof s_pend[0].cmd];
            if (snprintf(cmd, sizeof cmd, "%s", s_pend[i].cmd) < 0) cmd[0] = 0;
            console_exec_one(cmd);          // may add new ones (wait)
        } else {
            if (kept != i) s_pend[kept] = s_pend[i];
            kept++;
        }
    }
    // Tail [end, s_pend_n) — commands added during drain: shift them into
    // the freed slots (to run on the next frame).
    int tail = s_pend_n - end;
    if (tail > 0 && kept != end)
        memmove(&s_pend[kept], &s_pend[end], (size_t)tail * sizeof s_pend[0]);
    s_pend_n = kept + tail;
}

int console_active(void)
{
    return s_active;
}

void console_print(const char *fmt, ...)
{
    char tmp[CON_LINE_LEN];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    ring_push(tmp);
}

int console_add_command(const char *name, const char *help, ConsoleCmdFn fn)
{
    if (!name || !name[0] || !fn || s_cmds_n >= CON_MAX_COMMANDS) return 0;
    for (int i = 0; i < s_cmds_n; i++)
        if (strcmp(s_cmds[i].name, name) == 0) return 0;
    s_cmds[s_cmds_n].name = name;
    s_cmds[s_cmds_n].help = help ? help : "";
    s_cmds[s_cmds_n].fn = fn;
    s_cmds_n++;
    return 1;
}

// Executes ONE command (name + arguments). Aliases are expanded (with
// recursion protection).
static int s_exec_depth = 0;

static void console_exec_one(const char *line)
{
    while (*line == ' ' || *line == '\t') line++;
    if (!*line) return;

    char cmd[64];
    const char *args = line;
    int n = 0;
    while (*args && *args != ' ' && *args != '\t' && n < 63)
        cmd[n++] = *args++;
    cmd[n] = 0;
    while (*args == ' ' || *args == '\t') args++;

    for (int i = 0; i < s_cmds_n; i++) {
        if (strcmp(s_cmds[i].name, cmd) == 0) {
            s_cmds[i].fn(args);
            return;
        }
    }
    if (s_exec_depth < 8) {
        const char *body = alias_find(cmd);
        if (body) {
            s_exec_depth++;
            console_exec(body);
            s_exec_depth--;
            return;
        }
    }
    console_print("Unknown command \"%s\" (help — список команд)", cmd);
}

// Command substring with leading and trailing spaces stripped
static const char *stmt_trim(const char *p, size_t *len)
{
    while (*p == ' ' || *p == '\t') p++;
    size_t l = strlen(p);
    while (l > 0 && (p[l - 1] == ' ' || p[l - 1] == '\t')) l--;
    *len = l;
    return p;
}

// "wait" or "wait N" -> N (or 1), -1 if not wait
static int stmt_wait_delay(const char *s, size_t len)
{
    if (len >= 4 && strncmp(s, "wait", 4) == 0) {
        if (len == 4) return 1;
        if (s[4] == ' ' || s[4] == '\t') {
            const char *n = s + 4;
            while (*n == ' ' || *n == '\t') n++;
            int d = atoi(n);
            return d > 0 ? d : 1;
        }
    }
    return -1;
}

void console_exec(const char *line)
{
    // Commands may be separated by ";" — but NOT inside quotes
    // (alias/bind: "cmd1; cmd2" is one body). After `wait` all following
    // commands in the line are delayed by 1 frame (wait N — by N).
    char buf[256];
    snprintf(buf, sizeof buf, "%s", line ? line : "");
    int delay = 0;                 // accumulated delay for later commands
    char *p = buf;
    while (p && *p) {
        char *semi = NULL;
        int inq = 0;
        for (char *q = p; *q; q++) {
            if (*q == '"') inq = !inq;
            else if (*q == ';' && !inq) { semi = q; break; }
        }
        if (semi) *semi = 0;
        size_t len;
        const char *stmt = stmt_trim(p, &len);
        if (len > 0) {
            int wd = stmt_wait_delay(stmt, len);
            if (wd >= 0) {
                delay += wd;              // defer everything after wait
            } else if (delay > 0) {
                console_later(delay, stmt);   // deferred command
            } else {
                console_exec_one(stmt);
            }
        }
        p = semi ? semi + 1 : NULL;
    }
}

void console_toggle(void)
{
    s_active = !s_active;
    s_scroll = 0;                       // on open — bottom, at the newest
    s_hist_pos = -1;
    events_emit(s_active ? EV_CONSOLE_OPENED : EV_CONSOLE_CLOSED, NULL, NULL);
}

// --- Line input ---------------------------------------------------------------

// Insert text at the cursor position
static void input_insert(const char *text, int len)
{
    if (len <= 0 || s_inlen + len >= CON_INPUT_LEN) return;
    memmove(s_input + s_cur + len, s_input + s_cur, (size_t)s_inlen - s_cur);
    memcpy(s_input + s_cur, text, (size_t)len);
    s_inlen += len;
    s_cur += len;
    s_input[s_inlen] = 0;
}

// Delete the character BEFORE the cursor (Backspace). Returns 1 if deleted.
static int input_backspace(void)
{
    if (s_cur <= 0) return 0;
    int prev = utf8_prev(s_input, s_cur);
    memmove(s_input + prev, s_input + s_cur, (size_t)s_inlen - s_cur + 1);
    s_inlen -= s_cur - prev;
    s_cur = prev;
    return 1;
}

// Delete the character AT the cursor (Delete).
static int input_delete(void)
{
    if (s_cur >= s_inlen) return 0;
    int next = s_cur + utf8_len((unsigned char)s_input[s_cur]);
    memmove(s_input + s_cur, s_input + next, (size_t)s_inlen - next + 1);
    s_inlen -= next - s_cur;
    return 1;
}

// Start of the word before the cursor (for Ctrl+W)
static int input_word_start(void)
{
    int i = s_cur;
    while (i > 0 && s_input[i - 1] != ' ' && s_input[i - 1] != '\t') i--;
    return i;
}

// --- Tab: completion of command and cvar names --------------------------------
// A COPY of each name, not a pointer to it: cvar names live inside the
// realloc-grown s_cvars array, so a pointer stays valid only until the next
// cvar_register — and a map load or a Lua command registers some between Tabs.
typedef struct { char name[64]; } TabMatch;
static TabMatch *s_tab_matches = NULL;   // growing array of matches
static int s_tab_cap = 0, s_tab_n = 0, s_tab_idx = 0;
static int s_tab_base_len = 0, s_tab_base_hash = 0;   // base of the last cycle

static int tab_append(const char *m)
{
    if (s_tab_n + 1 > s_tab_cap) {
        int ncap = s_tab_cap ? s_tab_cap * 2 : 128;
        TabMatch *n = realloc(s_tab_matches, (size_t)ncap * sizeof *n);
        if (!n) return 0;
        s_tab_matches = n;
        s_tab_cap = ncap;
    }
    snprintf(s_tab_matches[s_tab_n].name, sizeof s_tab_matches[s_tab_n].name,
             "%s", m);
    s_tab_n++;
    return 1;
}

static unsigned tab_hash(const char *s)
{
    unsigned h = 5381;
    while (*s) h = h * 33 + (unsigned char)*s++;
    return h;
}

static void input_complete(void)
{
    // The word BEFORE the cursor (first word of the line — command/cvar name)
    int wstart = s_cur;
    while (wstart > 0 && s_input[wstart - 1] != ' ' && s_input[wstart - 1] != '\t')
        wstart--;
    int wlen = s_cur - wstart;

    // Same text as on the previous Tab? — cycle through the matches
    unsigned h = tab_hash(s_input + wstart);
    if (s_tab_base_len != wlen || (int)s_tab_base_hash != (int)h) {
        s_tab_base_len = wlen;
        s_tab_base_hash = h;
        s_tab_idx = 0;
        s_tab_n = 0;
        for (int i = 0; i < s_cmds_n; i++)
            if (strncmp(s_cmds[i].name, s_input + wstart, (size_t)wlen) == 0)
                tab_append(s_cmds[i].name);
        for (int i = 0; i < cvar_count(); i++) {
            const Cvar *c = cvar_at(i);
            if (strncmp(c->name, s_input + wstart, (size_t)wlen) == 0)
                tab_append(c->name);
        }
        if (s_tab_n == 0) return;        // no matches — stay silent
    }
    if (s_tab_n <= 0) return;
    if (s_tab_idx >= s_tab_n) s_tab_idx = 0;

    // Replace the word from wstart to the cursor with the match. First check
    // that the result fits in the input buffer — otherwise a long match
    // (cvar names up to 31 bytes) would write past s_input[128].
    const char *m = s_tab_matches[s_tab_idx].name;
    int mlen = (int)strlen(m);
    if (s_inlen + (mlen - wlen) >= CON_INPUT_LEN) {
        s_tab_idx++;
        return;                             // doesn't fit — silently skip
    }
    memmove(s_input + wstart + mlen, s_input + s_cur,
            (size_t)s_inlen - s_cur + 1);
    memcpy(s_input + wstart, m, (size_t)mlen);
    s_inlen += mlen - wlen;
    s_cur = wstart + mlen;
    s_tab_idx++;                          // next Tab — next match
}

// --- Event handling -----------------------------------------------------------
static int console_event_key(const SDL_Event *ev)
{
    SDL_Keymod mod = (SDL_Keymod)ev->key.mod;
    bool ctrl = (mod & SDL_KMOD_CTRL) != 0;

    if (ev->key.repeat && ev->key.key != SDLK_BACKSPACE && ev->key.key != SDLK_DELETE)
        return 0;   // key repeat doesn't reach the console (except deletions)

    switch (ev->key.key) {
    case SDLK_ESCAPE:
        console_toggle();
        return 1;
    case SDLK_RETURN:
        if (s_inlen > 0) {
            console_print("] %s", s_input);
            if (s_hist_n == 0 ||
                strcmp(s_history[s_hist_n - 1], s_input) != 0) {
                if (s_hist_n == CON_HISTORY) {
                    memmove(&s_history[0], &s_history[1],
                            (CON_HISTORY - 1) * sizeof s_history[0]);
                    s_hist_n--;
                }
                snprintf(s_history[s_hist_n], CON_INPUT_LEN, "%s", s_input);
                s_hist_n++;
            }
            console_exec(s_input);
            s_input[0] = 0;
            s_inlen = 0;
            s_cur = 0;
            s_hist_pos = s_hist_n;
            s_scroll = 0;                  // new output — scroll to bottom
        }
        return 1;
    case SDLK_UP:
        if (s_hist_n > 0) {
            if (s_hist_pos < 0) s_hist_pos = s_hist_n;
            if (s_hist_pos > 0) {
                s_hist_pos--;
                snprintf(s_input, CON_INPUT_LEN, "%s", s_history[s_hist_pos]);
                s_inlen = (int)strlen(s_input);
                s_cur = s_inlen;
            }
        }
        return 1;
    case SDLK_DOWN:
        if (s_hist_n > 0 && s_hist_pos >= 0) {
            s_hist_pos++;
            if (s_hist_pos >= s_hist_n) {
                s_hist_pos = s_hist_n;
                s_input[0] = 0;
                s_inlen = 0;
                s_cur = 0;
            } else {
                snprintf(s_input, CON_INPUT_LEN, "%s", s_history[s_hist_pos]);
                s_inlen = (int)strlen(s_input);
                s_cur = s_inlen;
            }
        }
        return 1;
    case SDLK_TAB:
        input_complete();
        return 1;
    case SDLK_BACKSPACE:
        if (ctrl) {                        // Ctrl+W — erase the word
            int ws = input_word_start();
            if (ws < s_cur) {
                memmove(s_input + ws, s_input + s_cur,
                        (size_t)s_inlen - s_cur + 1);
                s_inlen -= s_cur - ws;
                s_cur = ws;
            }
        } else {
            input_backspace();
        }
        return 1;
    case SDLK_DELETE:
        input_delete();
        return 1;
    case SDLK_LEFT:
        if (ctrl) {                        // Ctrl+← — jump to word start
            while (s_cur > 0 && s_input[s_cur - 1] == ' ') s_cur--;
            while (s_cur > 0 && s_input[s_cur - 1] != ' ') s_cur--;
        } else if (s_cur > 0) {
            s_cur = utf8_prev(s_input, s_cur);
        }
        return 1;
    case SDLK_RIGHT:
        if (s_cur < s_inlen) {
            if (ctrl) {
                while (s_cur < s_inlen && s_input[s_cur] == ' ') s_cur++;
                while (s_cur < s_inlen && s_input[s_cur] != ' ') s_cur++;
            } else {
                s_cur += utf8_len((unsigned char)s_input[s_cur]);
            }
        }
        return 1;
    case SDLK_HOME:
        s_cur = 0;
        return 1;
    case SDLK_END:
        s_cur = s_inlen;
        return 1;
    case SDLK_PAGEUP:
        s_scroll += s_vis_last > 1 ? s_vis_last - 1 : 1;
        return 1;
    case SDLK_PAGEDOWN:
        s_scroll -= s_vis_last > 1 ? s_vis_last - 1 : 1;
        if (s_scroll < 0) s_scroll = 0;
        return 1;
    default:
        if (ctrl && ev->key.key == SDLK_A) { s_cur = 0; return 1; }   // Ctrl+A
        if (ctrl && ev->key.key == SDLK_E) { s_cur = s_inlen; return 1; } // Ctrl+E
        if (ctrl && ev->key.key == SDLK_U) {   // Ctrl+U — erase up to the cursor
            memmove(s_input, s_input + s_cur, (size_t)s_inlen - s_cur + 1);
            s_inlen -= s_cur;
            s_cur = 0;
            return 1;
        }
        if (ctrl && ev->key.key == SDLK_K) {   // Ctrl+K — erase after cursor
            s_inlen = s_cur;
            s_input[s_inlen] = 0;
            return 1;
        }
        if (ctrl && ev->key.key == SDLK_C) {   // Ctrl+C — copy the line
            if (s_inlen > 0) SDL_SetClipboardText(s_input);
            return 1;
        }
        if (ctrl && ev->key.key == SDLK_V) {   // Ctrl+V — paste clipboard
            char *cb = SDL_GetClipboardText();
            if (cb) {
                input_insert(cb, (int)strlen(cb));
                SDL_free(cb);
            }
            return 1;
        }
        return 0;   // other keys — the main loop decides (it ignores the game)
    }
}

int console_event(const SDL_Event *ev)
{
    // ` — console toggle in ANY state (scancode — works on any layout)
    if (ev->type == SDL_EVENT_KEY_DOWN && !ev->key.repeat &&
        ev->key.scancode == SDL_SCANCODE_GRAVE) {
        console_toggle();
        return 1;
    }
    if (!s_active) return 0;

    if (ev->type == SDL_EVENT_KEY_DOWN)
        return console_event_key(ev);
    if (ev->type == SDL_EVENT_TEXT_INPUT) {
        if (ev->text.text && ev->text.text[0]) {
            // Control characters (Ctrl+A etc. arrive as text on some
            // platforms) — filter them out; everything else goes into the line.
            const char *p = ev->text.text;
            while (*p) {
                int l = utf8_len((unsigned char)*p);
                if (l == 1 && ((unsigned char)*p < 0x20 || *p == 0x7F)) {
                    p++;
                    continue;
                }
                input_insert(p, l);
                p += l;
            }
            return 1;
        }
        return 0;
    }
    if (ev->type == SDL_EVENT_MOUSE_WHEEL && ev->wheel.y != 0) {
        // Wheel UP (y>0) — toward older lines (s_scroll grows);
        // wheel DOWN (y<0) — toward the newest (s_scroll shrinks).
        s_scroll += (int)ev->wheel.y * 3;
        if (s_scroll < 0) s_scroll = 0;
        return 1;
    }
    return 0;
}

// --- Built-in commands --------------------------------------------------------
static void cmd_help(const char *args)
{
    (void)args;
    console_print("firerender console | commands:");
    for (int i = 0; i < s_cmds_n; i++)
        console_print("  %-14s %s", s_cmds[i].name, s_cmds[i].help);
}

static void cmd_cvarlist(const char *args)
{
    (void)args;
    console_print("cvars:");
    for (int i = 0; i < cvar_count(); i++) {
        const Cvar *c = cvar_at(i);
        if (c->flags & CVAR_STRING) {
            console_print("  %-14s = \"%s\" %s", c->name, cvar_get_string(c->name),
                          c->help);
            continue;
        }
        if (c->minv <= c->maxv)
            console_print("  %-14s = %-8.4g [%.4g..%.4g] %s",
                          c->name, c->value, c->minv, c->maxv, c->help);
        else
            console_print("  %-14s = %-8.4g %s", c->name, c->value, c->help);
    }
}

static void cmd_echo(const char *args)
{
    console_print("%s", args[0] ? args : "");
}

static void cmd_set(const char *args)
{
    char name[32];
    // set name "string" — for string cvars (CVAR_STRING)
    char sval[64];
    if (sscanf(args, "%31s \"%63[^\"]\"", name, sval) == 2) {
        if (cvar_index(name) < 0) {
            console_print("unknown variable \"%s\"", name);   // typo?
            return;
        }
        cvar_set_from_string(name, sval);
        if (cvar_at(cvar_index(name))->flags & CVAR_STRING)
            console_print("  %s = \"%s\"", name, cvar_get_string(name));
        else
            console_print("  %s = %.4g", name, cvar_get(name));
        return;
    }
    float v;
    int n = sscanf(args, "%31s %f", name, &v);
    if (n == 2) {
        if (cvar_index(name) < 0) {
            // Strictness for console input: a typo must not silently create
            // a variable (maps/Lua go through cvar_set directly).
            console_print("unknown variable \"%s\"", name);
            // prefix hint (like Tab completion)
            for (int i = 0; i < cvar_count(); i++) {
                const Cvar *c = cvar_at(i);
                if (strncmp(c->name, name, strlen(name)) == 0)
                    { console_print("  (похожая: %s)", c->name); break; }
            }
            return;
        }
        cvar_set(name, v);
        console_print("  %s = %.4g", name, cvar_get(name));
    } else if (n == 1) {
        int i = cvar_index(name);
        if (i >= 0 && (cvar_at(i)->flags & CVAR_STRING))
            console_print("  %s = \"%s\"%s", name, cvar_get_string(name),
                          " (строковая)");
        else
            console_print("  %s = %.4g%s", name, cvar_get(name),
                          i >= 0 ? "" : " (не зарегистрирован)");
    } else {
        console_print("usage: set <name> [value | \"string\"]");
    }
}

static void cmd_clear(const char *args)
{
    (void)args;
    s_count = 0;
    s_first = 0;
    s_scroll = 0;
}

static void cmd_cfg(const char *args)
{
    if (strcmp(args, "load") == 0) {
        int ok = cvar_load_config("config.cfg");
        binds_load_config();         // bind lines too
        console_print(ok ? "config.cfg loaded" : "no config.cfg");
    } else if (strcmp(args, "save") == 0) {
        console_save_config();
        console_print("config.cfg saved");
    } else {
        console_print("usage: cfg load|save");
    }
}

static void cmd_exec(const char *args)
{
    char path[256];
    if (sscanf(args, "%255s", path) != 1) {
        console_print("usage: exec <file.cfg>");
        return;
    }
    char *text = resources_read_text(path);
    if (!text) {
        console_print("exec: cannot read %s", path);
        return;
    }
    char *line = text;
    while (line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        size_t l = strlen(line);
        if (l > 0 && line[l - 1] == '\r') line[l - 1] = 0;
        console_exec(line);
        line = nl ? nl + 1 : NULL;
    }
    free(text);
}

static void cmd_quit(const char *args)
{
    (void)args;
    g_quit_request = 1;
}

static void cmd_version(const char *args)
{
    (void)args;
    console_print("firerender 0.4 build #%d (SDL3 + OpenGL)", ENGINE_BUILD_NUM);
}

// Strips surrounding quotes (declared below, used in cmd_bind)
static void strip_quotes(char *s);

// --- Binds: bind/unbind/bindlist (like in Half-Life) ---------------------------
static void cmd_bind(const char *args)
{
    char key[64], cmd[128];
    int n = sscanf(args, "%63s %127[^\r\n]", key, cmd);
    if (n == 2) strip_quotes(cmd);
    SDL_Scancode sc = binds_key_from_name(key);
    if (sc == SDL_SCANCODE_UNKNOWN) {
        console_print("bind: unknown key \"%s\"", key);
        return;
    }
    if (n == 2) {
        binds_set(sc, cmd);
        console_print("bind %s \"%s\"", key, cmd);
    } else {
        const char *c = binds_cmd(sc);
        console_print("bind %s \"%s\"", key, c ? c : "");
    }
}

static void cmd_unbind(const char *args)
{
    char key[64];
    if (sscanf(args, "%63s", key) != 1) {
        console_print("usage: unbind <key>");
        return;
    }
    SDL_Scancode sc = binds_key_from_name(key);
    if (sc == SDL_SCANCODE_UNKNOWN) {
        console_print("unbind: unknown key \"%s\"", key);
        return;
    }
    binds_clear(sc);
    console_print("unbind %s", key);
}

static void cmd_bindlist(const char *args)
{
    (void)args;
    console_print("binds:");
    for (int i = 0; i < binds_count(); i++) {
        const Bind *b = binds_at(i);
        console_print("  %-12s %s", binds_key_name(b->sc), b->cmd);
    }
}

static void cmd_unbindall(const char *args)
{
    (void)args;
    binds_clearall();
    console_print("all binds cleared (дефолты вернутся при перезапуске)");
}

// bind_reset — restore default bindings and save
static void cmd_bind_reset(const char *args)
{
    (void)args;
    binds_reset();
    console_save_config();
    console_print("binds reset to defaults");
}

// snd_debug [0|1] — HUD panel of playing sounds (audio debugging)
static void cmd_snd_debug(const char *args)
{
    float v;
    if (sscanf(args, "%f", &v) == 1)
        cvar_set("snd_debug", v > 0.5f ? 1.0f : 0.0f);
    console_print("snd_debug = %d", cvar_get("snd_debug") > 0.5f);
}

// profiler [0|1] — performance panel: polygons, vertices, draw calls,
// per-module time (what eats the most)
static void cmd_profiler(const char *args)
{
    float v;
    if (sscanf(args, "%f", &v) == 1)
        cvar_set("profiler", v > 0.5f ? 1.0f : 0.0f);
    console_print("profiler = %d", cvar_get("profiler") > 0.5f);
}

// m_smooth [0..1] — mouse look interpolation (0 = raw input,
// 1 = heavy smoothing). No argument — toggle on/off.
static void cmd_m_smooth(const char *args)
{
    float v;
    if (sscanf(args, "%f", &v) == 1) {
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        cvar_set("m_smooth", v);
    } else {
        cvar_set("m_smooth", cvar_get("m_smooth") > 0.5f ? 0.0f : 1.0f);
    }
    console_print("m_smooth = %.2f", cvar_get("m_smooth"));
}

// Strips surrounding quotes: '"cmd1; cmd2"' -> 'cmd1; cmd2'
static void strip_quotes(char *s)
{
    size_t l = strlen(s);
    if (l >= 2 && s[0] == '"' && s[l - 1] == '"') {
        memmove(s, s + 1, l - 2);
        s[l - 2] = 0;
    }
}

// alias <name> <command[; command]> — macro (like in HL)
static void cmd_alias(const char *args)
{
    char name[32], body[192];
    int n = sscanf(args, "%31s %191[^\r\n]", name, body);
    if (n == 2) {
        strip_quotes(body);
        int found = -1;
        for (int i = 0; i < s_alias_n; i++)
            if (strcmp(s_aliases[i].name, name) == 0) found = i;
        if (found < 0) {
            if (s_alias_n >= ALIAS_MAX) {
                console_print("alias: list full (%d)", ALIAS_MAX);
                return;
            }
            found = s_alias_n++;
        }
        snprintf(s_aliases[found].name, sizeof s_aliases[found].name, "%s", name);
        snprintf(s_aliases[found].body, sizeof s_aliases[found].body, "%s", body);
        console_print("alias %s \"%s\"", name, body);
    } else if (n == 1) {
        const char *b = alias_find(name);
        console_print("alias %s \"%s\"", name, b ? b : "");
    } else {
        console_print("usage: alias <name> <command[; command]>");
    }
}

static void cmd_unalias(const char *args)
{
    char name[32];
    if (sscanf(args, "%31s", name) != 1) {
        console_print("usage: unalias <name>");
        return;
    }
    for (int i = 0; i < s_alias_n; i++) {
        if (strcmp(s_aliases[i].name, name) == 0) {
            s_aliases[i] = s_aliases[s_alias_n - 1];
            s_alias_n--;
            console_print("unalias %s", name);
            return;
        }
    }
    console_print("unalias: нет алиаса \"%s\"", name);
}

static void cmd_aliaslist(const char *args)
{
    (void)args;
    console_print("aliases:");
    for (int i = 0; i < s_alias_n; i++)
        console_print("  %-14s %s", s_aliases[i].name, s_aliases[i].body);
}

// toggle <cvar> — flip 0/1 (a switch)
static void cmd_toggle(const char *args)
{
    char name[32];
    if (sscanf(args, "%31s", name) != 1) {
        console_print("usage: toggle <cvar>");
        return;
    }
    float v = cvar_get(name);
    cvar_set(name, v > 0.5f ? 0.0f : 1.0f);
    console_print("%s = %.4g", name, cvar_get(name));
}

// Alias load/save ("alias name body" lines in config.cfg)
static void alias_load_config(void)
{
    FILE *f = fopen("config.cfg", "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char name[32], body[192];
        if (sscanf(line, "alias %31s %191[^\r\n]", name, body) == 2) {
            char cmd[240];
            snprintf(cmd, sizeof cmd, "%s %s", name, body);
            cmd_alias(cmd);
        }
    }
    fclose(f);
}

// --- SDL_Log hook: engine output visible in the console -----------------------
static void log_hook(void *userdata, int category, SDL_LogPriority priority,
                     const char *message)
{
    (void)userdata;
    (void)category;
    (void)priority;
    fprintf(stderr, "%s\n", message);       // to the terminal — always
    if (cvar_get("developer") > 0.5f)       // to the console — per cvar
        ring_push(message);
}

// --- Initialization and lifecycle ---------------------------------------------
void console_init(void)
{
    g_quit_request = 0;
    cvar_init();
    cvar_register_ex("developer", 1.0f, 0, 1, CVAR_ARCHIVE, "вывод SDL_Log в консоль");
    cvar_register_ex("fov", 60.0f, 5.0f, 120.0f, CVAR_ARCHIVE, "угол обзора, градусы");
    cvar_register_ex("cl_sens", 0.0025f, 0.0001f, 0.1f, CVAR_ARCHIVE, "чувствительность мыши");
    cvar_register_ex("cl_walkspeed", 2.5f, 0.0f, 100.0f, CVAR_ARCHIVE, "скорость ходьбы, м/с");
    cvar_register_ex("sv_maxspeed", 0.0f, 0.0f, 100.0f, CVAR_ARCHIVE, "макс. скорость игрока, м/с (0 = cl_walkspeed)");
    cvar_register_ex("cl_flightspeed", 25.0f, 0.0f, 500.0f, CVAR_ARCHIVE, "скорость полёта, м/с");
    cvar_register_ex("sv_gravity", 20.0f, 0.0f, 100.0f, CVAR_ARCHIVE, "гравитация, м/с2");
    cvar_register_ex("sv_maxslope", 60.0f, 0.0f, 89.0f, CVAR_ARCHIVE, "крутизна склона, который можно взойти, градусы (0 = отключено)");
    cvar_register_ex("sv_jumpspeed", 7.0f, 0.0f, 50.0f, CVAR_ARCHIVE, "скорость прыжка, м/с");
    cvar_register_ex("r_shadows_mode", 1.0f, 0, 3, CVAR_ARCHIVE, "тени: 0=выкл, 1=CSM, 2=статические, 3=пятна");
    // Early (before the world): the menu's preset lighting must work pre-load
    cvar_register_ex("r_gi", 1.0f, 0.0f, 1.0f, CVAR_ARCHIVE, "глобальное освещение");
    cvar_register_ex("r_msaa", 0.0f, 0.0f, 16.0f, CVAR_ARCHIVE, "MSAA 0/4/16");
    cvar_register_ex("r_bloom", 1.0f, 0.0f, 1.0f, CVAR_ARCHIVE, "свечение (bloom)");
    cvar_register_ex("r_gamma", 1.0f, 0.5f, 2.5f, CVAR_ARCHIVE, "гамма-коррекция");
    cvar_register_ex("r_exposure", 1.0f, 0.1f, 5.0f, CVAR_ARCHIVE, "экспозиция HDR");
    cvar_register_ex("r_saturation", 1.4f, 0.0f, 3.0f, CVAR_ARCHIVE, "насыщенность");
    cvar_register_ex("r_contrast", 1.0f, 0.5f, 2.0f, CVAR_ARCHIVE, "контраст");
    cvar_register_ex("r_veg_vis_mult", 1.0f, 0.25f, 1.0f, CVAR_ARCHIVE,
                     "множитель радиуса прорисовки пшеницы (1 = карта)");
    cvar_register_ex("r_veg_minpix", 1.0f, 0.0f, 8.0f, CVAR_ARCHIVE,
                     "мин. размер спрайта травы в пикселях");
    cvar_register_ex("r_terr_detail_dist", 0.0f, 0.0f, 500.0f, CVAR_ARCHIVE,
                     "LOD деталей террейна: дальше — базовая карта (0 = выкл)");
    // Densities and GI: system defaults (a map may override them via
    // cvar_set_default) — registered RIGHT AWAY so the menu before the first
    // run shows real values, not zeros of unknown cvars
    cvar_register_ex("r_wheat_density", 140.0f, 0.0f, 200.0f, CVAR_ARCHIVE, "пшеница шт/м2");
    cvar_register_ex("r_grass_density", 1.2f, 0.0f, 10.0f, CVAR_ARCHIVE, "трава шт/м2");
    cvar_register_ex("r_dandelion_density", 0.05f, 0.0f, 2.0f, CVAR_ARCHIVE, "одуванчики шт/м2");
    cvar_register_ex("r_gi_scale", 0.5f, 0.25f, 1.0f, CVAR_ARCHIVE, "масштаб GI");
    cvar_register_ex("r_gi_rays", 6.0f, 2.0f, 32.0f, CVAR_ARCHIVE, "GI: лучей на пиксель");
    cvar_register_ex("r_gi_steps", 10.0f, 4.0f, 32.0f, CVAR_ARCHIVE, "GI: шагов марша");
    cvar_register_ex("r_render_scale", 1.0f, 0.4f, 1.0f, CVAR_ARCHIVE,
                     "масштаб рендера сцены (1 = полное разрешение окна)");
    cvar_register_ex("r_simple_render", 0.0f, 0.0f, 1.0f, CVAR_ARCHIVE,
                     "упрощённый рендер: пакет фпс-ухищрений для слабых GPU");
    cvar_register_ex("r_baked_light", 1.0f, 0.0f, 1.0f, CVAR_ARCHIVE,
                     "читать запечённый свет карты (map/baked), если он есть");
    cvar_register_ex("r_lowend_profile", 0.0f, 0.0f, 2.0f, CVAR_ARCHIVE,
                     "профиль: 0=balanced, 1=старый смартфон, 2=Celeron iGPU");
    cvar_register_ex("r_dynamic_resolution", 0.0f, 0.0f, 1.0f, CVAR_ARCHIVE,
                     "динамическое разрешение для low-end профиля");
    cvar_register_ex("r_veg_falloff_mult", 1.0f, 0.3f, 1.0f, CVAR_ARCHIVE,
                     "спад плотности поля: <1 — реже уже к среднему кольцу");
    cvar_register_ex("r_veg_regen_budget", 4096.0f, 1.0f, 4096.0f,
                     CVAR_ARCHIVE, "чанков регенерации за кадр");
    cvar_register_ex("r_cheap_fog", 0.0f, 0.0f, 1.0f, CVAR_ARCHIVE,
                     "туман только в шейдерах (без voxfog-проходов)");
    cvar_register_ex("r_scene_ldr", 0.0f, 0.0f, 1.0f, CVAR_ARCHIVE,
                     "сцена в RGBA8 без HDR (картошка: быстрее на слабых GPU)");
    cvar_register_ex("r_fog_mode", 1.0f, 0, 2, CVAR_ARCHIVE, "туман: 0=выкл, 1=объёмный, 2=простой");
    cvar_register_ex("r_bloom", 1.0f, 0, 1, CVAR_ARCHIVE, "свечение (bloom)");
    cvar_register_ex("r_crosshair", 1.0f, 0, 1, CVAR_ARCHIVE, "прицел (точка в центре)");
    cvar_register_ex("m_invert", 0.0f, 0, 1, CVAR_ARCHIVE, "инверсия мыши по Y");
    cvar_register_ex("m_smooth", 0.0f, 0.0f, 1.0f, CVAR_ARCHIVE,
                     "интерполяция взгляда (0 = обычный ввод, 1 = сильное сглаживание)");
    cvar_register_ex("r_fullscreen", 0.0f, 0, 1, CVAR_ARCHIVE, "полноэкранный режим");
    cvar_register_ex("r_vsync", 0.0f, 0, 1, CVAR_ARCHIVE, "вертикальная синхронизация");
    cvar_register_ex("r_wireframe", 0.0f, 0, 1, CVAR_ARCHIVE | CVAR_CHEAT, "каркасный режим (чит)");
    cvar_register_ex("snd_debug", 0.0f, 0, 1, CVAR_ARCHIVE, "играющие звуки в HUD (отладка)");
    cvar_register_ex("profiler", 0.0f, 0, 1, CVAR_ARCHIVE, "панель производительности (полигоны/время)");
    cvar_register_ex("sv_cheats", 1.0f, 0, 1, CVAR_ARCHIVE, "режим читов (0 = читы запрещены)");
    cvar_register_ex("sv_timescale", 1.0f, 0.01f, 10.0f, CVAR_ARCHIVE | CVAR_CHEAT, "скорость времени (чит)");
    cvar_register_ex("host_framerate", 0.0f, 0.0f, 1000.0f, CVAR_ARCHIVE, "лимит FPS (0 = нет)");
    cvar_register_ex("cam_thirdperson", 0.0f, 0, 1, CVAR_ARCHIVE, "вид от третьего лица");
    cvar_register_ex("cam_tpd", 4.0f, 1.0f, 12.0f, CVAR_ARCHIVE, "дистанция камеры 3-го лица, м");
    cvar_register_ex("snd_master", 1.0f, 0.0f, 1.0f, CVAR_ARCHIVE, "общая громкость");
    cvar_register_ex("snd_sfx", 1.0f, 0.0f, 1.0f, CVAR_ARCHIVE, "громкость звуков (шаги, эффекты)");
    cvar_register_ex("snd_dialog", 1.0f, 0.0f, 1.0f, CVAR_ARCHIVE, "громкость диалогов");
    cvar_register_ex("snd_music", 1.0f, 0.0f, 1.0f, CVAR_ARCHIVE, "громкость музыки");
    cvar_register_ex("snd_occlusion", 1.0f, 0.0f, 1.0f, CVAR_ARCHIVE,
                     "заглушение позиционного звука за холмом (0 = выкл)");
    cvar_register_ex("snd_occlusion_min", 0.25f, 0.0f, 1.0f, CVAR_ARCHIVE,
                     "пол глушения: минимальная громкость полностью заслонённого источника");
    // String variable (like name in GoldSrc): value stored in c->str.
    cvar_register_string("name", "player", "имя игрока (строка)");

    console_add_command("help", "список команд", cmd_help);
    console_add_command("cvarlist", "список переменных", cmd_cvarlist);
    console_add_command("echo", "печать текста", cmd_echo);
    console_add_command("set", "set <name> [value]", cmd_set);
    console_add_command("clear", "очистить консоль", cmd_clear);
    console_add_command("cfg", "cfg load|save", cmd_cfg);
    console_add_command("exec", "выполнить команды из файла", cmd_exec);
    console_add_command("quit", "выход из игры", cmd_quit);
    console_add_command("version", "версия движка", cmd_version);
    console_add_command("bind", "bind <key> <command>", cmd_bind);
    console_add_command("unbind", "unbind <key>", cmd_unbind);
    console_add_command("bindlist", "список привязок клавиш", cmd_bindlist);
    console_add_command("unbindall", "сбросить все привязки клавиш", cmd_unbindall);
    console_add_command("bind_reset", "вернуть дефолтные привязки", cmd_bind_reset);
    console_add_command("alias", "alias <имя> <команда[; команда]>", cmd_alias);
    console_add_command("unalias", "unalias <имя>", cmd_unalias);
    console_add_command("aliaslist", "список макросов", cmd_aliaslist);
    console_add_command("toggle", "toggle <cvar> — инверсия 0/1", cmd_toggle);
    console_add_command("snd_debug", "snd_debug [0|1] — звуки в HUD", cmd_snd_debug);
    console_add_command("profiler", "profiler [0|1] — панель производительности", cmd_profiler);
    console_add_command("m_smooth", "m_smooth [0..1] — интерполяция взгляда мыши", cmd_m_smooth);

    SDL_SetLogOutputFunction(log_hook, NULL);
    console_print("firerender console ready (help — список команд)");
}

void console_autoload(void)
{
    if (cvar_load_config("config.cfg"))
            console_print("config.cfg loaded");
    binds_load_config();        // bind lines from config.cfg
    alias_load_config();        // alias lines from config.cfg
}

void console_save_config(void)
{
    FILE *f = fopen("config.cfg", "w");
    if (!f) return;
    for (int i = 0; i < cvar_count(); i++) {
        const Cvar *c = cvar_at(i);
        if (!(c->flags & CVAR_ARCHIVE)) continue;
        // Save only CHANGED (different from default) variables — config.cfg
        // stores the differences, like in GoldSrc/Source.
        if (c->flags & CVAR_STRING) {
            if (strcmp(c->str, c->def_str) == 0) continue;
            fprintf(f, "%s \"%s\"\n", c->name, c->str);
        } else {
            if (c->value == c->def) continue;
            fprintf(f, "%s %.4g\n", c->name, c->value);
        }
    }
    binds_save_config(f);       // bind lines after the cvars
    for (int i = 0; i < s_alias_n; i++)
        fprintf(f, "alias %s %s\n", s_aliases[i].name, s_aliases[i].body);
    fclose(f);
}

// --- Rendering ----------------------------------------------------------------

// Copy of the line clipped to maxw width; unsupported codepoints become '?'
static int clip_line(const char *s, float maxw, char *out, int outsz)
{
    int n = 0;
    float w = 0;
    const char *p = s;
    while (*p && n < outsz - 1) {
        int cp = 0;
        int l = utf8_decode(p, &cp);
        float cw = hud_cp_width_small(cp);
        if (w + cw > maxw) break;
        w += cw;
        if (hud_cp_supported(cp)) {
            if (n + l >= outsz) break;
            memcpy(out + n, p, (size_t)l);
            n += l;
        } else {
            out[n++] = '?';
        }
        p += l;
    }
    out[n] = 0;
    return n;
}

// Index of the visible start of the input line (cursor stays on screen)
static int input_view_start(float maxw)
{
    float w = 0;
    int start = s_cur;
    while (start > 0) {
        int prev = utf8_prev(s_input, start);
        int cp = 0;
        utf8_decode(s_input + prev, &cp);
        float cw = hud_cp_width_small(cp);
        if (w + cw > maxw) break;
        w += cw;
        start = prev;
    }
    return start;
}

void console_render(int vw, int vh)
{
    if (!s_active || vw <= 0 || vh <= 0) return;
    glDisable(GL_DEPTH_TEST);        // console — on top of everything
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    float h = vh * 0.38f;
    const float panel[4] = { 0.08f, 0.08f, 0.12f, 0.90f };
    hud_rect_draw(vw, vh, 0, 0, (float)vw, h, panel);

    float line_h = hud_text_line_height_small();
    float maxw = (float)vw - 20.0f;         // text width inside the panel
    float y_in = h - 12.0f - line_h;        // input line — at panel bottom

    const float dim[4] = { 0.6f, 0.65f, 0.72f, 1.0f };
    // Hint line: baseline at ascent height — glyphs start at y=0
    // instead of going above the window top.
    hud_text_draw_small(vw, vh, 10, hud_text_ascent_small(),
                  "firerender console | ` или Esc — закрыть | ↑↓ история | Tab дополнение | Ctrl+V вставка",
                  dim);

    // Output lines: how many fit in the panel (from the hint bottom to the
    // input line). The panel is big — show all lines, not a fixed 14: with a
    // 15px font at 1080p that's ~24 lines.
    float hint_bottom = hud_text_ascent_small() + hud_text_descent_small() + 6.0f;
    int vis = (int)((y_in - hint_bottom) / line_h);
    if (vis < 4) vis = 4;
    if (vis > CON_VIS_MAX) vis = CON_VIS_MAX;
    s_vis_last = vis;
    if (s_count < vis) vis = s_count;
    int max_scroll = s_count > vis ? s_count - vis : 0;
    if (s_scroll > max_scroll) s_scroll = max_scroll;
    if (s_scroll < 0) s_scroll = 0;

    const float lc[4] = { 0.85f, 0.9f, 0.97f, 1.0f };
    char buf[CON_LINE_LEN];
    for (int i = 0; i < vis; i++) {
        int idx = s_count - vis - s_scroll + i;   // ring line index
        if (idx < 0) break;
        clip_line(ring_at(idx), maxw, buf, sizeof buf);
        hud_text_draw_small(vw, vh, 10, y_in - (float)(vis - i) * line_h, buf, lc);
    }

    // Scrollbar (if there's anything to scroll). Screen coordinates:
    // y grows DOWNWARD, so s_scroll=0 (we're at the bottom) — thumb at the
    // bottom, s_scroll=max (we're at the top) — thumb at the top.
    if (s_count > vis) {
        float sb_x = (float)vw - 8.0f, sb_w = 4.0f;
        float area_top = 30.0f, area_h = y_in - area_top;
        float thumb_h = area_h * vis / (float)s_count;
        if (thumb_h < 8.0f) thumb_h = 8.0f;
        float range = area_h - thumb_h;
        float pos = max_scroll > 0
                        ? range * (1.0f - (float)s_scroll / max_scroll)
                        : 0.0f;
        hud_rect_draw(vw, vh, sb_x, area_top + pos, sb_w, thumb_h, dim);
    }

    // Input line with horizontal scrolling and a blinking cursor
    char prompt[CON_INPUT_LEN + 8];
    snprintf(prompt, sizeof prompt, "] %s", s_input);
    int cur = s_cur + 2;                     // cursor position within prompt
    int start = input_view_start(maxw);
    clip_line(prompt + start, maxw, buf, sizeof buf);
    const float wc[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    float inx = 10.0f;
    hud_text_draw_small(vw, vh, inx, y_in, buf, wc);

    // Cursor: text width from the visible start to the cursor. The rectangle
    // matches the glyphs exactly: ascent above the baseline to descent below.
    if (start < cur) {
        float cw = 0;
        for (int i = start; i < cur; ) {
            int cp = 0;
            i += utf8_decode(prompt + i, &cp);
            cw += hud_cp_width_small(cp);
        }
        if ((SDL_GetTicks() / 500) % 2 == 0) {      // blink
            const float curc[4] = { 1.0f, 0.75f, 0.2f, 1.0f };
            hud_rect_draw(vw, vh, inx + cw + 1,
                          y_in - hud_text_ascent_small(),
                          8.0f,
                          hud_text_ascent_small() + hud_text_descent_small(),
                          curc);
        }
    }

    glDisable(GL_BLEND);
}

static int st_version(void)
{
    int fail = 0;
    // 10) version — build number (buildnum.h is generated by the Makefile)
    {
        console_exec("version");
        int n = 0;
        for (int i = 0; i < s_count && n == 0; i++)
            if (strstr(ring_at(i), "build #"))
                n = (int)strtol(strstr(ring_at(i), "build #") + 7, NULL, 10);
        if (n <= 0) { SDL_Log("console test: version/buildnum FAIL"); fail++; }
    }

    return fail;
}

static int st_fx(void)
{
    int fail = 0;
    // 11) fx — effect presets (fx_init_console in main.c)
    {
        console_exec("fx_list");
        int found = 0;
        for (int i = 0; i < s_count && !found; i++)
            if (strstr(ring_at(i), "blood")) found = 1;
        if (!found || !fx_find("blood") || !fx_find("rain") || !fx_find("smoke")
            || !fx_find("explosion") || !fx_find("fire") || !fx_find("snow")
            || !fx_find("sparks") || !fx_find("dust") || !fx_find("muzzle_flash")) {
            SDL_Log("console test: fx presets FAIL");
            fail++;
        }
    }

    return fail;
}

static int st_mesh(void)
{
    int fail = 0;
    // 12) mesh — .obj loader (quad = 2 tris, triangle = 1 tri)
    {
        Mesh *m = mesh_load_obj_mem("cube.obj",
            "# test cube\n"
            "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n"
            "vn 0 0 1\n"
            "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\n"
            "f 1/1/1 2/2/1 3/3/1 4/4/1\n"
            "v 0 0 1\nv 1 0 1\nv 0 1 1\n"
            "f 5//1 6//1 7//1\n");
        if (!m) { SDL_Log("console test: mesh load FAIL"); fail++; }
        else {
            // quad -> 6 verts / 2 tris; triangle -> 3 verts / 1 tri
            if (m->tri_count != 3 || m->vcount != 9) {
                SDL_Log("console test: mesh triangable FAIL (%d tri, %d v)",
                        m->tri_count, m->vcount);
                fail++;
            }
            mesh_free(m);
        }
    }

    return fail;
}

static int st_cmdreg(void)
{
    int fail = 0;
    // 13) Central command registry (cmdreg.c) — verify that key commands
    // from different modules really are registered (otherwise — silent
    // loss on registry overflow/build error).
    {
        const char *need[] = { "mesh", "glb", "map", "maps", "save", "load", "setpos",
                               "+forward", "-forward", "+use", "fx", "fx_list",
                               "weather", "matlist", "noclip", "thirdperson",
                               "reload_shaders", "ent_create", "screenshot" };
        for (size_t i = 0; i < sizeof need / sizeof need[0]; i++) {
            int found = 0;
            for (int j = 0; j < s_cmds_n && !found; j++)
                if (strcmp(s_cmds[j].name, need[i]) == 0) found = 1;
            if (!found) {
                SDL_Log("console test: command \"%s\" not registered", need[i]);
                fail++;
            }
        }
        if (s_cmds_n < 40) {
            SDL_Log("console test: only %d commands (<40), registry incomplete", s_cmds_n);
            fail++;
        }
    }

    return fail;
}

static int st_glb(void)
{
    int fail = 0;
    // 14) Binary glTF (.glb): glb_parse_file headless — chunks, hierarchy,
    // baking into world coordinates, materials (2004-era pseudo-PBR).
    {
        GlbModel *gm = glb_parse_file("models/test.glb");
        if (!gm) { SDL_Log("console test: glb parse FAIL (NULL)"); fail++; }
        else {
            int t = (gm->count != 1);
            if (!t) {
                GlbItem *o = &gm->items[0];
                if (o->index_count != 6 || o->vcount != 4) {
                    SDL_Log("console test: glb geo FAIL (idx=%d, v=%d)",
                            o->index_count, o->vcount); t = 1;
                }
                // root node translation [5,7,9] + child scale 2:
                // vertex 0 (0,0,0) -> world (5,7,9)
                if (fabsf(o->verts[0] - 5) > 1e-3f || fabsf(o->verts[1] - 7) > 1e-3f ||
                    fabsf(o->verts[2] - 9) > 1e-3f) {
                    SDL_Log("console test: glb world-tf FAIL (%.1f,%.1f,%.1f, expected 5,7,9)",
                            o->verts[0], o->verts[1], o->verts[2]); t = 1;
                }
                if (o->alpha < 0.999f) { SDL_Log("console test: glb alpha FAIL"); t = 1; }
                if (o->specular < 0.5f) { SDL_Log("console test: glb specular FAIL (%.2f)", o->specular); t = 1; }
                if (!(o->cull == MAT_CULL_NONE)) { SDL_Log("console test: glb doubleSided FAIL (cull=%d)", o->cull); t = 1; }
                if (o->blend != MAT_BLEND_OPAQUE) { SDL_Log("console test: glb alphaMode FAIL (blend=%d)", o->blend); t = 1; }
                if (o->tint[0] < 0.19f || o->tint[0] > 0.21f) {
                    SDL_Log("console test: glb baseColorFactor FAIL (%.2f)", o->tint[0]); t = 1;
                }
            }
            if (t) fail++;
            glb_model_free(gm);
        }
    }

    return fail;
}

static int st_glb_anim(void)
{
    int fail = 0;
    // 15) Animated glTF (.glb): glb_anim_parse headless — skins, weights,
    // animation clips (channels/samplers) and bind-pose geometry.
    {
        AnimGlb *a = glb_anim_parse("models/test_anim.glb");
        if (!a) { SDL_Log("console test: glb_anim parse FAIL (NULL)"); fail++; }
        else {
            int t = 0;
            if (a->nnode != 4) { SDL_Log("console test: glb_anim nodes FAIL (%d)", a->nnode); t = 1; }
            if (a->nprim != 1) { SDL_Log("console test: glb_anim prims FAIL (%d)", a->nprim); t = 1; }
            if (a->nskin != 1) { SDL_Log("console test: glb_anim skins FAIL (%d)", a->nskin); t = 1; }
            if (a->nclip != 1) { SDL_Log("console test: glb_anim clips FAIL (%d)", a->nclip); t = 1; }
            if (a->nskin > 0 && a->skins[0].joint_count != 2) {
                SDL_Log("console test: glb_anim joints FAIL (%d)", a->skins[0].joint_count); t = 1;
            }
            if (a->nprim > 0) {
                AnimPrim *P = &a->prims[0];
                if (P->vcount != 48) { SDL_Log("console test: glb_anim vcount FAIL (%d)", P->vcount); t = 1; }
                if (!P->bone_idx || !P->bone_w) { SDL_Log("console test: glb_anim weights FAIL"); t = 1; }
                // first half of verts (0-23) bound to bone 0, second (24-47) to bone 1
                if (P->bone_idx) {
                    if (P->bone_idx[0] != 0 || P->bone_idx[23*4] != 0 ||
                        P->bone_idx[24*4] != 1 || P->bone_idx[47*4] != 1) {
                        SDL_Log("console test: glb_anim joints FAIL (%u,%u,%u,%u)",
                                P->bone_idx[0], P->bone_idx[23*4], P->bone_idx[24*4], P->bone_idx[47*4]);
                        t = 1;
                    }
                    if (P->bone_w[3] != 0.0f || P->bone_w[24*4+3] != 0.0f) {
                        SDL_Log("console test: glb_anim weights FAIL"); t = 1;
                    }
                }
            }
            if (a->nclip > 0 && a->clips[0].nch != 1) {
                SDL_Log("console test: glb_anim channels FAIL (%d)", a->clips[0].nch); t = 1;
            }
            if (t) fail++;
            glb_anim_free(a);
        }
    }

    return fail;
}

static int st_glb_girl(void)
{
    int fail = 0;
    // 15b) Real model with a big skin: handpaint_-_girl_01.glb (99 bones,
    // clip name "IDLE", a non-skinned primitive). Tests per-primitive remap
    // and multi-bone skins that wouldn't fit into uBones[64] wholesale.
    {
        AnimGlb *a = glb_anim_parse("models/handpaint_-_girl_01.glb");
        if (!a) { SDL_Log("console test: glb_anim girl parse FAIL (NULL)"); fail++; }
        else {
            int t = 0;
            if (a->nskin < 1 || a->skins[0].joint_count != 99) {
                SDL_Log("console test: glb_anim girl joints FAIL (%d)",
                        a->nskin > 0 ? a->skins[0].joint_count : -1); t = 1;
            }
            if (a->nprim != 6) { SDL_Log("console test: glb_anim girl prims FAIL (%d)", a->nprim); t = 1; }
            if (a->nclip != 1) { SDL_Log("console test: glb_anim girl clips FAIL (%d)", a->nclip); t = 1; }
            if (a->nclip > 0 && strcmp(a->clips[0].name, "IDLE")) {
                SDL_Log("console test: glb_anim girl clip name FAIL (%s)", a->clips[0].name); t = 1;
            }
            int skinned = 0, plain = 0;
            for (int i = 0; i < a->nprim; i++) {
                if (a->prims[i].bone_idx) skinned++; else plain++;
                if (a->prims[i].bone_idx)
                    for (int v = 0; v < a->prims[i].vcount; v++)
                        for (int k = 0; k < 4; k++)
                            if (a->prims[i].bone_idx[4*v+k] >= a->skins[0].joint_count) { t = 1; }
            }
            if (skinned != 5 || plain != 1) {
                SDL_Log("console test: glb_anim girl skin split FAIL (%d skinned, %d plain)",
                        skinned, plain); t = 1;
            }
            if (t) fail++;
            glb_anim_free(a);
        }
    }

    return fail;
}

// --- Self-test (FR_TEST=x) -----------------------------------------------------
void console_selftest(void)
{
    int fail = 0;

    // 1) commands and cvars with clamping
    console_exec("set fov 90");
    if (cvar_get("fov") != 90.0f) { SDL_Log("console test: set FAIL"); fail++; }
    console_exec("set fov 999");
    if (cvar_get("fov") != 120.0f) { SDL_Log("console test: clamp hi FAIL"); fail++; }
    console_exec("set fov -7");
    if (cvar_get("fov") != 5.0f) { SDL_Log("console test: clamp lo FAIL"); fail++; }
    console_exec("set fov 60");

    // 2) text input and Backspace via SDL events
    console_toggle();
    SDL_Event ev;
    memset(&ev, 0, sizeof ev);
    static char txt[16] = "abc";
    ev.type = SDL_EVENT_TEXT_INPUT;
    ev.text.text = txt;
    if (!console_event(&ev)) { SDL_Log("console test: text consume FAIL"); fail++; }
    if (strcmp(s_input, "abc") != 0 || s_cur != 3)
        { SDL_Log("console test: insert FAIL"); fail++; }
    ev.type = SDL_EVENT_KEY_DOWN;
    ev.key.key = SDLK_LEFT;
    if (!console_event(&ev) || s_cur != 2) { SDL_Log("console test: left FAIL"); fail++; }
    ev.key.key = SDLK_BACKSPACE;
    console_event(&ev);
    if (strcmp(s_input, "ac") != 0 || s_cur != 1)
        { SDL_Log("console test: backspace mid FAIL"); fail++; }
    ev.key.key = SDLK_DELETE;
    console_event(&ev);
    if (strcmp(s_input, "a") != 0) { SDL_Log("console test: delete FAIL"); fail++; }

    // 3) Tab completion: "se" + Tab = "set" (replaces the word before cursor)
    s_inlen = 0; s_cur = 0; s_input[0] = 0;
    txt[0] = 's'; txt[1] = 'e'; txt[2] = 0;
    ev.type = SDL_EVENT_TEXT_INPUT;
    ev.text.text = txt;
    console_event(&ev);
    ev.type = SDL_EVENT_KEY_DOWN;
    ev.key.key = SDLK_TAB;
    console_event(&ev);
    if (strcmp(s_input, "set") != 0) {
        SDL_Log("console test: tab FAIL (got '%s')", s_input);
        fail++;
    }

    // 4) Enter executes the command
    txt[0] = ' '; txt[1] = 'f'; txt[2] = 'o'; txt[3] = 'v'; txt[4] = ' ';
    txt[5] = '1'; txt[6] = '0'; txt[7] = '0'; txt[8] = 0;
    ev.type = SDL_EVENT_TEXT_INPUT;
    ev.text.text = txt;
    console_event(&ev);
    ev.type = SDL_EVENT_KEY_DOWN;
    ev.key.key = SDLK_RETURN;
    console_event(&ev);
    if (cvar_get("fov") != 100.0f) { SDL_Log("console test: exec FAIL"); fail++; }
    if (s_hist_n < 1) { SDL_Log("console test: history FAIL"); fail++; }

    // 5) exec <file> — commands from a file
    {
        FILE *f = fopen("_fr_test.cfg", "w");
        if (f) {
            fprintf(f, "set fov 77\nset r_fog_mode 0\n");
            fclose(f);
            console_exec("exec _fr_test.cfg");
            if (cvar_get("fov") != 77.0f || cvar_get("r_fog_mode") != 0.0f) {
                SDL_Log("console test: exec FAIL");
                fail++;
            }
            remove("_fr_test.cfg");
            console_exec("set fov 60");
            console_exec("set r_fog_mode 1");
        } else {
            SDL_Log("console test: exec (no tmp file) SKIP");
        }
    }

    // 5b) a typo in set — does NOT silently create a variable (strict console)
    {
        console_exec("set r_fog 0");              // no such cvar
        if (cvar_index("r_fog") >= 0) {
            SDL_Log("console test: typo-set FAIL (junk cvar was created)");
            fail++;
        }
    }

    // 6) Cyrillic and typography: glyphs in the atlas, UTF-8 input, Backspace
    if (!hud_cp_supported(0x410) || !hud_cp_supported(0x2014) ||
        !hud_cp_supported(0x2191)) {
        SDL_Log("console test: cyrillic/typography glyph FAIL");
        fail++;
    }
    s_inlen = 0; s_cur = 0; s_input[0] = 0;
    {
        static char ru[16] = "\xD0\xBF\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82"; // UTF-8 "hello" (ru)
        ev.type = SDL_EVENT_TEXT_INPUT;
        ev.text.text = ru;
        console_event(&ev);
        if (strcmp(s_input, ru) != 0) {
            SDL_Log("console test: cyrillic input FAIL");
            fail++;
        }
        ev.type = SDL_EVENT_KEY_DOWN;
        ev.key.key = SDLK_BACKSPACE;
        console_event(&ev);
        if (strcmp(s_input, "\xD0\xBF\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5") != 0) {
            SDL_Log("console test: cyrillic backspace FAIL");
            fail++;
        }
        s_inlen = 0; s_cur = 0; s_input[0] = 0;
    }

    // 7) binds: bind/unbind, +commands
    {
        SDL_Scancode sc = SDL_GetScancodeFromName("K");
        console_exec("bind k +forward");
        const char *c = binds_cmd(sc);
        if (!c || strcmp(c, "+forward") != 0) {
            SDL_Log("console test: bind FAIL");
            fail++;
        }
        console_exec("+forward");
        if (!g_act[A_FORWARD]) { SDL_Log("console test: +forward FAIL"); fail++; }
        console_exec("-forward");
        if (g_act[A_FORWARD]) { SDL_Log("console test: -forward FAIL"); fail++; }
        console_exec("unbind k");
        if (binds_cmd(sc) != NULL) { SDL_Log("console test: unbind FAIL"); fail++; }
    }

    // 8) aliases, toggle, ";" combinations
    {
        console_exec("alias testme \"set fov 66; set r_fog_mode 0\"");
        console_exec("testme");
        if (cvar_get("fov") != 66.0f || cvar_get("r_fog_mode") != 0.0f) {
            SDL_Log("console test: alias FAIL");
            fail++;
        }
        console_exec("toggle r_fog_mode");
        if (cvar_get("r_fog_mode") != 1.0f) { SDL_Log("console test: toggle FAIL"); fail++; }
        console_exec("unalias testme");
        console_exec("set fov 60");
    }

    // 9) closing the console
    console_toggle();
    if (console_active()) { SDL_Log("console test: close FAIL"); fail++; }

    // 9.1) string cvars: set name "string" -> cvar_set_string /
    //      cvar_get_string (without config.cfg — just the API)
    {
        console_exec("set name \"bob\"");
        if (strcmp(cvar_get_string("name"), "bob") != 0) {
            SDL_Log("console test: string cvar set FAIL (%s)", cvar_get_string("name"));
            fail++;
        }
        cvar_set_string("name", "alice");
        if (strcmp(cvar_get_string("name"), "alice") != 0) {
            SDL_Log("console test: string cvar get FAIL");
            fail++;
        }
        cvar_set_string("name", "player");   // restore the default
    }

    // 9.2) wait: "set fov 55; wait; set fov 56" — the command after wait is
    //      delayed by 1 frame (runs in console_drain from the main loop)
    {
        console_exec("set fov 55; wait; set fov 56");
        if (cvar_get("fov") != 55.0f) {
            SDL_Log("console test: wait pre-drain FAIL (fov=%g)", cvar_get("fov"));
            fail++;
        }
        console_drain();                    // one frame later
        if (cvar_get("fov") != 56.0f) {
            SDL_Log("console test: wait post-drain FAIL (fov=%g)", cvar_get("fov"));
            fail++;
        }
        console_exec("set fov 59; wait 2; set fov 58");
        if (cvar_get("fov") != 59.0f) { SDL_Log("console test: wait2 FAIL"); fail++; }
        console_drain();
        if (cvar_get("fov") != 59.0f) { SDL_Log("console test: wait2 early FAIL"); fail++; }
        console_drain();
        if (cvar_get("fov") != 58.0f) { SDL_Log("console test: wait2 late FAIL"); fail++; }
        console_exec("set fov 60");
    }

    // 9.3) material registry by name (material_register/find/get)
    {
        Material m;
        material_defaults(&m);
        m.alpha = 0.5f;
        m.tint[0] = 0.2f;
        material_register("test_mat", &m);
        const Material *f = material_find("test_mat");
        if (!f || f->alpha != 0.5f || f->tint[0] != 0.2f) {
            SDL_Log("console test: material registry find FAIL");
            fail++;
        }
        Material g;
        int ok = material_get("test_mat", &g);
        if (!ok || g.alpha != 0.5f) { SDL_Log("console test: material get FAIL"); fail++; }
        ok = material_get("no_such_mat", &g);       // missing — defaults, 0
        if (ok || g.alpha != 1.0f) { SDL_Log("console test: material default FAIL"); fail++; }
        material_clear_all();
    }

    // Sections 10-15b are self-contained — moved into the st_* helpers above;
    // the check order and failure counting stay the same.
    fail += st_version();
    fail += st_fx();
    fail += st_mesh();
    fail += st_cmdreg();
    fail += st_glb();
    fail += st_glb_anim();
    fail += st_glb_girl();
    SDL_Log("console selftest: %s", fail ? "FAIL" : "OK");
}
