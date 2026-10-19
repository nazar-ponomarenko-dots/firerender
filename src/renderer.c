// ============================================================================
//  renderer.c — frame render orchestration (see renderer.h)
// ----------------------------------------------------------------------------
//  Extracted from main.c: profiled scene-tree traversal, the post-processing
//  cascade (scene -> bloom -> voxel fog) and 2D overlays
//  (minimap, HUD, crosshair, map message, debug panels).
// ============================================================================
#include "renderer.h"

#include "app.h"
#include "actions.h"
#include "audio.h"
#include "bloom.h"
#include "camera.h"
#include "console.h"
#include "cvars.h"
#include "game.h"
#include "gi.h"
#include "lowend_profile.h"
#include "gl_loader.h"
#include "gpu.h"
#include "hud.h"
#include "linalg.h"
#include "mapdata.h"
#include "profiler.h"
#include "resources.h"
#include "srender.h"
#include "sky.h"
#include "ssr.h"
#include "physics.h"
#include "phys_body.h"      // rigid-body particle coloring on debug points
#include "glb_anim.h"       // ragdoll particle debug
#include "terrain.h"
#include "voxfog.h"
#include "water.h"
#include "world.h"
#include "fx.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>             // mingw: _mkdir is declared only here
#endif

#include "png.h"
#include "stb_image.h"

// Scene and fog FBOs: created at world load (renderer_setup)
static BloomParams s_bloom;
// SSR — screen-space reflections (wet terrain in rain): scene + reflection.
static SsrParams s_ssr;
// GI — dynamic global illumination (Lumen analogue): indirect light + AO.
static GiParams s_gi;
// Water reflection cubemap (6 faces, rendered from the player — see renderer_cubemap_pass)
static GpuTex s_refl_cubemap = 0;
static GpuFbo s_refl_cubemap_fbo[6] = {0};
static GpuTex s_refl_cubemap_depth = 0;
static int s_refl_cubemap_size = 256; // size of each face
static float s_cubemap_pos[3] = { 0, 0, 0 };  // center of the last cubemap
static int s_cubemap_valid = 0;               // cubemap has not been rendered yet
// The cubemap is rendered EVERY frame (real time): reflections are always
// fresh, no lag while moving. 6 scene renders of 256x256 per frame.
static int s_active = 0;       // world loaded: FBOs exist
static int s_wire = 0;         // wireframe mode (cvar r_wireframe)
static GpuTex s_noise_tex = 0; // img/noise.png: wetness/haze unevenness
static int s_vw = 1280, s_vh = 720;   // window size in pixels
// Render backend (RHI): one device per process. All GL in this file
// (frame preset, face FBOs, cubemap, state) goes through gpu_*, not directly.
static const GpuDevice *s_gpu = NULL;

int renderer_vw(void) { return s_vw; }
int renderer_vh(void) { return s_vh; }

void renderer_set_viewport(int vw, int vh)
{
    if (vw > 0 && vh > 0) { s_vw = vw; s_vh = vh; }
}

// --- Render scale (r_render_scale) ------------------------------------------
// The scene (together with MSAA/GI/SSR/voxel fog) is drawn at a REDUCED
// resolution, the final composite (voxfog/bloom) and the HUD at native
// window size: upscaling happens for free, textures are sampled with
// normalized UVs. Every fullscreen pass costs proportional to pixels —
// this is the main fps lever on weak iGPUs (0.6 = ~2.8x fewer pixels).
static int s_rsw = 1280, s_rsh = 720;  // scaled scene size
static int s_scene_ldr = -1;

static void scene_size_update(void)
{
    float sc = cvar_get("r_render_scale");
    if (sc < 0.4f) sc = 0.4f;
    if (sc > 1.0f) sc = 1.0f;
    int w = (int)((float)s_vw * sc + 0.5f);
    int h = (int)((float)s_vh * sc + 0.5f);
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    int ldr = cvar_get("r_scene_ldr") > 0.5f;
    int format_changed = ldr != s_scene_ldr;
    if (w == s_rsw && h == s_rsh && !format_changed) return;
    s_rsw = w;
    s_rsh = h;
    s_scene_ldr = ldr;
    bloom_resize(&s_bloom, s_rsw, s_rsh);
#ifndef USE_GLES_PATH
    ssr_resize(&s_ssr, s_rsw, s_rsh);
    gi_resize(&s_gi, s_rsw, s_rsh);
    voxfog_resize(s_rsw, s_rsh);
#endif
}

// --- Cubemap reflection FBO ----------------------------------------------------
static GpuTex make_cubemap_color_tex(int size)
{
    GpuTex id = gpu_tex_gen(s_gpu);
    gpu_cubemap_rgba8(s_gpu, id, size);
    gpu_tex_params_cubemap(s_gpu, id, 1);
    return id;
}

static GpuTex load_cubemap_from_files(const char *dir, int *out_size)
{
    static const char *face_names[6] = { "px", "nx", "py", "ny", "pz", "nz" };
    GpuTex id = 0;
    int size = 0;
    int loaded = 0;
    int valid = 1;

    // PNG stores rows top-to-bottom (capture Y-flips them) while glTexImage2D
    // treats row 0 as the bottom — without the reverse flip reflections would
    // be upside down and lie "crooked". So flip on load.
    //
    // IMPORTANT: the stb_image flag is GLOBAL and "sticky". It used to be set
    // here and never reset — yet make_cubemap_fbo is called from renderer_setup
    // at the VERY END of scene setup. On return to the menu cache_purge_pak
    // drops every resource loaded while the map pak was open (including engine
    // img/grass.png, img/tree.png) from the cache, and on a REPEAT map load
    // they were read with the flip still on — all grass/wheat/trees came
    // upside down. Now the flag goes back to 0 right after the faces are read
    // (see below).
    stbi_set_flip_vertically_on_load(1);
    for (int face = 0; face < 6; face++) {
        char pattern[256];
        snprintf(pattern, sizeof pattern, "cubemap_*_%s.png", face_names[face]);
        int count = 0;
        char **files = SDL_GlobDirectory(dir, pattern, 0, &count);
        if (!files || count == 0) {
            if (files) SDL_free(files);
            valid = 0;
            break;
        }
        // SDL_GlobDirectory stores its strings inside one block: copy the name
        // before freeing the list.
        const char *latest = files[0];
        for (int i = 1; i < count; i++) {
            if (strcmp(files[i], latest) > 0) latest = files[i];
        }
        char latest_name[256];
        snprintf(latest_name, sizeof latest_name, "%s", latest);
        SDL_free(files);

        char fullpath[512];
        snprintf(fullpath, sizeof fullpath, "%s/%s", dir, latest_name);
        int w, h, ch;
        unsigned char *px = stbi_load(fullpath, &w, &h, &ch, 3);
        if (!px) {
            SDL_Log("Failed to load cubemap face %s: %s", latest_name, stbi_failure_reason());
            valid = 0;
            break;
        }
        if (w != h || (size && (w != size || h != size))) {
            SDL_Log("Invalid cubemap face %s: %dx%d, expected %dx%d square",
                    latest_name, w, h, size, size);
            stbi_image_free(px);
            valid = 0;
            break;
        }
        if (!id) {
            id = gpu_tex_gen(s_gpu);
            size = w;
        }
        gpu_cubemap_face_rgb8(s_gpu, id, face, w, h, px);
        loaded++;
        stbi_image_free(px);
    }
    stbi_set_flip_vertically_on_load(0);   // global flag — restore as it was

    if (!valid || loaded != 6) {
        if (id) gpu_tex_delete(s_gpu, &id);
        return 0;
    }
    if (id) {
        gpu_tex_params_cubemap(s_gpu, id, 1);
        if (out_size) *out_size = size;
        SDL_Log("Loaded static cubemap from %s (%dx%d)", dir, size, size);
    }
    return id;
}

static void make_cubemap_fbo(int size)
{
    if (size <= 0) return;
    if (s_refl_cubemap) {
        gpu_tex_delete(s_gpu, &s_refl_cubemap);
        for (int i = 0; i < 6; i++) gpu_fbo_delete(s_gpu, &s_refl_cubemap_fbo[i]);
        gpu_tex_delete(s_gpu, &s_refl_cubemap_depth);
    }
    s_refl_cubemap_size = size;
    // Try to load static cubemap from cubemaps/ directory
    s_refl_cubemap = load_cubemap_from_files("cubemaps", &size);
    if (!s_refl_cubemap) {
        // Fallback: create empty cubemap texture
        s_refl_cubemap = make_cubemap_color_tex(size);
        SDL_Log("Cubemap reflection FBO created (empty): %dx%d", size, size);
    } else {
        SDL_Log("Using static cubemap from cubemaps/ (%dx%d)", size, size);
    }
    // Depth (shared by all faces) - still needed for capture
    s_refl_cubemap_depth = gpu_tex_gen(s_gpu);
    for (int face = 0; face < 6; face++)
        gpu_cubemap_face_depth24(s_gpu, s_refl_cubemap_depth, face, size);
    gpu_tex_params_cubemap(s_gpu, s_refl_cubemap_depth, 0);   // NEAREST
    // FBO per face (still needed for capture command)
    for (int face = 0; face < 6; face++) {
        s_refl_cubemap_fbo[face] = gpu_fbo_gen(s_gpu);
        gpu_fbo_attach_cubemap_color(s_gpu, s_refl_cubemap_fbo[face],
                                     s_refl_cubemap, face);
        gpu_fbo_attach_cubemap_depth(s_gpu, s_refl_cubemap_fbo[face],
                                     s_refl_cubemap_depth, face);
        if (!gpu_fbo_complete(s_gpu))
            SDL_Log("Cubemap FBO face %d incomplete!", face);
    }
    gpu_bind_framebuffer(s_gpu, 0);
}

// --- Render context: unified per-frame scene state --------------------------
static RendererContext s_rc;

static void cubemap_center_pos(Game *g, float out[3]);   // fwd decl (below)

const RendererContext *renderer_context(void)
{
    return &s_rc;
}

// Collects the per-frame scene state (camera, lights, fog, shadows) — ONCE
// per frame, before node rendering. shadow.c refreshes the cascade matrices
// at the end of shadow_render (fresh for vegetation/objects in the same frame).
static void renderer_ctx_fill(Game *g)
{
    RendererContext *rc = &s_rc;
    memset(rc, 0, sizeof *rc);
    rc->vw = s_rsw;   // scene at r_render_scale (HUD stays native)
    rc->vh = s_rsh;

    // Camera
    if (g->cam) {
        const Camera *cam = &g->cam->cam;
        memcpy(rc->viewproj, g->cam->viewproj, sizeof rc->viewproj);
        memcpy(rc->cam_pos, cam->pos, sizeof rc->cam_pos);
        rc->cam_yaw = cam->yaw;
        rc->cam_pitch = cam->pitch;
        rc->cam_fov_deg = g->cam->fov_deg;
        rc->cam_aspect = g->cam->aspect;
        rc->cam_vy = cam->vy;
        rc->cam_fly = cam->fly ? 1 : 0;
        rc->znear = g->cam->znear;
        rc->zfar = g->cam->zfar;
        camera_dirs(cam, rc->cam_fwd, rc->cam_right);
        vec3_cross(rc->cam_right, rc->cam_fwd, rc->cam_up);
        vec3_normalize(rc->cam_up);
        // View basis — where the frame is REALLY rendered from. The Camera
        // node recomputes it every frame (in third person the camera sits
        // behind the character); skybox/billboards/SSR/voxel fog use it.
        if (g->cam->view_fwd[0] || g->cam->view_fwd[1] || g->cam->view_fwd[2]) {
            memcpy(rc->view_pos, g->cam->view_pos, sizeof rc->view_pos);
            memcpy(rc->view_fwd, g->cam->view_fwd, sizeof rc->view_fwd);
            memcpy(rc->view_right, g->cam->view_right, sizeof rc->view_right);
            memcpy(rc->view_up, g->cam->view_up, sizeof rc->view_up);
        } else {                        // before the first camera_node_update
            memcpy(rc->view_pos, rc->cam_pos, sizeof rc->view_pos);
            memcpy(rc->view_fwd, rc->cam_fwd, sizeof rc->view_fwd);
            memcpy(rc->view_right, rc->cam_right, sizeof rc->view_right);
            memcpy(rc->view_up, rc->cam_up, sizeof rc->view_up);
        }
        rc->cam_duck = g_act[A_DUCK] ? 1 : 0;
        frustum_build(&rc->frustum, rc->viewproj);   // color pass culling
    }
    // Wetness (rain) — smooth 0..1 from fx.c (builds up/dries out over seconds).
    // In rain the original light sources are not switched off, just dimmed to
    // 20% brightness; on top a bright diffuse "sky light" is added to
    // compensate for the lost 80% of illumination.
    rc->wetness = fx_wetness();
    rc->snowcover = fx_snowcover();
    rc->world_size[0] = g->world.world_w;
    rc->world_size[1] = g->world.world_d;
    float light_dim = 1.0f - 0.8f * rc->wetness;   // 1.0 -> 0.20
    // Lights: directional sources (0 = sun)
    float dir_sum = 0.0f;
    if (g->sun_node) {
        const SunParams *sun = g->sun_node->params;
        int n = sun->light_count > SUN_MAX_LIGHTS ? SUN_MAX_LIGHTS : sun->light_count;
        for (int i = 0; i < n; i++) {
            memcpy(rc->light_dir[i], sun->light_dir[i], 3 * sizeof(float));
            memcpy(rc->light_color[i], sun->light_color[i], 3 * sizeof(float));
            rc->light_intensity[i] = sun->light_intensity[i] * light_dim;
            dir_sum += sun->light_intensity[i];
        }
        rc->light_count = n;
    }
    // Sky light (rain only): shines straight down, overcast sky color;
    // brightness grows with wetness up to 80% of the sun's total intensity.
    if (rc->wetness > 0.001f && dir_sum > 0.0f && rc->light_count < SUN_MAX_LIGHTS) {
        int k = rc->light_count++;
        rc->light_dir[k][0] = 0.0f;
        rc->light_dir[k][1] = 1.0f;
        rc->light_dir[k][2] = 0.0f;
        rc->light_color[k][0] = 0.62f;
        rc->light_color[k][1] = 0.68f;
        rc->light_color[k][2] = 0.80f;
        rc->light_intensity[k] = 0.8f * dir_sum * rc->wetness;
    }
    // Point and spot lights (map.json lights[]: point/spot)
    rc->point_count = rc->spot_count = 0;
    for (int i = 0; i < mapdata_light_count(); i++) {
        const MapLight *l = mapdata_light(i);
        if (!l) continue;
        if (l->type == LIGHT_POINT && rc->point_count < SCENE_MAX_POINT) {
            int k = rc->point_count++;
            memcpy(rc->point_pos[k], l->pos, 3 * sizeof(float));
            memcpy(rc->point_color[k], l->color, 3 * sizeof(float));
            rc->point_intensity[k] = l->intensity;
            rc->point_range[k] = l->range > 0.1f ? l->range : 20.0f;
        } else if (l->type == LIGHT_SPOT && rc->spot_count < SCENE_MAX_SPOT) {
            int k = rc->spot_count++;
            memcpy(rc->spot_pos[k], l->pos, 3 * sizeof(float));
            memcpy(rc->spot_dir[k], l->dir, 3 * sizeof(float));
            memcpy(rc->spot_color[k], l->color, 3 * sizeof(float));
            rc->spot_intensity[k] = l->intensity;
            rc->spot_range[k] = l->range > 0.1f ? l->range : 20.0f;
            rc->spot_angle[k] = l->angle > 0.01f ? l->angle : 0.6f;
        }
    }
    // Environment and fog — unified for all scene materials
    rc->ambient[0] = 0.75f; rc->ambient[1] = 0.80f; rc->ambient[2] = 0.90f;
    rc->ground_color[0] = 0.22f; rc->ground_color[1] = 0.30f; rc->ground_color[2] = 0.18f;
    sky_fog_color(rc->sky_color);
    memcpy(rc->fog_color, rc->sky_color, sizeof rc->fog_color);
    {
        MapTerrain tc;
        mapdata_terrain(&tc);
        rc->fog_start = g->world.world_w * tc.fog_start;
        rc->fog_end = g->world.world_w * tc.fog_end;
    }
    rc->fog_on = cvar_get("r_fog_mode") > 0.5f;
    // Sun visibility (in rain the disk is hidden by clouds — fades with wetness).
    // Wetness is already computed above (rc->wetness) — it dims the light source too.
    rc->sun_glow = 1.0f - rc->wetness;
    if (rc->sun_glow < 0.0f) rc->sun_glow = 0.0f;
    if (rc->sun_glow > 1.0f) rc->sun_glow = 1.0f;
    // Viewport of the current pass (cubemap passes override it)
    rc->view_vp[0] = 0;
    rc->view_vp[1] = 0;
    rc->view_vp[2] = s_rsw;
    rc->view_vp[3] = s_rsh;
    // Reflection cubemap — static: loaded from cubemaps/ at startup,
    // rewritten only by the cubemap_capture command.
    // Cascade shadows: textures and derived data (shadow.c refreshes the matrices)
    {
        const float boxes[SHADOW_CASCADES] = {
            SHADOW_BOX_0, SHADOW_BOX_1, SHADOW_BOX_2, SHADOW_BOX_3
        };
        int sun_shadow = 0;
        float bias = 4.0f, bias_veg = 2.0f;
        if (g->sun_node) {
            const SunParams *sun = g->sun_node->params;
            sun_shadow = sun->shadow_on;
            bias = sun->shadow_bias;
            bias_veg = sun->shadow_bias_veg;
        }
    for (int c = 0; c < SHADOW_CASCADES; c++) {
            rc->shadow_tex[c] = shadow_texture(c);
            memcpy(rc->shadow_matrix[c], shadow_matrix(c),
                   sizeof rc->shadow_matrix[c]);
            // uShadowBox for shaders is the cascade coverage RADIUS, not the
            // box side. The box is centered on the camera (shadow_matrix_for
            // with center=cam_pos, ortho ±box/2), so a fragment is guaranteed
            // inside the cascade map only at dist <= box/2. We pass halves of
            // {4,8,48,256}; beyond that sample_cascade returns "lit".
            rc->shadow_box[c] = boxes[c] * 0.5f;
            rc->shadow_depth[c] = boxes[c] * SHADOW_DEPTH_MUL + 400.0f;
            // Terrain bias: shadow_bias lateral texels (from map.json). It
            // USED TO be that cascade 3 got half as much — but its texel is
            // 50 cm and depth quantization is at maximum: with a low sun on
            // gentle slopes acne produced even contour bands on the ground
            // (band width = depth step per texel). Bias in texels is now the
            // same on all cascades — the world-scale value grows with the texel.
            float bias_world = bias * boxes[c] / g_shadow_size[c];
            rc->shadow_bias[c] = bias_world / rc->shadow_depth[c];
            rc->shadow_grad[c] = 10.0f / rc->shadow_depth[c];
            // Vegetation bias: shadow_bias_veg texels (2x smaller)
            float bias_world_v = bias_veg * boxes[c] / g_shadow_size[c];
            rc->shadow_bias_veg[c] = bias_world_v / rc->shadow_depth[c];
            rc->shadow_grad_veg[c] = 4.5f / rc->shadow_depth[c];
        }
        rc->shadow_on = shadow_is_on() && sun_shadow;
    }
}

// --- Shared scene uniform set --------------------------------------------------
// Located by name; shadow matrices/bias/grad are declared differently:
// terrain uses uShadowMatrix0..3, vegetation/objects use array uShadowMatrix[4].
static GLint su_loc2(GLuint prog, const char *a, const char *b)
{
    GLint l = glGetUniformLocation(prog, a);
    if (l < 0) l = glGetUniformLocation(prog, b);
    return l;
}

void scene_uniforms_init(SceneUniforms *su, GLuint prog)
{
    memset(su, 0, sizeof *su);
    su->u_viewproj = glGetUniformLocation(prog, "uViewProj");
    su->u_campos   = glGetUniformLocation(prog, "uCamPos");
    su->u_lightdir = glGetUniformLocation(prog, "uLightDir");
    su->u_lightcolor = glGetUniformLocation(prog, "uLightColor");
    su->u_lightint = glGetUniformLocation(prog, "uLightIntensity");
    su->u_lightcount = glGetUniformLocation(prog, "uLightCount");
    su->u_pointpos = glGetUniformLocation(prog, "uPointPos");
    su->u_pointcolor = glGetUniformLocation(prog, "uPointColor");
    su->u_pointint = glGetUniformLocation(prog, "uPointIntensity");
    su->u_pointrange = glGetUniformLocation(prog, "uPointRange");
    su->u_pointcount = glGetUniformLocation(prog, "uPointCount");
    su->u_spotpos = glGetUniformLocation(prog, "uSpotPos");
    su->u_spotdir = glGetUniformLocation(prog, "uSpotDir");
    su->u_spotcolor = glGetUniformLocation(prog, "uSpotColor");
    su->u_spotint = glGetUniformLocation(prog, "uSpotIntensity");
    su->u_spotrange = glGetUniformLocation(prog, "uSpotRange");
    su->u_spotangle = glGetUniformLocation(prog, "uSpotAngle");
    su->u_spotcount = glGetUniformLocation(prog, "uSpotCount");
    su->u_ambient  = glGetUniformLocation(prog, "uAmbient");
    su->u_skycolor = glGetUniformLocation(prog, "uSkyColor");
    su->u_groundcolor = glGetUniformLocation(prog, "uGroundColor");
    su->u_probe_amb = glGetUniformLocation(prog, "uProbeAmb");
    su->u_probe_sun = glGetUniformLocation(prog, "uProbeSun");
    su->u_probe_on  = glGetUniformLocation(prog, "uProbeOn");
    su->u_fogcolor = glGetUniformLocation(prog, "uFogColor");
    su->u_fogstart = glGetUniformLocation(prog, "uFogStart");
    su->u_fogend   = glGetUniformLocation(prog, "uFogEnd");
    su->u_fogon    = glGetUniformLocation(prog, "uFogOn");
    su->u_shadow_on = glGetUniformLocation(prog, "uShadowOn");
    // Cascade bounds: shaders pick a cascade by distance via uShadowBox[4].
    // Source of truth is SHADOW_BOX_* in shadow.h, but HALF of each side is
    // passed here (cascade coverage radius — the box is centered on the
    // camera, see renderer_ctx_fill), i.e. {4,8,48,256}.
    su->u_shadowbox = glGetUniformLocation(prog, "uShadowBox");
    su->u_wetness = glGetUniformLocation(prog, "uWetness");
    su->u_snowcover = glGetUniformLocation(prog, "uSnowCover");
    su->u_snowmask  = glGetUniformLocation(prog, "uSnowMask");
    su->u_worldsize = glGetUniformLocation(prog, "uWorldSize");
    su->u_reflection = glGetUniformLocation(prog, "uReflection");
    su->u_noise = glGetUniformLocation(prog, "uNoise");
    for (int c = 0; c < SHADOW_CASCADES; c++) {
        char a[64], b[64];
        snprintf(a, sizeof a, "uShadowMap%d", c);
        su->u_shadowmap[c] = glGetUniformLocation(prog, a);
        snprintf(a, sizeof a, "uShadowMatrix%d", c);
        snprintf(b, sizeof b, "uShadowMatrix[%d]", c);
        su->u_shadowmatrix[c] = su_loc2(prog, a, b);
        snprintf(a, sizeof a, "uShadowBias%d", c);
        snprintf(b, sizeof b, "uShadowBias[%d]", c);
        su->u_shadowbias[c] = su_loc2(prog, a, b);
        snprintf(a, sizeof a, "uShadowDepth%d", c);
        su->u_shadowdepth[c] = glGetUniformLocation(prog, a);
        snprintf(a, sizeof a, "uShadowGrad%d", c);
        snprintf(b, sizeof b, "uShadowGrad[%d]", c);
        su->u_shadowgrad[c] = su_loc2(prog, a, b);
    }
}

// Pushes the shared set from the context. bias/gradient are the base (terrain)
// ones; vegetation/objects override them with their own values after the call.
void scene_uniforms_bind(const SceneUniforms *su, const RendererContext *rc)
{
    if (su->u_viewproj >= 0)
        glUniformMatrix4fv(su->u_viewproj, 1, GL_FALSE, rc->viewproj);
    if (su->u_campos >= 0)
        glUniform3f(su->u_campos, rc->cam_pos[0], rc->cam_pos[1], rc->cam_pos[2]);
    if (su->u_lightdir >= 0 && rc->light_count > 0)
        glUniform3fv(su->u_lightdir, rc->light_count, &rc->light_dir[0][0]);
    if (su->u_lightcolor >= 0 && rc->light_count > 0)
        glUniform3fv(su->u_lightcolor, rc->light_count, &rc->light_color[0][0]);
    if (su->u_lightint >= 0 && rc->light_count > 0)
        glUniform1fv(su->u_lightint, rc->light_count, rc->light_intensity);
    if (su->u_lightcount >= 0) glUniform1i(su->u_lightcount, rc->light_count);
    // Point and spot lights
    if (su->u_pointpos >= 0 && rc->point_count > 0) glUniform3fv(su->u_pointpos, rc->point_count, &rc->point_pos[0][0]);
    if (su->u_pointcolor >= 0 && rc->point_count > 0) glUniform3fv(su->u_pointcolor, rc->point_count, &rc->point_color[0][0]);
    if (su->u_pointint >= 0 && rc->point_count > 0) glUniform1fv(su->u_pointint, rc->point_count, rc->point_intensity);
    if (su->u_pointrange >= 0 && rc->point_count > 0) glUniform1fv(su->u_pointrange, rc->point_count, rc->point_range);
    if (su->u_pointcount >= 0) glUniform1i(su->u_pointcount, rc->point_count);
    if (su->u_spotpos >= 0 && rc->spot_count > 0) glUniform3fv(su->u_spotpos, rc->spot_count, &rc->spot_pos[0][0]);
    if (su->u_spotdir >= 0 && rc->spot_count > 0) glUniform3fv(su->u_spotdir, rc->spot_count, &rc->spot_dir[0][0]);
    if (su->u_spotcolor >= 0 && rc->spot_count > 0) glUniform3fv(su->u_spotcolor, rc->spot_count, &rc->spot_color[0][0]);
    if (su->u_spotint >= 0 && rc->spot_count > 0) glUniform1fv(su->u_spotint, rc->spot_count, rc->spot_intensity);
    if (su->u_spotrange >= 0 && rc->spot_count > 0) glUniform1fv(su->u_spotrange, rc->spot_count, rc->spot_range);
    if (su->u_spotangle >= 0 && rc->spot_count > 0) glUniform1fv(su->u_spotangle, rc->spot_count, rc->spot_angle);
    if (su->u_spotcount >= 0) glUniform1i(su->u_spotcount, rc->spot_count);
    if (su->u_ambient >= 0)
        glUniform3f(su->u_ambient, rc->ambient[0], rc->ambient[1], rc->ambient[2]);
    if (su->u_skycolor >= 0)
        glUniform3f(su->u_skycolor, rc->sky_color[0], rc->sky_color[1], rc->sky_color[2]);
    if (su->u_groundcolor >= 0)
        glUniform3f(su->u_groundcolor, rc->ground_color[0], rc->ground_color[1], rc->ground_color[2]);
    // Baked light is OFF by default — object/mesh paths enable it selectively,
    // passing ambient and sun visibility from terrain_baked_probe().
    if (su->u_probe_on >= 0) glUniform1f(su->u_probe_on, 0.0f);
    if (su->u_fogcolor >= 0)
        glUniform3f(su->u_fogcolor, rc->fog_color[0], rc->fog_color[1], rc->fog_color[2]);
    if (su->u_fogstart >= 0) glUniform1f(su->u_fogstart, rc->fog_start);
    if (su->u_fogend >= 0)   glUniform1f(su->u_fogend, rc->fog_end);
    if (su->u_fogon >= 0)    glUniform1f(su->u_fogon, rc->fog_on ? 1.0f : 0.0f);
    if (su->u_shadow_on >= 0)
        glUniform1f(su->u_shadow_on, rc->shadow_on ? 1.0f : 0.0f);
    if (su->u_shadowbox >= 0)
        glUniform1fv(su->u_shadowbox, SHADOW_CASCADES, rc->shadow_box);
    if (su->u_wetness >= 0)
        glUniform1f(su->u_wetness, rc->wetness);
    if (su->u_snowcover >= 0)
        glUniform1f(su->u_snowcover, rc->snowcover);
    // Snow mask: baked "sky visibility" (same as wetness, RG8).
    // Unit 8 is shared with the terrain wetness map — it is the SAME
    // texture.
    if (su->u_snowmask >= 0 && terrain_snow_mask_have()) {
        gpu_active_texture(s_gpu, 8);
        gpu_bind_2d(s_gpu, terrain_snow_mask_tex());
        glUniform1i(su->u_snowmask, 8);
    }
    if (su->u_worldsize >= 0)
        glUniform2f(su->u_worldsize, rc->world_size[0], rc->world_size[1]);
    // Water reflection cubemap on unit 9 (for wet ground/trees; water keeps
    // it on unit 7 separately — no conflict). Inside the cubemap face pass we
    // do NOT bind it: the texture is attached to the current FBO — sampling
    // it would be a feedback loop (UB).
    if (su->u_reflection >= 0 && s_refl_cubemap && rc->pass != RP_CUBEMAP_FACE) {
        gpu_active_texture(s_gpu, 9);
        gpu_bind_cubemap(s_gpu, s_refl_cubemap);
        glUniform1i(su->u_reflection, 9);
    }
    // Wetness/haze noise (img/noise.png, REPEAT) — unit 11
    if (su->u_noise >= 0 && s_noise_tex) {
        gpu_active_texture(s_gpu, 11);
        gpu_bind_2d(s_gpu, s_noise_tex);
        glUniform1i(su->u_noise, 11);
    }
    for (int c = 0; c < SHADOW_CASCADES; c++) {
        // Cascades on texture units 3..6 (samplers assigned at init)
        if (su->u_shadowmap[c] >= 0 && rc->shadow_tex[c]) {
            gpu_active_texture(s_gpu, 3 + c);
            gpu_bind_2d(s_gpu, rc->shadow_tex[c]);
        }
        if (su->u_shadowmatrix[c] >= 0)
            glUniformMatrix4fv(su->u_shadowmatrix[c], 1, GL_FALSE,
                               rc->shadow_matrix[c]);
        if (su->u_shadowbias[c] >= 0)
            glUniform1f(su->u_shadowbias[c], rc->shadow_bias[c]);
        if (su->u_shadowgrad[c] >= 0)
            glUniform1f(su->u_shadowgrad[c], rc->shadow_grad[c]);
    }
    gpu_active_texture(s_gpu, 0);
}

void scene_uniforms_probe(const SceneUniforms *su, float x, float y, float z)
{
    if (su->u_probe_on < 0) return;
    // Baked object/mesh probes are also a STATIC solution (uProbeSun = shadow
    // of static blockers from the bake). Like the terrain lightmap, they are
    // enabled only in static shadow mode (r_shadows_mode==2). In modes 0/1/3
    // objects take the dynamic path: hemispheric ambient + CSM.
    if (!(cvar_get("r_baked_light") > 0.5f) || !terrain_baked_probes_have()
        || cvar_get("r_shadows_mode") != 2.0f) {
        glUniform1f(su->u_probe_on, 0.0f);
        return;
    }
    float p[4];
    terrain_baked_probe(x, y, z, p);
    if (su->u_probe_amb >= 0) glUniform3f(su->u_probe_amb, p[0], p[1], p[2]);
    if (su->u_probe_sun >= 0) glUniform1f(su->u_probe_sun, p[3]);
    glUniform1f(su->u_probe_on, 1.0f);
}

void renderer_setup(const WorldState *w, int vw, int vh)
{
    if (!s_gpu) s_gpu = gpu_create();   // RHI backend (GL): one per process
    cvar_register_ex("r_render_scale", 1.0f, 0.4f, 1.0f, CVAR_ARCHIVE,
                     "масштаб рендера сцены (1 = полное разрешение окна)");
    cvar_register_ex("r_cheap_fog", 0.0f, 0.0f, 1.0f, CVAR_ARCHIVE,
                     "туман только в шейдерах (без voxfog-проходов)");
    cvar_register_ex("r_present_blit", 1.0f, 0.0f, 1.0f, CVAR_ARCHIVE,
                     "present: grade+blit двумя проходами (1, быстрее на 0.1мс) / один композит "
                     "натив (0)");
    lowend_profile_init();
    lowend_profile_apply();
#ifdef USE_GLES_PATH
    // GLES uses a unified shader-present: blitting the default FBO is unreliable on EGL.
    cvar_set("r_present_blit", 0.0f);
#endif
    // Scene starts scaled right away: targets (MSAA/GI/SSR/fog) smaller than the window
    float sc = cvar_get("r_render_scale");
    if (sc < 0.4f) sc = 0.4f;
    if (sc > 1.0f) sc = 1.0f;
    s_rsw = (int)((float)vw * sc + 0.5f);
    s_rsh = (int)((float)vh * sc + 0.5f);
    if (s_rsw < 1) s_rsw = 1;
    if (s_rsh < 1) s_rsh = 1;
    s_scene_ldr = cvar_get("r_scene_ldr") > 0.5f;
#ifdef USE_GLES_PATH
    // Not all GLES devices support RGBA16F as a color attachment.
    // In that case pick a valid LDR target instead of a black incomplete FBO.
    {
        SrDevice *probe = sr_create();
        if (probe && !sr_caps(probe)->color_half_float) {
            cvar_set("r_scene_ldr", 1.0f);
            s_scene_ldr = 1;
            SDL_Log("GLES: RGBA16F target unsupported, using RGBA8 scene");
        }
        sr_destroy(probe);
    }
#endif
    memset(&s_bloom, 0, sizeof s_bloom);
    bloom_init(&s_bloom, s_rsw, s_rsh);
    SDL_Log("Scene target: %dx%d %s, present=%s", s_rsw, s_rsh,
            cvar_get("r_scene_ldr") > 0.5f ? "RGBA8" : "RGBA16F",
#ifdef USE_GLES_PATH
            "shader"
#else
            cvar_get("r_present_blit") > 0.5f ? "blit" : "shader"
#endif
    );
#ifndef USE_GLES_PATH
    ssr_init(&s_ssr, s_rsw, s_rsh);
    memset(&s_gi, 0, sizeof s_gi);
    gi_init(&s_gi, s_rsw, s_rsh);
    voxfog_init(w, s_rsw, s_rsh);
#else
    (void)w;
#endif
    make_cubemap_fbo(256); // fixed-size cubemap
    s_cubemap_valid = 0;
    s_noise_tex = resources_texture("img/noise.png", 3, GL_REPEAT, 1);
    s_active = 1;
    // Wireframe mode from config.cfg: sync_cvars fires on change.
    s_wire = cvar_get("r_wireframe") > 0.5f;
}

void renderer_resize(int vw, int vh)
{
    if (!s_active || vw <= 0 || vh <= 0) return;
    s_vw = vw;
    s_vh = vh;
    // Force the common scaled resize: actual FBOs must match
    // r_render_scale, not the native window size.
    s_rsw = s_rsh = 0;
    scene_size_update();
}

void renderer_teardown(void)
{
    if (!s_active) return;
    bloom_cleanup(&s_bloom);
#ifndef USE_GLES_PATH
    ssr_cleanup(&s_ssr);
    gi_cleanup(&s_gi);
    voxfog_cleanup();
#endif
    if (s_refl_cubemap) {
        gpu_tex_delete(s_gpu, &s_refl_cubemap);
        for (int i = 0; i < 6; i++) gpu_fbo_delete(s_gpu, &s_refl_cubemap_fbo[i]);
        gpu_tex_delete(s_gpu, &s_refl_cubemap_depth);
    }
    if (s_noise_tex) {
        resources_texture_release(s_noise_tex);
        s_noise_tex = 0;
    }
    s_cubemap_valid = 0;
    s_active = 0;
}

void renderer_set_wireframe(int on)
{
    s_wire = on ? 1 : 0;
}

GpuTex renderer_refl_cubemap(void)
{
    return s_refl_cubemap;
}

// --- Scene tree traversal --------------------------------------------------------
// Renders part of the tree: 0 = 3D scene (into the bloom FBO), 1 = UI
// (minimap and HUD — over the composite, not included in bloom).
// Every node is timed for profiling (profiler).
static ProfSlot node_slot(const char *name)
{
    if (strcmp(name, "Sky") == 0) return PROF_SKY;
    if (strcmp(name, "Terrain") == 0) return PROF_TERRAIN;
    if (strcmp(name, "Vegetation") == 0) return PROF_VEG;
    if (strcmp(name, "Objects") == 0) return PROF_OBJECTS;
    if (strcmp(name, "Fauna") == 0) return PROF_FAUNA;
    if (strcmp(name, "FieldArea") == 0) return PROF_FIELD;
    if (strcmp(name, "Selection") == 0) return PROF_SEL;
    if (strcmp(name, "Minimap") == 0) return PROF_MM;
    if (strcmp(name, "DebugHUD") == 0) return PROF_HUD;
    return PROF_TERRAIN;   // unknown node — into the general terrain budget
}

static void render_part(SceneNode *world, float dt, int ui)
{
    int profiling = cvar_get("profiler") > 0.5f;
    for (SceneNode *c = world->first_child; c; c = c->next) {
        if (!c->render) continue;
        int is_ui = (strcmp(c->name, "Minimap") == 0) ||
                    (strcmp(c->name, "DebugHUD") == 0);
        if (is_ui != ui) continue;
        if (!profiling) {
            c->render(c, dt);
            continue;
        }
        ProfSlot slot = node_slot(c->name);
        Uint64 t0 = SDL_GetPerformanceCounter();
        prof_gpu_begin(slot);
        c->render(c, dt);
        prof_gpu_end(slot);
        Uint64 t1 = SDL_GetPerformanceCounter();
        float ms = (float)(t1 - t0) * 1000.0f /
                   (float)SDL_GetPerformanceFrequency();
        prof_add_ms(slot, ms);
    }
}

static void render_3d(SceneNode *world, float dt)
{
    render_part(world, dt, 0);
}

static void render_ui(SceneNode *world, float dt)
{
    render_part(world, dt, 1);
}

// --- Frame -----------------------------------------------------------------------
void renderer_begin(void)
{
    // profiler: reset per-frame timings and counters
    prof_reset();
    float clear_rgb[3] = { 0.6f, 0.65f, 0.7f };
    if (s_active) sky_fog_color(clear_rgb);
    // Live r_render_scale/r_msaa change: recreate targets BEFORE binding
    scene_size_update();
    bloom_msaa_sync(&s_bloom);
    gpu_bind_framebuffer(s_gpu, s_active ? bloom_scene_fbo(&s_bloom) : 0);
    // Scene — scaled; direct rendering to screen (menu background) — native
    if (s_active) gpu_viewport(s_gpu, 0, 0, s_rsw, s_rsh);
    else          gpu_viewport(s_gpu, 0, 0, s_vw, s_vh);
    gpu_clear(s_gpu, clear_rgb[0], clear_rgb[1], clear_rgb[2], 1.0f);
}

// --- Cubemap reflection pass ---------------------------------------------------
// 6 direction/up vectors of the cubemap faces (standard GL orientation:
// each face is rendered right-side-up so samplerCube reads it per spec).
static const float s_face_dirs[6][3] = {
    { 1,  0,  0},   // +X
    {-1,  0,  0},   // -X
    { 0,  1,  0},   // +Y
    { 0, -1,  0},   // -Y
    { 0,  0,  1},   // +Z
    { 0,  0, -1},   // -Z
};
static const float s_face_ups[6][3] = {
    { 0, -1,  0},   // +X
    { 0, -1,  0},   // -X
    { 0,  0,  1},   // +Y
    { 0,  0, -1},   // -Y
    { 0, -1,  0},   // +Z
    { 0, -1,  0},   // -Z
};

// Reflection cubemap center: player x/z, y — water level under the player
// (if there is no water — camera height: the cube captures the player's surroundings).
__attribute__((unused))
static void cubemap_center_pos(Game *g, float out[3])
{
    const Camera *cam = &g->cam->cam;
    out[0] = cam->pos[0];
    out[2] = cam->pos[2];
    float ws = water_surface_at(&g->world, cam->pos[0], cam->pos[2]);
    out[1] = (ws > -1e8f) ? ws : cam->pos[1];
}

// Renders all 6 reflection cubemap faces from point pos. Saves/restores the
// context camera: scene nodes (sky, billboards, frustum) must see the current
// face, not the main camera. dt = 0 — animation time must not advance.
static void renderer_cubemap_render_at(Game *g, SceneNode *world, const float *pos)
{
    if (!s_refl_cubemap || !g || !g->cam || !world) return;

    RendererContext *rc = &s_rc;
    // Save the current context state
    float orig_viewproj[16];
    memcpy(orig_viewproj, rc->viewproj, sizeof orig_viewproj);
    int orig_pass = rc->pass;
    float orig_pos[3], orig_fwd[3], orig_right[3], orig_up[3];
    memcpy(orig_pos, rc->cam_pos, sizeof orig_pos);
    memcpy(orig_fwd, rc->cam_fwd, sizeof orig_fwd);
    memcpy(orig_right, rc->cam_right, sizeof orig_right);
    memcpy(orig_up, rc->cam_up, sizeof orig_up);
    float orig_fov = rc->cam_fov_deg, orig_aspect = rc->cam_aspect;
    int orig_vp[4];
    memcpy(orig_vp, rc->view_vp, sizeof orig_vp);
    Frustum orig_frustum;
    memcpy(&orig_frustum, &rc->frustum, sizeof orig_frustum);
    int orig_fbo = gpu_fbo_current(s_gpu);

    float proj[16];
    mat4_perspective(proj, 90.0f * 3.14159f / 180.0f, 1.0f, rc->znear, rc->zfar);

    // Face pass: camera at the cube center; the clip plane cuts everything
    // below the center (underwater/underground garbage is not wanted in reflections).
    rc->pass = RP_CUBEMAP_FACE;
    memcpy(rc->cam_pos, pos, sizeof rc->cam_pos);
    rc->cam_fov_deg = 90.0f;
    rc->cam_aspect = 1.0f;
    rc->view_vp[0] = 0;
    rc->view_vp[1] = 0;
    rc->view_vp[2] = s_refl_cubemap_size;
    rc->view_vp[3] = s_refl_cubemap_size;

    for (int face = 0; face < 6; face++) {
        float view[16], viewproj[16], target[3];
        target[0] = pos[0] + s_face_dirs[face][0];
        target[1] = pos[1] + s_face_dirs[face][1];
        target[2] = pos[2] + s_face_dirs[face][2];
        mat4_lookat(view, pos, target, s_face_ups[face]);
        mat4_mul(viewproj, proj, view);

        memcpy(rc->viewproj, viewproj, sizeof rc->viewproj);
        // Face view basis: sky (sky.fs) and billboards (particle.vs)
        // reconstruct the ray direction from cam_fwd/right/up.
        memcpy(rc->cam_fwd, s_face_dirs[face], sizeof rc->cam_fwd);
        memcpy(rc->cam_up, s_face_ups[face], sizeof rc->cam_up);
        vec3_cross(rc->cam_fwd, rc->cam_up, rc->cam_right);
        vec3_normalize(rc->cam_right);
        vec3_cross(rc->cam_right, rc->cam_fwd, rc->cam_up);
        vec3_normalize(rc->cam_up);
        frustum_build(&rc->frustum, viewproj);

        gpu_bind_framebuffer(s_gpu, s_refl_cubemap_fbo[face]);
        gpu_viewport(s_gpu, 0, 0, s_refl_cubemap_size, s_refl_cubemap_size);
        gpu_clear(s_gpu, 0.10f, 0.10f, 0.15f, 1.0f);
        gpu_cap(s_gpu, GPU_CAP_DEPTH_TEST, 1);
        gpu_cap(s_gpu, GPU_CAP_CULL_FACE, 1);
        render_part(world, 0.0f, 0);   // dt = 0: do not advance scene time
    }

    // Restore the context
    memcpy(rc->viewproj, orig_viewproj, sizeof rc->viewproj);
    rc->pass = orig_pass;
    memcpy(rc->cam_pos, orig_pos, sizeof rc->cam_pos);
    memcpy(rc->cam_fwd, orig_fwd, sizeof rc->cam_fwd);
    memcpy(rc->cam_right, orig_right, sizeof rc->cam_right);
    memcpy(rc->cam_up, orig_up, sizeof rc->cam_up);
    rc->cam_fov_deg = orig_fov;
    rc->cam_aspect = orig_aspect;
    memcpy(rc->view_vp, orig_vp, sizeof rc->view_vp);
    memcpy(&rc->frustum, &orig_frustum, sizeof rc->frustum);
    gpu_bind_framebuffer(s_gpu, (GpuFbo)orig_fbo);
    gpu_viewport(s_gpu, orig_vp[0], orig_vp[1], orig_vp[2], orig_vp[3]);

    memcpy(s_cubemap_pos, pos, sizeof s_cubemap_pos);
    s_cubemap_valid = 1;
}

void renderer_frame(Game *g, float dt, int draw_3d)
{
    renderer_ctx_fill(g);        // single scene context for this frame
    SceneNode *world = g->root;
    int did_3d = 0;

    // Underwater effects: track whether the player's eyes are submerged and
    // smoothly fade post-processing in/out (tint, distortion, vignette). The
    // tint comes from the water body (water_underwater); time accumulates for
    // the wave animation.
    if (world && draw_3d) {
        static float s_under = 0.0f;
        static float s_undtime = 0.0f;
        float tint[3] = { 0.08f, 0.22f, 0.30f };
        int inw = g->world_loaded &&
                  water_underwater(&g->world, s_rc.cam_pos, tint);
        float target = inw ? 1.0f : 0.0f;
        float rate = 1.6f;                 // ~0.6 s to full in/out
        if (s_under < target) {
            s_under += rate * dt; if (s_under > target) s_under = target;
        } else if (s_under > target) {
            s_under -= rate * dt; if (s_under < 0.0f) s_under = 0.0f;
        }
        if (s_under > 0.0f) s_undtime += dt;   // waves keep going while the effect is visible
        bloom_set_underwater(s_under, tint, s_undtime);
    }

    // Reflection cubemap — static (loaded from cubemaps/ at startup).
    // Real-time rendering is disabled for performance.
    // Use the cubemap_capture console command to refresh it.

    // In game and on pause — draw the scene (pause darkens it with the menu).
    // In the main menu with a photo background the scene is not drawn at all (draw_3d = 0).
    if (world && draw_3d) {
        // Cascade shadows: nodes draw depth (pass = RP_SHADOW). Always filled
        // — until wireframe mode turns on.
        if (s_rc.shadow_on) {
            int profiling = cvar_get("profiler") > 0.5f;
            Uint64 t0 = profiling ? SDL_GetPerformanceCounter() : 0;
            if (profiling) prof_gpu_begin(PROF_SHADOW);
            shadow_render(world, &s_rc);
            if (profiling) {
                prof_gpu_end(PROF_SHADOW);
                Uint64 t1 = SDL_GetPerformanceCounter();
                prof_add_ms(PROF_SHADOW,
                            (float)(t1 - t0) * 1000.0f /
                            (float)SDL_GetPerformanceFrequency());
            }
        }
        if (s_wire) gpu_wireframe(s_gpu, 1);
        render_3d(world, dt);
        if (s_wire) gpu_wireframe(s_gpu, 0);
        did_3d = 1;
    }
    if (did_3d) {
        // Resolve the MSAA scene into textures (color+depth) for post-passes.
        // Depth is resolved only if it will actually be read below — same
        // conditions as for its consumers (GI/SSR/voxfog). In the POTATO preset
        // all three are off → the fullscreen depth blit is skipped.
        int need_depth =
            gi_is_on() ||
            (cvar_get("r_ssr") >= 0.5f &&
             (s_rc.wetness > 0.001f || cvar_get("r_ssr_wet") > 0.001f) &&
             cvar_get("r_ssr_strength") > 0.0f) ||
            (voxfog_is_on() && cvar_get("r_cheap_fog") < 0.5f);
        bloom_resolve(&s_bloom, need_depth);
        gpu_bind_framebuffer(s_gpu, 0);
        gpu_viewport(s_gpu, 0, 0, s_vw, s_vh);
        int profiling = cvar_get("profiler") > 0.5f;
        Uint64 pf = profiling ? SDL_GetPerformanceFrequency() : 0;
        GLuint scene_for_post = bloom_scene_tex(&s_bloom);
        // GI (Lumen analogue): indirect light + ambient occlusion. Runs FIRST
        // among post-passes — SSR and bloom must see the already-lit scene
        // (otherwise reflections and glow would be computed from an under-lit one).
        if (gi_is_on()) {
            Uint64 tg = profiling ? SDL_GetPerformanceCounter() : 0;
            if (profiling) prof_gpu_begin(PROF_GI);
            scene_for_post = gi_render(&s_gi, scene_for_post,
                                       bloom_scene_depth(&s_bloom), &s_rc);
            if (profiling) {
                prof_gpu_end(PROF_GI);
                Uint64 te = SDL_GetPerformanceCounter();
                prof_add_ms(PROF_GI, (float)(te - tg) * 1000.0f / (float)pf);
            }
        }
        // SSR: scene+reflection (wet terrain in rain). SSR off/dry or strength
        // 0 — skip entirely (the SSR FBO may be stale from earlier frames).
        if (cvar_get("r_ssr") >= 0.5f &&
            (s_rc.wetness > 0.001f || cvar_get("r_ssr_wet") > 0.001f) &&
            cvar_get("r_ssr_strength") > 0.0f) {
            Uint64 ts = profiling ? SDL_GetPerformanceCounter() : 0;
            if (profiling) prof_gpu_begin(PROF_SSR);
            ssr_render(&s_ssr, scene_for_post,
                       bloom_scene_depth(&s_bloom), &s_rc);
            if (profiling) {
                prof_gpu_end(PROF_SSR);
                Uint64 te = SDL_GetPerformanceCounter();
                prof_add_ms(PROF_SSR, (float)(te - ts) * 1000.0f / (float)pf);
            }
            scene_for_post = ssr_tex(&s_ssr);
        }
        // r_cheap_fog: fog ONLY in shaders (terrain.fs/wheat.fs — distance mix
        // + early exit for far grass), no voxfog march or composite. For the
        // POTATO preset: horizon softened, zero extra passes.
        if (voxfog_is_on() && cvar_get("r_cheap_fog") < 0.5f) {
            Uint64 t0 = profiling ? SDL_GetPerformanceCounter() : 0;
            if (profiling) prof_gpu_begin(PROF_BLOOM);
            bloom_render(&s_bloom, scene_for_post, voxfog_fbo(),
                         s_rsw, s_rsh);
            if (profiling) {
                prof_gpu_end(PROF_BLOOM);
                Uint64 t1 = SDL_GetPerformanceCounter();
                prof_add_ms(PROF_BLOOM, (float)(t1 - t0) * 1000.0f / (float)pf);
                t0 = SDL_GetPerformanceCounter();
                prof_gpu_begin(PROF_FOG);
            }
            voxfog_render(s_rsw, s_rsh, s_vw, s_vh, &s_rc,
                          voxfog_texture(), bloom_scene_depth(&s_bloom));
            if (profiling) {
                prof_gpu_end(PROF_FOG);
                Uint64 t1 = SDL_GetPerformanceCounter();
                prof_add_ms(PROF_FOG, (float)(t1 - t0) * 1000.0f / (float)pf);
            }
        } else {
            Uint64 t0 = profiling ? SDL_GetPerformanceCounter() : 0;
            if (profiling) prof_gpu_begin(PROF_BLOOM);
            // r_present_blit=1 — grading at scene scale + hardware blit
            // (faster on weak iGPUs); 0 — shader composite to the screen.
#ifdef USE_GLES_PATH
            // ES/EGL backbuffer: fullscreen shader present. Real Android
            // surfaces keep the frame until SDL_GL_SwapWindow; desktop offscreen
            // EGL may not support default framebuffer readback correctly.
            bloom_render(&s_bloom, scene_for_post, 0, s_vw, s_vh);
#else
            if (cvar_get("r_present_blit") > 0.5f)
                bloom_present(&s_bloom, scene_for_post, s_vw, s_vh);
            else
                bloom_render(&s_bloom, scene_for_post, 0, s_vw, s_vh);
#endif
            if (profiling) {
                prof_gpu_end(PROF_BLOOM);
                Uint64 t1 = SDL_GetPerformanceCounter();
                prof_add_ms(PROF_BLOOM,
                            (float)(t1 - t0) * 1000.0f / (float)pf);
            }
        }
    }

    // HUD/minimap — in game only; the menu draws over it
    if (world && app_state() == APP_PLAY)
        render_ui(world, dt);
}

// Frame readback for screenshots. On GLES/EGL drivers hand back the default
// framebuffer as black (even though the image does reach the screen after
// swap) — so read the scene target: a valid FBO of the same size (at
// r_render_scale 1.0), HUD overlays are not included in the frame.
void renderer_read_pixels(unsigned char *px, int vw, int vh)
{
    int rw = vw, rh = vh;
    // The caller's buffer is vw*vh*3 with no row padding, while GL pads each
    // returned row to GL_PACK_ALIGNMENT (default 4): a GL_RGB row of vw*3 bytes
    // overruns the buffer whenever vw is not a multiple of 4 (the driver does the
    // write, so a sanitizer cannot see it). See also sr_read_pixels.
    GLint pack_align = 4;
    glGetIntegerv(GL_PACK_ALIGNMENT, &pack_align);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
#ifdef USE_GLES_PATH
    GLuint prev = 0;   // binding is restored only on the GLES path
    rw = s_rsw < vw ? s_rsw : vw;
    rh = s_rsh < vh ? s_rsh : vh;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, (GLint *)&prev);
    glBindFramebuffer(GL_FRAMEBUFFER, bloom_scene_fbo(&s_bloom));
    if (getenv("FR_GLDBG")) {
        SDL_Log("capture: fbo=%u complete=%d err_before=0x%x",
                bloom_scene_fbo(&s_bloom),
                glCheckFramebufferStatus(GL_FRAMEBUFFER), glGetError());
    }
#endif
    // GLES drivers (NVIDIA EGL) return GL_INVALID_OPERATION when reading GL_RGB
    // from an RGBA8 attachment — read GL_RGBA and drop the alpha.
#ifdef USE_GLES_PATH
    memset(px, 0, (size_t)vw * vh * 3);
    {
        unsigned char *rgba = malloc((size_t)rw * rh * 4);
        if (rgba) {
            glReadPixels(0, 0, rw, rh, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
            for (int y = 0; y < rh; y++)
                for (int x = 0; x < rw; x++) {
                    memcpy(px + ((size_t)y * vw + x) * 3,
                           rgba + ((size_t)y * rw + x) * 4, 3);
                }
            free(rgba);
        }
    }
#else
    glReadPixels(0, 0, rw, rh, GL_RGB, GL_UNSIGNED_BYTE, px);
#endif
#ifdef USE_GLES_PATH
    if (getenv("FR_GLDBG")) {
        unsigned char t[3];
        glReadPixels(rw / 2, rh / 2, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, t);
        SDL_Log("capture: err_after=0x%x center=%d,%d,%d", glGetError(),
                t[0], t[1], t[2]);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, prev);
#endif
    glPixelStorei(GL_PACK_ALIGNMENT, pack_align);
}

void renderer_end(Game *g, float dt)
{
    lowend_profile_frame(dt * 1000.0f);
    // All overlays and screenshot read the default framebuffer. Some ES/EGL
    // drivers do not keep separate READ/DRAW bindings after post-passes.
    gpu_bind_framebuffer(s_gpu, 0);
    gpu_viewport(s_gpu, 0, 0, s_vw, s_vh);
    // Crosshair — a simple white dot in the center (in game, console inactive,
    // cvar r_crosshair; console: toggle_crosshair)
    if (g->world_loaded && app_state() == APP_PLAY && !console_active()
        && cvar_get("r_crosshair") > 0.5f) {
        const float cx = s_vw * 0.5f, cy = s_vh * 0.5f;
        const float white[4] = { 1.0f, 1.0f, 1.0f, 0.9f };
        gpu_cap(s_gpu, GPU_CAP_DEPTH_TEST, 0);
        gpu_cap(s_gpu, GPU_CAP_BLEND, 1);
        gpu_blend_func(s_gpu, GPU_BLEND_SRC_ALPHA, GPU_BLEND_ONE_MINUS_SRC_ALPHA);
        // 3x3 px dot in the center
        hud_rect_draw(s_vw, s_vh, cx - 1.5f, cy - 1.5f, 3.0f, 3.0f, white);
    }

    // Mini-physics (Verlet) debug: particles are projected to screen space and
    // drawn as dots. Ragdoll is close: later bones will replace the dots.
    {
        PhysWorld *w = physics_world();
        float dbg[512 * 3];
        int ndbg = glb_anim_ragdoll_debug_particles(dbg, 512);
        if ((w->np > 0 || ndbg > 0) && g->world_loaded) {
            const float *vp = s_rc.viewproj;
            gpu_cap(s_gpu, GPU_CAP_DEPTH_TEST, 0);
            gpu_cap(s_gpu, GPU_CAP_BLEND, 1);
            gpu_blend_func(s_gpu, GPU_BLEND_SRC_ALPHA,
                           GPU_BLEND_ONE_MINUS_SRC_ALPHA);
            for (int i = 0; i < w->np; i++) {
                const float *p = w->part[i].pos;
                float clip[4] = {
                    vp[0]*p[0] + vp[4]*p[1] + vp[8]*p[2]  + vp[12],
                    vp[1]*p[0] + vp[5]*p[1] + vp[9]*p[2]  + vp[13],
                    vp[2]*p[0] + vp[6]*p[1] + vp[10]*p[2] + vp[14],
                    vp[3]*p[0] + vp[7]*p[1] + vp[11]*p[2] + vp[15],
                };
                if (clip[3] <= 0.01f) continue;      // behind the camera
                float sx = (clip[0] / clip[3] * 0.5f + 0.5f) * (float)s_vw;
                float sy = (0.5f - clip[1] / clip[3] * 0.5f) * (float)s_vh;
                float col[4] = { 0.3f, 0.9f, 1.0f, 0.9f };   // free particle
                if (w->part[i].inv_mass <= 0.0f) {
                    col[0] = 1.0f; col[1] = 0.6f; col[2] = 0.2f;   // pinned particle
                } else if (phys_body_index_of_part(i) >= 0) {
                    // rigid-body particle: otherwise a box is indistinguishable from a rope
                    col[0] = 0.9f; col[1] = 0.3f; col[2] = 0.9f;
                }
                hud_rect_draw(s_vw, s_vh, sx - 3.0f, sy - 3.0f, 6.0f, 6.0f, col);
            }
            // Ragdoll particles — yellow
            for (int i = 0; i < ndbg; i++) {
                const float *p = &dbg[i * 3];
                float clip[4] = {
                    vp[0]*p[0] + vp[4]*p[1] + vp[8]*p[2]  + vp[12],
                    vp[1]*p[0] + vp[5]*p[1] + vp[9]*p[2]  + vp[13],
                    vp[2]*p[0] + vp[6]*p[1] + vp[10]*p[2] + vp[14],
                    vp[3]*p[0] + vp[7]*p[1] + vp[11]*p[2] + vp[15],
                };
                if (clip[3] <= 0.01f) continue;
                float sx = (clip[0] / clip[3] * 0.5f + 0.5f) * (float)s_vw;
                float sy = (0.5f - clip[1] / clip[3] * 0.5f) * (float)s_vh;
                const float col[4] = { 1.0f, 0.9f, 0.2f, 0.9f };
                hud_rect_draw(s_vw, s_vh, sx - 2.0f, sy - 2.0f, 4.0f, 4.0f, col);
            }
        }
    }

    // Map message (from triggers/use) — screen center.
    // The timer (g_ui_msg_t) is driven by game.c in game_update.
    if (g->ui_msg_t > 0.0f && g->world_loaded && app_state() == APP_PLAY) {
        float mw = hud_text_width(g->ui_msg);
        const float bg[4] = { 0.05f, 0.06f, 0.10f, 0.55f };
        const float fg[4] = { 0.9f, 0.95f, 1.0f, 1.0f };
        // We enable blending OURSELVES: hud_text_draw/hud_rect_draw do not set
        // GL state by contract (hud.h). The state used to "leak" from the crosshair
        // block, so with r_crosshair 0 the message drew without alpha —
        // opaque background and filled-in "holes" in the letters.
        gpu_cap(s_gpu, GPU_CAP_DEPTH_TEST, 0);
        gpu_cap(s_gpu, GPU_CAP_BLEND, 1);
        gpu_blend_func(s_gpu, GPU_BLEND_SRC_ALPHA, GPU_BLEND_ONE_MINUS_SRC_ALPHA);
        float my = s_vh * 0.62f;
        hud_rect_draw(s_vw, s_vh, (s_vw - mw) * 0.5f - 16.0f, my - 2.0f,
                      mw + 32.0f, hud_text_line_height() + 8.0f, bg);
        hud_text_draw(s_vw, s_vh, (s_vw - mw) * 0.5f,
                      my + hud_text_ascent(), g->ui_msg, fg);
        gpu_cap(s_gpu, GPU_CAP_BLEND, 0);
    }

    // --- HP: health bar (always in game), damage flash, death screen ---
    if (g->world_loaded && app_state() == APP_PLAY) {
        const Health *h = &g->player.hp;
        gpu_cap(s_gpu, GPU_CAP_DEPTH_TEST, 0);
        gpu_cap(s_gpu, GPU_CAP_BLEND, 1);
        gpu_blend_func(s_gpu, GPU_BLEND_SRC_ALPHA, GPU_BLEND_ONE_MINUS_SRC_ALPHA);

        // Full-screen red damage flash (fades over time).
        // Living: HP bar and damage flash. Dead — death screen only.
        if (!h->dead) {
        if (h->hurt_flash > 0.001f) {
            const float fl[4] = { 0.75f, 0.05f, 0.05f, h->hurt_flash * 0.38f };
            hud_rect_draw(s_vw, s_vh, 0.0f, 0.0f, (float)s_vw, (float)s_vh, fl);
        }

        // HP bar at the bottom left.
        float frac = health_frac(h);
        float bx = 24.0f, bh = 20.0f, bw = 240.0f;
        float by = (float)s_vh - 46.0f;
        const float border[4] = { 0.0f, 0.0f, 0.0f, 0.55f };
        const float back[4]   = { 0.12f, 0.12f, 0.14f, 0.62f };
        // Fill color: red (low) -> yellow -> green (high).
        float fr, fg2, fb = 0.10f;
        if (frac > 0.5f) { fr = (1.0f - frac) * 2.0f; fg2 = 0.85f; }
        else             { fr = 0.90f;                fg2 = frac * 2.0f * 0.85f; }
        const float fill[4] = { fr, fg2, fb, 0.92f };
        hud_rect_draw(s_vw, s_vh, bx - 2.0f, by - 2.0f, bw + 4.0f, bh + 4.0f, border);
        hud_rect_draw(s_vw, s_vh, bx, by, bw, bh, back);
        if (frac > 0.0f)
            hud_rect_draw(s_vw, s_vh, bx, by, bw * frac, bh, fill);
        char hpbuf[32];
        snprintf(hpbuf, sizeof hpbuf, "HP %d", (int)(h->hp + 0.5f));
        const float hptext[4] = { 0.96f, 0.98f, 1.0f, 1.0f };
        hud_text_draw_small(s_vw, s_vh, bx + 8.0f,
                            by + (bh - hud_text_line_height_small()) * 0.5f
                              + hud_text_ascent_small(), hpbuf, hptext);
        }

        // Death screen: gradual fade to black (~0.5 s after the killing blow,
        // not instant) with a red tint, caption and respawn countdown.
        if (h->dead) {
            // Time since death grows from 0; the fade-in takes ~0.4 s
            // (independent of respawn_delay length). dp — fraction of the way to respawn.
            float dl = h->respawn_delay > 0.001f ? h->respawn_delay : 1.0f;
            float td = dl - h->respawn_t; if (td < 0.0f) td = 0.0f;
            float fade = td / 0.4f; if (fade > 1.0f) fade = 1.0f;
            float dp = td / dl; if (dp > 1.0f) dp = 1.0f;
            // Darkening with a red tint (blood -> black towards the end).
            const float dim[4] = { 0.18f * (1.0f - dp), 0.0f, 0.0f,
                                   0.66f * fade };
            hud_rect_draw(s_vw, s_vh, 0.0f, 0.0f, (float)s_vw, (float)s_vh, dim);
            const char *msg = "ВЫ ПОГИБЛИ";
            float mw2 = hud_text_width_big(msg);
            const float dc[4] = { 0.92f, 0.18f, 0.18f, fade };
            hud_text_draw_big(s_vw, s_vh, (s_vw - mw2) * 0.5f,
                              s_vh * 0.40f + hud_text_ascent_big(), msg, dc);
            // Subtitle: countdown to respawn.
            char sub[48];
            float secs = h->respawn_t; if (secs < 0.0f) secs = 0.0f;
            snprintf(sub, sizeof sub, "Возрождение через %.0f...", secs + 0.99f);
            float sw = hud_text_width(sub);
            const float sc[4] = { 0.85f, 0.85f, 0.88f, fade };
            hud_text_draw(s_vw, s_vh, (s_vw - sw) * 0.5f,
                          s_vh * 0.40f + hud_text_line_height_big() * 0.95f
                            + hud_text_ascent(), sub, sc);
        }
        gpu_cap(s_gpu, GPU_CAP_BLEND, 0);
    }

    // snd_debug: panel at the top right — which sounds are playing now
    // plus a step counter (audio debug). Works in the menu and in game.
    if (cvar_get("snd_debug") > 0.5f) {
        extern int g_step_calls;    // step counter (camera.c)
        char sndbuf[128], line[192];
        audio_active_names(sndbuf, sizeof sndbuf);
        snprintf(line, sizeof line, "SND: %s | steps: %d",
                 sndbuf[0] ? sndbuf : "-", g_step_calls);
        gpu_cap(s_gpu, GPU_CAP_DEPTH_TEST, 0);
        gpu_cap(s_gpu, GPU_CAP_BLEND, 1);
        gpu_blend_func(s_gpu, GPU_BLEND_SRC_ALPHA, GPU_BLEND_ONE_MINUS_SRC_ALPHA);
        const float bg[4] = { 0.05f, 0.06f, 0.10f, 0.75f };
        hud_rect_draw(s_vw, s_vh, (float)s_vw - 470.0f, 8.0f, 462.0f, 26.0f, bg);
        // Text centered in the box: baseline = top + ascent
        const float fg[4] = { 0.85f, 0.95f, 1.0f, 1.0f };
        hud_text_draw_small(s_vw, s_vh, (float)s_vw - 458.0f,
                            8.0f + hud_text_ascent_small(), line, fg);
    }
    // profiler: panel at the bottom right — polygons, vertices, draw calls and
    // per-module time (profiler command). Shows what eats the most.
    if (cvar_get("profiler") > 0.5f && g->world_loaded) {
        char line[256];
        float total = prof_total_ms();
        float gpu_total = prof_gpu_total_ms();
        float frame = dt * 1000.0f;
        float fps = frame > 0.001f ? 1000.0f / frame : 0.0f;
        gpu_cap(s_gpu, GPU_CAP_DEPTH_TEST, 0);
        gpu_cap(s_gpu, GPU_CAP_BLEND, 1);
        gpu_blend_func(s_gpu, GPU_BLEND_SRC_ALPHA, GPU_BLEND_ONE_MINUS_SRC_ALPHA);
        const float bg[4] = { 0.05f, 0.06f, 0.10f, 0.75f };
        const float fg[4] = { 0.85f, 0.95f, 1.0f, 1.0f };
        const float dim[4] = { 0.62f, 0.66f, 0.74f, 1.0f };
        const float hot[4] = { 1.0f, 0.72f, 0.3f, 1.0f };
        float fh = hud_text_line_height_small();
        float y = (float)s_vh - 8.0f - fh * 9.0f;
        hud_rect_draw(s_vw, s_vh, (float)s_vw - 390.0f, y - 8.0f, 382.0f,
                      fh * 9.0f + 16.0f, bg);
        float x = (float)s_vw - 378.0f;
        snprintf(line, sizeof line, "Frame %.1f ms  (%.0f fps)", frame, fps);
        hud_text_draw_small(s_vw, s_vh, x, y + hud_text_ascent_small(), line, fg);
        y += fh;
        snprintf(line, sizeof line, "CPU slots %.1f | GPU slots %.1f ms", total, gpu_total);
        hud_text_draw_small(s_vw, s_vh, x, y + hud_text_ascent_small(), line, dim);
        y += fh;
        snprintf(line, sizeof line, "%s | %dx%d %.2fx", lowend_profile_name(),
                 s_rsw, s_rsh, lowend_profile_scale());
        hud_text_draw_small(s_vw, s_vh, x, y + hud_text_ascent_small(), line, fg);
        y += fh;
        snprintf(line, sizeof line, "Tris %d | Verts %d | Draws %d | GLskip %d",
                 g_prof_tris, g_prof_verts, g_prof_draws, g_prof_state_skipped);
        hud_text_draw_small(s_vw, s_vh, x, y + hud_text_ascent_small(), line, fg);
        y += fh;
        // per-module rows (nonzero only, in descending GPU-time order)
        int order[PROF_N];
        for (int i = 0; i < PROF_N; i++) order[i] = i;
        for (int a = 0; a < PROF_N; a++)
            for (int b = a + 1; b < PROF_N; b++)
                if (prof_gpu_ms(order[b]) > prof_gpu_ms(order[a])) {
                    int t = order[a]; order[a] = order[b]; order[b] = t;
                }
        for (int k = 0; k < PROF_N && y < (float)s_vh - 8.0f - fh; k++) {
            int i = order[k];
            float cpu = prof_ms(i);
            float gpu = prof_gpu_ms(i);
            if (cpu <= 0.01f && gpu <= 0.01f) continue;
            float cpu_pct = total > 0.01f ? 100.0f * cpu / total : 0.0f;
            float gpu_pct = gpu_total > 0.01f ? 100.0f * gpu / gpu_total : 0.0f;
            float pct = cpu_pct > gpu_pct ? cpu_pct : gpu_pct;
            snprintf(line, sizeof line, "%-10s CPU %5.2f | GPU %5.2f ms",
                     prof_name(i), cpu, gpu);
            const float *c = (pct >= 25.0f) ? hot : (pct >= 10.0f ? fg : dim);
            hud_text_draw_small(s_vw, s_vh, x, y + hud_text_ascent_small(), line, c);
            y += fh;
        }
    }
}

// --- Cubemap capture: render 6 faces at a position and save PNGs --------------
// Renders the reflection cubemap from point pos (size, capped at 512) with the
// same correct logic as the live reflection pass (renderer_cubemap_render_at),
// then reads each face straight back, saving 6 PNGs into cubemaps/ (px..nz).
// The captured cubemap also becomes the live one (water starts reflecting it).
void renderer_cubemap_capture_at(Game *g, const float *pos, int size)
{
    if (!s_refl_cubemap || size <= 0 || !g || !pos) return;
    if (size > 512) size = 512;   // cap
    if (size < 16) size = 16;

    // If the size differs from the current live cubemap — recreate for it
    if (size != s_refl_cubemap_size)
        make_cubemap_fbo(size);

    // Render 6 faces from point pos (correct orientation/clip/no sun in rain)
    renderer_cubemap_render_at(g, g->root, pos);

    static const char *face_names[6] = { "px", "nx", "py", "ny", "pz", "nz" };

    // Read each face back from its FBO and save a PNG
    int orig_fbo = gpu_fbo_current(s_gpu);
    for (int face = 0; face < 6; face++) {
        gpu_bind_framebuffer(s_gpu, s_refl_cubemap_fbo[face]);
        gpu_viewport(s_gpu, 0, 0, size, size);
        unsigned char *px = malloc((size_t)size * size * 3);
        unsigned char *flip = malloc((size_t)size * size * 3);
        if (px && flip) {
            gpu_read_rgb(s_gpu, 0, 0, size, size, px);
            // PNG stores rows top-to-bottom, GL returns them bottom-to-top — Y flip
            size_t row = (size_t)size * 3;
            for (int y = 0; y < size; y++)
                memcpy(flip + (size_t)y * row, px + (size_t)(size - 1 - y) * row, row);

            char path[256];
            time_t now = time(NULL);
            struct tm *tm = localtime(&now);
            snprintf(path, sizeof path, "cubemaps/cubemap_%04d%02d%02d_%02d%02d%02d_%s.png",
                     tm ? tm->tm_year + 1900 : 2026, tm ? tm->tm_mon + 1 : 1,
                     tm ? tm->tm_mday : 1, tm ? tm->tm_hour : 0,
                     tm ? tm->tm_min : 0, tm ? tm->tm_sec : 0,
                     face_names[face]);
#ifdef _WIN32
            _mkdir("cubemaps");
#else
            mkdir("cubemaps", 0755);
#endif
            if (png_save(path, size, size, flip) == 0)
                SDL_Log("cubemap_capture: saved %s (%dx%d)", path, size, size);
            else
                SDL_Log("cubemap_capture: failed to save %s", path);
        }
        free(px);
        free(flip);
    }
    gpu_bind_framebuffer(s_gpu, (GpuFbo)orig_fbo);
    gpu_viewport(s_gpu, s_rc.view_vp[0], s_rc.view_vp[1],
                 s_rc.view_vp[2], s_rc.view_vp[3]);

    SDL_Log("cubemap_capture: done at (%.0f, %.0f, %.0f) size %d", pos[0], pos[1], pos[2], size);
}
