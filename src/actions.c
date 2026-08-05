// ============================================================================
//  actions.c — действия и их +/-команды (см. actions.h)
// ============================================================================
#include "actions.h"

#include "console.h"

bool g_act[A_COUNT];

// Функции действий: "+forward" ставит флаг, "-forward" снимает
#define DEF_ACTION(id, name)                                             \
    static void a_##name##_on(const char *a) { (void)a; g_act[id] = 1; } \
    static void a_##name##_off(const char *a) { (void)a; g_act[id] = 0; }

DEF_ACTION(A_FORWARD, forward)
DEF_ACTION(A_BACK,    back)
DEF_ACTION(A_LEFT,    left)
DEF_ACTION(A_RIGHT,   right)
DEF_ACTION(A_JUMP,    jump)
DEF_ACTION(A_DOWN,    down)
DEF_ACTION(A_SPEED,   speed)
DEF_ACTION(A_DUCK,    duck)
DEF_ACTION(A_USE,     use)

#define REG_ACTION(name, desc)                                              \
    console_add_command("+" #name, desc, a_##name##_on);                   \
    console_add_command("-" #name, "", a_##name##_off)

void actions_register_commands(void)
{
    REG_ACTION(forward, "движение вперёд");
    REG_ACTION(back,    "движение назад");
    REG_ACTION(left,    "движение влево");
    REG_ACTION(right,   "движение вправо");
    REG_ACTION(jump,    "прыжок / вверх в полёте");
    REG_ACTION(down,    "вниз в полёте");
    REG_ACTION(speed,   "бег (ускорение)");
    REG_ACTION(duck,    "присесть");
    REG_ACTION(use,     "взаимодействие (E)");
}
