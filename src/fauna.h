// ============================================================================
//  fauna.h — живая карта: светлячки, бабочки, птицы (см. fauna.c)
// ============================================================================
#ifndef FAUNA_H
#define FAUNA_H
#include "gl_loader.h"
#include "scene.h"
#include "camera.h"

// Создаёт узел фауны из map.json (fauna[]). NULL, если в карте нет фауны.
SceneNode *fauna_make_node(CameraParams *cam);

void fauna_node_update(SceneNode *n, float dt);
void fauna_node_render(SceneNode *n, float dt);

// Освобождает GL-ресурсы узла.
void fauna_cleanup(SceneNode *n);

#endif
