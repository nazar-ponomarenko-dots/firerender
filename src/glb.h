// ============================================================================
//  glb.h — загрузчик бинарного glTF 2.0 (.glb) для статичных мешей (prop_static)
// ----------------------------------------------------------------------------
//  Пользователь любит .glb, а не .obj: движок умеет грузить полноценный
//  binary glTF — иерархию nodes с трансформами (TRS/матрицы), меши с
//  примитивами (POSITION/NORMAL/TEXCOORD_0 + индексы), материалы и встроенные
//  текстуры (изодата bufferView'ов). Всё «запекается» в мировые координаты.
//
//  ПРАВИЛО РЕНДЕРА: наш движок — ПСЕВДО-PBR 2004 (как HL2/Source, Far Cry 1),
//  реального PBR (металл/шероховатость) у нас нет. Поэтому настоящие PBR-данные
//  из GLB НЕ воспроизводятся дословно, а ПЕРЕФОРМАТИРУЮТСЯ в наш материал
//  (material.h) + scalar-спекуляр:
//    baseColorFactor  -> tint;   alphaMode -> blend/alpha
//    roughnessFactor  -> specular (гладкость); metallic -> добавка к specular
//    emissiveFactor   -> прибавка к tint;  normalTexture -> (пока) игнор
//  Это осознанное допущение «в стиле 2004», а не погрешность.
// ============================================================================
#ifndef GLB_H
#define GLB_H
#include "gl_loader.h"
#include "scene.h"
#include "render_ctx.h"
#include "material.h"

// Один бакет геометрии, сгруппированный по материалу (уже в мировых
// координатах, индексированный). geometry::CPU, без GL — для парсинга и тестов.
typedef struct {
    float *verts, *normals, *uvs;    // 3 / 3 / 2 на вершину
    int vcount;                      // уникальных вершин
    unsigned int *indices;           // 3 * tri_count
    int index_count;                 // = 3*tri_count
    // Материал, переформатированный в ПСЕВДО-PBR 2004
    float tint[3], alpha;
    int blend;                       // MatBlend
    int cull;                        // MatCull
    float tile[2];                   // повтор UV
    float specular;                  // сила спекуляра 0..1
    // Альбедо-текстура (RGBA-пиксели, декодирована stb_image).
    unsigned char *img; int iw, ih;
    int wrap, filter;                // MatWrap / MatFilter
} GlbItem;

typedef struct {
    GlbItem *items;
    int count;
    char err[128];                   // текст ошибки (или пусто)
} GlbModel;

// Парсит .glb: читает файл, разбирает чанки, запекает иерархию в мировые
// координаты, группирует по материалам, декодирует альбедо. НЕ трогает GL —
// безопасно звать в селфтесте/headless. NULL при ошибке (даёт model->err).
// Освобождение — glb_model_free.
GlbModel *glb_parse_file(const char *path);

void glb_model_free(GlbModel *m);

// Создаёт узел GlbInstance3D: грузит .glb, строит GL-буферы и текстуры для
// каждого бакета, вешает узел с позицией pos и масштабом scale.
// Вернёт NULL при ошибке. Узел НЕ привязан к родителю (node_add).
SceneNode *glb_make_node(const char *path, const float pos[3], float scale,
                         const RendererContext *rc);

void glb_node_render(SceneNode *n, float dt);

#endif