// ============================================================================
//  renderer.h — orchestration of frame rendering (see renderer.c)
// ----------------------------------------------------------------------------
//  Draws the 3D scene (tree traversal of nodes), post-processing (bloom, voxel
//  fog) and 2D overlays (minimap, HUD, crosshair, map message, debug
//  panels). Owns the scene/fog FBOs (bloom + voxfog), which are created
//  on world load (renderer_setup) and destroyed on unload.
//  World state comes from Game* (see game.h) — no globals.
//
//  The SINGLE RendererContext lives here too (see render_ctx.h): each frame
//  renderer_frame fills it with camera/lights/fog/shadows, and the scene modules
//  read it through renderer_context() instead of their own cam/sun.
// ============================================================================
#ifndef RENDERER_H
#define RENDERER_H
#include "game.h"
#include "gpu.h"
#include "render_ctx.h"
#include "scene.h"

// --- Render context ------------------------------------------------------------
// Current per-frame scene context (filled in renderer_frame). The pointer is
// stable — scene modules get it at node creation and read it in render.
const RendererContext *renderer_context(void);

// --- Window size in pixels ------------------------------------------------------
// Actual window size (updated by main.c and input.c on resize). Modules
// use the render context (rc->vw/vh) or these functions — instead of globals.
int renderer_vw(void);
int renderer_vh(void);

// Sets the window size in pixels (call at startup and on resize).
void renderer_set_viewport(int vw, int vh);

// Frame readback for screenshots (RGB, 3 bytes/pixel, bottom-up).
// On GLES it reads the scene-target: the default framebuffer on EGL drivers
// returns black, although after swap the picture shows on screen.
void renderer_read_pixels(unsigned char *px, int vw, int vh);

// --- Lifecycle ------------------------------------------------------------
// Creates the bloom and fog FBOs (window size). Call on world load.
void renderer_setup(const WorldState *w, int vw, int vh);

// Recreates the FBOs when the window size changes (world loaded).
void renderer_resize(int vw, int vh);

// Destroys the bloom and fog FBOs. Call on world unload.
void renderer_teardown(void);

// Wireframe mode (cvar r_wireframe, from main.c sync_cvars).
void renderer_set_wireframe(int on);

// Cubemap reflection texture (6 faces from the player position, water samples
// it by the reflection vector; updated as the player moves).
GpuTex renderer_refl_cubemap(void);

// Cubemap capture: render 6 faces at position pos and save them
// to cubemaps/*.png (size size, capped at 512).
void renderer_cubemap_capture_at(Game *g, const float *pos, int size);

// --- Frame -----------------------------------------------------------------------
// Frame start: profiler reset, scene clear (into the bloom FBO if the world is
// loaded) with the fog color.
void renderer_begin(void);

// 3D scene + post-processing (bloom/voxfog) + UI (minimap/HUD).
// g — world state (nodes, camera, sun). draw_3d — draw the world
// (0 = background only, e.g. the menu over a photo).
void renderer_frame(Game *g, float dt, int draw_3d);

// Frame end: crosshair, map message, snd_debug/profiler panels.
void renderer_end(Game *g, float dt);

#endif
