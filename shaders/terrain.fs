// ============================================================================
//  Terrain — terrain fragment shader: the ground material
// ----------------------------------------------------------------------------
//  Half-Life 2 style pseudo-PBR: base map + normal map + Blinn-Phong
//  specular + cheap fresnel + distance fog.
//  The base texture is img/ground.png, tiled across the whole map (uTexScale),
//  filtered without smoothing (NEAREST, pixel-art — as requested).
//  Light (direction, color, intensity) comes from the Sun node; camera position
//  and matrices come from the Camera node.
// ============================================================================

#define SUN_MAX_LIGHTS 4

in vec2 vUV;
in vec3 vWorldPos;

uniform sampler2D uBasemap;      // ground texture (img/ground.png)
uniform sampler2D uNormalmap;    // normal map, generated from heights
uniform vec2 uTexScale;          // ground texture tiling across the whole map
uniform vec4 uField;             // field rectangle: x0, x1, z0, z1
uniform vec3 uFieldColor;        // golden tint of the wheat field
uniform float uFieldBlend;       // blend strength (from map.json)

// --- Surfaces (splat): soil / sand / gravel / asphalt ------------------------
//  uSplat (unit 12) is a world-wide RGBA mask (splat.c): R soil, G sand,
//  B gravel/path, A asphalt. The base ground shows where all channels are 0.
//  Layers are ordinary tiling textures (units 13..16), each with its own scale
//  (meters per tile) and its own "roughness" for the wet highlight.
uniform sampler2D uSplat;
uniform sampler2D uLayer0;       // soil
uniform sampler2D uLayer1;       // sand
uniform sampler2D uLayer2;       // gravel / path
uniform sampler2D uLayer3;       // asphalt
uniform vec4 uLayerScale;        // meters per tile for layers 0..3
uniform float uSplatOn;          // 1 = mask baked (otherwise base only)

uniform vec3 uCamPos;            // position of the Camera node

uniform vec3 uAmbient;           // base lighting (hemisphere computed below)
uniform vec3 uSkyColor;          // sky color (light from above)
uniform vec3 uGroundColor;       // ground color (reflected light from below)
uniform vec3 uFogColor;          // fog/sky color
uniform float uFogStart;
uniform float uFogEnd;
uniform float uFogOn;           // 1 = fog enabled (cvar r_fog_mode)
uniform float uSpecPower;        // specular "metalness"
uniform float uSpecIntensity;    // highlight strength
uniform float uWetness;          // wetness 0..1 (fx.c)
uniform sampler2DShadow uShadowMap0;   // shadow cascades: 0 = at your feet, 1-2 = mid, 3 = far
uniform mat4 uShadowMatrix0;
uniform float uShadowBias0;
uniform sampler2D uShadowDepth0;       // the same maps, raw depth (for the gradient)
uniform float uShadowGrad0;            // depth to full darkness (NDC)
uniform sampler2DShadow uShadowMap1;
uniform mat4 uShadowMatrix1;
uniform float uShadowBias1;
uniform sampler2D uShadowDepth1;
uniform float uShadowGrad1;
uniform sampler2DShadow uShadowMap2;
uniform mat4 uShadowMatrix2;
uniform float uShadowBias2;
uniform sampler2D uShadowDepth2;
uniform float uShadowGrad2;
uniform sampler2DShadow uShadowMap3;
uniform mat4 uShadowMatrix3;
uniform float uShadowBias3;
uniform sampler2D uShadowDepth3;
uniform float uShadowGrad3;
uniform float uShadowOn;         // 1 = shadows enabled (the [] key)
uniform float uDetailDist;       // detail LOD: farther away — plain base map
                                 // (0 = full splatting everywhere, as before)
uniform float uSimple;           // 1 = simplified renderer: point/spotlight loops
                                 // → 0, the second octave of detail noise skipped.
                                 // On the MEDIUM/ULTRA presets uSimple=0 — the path doesn't change.

// Static shadows (mode 2): a baked map covering the whole world
uniform sampler2D uStaticShadow;   // R8: 1 = lit, ~0.25 = shadowed
uniform float uStaticOn;           // 1 = apply the static map
uniform vec2 uWorldSize;           // world size (for UV)
uniform sampler2D uBakedLM;        // RGB8 baked light (offline, unit 11)
uniform float uBakedOn;            // 1 = read the baked luxel instead of computing
uniform sampler2D uSunVis;         // grayscale sun visibility (offline, unit 16)
uniform float uSunVisOn;           // 1 = blend statics into far shadows (mode 1)

// Wetness mask: baked R8 texture, 0 = under an overhang (dry), 1 = open sky
uniform sampler2D uWetnessMask;

// Snow cover: shared snow.glsl block (uSnowCover, mask unit 8, noise).
// Terrain: snow lies on horizontal open patches. Enabled at the end of the
// uniform block (uNoise/uWorldSize needed), implementation in snow.glsl.
uniform float uSnowCover;
uniform sampler2D uSnowMask;

// Water reflection cubemap (unit 9, scene_uniforms_bind): wet ground turns
// glossy and reflects the environment (sky, mountains, trees).
uniform samplerCube uReflection;

// Wetness noise (img/noise.png, unit 11): patches "wetter here, almost dry there"
uniform sampler2D uNoise;

// --- Light sources: all directional ones from map.json -----------------------
// uLightDir[0] — the sun (the only one with cascaded shadows), the rest are
// ambient lights.
uniform vec3 uLightDir[SUN_MAX_LIGHTS];
uniform vec3 uLightColor[SUN_MAX_LIGHTS];
uniform float uLightIntensity[SUN_MAX_LIGHTS];
uniform int uLightCount;          // actual number of sources (1..4)
#define SCENE_MAX_POINT 8
#define SCENE_MAX_SPOT  4
// Point sources (map.json lights[]: point) — local light with quadratic
// distance falloff, not attenuated by the cascade shadow.
uniform vec3 uPointPos[SCENE_MAX_POINT];
uniform vec3 uPointColor[SCENE_MAX_POINT];
uniform float uPointIntensity[SCENE_MAX_POINT];
uniform float uPointRange[SCENE_MAX_POINT];
uniform int uPointCount;
// Spotlights (spot): like point lights, but within a direction cone (see the map)
uniform vec3 uSpotPos[SCENE_MAX_SPOT];
uniform vec3 uSpotDir[SCENE_MAX_SPOT];
uniform vec3 uSpotColor[SCENE_MAX_SPOT];
uniform float uSpotIntensity[SCENE_MAX_SPOT];
uniform float uSpotRange[SCENE_MAX_SPOT];
uniform float uSpotAngle[SCENE_MAX_SPOT];
uniform int uSpotCount;

// Cascade boundaries — uShadowBox[4]: the REACH RADIUS of each cascade (half
// the box side; the box is centered on the camera, source of truth — SHADOW_BOX_*
// in shadow.h) = {4,8,48,256}. At the boundaries the cascades BLEND (a zone
// ~20% of the boundary) — otherwise bias/softness jump and ring bands or holes
// appear. Beyond uShadowBox[3] (256 m) there are no shadows — that is the edge
// of cascade 3 coverage.
uniform float uShadowBox[4];

out vec4 FragColor;

#include "snow.glsl"

// Cheap hash — slight ground color scatter so the field isn't a flat fill
float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

// Shadow of one cascade: pure HARDWARE PCF (GL_COMPARE + GL_LINEAR) —
// a single texture() gives a smooth penumbra without banding. The depth
// gradient was removed: on 1024² maps it quantized into visible steps
// ("bands" in shadows).
// soft — penumbra radius in map texels: < 0.05 = hard single-tap shadow.
float shadow_cascade(sampler2DShadow map, mat4 m, float biasu,
                     vec3 wp, vec3 n, float soft) {
    vec4 sc = m * vec4(wp, 1.0);
    vec3 suv = sc.xyz / sc.w;
    // outside the cascade box (incl. suv.z <= 0 — geometry closer to the light
    // than the box, e.g. terrain far below when flying) — no shadow
    if (suv.z <= 0.0 || suv.z >= 1.0 || suv.x <= 0.0 || suv.x >= 1.0 ||
        suv.y <= 0.0 || suv.y >= 1.0)
        return 1.0;
    // bias against acne ("hair"/bands on slopes and gently lit ground): scaled
    // by the light incidence angle, not by world up — under a low sun a FLAT
    // ground is nearly parallel to the light and per-texel depth quantization
    // of the cascade yields even contour bands. (1 - N·L) grows exactly where
    // surface texels "lie down" along the ray.
    float nl = clamp(dot(n, normalize(uLightDir[0])), 0.0, 1.0);
    float bias = biasu * (0.6 + 1.8 * (1.0 - nl));
    float s = texture(map, vec3(suv.xy, suv.z - bias));
    if (soft < 0.05) return s;
    // Light penumbra: 4 extra cross taps with a FIXED rotation.
    // Rotation by a per-pixel hash gives shimmer at the shadow edge when the
    // camera moves, a stable core — does not. The radius is small (up to 2.5
    // texels, only far away): the edge stays crisp, only the stepping of the
    // far maps is smoothed.
    vec2 texel = 1.0 / vec2(textureSize(map, 0));
    vec2 t2 = texel * (soft * 0.5);
    vec2 d1 = vec2(0.8370, 0.5472) * t2;
    vec2 d2 = vec2(-0.5472, 0.8370) * t2;
    float acc = s;
    acc += texture(map, vec3(suv.xy + d1, suv.z - bias));
    acc += texture(map, vec3(suv.xy - d1, suv.z - bias));
    acc += texture(map, vec3(suv.xy + d2, suv.z - bias));
    acc += texture(map, vec3(suv.xy - d2, suv.z - bias));
    return acc * 0.2;
}

// Penumbra radius: near the feet (up to ~10 m) — hard shadow (sharp edge),
// farther out a smooth growth to 2.5 texels at the horizon — just enough to
// soften the stepping of the far maps; the edge stays crisp there too.
float shadow_soft(float dist) {
    return clamp((dist - 10.0) * 0.012, 0.0, 2.5);
}

// Shadow by distance: cascade selection with SMOOTH blending at the boundaries
// (a zone ~20% before the boundary) — removes ring bands and holes between
// cascades. Beyond uShadowBox[3] — no shadow.
float shadow_at(vec3 wp, vec3 n) {
    float dist = distance(uCamPos, wp);
    if (dist >= uShadowBox[3]) return 1.0;
    float soft = shadow_soft(dist);
    if (dist < uShadowBox[0]) {
        float t = smoothstep(uShadowBox[0] * 0.8, uShadowBox[0], dist);
        return mix(shadow_cascade(uShadowMap0, uShadowMatrix0, uShadowBias0, wp, n, soft),
                   shadow_cascade(uShadowMap1, uShadowMatrix1, uShadowBias1, wp, n, soft), t);
    }
    if (dist < uShadowBox[1]) {
        float t = smoothstep(uShadowBox[1] * 0.8, uShadowBox[1], dist);
        return mix(shadow_cascade(uShadowMap1, uShadowMatrix1, uShadowBias1, wp, n, soft),
                   shadow_cascade(uShadowMap2, uShadowMatrix2, uShadowBias2, wp, n, soft), t);
    }
    if (dist < uShadowBox[2]) {
        float t = smoothstep(uShadowBox[2] * 0.8, uShadowBox[2], dist);
        return mix(shadow_cascade(uShadowMap2, uShadowMatrix2, uShadowBias2, wp, n, soft),
                   shadow_cascade(uShadowMap3, uShadowMatrix3, uShadowBias3, wp, n, soft), t);
    }
    return shadow_cascade(uShadowMap3, uShadowMatrix3, uShadowBias3, wp, n, soft);
}

void main() {
    // Far fog — early exit. Beyond uFogEnd fog = smoothstep(...) = 1.0, so the
    // whole shader reduces to col = mix(col, uFogColor, 1.0) = uFogColor.
    // Hence base/normal/splat/wetness/light/shadows need not be computed on
    // these pixels — the result is exactly uFogColor. Cuts the ENTIRE terrain
    // material at the far edge (horizon, top-down view while flying) — 8-12
    // texture fetches and the light loop per such pixel. Byte-for-byte identical
    // to the old path for ALL presets (the condition uFogEnd > uFogStart
    // guarantees smoothstep on the main path would have yielded exactly 1.0).
    if (uFogOn > 0.5 && uFogEnd > uFogStart &&
        distance(uCamPos, vWorldPos) >= uFogEnd) {
        FragColor = vec4(uFogColor, 1.0);
        return;
    }
    // Detail LOD (uDetailDist > 0, the POTATO mode): past the distance
    // details are invisible — normal noise and splat layers fade to zero, only
    // the base map remains (far grass/roads are already baked into it) + fog.
    // This cuts 6-8 texture fetches over a LARGE part of the screen.
    float det_f = 0.0;
    if (uDetailDist > 0.5) {
        float dcam = distance(uCamPos, vWorldPos);
        det_f = smoothstep(uDetailDist * 0.55, uDetailDist, dcam);
    }
    vec3 base = texture(uBasemap, vUV * uTexScale).rgb;
    vec3 N = normalize(texture(uNormalmap, vUV).rgb * 2.0 - 1.0);
    // Noise-based detail normals: for small-scale relief (soil, rocks, paths)
    // on top of the base normal we add a perturbation from img/noise.png —
    // no extra texture, lively micro-geometric relief.
    // Two noise frequencies: fine (~2 m) and slightly coarser (~8 m). The
    // amplitude is modulated by a THIRD, large (~48 m) mask: without it the two
    // noise grids interfere — a regular diagonal "shimmer" on flat expanses
    // (by the shore it read as fake dunes). With the mask the shimmer lives in
    // patches, like real microgeometry.
    // Simplified renderer (POTATO): micro-relief disabled entirely — 3 noise
    // texture fetches + normalize per near ground pixel are cut, leaving a
    // smooth base normal. On the MEDIUM/ULTRA presets — as before.
    if (det_f < 0.999 && uSimple < 0.5) {
        vec2 nd = vWorldPos.xz;
        float mask = texture(uNoise, nd / 48.0 + 1.37).r;
        float amp = 0.25 * (0.25 + 0.75 * mask * mask) * (1.0 - det_f);
        float w1 = texture(uNoise, nd / 2.0 + 7.11).r;
        float w2 = texture(uNoise, nd / 8.0 + 3.77).r;
        vec2 perturb = vec2(w1 * 2.0 - 1.0, w2 * 2.0 - 1.0);
        N = normalize(N + vec3(perturb * amp, 0.0));
    }
    // --- Surface layers (splat): soil/sand/gravel/asphalt ---------------------
    // The mask is baked by splat.c: roads, paint patches, auto-gravel on cliffs.
    // smooth_layer — how "smooth" the surface is: asphalt shines after rain more
    // than grass, sand barely shines. rough_detail damps the noise perturbation
    // of the normal on asphalt (it is even).
    float smooth_layer = 0.0;
    float layer_sum = 0.0;
    if (uSplatOn > 0.5 && det_f < 0.999) {
        vec4 sp = texture(uSplat, vWorldPos.xz / uWorldSize);
        layer_sum = min(sp.r + sp.g + sp.b + sp.a, 1.0) * (1.0 - det_f);
        if (layer_sum > 0.002) {
            vec3 c0 = texture(uLayer0, vWorldPos.xz / max(uLayerScale.x, 0.01)).rgb;
            vec3 c1 = texture(uLayer1, vWorldPos.xz / max(uLayerScale.y, 0.01)).rgb;
            vec3 c2 = texture(uLayer2, vWorldPos.xz / max(uLayerScale.z, 0.01)).rgb;
            vec3 c3 = texture(uLayer3, vWorldPos.xz / max(uLayerScale.w, 0.01)).rgb;
            // Weighted sum of layers, normalized by its own weight: the base
            // ground stays in the remainder (1 - layer_sum).
            vec3 mixed = c0 * sp.r + c1 * sp.g + c2 * sp.b + c3 * sp.a;
            mixed /= max(sp.r + sp.g + sp.b + sp.a, 1e-4);
            base = mix(base, mixed, layer_sum);
            // Asphalt is smooth (0.9), gravel mid, soil/sand matte.
            smooth_layer = (sp.r * 0.15 + sp.g * 0.05 + sp.b * 0.35
                          + sp.a * 0.95) / max(sp.r + sp.g + sp.b + sp.a, 1e-4);
            smooth_layer *= layer_sum;
            // On even coverage we remove the noise normal detail:
            // asphalt must not "bulge".
            N = normalize(mix(N, vec3(0.0, 1.0, 0.0), smooth_layer * 0.55));
        }
    }
    // wetness: albedo darkening and a stronger highlight. In clear weather
    // (uWetness ~ 0, 99% of the time) the block is skipped ENTIRELY — no need
    // to spend 3 texture fetches (mask + two-frequency noise) per terrain
    // pixel (the uniform branch is free).
    float wet = clamp(uWetness, 0.0, 1.0);
    float wet_eff = 0.0;
    if (wet > 0.004) {
        // Wetness mask: 0 = under a roof (dry), 1 = open sky
        float wet_mask = texture(uWetnessMask, vWorldPos.xz / uWorldSize).r;
        // Water runs off: on vertical surfaces the effect is weaker
        float slope_factor = clamp(N.y, 0.0, 1.0);
        // Wetness unevenness: img/noise.png (REPEAT). A noise texel is
        // ≈ 6 m / 1.25 m — fine patches "wetter / almost dry".
        float noise = texture(uNoise, vWorldPos.xz / 1600.0).r * 0.7
                    + texture(uNoise, vWorldPos.xz / 320.0 + 0.6311).r * 0.3;
        noise = noise * noise;                 // contrast: sharper wet/dry split
        wet_eff = wet * wet_mask * slope_factor * mix(0.05, 1.8, noise);
        // Smooth coverage holds water in puddles: wetness on asphalt is even and stronger
        wet_eff = mix(wet_eff, wet * wet_mask * slope_factor, smooth_layer);
        base = mix(base, base * 0.6, wet_eff * 0.7);
    }
    // on the wheat field site the ground is golden soil (not flat grass).
    // Under a road/coverage the field tint does not show through (layer_sum damps).
    vec2 wp = vWorldPos.xz;
    float fx = smoothstep(uField.x - 8.0, uField.x, wp.x)
             * (1.0 - smoothstep(uField.y, uField.y + 8.0, wp.x));
    float fz = smoothstep(uField.z - 8.0, uField.z, wp.y)
             * (1.0 - smoothstep(uField.w, uField.w + 8.0, wp.y));
    base = mix(base, uFieldColor, fx * fz * uFieldBlend * (1.0 - layer_sum));
    base *= 0.94 + 0.12 * hash12(vWorldPos.xz * 0.5);
    // Snow cover (shared snow.glsl block): gentle patches open to the sky.
    {
        float a = snow_amount(vWorldPos, smoothstep(0.35, 0.70, N.y));
        if (a > 0.001) base = snow_color(base, vWorldPos, a);
    }
    vec3 V = normalize(uCamPos - vWorldPos);

    // cheap fresnel — a slight sky "env reflection" on slopes
    float fres = pow(1.0 - max(dot(N, V), 0.0), 5.0);

    // hemispherical ambient: sky from above, ground bounce from below
    vec3 amb = mix(uGroundColor, uSkyColor, N.y * 0.5 + 0.5) * uAmbient;

    // Sun shadow: cascade maps from the light's shadow camera.
    float csm = 1.0;
    if (uShadowOn > 0.5)
        csm = shadow_at(vWorldPos, N);
    // FALLOFF TOWARD THE HORIZON: past ~140 m the shadow smoothly fades to zero —
    // at the cascades' reach limit the shadow is subtexel anyway and, under a low
    // sun, noisy bands; distant slopes are better left simply lit.
    csm = mix(1.0, csm, 1.0 - smoothstep(140.0, 250.0,
                                         distance(uCamPos, vWorldPos)));
    // Mode 2 (statics): a world-wide shadow map, while DYNAMICS (CSM) apply only
    // near the player (2..3 m); beyond that — statics. In mode 1 (CSM) statics
    // are off (uStaticOn = 0) — shadows are pure cascades for the whole distance.
    float shadow = csm;
    // Mode 1 (CSM): beyond the cascades (140..250 m) the dynamic shadow fades out —
    // blend in the BAKED static sun visibility (uSunVis). Distant trees and the
    // field cast shadows on the horizon again, while up close everything is
    // honestly dynamic. The smooth transition matches the CSM falloff above.
    if (uSunVisOn > 0.5) {
        float far = smoothstep(140.0, 250.0, distance(uCamPos, vWorldPos));
        float svis = texture(uSunVis, vWorldPos.xz / uWorldSize).r;
        shadow = mix(shadow, svis, far);
    }
    if (uStaticOn > 0.5) {
        float st = texture(uStaticShadow, vWorldPos.xz / uWorldSize).r;
        shadow = mix(csm, st, smoothstep(2.0, 3.0, distance(uCamPos, vWorldPos)));
    }

    // Direct light: EVERY directional source from map.json lights the ground
    // SEPARATELY (the sun = index 0, then the ambient lights).
    // Deeper darkness: in shadow the hemispherical light dims to 1/4.
    vec3 light;
    if (uBakedOn > 0.5) {
        // Baked light (offline, mode 2 "statics" ONLY): the RGB luxel already
        // contains ambient + sky AO + direct light + the STATIC shadow + bounced
        // bonus. No cascades, no hemisphere — otherwise the shadow would be
        // computed twice (luxel × CSM). Dynamics come from mode 1 via the
        // branch below.
        light = texture(uBakedLM, vWorldPos.xz / uWorldSize).rgb;
    } else {
    light = amb * mix(0.25, 1.0, shadow);
    for (int i = 0; i < uLightCount; i++) {
        vec3 li = normalize(uLightDir[i]);
        float diff = max(dot(N, li), 0.0);
        float sh = (i == 0) ? shadow : 1.0;      // CSM shadows only for the sun
        // diffuse nearly flat at 0.5: ground is matte, without harsh halving
        light += uLightColor[i] * uLightIntensity[i]
               * (0.55 + 0.45 * diff) * sh;
        if (i == 0) {
            // rim contour against the light and a warm halo around the sun.
            // In rain (wet > 0) — DISABLED: a wet scene gets no backlit
            // outline (see request).
            float rim = pow(1.0 - max(dot(N, V), 0.0), 4.0) * (1.0 - wet);
            light += uLightColor[0] * uLightIntensity[0]
                   * (rim * 0.35 + pow(diff, 8.0) * 0.18 * sh * (1.0 - wet));
        }
    }
    }

    // Point sources and spotlights — full render only. On POTATO both loops
    // collapse to zero: the sun lights the ground, local sources on a distant
    // field are barely readable.
    if (uSimple < 0.5) {
    // Point sources: distance falloff, local light
    for (int i = 0; i < uPointCount; i++) {
        vec3 Lp = uPointPos[i] - vWorldPos;
        float dpl = length(Lp);
        vec3 lip = Lp / max(dpl, 0.001);
        float att = max(1.0 - dpl / uPointRange[i], 0.0);
        att *= att;
        float diff = max(dot(N, lip), 0.0);
        light += uPointColor[i] * uPointIntensity[i] * (0.5 + 0.5 * diff) * att;
    }
    // Spotlights: point + cone with a softened edge
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
    vec3 col = base * light;

    // wetness: add a specular highlight on wet surfaces
    if (wet > 0.0) {
        // wet_eff already computed above (with slope_factor and noise)
        float wet_spec = wet_eff * uSpecIntensity * 3.0;
        for (int i = 0; i < uLightCount; i++) {
            vec3 li = normalize(uLightDir[i]);
            vec3 H = normalize(li + V);
            float s = pow(max(dot(N, H), 0.0), uSpecPower + 30.0);
            col += uLightColor[i] * uLightIntensity[i] * s * wet_spec;
        }
        // Fresnel boost: a strong highlight at grazing angles
        col += uSkyColor * wet_eff * fres * 0.4;
    }

    col += fres * uFogColor * 0.15;                      // "from the sky" sheen

    // Wet ground — gloss: the reflection cubemap grows with wetness and
    // grazing angle (Fresnel). Only open sky (wetness mask) and only
    // up-facing surfaces (slope_factor) — water runs off.
    if (wet_eff > 0.001) {
        vec3 reflDir = reflect(-V, N);
        vec3 reflCol = texture(uReflection, reflDir).rgb;
        col = mix(col, reflCol, wet_eff * (0.18 + 0.55 * fres));
    }

    // distance fog — hides the world edge. With uFogOn = 0 (r_fog_mode 0)
    // the branch doesn't run: no smoothstep, no mix per pixel.
    if (uFogOn > 0.5) {
        float fog = smoothstep(uFogStart, uFogEnd, distance(uCamPos, vWorldPos));
        col = mix(col, uFogColor, fog);
    }
    FragColor = vec4(col, 1.0);
}
