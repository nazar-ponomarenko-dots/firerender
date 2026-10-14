// ============================================================================
//  world.c — world map loading, height/normal computation (see world.h)
// ============================================================================
#include "world.h"

#include "assets.h"
#include "gl_loader.h"
#include "mapdata.h"
#include "resources.h"
#include "splat.h"

#include "stb_image.h"
#include <math.h>
#include <stdint.h>
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>

// World textures and the map's average brightness (owned by this module)
static GLuint s_tex_height, s_tex_base, s_tex_norm;
static float s_avg_lum;

float frnd(unsigned int *s)
{
    *s ^= *s << 13;
    *s ^= *s >> 17;
    *s ^= *s << 5;
    return (float)(*s & 0xFFFFFF) / 16777216.0f;
}

float height_curve(const WorldState *w, float t)
{
    if (t <= 0.0f) return w->min_height;
    if (t >= 1.0f) return w->max_height;

    // The whole [0..1] range consists of smoothstep segments. Smoothstep has
    // zero slope at both ends, so at the SEGMENT JOINTS (band_t0, band_t1)
    // the slopes match — transitions are smooth (C1), without sharp "edges"/
    // kinks, as the old "linear tail + smoothstep" joint had (it had a kink).
    // Degenerate parameters (band_t0==0, band_t1==band_t0, band_t1==1 —
    // terrain.cfg's curve) give division by zero and NaN in the terrain: skip
    // a zero-width segment (take its top height right away).
    float s;
    if (t < w->band_t0) {                                  // below the playable band
        if (w->band_t0 <= 0.0f) return w->band_h0;
        s = t / w->band_t0;
        s = s * s * (3.0f - 2.0f * s);
        return w->min_height + s * (w->band_h0 - w->min_height);
    }
    if (t <= w->band_t1) {                                 // playable band
        float span = w->band_t1 - w->band_t0;
        if (span <= 0.0f) return w->band_h0;
        s = (t - w->band_t0) / span;
        s = s * s * (3.0f - 2.0f * s);
        return w->band_h0 + s * (w->band_h1 - w->band_h0);
    }
    if (1.0f - w->band_t1 <= 0.0f) return w->band_h1;
    s = (t - w->band_t1) / (1.0f - w->band_t1);            // above the band
    s = s * s * (3.0f - 2.0f * s);
    return w->band_h1 + s * (w->max_height - w->band_h1);
}

float sample_height(const WorldState *w, float wx, float wz)
{
    // World coordinate == index of the height-map texel (the GRID_DIV=1 grid
    // puts a vertex at world coordinate i, which reads texel i).
    // So we take texel floor(wx): the collision then matches what the terrain
    // vertex shader draws (GL_LINEAR between the live texels).
    int hw = w->tex_w, hh = w->tex_h;
    if (wx < 0.0f) wx = 0.0f;
    if (wz < 0.0f) wz = 0.0f;
    if (wx > w->world_w) wx = w->world_w;
    if (wz > w->world_d) wz = w->world_d;
    int x0 = (int)floorf(wx), y0 = (int)floorf(wz);   // edges: like the texture (clamp)
    if (x0 > hw - 1) x0 = hw - 1;
    if (y0 > hh - 1) y0 = hh - 1;
    int x1 = x0 + 1, y1 = y0 + 1;
    if (x1 > hw - 1) x1 = hw - 1;
    if (y1 > hh - 1) y1 = hh - 1;
    float tx = wx - x0, ty = wz - y0;
    // We read SMOOTHED heights from the float array w->heights (float
    // precision, without the 8-bit quantization that produced banded steps on
    // slopes). Render shaders use the same values via GL_R16 — collision and
    // picture match, without banding.
    const float *h = w->heights;
    float v00 = h[y0*hw + x0], v10 = h[y0*hw + x1];
    float v01 = h[y1*hw + x0], v11 = h[y1*hw + x1];
    float vx0 = v00 + (v10 - v00) * tx;
    float vx1 = v01 + (v11 - v01) * tx;
    return vx0 + (vx1 - vx0) * ty;
}

float world_line_open(const WorldState *w, const float from[3], const float to[3])
{
    if (!w || !w->heights || !from || !to) return 1.0f;
    float dx = to[0] - from[0];
    float dy = to[1] - from[1];
    float dz = to[2] - from[2];
    float len = sqrtf(dx * dx + dy * dy + dz * dz);
    if (len < 2.0f) return 1.0f;      // shorter than a measurement step — nothing to occlude with
    // Step of 2.5 m: finer than the map pixel (1 m) is pointless to check, and
    // over 400 m that is 160 bilinear samples per channel — fractions of a ms.
    int steps = (int)(len / 2.5f);
    if (steps < 2) steps = 2;
    int under = 0;
    for (int i = 1; i < steps; i++) {   // segment endpoints not counted: their own height
        float t = (float)i / (float)steps;
        float y = from[1] + dy * t;
        if (sample_height(w, from[0] + dx * t, from[2] + dz * t) > y + 0.5f)
            under++;
    }
    return 1.0f - (float)under / (float)(steps - 1);
}

// Height smoothing: separable box blur. Radius is in pixels (1 pixel =
// 1 meter). Implemented via prefix sums — O(N) per pass regardless of the
// radius, so wide windows (tens of meters) can smooth transitions cheaply.
// Edges are clamped (like in a texture, no wrap).
static void blur_heights(float *h, int w, int hh, int passes, int radius)
{
    if (radius < 1) radius = 1;
    int n = w > hh ? w : hh;
    float *tmp = malloc((size_t)w * hh * sizeof(float));
    float *pre = malloc((size_t)n * sizeof(float));
    if (!tmp || !pre) {   // OOM — skip smoothing (better than crashing)
        free(tmp); free(pre);
        SDL_Log("terrain: blur skipped (no memory)");
        return;
    }
    for (int pass = 0; pass < passes; pass++) {
        // --- horizontal pass (rows) ---
        for (int y = 0; y < hh; y++) {
            for (int x = 0; x < w; x++)
                pre[x] = (x ? pre[x-1] : 0.0f) + h[y*w + x];
            for (int x = 0; x < w; x++) {
                int l = x - radius; if (l < 0) l = 0;
                int r = x + radius; if (r > w - 1) r = w - 1;
                float s = pre[r] - (l ? pre[l-1] : 0.0f);
                tmp[y*w + x] = s / (float)(r - l + 1);
            }
        }
        // --- vertical pass (columns) ---
        for (int x = 0; x < w; x++) {
            for (int y = 0; y < hh; y++)
                pre[y] = (y ? pre[y-1] : 0.0f) + tmp[y*w + x];
            for (int y = 0; y < hh; y++) {
                int l = y - radius; if (l < 0) l = 0;
                int r = y + radius; if (r > hh - 1) r = hh - 1;
                float s = pre[r] - (l ? pre[l-1] : 0.0f);
                h[y*w + x] = s / (float)(r - l + 1);
            }
        }
    }
    free(tmp);
    free(pre);
}

// Slope steepness limiting (thermal erosion). If the gradient between
// neighbors exceeds tan(slope_deg), the excess flows over to the neighbor.
// We iterate WHILE THERE IS SOMETHING to dissolve (it converges), so in the
// end NO gradient exceeds the given one — sharp transitions are excluded by
// construction. passes is a safety ceiling.
static void slope_limit_heights(float *h, int w, int hh,
                                int passes, float slope_deg)
{
    float maxdh = tanf(slope_deg * (float)M_PI / 180.0f); // meters per meter
    if (maxdh <= 1e-4f) return;
    const float eps = 0.001f;   // threshold: don't chase a tiny steepness tail forever
    const int dx[4] = { 1, -1, 0, 0 };
    const int dy[4] = { 0, 0, 1, -1 };
    for (int pass = 0; pass < passes; pass++) {
        int changed = 0;
        for (int y = 0; y < hh; y++) {
            for (int x = 0; x < w; x++) {
                float v = h[y*w + x];
                for (int k = 0; k < 4; k++) {
                    int nx = x + dx[k], ny = y + dy[k];
                    if (nx < 0 || nx >= w || ny < 0 || ny >= hh) continue;
                    float d = v - h[ny*w + nx];
                    if (d > maxdh + eps) {
                        float excess = (d - maxdh) * 0.5f;
                        h[ny*w + nx] += excess;
                        h[y*w + x]   -= excess;
                        v = h[y*w + x];
                        changed = 1;
                    }
                }
            }
        }
        if (!changed) break;   // converged: no transition steeper than maxdh anywhere
    }
}

int world_load(WorldState *w, WorldTextures *out)
{
    // Height curve and smoothing come from map/map.json (terrain = { ... })
    {
        MapTerrain t;
        mapdata_terrain(&t);
        w->min_height = t.min_height;
        w->max_height = t.max_height;
        w->band_t0 = t.band_t0;
        w->band_t1 = t.band_t1;
        w->band_h0 = t.band_h0;
        w->band_h1 = t.band_h1;
        w->terrain_blur = t.blur > 0 ? t.blur : 0;
        w->terrain_blur_radius = t.blur_radius > 0 ? t.blur_radius : 2;
        w->terrain_erosion = t.erosion > 0 ? t.erosion : 0;
        w->terrain_erosion_slope = t.erosion_slope > 0.0f ? t.erosion_slope : 45.0f;
        SDL_Log("Terrain cfg: h %.0f..%.0f, band t %.2f..%.2f h %.0f..%.0f, "
                "blur %dx%d, erosion %d (<%.0f deg)",
                w->min_height, w->max_height, w->band_t0, w->band_t1,
                w->band_h0, w->band_h1, w->terrain_blur, w->terrain_blur_radius,
                w->terrain_erosion, w->terrain_erosion_slope);
    }
    // Height map: from the map pack (zip) or from the map/ folder
    char map_path[512];
    if (resources_pak_is_open())
        snprintf(map_path, sizeof map_path, "%s", mapdata_image());
    else
        snprintf(map_path, sizeof map_path, "map/%s", mapdata_image());
    int mw = 0, mh = 0;
    // The height map is geometry data, NOT a render texture: substituting
    // notfound would break the terrain (all heights one color = flat world).
    // strict — an honest failure when the map-of-the-map.w exists nowhere.
    unsigned char *img = resources_image_strict(map_path, &mw, &mh, STBI_rgb);
    if (!img) return -1;
    if (mw <= 0 || mh <= 0 || (size_t)mw > SIZE_MAX / (size_t)mh ||
        (size_t)mw * (size_t)mh > SIZE_MAX / (3 * sizeof(float))) {
        stbi_image_free(img);
        SDL_Log("World: invalid or oversized map dimensions %dx%d", mw, mh);
        return -1;
    }
    const size_t pixels = (size_t)mw * (size_t)mh;
    SDL_Log("Map loaded: %s (%dx%d)", map_path, mw, mh);

    // --- World from the map: 1 pixel = 1 meter -------------------------------------
    // World, texture and terrain-grid sizes depend on the map resolution.
    // A 2000x1125 map -> a 2000x1125 m world, a 6k map -> a 6k m world. The
    // grid is 1 quad per GRID_DIV pixels. Height is taken by the shader from
    // the texture (bilinearly), so the grid can be coarse — the picture is the
    // same while there are 16 times fewer triangles.
    w->tex_w = mw;
    w->tex_h = mh;
    w->world_w = (float)mw;
    w->world_d = (float)mh;
    w->grid_w = mw / GRID_DIV;
    w->grid_h = mh / GRID_DIV;
    if (w->grid_w < 64) w->grid_w = 64;
    if (w->grid_h < 64) w->grid_h = 64;
    if (w->grid_w > GRID_MAX) w->grid_w = GRID_MAX;
    if (w->grid_h > GRID_MAX) w->grid_h = GRID_MAX;
    w->tex_per_quad = (float)mw / w->grid_w;
    SDL_Log("World: %.0fx%.0f m, grid %dx%d quads",
            w->world_w, w->world_d, w->grid_w, w->grid_h);

    // Brightness (height) and map color at full resolution
    float *lum = malloc(pixels * sizeof(float));
    unsigned char *color = malloc(pixels * 3);
    w->heights = malloc(pixels * sizeof(float));
    if (!lum || !color || !w->heights) {   // OOM — honest load failure
        free(lum); free(color); free(w->heights);
        stbi_image_free(img);
        w->heights = NULL;                 // world_free tolerates NULL
        SDL_Log("World: no memory for %dx%d map", mw, mh);
        return -1;
    }
    for (size_t i = 0; i < pixels; i++) {
        const unsigned char *px = img + i * 3;
        color[i*3 + 0] = px[0];
        color[i*3 + 1] = px[1];
        color[i*3 + 2] = px[2];
        lum[i] = (0.299f*px[0] + 0.587f*px[1] + 0.114f*px[2]) / 255.0f;
    }
    s_avg_lum = 0;
    for (size_t i = 0; i < pixels; i++) s_avg_lum += lum[i];
    s_avg_lum /= (float)pixels;
    stbi_image_free(img);
    img = NULL;

    // --- Heights: curve with the playable band + smoothing + normals -------------
    // Heights are computed in C (height_curve), so the normal map, wheat
    // placement and HUD always agree with the terrain. blur_heights
    // "interpolates" the map: corners and steps get smoothed even before
    // rendering. 1 px = 1 m.
    for (size_t i = 0; i < pixels; i++)
        w->heights[i] = height_curve(w, 1.0f - lum[i]);
    // Smoothing transitions + limiting steepness: blur first to remove
    // steps, then erode so gradients never exceed erosion_slope (climbable
    // physically, see sv_maxslope). Both passes affect the visible terrain and
    // the collision alike (single height source).
    blur_heights(w->heights, mw, mh, w->terrain_blur, w->terrain_blur_radius);
    if (w->terrain_erosion > 0)
        slope_limit_heights(w->heights, mw, mh, w->terrain_erosion,
                            w->terrain_erosion_slope);
    // ROADS CUT THE TERRAIN: the roadbed is aligned to the longitudinal
    // profile AFTER blur/erosion and BEFORE normals — normals, collision
    // (sample_height), vegetation and geometry all see the same leveled terrain.
    splat_flatten_roads(w);
    unsigned char *nm = malloc(pixels * 3);
    if (!nm) {
        free(lum); free(color);
        SDL_Log("World: no memory for normal map");
        return -1;                         // w->heights still reaches world_free
    }
    for (int y = 0; y < mh; y++) {
        for (int x = 0; x < mw; x++) {
            int xl = x > 0 ? x - 1 : x;                  // edges — no wrap,
            int xr = x < mw - 1 ? x + 1 : x;             // like in a texture
            int yu = y > 0 ? y - 1 : y;
            int yd = y < mh - 1 ? y + 1 : y;
            float nx = -(w->heights[y*mw + xr] - w->heights[y*mw + xl]) * 0.5f;
            float nz = -(w->heights[yd*mw + x]  - w->heights[yu*mw + x]) * 0.5f;
            float len = sqrtf(nx*nx + nz*nz + 1.0f);
            unsigned char *outp = nm + (y*mw + x)*3;
            outp[0] = (unsigned char)((nx/len * 0.5f + 0.5f) * 255.0f);
            outp[1] = (unsigned char)((1.0f/len * 0.5f + 0.5f) * 255.0f);
            outp[2] = (unsigned char)((nz/len * 0.5f + 0.5f) * 255.0f);
        }
    }
    free(lum);
    // Map pixels stay owned by the resource cache — do not free
    // (see resources.h; the stb buffer lives until resources_shutdown)

    // Heights -> GL_R16 texture (precision 1/65536) instead of GL_R8: 8-bit
    // quantization over a 500 m range produced banded steps (~2 m/level) on
    // slopes — "sharp transitions". 16 bits remove them completely.
    // Bilinear filtering + LINEAR give a smooth surface; the collision
    // (sample_height) reads the same values from the float array — all matches.
    unsigned short *hb = malloc(pixels * sizeof(unsigned short));
    if (!hb) {
        free(color); free(nm);            // lum already freed above
        SDL_Log("World: no memory for height texture");
        return -1;
    }
    // Degenerate curve (max==min in map.json): division by zero -> NaN.
    // Clamp the range to at least 1 m so the height quantizes correctly.
    float hspan = w->max_height - w->min_height;
    if (hspan < 1.0f) hspan = 1.0f;
    for (size_t i = 0; i < pixels; i++) {
        float t = (w->heights[i] - w->min_height) / hspan;
        if (t < 0) t = 0;
        if (t > 1) t = 1;
        hb[i] = (unsigned short)(t * 65535.0f);
    }
    glGenTextures(1, &s_tex_height);
    glBindTexture(GL_TEXTURE_2D, s_tex_height);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
#ifdef USE_GLES_PATH
    // GLES 3.0 does not guarantee EXT_texture_norm16. RG8 stores the same 16
    // bits: high byte in R, low in G; the shader restores the value.
    unsigned char *h_rg = malloc(pixels * 2);
    if (!h_rg) { free(hb); free(color); free(nm); return -1; }
    for (size_t i = 0; i < pixels; i++) {
        h_rg[i*2+0] = (unsigned char)(hb[i] >> 8);
        h_rg[i*2+1] = (unsigned char)(hb[i] & 255);
    }
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, mw, mh, 0, GL_RG,
                 GL_UNSIGNED_BYTE, h_rg);
    free(h_rg);
#else
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R16, mw, mh, 0, GL_RED,
                 GL_UNSIGNED_SHORT, hb);
#endif
    free(hb);

    s_tex_base = make_texture(mw, mh, color, GL_RGB, GL_CLAMP_TO_EDGE, 0); // minimap
    s_tex_norm = make_texture(mw, mh, nm, GL_RGB, GL_CLAMP_TO_EDGE, 1);
    free(color);
    free(nm);

    out->tex_height = s_tex_height;
    out->tex_base = s_tex_base;
    out->tex_norm = s_tex_norm;
    return 0;
}

void world_free(WorldState *w)
{
    free(w->heights);
    w->heights = NULL;
    GLuint texs[3] = { s_tex_height, s_tex_base, s_tex_norm };
    glDeleteTextures(3, texs);
    s_tex_height = s_tex_base = s_tex_norm = 0;
}

float world_avg_lum(void)
{
    return s_avg_lum;
}

// Height map texture (GL_R16, LINEAR) — for volumetric fog (haze near the
// ground). 0 until the world is loaded / once already freed.
GLuint world_height_tex(void)
{
    return s_tex_height;
}
