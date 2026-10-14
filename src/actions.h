// ============================================================================
//  actions.h — активные действия (движение и т.п.)
// ----------------------------------------------------------------------------
//  Движение в HL идёт через "+команды": клавиша привязана к "+forward",
//  при нажатии выполняется "+forward" (флаг=1), при отпускании "-forward"
//  (флаг=0). Камера читает эти флаги, а не клавиши напрямую — поэтому
//  любую клавишу можно переназначить через бинды.
// ============================================================================
#ifndef ACTIONS_H
#define ACTIONS_H
#include <stdbool.h>

typedef enum {
    A_FORWARD = 0,   // +forward  — вперёд
    A_BACK,          // +back     — назад
    A_LEFT,          // +left     — влево
    A_RIGHT,         // +right    — вправо
    A_JUMP,          // +jump     — прыжок (ходьба) / вверх (полёт)
    A_DOWN,          // +down     — вниз (полёт)
    A_SPEED,         // +speed    — бег (Shift)
    A_DUCK,          // +duck     — присесть (C)
    A_USE,           // +use      — взаимодействие (E)
    A_COUNT
} ActionId;

typedef enum {
    ACTION_SRC_BIND = 0,
    ACTION_SRC_TOUCH,
    ACTION_SRC_GAMEPAD,
    ACTION_SRC_COUNT
} ActionSource;

// Итоговое состояние — OR всех источников. Это не даёт отпусканию пальца
// погасить действие, которое всё ещё удерживает клавиатура или геймпад.
extern bool g_act[A_COUNT];
void action_set(ActionSource src, ActionId id, bool on);
void action_clear_source(ActionSource src);

#endif
