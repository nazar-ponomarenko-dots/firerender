// ============================================================================
//  srender.h — SIMPLE RENDER: рендерное API движка (драйверо-независимое)
// ----------------------------------------------------------------------------
//  Единственная точка, через которую код движка должен работать с GPU.
//  Драйвер (OpenGL / OpenGL ES сегодня, Vulkan / Metal в будущем) сидит за
//  спиной и реализует этот интерфейс: сейчас — srender_gl.c. Прямые вызовы
//  gl* в модулях движка — легаси, миграция на sr_* идёт поэтапно.
//
//  Правила проектирования (чтобы Vulkan/Metal легли без переписывания API):
//    1. Никаких типов/констант GL в этом заголовке — только свои енумы.
//    2. Возможности контекста читаются из sr_caps(), а не через ifdef:
//       драйвер сам решает, чем закрыть фичу (фолбэк внутри бэкенда).
//    3. State-модель командная (cap on/off, bind, draw), как в GL/GLES —
//       бэкенду с другой моделью (Vulkan) придётся кешировать состояние
//       у себя; движку от этого не больно.
//    4. Ресурсы — непрозрачные хэндлы (unsigned), а не GL-имена.
// ============================================================================
#ifndef SRENDER_H
#define SRENDER_H

// --- Хэндлы ресурсов (непрозрачные; 0 = "нет ресурса") ----------------------
typedef unsigned int SrTex;      // текстура (2D / 3D / кубомапа)
typedef unsigned int SrFbo;      // фреймбуфер (таргет рендера)
typedef unsigned int SrRb;       // рендербуфер (depth/MSAA)
typedef unsigned int SrBuf;      // вершинный/индексный/SSBO-буфер
typedef unsigned int SrVao;      // набор вершинных атрибутов
typedef unsigned int SrProg;     // шейдерная программа
typedef int          SrLoc;      // локация юниформа (-1 = нет)
typedef unsigned int SrQuery;    // GPU-таймер

// --- Девайс: один на процесс, создаётся после инициализации контекста -------
typedef struct SrDevice SrDevice;

// Возможности контекста: драйвер заполняет при sr_create().
typedef struct {
    int gles;            // 1 = OpenGL ES, 0 = desktop OpenGL
    int gl_major, gl_minor;
    int compute;         // dispatch + SSBO (GL 4.3 / GLES 3.1)
    int indirect;        // draw indirect (+ multi)
    int base_instance;   // baseinstance в инстансных draw
    int timer_query;     // GPU-таймеры (GL 3.3 / GLES с EXT_disjoint_timer_query)
    int tex3d;           // 3D-текстуры
    int color_half_float;// RGBA16F можно использовать как render target
    int texture_norm16;  // нормализованные R16/RG16 текстуры
    int msaa_max;        // максимум сэмплов MSAA (0 = нет)
    int max_tex_size;    // максимум габарит текстуры, px
    const char *vendor, *renderer, *version;
} SrCaps;

// --- Цели текстур и форматы --------------------------------------------------
typedef enum {
    SR_TEX_2D,
    SR_TEX_3D,
    SR_TEX_CUBE,
} SrTexTarget;

// Внутренний формат хранения (GL-маппинг — в бэкенде).
typedef enum {
    SR_FMT_RGBA8,        // LDR-сцена, HUD-атласы, спрайты
    SR_FMT_RGB8,
    SR_FMT_R8,           // маски (снег, декали)
    SR_FMT_R16,          // высоты/поля (16 бит целых)
    SR_FMT_RG8,
    SR_FMT_RGBA16F,      // HDR-сцена (bloom, GI, SSR)
    SR_FMT_DEPTH24,      // теневые карты, depth-only проходы
} SrFormat;

// Раскладка пикселей при загрузке с CPU (что лежит в data).
typedef enum {
    SR_PIX_RGBA,         // 4 x unsigned byte
    SR_PIX_RGB,          // 3 x unsigned byte
    SR_PIX_RG,           // 2 x unsigned byte
    SR_PIX_RED,          // 1 x unsigned byte
    SR_PIX_DEPTH_UINT,   // unsigned int (depth)
} SrPixFormat;

// Фильтры и обёртка.
typedef enum {
    SR_FILTER_NEAREST,
    SR_FILTER_LINEAR,
    SR_FILTER_NEAREST_MIPMAP_NEAREST,
    SR_FILTER_LINEAR_MIPMAP_NEAREST,
    SR_FILTER_LINEAR_MIPMAP_LINEAR,
} SrFilter;

typedef enum {
    SR_WRAP_REPEAT,
    SR_WRAP_CLAMP,       // clamp-to-edge
    SR_WRAP_MIRROR,
} SrWrap;

// --- Буферы и рисование ------------------------------------------------------
typedef enum {
    SR_BUF_ARRAY,        // вершинные данные
    SR_BUF_ELEMENT,      // индексы
    SR_BUF_UNIFORM,
    SR_BUF_SSBO,         // shader storage
    SR_BUF_INDIRECT,     // команды indirect draw
} SrBufTarget;

typedef enum {
    SR_PRIM_LINES,
    SR_PRIM_TRIANGLES,
    SR_PRIM_TRIANGLE_STRIP,
    SR_PRIM_POINTS,
} SrPrim;

// Биты барьера памяти после compute-записей (комбинируются через |).
enum {
    SR_BARRIER_SSBO      = 1 << 0,
    SR_BARRIER_VERTEX    = 1 << 1,
    SR_BARRIER_INDEX     = 1 << 2,
    SR_BARRIER_UNIFORM   = 1 << 3,
    SR_BARRIER_COMMAND   = 1 << 4,
    SR_BARRIER_TEXTURE   = 1 << 5,
    SR_BARRIER_IMAGE     = 1 << 6,
    SR_BARRIER_ALL       = 0x7F,
};

// --- Состояние ----------------------------------------------------------------
typedef enum {
    SR_CAP_DEPTH_TEST,
    SR_CAP_CULL_FACE,
    SR_CAP_BLEND,
} SrStateCap;

typedef enum {
    SR_BLEND_ZERO,
    SR_BLEND_ONE,
    SR_BLEND_SRC_ALPHA,
    SR_BLEND_ONE_MINUS_SRC_ALPHA,
} SrBlendFac;

// --- Рендерные API (бэкенды) ---------------------------------------------------
// Движок работает через sr_* — сами бэкенды подключаются за ним. Сегодня
// реализован GL/GLES (srender_gl.c); Vulkan и Metal — зачатки: каркас выбора
// и зонды есть, реализация — поэтапная миграция (см. ROADMAP в заголовке).
//
// План подключения нового API (например Vulkan):
//   1. src/srender_vk.c реализует те же sr_* функции поверх VkInstance/
//      VkDevice (или переиспользует диспетчер из srender_gl.c, если мы
//      оставим GL-именование состояний).
//   2. Шейдеры GLSL 330 компилируются в SPIR-V (glslc/glslangValidator,
//      цель: --target-env=vulkan1.1); Metal — транслируется MSL-бэкендом
//      SPIRV-Cross (или MoltenVK, который ест SPIR-V напрямую).
//   3. Поверхность окна — SDL_Vulkan_CreateSurface / SDL_Metal_CreateView.
//   4. main.c выбирает API через sr_api_available() + cvar r_backend.
typedef enum {
    SR_API_GL = 0,       // desktop OpenGL (сегодня: основной бэкенд)
    SR_API_GLES,         // OpenGL ES 3.x (сегодня: low-end/Android профиль)
    SR_API_VULKAN,       // Vulkan 1.1+ (заготовка)
    SR_API_METAL,        // Metal 2 через MSL или MoltenVK (заготовка)
    SR_API_NULL,         // no-op заглушка (headless-серверы, тесты логики)
    SR_API_COUNT
} SrApi;

// Человекочитаемое имя ("OpenGL", "Vulkan", ...).
const char *sr_api_name(SrApi api);
// 1 = API скомпилирован в движок и может быть выбран. Внутри также
// учитывает платформу (Metal — только __APPLE__ и т.п.).
int  sr_api_available(SrApi api);
// SDL_Log-отчёт по всем API (вызывает main.c при старте).
void sr_api_report(void);

// --- Жизненный цикл ----------------------------------------------------------
// Создать девайс над ТЕКУЩИМ контекстом (GL-функции уже загружены).
// Будущее: SrDevice *sr_create_api(SrApi api, void *native_surface) —
// единая фабрика всех бэкендов; пока GL/GLES создаётся здесь.
SrDevice *sr_create(void);
void      sr_destroy(SrDevice *d);
const SrCaps *sr_caps(const SrDevice *d);

// --- Кадр --------------------------------------------------------------------
void sr_bind_fbo(SrDevice *d, SrFbo fbo);          // 0 = окно/экран
void sr_viewport(SrDevice *d, int x, int y, int w, int h);
void sr_clear(SrDevice *d, float r, float g, float b, float a,
              int clear_color, int clear_depth);
int  sr_fbo_current(SrDevice *d);                  // текущий привязанный
int  sr_fbo_complete(SrDevice *d);                 // валиден ли текущий FBO

// --- Состояние пайплайна ------------------------------------------------------
void sr_cap(SrDevice *d, SrStateCap cap, int on);
void sr_depth_mask(SrDevice *d, int write_enabled);
void sr_blend_func(SrDevice *d, SrBlendFac src, SrBlendFac dst);
void sr_cull_front(SrDevice *d, int front_ccw_is_front); // ориентация фронтa
void sr_wireframe(SrDevice *d, int on);            // может быть no-op (GLES)

// --- Текстуры ------------------------------------------------------------------
SrTex sr_tex_gen(SrDevice *d);
void  sr_tex_delete(SrDevice *d, SrTex *t);
void  sr_tex_active(SrDevice *d, int unit);
void  sr_tex_bind(SrDevice *d, SrTexTarget target, SrTex t);
// Выделить/загрузить уровень 0 (data == NULL — только выделить).
// Для кубомапы face = 0..5 (+X..-Z), для обычных текстур face = -1.
void  sr_tex_image_2d(SrDevice *d, SrTex t, int face, SrFormat fmt,
                      int w, int h, SrPixFormat pix, const void *data);
void  sr_tex_image_3d(SrDevice *d, SrTex t, SrFormat fmt,
                      int w, int h, int depth, SrPixFormat pix,
                      const void *data);
// Обновить диапазон 3D-текстуры (слои z..z+depth).
void  sr_tex_sub_image_3d(SrDevice *d, SrTex t, int x, int y, int z,
                          int w, int h, int depth, SrPixFormat pix,
                          const void *data);
void  sr_tex_params(SrDevice *d, SrTex t, SrTexTarget target,
                    SrFilter min_f, SrFilter mag_f,
                    SrWrap wrap_s, SrWrap wrap_t, SrWrap wrap_r);
void  sr_tex_generate_mipmap(SrDevice *d, SrTex t, SrTexTarget target);

// --- Рендербуферы и FBO ---------------------------------------------------------
SrRb  sr_rb_gen(SrDevice *d);
void  sr_rb_delete(SrDevice *d, SrRb *rb);
// Хранилище рендербуфера; samples > 1 — MSAA (если бэкенд умеет).
void  sr_rb_storage(SrDevice *d, SrRb rb, SrFormat fmt, int samples,
                    int w, int h);
SrFbo sr_fbo_gen(SrDevice *d);
void  sr_fbo_delete(SrDevice *d, SrFbo *fbo);
// Прицепить текстуру к точке: attachment 0 = color0, -1 = depth.
// Для кубомапы face = 0..5 (конкретная грань), для остальных face = -1.
void  sr_fbo_attach_tex(SrDevice *d, SrFbo fbo, int attachment,
                        SrTexTarget target, SrTex t, int face);
void  sr_fbo_attach_rb(SrDevice *d, SrFbo fbo, int attachment, SrRb rb);
// Копировать прямоугольник src -> dst (размер в пикселях, фильтр для color).
void  sr_blit(SrDevice *d, SrFbo src, SrFbo dst,
              int w, int h, int linear, int copy_depth);

// --- Шейдерные программы ---------------------------------------------------------
// vs/fs/cs — пути внутри shaders/ (компиляция с платформенным префиксом
// версии GLSL делает драйвер; кеш программ — внутри).
SrProg sr_prog_new(SrDevice *d, const char *vs_path, const char *fs_path);
SrProg sr_prog_compute_new(SrDevice *d, const char *cs_path);
void   sr_prog_delete(SrDevice *d, SrProg *prog);
void   sr_prog_use(SrDevice *d, SrProg prog);
SrLoc  sr_prog_loc(SrDevice *d, SrProg prog, const char *name);

// --- Юниформы ---------------------------------------------------------------------
void sr_uniform_int(SrDevice *d, SrLoc loc, int v);
void sr_uniform_float(SrDevice *d, SrLoc loc, float v);
void sr_uniform_float2(SrDevice *d, SrLoc loc, float x, float y);
void sr_uniform_float3(SrDevice *d, SrLoc loc, float x, float y, float z);
void sr_uniform_float4(SrDevice *d, SrLoc loc, float x, float y, float z, float w);
void sr_uniform_float1v(SrDevice *d, SrLoc loc, int n, const float *v);
void sr_uniform_float3v(SrDevice *d, SrLoc loc, int n, const float *v);
void sr_uniform_float4v(SrDevice *d, SrLoc loc, int n, const float *v);
void sr_uniform_mat4(SrDevice *d, SrLoc loc, const float *m);   // column-major

// --- Буферы и вершинные атрибуты ----------------------------------------------------
SrBuf sr_buf_gen(SrDevice *d);
void  sr_buf_delete(SrDevice *d, SrBuf *buf);
void  sr_buf_bind(SrDevice *d, SrBufTarget target, SrBuf buf);
// usage: 1 = static (гружим один раз), 0 = stream (перевыгружается каждый кадр).
void  sr_buf_upload(SrDevice *d, SrBufTarget target,
                    const void *data, unsigned int bytes, int static_use);
void  sr_buf_sub_upload(SrDevice *d, SrBufTarget target, unsigned int offset,
                        const void *data, unsigned int bytes);
// SSBO на точку привязки (нужен caps->compute).
void  sr_buf_bind_ssbo(SrDevice *d, SrBuf buf, int binding);

SrVao sr_vao_gen(SrDevice *d);
void  sr_vao_delete(SrDevice *d, SrVao *vao);
void  sr_vao_bind(SrDevice *d, SrVao vao);
// Атрибут из float-векторов comps (1..4); divisor > 0 — инстансный.
void  sr_attrib_float(SrDevice *d, SrLoc loc, int comps, int stride_bytes,
                      unsigned int offset, int divisor);

// --- Draw ----------------------------------------------------------------------------
void sr_draw_arrays(SrDevice *d, SrPrim prim, int first, int count);
// type: индексы — uint16 (0) или uint32 (1).
void sr_draw_elements(SrDevice *d, SrPrim prim, int count, int indices_u32,
                      unsigned int byte_offset);
void sr_draw_arrays_instanced(SrDevice *d, SrPrim prim, int first, int count,
                              int instances);
void sr_draw_elements_instanced(SrDevice *d, SrPrim prim, int count,
                                int indices_u32, unsigned int byte_offset,
                                int instances);
// baseinstance: если бэкенд не умеет (caps->base_instance == 0), работает
// только с base == 0 — вызывающий обязан это учитывать.
void sr_draw_elements_instanced_base(SrDevice *d, SrPrim prim, int count,
                                     int indices_u32, unsigned int byte_offset,
                                     int instances, unsigned int base);
// Indirect: в буфере SR_BUF_INDIRECT лежит команда draw elements.
void sr_draw_elements_indirect(SrDevice *d, SrPrim prim,
                               SrBuf cmd_buf, unsigned int byte_offset);

// --- Compute --------------------------------------------------------------------------
void sr_dispatch(SrDevice *d, int x, int y, int z);
void sr_barrier(SrDevice *d, unsigned int barrier_bits);

// --- GPU-таймеры (caps->timer_query) ----------------------------------------------------
SrQuery sr_query_gen(SrDevice *d);
void    sr_query_delete(SrDevice *d, SrQuery *q);
void    sr_query_begin(SrDevice *d, SrQuery q);
void    sr_query_end(SrDevice *d);
// Результат последнего завершённого запроса, миллисекунды.
float   sr_query_result_ms(SrDevice *d, SrQuery q);

// --- Чтение обратно -----------------------------------------------------------------------
// Прямоугольник текущего FBO -> out (нижний-левый origin, как в GL).
void sr_read_pixels(SrDevice *d, int x, int y, int w, int h,
                    SrPixFormat pix, void *out);

#endif
