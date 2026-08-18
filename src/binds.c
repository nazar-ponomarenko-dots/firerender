// ============================================================================
//  binds.c — привязки клавиш (см. binds.h)
// ============================================================================
#include "binds.h"

#include "console.h"

#include <string.h>

static Bind s_binds[BINDS_MAX];
static int s_n = 0;

// Дефолтные бинды (как в HL): движение, прыжок, бег, присед, тумблеры
static const struct { SDL_Scancode sc; const char *cmd; } s_defaults[] = {
    { SDL_SCANCODE_W,           "+forward" },
    { SDL_SCANCODE_S,           "+back" },
    { SDL_SCANCODE_A,           "+left" },
    { SDL_SCANCODE_D,           "+right" },
    { SDL_SCANCODE_SPACE,       "+jump" },
    { SDL_SCANCODE_C,           "+duck" },
    { SDL_SCANCODE_E,           "+use" },
    { SDL_SCANCODE_LCTRL,       "+down" },
    { SDL_SCANCODE_LSHIFT,      "+speed" },
    { SDL_SCANCODE_F,           "noclip" },
    { SDL_SCANCODE_LEFTBRACKET, "toggle_shadows" },
    { SDL_SCANCODE_P,           "toggle_fog" },
    { SDL_SCANCODE_G,           "toggle_map" },
    { SDL_SCANCODE_H,           "toggle_debug" },
    { SDL_SCANCODE_Q,           "demo" },
    { SDL_SCANCODE_M,           "fx_sparks" },
    { SDL_SCANCODE_COMMA,       "fx_smoke" },
    { SDL_SCANCODE_PERIOD,      "fx_blood" },
    { SDL_SCANCODE_F5,          "save" },
    { SDL_SCANCODE_F9,          "load" },
    { SDL_SCANCODE_F12,         "screenshot" },
};

// Имя клавиши (в т.ч. виртуальные клавиши колеса)
const char *binds_key_name(SDL_Scancode sc)
{
    if (sc == BIND_WHEEL_UP) return "mouse_wheel_up";
    if (sc == BIND_WHEEL_DOWN) return "mouse_wheel_down";
    return SDL_GetScancodeName(sc);
}

// Имя -> scancode (в т.ч. виртуальные колеса)
SDL_Scancode binds_key_from_name(const char *name)
{
    if (strcmp(name, "mouse_wheel_up") == 0) return BIND_WHEEL_UP;
    if (strcmp(name, "mouse_wheel_down") == 0) return BIND_WHEEL_DOWN;
    return SDL_GetScancodeFromName(name);
}

const char *binds_cmd(SDL_Scancode sc)
{
    for (int i = 0; i < s_n; i++)
        if (s_binds[i].sc == sc) return s_binds[i].cmd;
    return NULL;
}

void binds_set(SDL_Scancode sc, const char *cmd)
{
    if (sc == SDL_SCANCODE_UNKNOWN || !cmd || !cmd[0]) return;
    for (int i = 0; i < s_n; i++) {
        if (s_binds[i].sc == sc) {
            snprintf(s_binds[i].cmd, sizeof s_binds[i].cmd, "%s", cmd);
            return;
        }
    }
    if (s_n < BINDS_MAX) {
        s_binds[s_n].sc = sc;
        snprintf(s_binds[s_n].cmd, sizeof s_binds[s_n].cmd, "%s", cmd);
        s_n++;
    }
}

void binds_clear(SDL_Scancode sc)
{
    for (int i = 0; i < s_n; i++) {
        if (s_binds[i].sc == sc) {
            s_binds[i] = s_binds[s_n - 1];
            s_n--;
            return;
        }
    }
}

void binds_clearall(void)
{
    s_n = 0;
}

SDL_Scancode binds_key_for_cmd(const char *cmd)
{
    for (int i = 0; i < s_n; i++)
        if (strcmp(s_binds[i].cmd, cmd) == 0) return s_binds[i].sc;
    return SDL_SCANCODE_UNKNOWN;
}

int binds_count(void)
{
    return s_n;
}

const Bind *binds_at(int i)
{
    return (i >= 0 && i < s_n) ? &s_binds[i] : NULL;
}

void binds_init(void)
{
    for (size_t i = 0; i < sizeof s_defaults / sizeof s_defaults[0]; i++)
        binds_set(s_defaults[i].sc, s_defaults[i].cmd);
}

void binds_reset(void)
{
    binds_clearall();
    binds_init();
}

// Выполняет команду (или комбинацию "cmd1; cmd2"). ';' — НЕ разделитель внутри
// кавычек (alias q "echo a; echo b" — единое тело, как в console_exec).
// При отпускании (down=0) исполняются ТОЛЬКО statement'ы с "+", причём как
// "-" (снимают действие); остальные пропускаются — иначе комбинированный бинд
// вида "noclip; +forward" при отпускании оставлял действие залипшим.
static void bind_exec(const char *cmd, int down)
{
    char buf[256];
    snprintf(buf, sizeof buf, "%s", cmd);
    char *p = buf;
    while (p && *p) {
        char *semi = NULL;
        int inq = 0;
        for (char *q = p; *q; q++) {
            if (*q == '"') inq = !inq;
            else if (*q == ';' && !inq) { semi = q; break; }
        }
        if (semi) *semi = 0;
        while (*p == ' ' || *p == '\t') p++;
        if (!down) {
            if (*p == '+') { *p = '-'; console_exec(p); }
        } else if (*p) {
            console_exec(p);
        }
        p = semi ? semi + 1 : NULL;
    }
}

void binds_key(SDL_Scancode sc, int down)
{
    const char *cmd = binds_cmd(sc);
    if (!cmd) return;
    bind_exec(cmd, down);
}

// Колесо мыши: импульсное действие (up=1 — вверх, 0 — вниз)
void binds_wheel(int up)
{
    binds_key(up ? BIND_WHEEL_UP : BIND_WHEEL_DOWN, 1);
}

// --- config.cfg ---------------------------------------------------------------
// Таблица «имя клавиши -> scancode». Имена SDL МОГУТ содержать пробелы
// ("Left Shift", "Keypad 1"), а команда — пробелы и ';' (alias-тела), поэтому
// строка "bind <клавиша> <команда>" разбирается жадным матчем имени из таблицы
// (отсортирована по длине: "Left Shift" раньше, чем "Left"). Команда = остаток
// строки после имени.
typedef struct { const char *name; SDL_Scancode sc; size_t len; } KeyName;
static KeyName s_keynames[SDL_SCANCODE_COUNT + 2];
static int s_keynames_n = 0;

static void keynames_init(void)
{
    if (s_keynames_n) return;
    for (int i = 0; i < SDL_SCANCODE_COUNT; i++) {
        const char *nm = SDL_GetScancodeName((SDL_Scancode)i);
        if (!nm || !nm[0]) continue;
        s_keynames[s_keynames_n].name = nm;
        s_keynames[s_keynames_n].sc = (SDL_Scancode)i;
        s_keynames[s_keynames_n].len = strlen(nm);
        s_keynames_n++;
    }
    s_keynames[s_keynames_n] = (KeyName){ "mouse_wheel_up", BIND_WHEEL_UP, 14 };
    s_keynames_n++;
    s_keynames[s_keynames_n] = (KeyName){ "mouse_wheel_down", BIND_WHEEL_DOWN, 16 };
    s_keynames_n++;
    // По длине — по убыванию: жадный матч на самое длинное имя
    for (int i = 1; i < s_keynames_n; i++)
        for (int j = i; j > 0 && s_keynames[j].len > s_keynames[j - 1].len; j--) {
            KeyName t = s_keynames[j];
            s_keynames[j] = s_keynames[j - 1];
            s_keynames[j - 1] = t;
        }
}

void binds_load_config(void)
{
    FILE *f = fopen("config.cfg", "r");
    if (!f) return;
    keynames_init();
    char line[256];
    int any = 0;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "bind ", 5) != 0) continue;
        char *nl = strpbrk(line, "\r\n");
        if (nl) *nl = 0;
        char *body = line + 5;
        while (*body == ' ' || *body == '\t') body++;

        SDL_Scancode sc = SDL_SCANCODE_UNKNOWN;
        char *cmd = NULL;
        for (int k = 0; k < s_keynames_n; k++) {
            const KeyName *kn = &s_keynames[k];
            if (strncmp(body, kn->name, kn->len) == 0 &&
                (body[kn->len] == ' ' || body[kn->len] == '\t' || body[kn->len] == 0)) {
                sc = kn->sc;
                cmd = body + kn->len;
                break;
            }
        }
        if (!cmd || sc == SDL_SCANCODE_UNKNOWN) continue;   // битое имя — игнор
        while (*cmd == ' ' || *cmd == '\t') cmd++;
        if (!cmd[0]) continue;
        if (!any) {
            any = 1;
            binds_clearall();      // пользовательские бинды — дефолты долой
        }
        // Дедупликация: последняя строка config.cfg побеждает — снимаем
        // эту же команду с прежних клавиш (напр. bind Q demo; bind X demo)
        SDL_Scancode olds[BINDS_MAX];
        int nold = 0;
        for (int i = 0; i < s_n; i++)
            if (s_binds[i].sc != sc && strcmp(s_binds[i].cmd, cmd) == 0 &&
                nold < BINDS_MAX)
                olds[nold++] = s_binds[i].sc;
        for (int i = 0; i < nold; i++) binds_clear(olds[i]);
        binds_set(sc, cmd);
    }
    fclose(f);
}

void binds_save_config(FILE *f)
{
    for (int i = 0; i < s_n; i++)
        fprintf(f, "bind %s %s\n", binds_key_name(s_binds[i].sc),
                s_binds[i].cmd);
}
