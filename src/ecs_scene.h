// ============================================================================
//  ecs_scene.h — мост дерева сцены и ECS (см. ecs.h)
// ----------------------------------------------------------------------------
//  Узел сцены получает парную ECS-сущность: компонент NodeRef (указатель на
//  узел + узел-родитель -> ECS-родитель + поколение дерева) и компонент
//  Transform (pos/rot/scl + биты modified). За тик моста:
//  pull (узлы -> ECS, сброс modified) -> ecs_update (системы двигают
//  трансформы и помечают modified) -> push (меченые ECS -> узлы). Вызывать
//  ПОСЛЕ node_update_tree и ДО node_update_transforms, чтобы мировые матрицы
//  для рендера считались уже от финальных позиций.
//
//  Устаревание: при смене карты узлы умирают вместе с деревом. Мост ведёт
//  счётчик поколений (gen): перед прогоном gen инкрементируется, компоненты
//  с чужим gen пересоздаются, «сироты» (узел умер, сущность жива) убиваются.
//  Висячих указателей нет: gen сверяется до разыменования node.
//
//  Чистые ECS-сущности (без NodeRef) мост не трогает — живут своей жизнью.
// ============================================================================
#ifndef ECS_SCENE_H
#define ECS_SCENE_H

#include "scene.h"
#include "ecs.h"
#include <stdint.h>

typedef struct { SceneNode *node; EcsId parent_id; int gen; } EcsNodeRef;

#define ECS_TF_POS  1u
#define ECS_TF_ROT  2u
#define ECS_TF_SCL  4u
typedef struct {
    float pos[3], rot[3], scl[3];
    uint32_t modified;                 // биты ECS_TF_* — ставить из систем
} EcsTransform;

// Поднимает мост: регистрация компонентов. Идемпотентно.
void ecs_scene_init(void);

// Рубильник (cvar ecs_enable). Выключенный мост не делает НИЧЕГО в кадре.
void ecs_scene_enable(int on);
int  ecs_scene_enabled(void);

// Новый generation (перед первым прогоном на новой карте).
void ecs_scene_new_generation(void);

// Один тик моста: pull -> ecs_update -> push. root — корень сцены (g->root).
void ecs_scene_frame(SceneNode *root, float dt);

// Убить все синхронизированные сущности (выгрузка карты/выход).
void ecs_scene_shutdown(void);

// Компонентные id моста (0 — не инициализирован).
int ecs_comp_noderef(void);
int ecs_comp_transform(void);

// Маска «узел-сущность»: NodeRef & Transform — для ecs_register_system.
uint64_t ecs_scene_mask(void);

// Узел -> его ECS-сущность, создав пару при отсутствии; -1 — мост мёртв.
EcsId ecs_scene_ensure(SceneNode *n);

// ECS-сущность узла без создания; -1 — не синхронизирован.
EcsId ecs_scene_id_of(const SceneNode *n);

// Трансформ сущности моста; NULL — компонента нет.
EcsTransform *ecs_scene_transform(EcsId id);

// Пометить, что система изменила поля (флаги ECS_TF_*).
void ecs_scene_mark(EcsId id, uint32_t flags);

// Проверить связность моста на живом дереве: каждый узел имеет пару,
// трансформы совпадают, сирот нет. mismatch==0 — связность ок.
void ecs_scene_verify(SceneNode *root, int *nodes, int *synced, int *mismatch);

// Однократная верификация на следующем тике (FR_TEST 'V').
void ecs_scene_verify_once(void);

// Статистика для ecs_list/дебаг-панели.
int  ecs_scene_synced(void);
int  ecs_scene_generation(void);

// Консольная команда (регистрация — в cmdreg.c).
void cmd_ecs_verify(const char *args);

#endif
