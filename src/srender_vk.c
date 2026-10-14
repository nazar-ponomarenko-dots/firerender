// ============================================================================
//  srender_vk.c — каркас Vulkan-бэкенда (сборка: make vulkan)
// ----------------------------------------------------------------------------
//  ЧЕСТНЫЙ зачаток: sr_create() реально поднимает Vulkan — грузит библиотеку
//  через SDL (никакого Vulkan SDK при сборке НЕ нужно: типы/прототипы
//  минимально объявлены здесь), создаёт VkInstance, выбирает физическое
//  устройство с graphics-очередью, создаёт логическое устройство. sr_caps()
//  отдаёт возможности. Всё остальное (текстуры/FBO/шейдеры/draw) — заглушки
//  с однократным предупреждением: рендер-пайплайн на Vulkan не портирован.
//
//  Дорожная карта (порядок портирования):
//    1. SDL_Vulkan_CreateSurface + swapchain (present), барьеры кадров.
//    2. Текстуры/FBO -> VkImage/VkImageView/VkFramebuffer + render pass.
//    3. GLSL -> SPIR-V (glslc --target-env=vulkan1.1) + pipeline cache.
//    4. Draw: команды в VkCommandBuffer, состояние ->VkPipeline по хешу.
//    5. Compute/SSBO/indirect — прямой маппинг на Vulkan-примитивы.
//  Компилируется как: $(CC) -DFR_BACKEND_VULKAN -DSR_ENABLE_VULKAN=1
//  (srender_gl.c из сборки исключён).
// ============================================================================
#if defined(FR_BACKEND_VULKAN)

#include "srender.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Настоящие заголовки Vulkan, если стоят (предпочтительно); иначе
// минимальные объявления ниже — сборка работает и без Vulkan SDK.
#if defined(__has_include)
#  if __has_include(<vulkan/vulkan.h>)
#    include <vulkan/vulkan.h>
#    define SR_VK_REAL_HEADERS 1
#  endif
#endif

#ifndef SR_VK_REAL_HEADERS
// --- Минимальные Vulkan-типы (/layouts по спеке v1.1) ------------------------
typedef void    *VkInstance, *VkPhysicalDevice, *VkDevice, *VkQueue;
typedef int32_t  VkResult;


#define VK_SUCCESS                       0
#define VK_STRUCTURE_TYPE_APPLICATION_INFO        0
#define VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO    1
#define VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO 2
#define VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO      3
#define VK_QUEUE_GRAPHICS_BIT            0x1
#define VK_QUEUE_COMPUTE_BIT             0x2
#define VK_API_VERSION_1_1  ((1u << 22) | (1u << 12))

typedef struct VkApplicationInfo {
    int sType; const void *pNext;
    const char *pApplicationName; uint32_t applicationVersion;
    const char *pEngineName; uint32_t engineVersion; uint32_t apiVersion;
} VkApplicationInfo;

typedef struct VkInstanceCreateInfo {
    int sType; const void *pNext; uint32_t flags;
    const VkApplicationInfo *pApplicationInfo;
    uint32_t enabledLayerCount; const char *const *ppEnabledLayerNames;
    uint32_t enabledExtensionCount; const char *const *ppEnabledExtensionNames;
} VkInstanceCreateInfo;

typedef struct VkQueueFamilyProperties {
    uint32_t queueFlags, queueCount, timestampValidBits;
    int32_t minImageTransferGranularity[3];
} VkQueueFamilyProperties;

typedef struct VkDeviceQueueCreateInfo {
    int sType; const void *pNext; uint32_t flags;
    uint32_t queueFamilyIndex, queueCount; const float *pQueuePriorities;
} VkDeviceQueueCreateInfo;

typedef struct VkDeviceCreateInfo {
    int sType; const void *pNext; uint32_t flags;
    uint32_t queueCreateInfoCount; const VkDeviceQueueCreateInfo *pQueueCreateInfos;
    uint32_t enabledLayerCount; const char *const *ppEnabledLayerNames;
    uint32_t enabledExtensionCount; const char *const *ppEnabledExtensionNames;
    const void *pEnabledFeatures;
} VkDeviceCreateInfo;

typedef VkResult (*PFN_vkCreateInstance)(const VkInstanceCreateInfo *,
                                         const void *, VkInstance *);
typedef VkResult (*PFN_vkEnumeratePhysicalDevices)(VkInstance, uint32_t *,
                                                   VkPhysicalDevice *);
typedef void (*PFN_vkGetPhysicalDeviceQueueFamilyProperties)(VkPhysicalDevice,
                                                             uint32_t *,
                                                             VkQueueFamilyProperties *);
typedef VkResult (*PFN_vkCreateDevice)(VkPhysicalDevice,
                                       const VkDeviceCreateInfo *,
                                       const void *, VkDevice *);
typedef void (*PFN_vkGetDeviceQueue)(VkDevice, uint32_t, uint32_t, VkQueue *);
typedef void (*PFN_vkDestroyInstance)(VkInstance, const void *);
typedef void (*PFN_vkDestroyDevice)(VkDevice, const void *);
#endif // !SR_VK_REAL_HEADERS

// --- Девайс ------------------------------------------------------------------
struct SrDevice {
    VkInstance  instance;
    VkPhysicalDevice phys;
    VkDevice    dev;
    VkQueue     queue;
    uint32_t    queue_family;
    SrCaps      caps;
};

// --- Заглушки операций: одно предупреждение на операцию -----------------------
#define SR_VK_UNIMPL(op)                                                      \
    do {                                                                      \
        static int warned;                                                    \
        if (!warned) {                                                        \
            warned = 1;                                                       \
            SDL_Log("Vulkan backend: '%s' не портирован (заглушка)", op);     \
        }                                                                     \
    } while (0)

#define SR_VK_STUB_VOID(name)      void name(SrDevice *d) { (void)d; SR_VK_UNIMPL(#name); }
#define SR_VK_STUB_ZERO(ret, name) ret name(SrDevice *d) { (void)d; SR_VK_UNIMPL(#name); return 0; }

// --- sr_create: бутстрап Vulkan ------------------------------------------------
SrDevice *sr_create(void)
{
    void *vklib = NULL;   // свой dlopen-хэндл (fallback, если SDL не грузит)
    PFN_vkGetInstanceProcAddr gpa = NULL;
    if (SDL_Vulkan_LoadLibrary(NULL) == 0) {
        gpa = (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
    } else {
        // Fallback: грузим libvulkan напрямую (SDL-путь иногда недоступен).
        vklib = SDL_LoadObject("libvulkan.so.1");
        if (!vklib) vklib = SDL_LoadObject("libvulkan.so");
        if (vklib)
            gpa = (PFN_vkGetInstanceProcAddr)SDL_LoadFunction(
                vklib, "vkGetInstanceProcAddr");
        if (!gpa)
            SDL_Log("Vulkan: loader не загрузился (SDL: %s)", SDL_GetError());
    }
    if (!gpa) {
        if (vklib) SDL_UnloadObject(vklib);
        else SDL_Vulkan_UnloadLibrary();
        return NULL;
    }
#define VKLOAD(instance, name)                                                \
    ((PFN_##name)(uintptr_t)gpa(instance, #name))
    PFN_vkCreateInstance vkCreateInstance = VKLOAD(NULL, vkCreateInstance);
    if (!vkCreateInstance) {
        SDL_Log("Vulkan: vkCreateInstance не найден");
        SDL_Vulkan_UnloadLibrary();
        return NULL;
    }

    VkApplicationInfo app = {};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "firerender";
    app.applicationVersion = 1;
    app.pEngineName = "firerender";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici = {};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;

    SrDevice *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    VkResult r = vkCreateInstance(&ici, NULL, &d->instance);
    if (r != VK_SUCCESS) {
        SDL_Log("Vulkan: vkCreateInstance = %d", r);
        free(d);
        SDL_Vulkan_UnloadLibrary();
        return NULL;
    }

    // Физическое устройство с graphics-очередью (первый подходящий)
    PFN_vkEnumeratePhysicalDevices vkEnumPhys =
        VKLOAD(d->instance, vkEnumeratePhysicalDevices);
    PFN_vkGetPhysicalDeviceQueueFamilyProperties vkQueueProps =
        VKLOAD(d->instance, vkGetPhysicalDeviceQueueFamilyProperties);
    PFN_vkCreateDevice vkCreateDevice = VKLOAD(d->instance, vkCreateDevice);
    PFN_vkGetDeviceQueue vkGetQueue = VKLOAD(d->instance, vkGetDeviceQueue);
    uint32_t np = 0;
    if (vkEnumPhys(d->instance, &np, NULL) != VK_SUCCESS || np == 0) {
        SDL_Log("Vulkan: физических устройств нет");
        goto fail;
    }
    VkPhysicalDevice *phys = calloc(np, sizeof *phys);
    vkEnumPhys(d->instance, &np, phys);
    uint32_t qf = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < np && qf == 0xFFFFFFFFu; i++) {
        uint32_t nq = 0;
        vkQueueProps(phys[i], &nq, NULL);
        VkQueueFamilyProperties *qp = calloc(nq ? nq : 1, sizeof *qp);
        vkQueueProps(phys[i], &nq, qp);
        for (uint32_t q = 0; q < nq; q++)
            if (qp[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                d->phys = phys[i];
                qf = q;
                break;
            }
        free(qp);
    }
    free(phys);
    if (qf == 0xFFFFFFFFu) {
        SDL_Log("Vulkan: ни у одного устройства нет graphics-очереди");
        goto fail;
    }

    VkDeviceQueueCreateInfo qci = {};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = qf;
    qci.queueCount = 1;
    float prio = 1.0f;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci = {};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    // Расширения поверхностей/swapchain добавляются на шаге present.
    if (vkCreateDevice(d->phys, &dci, NULL, &d->dev) != VK_SUCCESS) {
        SDL_Log("Vulkan: vkCreateDevice не удался");
        goto fail;
    }
    d->queue_family = qf;
    vkGetQueue(d->dev, qf, 0, &d->queue);

    // Возможности: Vulkan 1.1 гарантирует compute/indirect/3D-текстуры.
    static char ver[64];
    snprintf(ver, sizeof ver, "Vulkan 1.1 (queue family %u)", qf);
    d->caps.gles = 0;
    d->caps.gl_major = 1;
    d->caps.gl_minor = 1;              // семантика версии: 1.1 = Vulkan 1.1
    d->caps.compute = 1;
    d->caps.indirect = 1;
    d->caps.base_instance = 1;
    d->caps.timer_query = 1;
    d->caps.tex3d = 1;
    d->caps.color_half_float = 1;
    d->caps.texture_norm16 = 1;
    d->caps.msaa_max = 8;
    d->caps.max_tex_size = 16384;
    d->caps.vendor = "Vulkan";
    d->caps.renderer = "VkPhysicalDevice";
    d->caps.version = ver;
    SDL_Log("Vulkan backend: instance+device OK (%s)", ver);
    return d;

fail:
    PFN_vkDestroyInstance vkDestroyInst = VKLOAD(d->instance, vkDestroyInstance);
    if (vkDestroyInst) vkDestroyInst(d->instance, NULL);
    free(d);
    SDL_Vulkan_UnloadLibrary();
    return NULL;
#undef VKLOAD
}

void sr_destroy(SrDevice *d)
{
    if (!d) return;
    PFN_vkGetInstanceProcAddr gpa =
        (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
    if (gpa) {
        if (d->dev) {
            PFN_vkDestroyDevice dd =
                (PFN_vkDestroyDevice)(uintptr_t)gpa(d->instance, "vkDestroyDevice");
            if (dd) dd(d->dev, NULL);
        }
        if (d->instance) {
            PFN_vkDestroyInstance di =
                (PFN_vkDestroyInstance)(uintptr_t)gpa(d->instance, "vkDestroyInstance");
            if (di) di(d->instance, NULL);
        }
    }
    free(d);
    SDL_Vulkan_UnloadLibrary();
}

const SrCaps *sr_caps(const SrDevice *d) { return d ? &d->caps : NULL; }

// --- Операции рендера: каркас, реализация по дорожной карте -------------------
void sr_bind_fbo(SrDevice *d, SrFbo fbo) { (void)d; (void)fbo; SR_VK_UNIMPL("sr_bind_fbo"); }
void sr_viewport(SrDevice *d, int x, int y, int w, int h) { (void)d; (void)x; (void)y; (void)w; (void)h; SR_VK_UNIMPL("sr_viewport"); }
void sr_clear(SrDevice *d, float r, float g, float b, float a, int cc, int cd) { (void)d; (void)r; (void)g; (void)b; (void)a; (void)cc; (void)cd; SR_VK_UNIMPL("sr_clear"); }
int  sr_fbo_current(SrDevice *d) { (void)d; return 0; }
int  sr_fbo_complete(SrDevice *d) { (void)d; return 0; }
void sr_cap(SrDevice *d, SrStateCap cap, int on) { (void)d; (void)cap; (void)on; SR_VK_UNIMPL("sr_cap"); }
void sr_depth_mask(SrDevice *d, int w) { (void)d; (void)w; SR_VK_UNIMPL("sr_depth_mask"); }
void sr_blend_func(SrDevice *d, SrBlendFac s, SrBlendFac t) { (void)d; (void)s; (void)t; SR_VK_UNIMPL("sr_blend_func"); }
void sr_cull_front(SrDevice *d, int ccw) { (void)d; (void)ccw; SR_VK_UNIMPL("sr_cull_front"); }
void sr_wireframe(SrDevice *d, int on) { (void)d; (void)on; SR_VK_UNIMPL("sr_wireframe"); }

SrTex sr_tex_gen(SrDevice *d) { (void)d; SR_VK_UNIMPL("sr_tex_gen"); return 0; }
void sr_tex_delete(SrDevice *d, SrTex *t) { (void)d; (void)t; *t = 0; SR_VK_UNIMPL("sr_tex_delete"); }
void sr_tex_active(SrDevice *d, int u) { (void)d; (void)u; SR_VK_UNIMPL("sr_tex_active"); }
void sr_tex_bind(SrDevice *d, SrTexTarget tg, SrTex t) { (void)d; (void)tg; (void)t; SR_VK_UNIMPL("sr_tex_bind"); }
void sr_tex_image_2d(SrDevice *d, SrTex t, int face, SrFormat f, int w, int h, SrPixFormat p, const void *data) { (void)d; (void)t; (void)face; (void)f; (void)w; (void)h; (void)p; (void)data; SR_VK_UNIMPL("sr_tex_image_2d"); }
void sr_tex_image_3d(SrDevice *d, SrTex t, SrFormat f, int w, int h, int dep, SrPixFormat p, const void *data) { (void)d; (void)t; (void)f; (void)w; (void)h; (void)dep; (void)p; (void)data; SR_VK_UNIMPL("sr_tex_image_3d"); }
void sr_tex_sub_image_3d(SrDevice *d, SrTex t, int x, int y, int z, int w, int h, int dep, SrPixFormat p, const void *data) { (void)d; (void)t; (void)x; (void)y; (void)z; (void)w; (void)h; (void)dep; (void)p; (void)data; SR_VK_UNIMPL("sr_tex_sub_image_3d"); }
void sr_tex_params(SrDevice *d, SrTex t, SrTexTarget tg, SrFilter mn, SrFilter mg, SrWrap ws, SrWrap wt, SrWrap wr) { (void)d; (void)t; (void)tg; (void)mn; (void)mg; (void)ws; (void)wt; (void)wr; SR_VK_UNIMPL("sr_tex_params"); }
void sr_tex_generate_mipmap(SrDevice *d, SrTex t, SrTexTarget tg) { (void)d; (void)t; (void)tg; SR_VK_UNIMPL("sr_tex_generate_mipmap"); }

SrRb sr_rb_gen(SrDevice *d) { (void)d; SR_VK_UNIMPL("sr_rb_gen"); return 0; }
void sr_rb_delete(SrDevice *d, SrRb *rb) { (void)d; (void)rb; *rb = 0; SR_VK_UNIMPL("sr_rb_delete"); }
void sr_rb_storage(SrDevice *d, SrRb rb, SrFormat f, int samples, int w, int h) { (void)d; (void)rb; (void)f; (void)samples; (void)w; (void)h; SR_VK_UNIMPL("sr_rb_storage"); }
SrFbo sr_fbo_gen(SrDevice *d) { (void)d; SR_VK_UNIMPL("sr_fbo_gen"); return 0; }
void sr_fbo_delete(SrDevice *d, SrFbo *f) { (void)d; (void)f; *f = 0; SR_VK_UNIMPL("sr_fbo_delete"); }
void sr_fbo_attach_tex(SrDevice *d, SrFbo f, int att, SrTexTarget tg, SrTex t, int face) { (void)d; (void)f; (void)att; (void)tg; (void)t; (void)face; SR_VK_UNIMPL("sr_fbo_attach_tex"); }
void sr_fbo_attach_rb(SrDevice *d, SrFbo f, int att, SrRb rb) { (void)d; (void)f; (void)att; (void)rb; SR_VK_UNIMPL("sr_fbo_attach_rb"); }
void sr_blit(SrDevice *d, SrFbo src, SrFbo dst, int w, int h, int lin, int cdep) { (void)d; (void)src; (void)dst; (void)w; (void)h; (void)lin; (void)cdep; SR_VK_UNIMPL("sr_blit"); }

SrProg sr_prog_new(SrDevice *d, const char *vs, const char *fs) { (void)d; (void)vs; (void)fs; SR_VK_UNIMPL("sr_prog_new"); return 0; }
SrProg sr_prog_compute_new(SrDevice *d, const char *cs) { (void)d; (void)cs; SR_VK_UNIMPL("sr_prog_compute_new"); return 0; }
void sr_prog_delete(SrDevice *d, SrProg *p) { (void)d; (void)p; *p = 0; SR_VK_UNIMPL("sr_prog_delete"); }
void sr_prog_use(SrDevice *d, SrProg p) { (void)d; (void)p; SR_VK_UNIMPL("sr_prog_use"); }
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

SrBuf sr_buf_gen(SrDevice *d) { (void)d; SR_VK_UNIMPL("sr_buf_gen"); return 0; }
void sr_buf_delete(SrDevice *d, SrBuf *b) { (void)d; (void)b; *b = 0; SR_VK_UNIMPL("sr_buf_delete"); }
void sr_buf_bind(SrDevice *d, SrBufTarget tg, SrBuf b) { (void)d; (void)tg; (void)b; SR_VK_UNIMPL("sr_buf_bind"); }
void sr_buf_upload(SrDevice *d, SrBufTarget tg, const void *data, unsigned int bytes, int st) { (void)d; (void)tg; (void)data; (void)bytes; (void)st; SR_VK_UNIMPL("sr_buf_upload"); }
void sr_buf_sub_upload(SrDevice *d, SrBufTarget tg, unsigned int off, const void *data, unsigned int bytes) { (void)d; (void)tg; (void)off; (void)data; (void)bytes; SR_VK_UNIMPL("sr_buf_sub_upload"); }
void sr_buf_bind_ssbo(SrDevice *d, SrBuf b, int binding) { (void)d; (void)b; (void)binding; SR_VK_UNIMPL("sr_buf_bind_ssbo"); }

SrVao sr_vao_gen(SrDevice *d) { (void)d; SR_VK_UNIMPL("sr_vao_gen"); return 0; }
void sr_vao_delete(SrDevice *d, SrVao *v) { (void)d; (void)v; *v = 0; SR_VK_UNIMPL("sr_vao_delete"); }
void sr_vao_bind(SrDevice *d, SrVao v) { (void)d; (void)v; SR_VK_UNIMPL("sr_vao_bind"); }
void sr_attrib_float(SrDevice *d, SrLoc l, int comps, int stride, unsigned int off, int div) { (void)d; (void)l; (void)comps; (void)stride; (void)off; (void)div; SR_VK_UNIMPL("sr_attrib_float"); }

void sr_draw_arrays(SrDevice *d, SrPrim p, int f, int c) { (void)d; (void)p; (void)f; (void)c; SR_VK_UNIMPL("sr_draw_arrays"); }
void sr_draw_elements(SrDevice *d, SrPrim p, int c, int u32, unsigned int off) { (void)d; (void)p; (void)c; (void)u32; (void)off; SR_VK_UNIMPL("sr_draw_elements"); }
void sr_draw_arrays_instanced(SrDevice *d, SrPrim p, int f, int c, int inst) { (void)d; (void)p; (void)f; (void)c; (void)inst; SR_VK_UNIMPL("sr_draw_arrays_instanced"); }
void sr_draw_elements_instanced(SrDevice *d, SrPrim p, int c, int u32, unsigned int off, int inst) { (void)d; (void)p; (void)c; (void)u32; (void)off; (void)inst; SR_VK_UNIMPL("sr_draw_elements_instanced"); }
void sr_draw_elements_instanced_base(SrDevice *d, SrPrim p, int c, int u32, unsigned int off, int inst, unsigned int base) { (void)d; (void)p; (void)c; (void)u32; (void)off; (void)inst; (void)base; SR_VK_UNIMPL("sr_draw_elements_instanced_base"); }
void sr_draw_elements_indirect(SrDevice *d, SrPrim p, SrBuf cb, unsigned int off) { (void)d; (void)p; (void)cb; (void)off; SR_VK_UNIMPL("sr_draw_elements_indirect"); }
void sr_dispatch(SrDevice *d, int x, int y, int z) { (void)d; (void)x; (void)y; (void)z; SR_VK_UNIMPL("sr_dispatch"); }
void sr_barrier(SrDevice *d, unsigned int bits) { (void)d; (void)bits; SR_VK_UNIMPL("sr_barrier"); }

SrQuery sr_query_gen(SrDevice *d) { (void)d; SR_VK_UNIMPL("sr_query_gen"); return 0; }
void sr_query_delete(SrDevice *d, SrQuery *q) { (void)d; (void)q; *q = 0; SR_VK_UNIMPL("sr_query_delete"); }
void sr_query_begin(SrDevice *d, SrQuery q) { (void)d; (void)q; SR_VK_UNIMPL("sr_query_begin"); }
void sr_query_end(SrDevice *d) { (void)d; SR_VK_UNIMPL("sr_query_end"); }
float sr_query_result_ms(SrDevice *d, SrQuery q) { (void)d; (void)q; return 0.0f; }

void sr_read_pixels(SrDevice *d, int x, int y, int w, int h, SrPixFormat pix, void *out)
{ (void)d; (void)x; (void)y; (void)w; (void)h; (void)pix; (void)out; SR_VK_UNIMPL("sr_read_pixels"); }

typedef int sr_scaffold_marker;

#else
// Не Vulkan-сборка — файл пуст (движок собирает srender_gl.c).
typedef int sr_vk_disabled_marker;
#endif
