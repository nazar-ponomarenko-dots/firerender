// ============================================================================
//  mesh.h — статичные меши (prop_static): загрузка .obj и рендер-узел
// ----------------------------------------------------------------------------
//  Фаза 4: движок умеет грузить 3D-геометрию из Wavefront .obj и ставить её
//  как узел MeshInstance3D в сцене. В отличие от биллбордов дерева меш
//  освещается ПО-НАСТОЯЩЕМУ: вершинные нормали, Ламбертов диффуз, спекуляр,
//  каскадные тени, туман (shaders/mesh.vs + mesh.fs).
//
//  Загрузчик: v/vt/vn + треугольники и четырёхугольники (фан-триангуляция).
//  Вершины РАСШИРЯЮТСЯ по корнеру (каждая грань — свои вертексы), поэтому
//  индексы тривиальны: рисуем glDrawArrays(GL_TRIANGLES). Упрощение оправдано
//  для статичных декораций — ценой памяти, но зато без сложного индексирования.
// ============================================================================
#ifndef MESH_H
#define MESH_H
#include "gl_loader.h"
#include "scene.h"
#include "render_ctx.h"
#include "material.h"

typedef struct {
    float *verts;      // 3 * vcount  (позиции, расширены по корнеру)
    float *normals;    // 3 * vcount  (или NULL, если в OBJ нет vn)
    float *uvs;        // 2 * vcount  (или NULL, если в OBJ нет vt)
    int vcount;        // всего вертексов = tri_count * 3
    int tri_count;     // число треугольников
    // GL-ресурсы (создаются при первом рендере)
    GLuint vao, vbo_pos, vbo_nrm, vbo_uv;
    int ready;         // 1 — геометрия загружена в GL
} Mesh;

// Загружает .obj через resources_read_text. Вернёт NULL при ошибке.
// Освобождение — mesh_free.
Mesh *mesh_load_obj(const char *path);

// Загружает .obj из буфера в памяти (для тестов/селфтеста, без файла).
// name — имя для логов.
Mesh *mesh_load_obj_mem(const char *name, const char *src);

// Освобождает CPU-данные и GL-объекты меша.
void mesh_free(Mesh *m);

// Гарантирует загрузку геометрии меша в GL (идемпотентно). Вызывается в render.
void mesh_gpu_upload(Mesh *m);

// Параметры узла-инстанса меша.
typedef struct {
    const RendererContext *rc;
    Mesh *mesh;                  // общая геометрия (узел НЕ владеет — не free)
    GLuint prog;                 // общий шейдер mesh.vs+mesh.fs
    SceneUniforms su;            // общий набор сцены
    Material mat;                // материал (tint/blend/UV, material.h)
    MaterialLocs ml;             // его uniform-хендлы
    GLint u_model, u_tex;        // своя модельная матрица и текстура
    GLuint tex;                  // альбедо-текстура
    float scale;                 // равномерный масштаб
} MeshLocs;

// Создаёт узел MeshInstance3D: грузит .obj, берёт текстуру tex_path
// (или img/notfound.png) и вешает узел с позицией pos и масштабом scale.
// Вернёт NULL при ошибке загрузки. Узел НЕ привязан к родителю (node_add).
SceneNode *mesh_make_node(const char *obj_path, const float pos[3],
                          float scale, const char *tex_path,
                          const RendererContext *rc);

void mesh_node_render(SceneNode *n, float dt);

#endif