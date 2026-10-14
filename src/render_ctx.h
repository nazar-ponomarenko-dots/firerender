// ============================================================================
//  render_ctx.h — unified render context and shared scene uniform set
// ----------------------------------------------------------------------------
//  Previously each module (terrain/vegetation/objects/sky...) kept its own
//  CameraParams* and SunParams* and sent per-frame uniforms itself (uViewProj,
//  uCamPos, light, fog, shadow cascades) — with duplicated handles and code.
//
//  Now all per-frame state is assembled ONCE per frame in
//  RendererContext (renderer.c fills it from Game: camera, light, fog,
//  shadows; plus the pass flag for shadow rendering). Scene modules
//  receive a const RendererContext* at node creation and read it in
//  render — no cam/sun in parameters.
//
//  SceneUniforms — handles of the SHARED uniform set (present in all scene
//  shaders): looked up once at program creation (optional ones = -1),
//  sent in a single scene_uniforms_bind call.
// ============================================================================
#ifndef RENDER_CTX_H
#define RENDER_CTX_H
#include "gl_loader.h"
#include "frustum.h"
#include "shadow.h"      // SHADOW_CASCADES
#include "sun.h"         // SUN_MAX_LIGHTS

// --- Point and spot light sources (uniform array limits) ---------------
#define SCENE_MAX_POINT 8
#define SCENE_MAX_SPOT  4

// --- Render pass ------------------------------------------------------------
enum {
    RP_COLOR = 0,   // regular pass (into the scene FBO)
    RP_SHADOW,      // shadow pass (depth into a cascade) — see shadow.c
    RP_CUBEMAP_FACE, // rendering of one cubemap face (water reflections)
};

// --- Per-frame scene state (renderer.c fills it every frame) --------------
// The struct name matches the forward declaration in shadow.h.
typedef struct RendererContext {
    // Camera
    float viewproj[16];      // view * projection (computed by the Camera node)
    float cam_pos[3];        // player position
    float cam_fwd[3], cam_right[3], cam_up[3];   // view basis
    // Actual basis of the VIEW camera — where the frame is REALLY rendered from.
    // In 1st person it matches cam_*; in 3rd person the camera stands behind the character.
    // The skybox, particle billboards, SSR and voxel fog take the basis FROM HERE —
    // otherwise the sky and rays drifted apart from the scene in 3rd person.
    float view_pos[3];
    float view_fwd[3], view_right[3], view_up[3];
    float cam_yaw, cam_pitch;
    float cam_fov_deg, cam_aspect;
    float cam_vy;                // vertical speed (HUD, walk physics)
    int cam_fly;                 // flight mode (wind, HUD)
    int cam_duck;                // crouch (character collision frame in 3rd person)
    float znear, zfar;
    // Window: size in pixels (current, updated by input.c/renderer)
    int vw, vh;
    // Light: directional sources (0 = sun)
    float light_dir[SUN_MAX_LIGHTS][3];
    float light_color[SUN_MAX_LIGHTS][3];
    float light_intensity[SUN_MAX_LIGHTS];
    int light_count;
    // Point light sources (map.json lights[]: point)
    float point_pos[SCENE_MAX_POINT][3];
    float point_color[SCENE_MAX_POINT][3];
    float point_intensity[SCENE_MAX_POINT];
    float point_range[SCENE_MAX_POINT];
    int point_count;
    // Spotlights (spot): position + direction + half cone angle
    float spot_pos[SCENE_MAX_SPOT][3];
    float spot_dir[SCENE_MAX_SPOT][3];
    float spot_color[SCENE_MAX_SPOT][3];
    float spot_intensity[SCENE_MAX_SPOT];
    float spot_range[SCENE_MAX_SPOT];
    float spot_angle[SCENE_MAX_SPOT];
    int spot_count;
    // Environment colors (unified for all scene materials)
    float ambient[3], sky_color[3], ground_color[3];
    // Fog
    float fog_color[3], fog_start, fog_end;
    int fog_on;
    // Cascaded shadows (matrices/textures — from shadow.c)
    float shadow_matrix[SHADOW_CASCADES][16];
    GLuint shadow_tex[SHADOW_CASCADES];
    int shadow_on;                       // shadows enabled and the sun casts them
    // Cascade derivatives (bias/gradient from map.json: shadow.bias, bias_veg)
    float shadow_box[SHADOW_CASCADES];
    float shadow_depth[SHADOW_CASCADES];
    float shadow_bias[SHADOW_CASCADES];      // terrain
    float shadow_grad[SHADOW_CASCADES];
    float shadow_bias_veg[SHADOW_CASCADES];  // vegetation/objects
    float shadow_grad_veg[SHADOW_CASCADES];
    // Shadow pass (shadow_render fills it for each cascade)
    int pass;                            // RP_COLOR / RP_SHADOW
    float shadow_vp[16];                 // light matrix of the current cascade
    float shadow_box_cur;                // side of the current cascade's box
    int shadow_cascade;                  // cascade index
    int shadow_dyn, shadow_static;       // draw dynamics/statics
    int shadow_cull;                     // culling by cascade box
    // Shadow reach: how many meters the shadow of a 1 m tall object
    // stretches from it horizontally (depends on the sun angle). Cascades 0/1
    // draw not only objects inside the box but also distant ones whose long
    // shadow reaches the box (otherwise the player has no shadows from
    // distant trees).
    float shadow_slope;
    // Frustum of the color pass (extracted from viewproj in renderer_ctx_fill)
    Frustum frustum;
    float wetness;             // wetness 0..1 (from fx.c)
    float snowcover;           // snow cover 0..1 (from fx.c, grows under snow)
    float world_size[2];       // world size (X, Z), m — UV of the snow mask
    float sun_glow;            // brightness of the solar disk 0..1 (fades in rain)
    // Water reflection cubemap — STATIC: faces are read from cubemaps/ at
    // startup, updated only by the cubemap_capture command (see renderer.c).
    // Viewport of the current pass: regular passes — the window (vw/vh),
    // cubemap passes — the face size. Scene nodes set it instead of vw/vh.
    int view_vp[4];
} RendererContext;

// --- Shared uniform-handle set of scene shaders --------------------------------
typedef struct {
    GLint u_viewproj, u_campos;
    GLint u_lightdir, u_lightcolor, u_lightint, u_lightcount;
    GLint u_pointpos, u_pointcolor, u_pointint, u_pointrange, u_pointcount;
    GLint u_spotpos, u_spotdir, u_spotcolor, u_spotint, u_spotrange, u_spotangle, u_spotcount;
    GLint u_ambient, u_skycolor, u_groundcolor;
    GLint u_probe_amb, u_probe_sun, u_probe_on; // baked light for dynamics (per-instance)
    GLint u_fogcolor, u_fogstart, u_fogend, u_fogon;
    GLint u_shadowmap[SHADOW_CASCADES], u_shadowmatrix[SHADOW_CASCADES];
    GLint u_shadowbias[SHADOW_CASCADES], u_shadowdepth[SHADOW_CASCADES];
    GLint u_shadowgrad[SHADOW_CASCADES], u_shadow_on;
    GLint u_shadowbox;  // uShadowBox[4]: cascade blend boundaries (m)
    GLint u_wetness;    // wetness 0..1
    GLint u_snowcover;  // snow cover 0..1
    GLint u_snowmask;   // "open to sky" mask (unit 8, RG8: R=snow, G=dry under a roof)
    GLint u_worldsize;  // world size (X, Z) — for UV of the snow mask
    GLint u_reflection; // reflection cubemap (unit 9): wet ground/trees
    GLint u_noise;      // img/noise.png (unit 11): wetness unevenness
} SceneUniforms;

// Finds the shared set's locations in the program (optional ones = -1). Shadow
// matrices/bias/grad are looked up under BOTH names: terrain declares
// uShadowMatrix0..3, vegetation/objects — the array uShadowMatrix[4].
void scene_uniforms_init(SceneUniforms *su, GLuint prog);

// Sends the shared set from the context: views, light, environment, fog, cascades
// (matrices, textures on units 3..6, bias/gradient — base, terrain ones).
// Vegetation/objects override bias/gradient with their own values
// (rc->shadow_bias_veg etc.) AFTER the call.
void scene_uniforms_bind(const SceneUniforms *su, const RendererContext *rc);

// Selectively enables baked light for one dynamic object at the point
// (x,y,z): samples terrain_baked_probe() and sends uProbeAmb/uProbeSun/uProbeOn.
// If there is no bake or r_baked_light is off — zeroes uProbeOn (regular path).
void scene_uniforms_probe(const SceneUniforms *su, float x, float y, float z);

#endif
