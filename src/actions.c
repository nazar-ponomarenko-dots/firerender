// ============================================================================
//  actions.c — действия и их +/-команды (см. actions.h)
// ----------------------------------------------------------------------------
//  Здесь только состояние действий (g_act). Сами +/-команды регистрирует
//  ЦЕНТРАЛЬНЫЙ реестр (cmdreg.c) — обработчики ставят соответствующий флаг.
// ============================================================================
#include "actions.h"

#include "console.h"

#include <string.h>

bool g_act[A_COUNT];
static bool s_source[ACTION_SRC_COUNT][A_COUNT];

void action_set(ActionSource src, ActionId id, bool on)
{
    if (src < 0 || src >= ACTION_SRC_COUNT || id < 0 || id >= A_COUNT) return;
    s_source[src][id] = on;
    bool active = false;
    for (int i = 0; i < ACTION_SRC_COUNT; i++) active |= s_source[i][id];
    g_act[id] = active;
}

void action_clear_source(ActionSource src)
{
    if (src < 0 || src >= ACTION_SRC_COUNT) return;
    memset(s_source[src], 0, sizeof s_source[src]);
    for (int id = 0; id < A_COUNT; id++) {
        bool active = false;
        for (int i = 0; i < ACTION_SRC_COUNT; i++) active |= s_source[i][id];
        g_act[id] = active;
    }
}
