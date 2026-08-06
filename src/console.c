// ============================================================================
//  console.c — реализация консоли разработчика (см. console.h)
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

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CON_LINES      400     // строк вывода в кольце (скроллбек)
#define CON_LINE_LEN   160     // длина строки вывода
#define CON_INPUT_LEN  128     // длина строки ввода
#define CON_HISTORY    32      // глубина истории команд
#define CON_VIS_MAX    60      // потолок строк вывода на экране (считается по панели)

int g_quit_request = 0;

// --- Кольцо строк вывода ------------------------------------------------------
static char s_lines[CON_LINES][CON_LINE_LEN];
static int  s_first = 0;      // индекс старейшей строки в кольце
static int  s_count = 0;      // всего строк в кольце

static void ring_push(const char *s)
{
    snprintf(s_lines[(s_first + s_count) % CON_LINES], CON_LINE_LEN, "%s", s);
    if (s_count < CON_LINES) s_count++;
    else s_first = (s_first + 1) % CON_LINES;
}

static const char *ring_at(int i)   // i: 0 = старейшая строка
{
    return s_lines[(s_first + i) % CON_LINES];
}

// --- Ввод и история -----------------------------------------------------------
static char s_input[CON_INPUT_LEN];
static int  s_inlen = 0;      // длина строки ввода (байт)
static int  s_cur = 0;        // позиция курсора (байт, может быть < inlen)
static char s_history[CON_HISTORY][CON_INPUT_LEN];
static int  s_hist_n = 0, s_hist_pos = -1;

// --- Состояние ----------------------------------------------------------------
static int s_active = 0;
static int s_scroll = 0;      // сколько строк прокручено вверх (0 = низ)
static int s_vis_last = 14;   // строк вывода, поместившихся в панель (для PgUp/PgDn)

// --- Команды ------------------------------------------------------------------
static struct {
    const char *name;
    const char *help;
    ConsoleCmdFn fn;
} s_cmds[CON_MAX_COMMANDS];
static int s_cmds_n = 0;

// --- Алиасы (макросы, как в HL): alias <имя> <команда; команда> ----------------
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

// Выполняет ОДНУ команду (имя + аргументы). Алиасы раскрываются (с защитой
// от рекурсии).
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

void console_exec(const char *line)
{
    // Команды могут разделяться ";" — но НЕ внутри кавычек
    // (alias/bind: "cmd1; cmd2" — единое тело).
    char buf[256];
    snprintf(buf, sizeof buf, "%s", line ? line : "");
    char *p = buf;
    while (p && *p) {
        char *semi = NULL;
        int inq = 0;
        for (char *q = p; *q; q++) {
            if (*q == '"') inq = !inq;
            else if (*q == ';' && !inq) { semi = q; break; }
        }
        if (semi) *semi = 0;
        console_exec_one(p);
        p = semi ? semi + 1 : NULL;
    }
}

void console_toggle(void)
{
    s_active = !s_active;
    s_scroll = 0;                       // при открытии — вниз, к свежему
    s_hist_pos = -1;
    events_emit(s_active ? EV_CONSOLE_OPENED : EV_CONSOLE_CLOSED, NULL, NULL);
}

// --- Ввод строки --------------------------------------------------------------

// Вставить текст на позицию курсора
static void input_insert(const char *text, int len)
{
    if (len <= 0 || s_inlen + len >= CON_INPUT_LEN) return;
    memmove(s_input + s_cur + len, s_input + s_cur, (size_t)s_inlen - s_cur);
    memcpy(s_input + s_cur, text, (size_t)len);
    s_inlen += len;
    s_cur += len;
    s_input[s_inlen] = 0;
}

// Удалить символ ПЕРЕД курсором (Backspace). Возвращает 1, если удалил.
static int input_backspace(void)
{
    if (s_cur <= 0) return 0;
    int prev = utf8_prev(s_input, s_cur);
    memmove(s_input + prev, s_input + s_cur, (size_t)s_inlen - s_cur + 1);
    s_inlen -= s_cur - prev;
    s_cur = prev;
    return 1;
}

// Удалить символ ПОД курсором (Delete).
static int input_delete(void)
{
    if (s_cur >= s_inlen) return 0;
    int next = s_cur + utf8_len((unsigned char)s_input[s_cur]);
    memmove(s_input + s_cur, s_input + next, (size_t)s_inlen - next + 1);
    s_inlen -= next - s_cur;
    return 1;
}

// Слово перед курсором (для Ctrl+W)
static int input_word_start(void)
{
    int i = s_cur;
    while (i > 0 && s_input[i - 1] != ' ' && s_input[i - 1] != '\t') i--;
    return i;
}

// --- Tab: дополнение имён команд и переменных ---------------------------------
static const char *s_tab_matches[CON_MAX_COMMANDS + CVARS_MAX];
static int s_tab_n = 0, s_tab_idx = 0;
static int s_tab_base_len = 0, s_tab_base_hash = 0;   // база последнего цикла

static unsigned tab_hash(const char *s)
{
    unsigned h = 5381;
    while (*s) h = h * 33 + (unsigned char)*s++;
    return h;
}

static void input_complete(void)
{
    // Слово ДО курсора (первое слово строки — имя команды/переменной)
    int wstart = s_cur;
    while (wstart > 0 && s_input[wstart - 1] != ' ' && s_input[wstart - 1] != '\t')
        wstart--;
    int wlen = s_cur - wstart;

    // Тот же текст, что при прошлом Tab? — цикл по совпадениям
    unsigned h = tab_hash(s_input + wstart);
    if (s_tab_base_len != wlen || (int)s_tab_base_hash != (int)h) {
        s_tab_base_len = wlen;
        s_tab_base_hash = h;
        s_tab_idx = 0;
        s_tab_n = 0;
        for (int i = 0; i < s_cmds_n; i++)
            if (strncmp(s_cmds[i].name, s_input + wstart, (size_t)wlen) == 0)
                s_tab_matches[s_tab_n++] = s_cmds[i].name;
        for (int i = 0; i < cvar_count(); i++) {
            const Cvar *c = cvar_at(i);
            if (strncmp(c->name, s_input + wstart, (size_t)wlen) == 0)
                s_tab_matches[s_tab_n++] = c->name;
        }
        if (s_tab_n == 0) return;        // нет совпадений — молчим
    }
    if (s_tab_n <= 0) return;
    if (s_tab_idx >= s_tab_n) s_tab_idx = 0;

    // Заменить слово от wstart до курсора на совпадение
    const char *m = s_tab_matches[s_tab_idx];
    int mlen = (int)strlen(m);
    memmove(s_input + wstart + mlen, s_input + s_cur,
            (size_t)s_inlen - s_cur + 1);
    memcpy(s_input + wstart, m, (size_t)mlen);
    s_inlen += mlen - wlen;
    s_cur = wstart + mlen;
    s_tab_idx++;                          // следующий Tab — следующее совпадение
}

// --- Обработка событий --------------------------------------------------------
static int console_event_key(const SDL_Event *ev)
{
    SDL_Keymod mod = (SDL_Keymod)ev->key.mod;
    bool ctrl = (mod & SDL_KMOD_CTRL) != 0;

    if (ev->key.repeat && ev->key.key != SDLK_BACKSPACE && ev->key.key != SDLK_DELETE)
        return 0;   // повтор клавиши в консоль не идёт (кроме удаления)

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
            s_scroll = 0;                  // новый вывод — вниз
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
        if (ctrl) {                        // Ctrl+W — стереть слово
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
        if (ctrl) {                        // Ctrl+← — к началу слова
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
        if (ctrl && ev->key.key == SDLK_U) {   // Ctrl+U — стереть до курсора
            memmove(s_input, s_input + s_cur, (size_t)s_inlen - s_cur + 1);
            s_inlen -= s_cur;
            s_cur = 0;
            return 1;
        }
        if (ctrl && ev->key.key == SDLK_K) {   // Ctrl+K — стереть после
            s_inlen = s_cur;
            s_input[s_inlen] = 0;
            return 1;
        }
        if (ctrl && ev->key.key == SDLK_C) {   // Ctrl+C — копировать строку
            if (s_inlen > 0) SDL_SetClipboardText(s_input);
            return 1;
        }
        if (ctrl && ev->key.key == SDLK_V) {   // Ctrl+V — вставить буфер
            char *cb = SDL_GetClipboardText();
            if (cb) {
                input_insert(cb, (int)strlen(cb));
                SDL_free(cb);
            }
            return 1;
        }
        return 0;   // прочие клавиши — главный цикл сам решит (он игнорит игру)
    }
}

int console_event(const SDL_Event *ev)
{
    // ` — тумблер консоли в ЛЮБОМ состоянии (scancode — на любой раскладке)
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
            // Управляющие символы (Ctrl+A и пр. на некоторых платформах
            // приходят текстом) — отфильтровать; остальное — в строку.
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
        // Колесо ВВЕРХ (y>0) — к старым строкам (s_scroll растёт);
        // колесо ВНИЗ (y<0) — к свежим (s_scroll падает).
        s_scroll += (int)ev->wheel.y * 3;
        if (s_scroll < 0) s_scroll = 0;
        return 1;
    }
    return 0;
}

// --- Встроенные команды -------------------------------------------------------
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
    float v;
    int n = sscanf(args, "%31s %f", name, &v);
    if (n == 2) {
        cvar_set(name, v);
        console_print("  %s = %.4g", name, cvar_get(name));
    } else if (n == 1) {
        int i = cvar_index(name);
        console_print("  %s = %.4g%s", name, cvar_get(name),
                      i >= 0 ? "" : " (не зарегистрирован)");
    } else {
        console_print("usage: set <name> [value]");
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
        binds_load_config();         // bind-строки — тоже
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

// Снимает обрамляющие кавычки (объявлена ниже, используется в cmd_bind)
static void strip_quotes(char *s);

// --- Бинды: bind/unbind/bindlist (как в Half-Life) -----------------------------
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

// bind_reset — вернуть дефолтные привязки и сохранить
static void cmd_bind_reset(const char *args)
{
    (void)args;
    binds_reset();
    console_save_config();
    console_print("binds reset to defaults");
}

// snd_debug [0|1] — панель играющих звуков в HUD (отладка звука)
static void cmd_snd_debug(const char *args)
{
    float v;
    if (sscanf(args, "%f", &v) == 1)
        cvar_set("snd_debug", v > 0.5f ? 1.0f : 0.0f);
    console_print("snd_debug = %d", cvar_get("snd_debug") > 0.5f);
}

// profiler [0|1] — панель производительности: полигоны, вершины, draw calls,
// время по модулям (что жрёт больше всего)
static void cmd_profiler(const char *args)
{
    float v;
    if (sscanf(args, "%f", &v) == 1)
        cvar_set("profiler", v > 0.5f ? 1.0f : 0.0f);
    console_print("profiler = %d", cvar_get("profiler") > 0.5f);
}

// Снимает обрамляющие кавычки: '"cmd1; cmd2"' -> 'cmd1; cmd2'
static void strip_quotes(char *s)
{
    size_t l = strlen(s);
    if (l >= 2 && s[0] == '"' && s[l - 1] == '"') {
        memmove(s, s + 1, l - 2);
        s[l - 2] = 0;
    }
}

// alias <имя> <команда[; команда]> — макрос (как в HL)
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

// toggle <cvar> — инверсия 0/1 (тумблер)
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

// Загрузка/сохранение алиасов (строки "alias name body" в config.cfg)
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

// --- Перехват SDL_Log: вывод движка виден в консоли ---------------------------
static void log_hook(void *userdata, int category, SDL_LogPriority priority,
                     const char *message)
{
    (void)userdata;
    (void)category;
    (void)priority;
    fprintf(stderr, "%s\n", message);       // в терминал — всегда
    if (cvar_get("developer") > 0.5f)       // в консоль — по cvar'у
        ring_push(message);
}

// --- Инициализация и жизненный цикл -------------------------------------------
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
    cvar_register_ex("sv_jumpspeed", 7.0f, 0.0f, 50.0f, CVAR_ARCHIVE, "скорость прыжка, м/с");
    cvar_register_ex("r_shadows_mode", 1.0f, 0, 3, CVAR_ARCHIVE, "тени: 0=выкл, 1=CSM, 2=статические, 3=пятна");
    cvar_register_ex("r_fog_mode", 1.0f, 0, 2, CVAR_ARCHIVE, "туман: 0=выкл, 1=объёмный, 2=простой");
    cvar_register_ex("r_bloom", 1.0f, 0, 1, CVAR_ARCHIVE, "свечение (bloom)");
    cvar_register_ex("m_invert", 0.0f, 0, 1, CVAR_ARCHIVE, "инверсия мыши по Y");
    cvar_register_ex("r_fullscreen", 0.0f, 0, 1, CVAR_ARCHIVE, "полноэкранный режим");
    cvar_register_ex("r_vsync", 0.0f, 0, 1, CVAR_ARCHIVE, "вертикальная синхронизация");
    cvar_register_ex("r_wireframe", 0.0f, 0, 1, CVAR_ARCHIVE | CVAR_CHEAT, "каркасный режим (чит)");
    cvar_register_ex("snd_debug", 0.0f, 0, 1, CVAR_ARCHIVE, "играющие звуки в HUD (отладка)");
    cvar_register_ex("profiler", 0.0f, 0, 1, CVAR_ARCHIVE, "панель производительности (полигоны/время)");
    cvar_register_ex("sv_cheats", 1.0f, 0, 1, CVAR_ARCHIVE, "режим читов (0 = читы запрещены)");
    cvar_register_ex("sv_timescale", 1.0f, 0.01f, 10.0f, CVAR_ARCHIVE | CVAR_CHEAT, "скорость времени (чит)");
    cvar_register_ex("host_framerate", 0.0f, 0.0f, 1000.0f, CVAR_ARCHIVE, "лимит FPS (0 = нет)");
    cvar_register_ex("cam_thirdperson", 0.0f, 0, 1, CVAR_ARCHIVE, "вид от третьего лица");
    cvar_register_ex("snd_master", 1.0f, 0.0f, 1.0f, CVAR_ARCHIVE, "общая громкость");
    cvar_register_ex("snd_sfx", 1.0f, 0.0f, 1.0f, CVAR_ARCHIVE, "громкость звуков (шаги, эффекты)");
    cvar_register_ex("snd_dialog", 1.0f, 0.0f, 1.0f, CVAR_ARCHIVE, "громкость диалогов");
    cvar_register_ex("snd_music", 1.0f, 0.0f, 1.0f, CVAR_ARCHIVE, "громкость музыки");

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

    SDL_SetLogOutputFunction(log_hook, NULL);
    console_print("firerender console ready (help — список команд)");
}

void console_autoload(void)
{
    if (cvar_load_config("config.cfg"))
        console_print("config.cfg loaded");
    binds_load_config();        // bind-строки из config.cfg
    alias_load_config();        // alias-строки из config.cfg
}

void console_save_config(void)
{
    FILE *f = fopen("config.cfg", "w");
    if (!f) return;
    for (int i = 0; i < cvar_count(); i++) {
        const Cvar *c = cvar_at(i);
        if (!(c->flags & CVAR_ARCHIVE)) continue;
        fprintf(f, "%s %.4g\n", c->name, c->value);
    }
    binds_save_config(f);       // bind-строки после cvar'ов
    for (int i = 0; i < s_alias_n; i++)
        fprintf(f, "alias %s %s\n", s_aliases[i].name, s_aliases[i].body);
    fclose(f);
}

// --- Рендер -------------------------------------------------------------------

// Копия строки, обрезанная по ширине maxw; неподдержанные codepoint'ы — '?'
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

// Индекс начала видимой части строки ввода (курсор остаётся на экране)
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
    glDisable(GL_DEPTH_TEST);        // консоль — поверх всего
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    float h = vh * 0.38f;
    const float panel[4] = { 0.08f, 0.08f, 0.12f, 0.90f };
    hud_rect_draw(vw, vh, 0, 0, (float)vw, h, panel);

    float line_h = hud_text_line_height_small();
    float maxw = (float)vw - 20.0f;         // ширина текста в панели
    float y_in = h - 12.0f - line_h;        // строка ввода — внизу панели

    const float dim[4] = { 0.6f, 0.65f, 0.72f, 1.0f };
    // Подсказка: базовая линия на высоте ascent — глифы начинаются с y=0,
    // а не уходят за верх окна.
    hud_text_draw_small(vw, vh, 10, hud_text_ascent_small(),
                  "firerender console | ` или Esc — закрыть | ↑↓ история | Tab дополнение | Ctrl+V вставка",
                  dim);

    // Строки вывода: сколько помещается в панель (от низа подсказки до
    // строки ввода). Панель большая — показываем все строки, а не фиксированные
    // 14: при 15px шрифте на 1080p это ~24 строки.
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
        int idx = s_count - vis - s_scroll + i;   // строка кольца
        if (idx < 0) break;
        clip_line(ring_at(idx), maxw, buf, sizeof buf);
        hud_text_draw_small(vw, vh, 10, y_in - (float)(vis - i) * line_h, buf, lc);
    }

    // Полоса прокрутки (если есть что крутить). Экранные координаты:
    // y растёт ВНИЗ, поэтому s_scroll=0 (мы внизу) — бегунок внизу,
    // s_scroll=max (мы наверху) — бегунок вверху.
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

    // Строка ввода с горизонтальной прокруткой и мигающим курсором
    char prompt[CON_INPUT_LEN + 8];
    snprintf(prompt, sizeof prompt, "] %s", s_input);
    int cur = s_cur + 2;                     // позиция курсора в prompt
    int start = input_view_start(maxw);
    clip_line(prompt + start, maxw, buf, sizeof buf);
    const float wc[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    float inx = 10.0f;
    hud_text_draw_small(vw, vh, inx, y_in, buf, wc);

    // Курсор: ширина текста от начала видимой части до курсора. Прямоугольник
    // ровно по глифам: от ascent над базовой линией до descent под ней.
    if (start < cur) {
        float cw = 0;
        for (int i = start; i < cur; ) {
            int cp = 0;
            i += utf8_decode(prompt + i, &cp);
            cw += hud_cp_width_small(cp);
        }
        if ((SDL_GetTicks() / 500) % 2 == 0) {      // мигание
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

// --- Самопроверка (FR_TEST=x) --------------------------------------------------
void console_selftest(void)
{
    int fail = 0;

    // 1) команды и cvar'ы с клэмпом
    console_exec("set fov 90");
    if (cvar_get("fov") != 90.0f) { SDL_Log("console test: set FAIL"); fail++; }
    console_exec("set fov 999");
    if (cvar_get("fov") != 120.0f) { SDL_Log("console test: clamp hi FAIL"); fail++; }
    console_exec("set fov -7");
    if (cvar_get("fov") != 5.0f) { SDL_Log("console test: clamp lo FAIL"); fail++; }
    console_exec("set fov 60");

    // 2) ввод текста и Backspace через события SDL
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

    // 3) Tab-дополнение: "se" + Tab = "set" (заменяет слово до курсора)
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

    // 4) Enter выполняет команду
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

    // 5) exec <файл> — команды из файла
    {
        FILE *f = fopen("_fr_test.cfg", "w");
        if (f) {
            fprintf(f, "set fov 77\nset r_fog 0\n");
            fclose(f);
            console_exec("exec _fr_test.cfg");
            if (cvar_get("fov") != 77.0f || cvar_get("r_fog") != 0.0f) {
                SDL_Log("console test: exec FAIL");
                fail++;
            }
            remove("_fr_test.cfg");
            console_exec("set fov 60");
            console_exec("set r_fog 1");
        } else {
            SDL_Log("console test: exec (no tmp file) SKIP");
        }
    }

    // 6) кириллица и типографика: глифы в атласе, ввод UTF-8, Backspace
    if (!hud_cp_supported(0x410) || !hud_cp_supported(0x2014) ||
        !hud_cp_supported(0x2191)) {
        SDL_Log("console test: cyrillic/typography glyph FAIL");
        fail++;
    }
    s_inlen = 0; s_cur = 0; s_input[0] = 0;
    {
        static char ru[16] = "\xD0\xBF\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82"; // "привет"
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

    // 7) бинды: bind/unbind, +команды
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

    // 8) алиасы, toggle, комбинации ";"
    {
        console_exec("alias testme \"set fov 66; set r_fog 0\"");
        console_exec("testme");
        if (cvar_get("fov") != 66.0f || cvar_get("r_fog") != 0.0f) {
            SDL_Log("console test: alias FAIL");
            fail++;
        }
        console_exec("toggle r_fog");
        if (cvar_get("r_fog") != 1.0f) { SDL_Log("console test: toggle FAIL"); fail++; }
        console_exec("unalias testme");
        console_exec("set fov 60");
    }

    // 9) закрытие консоли
    console_toggle();
    if (console_active()) { SDL_Log("console test: close FAIL"); fail++; }

    // 10) version — номер сборки (buildnum.h генерируется Makefile)
    {
        console_exec("version");
        int n = 0;
        for (int i = 0; i < s_count && n == 0; i++)
            if (strstr(ring_at(i), "build #"))
                n = (int)strtol(strstr(ring_at(i), "build #") + 7, NULL, 10);
        if (n <= 0) { SDL_Log("console test: version/buildnum FAIL"); fail++; }
    }

    // 11) fx — пресеты эффектов (fx_init_console в main.c)
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

    SDL_Log("console selftest: %s", fail ? "FAIL" : "OK");
}
