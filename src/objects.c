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
#include "cvars.h"
#include "gl_loader.h"
#include "linalg.h"
#include "mapdata.h"
#include "resources.h"
#include "shader.h"
#include "shadow.h"
#include "sky.h"
#include "world.h"

#include "stb_image.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#define OBJECT_MAX 256
#define OBJECT_BASE_H 9.0f     // высота квада при scale.y = 1

// Uniform-локации шейдера объекта (общие для всех программ):
// общий набор сцены (SceneUniforms) + свои uniform'ы объекта/материала.
typedef struct {
    SceneUniforms su;            // общий набор сцены (render_ctx.h)
    GLint u_objpos, u_objsize, u_objright, u_tex;
    GLint u_tint;
    GLint u_blend, u_alpha;
    GLint u_uvtile, u_uvoffset, u_uvscroll, u_time;
} ObjLocs;

typedef struct {
    GLuint tex;
    GLuint tex_custom;       // своя текстура (кастомные wrap/filter)
    GLuint prog;             // свой шейдер (0 = общий p->prog)
    ObjLocs locs;            // локации для prog
    int billboard;           // квад к камере (вместо креста)
    float pos[3];
    float width, height;
    int collision;           // цилиндр непроходимости
    float radius;            // радиус коллизии, м
    MapObjectRender r;       // полная настройка отображения
} ObjectInst;

typedef struct {
    const WorldState *world; // данные мира: высоты (посадка на рельеф)
    const RendererContext *rc;   // кадровые данные (renderer.c)
    GLuint prog, vao, vbo, ibo;
    GLuint flat_prog;        // встроенный "flat" (без освещения)
    GLuint shadow_prog;      // глубина в карты теней
    GLint s_viewproj, s_objpos, s_objsize, s_objright;
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
// Время (для анимации UV)
static float s_time = 0.0f;

// Заполняет локации uniform'ов программы (необязательные = -1)
static ObjLocs obj_locs(GLenum prog)
{
    ObjLocs l;
    memset(&l, 0, sizeof l);
    scene_uniforms_init(&l.su, prog);      // общий набор сцены
    l.u_objpos   = glGetUniformLocation(prog, "uObjPos");
    l.u_objsize  = glGetUniformLocation(prog, "uObjSize");
    l.u_objright = glGetUniformLocation(prog, "uObjRight");
    l.u_tex      = glGetUniformLocation(prog, "uTex");
    l.u_tint     = glGetUniformLocation(prog, "uTint");
    l.u_blend    = glGetUniformLocation(prog, "uBlend");
    l.u_alpha    = glGetUniformLocation(prog, "uAlpha");
    l.u_uvtile   = glGetUniformLocation(prog, "uUvTile");
    l.u_uvoffset = glGetUniformLocation(prog, "uUvOffset");
    l.u_uvscroll = glGetUniformLocation(prog, "uUvScroll");
    l.u_time     = glGetUniformLocation(prog, "uTime");
    return l;
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
    if (l->u_time >= 0) glUniform1f(l->u_time, s_time);
    // Дефолты отображения (устанавливаются до per-instance uniform'ов)
    if (l->u_blend >= 0) glUniform1i(l->u_blend, RENDER_BLEND_ALPHATEST);
    if (l->u_alpha >= 0) glUniform1f(l->u_alpha, 1.0f);
    if (l->u_uvtile >= 0) glUniform2f(l->u_uvtile, 1.0f, 1.0f);
    if (l->u_uvoffset >= 0) glUniform2f(l->u_uvoffset, 0.0f, 0.0f);
    if (l->u_uvscroll >= 0) glUniform2f(l->u_uvscroll, 0.0f, 0.0f);
}

// Копия текстуры с другими параметрами (wrap/filter) — кеш движка не трогаем
static GLuint texture_custom(const unsigned char *img, int w, int h,
                             int wrap, int filter)
{
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    GLint wmode = GL_CLAMP_TO_EDGE, fmag = GL_LINEAR, fmin = GL_LINEAR;
    if (wrap == TEX_WRAP_REPEAT) wmode = GL_REPEAT;
    else if (wrap == TEX_WRAP_MIRROR) wmode = GL_MIRRORED_REPEAT;
    if (filter == TEX_FILTER_NEAREST) fmag = fmin = GL_NEAREST;
    else if (filter == TEX_FILTER_MIPMAP) fmin = GL_LINEAR_MIPMAP_LINEAR;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wmode);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wmode);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, fmin);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, fmag);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, img);
    if (filter == TEX_FILTER_MIPMAP) glGenerateMipmap(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
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
        int w = 0, h = 0;
        // Пиксели нужны для пропорций и кастомных текстур; текстура —
        // через кеш ресурсов (60 деревьев tree.png = ОДНА GL-текстура).
        unsigned char *img = load_img(o->model, &w, &h, STBI_rgb_alpha);
        if (!img || w <= 0 || h <= 0) {
            SDL_Log("Object %s: cannot load texture %s", o->name, o->model);
            continue;
        }
        it->r = o->render;
        // Своя текстура, если заданы нестандартные wrap/filter
        if (it->r.set &&
            (it->r.wrap != TEX_WRAP_CLAMP || it->r.filter != TEX_FILTER_LINEAR)) {
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
                const MapObject *o = mapdata_object(k);
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
        SDL_Log("Collision: box at %.0f,%.0f size %.0fx%.0f%s", x, z,
                c->size[0], c->size[1],
                c->object[0] ? " -> " : "", c->object[0] ? c->object : "");
    }

    // --- Шейдеры ---
    p->prog = make_program("shaders/object.vs", "shaders/object.fs");
    if (!p->prog) { free(p); return NULL; }
    p->locs = obj_locs(p->prog);
    p->flat_prog = make_program("shaders/object.vs", "shaders/object_flat.fs");
    if (p->flat_prog) p->flat_locs = obj_locs(p->flat_prog);
    glUseProgram(p->prog);
    glUniform1i(p->locs.u_tex, 0);
    glUniform3f(p->locs.u_tint, 1.0f, 1.0f, 1.0f);
    // Каскады на юнитах 3-6 (uShadowMap%d и сырая глубина uShadowDepth%d)
    for (int c = 0; c < SHADOW_CASCADES; c++) {
        if (p->locs.su.u_shadowmap[c] >= 0)
            glUniform1i(p->locs.su.u_shadowmap[c], 3 + c);
        if (p->locs.su.u_shadowdepth[c] >= 0)
            glUniform1i(p->locs.su.u_shadowdepth[c], 3 + c);
    }
    glUseProgram(0);
    // Свои шейдеры объектов (custom): отдельная программа на объект
    for (int i = 0; i < p->count; i++) {
        const MapObject *o = mapdata_object(i);
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
        if (it->locs.u_tint >= 0) glUniform3f(it->locs.u_tint, 1.0f, 1.0f, 1.0f);
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
    if (!p->shadow_prog) { free(p); return NULL; }
    p->s_viewproj = glGetUniformLocation(p->shadow_prog, "uViewProj");
    p->s_objpos   = glGetUniformLocation(p->shadow_prog, "uObjPos");
    p->s_objsize  = glGetUniformLocation(p->shadow_prog, "uObjSize");
    p->s_objright = glGetUniformLocation(p->shadow_prog, "uObjRight");
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

    const float size[3] = { 100.0f, OBJECT_BASE_H * 4.0f, 100.0f };
    return node_new("Objects", "MeshInstance3D", NULL, size, p,
                    NULL, objects_node_render);
}

// Теневой проход (RP_SHADOW): та же геометрия и выбор грани, что в цветном
// проходе, но программа object_shadow (глубина с альфа-тестом). Только
// объекты внутри бокса каскада. Раньше это была копипаста objects_shadow_render.
static void objects_shadow_draw(ObjectsParams *p, const RendererContext *rc)
{
    if (p->count <= 0) return;
    float r = rc->shadow_box_cur * 0.7071f + 40.0f;   // бокс + диагональ+запас
    float r2 = r * r;
    glDisable(GL_CULL_FACE);   // биллборды — с двух сторон
    glUseProgram(p->shadow_prog);
    glUniformMatrix4fv(p->s_viewproj, 1, GL_FALSE, rc->shadow_vp);
    glBindVertexArray(p->vao);
    for (int i = 0; i < p->count; i++) {
        ObjectInst *it = &p->inst[i];
        // отсечение по боксу каскада (от камеры)
        float dx = rc->cam_pos[0] - it->pos[0];
        float dz = rc->cam_pos[2] - it->pos[2];
        if (dx * dx + dz * dz > r2) continue;
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, it->tex);
        glUniform3f(p->s_objpos, it->pos[0], it->pos[1], it->pos[2]);
        glUniform2f(p->s_objsize, it->width, it->height);
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
        glDisable(GL_BLEND);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_TRUE);
        glUseProgram(p->blob_prog);
        glUniformMatrix4fv(p->b_viewproj, 1, GL_FALSE, p->rc->viewproj);
        glUniform4f(p->b_color, col[0], col[1], col[2], col[3]);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, p->blob_tex);
        for (int i = 0; i < p->count; i++) {
            ObjectInst *it = &p->inst[i];
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
        glDisable(GL_BLEND);
        glUseProgram(0);
    }

    GLuint cur = 0;
    for (int i = 0; i < p->count; i++) {
        ObjectInst *it = &p->inst[i];
        GLuint prog = it->prog ? it->prog : p->prog;
        if (prog != cur) {
            cur = prog;
            const ObjLocs *l = it->prog ? &it->locs : &p->locs;
            glUseProgram(prog);
            set_per_frame(l, p);
        }
        const ObjLocs *l = it->prog ? &it->locs : &p->locs;

        // --- render: блендинг, отсечение граней, глубина ---
        if (it->r.set) {
            if (it->r.blend == RENDER_BLEND_ALPHA) {
                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            } else if (it->r.blend == RENDER_BLEND_ADDITIVE) {
                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE);
            } else {
                glDisable(GL_BLEND);
            }
            if (it->r.cull == RENDER_CULL_NONE) glDisable(GL_CULL_FACE);
            else {
                glEnable(GL_CULL_FACE);
                glCullFace(it->r.cull == RENDER_CULL_BACK
                               ? GL_BACK : GL_FRONT);
            }
            glDepthMask(it->r.depth_write ? GL_TRUE : GL_FALSE);
        } else {
            glDisable(GL_BLEND);
            glDisable(GL_CULL_FACE);
            glDepthMask(GL_TRUE);
        }

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, it->tex);
        glUniform3f(l->u_objpos, it->pos[0], it->pos[1], it->pos[2]);
        glUniform2f(l->u_objsize, it->width, it->height);
        // render: tint/alpha и настройки текстуры (UV)
        if (it->r.set) {
            if (l->u_tint >= 0)
                glUniform3f(l->u_tint, it->r.tint[0], it->r.tint[1], it->r.tint[2]);
            if (l->u_blend >= 0) glUniform1i(l->u_blend, it->r.blend);
            if (l->u_alpha >= 0) glUniform1f(l->u_alpha, it->r.alpha);
            if (l->u_uvtile >= 0) glUniform2f(l->u_uvtile, it->r.tile[0], it->r.tile[1]);
            if (l->u_uvoffset >= 0)
                glUniform2f(l->u_uvoffset, it->r.offset[0], it->r.offset[1]);
            if (l->u_uvscroll >= 0)
                glUniform2f(l->u_uvscroll, it->r.scroll[0], it->r.scroll[1]);
        }
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
    ObjectsParams *p = n->params;
    if (!p) return;
    s_col_n = 0;
    s_box_n = 0;
    for (int i = 0; i < p->count; i++) {
        if (p->inst[i].tex_custom)
            glDeleteTextures(1, &p->inst[i].tex_custom);
        else
            resources_texture_release(p->inst[i].tex);
        if (p->inst[i].prog) resources_program_release(p->inst[i].prog);
    }
    resources_program_release(p->prog);
    if (p->flat_prog) resources_program_release(p->flat_prog);
    resources_program_release(p->shadow_prog);
    if (p->blob_prog) {
        resources_program_release(p->blob_prog);
        resources_texture_release(p->blob_tex);
    }
    glDeleteBuffers(1, &p->vbo);
    glDeleteBuffers(1, &p->ibo);
    glDeleteVertexArrays(1, &p->vao);
}
