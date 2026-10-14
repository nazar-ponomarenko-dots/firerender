// ============================================================================
//  actions.h — active actions (movement, etc.)
// ----------------------------------------------------------------------------
//  HL-style movement goes through "+commands": a key is bound to "+forward",
//  pressing runs "+forward" (flag=1), releasing runs "-forward" (flag=0).
//  The camera reads these flags instead of raw keys — which is why any key
//  can be rebound through the binds.
// ============================================================================
#ifndef ACTIONS_H
#define ACTIONS_H
#include <stdbool.h>

typedef enum {
    A_FORWARD = 0,   // +forward  — move forward
    A_BACK,          // +back     — move back
    A_LEFT,          // +left     — strafe left
    A_RIGHT,         // +right    — strafe right
    A_JUMP,          // +jump     — jump (walk) / up (fly)
    A_DOWN,          // +down     — down (fly)
    A_SPEED,         // +speed    — run (Shift)
    A_DUCK,          // +duck     — crouch (C)
    A_USE,           // +use      — interact (E)
    A_COUNT
} ActionId;

typedef enum {
    ACTION_SRC_BIND = 0,
    ACTION_SRC_TOUCH,
    ACTION_SRC_GAMEPAD,
    ACTION_SRC_COUNT
} ActionSource;

// The final state is the OR of all sources. This keeps a released touch
// finger from clearing an action that the keyboard or gamepad still holds.
extern bool g_act[A_COUNT];
void action_set(ActionSource src, ActionId id, bool on);
void action_clear_source(ActionSource src);

#endif
