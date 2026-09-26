// ============================================================================
//  srender_stub.c — выбор рендерного API и заготовки будущих бэкендов
// ----------------------------------------------------------------------------
//  Зачатки мульти-рендера: движок уже не завязан на GL напрямую (весь GPU-код
//  идёт через sr_* из srender.h), здесь живёт каркас, который решает, КАКОЙ
//  бэкенд вообще возможен на данной платформе и сборке.
//
//  Статус API:
//    OpenGL   — реализован (srender_gl.c), основной.
//    GLES 3.x — реализован (srender_gl.c, сборка make gles), low-end/Android.
//    Vulkan   — ЗАГОТОВКА. Для включения: srender_vk.c поверх VkInstance +
//               SPIR-V (glslc --target-env=vulkan1.1), поверхность —
//               SDL_Vulkan_CreateSurface, sync-примитивы вместо GL-модели
//               состояния (бэкенд кеширует пайплайны по хешу состояния).
//    Metal    — ЗАГОТОВКА. Два пути: (а) MoltenVK = Vulkan-бэкенд переисполь-
//               зует srender_vk.c как есть; (б) нативный MSL через SPIRV-Cross
//               + SDL_Metal_CreateView. Собирается только на __APPLE__.
//    Null     — no-op бэкенд: все sr_* как пустышки. Нужен для headless-
//               серверов/тестов логики без GPU. Пока — только каркас.
//
//  Правило миграции (см. [[roadmap]] проекта): каждый модуль, переведённый
//  на sr_*, автоматически становится бэкенд-независимым; ~25 модулей ещё на
//  сыром gl* — они и есть основной объём работ перед Vulkan/Metal.
// ============================================================================
#include "srender.h"

#include <SDL3/SDL.h>
#include <string.h>

#ifndef SR_ENABLE_VULKAN
#define SR_ENABLE_VULKAN 0   // 1 = srender_vk.c реализован и вкомпилирован
#endif
#ifndef SR_ENABLE_METAL
#define SR_ENABLE_METAL 0    // 1 = srender_metal.c реализован и вкомпилирован
#endif
#ifndef SR_ENABLE_NULL
#define SR_ENABLE_NULL 0     // 1 = no-op бэкенд реализован
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
        // Desktop GL всегда в билде (srender_gl.c + gl_loader.h).
        return 1;
    case SR_API_GLES:
        // GLES собирается из того же srender_gl.c — доступен на всех
        // платформах, где SDL даёт ES-контекст (EGL/Android).
        return 1;
    case SR_API_VULKAN:
#if SR_ENABLE_VULKAN
        return 1;
#else
        // Хотя бы сообщим, есть ли у SDL загрузчик Vulkan — пригодится
        // при включении бэкенда, чтобы не путать "нет в билде" и
        // "нет в системе".
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
    // Планируемые: показываем отдельно, чтобы статус был виден в логе старта.
    // (Зонд SDL_Vulkan_* добавим вместе с самим бэкендом.)
    if (!sr_api_available(SR_API_VULKAN))
        SDL_Log("Render API: Vulkan [stub — каркас есть, бэкенд не включён]");
    if (!sr_api_available(SR_API_METAL))
        SDL_Log("Render API: Metal [stub%s]",
#ifdef __APPLE__
                " — план: MoltenVK или SPIRV-Cross -> MSL");
#else
                " — платформа не Apple");
#endif
}
