// ============================================================================
//  gpu.h — render backend abstraction (RHI: render hardware interface)
// ----------------------------------------------------------------------------
//  A thin layer between the frame orchestrator and a concrete GPU API. Right
//  now the only backend is OpenGL 3.3 (gpu_gl.c); the interface is designed so
//  others (GLES, Vulkan, SDL_GPU) can be plugged in without touching the code
//  that draws the frame. Everything the frame does to the GPU goes through
//  here, not directly.
//
//  Deliberately does NOT wrap everything: uniform-location/gl-shader-art work
//  of leaf modules (terrain/water/sky etc.) will be absorbed by the backend
//  abstraction incrementally. This file pins down the SEAM: frame orchestration
//  does not know which real API is behind it.
// ============================================================================
#ifndef GPU_H
#define GPU_H

// Opaque per-backend device (global, one per process)
typedef struct GpuDevice GpuDevice;

// GPU resource handles (each backend has its own "handles" inside)
typedef unsigned int GpuTex;    // texture (2D / cubemap)
typedef unsigned int GpuFbo;    // framebuffer
typedef unsigned int GpuProg;   // shader program
typedef unsigned int GpuBuf;    // vertex/index buffer
typedef unsigned int GpuVao;    // vertex array (attribute set)
typedef unsigned int GpuLoc;    // uniform location

// Primitive to draw.
typedef enum {
    GPU_PRIM_LINES,
    GPU_PRIM_TRIANGLES,
} GpuPrim;

// GPU state capsules that the frame orchestrator touches (not tied to
// any specific API's names):
typedef enum {
    GPU_CAP_DEPTH_TEST,
    GPU_CAP_CULL_FACE,
    GPU_CAP_BLEND,
} GpuCap;

enum { GPU_CAP_COUNT = 3 };

// Blend factors, independent of any specific API's names.
typedef enum {
    GPU_BLEND_SRC_ALPHA = 1,
    GPU_BLEND_ONE_MINUS_SRC_ALPHA = 2,
    GPU_BLEND_ZERO = 3,
    GPU_BLEND_ONE = 4,
} GpuBlendFac;

// --- Lifecycle ---
// Creates the backend (all GL functions are already loaded via gl_loader.h before the call).
const GpuDevice *gpu_create(void);

// --- Frame preset (main pass / FBO) ---
// Bind a framebuffer for rendering (0 = default/window).
void gpu_bind_framebuffer(const GpuDevice *d, GpuFbo fbo);
// Set the viewport rectangle of the current framebuffer.
void gpu_viewport(const GpuDevice *d, int x, int y, int w, int h);
// Clear the current framebuffer with color and depth buffer.
void gpu_clear(const GpuDevice *d, float r, float g, float b, float a);

// --- State ---
void gpu_depth_mask(const GpuDevice *d, int write_enabled);
void gpu_cap(const GpuDevice *d, GpuCap cap, int on);
void gpu_blend_func(const GpuDevice *d, GpuBlendFac src, GpuBlendFac dst);
void gpu_wireframe(const GpuDevice *d, int on);   // wireframe mode (if available)

// --- Textures ---
GpuTex gpu_tex_gen(const GpuDevice *d);
void gpu_tex_delete(const GpuDevice *d, GpuTex *t);
void gpu_bind_cubemap(const GpuDevice *d, GpuTex t);   // bind as cubemap
void gpu_bind_2d(const GpuDevice *d, GpuTex t);        // bind as 2D texture
void gpu_cubemap_rgba8(const GpuDevice *d, GpuTex t, int size);
void gpu_cubemap_face_rgba8(const GpuDevice *d, GpuTex t, int face,
                            int w, int h, const void *pixels);   // abort=NULL
void gpu_cubemap_face_rgb8(const GpuDevice *d, GpuTex t, int face,
                           int w, int h, const void *pixels);
void gpu_cubemap_face_depth24(const GpuDevice *d, GpuTex t, int face, int size);
void gpu_tex_params_cubemap(const GpuDevice *d, GpuTex t, int linear_min);
void gpu_active_texture(const GpuDevice *d, int unit);

// --- Framebuffers (for cubemap-face FBOs and postprocessing) ---
GpuFbo gpu_fbo_gen(const GpuDevice *d);
void gpu_fbo_delete(const GpuDevice *d, GpuFbo *fbo);
void gpu_fbo_attach_cubemap_color(const GpuDevice *d, GpuFbo fbo,
                                  GpuTex t, int face);
void gpu_fbo_attach_cubemap_depth(const GpuDevice *d, GpuFbo fbo,
                                  GpuTex t, int face);
int gpu_fbo_complete(const GpuDevice *d);                     // 1 — valid
int gpu_fbo_current(const GpuDevice *d);                      // currently bound

// --- Readback (for cubemap_capture) ---
void gpu_read_rgb(const GpuDevice *d, int x, int y, int w, int h, void *out);

// --- Programs/shaders ---
GpuProg gpu_program_new(const GpuDevice *d, const char *vs_path, const char *fs_path);
void gpu_program_delete(const GpuDevice *d, GpuProg *prog);
void gpu_use_program(const GpuDevice *d, GpuProg prog);
GpuLoc gpu_program_loc(const GpuDevice *d, GpuProg prog, const char *name);

// --- Vertex arrays and buffers ---
GpuVao gpu_vao_gen(const GpuDevice *d);
void gpu_vao_delete(const GpuDevice *d, GpuVao *vao);
void gpu_vao_bind(const GpuDevice *d, GpuVao vao);
GpuBuf gpu_buf_gen(const GpuDevice *d);
void gpu_buf_delete(const GpuDevice *d, GpuBuf *buf);
// Bind a buffer as GL_ARRAY_BUFFER (for uploading and setting up attributes).
void gpu_buf_bind_array(const GpuDevice *d, GpuBuf buf);
void gpu_buf_upload_array(const GpuDevice *d, const void *data,
                          unsigned int bytes, int stream);
// Describe a 3-float attribute vector (e.g. positions) at loc (0..) and enable it.
void gpu_vertex_attr_3f(const GpuDevice *d, GpuLoc loc,
                        int stride_bytes, const void *offset);

// --- Uniforms (shader art via RHI, so the frame never calls GL directly) ---
void gpu_uniform_mat4(const GpuDevice *d, GpuLoc loc, const float *m);
void gpu_uniform_float3(const GpuDevice *d, GpuLoc loc, float x, float y, float z);

// --- Drawing ---
void gpu_draw_arrays(const GpuDevice *d, GpuPrim prim, int vertex_count);

#endif