// ============================================================================
//  gpu.h — абстракция рендер-бэкенда (RHI: render hardware interface)
// ----------------------------------------------------------------------------
//  Тонкая прослойка между оркестратором кадра и конкретным GPU API. Сейчас
//  единственный бэкенд — OpenGL 3.3 (gpu_gl.c), интерфейс спроектирован так,
//  чтобы подключать другие (GLES, Vulkan, SDL_GPU) без правки кода, который
//  рисует кадр. Всё, что Команда кадра делает с GPU, идёт сюда, а не напрямую.
//
//  Намеренно НЕ оборачивается всё подряд: униформ-локации/гей-шейдерную арт-
//  работу листовых модулей (terrain/water/sky и пр.) бэкенд-абстракция
//  поглотит поэтапно. Этот файл фиксирует СЕАМ: оркестрация кадра не знает,
//  какой настоящий API за спиной.
// ============================================================================
#ifndef GPU_H
#define GPU_H

// Непрозрачный девайс-бэкенд (глобальный, один на процесс)
typedef struct GpuDevice GpuDevice;

// Обработчики GPU-ресурсов (у каждого бэкенда свои "ручки" внутрь)
typedef unsigned int GpuTex;    // текстура (2D / кубомап)
typedef unsigned int GpuFbo;    // фреймбуфер
typedef unsigned int GpuProg;   // шейдерная программа
typedef unsigned int GpuBuf;    // вершинный/индексный буфер
typedef unsigned int GpuVao;    // вершинный массив (набор атрибутов)
typedef unsigned int GpuLoc;    // локация юниформа

// Примитив для рисования.
typedef enum {
    GPU_PRIM_LINES,
    GPU_PRIM_TRIANGLES,
} GpuPrim;

// Капсулы состояния GPU, которые оркестратор кадра трогает (не привязаны к
// именам конкретного API):
typedef enum {
    GPU_CAP_DEPTH_TEST,
    GPU_CAP_CULL_FACE,
    GPU_CAP_BLEND,
} GpuCap;

enum { GPU_CAP_COUNT = 3 };

// Факторы блендинга, независимые от имён конкретного API.
typedef enum {
    GPU_BLEND_SRC_ALPHA = 1,
    GPU_BLEND_ONE_MINUS_SRC_ALPHA = 2,
    GPU_BLEND_ZERO = 3,
    GPU_BLEND_ONE = 4,
} GpuBlendFac;

// --- Жизненный цикл ---
// Создаёт бэкенд (все GL-функции уже подгружены через gl_loader.h до вызова).
const GpuDevice *gpu_create(void);

// --- Пресет кадра (главный проход / FBO) ---
// Привязать фреймбуфер для отрисовки (0 = дефолтный/окно).
void gpu_bind_framebuffer(const GpuDevice *d, GpuFbo fbo);
// Установить прямоугольник вьюпорта текущего фреймбуфера.
void gpu_viewport(const GpuDevice *d, int x, int y, int w, int h);
// Очистить текущий фреймбуфер цветом и буфером глубины.
void gpu_clear(const GpuDevice *d, float r, float g, float b, float a);

// --- Состояние ---
void gpu_depth_mask(const GpuDevice *d, int write_enabled);
void gpu_cap(const GpuDevice *d, GpuCap cap, int on);
void gpu_blend_func(const GpuDevice *d, GpuBlendFac src, GpuBlendFac dst);
void gpu_wireframe(const GpuDevice *d, int on);   // каркасный режим (если есть)

// --- Текстуры ---
GpuTex gpu_tex_gen(const GpuDevice *d);
void gpu_tex_delete(const GpuDevice *d, GpuTex *t);
void gpu_bind_cubemap(const GpuDevice *d, GpuTex t);   // привязка как кубомап
void gpu_bind_2d(const GpuDevice *d, GpuTex t);        // привязка как 2D-текстуру
void gpu_cubemap_rgba8(const GpuDevice *d, GpuTex t, int size);
void gpu_cubemap_face_rgba8(const GpuDevice *d, GpuTex t, int face,
                            int w, int h, const void *pixels);   // abort=NULL
void gpu_cubemap_face_rgb8(const GpuDevice *d, GpuTex t, int face,
                           int w, int h, const void *pixels);
void gpu_cubemap_face_depth24(const GpuDevice *d, GpuTex t, int face, int size);
void gpu_tex_params_cubemap(const GpuDevice *d, GpuTex t, int linear_min);
void gpu_active_texture(const GpuDevice *d, int unit);

// --- Phреймбуферы (для FBO граней кубомапа и постобработки) ---
GpuFbo gpu_fbo_gen(const GpuDevice *d);
void gpu_fbo_delete(const GpuDevice *d, GpuFbo *fbo);
void gpu_fbo_attach_cubemap_color(const GpuDevice *d, GpuFbo fbo,
                                  GpuTex t, int face);
void gpu_fbo_attach_cubemap_depth(const GpuDevice *d, GpuFbo fbo,
                                  GpuTex t, int face);
int gpu_fbo_complete(const GpuDevice *d);                     // 1 — валиден
int gpu_fbo_current(const GpuDevice *d);                      // текущий привязка

// --- Чтение обратно (для cubemap_capture) ---
void gpu_read_rgb(const GpuDevice *d, int x, int y, int w, int h, void *out);

// --- Программы/шейдеры ---
GpuProg gpu_program_new(const GpuDevice *d, const char *vs_path, const char *fs_path);
void gpu_program_delete(const GpuDevice *d, GpuProg *prog);
void gpu_use_program(const GpuDevice *d, GpuProg prog);
GpuLoc gpu_program_loc(const GpuDevice *d, GpuProg prog, const char *name);

// --- Вершинные массивы и буферы ---
GpuVao gpu_vao_gen(const GpuDevice *d);
void gpu_vao_delete(const GpuDevice *d, GpuVao *vao);
void gpu_vao_bind(const GpuDevice *d, GpuVao vao);
GpuBuf gpu_buf_gen(const GpuDevice *d);
void gpu_buf_delete(const GpuDevice *d, GpuBuf *buf);
// Привязать буфер как GL_ARRAY_BUFFER (для загрузки и связывания атрибутов).
void gpu_buf_bind_array(const GpuDevice *d, GpuBuf buf);
void gpu_buf_upload_array(const GpuDevice *d, const void *data,
                          unsigned int bytes, int stream);
// Описать атрибут-вектор из 3 float (например позиции) по loc (0..) и включить.
void gpu_vertex_attr_3f(const GpuDevice *d, GpuLoc loc,
                        int stride_bytes, const void *offset);

// --- Юниформы (шейдер-арт через RHI, чтобы кадр не звал GL напрямую) ---
void gpu_uniform_mat4(const GpuDevice *d, GpuLoc loc, const float *m);
void gpu_uniform_float3(const GpuDevice *d, GpuLoc loc, float x, float y, float z);

// --- Рисование ---
void gpu_draw_arrays(const GpuDevice *d, GpuPrim prim, int vertex_count);

#endif