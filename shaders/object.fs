// ============================================================================
//  Object — fragment shader for the object billboard (tree etc.)
// ----------------------------------------------------------------------------
//  Material ALMOST ONE-TO-ONE the same as wheat (wheat.fs): alpha-test,
//  hemispheric ambient, flat diffuse (the billboard normal is meaningless),
//  all directional lights from map.json, sun CSM shadows with depth
//  gradient, tint, light clamp from above, fog.
// ============================================================================

in vec2 vUV;
in vec3 vWorldPos;

uniform sampler2D uTex;
uniform vec3 uCamPos;
uniform vec3 uTint;              // object tint (default 1)

// Display mode (render.blend/alpha):
//   0 = alpha-test (transparent background discarded),
//   1 = regular blending, 2 = additive, 3 = opaque.
uniform int uBlend;
uniform float uAlpha;
uniform float uWetness;        // wetness 0..1 (fx.c)
// Water reflection cubemap (unit 9, scene_uniforms_bind): wet trees
// (billboards) get gloss and reflect the environment.
uniform samplerCube uReflection;
// Wetness noise (img/noise.png, unit 11): patches "wetter / almost dry"
uniform sampler2D uNoise;

// Snow cover: shared snow.glsl block. Billboards: snow on the tops
// (crown at the top of the PNG, vUV.y -> 0 due to UV flip).
uniform float uSnowCover;
uniform sampler2D uSnowMask;
uniform vec2 uWorldSize;
#include "snow.glsl"

// Lights: all directional ones from map.json (index 0 — the sun, it casts shadows)
#define SUN_MAX_LIGHTS 4
uniform vec3 uLightDir[SUN_MAX_LIGHTS];
uniform vec3 uLightColor[SUN_MAX_LIGHTS];
uniform float uLightIntensity[SUN_MAX_LIGHTS];
uniform int uLightCount;
#define SCENE_MAX_POINT 8
#define SCENE_MAX_SPOT  4
// Point lights (map.json lights[]: point) — local light with quadratic
// distance falloff, not faded into the cascade shadow.
uniform vec3 uPointPos[SCENE_MAX_POINT];
uniform vec3 uPointColor[SCENE_MAX_POINT];
uniform float uPointIntensity[SCENE_MAX_POINT];
uniform float uPointRange[SCENE_MAX_POINT];
uniform int uPointCount;
// Spotlights (spot): like point lights but within a direction cone (see map)
uniform vec3 uSpotPos[SCENE_MAX_SPOT];
uniform vec3 uSpotDir[SCENE_MAX_SPOT];
uniform vec3 uSpotColor[SCENE_MAX_SPOT];
uniform float uSpotIntensity[SCENE_MAX_SPOT];
uniform float uSpotRange[SCENE_MAX_SPOT];
uniform float uSpotAngle[SCENE_MAX_SPOT];
uniform int uSpotCount;
uniform vec3 uAmbient;
uniform vec3 uSkyColor;
uniform vec3 uGroundColor;
uniform vec3 uProbeAmb;          // baked ambient at the object's position (with bounces)
uniform float uProbeSun;         // baked sun visibility (0..1) from static obstacles
uniform float uProbeOn;          // 1 = use baked light (r_baked_light + data present)
uniform vec3 uFogColor;
uniform float uFogStart;
uniform float uFogEnd;
uniform float uFogOn;           // 1 = fog enabled (cvar r_fog_mode)

uniform sampler2DShadow uShadowMap0;  // shadow cascades — SEPARATE samplers:
uniform sampler2DShadow uShadowMap1;  // sampler array indexing
uniform sampler2DShadow uShadowMap2;  // is forbidden in GLSL 330 (Mesa)
uniform sampler2DShadow uShadowMap3;
uniform mat4 uShadowMatrix[4];
uniform float uShadowBias[4];
uniform sampler2D uShadowDepth0;
uniform sampler2D uShadowDepth1;
uniform sampler2D uShadowDepth2;
uniform sampler2D uShadowDepth3;
uniform float uShadowGrad[4];
uniform float uShadowOn;

out vec4 FragColor;

// One cascade shadow: PCF + depth gradient (as in wheat.fs)
// Pure PCF: depth gradient removed (on 1024² it produced banding)
float cascade_tap(sampler2DShadow map, float bias, vec3 suv) {
    return texture(map, vec3(suv.xy, suv.z - bias));
}

float sample_cascade(vec3 wp, int c) {
    vec4 sc = uShadowMatrix[c] * vec4(wp, 1.0);
    vec3 suv = sc.xyz / sc.w;
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

// cascade bounds — uShadowBox[4]: the RADIUS of a cascade's coverage (half
// the box side; the box is centered on the camera) = {4,8,48,256}.
uniform float uShadowBox[4];

float object_shadow(vec3 wp) {
    if (uShadowOn < 0.5) return 1.0;
    float dist = distance(uCamPos, wp);
    // Distant trees: billboard self-shadowing at 100+ m is unreadable, while the
    // cost is 1-2 CSM taps on EVERY tree pixel (they are the most on screen).
    if (dist >= 100.0) return 1.0;
    if (dist < uShadowBox[0]) {
        float t = smoothstep(uShadowBox[0] * 0.8, uShadowBox[0], dist);
        // Early exit outside the blend zone: most pixels pay for just 1 tap
        if (t < 0.02) return sample_cascade(wp, 0);
        if (t > 0.98) return sample_cascade(wp, 1);
        return mix(sample_cascade(wp, 0), sample_cascade(wp, 1), t);
    }
    if (dist < uShadowBox[1]) {
        float t = smoothstep(uShadowBox[1] * 0.8, uShadowBox[1], dist);
        if (t < 0.02) return sample_cascade(wp, 1);
        if (t > 0.98) return sample_cascade(wp, 2);
        return mix(sample_cascade(wp, 1), sample_cascade(wp, 2), t);
    }
    if (dist < uShadowBox[2]) {
        float t = smoothstep(uShadowBox[2] * 0.8, uShadowBox[2], dist);
        if (t < 0.02) return sample_cascade(wp, 2);
        if (t > 0.98) return sample_cascade(wp, 3);
        return mix(sample_cascade(wp, 2), sample_cascade(wp, 3), t);
    }
    return sample_cascade(wp, 3);
}

void main() {
    vec4 c = texture(uTex, vUV);
    // Alpha-test and opaque: discard the transparent background;
    // alpha/additive — translucency via uAlpha.
    if (uBlend == 0 || uBlend == 3) {
        if (c.a < 0.5) discard;
    }
    vec3 N = normalize(uCamPos - vWorldPos);   // billboard faces the camera
    vec3 V = N;
    // Wetness/snow are rare weather modes: in clear weather (99% of the time)
    // the blocks are skipped ENTIRELY (uniform branch is free): saves
    // ~4 texture fetches per tree pixel.
    float wet = 0.0;
    if (uWetness > 0.004) {
        wet = clamp(uWetness, 0.0, 1.0);
        // Wetness unevenness: img/noise.png — small patches (noise texel
        // ≈ 6 m / 1.25 m): every tree sits in its own wetness patch.
        float noise = texture(uNoise, vWorldPos.xz / 1600.0).r * 0.7
                    + texture(uNoise, vWorldPos.xz / 320.0 + 0.6311).r * 0.3;
        wet *= mix(0.05, 1.8, noise * noise);
        c.rgb = mix(c.rgb, c.rgb * 0.7, wet * 0.6);
    }

    // Snow cover (shared snow.glsl block): a billboard's crown is the top
    // of the image (v=0 after the UV flip, see object.vs). The crown itself
    // occludes the sky, so the mask gets only half the weight — snow
    // also lies under neighboring overhanging crowns. Smooth curve,
    // no noisy patches: the crown whitens evenly as snow accumulates.
    if (uSnowCover > 0.02) {
        float smask = texture(uSnowMask, vWorldPos.xz / uWorldSize).r;
        float cover = smoothstep(0.10, 0.55, uSnowCover);
        float atop  = 1.0 - smoothstep(0.15, 0.9, vUV.y);    // top along the tree
        float a = cover * atop * (0.5 + 0.5 * smask) * (0.85 + 0.15 * texture(uNoise, vWorldPos.xz / 1.6).r);
        if (a > 0.001) c.rgb = snow_color(c.rgb, vWorldPos, a);
    }

    // Hemispheric ambient: sky above, ground bounce below
    vec3 amb = mix(uGroundColor, uSkyColor, N.y * 0.5 + 0.5) * uAmbient;
    if (uProbeOn > 0.5) amb = uProbeAmb;   // baked light: bounces + AO + fill
    float shadow = object_shadow(vWorldPos);
    float sunvis = (uProbeOn > 0.5) ? uProbeSun : 1.0;  // static-obstacle shadow from the bake

    // Diffuse is almost FLAT, like wheat: a billboard's normal is meaningless.
    // EVERY directional light shines; the CSM shadow — only the sun (0).
    // In shadow the hemispheric light fades to 1/4 (deeper darkness beyond).
    vec3 light = amb * mix(0.25, 1.0, shadow);
    vec3 L = vec3(0.0);
    for (int i = 0; i < uLightCount; i++) {
        vec3 li = normalize(uLightDir[i]);
        float diff = 0.55 + 0.25 * max(dot(N, li), 0.0);
        float sh = (i == 0) ? shadow * sunvis : 1.0;
        light += uLightColor[i] * uLightIntensity[i] * diff * sh;
        if (i == 0) L = li;
    }
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
    // Spotlights: point light + cone with a soft edge
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
    light = min(light, vec3(1.0));       // doesn't burn out to white, like wheat
    vec3 col = c.rgb * uTint * light;

    // wetness: add a specular highlight on wet surfaces (the billboard is oriented along V)
    if (wet > 0.0) {
        for (int i = 0; i < uLightCount; i++) {
            vec3 li = normalize(uLightDir[i]);
            vec3 H = normalize(li + V);
            float s = pow(max(dot(N, H), 0.0), 64.0);
            col += uLightColor[i] * uLightIntensity[i] * s * wet * 1.5;
        }
        // Wet gloss: reflection cubemap. The billboard normal always points
        // at the camera — for the reflection take a "leaf" normal: up (crown)
        // mixed with the view, so wet leaves reflect the sky and environment.
        vec3 leafN = normalize(vec3(0.0, 1.0, 0.0) * 0.65 + N * 0.35);
        vec3 reflDir = reflect(-V, leafN);
        vec3 reflCol = texture(uReflection, reflDir).rgb;
        float fres = pow(1.0 - max(dot(leafN, V), 0.0), 3.0);
        col = mix(col, reflCol, wet * (0.08 + 0.35 * fres));
    }

    // GLOWING RIM OUTLINE under BACKLIGHT (like wheat): looking
    // at a tree from the shadowed side, the sun is behind it — the silhouette
    // glows with a rim. Two layers: a sharp ring (~1.5 texels) + a soft halo
    // (~3 texels). Works up to 180 m (trees stand far from spawn).
    vec3 lit = col;
    float dist = distance(uCamPos, vWorldPos);
    if (uLightCount > 0 && length(L) > 0.5) {
        float near_fade = 1.0 - smoothstep(40.0, 180.0, dist);
        // In RAIN the tree rim light is off (wet scene without glow)
        float backlit = (1.0 - smoothstep(-0.1, 0.55, dot(L, V))) * near_fade
                      * (1.0 - clamp(wet, 0.0, 1.0));
        if (backlit > 0.001) {
            vec2 tv = 1.5 / vec2(textureSize(uTex, 0).xy);
            vec2 tv2 = tv * 2.0;
            float o1 = max(max(1.0 - texture(uTex, vec2(vUV.x + tv.x, vUV.y)).a,
                               1.0 - texture(uTex, vec2(vUV.x - tv.x, vUV.y)).a),
                           max(1.0 - texture(uTex, vec2(vUV.x, vUV.y + tv.y)).a,
                               1.0 - texture(uTex, vec2(vUV.x, vUV.y - tv.y)).a));
            float o2 = max(max(1.0 - texture(uTex, vec2(vUV.x + tv2.x, vUV.y)).a,
                               1.0 - texture(uTex, vec2(vUV.x - tv2.x, vUV.y)).a),
                           max(1.0 - texture(uTex, vec2(vUV.x, vUV.y + tv2.y)).a,
                               1.0 - texture(uTex, vec2(vUV.x, vUV.y - tv2.y)).a));
            // 2x+ stronger: brighter ring and halo
            float glow = smoothstep(0.2, 0.7, o1) * 0.9
                       + smoothstep(0.1, 0.45, o2) * 0.55;
            col += lit * backlit * glow;
        }
    }

    // Distance fog. With uFogOn = 0 (r_fog_mode 0) — zero cost.
    if (uFogOn > 0.5) {
        float fog = smoothstep(uFogStart, uFogEnd, dist);
        col = mix(col, uFogColor, fog);
    }
    // alphatest: texture alpha (0.5..1 at the edge) — with MSAA active
    // alpha-to-coverage is on, partial coverage smooths the edge.
    float a = (uBlend == 0 || uBlend == 3) ? c.a : c.a * uAlpha;
    FragColor = vec4(col, a);
}
