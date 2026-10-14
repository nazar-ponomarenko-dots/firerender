// ============================================================================
//  Vegetation — vegetation vertex shader (instancing)
// ----------------------------------------------------------------------------
//  Base mesh — a single quad (4 vertices, 6 indices). Instance attributes
//  (12 floats): iPos (x,y,z), iN (nx,nz — ground normal),
//  iAttr (yaw, scale, tint, layer), iLean (side lean),
//  iSlope (dh/dx, dh/dz — ground slope).
//  The plant stands ALONG THE GROUND NORMAL: on slopes it tilts with
//  the terrain. The base rests on the terrain per the slope (no heightmap
//  texture lookup — height and slopes are already computed in C), the tip
//  sways in the wind (uTime). All vegetation — 4 instanced draws
//  (static/dynamic layers x grass/wheat), one shader.
// ============================================================================

layout (location = 0) in vec3 aPos;    // local point (x, 0..1, z)
layout (location = 1) in vec2 aUV;     // sprite uv
layout (location = 2) in vec3 iPos;    // instance: plant position
layout (location = 3) in vec2 iN;      // instance: ground normal (nx, nz)
layout (location = 4) in vec4 iAttr;   // instance: yaw, scale, tint, species
layout (location = 5) in float iLean;  // instance: side lean
layout (location = 6) in vec2 iSlope;  // instance: ground slope (dh/dx, dh/dz)

uniform mat4 uViewProj;                // view * projection (from the Camera node)
uniform float uTime;                   // simulation time (wind)
uniform vec3 uCamPos;                  // player (for vegetation bending)
uniform float uWalk;                   // 1 = walking (push plants aside harder)
uniform float uDyn;                    // 1 = dynamic layer (around the camera)
uniform float uWindBoost;
uniform float uLodShift;        // 0 = LOD thresholds from the map; 1/2 — shifted toward the camera
                                // (low density: clumps widen/become bushier earlier)              // wetness 0..1: wind is stronger in rain
uniform float uGrassFall;       // distance where grass density falls off (m)
uniform float uGrassFloorFrac;  // floor/full ratio — what grass thins to near the edge

// Species (iAttr.w): 0/1/2 — three grass kinds, 3 — dandelion, 4 — wheat.
// Species index = base layer of the 2D sprite array. Wheat gets LOD
// (layers 4/5/6) as before — grass/dandelion are sampled as is.
#define WHEAT_BASE  4.0
#define WHEAT_MARK  3.5

// Bending under the player and sprite cutout — shared block with shadowveg.vs
// (the numbers must match, otherwise the shadow drifts off the plant).
#include "veg_common.glsl"

out vec2 vUV;
out vec2 vUVBase;   // sprite UV [0,1] without the canvas crop (for FS LOD blending)
out vec3 vWorldPos;
out vec3 vNormal;      // quad normal (for lighting)
out float vTint;
flat out float vLayer;   // integer LOD across the whole quad (do not interpolate)
out float vHeight;     // plant height in meters (for darkening the base)

void main() {
    float ny = sqrt(max(1.0 - dot(iN, iN), 0.0));
    vec3 up = vec3(iN.x, ny, iN.y);            // ground normal (up)
    float ca = cos(iAttr.x), sa = sin(iAttr.x);
    vec3 hvec = vec3(ca, 0.0, sa);             // quad width axis (by yaw)

    // Wheat LOD by distance to the player, WITHOUT hard jumps. Grass and
    // dandelion (layers 0..3) stay unchanged.
    float layer = iAttr.w;
    float wx = 128.0;                     // sprite width on the canvas (px)
    if (layer > WHEAT_MARK) {
        float dist = length(iPos.xz - uCamPos.xz);
        // Layer and sprite width transition through a SHORT band (0.8 m):
        // the crossfade is visible, but the band of translucent "ghosts" stays
        // narrow — plant edges remain crisp.
        // uLodShift: at low field density the LOD thresholds shift toward the
        // camera — each clump becomes wider/bushier sooner (lod1, lod2),
        // visually compensating for sparseness. Thinning shifts TOGETHER
        // with the stretch (it dampens the sprite area growth).
        float b1a = 4.6, b1b = 5.4, b2a = 24.5, b2b = 25.5;
        float k1a = 4.0, k1b = 8.0, k2a = 22.0, k2b = 30.0;
        if (uLodShift >= 0.5) { b1a = 0.0; b1b = 0.9; k1a = 0.4; k1b = 1.8; }
        if (uLodShift >= 1.5) { b2a = 3.0; b2b = 5.0; k2a = 2.4; k2b = 4.4; }
        float ss1 = smoothstep(b1a, b1b, dist);
        float ss2 = smoothstep(b2a, b2b, dist);
        // lod2 (stretched) is drawn only by the STATIC layer — it covers
        // the whole map. Dynamics beyond lod1 are redundant: otherwise lod2
        // shows twice (dense ring at the dynamic edge + stretched across
        // the whole map).
        // Dynamics stay at lod1 and vanish over the 24.5-25 m band.
        // EXCEPTION: uLodShift >= 2 (very sparse field) — lod2 is needed for
        // dynamics too, otherwise there is a hole in the ring before the static layer.
        if (uDyn > 0.5 && ss2 > 0.5 && uLodShift < 1.5) {
            gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
            return;
        }
        layer = WHEAT_BASE + ss1 + ss2;   // continuous layer 4..5..6
        wx = mix(128.0, 152.0, ss1);
        // lod2: immediately maximum width (x10 = 1280) at every distance
        // — the texture is stretched uniformly everywhere, holes invisible. Dynamics
        // need no stretch — they never reach lod2 (except shift 2).
        if (uDyn < 0.5 || uLodShift >= 1.5) wx = mix(wx, 1280.0, ss2);
        // THINNING — PER PLANT: hash of the point position against keep.
        // Points die independently and at random — "by noise", no
        // cells, grids or holes. lod1: 1 of 4; lod2: 3 of 16 —
        // SAME density across the whole lod2 range.
        float k1 = smoothstep(k1a, k1b, dist);
        float k2 = smoothstep(k2a, k2b, dist);
        float keep = 1.0;
        keep = mix(keep, 0.25, k1);       // lod1: 1 of 4
        keep = mix(keep, 0.25, k2);       // lod2: 1 of 4, uniform everywhere
        float h = fract(sin(dot(iPos.xz + vec2(0.123, 0.456),
                                vec2(12.9898, 78.233))) * 43758.5453);
        if (h >= keep) {
            gl_Position = vec4(2.0, 2.0, 2.0, 1.0);   // beyond range — culled
            return;
        }
    }
    // GRASS/DANDELION (layers 0..3), DYNAMIC layer: densely generated over
    // the whole radius, and the density falloff toward the edge is done HERE,
    // smoothly — like wheat. Previously chunk streaming changed grass density
    // (0.30 plants/m² steps, budget 2 chunks/frame) — you could see grass
    // "topping up" in steps (jerky). Now each chunk keeps full density, and the
    // shader drops the excess by distance per frame: a blade does not pop in,
    // it GROWS via scale (a 0.12 band around its hash). Statics (uDyn<0.5,
    // floor across the whole map) are untouched.
    float gscale = 1.0;
    if (layer <= WHEAT_MARK && uDyn > 0.5 && uGrassFall > 0.01) {
        float dist = length(iPos.xz - uCamPos.xz);
        float tg = clamp(dist / uGrassFall, 0.0, 1.0);
        tg = tg * tg * (3.0 - 2.0 * tg);        // the same smoothstep as in C
        float keepg = mix(1.0, uGrassFloorFrac, tg);
        float hg = fract(sin(dot(iPos.xz + vec2(0.71, 0.13),
                                 vec2(12.9898, 78.233))) * 43758.5453);
        gscale = smoothstep(hg - 0.12, hg, keepg);
        if (gscale <= 0.001) {
            gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
            return;
        }
    }
    // THE QUAD GROWS WITH THE LOD TEXTURE: lod1 is wider than lod0 (152/128),
    // lod2 stretches smoothly from 152 to x10 (1280) toward the visibility
    // edge — in the distance the sprite blurs into a solid field, no holes.
    // The multiplier is wx, computed above.
    float wmul = wx / 128.0;

    // horizontal offset of the vertex from the plant center
    // gscale (1.0 for wheat/statics) smoothly shrinks a dynamic blade toward the edge
    float hw = aPos.x * iAttr.y * wmul * gscale;
    vec3 wp = iPos + hvec * hw;

    // THE PLANT BASE LIES EXACTLY ON THE TERRAIN: the height under the quad
    // corners comes from the ground slope (iSlope, computed from the same
    // quantized heights as the terrain) — corners neither "float" nor get
    // buried on slopes, and the vertex shader needs no texture lookups.
    // iPos.y — exactly the ground height (sinking and bounce removed in
    // vegetation.c), so the sprite bottom sits right on the terrain mesh.
    wp.y = iPos.y + (iSlope.x * hvec.x + iSlope.y * hvec.z) * hw;

    // up along the ground normal + tip lean scaled by width (as in real life).
    // Wheat (species 4) is stretched 3x in height — a narrow tall ear.
    float ht = iAttr.y * (iAttr.w > WHEAT_MARK ? 3.0 : 1.0) * gscale;
    // THE QUAD IS SHORTENED TO THE OPAQUE PART OF THE IMAGE (see veg_common.glsl):
    // the top of the sprites is empty, and earlier this third of the area was
    // rasterized only to hit discard. vis — the VISIBLE plant height; on screen
    // the pixels are the same, fewer fragments (~30% for grass, ~10% for wheat).
    float cf = veg_content_frac(layer);
    float vis = ht * cf;
    float lc = cos(iLean), ls = sin(iLean);
    vec3 lt = hvec * (aPos.y * ls * iAttr.y * wmul * gscale)
            + up  * (aPos.y * lc * vis);
    wp += lt;

    // WIND, not monotonous: two sines with separated frequencies
    // (1.15 and 2.35 rad/s), phases — from the world position AND the plant
    // rotation, amplitude — its own per clump (0.7..1.3x). The second sine
    // bends in a different direction — waves look alive, not synchronized.
    float ph = dot(iPos.xz, vec2(0.71, 0.39)) + iAttr.x * 2.5;
    float w1 = sin(uTime * 1.15 + ph);
    float w2 = sin(uTime * 2.35 + dot(iPos.xz * 1.7, vec2(0.53, -0.81))
                   + iAttr.w * 4.0);
    float amp = (0.7 + 0.6 * fract(iPos.x * 0.37 + iPos.z * 0.91))
              * (1.0 + 0.6 * uWindBoost);   // rain = wind +60% amplitude
    float sway = (w1 * 0.07 + w2 * 0.035) * aPos.y * amp;
    wp.x += sway;
    wp.z += sway * 0.35 + w2 * 0.02 * aPos.y;

    // BENDING UNDER THE CHARACTER (shared block veg_common.glsl): the plant
    // ROTATES about its base away from the player instead of shifting by
    // meters — no more "wild splaying", stem length is preserved.
    // Strength grows with plant height: low brush barely reacts, tall wheat
    // bows completely.
    vec2 bdir;
    float bang = veg_bend_angle(iPos.xz, iPos.y, uCamPos, vis, aPos.y, uWalk,
                                bdir);
    if (bang > 0.0) {
        float rise = aPos.y * lc * vis;     // height of this vertex above the base
        wp.xz += bdir * (sin(bang) * rise);
        wp.y  -= (1.0 - cos(bang)) * rise;
    }

    // UV: the quad is shortened to the opaque part, so the UV shrinks too —
    // otherwise the image would stretch. aUV.y: 1 at the base, 0 at the top.
    vec2 auv = vec2(aUV.x, 1.0 - (1.0 - aUV.y) * cf);
    vUVBase = auv;   // UV [0,1] without the crop — the FS takes the needed canvas piece itself
    vUV = auv;
    // The 2D-array canvas is 256x178, sprites sit at the top left: three grass
    // kinds and dandelion 128x128 (a 128x128 piece), wheat lod0 128x178, lod1 152x178,
    // lod2 256x178. For wheat the piece is taken by the current width wx
    // (continuous between LODs); exact per-layer pieces are computed in wheat.fs.
    if (layer < WHEAT_MARK)  vUV *= vec2(128.0 / 256.0, 128.0 / 178.0);
    else                     vUV *= vec2(wx / 256.0, 1.0);
    vWorldPos = wp;
    vNormal = normalize(cross(hvec, up));      // facing normal of the quad
    vTint = iAttr.z;
    vLayer = layer;
    vHeight = vis;                             // plant top = vis meters
    gl_Position = uViewProj * vec4(wp, 1.0);
}
