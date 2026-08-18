// ============================================================================
//  entity.h — сущности поверх дерева сцены (как энтити в GoldSrc)
// ----------------------------------------------------------------------------
//  Сущность = узел сцены + класс + данные экземпляра. Класс задаёт события:
//    spawn  — сразу после создания (после EV_ENTITY_SPAWN);
//    think  — каждый кадр из главного цикла;
//    kill   — перед удалением (и EV_ENTITY_KILL).
//  entity_spawn создаёт новый узел Node3D в корне сцены, entity_bind
//  привязывает сущность к УЖЕ СУЩЕСТВУЮЩЕМУ узлу (камера, мир).
//  Это фундамент для Lua (фаза 4): скриптовые классы будут регистрировать
//  те же события, что и нативные.
// ============================================================================
#ifndef ENTITY_H
#define ENTITY_H
#include "scene.h"

typedef struct Entity Entity;
typedef void (*EntityThinkFn)(Entity *e, float dt);

typedef struct {
    const char *classname;       // имя класса (ключ регистрации)
    void (*spawn)(Entity *e);    // после создания (может быть NULL)
    EntityThinkFn think;         // каждый кадр (может быть NULL)
    void (*kill)(Entity *e);     // перед удалением (может быть NULL)
} EntityClass;

struct Entity {
    const char *classname;       // имя класса (строка из реестра)
    const EntityClass *cls;      // класс
    SceneNode *node;             // узел сцены (pos/size — его)
    void *data;                  // данные экземпляра (владеет класс)
    int owns_node;               // 1 = узел создан entity_spawn (внутреннее)
};

// Куда цепляются новые узлы сущностей (корень сцены World). Вызывать
// до entity_spawn.
void entity_set_root(SceneNode *root);

// Регистрация класса. Повторная регистрация имени игнорируется.
void entity_register_class(const EntityClass *cls);

// Создаёт сущность: новый узел Node3D (имя = classname) в корне сцены.
// pos/size — начальные позиция/размер узла (NULL — нули).
// Шлёт EV_ENTITY_SPAWN и вызывает cls->spawn. NULL — класс не найден
// или список полон.
Entity *entity_spawn(const char *classname, const float *pos,
                     const float *size, void *data);

// Привязывает сущность к существующему узлу (новый узел не создаётся,
// при удалении узел НЕ освобождается — он принадлежит своему модулю).
Entity *entity_bind(const char *classname, SceneNode *node, void *data);

// --- Хендлы сущностей --------------------------------------------------------
// Сущности живут в СТАБИЛЬНЫХ слотах (entindex = номер слота не меняется во
// время жизни) с номером серии serial. Хендл кодирует (serial, entindex) и
// гарантирует, что обращение к убитой/переиспользованной сущности вернёт NULL,
// а не висячий указатель. Внутренний код держит прямой указатель (валиден,
// пока сущность жива и слот не освобожден), а через границы Lua использовать
// только хендл.
int  entity_handle(Entity *e);   // 0 = некорректный/мёртвый указатель
Entity *entity_get(int handle);  // NULL = сущность нет/убита/переиспользована

// Удаление: EV_ENTITY_KILL, cls->kill, освобождение узла (если создан
// entity_spawn) и слот в списке.
void entity_kill(Entity *e);

// think всех сущностей (каждый кадр из главного цикла).
void entity_update_all(float dt);

// Убивает все сущности и очищает список (при выходе).
void entity_shutdown(void);

// Доступ к списку (для отладки и будущего редактора).
int entity_count(void);
Entity *entity_at(int i);

// n-я (с нуля) сущность данного класса, NULL — такой нет.
// Пригодится Lua: найти все "trigger_x" и т.п.
Entity *entity_find_class(const char *classname, int n);

#endif
