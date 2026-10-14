// ============================================================================
//  srender_metal.c — Metal backend scaffold for macOS (build: make metal)
// ----------------------------------------------------------------------------
//  SEED FRAMEWORK: selection scaffolding and stubs. The render pipeline is not
//  ported — the file exists so that (a) the macOS build exists architecturally,
//  (b) the full list of operations still to implement stays visible.
//
//  Two implementation paths (chosen later):
//    A) MoltenVK — the Vulkan backend (srender_vk.c) runs on Metal as is:
//       this file would not be needed at all, SDL_Vulkan_CreateSurface suffices.
//    B) Native Metal: shaders SPIR-V -> MSL (SPIRV-Cross), resources ->
//       MTLTexture/MTLBuffer, state -> MTLRenderPipelineState (hash-keyed
//       cache, as envisioned in srender.h), surface via SDL_Metal_CreateView.
//  Path B requires Objective-C for the Metal calls — the file is then renamed
//  to srender_metal.m or the Metal parts move into a separate .m module.
// ============================================================================
#if defined(FR_BACKEND_METAL) && defined(__APPLE__)

#include "srender.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct SrDevice {
    int      unused;      // the native Metal device appears on path B
    SrCaps   caps;
};

#define SR_MT_UNIMPL(op)                                                      \
    do {                                                                      \
        static int warned;                                                    \
        if (!warned) {                                                        \
            warned = 1;                                                       \
            SDL_Log("Metal backend: '%s' не портирован (заглушка)", op);      \
        }                                                                     \
    } while (0)

#define SR_MT_STUB_VOID(name)      void name(SrDevice *d) { (void)d; SR_MT_UNIMPL(#name); }
#define SR_MT_STUB_ZERO(ret, name) ret name(SrDevice *d) { (void)d; SR_MT_UNIMPL(#name); return 0; }

SrDevice *sr_create(void)
{
    // SDL provides a Metal view: SDL_Metal_CreateView(window) + SDL_Metal_GetLayer
    // -> CAMetalLayer; MTLCreateSystemDefaultDevice() — at step B.
    SrDevice *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    static char ver[64];
    snprintf(ver, sizeof ver, "Metal 2 (scaffold)");
    d->caps.gles = 0;
    d->caps.gl_major = 2;
    d->caps.gl_minor = 0;
    d->caps.compute = 1;              // MTLComputePipelineState
    d->caps.indirect = 1;             // MTLIndirectCommandBuffer / dispatch
    d->caps.base_instance = 1;        // baseInstance in drawInstanced
    d->caps.timer_query = 1;          // MTLCounterSampleBuffer (GPU family 4+)
    d->caps.tex3d = 1;
    d->caps.color_half_float = 1;     // MTLPixelFormatRGBA16Float renders
    d->caps.texture_norm16 = 1;
    d->caps.msaa_max = 8;
    d->caps.max_tex_size = 16384;
    d->caps.vendor = "Apple";
    d->caps.renderer = "Apple GPU";
    d->caps.version = ver;
    SDL_Log("Metal backend: каркас поднят (%s); рендер-пайплайн не портирован",
            ver);
    return d;
}

void sr_destroy(SrDevice *d) { free(d); }
const SrCaps *sr_caps(const SrDevice *d) { return d ? &d->caps : NULL; }

// --- Render operations: scaffold, implementation per roadmap -------------------
void sr_bind_fbo(SrDevice *d, SrFbo fbo) { (void)d; (void)fbo; SR_MT_UNIMPL("sr_bind_fbo"); }
void sr_viewport(SrDevice *d, int x, int y, int w, int h) { (void)d; (void)x; (void)y; (void)w; (void)h; SR_MT_UNIMPL("sr_viewport"); }
void sr_clear(SrDevice *d, float r, float g, float b, float a, int cc, int cd) { (void)d; (void)r; (void)g; (void)b; (void)a; (void)cc; (void)cd; SR_MT_UNIMPL("sr_clear"); }
int  sr_fbo_current(SrDevice *d) { (void)d; return 0; }
int  sr_fbo_complete(SrDevice *d) { (void)d; return 0; }
void sr_cap(SrDevice *d, SrStateCap cap, int on) { (void)d; (void)cap; (void)on; SR_MT_UNIMPL("sr_cap"); }
void sr_depth_mask(SrDevice *d, int w) { (void)d; (void)w; SR_MT_UNIMPL("sr_depth_mask"); }
void sr_blend_func(SrDevice *d, SrBlendFac s, SrBlendFac t) { (void)d; (void)s; (void)t; SR_MT_UNIMPL("sr_blend_func"); }
void sr_cull_front(SrDevice *d, int ccw) { (void)d; (void)ccw; SR_MT_UNIMPL("sr_cull_front"); }
void sr_wireframe(SrDevice *d, int on) { (void)d; (void)on; SR_MT_UNIMPL("sr_wireframe"); }

SrTex sr_tex_gen(SrDevice *d) { (void)d; SR_MT_UNIMPL("sr_tex_gen"); return 0; }
void sr_tex_delete(SrDevice *d, SrTex *t) { (void)d; (void)t; *t = 0; SR_MT_UNIMPL("sr_tex_delete"); }
void sr_tex_active(SrDevice *d, int u) { (void)d; (void)u; SR_MT_UNIMPL("sr_tex_active"); }
void sr_tex_bind(SrDevice *d, SrTexTarget tg, SrTex t) { (void)d; (void)tg; (void)t; SR_MT_UNIMPL("sr_tex_bind"); }
void sr_tex_image_2d(SrDevice *d, SrTex t, int face, SrFormat f, int w, int h, SrPixFormat p, const void *data) { (void)d; (void)t; (void)face; (void)f; (void)w; (void)h; (void)p; (void)data; SR_MT_UNIMPL("sr_tex_image_2d"); }
void sr_tex_image_3d(SrDevice *d, SrTex t, SrFormat f, int w, int h, int dep, SrPixFormat p, const void *data) { (void)d; (void)t; (void)f; (void)w; (void)h; (void)dep; (void)p; (void)data; SR_MT_UNIMPL("sr_tex_image_3d"); }
void sr_tex_sub_image_3d(SrDevice *d, SrTex t, int x, int y, int z, int w, int h, int dep, SrPixFormat p, const void *data) { (void)d; (void)t; (void)x; (void)y; (void)z; (void)w; (void)h; (void)dep; (void)p; (void)data; SR_MT_UNIMPL("sr_tex_sub_image_3d"); }
void sr_tex_params(SrDevice *d, SrTex t, SrTexTarget tg, SrFilter mn, SrFilter mg, SrWrap ws, SrWrap wt, SrWrap wr) { (void)d; (void)t; (void)tg; (void)mn; (void)mg; (void)ws; (void)wt; (void)wr; SR_MT_UNIMPL("sr_tex_params"); }
void sr_tex_generate_mipmap(SrDevice *d, SrTex t, SrTexTarget tg) { (void)d; (void)t; (void)tg; SR_MT_UNIMPL("sr_tex_generate_mipmap"); }

SrRb sr_rb_gen(SrDevice *d) { (void)d; SR_MT_UNIMPL("sr_rb_gen"); return 0; }
void sr_rb_delete(SrDevice *d, SrRb *rb) { (void)d; (void)rb; *rb = 0; SR_MT_UNIMPL("sr_rb_delete"); }
void sr_rb_storage(SrDevice *d, SrRb rb, SrFormat f, int samples, int w, int h) { (void)d; (void)rb; (void)f; (void)samples; (void)w; (void)h; SR_MT_UNIMPL("sr_rb_storage"); }
SrFbo sr_fbo_gen(SrDevice *d) { (void)d; SR_MT_UNIMPL("sr_fbo_gen"); return 0; }
void sr_fbo_delete(SrDevice *d, SrFbo *f) { (void)d; (void)f; *f = 0; SR_MT_UNIMPL("sr_fbo_delete"); }
void sr_fbo_attach_tex(SrDevice *d, SrFbo f, int att, SrTexTarget tg, SrTex t, int face) { (void)d; (void)f; (void)att; (void)tg; (void)t; (void)face; SR_MT_UNIMPL("sr_fbo_attach_tex"); }
void sr_fbo_attach_rb(SrDevice *d, SrFbo f, int att, SrRb rb) { (void)d; (void)f; (void)att; (void)rb; SR_MT_UNIMPL("sr_fbo_attach_rb"); }
void sr_blit(SrDevice *d, SrFbo src, SrFbo dst, int w, int h, int lin, int cdep) { (void)d; (void)src; (void)dst; (void)w; (void)h; (void)lin; (void)cdep; SR_MT_UNIMPL("sr_blit"); }

SrProg sr_prog_new(SrDevice *d, const char *vs, const char *fs) { (void)d; (void)vs; (void)fs; SR_MT_UNIMPL("sr_prog_new"); return 0; }
SrProg sr_prog_compute_new(SrDevice *d, const char *cs) { (void)d; (void)cs; SR_MT_UNIMPL("sr_prog_compute_new"); return 0; }
void sr_prog_delete(SrDevice *d, SrProg *p) { (void)d; (void)p; *p = 0; SR_MT_UNIMPL("sr_prog_delete"); }
void sr_prog_use(SrDevice *d, SrProg p) { (void)d; (void)p; SR_MT_UNIMPL("sr_prog_use"); }
SrLoc sr_prog_loc(SrDevice *d, SrProg p, const char *n) { (void)d; (void)p; (void)n; return -1; }

void sr_uniform_int(SrDevice *d, SrLoc l, int v) { (void)d; (void)l; (void)v; }
void sr_uniform_float(SrDevice *d, SrLoc l, float v) { (void)d; (void)l; (void)v; }
void sr_uniform_float2(SrDevice *d, SrLoc l, float x, float y) { (void)d; (void)l; (void)x; (void)y; }
void sr_uniform_float3(SrDevice *d, SrLoc l, float x, float y, float z) { (void)d; (void)l; (void)x; (void)y; (void)z; }
void sr_uniform_float4(SrDevice *d, SrLoc l, float x, float y, float z, float w) { (void)d; (void)l; (void)x; (void)y; (void)z; (void)w; }
void sr_uniform_float1v(SrDevice *d, SrLoc l, int n, const float *v) { (void)d; (void)l; (void)n; (void)v; }
void sr_uniform_float3v(SrDevice *d, SrLoc l, int n, const float *v) { (void)d; (void)l; (void)n; (void)v; }
void sr_uniform_float4v(SrDevice *d, SrLoc l, int n, const float *v) { (void)d; (void)l; (void)n; (void)v; }
void sr_uniform_mat4(SrDevice *d, SrLoc l, const float *m) { (void)d; (void)l; (void)m; }

SrBuf sr_buf_gen(SrDevice *d) { (void)d; SR_MT_UNIMPL("sr_buf_gen"); return 0; }
void sr_buf_delete(SrDevice *d, SrBuf *b) { (void)d; (void)b; *b = 0; SR_MT_UNIMPL("sr_buf_delete"); }
void sr_buf_bind(SrDevice *d, SrBufTarget tg, SrBuf b) { (void)d; (void)tg; (void)b; SR_MT_UNIMPL("sr_buf_bind"); }
void sr_buf_upload(SrDevice *d, SrBufTarget tg, const void *data, unsigned int bytes, int st) { (void)d; (void)tg; (void)data; (void)bytes; (void)st; SR_MT_UNIMPL("sr_buf_upload"); }
void sr_buf_sub_upload(SrDevice *d, SrBufTarget tg, unsigned int off, const void *data, unsigned int bytes) { (void)d; (void)tg; (void)off; (void)data; (void)bytes; SR_MT_UNIMPL("sr_buf_sub_upload"); }
void sr_buf_bind_ssbo(SrDevice *d, SrBuf b, int binding) { (void)d; (void)b; (void)binding; SR_MT_UNIMPL("sr_buf_bind_ssbo"); }

SrVao sr_vao_gen(SrDevice *d) { (void)d; SR_MT_UNIMPL("sr_vao_gen"); return 0; }
void sr_vao_delete(SrDevice *d, SrVao *v) { (void)d; (void)v; *v = 0; SR_MT_UNIMPL("sr_vao_delete"); }
void sr_vao_bind(SrDevice *d, SrVao v) { (void)d; (void)v; SR_MT_UNIMPL("sr_vao_bind"); }
void sr_attrib_float(SrDevice *d, SrLoc l, int comps, int stride, unsigned int off, int div) { (void)d; (void)l; (void)comps; (void)stride; (void)off; (void)div; SR_MT_UNIMPL("sr_attrib_float"); }

void sr_draw_arrays(SrDevice *d, SrPrim p, int f, int c) { (void)d; (void)p; (void)f; (void)c; SR_MT_UNIMPL("sr_draw_arrays"); }
void sr_draw_elements(SrDevice *d, SrPrim p, int c, int u32, unsigned int off) { (void)d; (void)p; (void)c; (void)u32; (void)off; SR_MT_UNIMPL("sr_draw_elements"); }
void sr_draw_arrays_instanced(SrDevice *d, SrPrim p, int f, int c, int inst) { (void)d; (void)p; (void)f; (void)c; (void)inst; SR_MT_UNIMPL("sr_draw_arrays_instanced"); }
void sr_draw_elements_instanced(SrDevice *d, SrPrim p, int c, int u32, unsigned int off, int inst) { (void)d; (void)p; (void)c; (void)u32; (void)off; (void)inst; SR_MT_UNIMPL("sr_draw_elements_instanced"); }
void sr_draw_elements_instanced_base(SrDevice *d, SrPrim p, int c, int u32, unsigned int off, int inst, unsigned int base) { (void)d; (void)p; (void)c; (void)u32; (void)off; (void)inst; (void)base; SR_MT_UNIMPL("sr_draw_elements_instanced_base"); }
void sr_draw_elements_indirect(SrDevice *d, SrPrim p, SrBuf cb, unsigned int off) { (void)d; (void)p; (void)cb; (void)off; SR_MT_UNIMPL("sr_draw_elements_indirect"); }
void sr_dispatch(SrDevice *d, int x, int y, int z) { (void)d; (void)x; (void)y; (void)z; SR_MT_UNIMPL("sr_dispatch"); }
void sr_barrier(SrDevice *d, unsigned int bits) { (void)d; (void)bits; SR_MT_UNIMPL("sr_barrier"); }

SrQuery sr_query_gen(SrDevice *d) { (void)d; SR_MT_UNIMPL("sr_query_gen"); return 0; }
void sr_query_delete(SrDevice *d, SrQuery *q) { (void)d; (void)q; *q = 0; SR_MT_UNIMPL("sr_query_delete"); }
void sr_query_begin(SrDevice *d, SrQuery q) { (void)d; (void)q; SR_MT_UNIMPL("sr_query_begin"); }
void sr_query_end(SrDevice *d) { (void)d; SR_MT_UNIMPL("sr_query_end"); }
float sr_query_result_ms(SrDevice *d, SrQuery q) { (void)d; (void)q; return 0.0f; }

void sr_read_pixels(SrDevice *d, int x, int y, int w, int h, SrPixFormat pix, void *out)
{ (void)d; (void)x; (void)y; (void)w; (void)h; (void)pix; (void)out; SR_MT_UNIMPL("sr_read_pixels"); }

typedef int sr_scaffold_marker;

#else
// Not macOS or not a Metal build — the file is empty.
typedef int sr_metal_disabled_marker;
#endif
