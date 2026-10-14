// ============================================================================
//  events.h — event bus: subscribing to and dispatching events
// ----------------------------------------------------------------------------
//  Modules do not call each other directly, they post events: the console —
//  EV_CONSOLE_OPENED/CLOSED, entities — EV_ENTITY_SPAWN/KILL. The main loop
//  and other systems subscribe to the types they care about. This way Lua
//  (phase 4) can listen to the same events without touching the core.
// ============================================================================
#ifndef EVENTS_H
#define EVENTS_H

// Event types (extend as needed; a/b — event payload).
enum {
    EV_ENTITY_SPAWN = 1,   // a = Entity*
    EV_ENTITY_KILL,        // a = Entity*
    EV_CONSOLE_OPENED,     // a = NULL
    EV_CONSOLE_CLOSED,     // a = NULL
    EV_NODE_INIT,          // a = SceneNode*  (node added to the tree)
    EV_NODE_EXIT,          // a = SceneNode*  (node being removed from the tree)
};

typedef struct {
    int type;              // event type (from the enum above)
    const void *a;         // event payload (type-dependent)
    const void *b;
} Event;

typedef void (*EventListener)(const Event *e, void *user);

#define EVENTS_MAX_TYPES 32
#define EVENTS_MAX_LISTENERS 64

// Subscribe to an event type (user is passed to the callback). A repeated
// subscription with the same (fn, user) pair is ignored.
void events_listen(int type, EventListener fn, void *user);

// Unsubscribe.
void events_unlisten(int type, EventListener fn, void *user);

// Dispatch: all subscribers of the type are called in subscription order.
void events_emit(int type, const void *a, const void *b);

// Deferred dispatch: the event is queued and dispatched later from
// events_flush_deferred() at a safe point in the main loop. This allows
// emitting events safely from inside callbacks (reentrancy protection without
// manual snapshots): a listener that removes/adds subscriptions in this frame
// does not break the iteration. ONLY (type, a, b) is stored — no nested
// ownership (same as events_emit).
void events_emit_deferred(int type, const void *a, const void *b);

// Delivers deferred events in insertion order. Call once per frame.
void events_flush_deferred(void);

#endif
