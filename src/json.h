// ============================================================================
//  json.h — dependency-free mini JSON parser (objects/arrays/strings/numbers)
// ----------------------------------------------------------------------------
//  Needed only for map/map.json: loaded once at startup, parsed into a node
//  pool that lives until json_free(). No libraries — pure C, like the whole
//  project. Limitations (more than enough for our data):
//    - nesting depth up to JSON_MAX_DEPTH (64);
//    - strings and numbers are stored as pointers into the arena (json_free releases it).
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
    const char *key;     // field name (for objects), NULL for array elements
    const char *sval;    // string (JSON_STRING), escapes already expanded
    double nval;         // number (JSON_NUMBER)
    int bval;            // JSON_BOOL
    int child;           // first child (array/object), -1 = none
    int next;            // next node at the same level, -1 = none
} JsonNode;

// Parses JSON text into a node pool. Returns the root node (or NULL on error).
// Individual nodes need no freeing — everything lives until json_free().
const JsonNode *json_parse(const char *text, size_t len);

// Frees the node and string pool of the previous json_parse.
void json_free(void);

// --- Node access -------------------------------------------------------------

// Finds a field by name in an object. NULL if absent or the node is not an object.
const JsonNode *json_find(const JsonNode *obj, const char *key);

// Array element by index (NULL if absent or the node is not an array).
const JsonNode *json_array_get(const JsonNode *arr, int idx);

// Array length (0 if the node is not an array).
int json_array_len(const JsonNode *arr);

// Number (def — if the node is missing or not a number).
double json_num(const JsonNode *n, double def);

// String (def — if the node is missing or not a string).
const char *json_str(const JsonNode *n, const char *def);

// Boolean (def — if the node is missing or not a boolean).
int json_bool(const JsonNode *n, int def);

// Iteration over object or array elements:
//   for (n = json_child_first(o); n; n = json_child_next(o, n)) ...
// For object elements the key is in n->key.
const JsonNode *json_child_first(const JsonNode *parent);
const JsonNode *json_child_next(const JsonNode *parent, const JsonNode *cur);

#endif
