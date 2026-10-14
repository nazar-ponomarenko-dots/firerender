// ============================================================================
//  srender.h — SIMPLE RENDER: the engine's render API (driver-independent)
// ----------------------------------------------------------------------------
//  The single point through which engine code must talk to the GPU.
//  The driver (OpenGL / OpenGL ES today, Vulkan / Metal in the future) sits
//  behind it and implements this interface: for now — srender_gl.c. Direct
//  gl* calls in engine modules are legacy; migration to sr_* goes step by step.
//
//  Design rules (so that Vulkan/Metal can land without rewriting the API):
//    1. No GL types/constants in this header — only our own enums.
//    2. Context capabilities are read from sr_caps(), not through ifdef:
//       the driver itself decides how to cover a feature (fallback inside the
//       backend).
//    3. The state model is command-based (cap on/off, bind, draw), as in
//       GL/GLES — a backend with a different model (Vulkan) will have to cache
//       state itself; the engine does not suffer from that.
//    4. Resources are opaque handles (unsigned), not GL names.
// ============================================================================
#ifndef SRENDER_H
#define SRENDER_H

// --- Resource handles (opaque; 0 = "no resource") ----------------------
typedef unsigned int SrTex;      // texture (2D / 3D / cubemap)
typedef unsigned int SrFbo;      // framebuffer (render target)
typedef unsigned int SrRb;       // renderbuffer (depth/MSAA)
typedef unsigned int SrBuf;      // vertex/index/SSBO buffer
typedef unsigned int SrVao;      // vertex attribute set
typedef unsigned int SrProg;     // shader program
typedef int          SrLoc;      // uniform location (-1 = none)
typedef unsigned int SrQuery;    // GPU timer

// --- Device: one per process, created after context initialization -------
typedef struct SrDevice SrDevice;

// Context capabilities: the driver fills this in at sr_create().
typedef struct {
    int gles;            // 1 = OpenGL ES, 0 = desktop OpenGL
    int gl_major, gl_minor;
    int compute;         // dispatch + SSBO (GL 4.3 / GLES 3.1)
    int indirect;        // draw indirect (+ multi)
    int base_instance;   // baseinstance in instanced draws
    int timer_query;     // GPU timers (GL 3.3 / GLES with EXT_disjoint_timer_query)
    int tex3d;           // 3D textures
    int color_half_float;// RGBA16F can be used as a render target
    int texture_norm16;  // normalized R16/RG16 textures
    int msaa_max;        // max number of MSAA samples (0 = none)
    int max_tex_size;    // max texture dimension, px
    const char *vendor, *renderer, *version;
} SrCaps;

// --- Texture targets and formats --------------------------------------------------
typedef enum {
    SR_TEX_2D,
    SR_TEX_3D,
    SR_TEX_CUBE,
} SrTexTarget;

// Internal storage format (the GL mapping lives in the backend).
typedef enum {
    SR_FMT_RGBA8,        // LDR scene, HUD atlases, sprites
    SR_FMT_RGB8,
    SR_FMT_R8,           // masks (snow, decals)
    SR_FMT_R16,          // heights/fields (16-bit integers)
    SR_FMT_RG8,
    SR_FMT_RGBA16F,      // HDR scene (bloom, GI, SSR)
    SR_FMT_DEPTH24,      // shadow maps, depth-only passes
} SrFormat;

// Pixel layout when loading from the CPU (what data contains).
typedef enum {
    SR_PIX_RGBA,         // 4 x unsigned byte
    SR_PIX_RGB,          // 3 x unsigned byte
    SR_PIX_RG,           // 2 x unsigned byte
    SR_PIX_RED,          // 1 x unsigned byte
    SR_PIX_DEPTH_UINT,   // unsigned int (depth)
} SrPixFormat;

// Filters and wrapping.
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

// --- Buffers and drawing ------------------------------------------------------
typedef enum {
    SR_BUF_ARRAY,        // vertex data
    SR_BUF_ELEMENT,      // indices
    SR_BUF_UNIFORM,
    SR_BUF_SSBO,         // shader storage
    SR_BUF_INDIRECT,     // indirect draw commands
} SrBufTarget;

typedef enum {
    SR_PRIM_LINES,
    SR_PRIM_TRIANGLES,
    SR_PRIM_TRIANGLE_STRIP,
    SR_PRIM_POINTS,
} SrPrim;

// Memory barrier bits after compute writes (combined with |).
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

// --- State -----------------------------------------------------------------
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

// --- Render APIs (backends) ---------------------------------------------------
// The engine works through sr_* — the backends themselves plug in behind it.
// Today GL/GLES is implemented (srender_gl.c); Vulkan and Metal are stubs: the
// selection scaffolding and probes exist, implementation is a staged migration
// (see the ROADMAP in the header).
//
// Plan for hooking up a new API (e.g. Vulkan):
//   1. src/srender_vk.c implements the same sr_* functions on top of
//      VkInstance/VkDevice (or reuses the dispatcher from srender_gl.c if we
//      keep the GL naming of states).
//   2. GLSL 330 shaders get compiled to SPIR-V (glslc/glslangValidator,
//      target: --target-env=vulkan1.1); Metal is translated by the MSL backend
//      SPIRV-Cross (or MoltenVK, which consumes SPIR-V directly).
//   3. The window surface — SDL_Vulkan_CreateSurface / SDL_Metal_CreateView.
//   4. main.c picks the API via sr_api_available() + cvar r_backend.
typedef enum {
    SR_API_GL = 0,       // desktop OpenGL (today: the main backend)
    SR_API_GLES,         // OpenGL ES 3.x (today: low-end/Android profile)
    SR_API_VULKAN,       // Vulkan 1.1+ (work in progress)
    SR_API_METAL,        // Metal 2 via MSL or MoltenVK (work in progress)
    SR_API_NULL,         // no-op stub (headless servers, logic tests)
    SR_API_COUNT
} SrApi;

// Human-readable name ("OpenGL", "Vulkan", ...).
const char *sr_api_name(SrApi api);
// 1 = the API is compiled into the engine and can be selected. It also takes
// the platform into account (Metal — __APPLE__ only, etc.).
int  sr_api_available(SrApi api);
// SDL_Log report on all APIs (main.c calls it at startup).
void sr_api_report(void);

// --- Lifecycle ---------------------------------------------------------------
// Create the device over the CURRENT context (GL functions already loaded).
// Future: SrDevice *sr_create_api(SrApi api, void *native_surface) —
// a single factory for all backends; for now GL/GLES is created here.
SrDevice *sr_create(void);
void      sr_destroy(SrDevice *d);
const SrCaps *sr_caps(const SrDevice *d);

// --- Frame -------------------------------------------------------------------
void sr_bind_fbo(SrDevice *d, SrFbo fbo);          // 0 = window/screen
void sr_viewport(SrDevice *d, int x, int y, int w, int h);
void sr_clear(SrDevice *d, float r, float g, float b, float a,
              int clear_color, int clear_depth);
int  sr_fbo_current(SrDevice *d);                  // currently bound one
int  sr_fbo_complete(SrDevice *d);                 // is the current FBO valid

// --- Pipeline state ----------------------------------------------------------
void sr_cap(SrDevice *d, SrStateCap cap, int on);
void sr_depth_mask(SrDevice *d, int write_enabled);
void sr_blend_func(SrDevice *d, SrBlendFac src, SrBlendFac dst);
void sr_cull_front(SrDevice *d, int front_ccw_is_front); // front-face orientation
void sr_wireframe(SrDevice *d, int on);            // may be a no-op (GLES)

// --- Textures ----------------------------------------------------------------
SrTex sr_tex_gen(SrDevice *d);
void  sr_tex_delete(SrDevice *d, SrTex *t);
void  sr_tex_active(SrDevice *d, int unit);
void  sr_tex_bind(SrDevice *d, SrTexTarget target, SrTex t);
// Allocate/load level 0 (data == NULL — allocate only).
// For a cubemap face = 0..5 (+X..-Z), for ordinary textures face = -1.
void  sr_tex_image_2d(SrDevice *d, SrTex t, int face, SrFormat fmt,
                      int w, int h, SrPixFormat pix, const void *data);
void  sr_tex_image_3d(SrDevice *d, SrTex t, SrFormat fmt,
                      int w, int h, int depth, SrPixFormat pix,
                      const void *data);
// Update a range of a 3D texture (layers z..z+depth).
void  sr_tex_sub_image_3d(SrDevice *d, SrTex t, int x, int y, int z,
                          int w, int h, int depth, SrPixFormat pix,
                          const void *data);
void  sr_tex_params(SrDevice *d, SrTex t, SrTexTarget target,
                    SrFilter min_f, SrFilter mag_f,
                    SrWrap wrap_s, SrWrap wrap_t, SrWrap wrap_r);
void  sr_tex_generate_mipmap(SrDevice *d, SrTex t, SrTexTarget target);

// --- Renderbuffers and FBO ---------------------------------------------------
SrRb  sr_rb_gen(SrDevice *d);
void  sr_rb_delete(SrDevice *d, SrRb *rb);
// Renderbuffer storage; samples > 1 — MSAA (if the backend supports it).
void  sr_rb_storage(SrDevice *d, SrRb rb, SrFormat fmt, int samples,
                    int w, int h);
SrFbo sr_fbo_gen(SrDevice *d);
void  sr_fbo_delete(SrDevice *d, SrFbo *fbo);
// Attach a texture to a point: attachment 0 = color0, -1 = depth.
// For a cubemap face = 0..5 (a specific side), for the rest face = -1.
void  sr_fbo_attach_tex(SrDevice *d, SrFbo fbo, int attachment,
                        SrTexTarget target, SrTex t, int face);
void  sr_fbo_attach_rb(SrDevice *d, SrFbo fbo, int attachment, SrRb rb);
// Copy a rectangle src -> dst (size in pixels, filter for color).
void  sr_blit(SrDevice *d, SrFbo src, SrFbo dst,
              int w, int h, int linear, int copy_depth);

// --- Shader programs ------------------------------------------------------------
// vs/fs/cs — paths inside shaders/ (compilation with the platform-specific
// GLSL version prefix is done by the driver; the program cache is internal).
SrProg sr_prog_new(SrDevice *d, const char *vs_path, const char *fs_path);
SrProg sr_prog_compute_new(SrDevice *d, const char *cs_path);
void   sr_prog_delete(SrDevice *d, SrProg *prog);
void   sr_prog_use(SrDevice *d, SrProg prog);
SrLoc  sr_prog_loc(SrDevice *d, SrProg prog, const char *name);

// --- Uniforms ----------------------------------------------------------------
void sr_uniform_int(SrDevice *d, SrLoc loc, int v);
void sr_uniform_float(SrDevice *d, SrLoc loc, float v);
void sr_uniform_float2(SrDevice *d, SrLoc loc, float x, float y);
void sr_uniform_float3(SrDevice *d, SrLoc loc, float x, float y, float z);
void sr_uniform_float4(SrDevice *d, SrLoc loc, float x, float y, float z, float w);
void sr_uniform_float1v(SrDevice *d, SrLoc loc, int n, const float *v);
void sr_uniform_float3v(SrDevice *d, SrLoc loc, int n, const float *v);
void sr_uniform_float4v(SrDevice *d, SrLoc loc, int n, const float *v);
void sr_uniform_mat4(SrDevice *d, SrLoc loc, const float *m);   // column-major

// --- Buffers and vertex attributes ----------------------------------------------------
SrBuf sr_buf_gen(SrDevice *d);
void  sr_buf_delete(SrDevice *d, SrBuf *buf);
void  sr_buf_bind(SrDevice *d, SrBufTarget target, SrBuf buf);
// usage: 1 = static (uploaded once), 0 = stream (re-uploaded every frame).
void  sr_buf_upload(SrDevice *d, SrBufTarget target,
                    const void *data, unsigned int bytes, int static_use);
void  sr_buf_sub_upload(SrDevice *d, SrBufTarget target, unsigned int offset,
                        const void *data, unsigned int bytes);
// SSBO to a binding point (requires caps->compute).
void  sr_buf_bind_ssbo(SrDevice *d, SrBuf buf, int binding);

SrVao sr_vao_gen(SrDevice *d);
void  sr_vao_delete(SrDevice *d, SrVao *vao);
void  sr_vao_bind(SrDevice *d, SrVao vao);
// Attribute from float vectors of comps (1..4); divisor > 0 — instanced.
void  sr_attrib_float(SrDevice *d, SrLoc loc, int comps, int stride_bytes,
                      unsigned int offset, int divisor);

// --- Draw ----------------------------------------------------------------------------
void sr_draw_arrays(SrDevice *d, SrPrim prim, int first, int count);
// type: indices — uint16 (0) or uint32 (1).
void sr_draw_elements(SrDevice *d, SrPrim prim, int count, int indices_u32,
                      unsigned int byte_offset);
void sr_draw_arrays_instanced(SrDevice *d, SrPrim prim, int first, int count,
                              int instances);
void sr_draw_elements_instanced(SrDevice *d, SrPrim prim, int count,
                                int indices_u32, unsigned int byte_offset,
                                int instances);
// baseinstance: if the backend cannot do it (caps->base_instance == 0), it
// works only with base == 0 — the caller must take that into account.
void sr_draw_elements_instanced_base(SrDevice *d, SrPrim prim, int count,
                                     int indices_u32, unsigned int byte_offset,
                                     int instances, unsigned int base);
// Indirect: the SR_BUF_INDIRECT buffer holds a draw elements command.
void sr_draw_elements_indirect(SrDevice *d, SrPrim prim,
                               SrBuf cmd_buf, unsigned int byte_offset);

// --- Compute --------------------------------------------------------------------------
void sr_dispatch(SrDevice *d, int x, int y, int z);
void sr_barrier(SrDevice *d, unsigned int barrier_bits);

// --- GPU timers (caps->timer_query) ----------------------------------------------------
SrQuery sr_query_gen(SrDevice *d);
void    sr_query_delete(SrDevice *d, SrQuery *q);
void    sr_query_begin(SrDevice *d, SrQuery q);
void    sr_query_end(SrDevice *d);
// Result of the last completed query, milliseconds.
float   sr_query_result_ms(SrDevice *d, SrQuery q);

// --- Readback -----------------------------------------------------------------------
// A rectangle of the current FBO -> out (bottom-left origin, like in GL).
void sr_read_pixels(SrDevice *d, int x, int y, int w, int h,
                    SrPixFormat pix, void *out);

#endif
