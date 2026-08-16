// ============================================================================
//  events.h — событийная шина: подписка и рассылка событий
// ----------------------------------------------------------------------------
//  Модули не вызывают друг друга напрямую, а шлют события: консоль —
//  EV_CONSOLE_OPENED/CLOSED, сущности — EV_ENTITY_SPAWN/KILL. Главный цикл
//  и другие системы подписываются на интересные им типы. Так Lua (фаза 4)
//  сможет слушать те же события, не трогая ядро.
// ============================================================================
#ifndef EVENTS_H
#define EVENTS_H

// Типы событий (расширяйте по мере надобности; a/b — данные события).
enum {
    EV_ENTITY_SPAWN = 1,   // a = Entity*
    EV_ENTITY_KILL,        // a = Entity*
    EV_CONSOLE_OPENED,     // a = NULL
    EV_CONSOLE_CLOSED,     // a = NULL
    EV_NODE_INIT,          // a = SceneNode*  (узел добавлен в дерево)
    EV_NODE_EXIT,          // a = SceneNode*  (узел удаляется из дерева)
};

typedef struct {
    int type;              // тип события (из перечисления выше)
    const void *a;         // данные события (тип зависит от type)
    const void *b;
} Event;

typedef void (*EventListener)(const Event *e, void *user);

#define EVENTS_MAX_TYPES 32
#define EVENTS_MAX_LISTENERS 64

// Подписка на тип события (user передаётся в колбэк). Повторная подписка
// с той же парой (fn, user) игнорируется.
void events_listen(int type, EventListener fn, void *user);

// Отписка.
void events_unlisten(int type, EventListener fn, void *user);

// Рассылка: все подписчики типа вызываются в порядке подписки.
void events_emit(int type, const void *a, const void *b);

#endif
