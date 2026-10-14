// ============================================================================
//  voxfog.h — volumetric voxel fog (3D-texture cascades)
// ----------------------------------------------------------------------------
//  Like shadows, but in 3D: around the camera sit several 3D voxel cascades
//  (near 16 m / mid 48 m / far 160 m), fog density is computed on the CPU from
//  the terrain and the wheat field. Rendering is ONE fullscreen pass
//  (deferred): a ray from the camera into the scene depth marches the cascades,
//  the fog lands on everything (ground, plants, trees, sky) automatically.
//  Key P — toggle on/off.
// ============================================================================
#ifndef VOXFOG_H
#define VOXFOG_H
#include "gl_loader.h"
#include "render_ctx.h"
#include "world.h"

#define VF_CASCADES 3
#define VF_BOX0 16.0f      // side of cascade 0, m
#define VF_BOX1 48.0f
#define VF_BOX2 160.0f
#define VF_RES0 32         // cascade resolution (voxel 0.5 / 1 / 5 m)
#define VF_RES1 48
#define VF_RES2 32         // the far one is COARSER (soft; fps costs more than sharpness)

// Creates the cascade 3D textures and the fullscreen pass FBO.
// w — world data (terrain and field: fog density).
void voxfog_init(const WorldState *w, int vw, int vh);

// Recreates the FBO when the window size changes.
void voxfog_resize(int vw, int vh);

// Every frame: cascades are anchored to the WORLD (center snaps to voxel steps),
// only the cascade whose anchor changed is rebuilt. Fast.
void voxfog_update(const float cam_pos[3]);

// Fog pipeline: scene -> bloom (the composite is written INTO voxfog_fbo) ->
// voxfog_render (fog over the bloom, straight to screen — bloom does not
// light it up). Without fog: bloom_render(scene, 0) as before.
GLuint voxfog_fbo(void);            // where bloom writes its composite
GLuint voxfog_texture(void);        // that composite (texture)

// Fullscreen pass: bloom composite (color) + scene depth + cascade
// marching -> screen. FULL resolution. Camera/sun/shadows come from rc.
void voxfog_render(int scene_w, int scene_h, int out_w, int out_h,
                   const RendererContext *rc, GLuint bloomed_tex,
                   GLuint scene_depth);

int  voxfog_is_on(void);
void voxfog_toggle(void);
void voxfog_set(int on);   // force it (FR_TEST)
// Fog modes (r_fog_mode):
//   0 = off, 1 = volumetric (3D cascades), 2 = cheap (screen-space
//   height fog, one fullscreen pass — for weak machines).
void voxfog_set_mode(int mode);
int  voxfog_mode(void);

void voxfog_cleanup(void);

#endif
