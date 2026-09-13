// ============================================================================
//  objects.c — объекты из map.json: биллборды (деревья и пр.)
// ----------------------------------------------------------------------------
//  Читает objects[] из map.json: каждый объект — квад(ы) с текстурой
//  (model — путь к PNG, напр. "img/tree.png"), позицией pos и масштабом
//  scale. По умолчанию — КРЕСТ из двух статичных квадов; billboard=1 —
//  один квад всегда лицом к камере. Высота квада = 5 м * scale.y
//  (OBJECT_BASE_H), ширина — из пропорций текстуры. База ложится на рельеф
//  (если y <= 0). Шейдер: встроенный default/billboard/flat или СВОЙ
//  (vs/fs из карты) — uniform'ы те же, что у object.vs/object.fs
//  (обязательные uTex/uTint/uObjPos/uObjSize/uObjRight/uViewProj, остальные
//  можно не объявлять). collision=1 — объект непроходим (цилиндр radius
//  вокруг базы, objects_collide выталкивает игрока).
//  Освещение — как у растительности: все направленные источники из
//  map.json + CSM-тени солнца + туман.
// ============================================================================
#include "objects.h"

#include "assets.h"
#include "bloom.h"
#include "cvars.h"
#include "gl_loader.h"
#include "linalg.h"
#include "mapdata.h"
#include "material.h"
#include "resources.h"
#include "shader.h"
#include "shadow.h"
#include "world.h"

#include "stb_image.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#define OBJECT_MAX 256
#define OBJECT_BASE_H 9.0f     // высота квада при scale.y = 1

// Uniform-локации шейдера объекта (общие для всех программ):
// общий набор сцены (SceneUniforms) + геометрия объекта + материал.
typedef struct {
    SceneUniforms su;            // общий набор сцены (render_ctx.h)
    GLint u_objpos, u_objsize, u_objright, u_objslope, u_tex;
    MaterialLocs mat;            // материал (material.h)
} ObjLocs;

typedef struct {
    GLuint tex;
    GLuint tex_custom;       // своя текстура (кастомные wrap/filter)
    GLuint prog;             // свой шейдер (0 = общий p->prog)
    ObjLocs locs;            // локации для prog
    int billboard;           // квад к камере (вместо креста)
    int src;                 // индекс в map.json (objects[]): инстансы идут
                             // плотнее mapdata (пустые model пропускаются),
                             // поэтому связь только через src
    float pos[3];
    float slope[2];          // уклон земли под объектом (dh/dx, dh/dz):
                             // низ креста ложится на рельеф, как у травы
    float width, height;
    int collision;           // цилиндр непроходимости
    float radius;            // радиус коллизии, м
    Material r;              // материал (material.h)
} ObjectInst;

typedef struct {
    const WorldState *world; // данные мира: высоты (посадка на рельеф)
    const RendererContext *rc;   // кадровые данные (renderer.c)
    GLuint prog, vao, vbo, ibo;
    GLuint flat_prog;        // встроенный "flat" (без освещения)
    GLuint shadow_prog;      // глубина в карты теней
    GLint s_viewproj, s_objpos, s_objsize, s_objright, s_objslope;
    // Blob-тени (режим r_shadows_mode 3): мягкое пятно под объектом
    GLuint blob_prog, blob_tex;
    GLint b_viewproj, b_objpos, b_objsize, b_objright, b_objfwd, b_tex, b_color;
    int count;
    ObjectInst inst[OBJECT_MAX];
    ObjLocs locs;            // локации общих программ (p->prog)
    ObjLocs flat_locs;
} ObjectsParams;

// Цилиндры коллизий объектов (для camera.c — выталкивание игрока)
static struct { float x, z, r; } s_col[OBJECT_MAX];
static int s_col_n = 0;
// Квадраты коллизий из collisions[] (XZ-прямоугольники)
static struct { float x, z, hw, hz; } s_box[OBJECT_MAX];
static int s_box_n = 0;
static ObjectsParams *s_objs = NULL;   // для hot-reload (shader_on_reshade)
// Время (для анимации UV)
static float s_time = 0.0f;

// Заполняет локации uniform'ов программы (необязательные = -1)
static ObjLocs obj_locs(GLenum prog)
{
    ObjLocs l;
    memset(&l, 0, sizeof l);
    scene_uniforms_init(&l.su, prog);      // общий набор сцены
    material_locs_init(&l.mat, prog);      // материал (tint/blend/UV)
    l.u_objpos   = glGetUniformLocation(prog, "uObjPos");
    l.u_objsize  = glGetUniformLocation(prog, "uObjSize");
    l.u_objright = glGetUniformLocation(prog, "uObjRight");
    l.u_objslope = glGetUniformLocation(prog, "uObjSlope");
    l.u_tex      = glGetUniformLocation(prog, "uTex");
    return l;
}

// Пересоздаёт программы объектов и переищет uniform'ы (для hot-reload).
// replace балансирует refcount (release старой + новая ссылка); если пара
// не собралась — возвращает 0 и остаётся прежний id. Пересоздаём ВСЕ
// программы узла (включая flat/shadow/blob и custom каждого инстанса),
// иначе после reload часть объектов рисовала бы старым шейдером, а его
// ссылка утекала навсегда (старый код даже не перезапрашивал flat/shadow).
static void objects_reshade(void)
{
    ObjectsParams *p = s_objs;
    if (!p) return;
    GLuint prog = resources_program_replace(p->prog,
                                            "shaders/object.vs",
                                            "shaders/object.fs");
    if (prog) p->prog = prog;
    p->locs = obj_locs(p->prog);
    if (p->flat_prog) {
        GLuint f = resources_program_replace(p->flat_prog,
                                             "shaders/object.vs",
                                             "shaders/object_flat.fs");
        if (f) p->flat_prog = f;
        p->flat_locs = obj_locs(p->flat_prog);
    }
    {
        GLuint sp = resources_program_replace(p->shadow_prog,
                                              "shaders/object_shadow.vs",
                                              "shaders/object_shadow.fs");
        if (sp) p->shadow_prog = sp;
    }
    if (p->blob_prog) {
        GLuint bp = resources_program_replace(p->blob_prog,
                                              "shaders/blob.vs",
                                              "shaders/blob.fs");
        if (bp) p->blob_prog = bp;
    }
    glUseProgram(p->prog);
    glUniform1i(p->locs.u_tex, 0);
    for (int c = 0; c < SHADOW_CASCADES; c++) {
        if (p->locs.su.u_shadowmap[c] >= 0)
            glUniform1i(p->locs.su.u_shadowmap[c], 3 + c);
        if (p->locs.su.u_shadowdepth[c] >= 0)
            glUniform1i(p->locs.su.u_shadowdepth[c], 3 + c);
    }
    glUseProgram(0);
    for (int i = 0; i < p->count; i++) {
        ObjectInst *it = &p->inst[i];
        if (!it->prog) continue;
        const MapObject *o = mapdata_object(it->src);
        if (o && o->shader_kind == OBJ_SHADER_CUSTOM) {
            const char *vs = o->shader_vs[0] ? o->shader_vs
                                             : "shaders/object.vs";
            GLuint cp = resources_program_replace(it->prog, vs, o->shader_fs);
            if (cp) it->prog = cp;
        }
        it->locs = obj_locs(it->prog);
        glUseProgram(it->prog);
        if (it->locs.u_tex >= 0) glUniform1i(it->locs.u_tex, 0);
        for (int c = 0; c < SHADOW_CASCADES; c++) {
            if (it->locs.su.u_shadowmap[c] >= 0)
                glUniform1i(it->locs.su.u_shadowmap[c], 3 + c);
            if (it->locs.su.u_shadowdepth[c] >= 0)
                glUniform1i(it->locs.su.u_shadowdepth[c], 3 + c);
        }
        glUseProgram(0);
    }
    p->s_viewproj = glGetUniformLocation(p->shadow_prog, "uViewProj");
    p->s_objpos   = glGetUniformLocation(p->shadow_prog, "uObjPos");
    p->s_objsize  = glGetUniformLocation(p->shadow_prog, "uObjSize");
    p->s_objright = glGetUniformLocation(p->shadow_prog, "uObjRight");
    p->s_objslope = glGetUniformLocation(p->shadow_prog, "uObjSlope");
}

// Кадровые uniform'ы (виды, свет, тени, туман) — при переключении программы
static void set_per_frame(const ObjLocs *l, const ObjectsParams *p)
{
    const RendererContext *rc = p->rc;
    // Общий набор: виды, свет, окружение, туман, каскады теней (из rc)
    scene_uniforms_bind(&l->su, rc);
    // Объекты: bias как у растительности (shadow_bias_veg из map.json),
    // градиент ~5 м вглубь тени
    for (int c = 0; c < SHADOW_CASCADES; c++) {
        if (l->su.u_shadowbias[c] >= 0)
            glUniform1f(l->su.u_shadowbias[c], rc->shadow_bias_veg[c]);
        if (l->su.u_shadowgrad[c] >= 0)
            glUniform1f(l->su.u_shadowgrad[c], 5.0f / rc->shadow_depth[c]);
    }
    // Материал (tint/blend/UV-анимация) применяет material_bind на инстанс
}

// Копия текстуры с другими параметрами (wrap/filter) — кеш движка не трогаем
static GLuint texture_custom(const unsigned char *img, int w, int h,
                             int wrap, int filter)
{
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    GLint wmode = GL_CLAMP_TO_EDGE, fmag = GL_LINEAR, fmin = GL_LINEAR;
    if (wrap == MAT_WRAP_REPEAT) wmode = GL_REPEAT;
    else if (wrap == MAT_WRAP_MIRROR) wmode = GL_MIRRORED_REPEAT;
    if (filter == MAT_FILTER_NEAREST) fmag = fmin = GL_NEAREST;
    else if (filter == MAT_FILTER_MIPMAP) fmin = GL_LINEAR_MIPMAP_LINEAR;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wmode);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wmode);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, fmin);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, fmag);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, img);
    if (filter == MAT_FILTER_MIPMAP) glGenerateMipmap(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
}

static void objects_params_release(ObjectsParams *p)
{
    if (!p) return;
    if (s_objs == p) s_objs = NULL;
    s_col_n = 0;
    s_box_n = 0;
    for (int i = 0; i < p->count; i++) {
        if (p->inst[i].tex_custom)
            glDeleteTextures(1, &p->inst[i].tex_custom);
        else if (p->inst[i].tex)
            resources_texture_release(p->inst[i].tex);
        if (p->inst[i].prog) resources_program_release(p->inst[i].prog);
    }
    if (p->prog) resources_program_release(p->prog);
    if (p->flat_prog) resources_program_release(p->flat_prog);
    if (p->shadow_prog) resources_program_release(p->shadow_prog);
    if (p->blob_prog) resources_program_release(p->blob_prog);
    if (p->blob_tex) resources_texture_release(p->blob_tex);
    if (p->vbo) glDeleteBuffers(1, &p->vbo);
    if (p->ibo) glDeleteBuffers(1, &p->ibo);
    if (p->vao) glDeleteVertexArrays(1, &p->vao);
}

SceneNode *objects_make_node(const WorldState *ws, const RendererContext *rc)
{
    ObjectsParams *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->world = ws;
    p->rc = rc;

    // Объекты из map.json: путь к PNG — model, позиция — pos,
    // масштаб — scale. y <= 0 — высота из рельефа.
    int n = mapdata_object_count();
    if (n > OBJECT_MAX) n = OBJECT_MAX;
    s_col_n = 0;
    s_box_n = 0;
    for (int i = 0; i < n; i++) {
        const MapObject *o = mapdata_object(i);
        if (!o || !o->model[0]) continue;
        ObjectInst *it = &p->inst[p->count];
        it->src = i;             // помним индекс map.json (инстансы плотнее)
        int w = 0, h = 0;
        // Пиксели нужны для пропорций и кастомных текстур; текстура —
        // через кеш ресурсов (60 деревьев tree.png = ОДНА GL-текстура).
        unsigned char *img = load_img(o->model, &w, &h, STBI_rgb_alpha);
        if (!img || w <= 0 || h <= 0) {
            SDL_Log("Object %s: cannot load texture %s (will use notfound)", o->name, o->model);
            w = 64; h = 64;   // фолбек-пропорции
        }
        // Материал: дефолты + переопределения из блока render в JSON
        material_defaults(&it->r);
        if (o->render.set) it->r = o->render;
        // Своя текстура, если заданы нестандартные wrap/filter
        if (it->r.set &&
            (it->r.wrap != MAT_WRAP_CLAMP || it->r.filter != MAT_FILTER_LINEAR)) {
            it->tex_custom = texture_custom(img, w, h, it->r.wrap, it->r.filter);
            it->tex = it->tex_custom;
            SDL_Log("Object %s: custom texture (wrap=%d filter=%d)", o->name,
                    it->r.wrap, it->r.filter);
        } else {
            it->tex = resources_texture(o->model, 4, GL_CLAMP_TO_EDGE, 0);
        }
        it->pos[0] = o->pos[0];
        it->pos[1] = o->pos[1];
        it->pos[2] = o->pos[2];
        if (it->pos[1] <= 0.0f)   // y = 0 — поставить на рельеф
            it->pos[1] = sample_height(ws, it->pos[0], it->pos[2]);
        // Уклон земли под объектом: низ креста ложится на рельеф по уклону
        // (как база растительности в wheat.vs). Дерево шириной ~12.5 м иначе
        // на склоне висело одним углом в воздухе, а другим уходило в землю.
        {
            const float e = 1.0f;         // шаг замера уклона, м
            float sdx = sample_height(ws, it->pos[0] + e, it->pos[2])
                      - sample_height(ws, it->pos[0] - e, it->pos[2]);
            float sdz = sample_height(ws, it->pos[0], it->pos[2] + e)
                      - sample_height(ws, it->pos[0], it->pos[2] - e);
            it->slope[0] = sdx / (2.0f * e);
            it->slope[1] = sdz / (2.0f * e);
        }
        float sy = o->scale[1] > 0.01f ? o->scale[1] : 1.0f;
        if (it->r.set && it->r.fixed_size) {
            it->height = it->r.height * sy;
            it->width = it->r.width * (o->scale[0] > 0.01f ? o->scale[0] : 1.0f);
        } else {
            it->height = OBJECT_BASE_H * sy;
            it->width = it->height * (float)w / (float)h;
        }
        // Биллборд/коллизия/шейдер
        it->billboard = o->billboard;
        it->collision = o->collision;
        it->radius = o->radius > 0.01f ? o->radius : 1.5f;
        if (o->shader_kind == OBJ_SHADER_BILLBOARD) it->billboard = 1;
        if (it->collision && s_col_n < OBJECT_MAX) {
            s_col[s_col_n].x = it->pos[0];
            s_col[s_col_n].z = it->pos[2];
            s_col[s_col_n].r = it->radius;
            s_col_n++;
        }
        p->count++;
        SDL_Log("Object: %s (%s) at %.0f,%.0f h=%.1fm%s%s%s", o->name, o->model,
                it->pos[0], it->pos[2], it->height,
                it->billboard ? " billboard" : "",
                it->collision ? " collision" : "",
                it->r.set ? " render" : "");
    }
    if (p->count == 0) {
        SDL_Log("Objects: no objects in map.json");
        free(p);
        return NULL;
    }

    // --- Коллизии-квадраты из collisions[]: привязка к объектам по имени ------
    for (int i = 0; i < mapdata_collision_count() && s_box_n < OBJECT_MAX; i++) {
        const MapCollision *c = mapdata_collision(i);
        if (!c || c->size[0] <= 0.01f || c->size[1] <= 0.01f) continue;
        float x = c->pos[0], z = c->pos[2];
        if (c->object[0]) {            // привязан к объекту: база = объект
            int found = 0;
            for (int k = 0; k < p->count; k++) {
                const MapObject *o = mapdata_object(p->inst[k].src);
                if (!o || strcmp(o->name, c->object) != 0) continue;
                x = p->inst[k].pos[0] + c->pos[0];
                z = p->inst[k].pos[2] + c->pos[2];
                found = 1;
                break;
            }
            if (!found) {
                SDL_Log("Collision: object \"%s\" not found, using absolute pos",
                        c->object);
            }
        }
        s_box[s_box_n].x = x;
        s_box[s_box_n].z = z;
        s_box[s_box_n].hw = c->size[0] * 0.5f;
        s_box[s_box_n].hz = c->size[1] * 0.5f;
        s_box_n++;
        SDL_Log("Collision: box at %.0f,%.0f size %.0fx%.0f%s%s", x, z,
                c->size[0], c->size[1],
                c->object[0] ? " -> " : "", c->object[0] ? c->object : "");
    }

    // --- Шейдеры ---
    p->prog = make_program("shaders/object.vs", "shaders/object.fs");
    if (!p->prog) { objects_params_release(p); free(p); return NULL; }
    p->locs = obj_locs(p->prog);
    p->flat_prog = make_program("shaders/object.vs", "shaders/object_flat.fs");
    if (p->flat_prog) p->flat_locs = obj_locs(p->flat_prog);
    glUseProgram(p->prog);
    glUniform1i(p->locs.u_tex, 0);
    // Каскады на юнитах 3-6 (uShadowMap%d и сырая глубина uShadowDepth%d)
    for (int c = 0; c < SHADOW_CASCADES; c++) {
        if (p->locs.su.u_shadowmap[c] >= 0)
            glUniform1i(p->locs.su.u_shadowmap[c], 3 + c);
        if (p->locs.su.u_shadowdepth[c] >= 0)
            glUniform1i(p->locs.su.u_shadowdepth[c], 3 + c);
    }
    glUseProgram(0);
    // Свои шейдеры объектов (custom): отдельная программа на объект.
    // Индекс инстанса НЕ равен индексу map.json (пустые model пропускаются) —
    // поэтому mapdata_object берём по сохранённому src.
    for (int i = 0; i < p->count; i++) {
        const MapObject *o = mapdata_object(p->inst[i].src);
        if (!o || o->shader_kind != OBJ_SHADER_CUSTOM) continue;
        ObjectInst *it = &p->inst[i];
        const char *vs = o->shader_vs[0] ? o->shader_vs : "shaders/object.vs";
        it->prog = make_program(vs, o->shader_fs);
        if (!it->prog) {
            SDL_Log("Object %s: custom shader failed, using default", o->name);
            continue;
        }
        it->locs = obj_locs(it->prog);
        glUseProgram(it->prog);
        if (it->locs.u_tex >= 0) glUniform1i(it->locs.u_tex, 0);
        for (int c = 0; c < SHADOW_CASCADES; c++) {
            if (it->locs.su.u_shadowmap[c] >= 0)
                glUniform1i(it->locs.su.u_shadowmap[c], 3 + c);
            if (it->locs.su.u_shadowdepth[c] >= 0)
                glUniform1i(it->locs.su.u_shadowdepth[c], 3 + c);
        }
        glUseProgram(0);
    }

    // Общий квад: x -0.5..0.5 (ширина), y 0..1 (высота), uv 0..1
    glGenVertexArrays(1, &p->vao);
    glBindVertexArray(p->vao);
    glGenBuffers(1, &p->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, p->vbo);
    const float quad[4][4] = {
        { -0.5f, 0.0f, 0.0f, 0.0f },
        {  0.5f, 0.0f, 1.0f, 0.0f },
        {  0.5f, 1.0f, 1.0f, 1.0f },
        { -0.5f, 1.0f, 0.0f, 1.0f },
    };
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), NULL);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          (const void *)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glGenBuffers(1, &p->ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, p->ibo);
    const unsigned int idx[6] = { 0, 1, 2, 0, 2, 3 };
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof idx, idx, GL_STATIC_DRAW);
    glBindVertexArray(0);

    // --- Теневой шейдер (глубина с альфа-тестом) ---
    p->shadow_prog = make_program("shaders/object_shadow.vs",
                                  "shaders/object_shadow.fs");
    if (!p->shadow_prog) { objects_params_release(p); free(p); return NULL; }
    p->s_viewproj = glGetUniformLocation(p->shadow_prog, "uViewProj");
    p->s_objpos   = glGetUniformLocation(p->shadow_prog, "uObjPos");
    p->s_objsize  = glGetUniformLocation(p->shadow_prog, "uObjSize");
    p->s_objright = glGetUniformLocation(p->shadow_prog, "uObjRight");
    p->s_objslope = glGetUniformLocation(p->shadow_prog, "uObjSlope");
    glUseProgram(p->shadow_prog);
    glUniform1i(glGetUniformLocation(p->shadow_prog, "uTex"), 0);
    glUseProgram(0);

    // --- Blob-тени (режим 3): мягкое пятно под каждым объектом --------------
    p->blob_prog = make_program("shaders/blob.vs", "shaders/blob.fs");
    if (p->blob_prog) {
        p->b_viewproj = glGetUniformLocation(p->blob_prog, "uViewProj");
        p->b_objpos   = glGetUniformLocation(p->blob_prog, "uObjPos");
        p->b_objsize  = glGetUniformLocation(p->blob_prog, "uObjSize");
        p->b_objright = glGetUniformLocation(p->blob_prog, "uObjRight");
        p->b_objfwd   = glGetUniformLocation(p->blob_prog, "uObjFwd");
        p->b_tex      = glGetUniformLocation(p->blob_prog, "uTex");
        p->b_color    = glGetUniformLocation(p->blob_prog, "uColor");
        p->blob_tex = resources_texture("img/fauna_glow.png", 4,
                                        GL_CLAMP_TO_EDGE, 1);
        glUseProgram(p->blob_prog);
        glUniform1i(p->b_tex, 0);
        glUseProgram(0);
    }

    s_objs = p;                   // для hot-reload
    shader_on_reshade(objects_reshade);

    const float size[3] = { 100.0f, OBJECT_BASE_H * 4.0f, 100.0f };
    SceneNode *node = node_new("Objects", "MeshInstance3D", NULL, size, p,
                               NULL, objects_node_render);
    if (!node) {
        objects_params_release(p);
        free(p);
    }
    return node;
}

// Теневой проход (RP_SHADOW): та же геометрия и выбор грани, что в цветном
// проходе, но программа object_shadow (глубина с альфа-тестом). Только
// объекты внутри бокса каскада. Раньше это была копипаста objects_shadow_render.
static void objects_shadow_draw(ObjectsParams *p, const RendererContext *rc)
{
    if (p->count <= 0) return;
    glDisable(GL_CULL_FACE);   // биллборды — с двух сторон
    glUseProgram(p->shadow_prog);
    glUniformMatrix4fv(p->s_viewproj, 1, GL_FALSE, rc->shadow_vp);
    glBindVertexArray(p->vao);
    for (int i = 0; i < p->count; i++) {
        ObjectInst *it = &p->inst[i];
        // отсечение по боксу каскада (от камеры). Но объект рисует и дальний,
        // если его ДЛИННАЯ ТЕНЬ достаёт до бокса: тень уходит по направлению
        // солнца на height*shadow_slope метров. Иначе ближние каскады не
        // рисуют дальние деревья, хотя их тень лежит рядом с игроком.
        float reach = it->height * rc->shadow_slope + it->width * 0.5f;
        float r = rc->shadow_box_cur * 0.7071f + 6.0f + reach;
        float r2 = r * r;
        float dx = rc->cam_pos[0] - it->pos[0];
        float dz = rc->cam_pos[2] - it->pos[2];
        if (dx * dx + dz * dz > r2) continue;
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, it->tex);
        glUniform3f(p->s_objpos, it->pos[0], it->pos[1], it->pos[2]);
        glUniform2f(p->s_objsize, it->width, it->height);
        if (p->s_objslope >= 0)
            glUniform2f(p->s_objslope, it->slope[0], it->slope[1]);
        // тот же выбор грани, что в objects_node_render
        if (it->billboard) {
            float d = sqrtf(dx * dx + dz * dz);
            if (d < 0.001f) d = 1.0f;
            glUniform3f(p->s_objright, -dz / d, 0.0f, dx / d);
        } else {
            if (fabsf(dx) >= fabsf(dz))
                glUniform3f(p->s_objright, 0.0f, 0.0f, 1.0f);
            else
                glUniform3f(p->s_objright, 1.0f, 0.0f, 0.0f);
        }
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, NULL);
    }
}

void objects_node_render(SceneNode *n, float dt)
{
    ObjectsParams *p = n->params;
    if (p->count <= 0) return;
    const RendererContext *rc = p->rc;
    // Теневой проход (RP_SHADOW): глубина с альфа-тестом
    if (rc->pass == RP_SHADOW) {
        if (g_shadow_obj) objects_shadow_draw(p, rc);
        return;
    }
    s_time += dt;
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);   // по умолчанию — двусторонний (cull: none)
    glBindVertexArray(p->vao);

    // --- Blob-тени (режим 3): мягкое пятно на земле под каждым объектом -----
    // Смещение пятна от солнца: тень падает по направлению света.
    if (cvar_get("r_shadows_mode") == 3.0f && p->blob_prog) {
        const float *ld = p->rc->light_dir[0];
        float ll = sqrtf(ld[0] * ld[0] + ld[2] * ld[2]);
        float sdx = ll > 0.001f ? ld[0] / ll : 0.0f;   // горизонталь к солнцу
        float sdz = ll > 0.001f ? ld[2] / ll : 0.0f;
        const float col[4] = { 0.05f, 0.05f, 0.06f, 0.5f };
        // БЕЗ записи глубины: пятна полупрозрачные, а запись глубины
        // отсекала биллборды, нарисованные позже ЗА пятном (дырки в траве
        // вокруг теней деревьев). Блендинг поверх рельефа, глубина читается.
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);
        glUseProgram(p->blob_prog);
        glUniformMatrix4fv(p->b_viewproj, 1, GL_FALSE, p->rc->viewproj);
        glUniform4f(p->b_color, col[0], col[1], col[2], col[3]);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, p->blob_tex);
        for (int i = 0; i < p->count; i++) {
            ObjectInst *it = &p->inst[i];
            // Пятно не во фрустуме — мимо (и uniform'ы не льём): без этого
            // слабый iGPU перемалывал тысячи невидимых квадов каждый кадр
            if (!frustum_test_sphere(&p->rc->frustum, it->pos[0],
                                     it->pos[1] + it->height * 0.5f,
                                     it->pos[2],
                                     it->height + it->width))
                continue;
            float h = it->height;
            float sz = it->width * 0.65f;
            float px = it->pos[0] - sdx * h * 0.55f;
            float pz = it->pos[2] - sdz * h * 0.55f;
            // пятно ложится на рельеф ПОД СОБОЙ (на склонах не проваливается)
            float base = sample_height(p->world, px, pz);
            glUniform3f(p->b_objpos, px, base + 0.25f, pz);
            glUniform2f(p->b_objsize, sz, sz);
            glUniform3f(p->b_objright, 1.0f, 0.0f, 0.0f);
            glUniform3f(p->b_objfwd, 0.0f, 0.0f, 1.0f);
            glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, NULL);
        }
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
        glUseProgram(0);
    }

    GLuint cur = 0, cur_tex = 0;
    Material mat_cache;
    int mat_valid = 0;
    // MSAA не сглаживает alpha-cutout края (discard) — для альфа-тестовых
    // материалов включаем alpha-to-coverage (полупрозрачная кромка текселя
    // даёт частичное покрытие сэмплов). Блендинговые материалы не трогаем:
    // там частичное покрытие умножалось бы на блендинг (a*a на краях).
    const int msaa_on = bloom_msaa_active() > 0;
    int a2c = 0;
    for (int i = 0; i < p->count; i++) {
        ObjectInst *it = &p->inst[i];
        // Отсечение фрустумом: сфера (позиция + максимальный размер)
        if (!frustum_test_sphere(&p->rc->frustum, it->pos[0], it->pos[1],
                                 it->pos[2],
                                 it->height + it->width * 0.5f))
            continue;
        GLuint prog = it->prog ? it->prog : p->prog;
        if (prog != cur) {
            cur = prog;
            const ObjLocs *l = it->prog ? &it->locs : &p->locs;
            glUseProgram(prog);
            set_per_frame(l, p);
            mat_valid = 0;
        }
        const ObjLocs *l = it->prog ? &it->locs : &p->locs;

        material_bind_cached(&it->r, &l->mat, s_time, &mat_cache, &mat_valid);

        int want_a2c = msaa_on && mat_cache.blend == MAT_BLEND_ALPHATEST;
        if (want_a2c != a2c) {
            a2c = want_a2c;
            if (a2c) glEnable(GL_SAMPLE_ALPHA_TO_COVERAGE);
            else     glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
        }

        if (it->tex != cur_tex) {
            cur_tex = it->tex;
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, it->tex);
        }
        glUniform3f(l->u_objpos, it->pos[0], it->pos[1], it->pos[2]);
        glUniform2f(l->u_objsize, it->width, it->height);
        if (l->u_objslope >= 0)
            glUniform2f(l->u_objslope, it->slope[0], it->slope[1]);
        // Ось ширины квада:
        //  - биллборд: перпендикуляр к направлению на камеру (квад к ней);
        //  - крест: статичный выбор грани по углу обзора.
        float dx = p->rc->cam_pos[0] - it->pos[0];
        float dz = p->rc->cam_pos[2] - it->pos[2];
        if (it->billboard) {
            float d = sqrtf(dx * dx + dz * dz);
            if (d < 0.001f) d = 1.0f;
            glUniform3f(l->u_objright, -dz / d, 0.0f, dx / d);
        } else {
            if (fabsf(dx) >= fabsf(dz))
                glUniform3f(l->u_objright, 0.0f, 0.0f, 1.0f);
            else
                glUniform3f(l->u_objright, 1.0f, 0.0f, 0.0f);
        }
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, NULL);
    }
    if (a2c) glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glDisable(GL_CULL_FACE);
    glUseProgram(0);
}

// Выталкивание точки из коллизий карты: цилиндры объектов + квадраты
// collisions[]. Игрок радиусом pr не может зайти внутрь. Изменяет *x,*z.
void objects_collide(float *x, float *z, float pr)
{
    for (int i = 0; i < s_col_n; i++) {
        float dx = *x - s_col[i].x;
        float dz = *z - s_col[i].z;
        float rr = s_col[i].r + pr;
        float d2 = dx * dx + dz * dz;
        if (d2 >= rr * rr) continue;
        if (d2 < 1e-6f) {          // стоим на центре — толкаем в сторону X
            *x += rr;
            continue;
        }
        float d = sqrtf(d2);
        float push = (rr - d) / d;
        *x += dx * push;
        *z += dz * push;
    }
    // Квадраты: выталкивание по ближайшей грани
    for (int i = 0; i < s_box_n; i++) {
        float hx = s_box[i].hw + pr;
        float hz = s_box[i].hz + pr;
        float dx = *x - s_box[i].x;
        float dz = *z - s_box[i].z;
        if (fabsf(dx) >= hx || fabsf(dz) >= hz) continue;
        float ox = hx - fabsf(dx);   // перекрытие по X
        float oz = hz - fabsf(dz);   // перекрытие по Z
        if (ox < oz)                 // толкаем по меньшему перекрытию
            *x += (dx >= 0.0f ? ox : -ox);
        else
            *z += (dz >= 0.0f ? oz : -oz);
    }
}


void objects_cleanup(SceneNode *n)
{
    if (!n) return;
    ObjectsParams *p = n->params;
    if (!p) return;
    objects_params_release(p);
}
