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
    { SDL_SCANCODE_H,           "toggle_hud" },
    { SDL_SCANCODE_Q,           "demo" },
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

// Выполняет команду (или комбинацию "cmd1; cmd2"). При отпускании (down=0)
// команды с "+" выполняются как "-" (снимают действие).
static void bind_exec(const char *cmd, int down)
{
    char buf[256];
    snprintf(buf, sizeof buf, "%s", cmd);
    char *p = buf;
    while (p && *p) {
        char *semi = strchr(p, ';');
        if (semi) *semi = 0;
        while (*p == ' ' || *p == '\t') p++;
        if (!down && *p == '+') *p = '-';
        if (*p) console_exec(p);
        p = semi ? semi + 1 : NULL;
    }
}

void binds_key(SDL_Scancode sc, int down)
{
    const char *cmd = binds_cmd(sc);
    if (!cmd) return;
    if (down)
        bind_exec(cmd, 1);
    else if (cmd[0] == '+')
        bind_exec(cmd, 0);
}

// Колесо мыши: импульсное действие (up=1 — вверх, 0 — вниз)
void binds_wheel(int up)
{
    binds_key(up ? BIND_WHEEL_UP : BIND_WHEEL_DOWN, 1);
}

// --- config.cfg ---------------------------------------------------------------
void binds_load_config(void)
{
    FILE *f = fopen("config.cfg", "r");
    if (!f) return;
    char line[256];
    int any = 0;
    while (fgets(line, sizeof line, f)) {
        char key[64], cmd[128];
        if (sscanf(line, "bind %63s %127[^\r\n]", key, cmd) == 2) {
            if (!any) {
                any = 1;
                binds_clearall();      // пользовательские бинды — дефолты долой
            }
            SDL_Scancode sc = binds_key_from_name(key);
            if (sc == SDL_SCANCODE_UNKNOWN) continue;
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
    }
    fclose(f);
}

void binds_save_config(FILE *f)
{
    for (int i = 0; i < s_n; i++)
        fprintf(f, "bind %s %s\n", binds_key_name(s_binds[i].sc),
                s_binds[i].cmd);
}
