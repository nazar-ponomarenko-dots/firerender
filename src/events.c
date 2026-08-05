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
