// ============================================================================
//  Vegetation — vegetation fragment shader: grass and wheat material
// ----------------------------------------------------------------------------
//  A layer from the 2D array (0 = grass, 1 = wheat) picks the sprite; hard
//  alpha test. Lighting:
//    - hemispheric ambient (sky from above, grass/soil bounce from below);
//    - soft wrap diffuse — no hard terminator on the shadow side;
//    - CASCADED SHADOWS (CSM): plants receive shadows from the terrain and
//      from each other (PCF ~1.5 texels, bias 2 texels — thin quads);
//    - specular on wheat ears (grass surface stays matte);
//    - height gradient: base darkened by neighbor occlusion, top catches
//      more light;
//    - BACKLIGHT (special material mode): when the sun is behind the plant,
//      the body does NOT glow through — it darkens into a silhouette,
//      and a warm glowing outline flares along the silhouette edges.
//  Distance fog — same as terrain.
// ============================================================================

in vec2 vUV;
in vec2 vUVBase;    // UV [0,1] without the cutout — from wheat.vs
in vec3 vWorldPos;
in vec3 vNormal;
in float vTint;
flat in float vLayer;   // from wheat.vs — LOD is whole per quad
in float vHeight;      // plant height in meters (from wheat.vs)

uniform sampler2DArray uVeg;      // sprite array: 0-3 = grass/dandelion, 4-6 = wheat LODs
uniform vec3 uCamPos;             // Camera node position
// Z-PREPASS: 1 = depth-only pass (no lighting). The same quad and the same
// alpha test as in the color pass, so depth matches bit-for-bit and the color
// pass with glDepthFunc(GL_EQUAL) shades each pixel ONCE — overdraw is cut in
// the most expensive shader of the frame. The image is identical.
uniform float uDepthOnly;
// SIMPLE RENDER (POTATO preset, r_simple_render): we cut everything barely
// visible on a distant/dense field but expensive in fragments — point/spot
// loops (→ 0 iterations), specular, subsurface scattering, backlight and the
// 8-tap glowing outline, plus sprite LOD blending (1 sample instead of 2).
// What remains is the core: albedo + hemisphere + sun (+ its shadow) + fog.
// On MEDIUM/ULTRA uSimple=0 — the path is unchanged.
uniform float uSimple;

// Kind (vLayer from wheat.vs): 0/1/2 — three grass kinds, 3 — dandelion,
// 4 (and continuously 4..6 under LOD) — wheat.
#define WHEAT_MARK 3.5
// ALL directional lights from map.json: index 0 — the sun (with shadows),
// then — background fills. Plants glow a little from every one of them.
#define SUN_MAX_LIGHTS 4
uniform vec3 uLightDir[SUN_MAX_LIGHTS];
uniform vec3 uLightColor[SUN_MAX_LIGHTS];
uniform float uLightIntensity[SUN_MAX_LIGHTS];
uniform int uLightCount;
#define SCENE_MAX_POINT 8
#define SCENE_MAX_SPOT  4
// Point lights (map.json lights[]: point) — local light with quadratic
// distance falloff, not extinguished by the cascaded shadow.
uniform vec3 uPointPos[SCENE_MAX_POINT];
uniform vec3 uPointColor[SCENE_MAX_POINT];
uniform float uPointIntensity[SCENE_MAX_POINT];
uniform float uPointRange[SCENE_MAX_POINT];
uniform int uPointCount;
// Spotlights (spot): like point lights, but inside a direction cone (see map)
uniform vec3 uSpotPos[SCENE_MAX_SPOT];
uniform vec3 uSpotDir[SCENE_MAX_SPOT];
uniform vec3 uSpotColor[SCENE_MAX_SPOT];
uniform float uSpotIntensity[SCENE_MAX_SPOT];
uniform float uSpotRange[SCENE_MAX_SPOT];
uniform float uSpotAngle[SCENE_MAX_SPOT];
uniform int uSpotCount;
uniform vec3 uAmbient;            // ambient light (hemisphere computed below)
uniform vec3 uSkyColor;           // sky color (light from above)
uniform vec3 uGroundColor;        // grass/soil color (bounce from below)
uniform vec3 uFogColor;           // fog (same as terrain)
uniform float uFogStart;
uniform float uFogEnd;
uniform float uFogOn;            // 1 = fog enabled (cvar r_fog_mode)

uniform sampler2DShadow uShadowMap0;  // shadow cascades — SEPARATE samplers:
uniform sampler2DShadow uShadowMap1;  // dynamic indexing of sampler arrays
uniform sampler2DShadow uShadowMap2;  // is forbidden in GLSL 330
uniform sampler2DShadow uShadowMap3;  // (Mesa won't compile, NVIDIA tolerated)
uniform mat4 uShadowMatrix[4];        // matrices/numbers — arrays are fine
uniform float uShadowBias[4];
uniform sampler2D uShadowDepth0;
uniform sampler2D uShadowDepth1;
uniform sampler2D uShadowDepth2;
uniform sampler2D uShadowDepth3;
uniform float uShadowGrad[4];         // depth until full darkness (NDC)
uniform float uShadowOn;          // 1 = shadows enabled (key [)
uniform float uWetness;        // wetness 0..1 (fx.c)

// Snow cover: shared snow.glsl block. Grass/dandelion/wheat: white caps on
// the tops (vUVBase.y = 0 — quad top, PNG top row).
uniform float uSnowCover;
uniform sampler2D uSnowMask;
uniform sampler2D uNoise;
uniform vec2 uWorldSize;
#include "snow.glsl"

// Plants are lit by direct sun light and CSM shadows; no AO at all —
// the field stays golden, plant shadows on the ground are cast by the cascades.

out vec4 FragColor;

// Exact canvas patches of 256x178 per layer: grass/dandelion 128x128 (top
// left, a 128x128 patch), wheat lod0 128x178, lod1 152x178, lod2 256x178.
vec2 grass_uv(vec2 u) { return u * vec2(128.0 / 256.0, 128.0 / 178.0); }
vec2 lod0_uv(vec2 u)  { return u * vec2(128.0 / 256.0, 1.0); }
vec2 lod1_uv(vec2 u)  { return u * vec2(152.0 / 256.0, 1.0); }
vec2 lod2_uv(vec2 u)  { return u; }

// Sprite with smooth LOD: vLayer is continuous (4..5..6 for wheat, flat — one
// LOD per quad). Grasses/dandelion (0..3) are taken as is. Within a short
// transition band (0.8 m) neighboring wheat layers blend smoothly by color
// AND alpha — the transition is visible, but the ghost band stays narrow and
// the edges remain crisp. Outside the band — a single texel.
vec4 veg_sample(vec2 base, float lf, out vec2 uv_out) {
    if (lf < WHEAT_MARK) {          // grass / dandelion — as is
        uv_out = grass_uv(base);
        return texture(uVeg, vec3(uv_out, lf));
    }
    float li = floor(lf);           // 4, 5 or 6
    float w = lf - li;
    vec2 ua = li < 4.5 ? lod0_uv(base) : (li < 5.5 ? lod1_uv(base) : lod2_uv(base));
    vec4 a = texture(uVeg, vec3(ua, li < 4.5 ? 4.0 : (li < 5.5 ? 5.0 : 6.0)));
    uv_out = ua;
    if (w > 0.001 && uSimple < 0.5) {
        vec2 ub = li < 4.5 ? lod1_uv(base) : lod2_uv(base);
        vec4 b = texture(uVeg, vec3(ub, li < 5.5 ? 5.0 : 6.0));
        uv_out = mix(ua, ub, w);
        return mix(a, b, w);
    }
    return a;
}

// cascade bounds — uShadowBox[4]: COVERAGE RADIUS of a cascade (half the box
// side; the box is centered on the camera) = {4,8,48,256}. Beyond
// uShadowBox[3] (256 m) there are no shadows — that is the edge of cascade 3.
uniform float uShadowBox[4];

// A cascade shadow = UNION: hardware PCF (vis — dense field dark under the
// canopy, nearest stem ~5 cm) + DEPTH GRADIENT along the light's 3D ray
// (deep: d≈0 at the plant, d≈3.6 m at the shadow tip). Dark if either measure
// is dark: 1 − (1−vis)·(1−deep). The field is evenly dark, open shadows show
// a visible gradient from the lit edge down to the base.
// One cascade shadow: clean hardware PCF. The depth gradient was removed —
// on 1024² maps it quantized into visible bands.
float cascade_tap(sampler2DShadow map, float bias, vec3 suv) {
    return texture(map, vec3(suv.xy, suv.z - bias));
}

float sample_cascade(vec3 wp, int c) {
    vec4 sc = uShadowMatrix[c] * vec4(wp, 1.0);
    vec3 suv = sc.xyz / sc.w;
    // outside the cascade box — no shadow
    if (suv.z <= 0.0 || suv.z >= 1.0 || suv.x <= 0.0 || suv.x >= 1.0 ||
        suv.y <= 0.0 || suv.y >= 1.0)
        return 1.0;
    if (c == 0)
        return cascade_tap(uShadowMap0, uShadowBias[0], suv);
    if (c == 1)
        return cascade_tap(uShadowMap1, uShadowBias[1], suv);
    if (c == 2)
        return cascade_tap(uShadowMap2, uShadowBias[2], suv);
    return cascade_tap(uShadowMap3, uShadowBias[3], suv);
}

float veg_shadow(vec3 wp) {
    if (uShadowOn < 0.5) return 1.0;
    float dist = distance(uCamPos, wp);
    if (dist >= uShadowBox[3]) return 1.0;
    // cascade choice with blending at boundaries (like in terrain.fs)
    if (dist < uShadowBox[0]) {
        float t = smoothstep(uShadowBox[0] * 0.8, uShadowBox[0], dist);
        return mix(sample_cascade(wp, 0), sample_cascade(wp, 1), t);
    }
    if (dist < uShadowBox[1]) {
        float t = smoothstep(uShadowBox[1] * 0.8, uShadowBox[1], dist);
        return mix(sample_cascade(wp, 1), sample_cascade(wp, 2), t);
    }
    if (dist < uShadowBox[2]) {
        float t = smoothstep(uShadowBox[2] * 0.8, uShadowBox[2], dist);
        return mix(sample_cascade(wp, 2), sample_cascade(wp, 3), t);
    }
    return sample_cascade(wp, 3);
}

void main() {
    vec2 suv;
    vec4 c = veg_sample(vUVBase, vLayer, suv);
    if (c.a < 0.5) discard;              // transparent sprite background
    // Depth pass: the alpha test above is already done — that is enough to
    // record the silhouette depth. Expensive lighting/shadows/snow are NOT
    // computed.
    if (uDepthOnly > 0.5) { FragColor = vec4(0.0); return; }
    vec3 N = normalize(vNormal);
    vec3 V = normalize(uCamPos - vWorldPos);
    float wet = clamp(uWetness, 0.0, 1.0);
    c.rgb = mix(c.rgb, c.rgb * 0.7, wet * 0.5);

    float d = distance(uCamPos, vWorldPos);

    // EARLY EXIT: past the far fog boundary the color is FULLY equal to the
    // fog color — recomputing light/shadows/snow is pointless and expensive
    // (distant grass is precisely what yields most fragments). Alpha — the
    // same c.a as in the full path: with alpha-to-coverage (MSAA) the full
    // path gives a soft edge at the sprite border, while the previous hard
    // 1.0 at the fog line clipped tops "lopsided" (a tall ear crossed
    // uFogEnd with its top before its bottom — part of the quad took the
    // early exit with alpha=1, part the full path with alpha=c.a).
    // Now at d>=uFogEnd the result is bit-for-bit equal to the full path's mix(...,1.0).
    if (uFogOn > 0.5 && d >= uFogEnd) {
        FragColor = vec4(uFogColor, c.a);
        return;
    }

    // Snow cover (shared snow.glsl block): snow lays over the whole plant,
    // stronger toward the top (cap). vUVBase.y = 1 at the base, 0 at the top.
    // Plants are billboards (no volume, and thin translucent blades at the
    // edges), so we do NOT gate them with noisy patches like the ground: take
    // a smooth coverage curve from uSnowCover and sky openness. The white cap
    // appears as soon as snow starts, not speckled.
    // Without snow (uSnowCover~0) the block is SKIPPED entirely — no reason to
    // burn 2 extra texture samples on every fragment (summer/autumn).
    if (uSnowCover > 0.001) {
        float smask = texture(uSnowMask, vWorldPos.xz / uWorldSize).r;
        float cover = smoothstep(0.10, 0.55, uSnowCover);
        float atop  = 1.0 - smoothstep(0.05, 0.9, vUVBase.y);
        float a = cover * atop * smask * (0.85 + 0.15 * texture(uNoise, vWorldPos.xz / 1.6).r);
        if (a > 0.001) c.rgb = snow_color(c.rgb, vWorldPos, a);
    }

    // hemispheric ambient: sky from above, ground bounce from below
    vec3 amb = mix(uGroundColor, uSkyColor, N.y * 0.5 + 0.5) * uAmbient;

    // Diffuse is almost FLAT: a billboard has a random yaw, and lighting by
    // the quad normal would cut "half" of the grass 2x darker (N·L=0 for
    // sprites turned away from the sun). For a flat sprite the normal is
    // meaningless — keep only a slight variety from the orientation.
    // The base is dimmed so that plants do not glow.

    // specular on wheat ears (kind 4+); grass/dandelion — matte
    float layer = step(WHEAT_MARK, vLayer);

//    // shadow from the terrain and neighboring plants
    float shadow = veg_shadow(vWorldPos);

    // No AO: the wheat field stays golden, shadows on the ground are cast by the cascades.

    // There is NO static vertical gradient: it cut every plant in half (dark
    // bottom / bright top). Only the tint sets the tone.
    float grad = 1.0;

    // distance to camera (d) is already computed above — needed by both the
    // backlight (nearby meters) and fog and the early exit.

    // All lighting accumulates in light and IS CLAMPED from above: even from
    // 100000% light a plant does NOT burn out to white — nothing is brighter
    // than full albedo, the ear color always stays visible.
    // EVERY directional light from map.json shines; only the sun (index 0)
    // takes the CSM shadow. The darkest depths go deeper: in shadow the
    // hemispheric light fades to 1/4.
    vec3 light = amb * mix(0.25, 1.0, shadow);
    vec3 L = vec3(0.0);
    // EVERY directional light shines: the sun (0, warm, with shadow) + a cold
    // "fill" — it balances the sun's yellowness. Not cut on potato (without
    // fill wheat drifts into yellow).
    for (int i = 0; i < uLightCount; i++) {
        vec3 li = normalize(uLightDir[i]);
        float diff = 0.55 + 0.25 * max(dot(N, li), 0.0);
        float sh = (i == 0) ? shadow : 1.0;
        light += uLightColor[i] * uLightIntensity[i] * diff * sh;
        if (i == 0) L = li;
    }
    // Point lights and spotlights — full render only: on potato the loops
    // collapse to zero (the field is lit by the sun, local light on distant
    // grass is barely readable).
    if (uSimple < 0.5) {
    // Point lights: distance falloff, local light
    for (int i = 0; i < uPointCount; i++) {
        vec3 Lp = uPointPos[i] - vWorldPos;
        float dpl = length(Lp);
        vec3 lip = Lp / max(dpl, 0.001);
        float att = max(1.0 - dpl / uPointRange[i], 0.0);
        att *= att;
        float diff = max(dot(N, lip), 0.0);
        light += uPointColor[i] * uPointIntensity[i] * (0.5 + 0.5 * diff) * att;
    }
    // Spotlights: point light + cone with a softened edge
    for (int i = 0; i < uSpotCount; i++) {
        vec3 Lp = uSpotPos[i] - vWorldPos;
        float dpl = length(Lp);
        vec3 lip = Lp / max(dpl, 0.001);
        float att = max(1.0 - dpl / uSpotRange[i], 0.0);
        att *= att;
        float cone = max(dot(-lip, normalize(uSpotDir[i])), 0.0);
        float edge = smoothstep(uSpotAngle[i] - 0.08, uSpotAngle[i], cone);
        float diff = max(dot(N, lip), 0.0);
        light += uSpotColor[i] * uSpotIntensity[i] * (0.5 + 0.5 * diff) * att * edge;
    }
    }
    light = min(light, vec3(1.0));
    vec3 col = c.rgb * vTint * grad * light;
    if (uSimple < 0.5 && uLightCount > 0 && length(L) > 0.5) {
        vec3 H = normalize(L + V);
        float spec = pow(max(dot(N, H), 0.0), 48.0) * layer;
        col += uLightColor[0] * spec * 0.5 * shadow;

        // wetness: slight damp sheen on the ears (layer 1). Weak — so an ear
        // does not "burn out" to white in rain; muted with height.
        if (wet > 0.0) {
            float wet_spec = wet * layer * 0.18;
            col += uLightColor[0] * wet_spec * shadow;
        }
    }

    // SUBSURFACE SCATTERING — to all plants by default: when the light is
    // behind the plant (looking against the sun), part of the light passes
    // through the stem/ear and glows from inside — a warm halo like real
    // foliage. Tips are thinner — they transmit more. Shadow is respected:
    // a plant in shadow does not glow. In RAIN — disabled: wet foliage does
    // not backlight (no rim sun).
    if (uSimple < 0.5 && uLightCount > 0 && length(L) > 0.5) {
        float trans = max(dot(-L, V), 0.0);          // 1 = light strictly behind the plant
        float tip = smoothstep(0.25, 1.0, suv.y);    // tip thinner — brighter
        float sss = trans * trans * (0.25 + 0.75 * tip) * shadow * (1.0 - wet);
        col += c.rgb * vTint * uLightColor[0] * uLightIntensity[0] * sss * 0.7;
    }

    // BACKLIGHT + glowing outline — full render only. On potato the whole
    // block (including up to 8 outline texture samples) is skipped.
    if (uSimple < 0.5) {
    // BACKLIGHT: sun BEHIND the plant (viewed from the shadow side, L·V < 0).
    // The bottom darkens the MORE the TALLER the plant (high wheat shades its
    // own base, short grass — barely), and the gradient fades out toward the
    // top. Only in nearby meters (up to ~55 m). In RAIN the whole backlight
    // effect is off (wet) — no rim outlines.
    vec3 lit = col;                          // lit color before darkening
    float near_fade = 1.0 - smoothstep(15.0, 55.0, d);
    float backlit = (1.0 - smoothstep(-0.1, 0.55, dot(L, V))) * near_fade
                  * (1.0 - wet);
    float hf = clamp(vHeight / 0.9, 0.0, 1.0);   // 0 = short grass, 1 = wheat
    float bottom = mix(0.85, 0.5, hf);           // darkening right at the bottom
    float shade = mix(bottom, 1.0, suv.y);       // bottom → top
    col *= mix(1.0, shade, backlit);

    // GLOWING OUTLINE along the sprite silhouette: where a nearby pixel is
    // transparent. Two layers: a weak sharp ring (~1.5 sprite texels) + a
    // wide soft halo (~3 texels) — giving a smooth glow, not a hard line.
    // Color — the object's lit color (not white), fading with distance.
    // Branch: when backlit == 0 the result zeroes out anyway, and the 8
    // texture samples would be wasted (all distant ears and grass are "not
    // rimmed").
    if (backlit > 0.001) {
        vec2 tv = 1.5 / vec2(textureSize(uVeg, 0).xy);
        vec2 tv2 = tv * 2.0;
        float o1 = max(max(1.0 - texture(uVeg, vec3(suv + vec2(tv.x, 0.0), vLayer)).a,
                           1.0 - texture(uVeg, vec3(suv - vec2(tv.x, 0.0), vLayer)).a),
                       max(1.0 - texture(uVeg, vec3(suv + vec2(0.0, tv.y), vLayer)).a,
                           1.0 - texture(uVeg, vec3(suv - vec2(0.0, tv.y), vLayer)).a));
        float o2 = max(max(1.0 - texture(uVeg, vec3(suv + vec2(tv2.x, 0.0), vLayer)).a,
                           1.0 - texture(uVeg, vec3(suv - vec2(tv2.x, 0.0), vLayer)).a),
                       max(1.0 - texture(uVeg, vec3(suv + vec2(0.0, tv2.y), vLayer)).a,
                           1.0 - texture(uVeg, vec3(suv - vec2(0.0, tv2.y), vLayer)).a));
        float glow = (smoothstep(0.2, 0.7, o1) * 0.42
                    + smoothstep(0.1, 0.45, o2) * 0.26) * 0.6;   // -40% glow
        col += lit * backlit * glow;
    }
    } // uSimple < 0.5 — end of backlight and outline

    // distance fog. With uFogOn = 0 (r_fog_mode 0) — no cost at all.
    if (uFogOn > 0.5) {
        float fog = smoothstep(uFogStart, uFogEnd, d);
        col = mix(col, uFogColor, fog);
    }
    // Alpha from the texture (0.5..1 at the smoothed edge): with MSAA active,
    // alpha-to-coverage is on — partial coverage smooths the edge.
    // Blending is off, so alpha does not affect the target.
    FragColor = vec4(col, c.a);
}
