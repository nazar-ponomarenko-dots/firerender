// ============================================================================
//  json.h — мини-парсер JSON без зависимостей (объекты/массивы/строки/числа)
// ----------------------------------------------------------------------------
//  Нужен только для map/map.json: грузим один раз при старте, парсим в пул
//  узлов, который живёт до json_free(). Никаких библиотек — чистый C, как
//  и весь проект. Ограничения (для наших данных их хватает с запасом):
//    - глубина вложенности до JSON_MAX_DEPTH (64);
//    - строки и числа хранятся как указатели на арену (json_free освобождает).
// ============================================================================
#ifndef JSON_H
#define JSON_H

#include <stddef.h>

typedef enum {
    JSON_NULL, JSON_BOOL, JSON_NUMBER, JSON_STRING,
    JSON_ARRAY, JSON_OBJECT
} JsonType;

typedef struct JsonNode {
    JsonType type;
    const char *key;     // имя поля (для объектов), NULL — у элементов массива
    const char *sval;    // строка (JSON_STRING), уже с раскрытыми escape
    double nval;         // число (JSON_NUMBER)
    int bval;            // JSON_BOOL
    int child;           // первый ребёнок (массив/объект), -1 = нет
    int next;            // следующий узел на том же уровне, -1 = нет
} JsonNode;

// Парсит JSON-текст в пул узлов. Возвращает корневой узел (или NULL при
// ошибке). Освобождать отдельные узлы не нужно — всё живёт до json_free().
const JsonNode *json_parse(const char *text, size_t len);

// Освобождает пул узлов и строк предыдущего json_parse.
void json_free(void);

// --- Доступ к узлам -----------------------------------------------------------

// Ищет поле по имени в объекте. NULL, если нет или узел не объект.
const JsonNode *json_find(const JsonNode *obj, const char *key);

// Элемент массива по индексу (NULL, если нет или узел не массив).
const JsonNode *json_array_get(const JsonNode *arr, int idx);

// Длина массива (0, если узел не массив).
int json_array_len(const JsonNode *arr);

// Число (def — если узла нет или он не число).
double json_num(const JsonNode *n, double def);

// Строка (def — если узла нет или он не строка).
const char *json_str(const JsonNode *n, const char *def);

// Булево (def — если узла нет или он не булев).
int json_bool(const JsonNode *n, int def);

// Перебор элементов объекта или массива:
//   for (n = json_child_first(o); n; n = json_child_next(o, n)) ...
// У элементов объекта ключ лежит в n->key.
const JsonNode *json_child_first(const JsonNode *parent);
const JsonNode *json_child_next(const JsonNode *parent, const JsonNode *cur);

#endif
