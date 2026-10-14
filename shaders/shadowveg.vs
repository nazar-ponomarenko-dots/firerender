// ============================================================================
//  ShadowVeg — vertex shader for the shadow map (vegetation)
// ----------------------------------------------------------------------------
//  Same instances as in wheat.vs, but positions are in light space —
//  grass and wheat cast real shadows onto the ground. Heights and slopes are
//  already in the instance attributes (no texture fetch) — the shadow pass is
//  cheap. Thinning is now done on the CPU (vegetation.c): dynamics follow the
//  same uThin rule ((id*2654435761)%100<50), statics — every 2nd.
//  uThin is always 0; the branch is kept for compatibility.
//  Wind is the same as in wheat.vs — shadows don't detach from the plants.
// ============================================================================

layout (location = 0) in vec3 aPos;    // local point (x, 0..1, z)
layout (location = 1) in vec2 aUV;     // sprite uv
layout (location = 2) in vec3 iPos;    // instance: plant position
layout (location = 3) in vec2 iN;      // instance: ground normal (nx, nz)
layout (location = 4) in vec4 iAttr;   // instance: yaw, scale, tint, layer
layout (location = 5) in float iLean;  // instance: sideways lean
layout (location = 6) in vec2 iSlope;  // instance: ground slope (dh/dx, dh/dz)

uniform mat4 uViewProj;                // view * projection FROM THE SUN
uniform int uThin;                     // 1 = thin out (dense vegetation)
uniform float uTime;                   // simulation time (wind)
uniform vec3 uCamPos;                  // player (for vegetation bending)
uniform float uWalk;                   // 1 = walking (push aside harder)
uniform float uDyn;                    // 1 = dynamic layer (around the camera)
uniform float uWindBoost;              // wetness 0..1: wind is stronger in rain
uniform float uGrassFall;              // grass density falloff distance (m)
uniform float uGrassFloorFrac;         // floor/total fraction — what grass thins to at the edge

out vec2 vUV;
flat out float vLayer;   // LOD is integer for the whole quad (do not interpolate)

// Species (iAttr.w): 0/1/2 — three grass kinds, 3 — dandelion, 4 — wheat
// (see wheat.vs). Wheat gets LOD 4..6, the rest — as is.
#define WHEAT_BASE 4.0
#define WHEAT_MARK 3.5

// Bend under the player and useful quad height — SHARED block with wheat.vs:
// shadow geometry must match the plant geometry exactly.
#include "veg_common.glsl"

void main() {
    // Thin dense vegetation (70 pcs/m2) in the shadow map by 40%:
    // the dense wheat-on-wheat shadows must stay solid, not a sparse
    // grid of halves.
    if (uThin == 1 && (uint(gl_InstanceID) * 2654435761u) % 100u >= 50u) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);   // out of range — culled
        return;
    }
    float ny = sqrt(max(1.0 - dot(iN, iN), 0.0));
    vec3 up = vec3(iN.x, ny, iN.y);
    float ca = cos(iAttr.x), sa = sin(iAttr.x);
    vec3 hvec = vec3(ca, 0.0, sa);
    // Wheat LOD and quad width — same as in wheat.vs (short 0.8 m crossfade
    // band, wide smooth thinning), the shadow repeats the sprite geometry.
    float layer = iAttr.w;
    float wx = 128.0;
    if (layer > WHEAT_MARK) {
        float dist = length(iPos.xz - uCamPos.xz);
        float ss1 = smoothstep(4.6, 5.4, dist);
        float ss2 = smoothstep(24.5, 25.5, dist);
        // The dynamic layer draws nothing past lod1 (as in wheat.vs): otherwise
        // lod2 would be visible twice — a dense ring at the dynamic edge + statics
        // across the whole map. The shadow must match the frame.
        if (uDyn > 0.5 && ss2 > 0.5) {
            gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
            return;
        }
        layer = WHEAT_BASE + ss1 + ss2;   // continuous layer 4..5..6
        wx = mix(128.0, 152.0, ss1);
        // lod2: immediately maximum width (x10) at all distances — as in
        // wheat.vs, the shadow repeats the sprite geometry.
        if (uDyn < 0.5) wx = mix(wx, 1280.0, ss2);
        // THINNING — same as in wheat.vs: per-plant position hash against
        // keep (by noise, no cells), same thresholds. Otherwise the shadow map
        // would keep plants that are absent from the frame. lod1: 1 of 4;
        // lod2: 3 of 16 — the same density at all distances.
        float k1 = smoothstep(4.0, 8.0, dist);
        float k2 = smoothstep(22.0, 30.0, dist);
        float keep = 1.0;
        keep = mix(keep, 0.25, k1);       // lod1: 1 of 4
        keep = mix(keep, 0.25, k2);       // lod2: 1 of 4, uniform everywhere
        float h = fract(sin(dot(iPos.xz + vec2(0.123, 0.456),
                                vec2(12.9898, 78.233))) * 43758.5453);
        if (h >= keep) {
            gl_Position = vec4(2.0, 2.0, 2.0, 1.0);   // out of range — culled
            return;
        }
    }
    // DYNAMIC grass: the same smooth distance falloff as in wheat.vs —
    // the grass shadow must match the visible grass (otherwise the shadow is denser).
    float gscale = 1.0;
    if (layer <= WHEAT_MARK && uDyn > 0.5 && uGrassFall > 0.01) {
        float dist = length(iPos.xz - uCamPos.xz);
        float tg = clamp(dist / uGrassFall, 0.0, 1.0);
        tg = tg * tg * (3.0 - 2.0 * tg);
        float keepg = mix(1.0, uGrassFloorFrac, tg);
        float hg = fract(sin(dot(iPos.xz + vec2(0.71, 0.13),
                                 vec2(12.9898, 78.233))) * 43758.5453);
        gscale = smoothstep(hg - 0.12, hg, keepg);
        if (gscale <= 0.001) {
            gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
            return;
        }
    }
    float wmul = wx / 128.0;
    // base on the terrain — as in wheat.vs (quad corners follow the ground slope)
    float hw = aPos.x * iAttr.y * wmul * gscale;
    vec3 wp = iPos + hvec * hw;
    wp.y = iPos.y + (iSlope.x * hvec.x + iSlope.y * hvec.z) * hw;
    // wheat (species 4+) is stretched 3x taller — as in wheat.vs
    float ht = iAttr.y * (iAttr.w > WHEAT_MARK ? 3.0 : 1.0) * gscale;
    // quad shortened to the opaque part of the picture — as in wheat.vs, otherwise
    // the shadow silhouette would be taller than the plant itself
    float cf = veg_content_frac(layer);
    float vis = ht * cf;
    float lc = cos(iLean), ls = sin(iLean);
    vec3 lt = hvec * (aPos.y * ls * iAttr.y * wmul * gscale)
            + up  * (aPos.y * lc * vis);
    wp += lt;
    // same wind and same bend under the player as in wheat.vs — shadows
    // don't detach from the plants
    float ph = dot(iPos.xz, vec2(0.71, 0.39)) + iAttr.x * 2.5;
    float w1 = sin(uTime * 1.15 + ph);
    float w2 = sin(uTime * 2.35 + dot(iPos.xz * 1.7, vec2(0.53, -0.81))
                   + iAttr.w * 4.0);
    float amp = (0.7 + 0.6 * fract(iPos.x * 0.37 + iPos.z * 0.91))
              * (1.0 + 0.6 * uWindBoost);   // rain = wind +60% amplitude
    float sway = (w1 * 0.07 + w2 * 0.035) * aPos.y * amp;
    wp.x += sway;
    wp.z += sway * 0.35 + w2 * 0.02 * aPos.y;
    // bend under the player — shared block veg_common.glsl (rotation around
    // the base, stronger for tall plants)
    vec2 bdir;
    float bang = veg_bend_angle(iPos.xz, iPos.y, uCamPos, vis, aPos.y, uWalk,
                                bdir);
    if (bang > 0.0) {
        float rise = aPos.y * lc * vis;
        wp.xz += bdir * (sin(bang) * rise);
        wp.y  -= (1.0 - cos(bang)) * rise;
    }
    vUV = vec2(aUV.x, 1.0 - (1.0 - aUV.y) * cf);   // UV for the shortened quad
    // Canvas 256x178, sprites at the top left. The piece is taken by the current
    // wx; the layer for the alpha test in shadowveg.fs is cut with GL_NEAREST —
    // the silhouette switch happens in the middle of the band, where in the
    // frame the sprite is already half-blended.
    if (layer < WHEAT_MARK)  vUV *= vec2(128.0 / 256.0, 128.0 / 178.0);
    else                     vUV *= vec2(wx / 256.0, 1.0);
    vLayer = layer;
    gl_Position = uViewProj * vec4(wp, 1.0);
}
