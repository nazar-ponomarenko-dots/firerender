// ============================================================================
//  events.c — реализация событийной шины (см. events.h)
// ============================================================================
#include "events.h"

#include <string.h>

static struct {
    EventListener fn;
    void *user;
} s_listeners[EVENTS_MAX_TYPES][EVENTS_MAX_LISTENERS];
static int s_count[EVENTS_MAX_TYPES];

void events_listen(int type, EventListener fn, void *user)
{
    if (type < 0 || type >= EVENTS_MAX_TYPES || !fn) return;
    for (int i = 0; i < s_count[type]; i++)
        if (s_listeners[type][i].fn == fn && s_listeners[type][i].user == user)
            return;                          // уже подписан
    if (s_count[type] < EVENTS_MAX_LISTENERS) {
        s_listeners[type][s_count[type]].fn = fn;
        s_listeners[type][s_count[type]].user = user;
        s_count[type]++;
    }
}

void events_unlisten(int type, EventListener fn, void *user)
{
    if (type < 0 || type >= EVENTS_MAX_TYPES) return;
    for (int i = 0; i < s_count[type]; i++) {
        if (s_listeners[type][i].fn != fn) continue;
        if (s_listeners[type][i].user != user) continue;
        for (int j = i; j < s_count[type] - 1; j++)
            s_listeners[type][j] = s_listeners[type][j + 1];
        s_count[type]--;
        return;
    }
}

void events_emit(int type, const void *a, const void *b)
{
    if (type < 0 || type >= EVENTS_MAX_TYPES) return;
    Event e = { type, a, b };
    for (int i = 0; i < s_count[type]; i++)
        s_listeners[type][i].fn(&e, s_listeners[type][i].user);
}

// --- Отложенная рассылка (ring буфер) ----------------------------------------
#define EVENTS_DEFER_MAX 256
static Event s_deferred[EVENTS_DEFER_MAX];
static int s_defer_head = 0;
static int s_defer_tail = 0;      // [head, tail) — занять, кольцо

void events_emit_deferred(int type, const void *a, const void *b)
{
    if (type < 0 || type >= EVENTS_MAX_TYPES) return;
    int n = (s_defer_tail - s_defer_head + EVENTS_DEFER_MAX) % EVENTS_DEFER_MAX;
    if (n >= EVENTS_DEFER_MAX - 1) return;       // переполнено — молча теряем
    s_deferred[s_defer_tail].type = type;
    s_deferred[s_defer_tail].a = a;
    s_deferred[s_defer_tail].b = b;
    s_defer_tail = (s_defer_tail + 1) % EVENTS_DEFER_MAX;
}

void events_flush_deferred(void)
{
    if (s_defer_head == s_defer_tail) return;
    // Снимок: события, добавленные во время доставки, останутся на след. flush.
    int end = s_defer_tail;
    while (s_defer_head != end) {
        Event *ev = &s_deferred[s_defer_head];
        s_defer_head = (s_defer_head + 1) % EVENTS_DEFER_MAX;
        events_emit(ev->type, ev->a, ev->b);
    }
}
