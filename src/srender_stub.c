// ============================================================================
//  srender_stub.c — render API selection and scaffolding for future backends
// ----------------------------------------------------------------------------
//  Embryos of multi-rendering: the engine is no longer tied to GL directly
//  (all GPU code goes through sr_* from srender.h); this file holds the
//  scaffold that decides WHICH backend is even possible on a given platform
//  and build.
//
//  API status:
//    OpenGL   — implemented (srender_gl.c), the main one.
//    GLES 3.x — implemented (srender_gl.c, build make gles), low-end/Android.
//    Vulkan   — SCAFFOLD. To enable: srender_vk.c on top of VkInstance +
//               SPIR-V (glslc --target-env=vulkan1.1), surface via
//               SDL_Vulkan_CreateSurface, sync primitives instead of the GL
//               state model (the backend caches pipelines by state hash).
//    Metal    — SCAFFOLD. Two paths: (a) MoltenVK = the Vulkan backend reuses
//               srender_vk.c as is; (b) native MSL via SPIRV-Cross
//               + SDL_Metal_CreateView. Built only on __APPLE__.
//    Null     — no-op backend: all sr_* are empty. Needed for headless
//               servers/logic tests without a GPU. For now — only a scaffold.
//
//  Migration rule (see the project [[roadmap]]): every module ported to
//  sr_* automatically becomes backend-independent; ~25 modules are still on
//  raw gl* — they are the main body of work before Vulkan/Metal.
// ============================================================================
#include "srender.h"

#include <SDL3/SDL.h>
#include <string.h>

#ifndef SR_ENABLE_VULKAN
#define SR_ENABLE_VULKAN 0   // 1 = srender_vk.c implemented and compiled in
#endif
#ifndef SR_ENABLE_METAL
#define SR_ENABLE_METAL 0    // 1 = srender_metal.c implemented and compiled in
#endif
#ifndef SR_ENABLE_NULL
#define SR_ENABLE_NULL 0     // 1 = no-op backend implemented
#endif

const char *sr_api_name(SrApi api)
{
    switch (api) {
    case SR_API_GL:     return "OpenGL";
    case SR_API_GLES:   return "OpenGL ES";
    case SR_API_VULKAN: return "Vulkan";
    case SR_API_METAL:  return "Metal";
    case SR_API_NULL:   return "Null";
    default:            return "?";
    }
}

int sr_api_available(SrApi api)
{
    switch (api) {
    case SR_API_GL:
        // Desktop GL is always in the build (srender_gl.c + gl_loader.h).
        return 1;
    case SR_API_GLES:
        // GLES is built from the same srender_gl.c — available on all
        // platforms where SDL provides an ES context (EGL/Android).
        return 1;
    case SR_API_VULKAN:
#if SR_ENABLE_VULKAN
        return 1;
#else
        // At least report whether SDL has a Vulkan loader — useful when
        // enabling the backend, to avoid confusing "not in the build" with
        // "not on the system".
        return 0;
#endif
    case SR_API_METAL:
#if SR_ENABLE_METAL && defined(__APPLE__)
        return 1;
#else
        return 0;
#endif
    case SR_API_NULL:
        return SR_ENABLE_NULL;
    default:
        return 0;
    }
}

void sr_api_report(void)
{
    for (int i = 0; i < SR_API_COUNT; i++) {
        SrApi api = (SrApi)i;
        if (!sr_api_available(api)) continue;
        SDL_Log("Render API: %s [ready]", sr_api_name(api));
    }
    // Planned: shown separately so the status is visible in the startup log.
    // (SDL_Vulkan_* probe will be added together with the backend itself.)
    if (!sr_api_available(SR_API_VULKAN))
        SDL_Log("Render API: Vulkan [stub — skeleton only, backend not enabled]");
    if (!sr_api_available(SR_API_METAL))
        SDL_Log("Render API: Metal [stub%s]",
#ifdef __APPLE__
                " — plan: MoltenVK or SPIRV-Cross -> MSL");
#else
                " — non-Apple platform");
#endif
}
