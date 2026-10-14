// ============================================================================
//  Volumetric — fullscreen pass of voxel volumetric fog
// ----------------------------------------------------------------------------
//  Deferred: scene color+depth -> world point reconstruction -> march through
//  3D density cascades (like shadows, but in 3D), Beer-Lambert accumulation.
//  GOD RAYS: sun scattering at each point is shaded by the sun's CSM shadows
//  (hard PCF visibility — rays come out crisp), shadows are contrasty.
//  Realistic color: warm toward the sun, bluish to the side, halo near the sun.
//  The pass runs at HALF RESOLUTION (4x cheaper), then a bilinear
//  upsample (volumetric_up.fs).
// ============================================================================

in vec2 vUV;

uniform sampler2D uScene;        // scene color (unit 0)
uniform sampler2D uSceneDepth;   // scene depth (unit 1)
uniform mat4 uInvViewProj;
uniform vec3 uCamPos;
uniform float uZNear, uZFar;

uniform sampler3D uFogTex0;      // 3D density cascades (units 2-4)
uniform sampler3D uFogTex1;
uniform sampler3D uFogTex2;
uniform vec3 uFogCenter0, uFogCenter1, uFogCenter2;
uniform float uFogBox0, uFogBox1, uFogBox2;
uniform float uFogRes0, uFogRes1, uFogRes2;

uniform vec3 uSunDir;
uniform float uStrength;
uniform float uFogMode;        // 1 = cheap mode: only the coarse cascade 2
uniform vec3 uFogColor;      // BASE fog color (everywhere), from map.json
uniform vec3 uFogColorSun;   // warm color near the sun
uniform float uGlare;        // halo right at the sun
uniform float uRayStrength;  // god rays contrast (0 = none, 2 = sharp)
uniform float uWetness;      // wetness 0..1 — in rain fog is lit by the sky, not the sun
uniform vec3 uSkyColor;      // sky color (the fog's main light)

// Ground haze (rain): gray, ~0.5 m, with noise. The heightmap (unit 10)
// gives the exact terrain height at each march point — the layer lies on the slope.
// Haze noise — img/noise.png (unit 12): lively unevenness.
uniform sampler2D uHeightmap;
uniform sampler2D uNoise;
uniform vec2 uWorldSize;
uniform float uHeightMin, uHeightMax;
uniform float uTime;
uniform float uHaze;         // haze strength (0 = none), grows with wetness
uniform float uHazeH;        // layer thickness, m
uniform vec3 uWColor;        // fog tint over water bodies (average water color)

// Sun CSM shadows (separate samplers — GLSL 330)
uniform sampler2DShadow uShadowMap0;
uniform sampler2DShadow uShadowMap1;
uniform sampler2DShadow uShadowMap2;
uniform sampler2DShadow uShadowMap3;
uniform mat4 uShadowMatrix[4];
uniform float uShadowBias[4];
uniform float uShadowOn;

out vec4 FragColor;
uniform float uFogOnly;          // 1 = output only the fog (color+trans), without the scene

#define FOG_MAX 96.0f         // no fog beyond this (farthest cascade reaches 80 m)
#define C0_HALF 8.0f          // half-sizes of the cascade boxes
#define C1_HALF 24.0f
#define C2_HALF 80.0f
// cascade bounds for god rays — uShadowBox[4] from RendererContext
uniform float uShadowBox[4];

// Density at a world point: cascade chosen by distance from the camera.
// Returns vec2: x = base density (R), y = fog over water (G).
vec2 fog_density(vec3 p) {
    float dist = length(p - uCamPos);
    vec3 uvw;
    if (uFogMode > 0.5) {
        // Mode 2 (cheap): ONE coarse cascade (2, 5 m voxel) for everything —
        // no near cascades: cheaper in memory/CPU and in the march.
        uvw = (p - uFogCenter2) / uFogBox2 + 0.5;
    } else if (dist < C0_HALF)
        uvw = (p - uFogCenter0) / uFogBox0 + 0.5;
    else if (dist < C1_HALF)
        uvw = (p - uFogCenter1) / uFogBox1 + 0.5;
    else if (dist < C2_HALF)
        uvw = (p - uFogCenter2) / uFogBox2 + 0.5;
    else
        return vec2(0.0);
    if (uvw.x < 0.0 || uvw.x > 1.0 || uvw.y < 0.0 || uvw.y > 1.0 ||
        uvw.z < 0.0 || uvw.z > 1.0)
        return vec2(0.0);
    if (dist < C0_HALF) return texture(uFogTex0, uvw).rg;
    if (dist < C1_HALF) return texture(uFogTex1, uvw).rg;
    return texture(uFogTex2, uvw).rg;
}

// Hard sun visibility (PCF, no gradient) — crisp god rays
float point_vis(sampler2DShadow map, float bias, vec3 suv) {
    return texture(map, vec3(suv.xy, suv.z - bias));
}

float point_shadow(vec3 wp) {
    float dist = length(wp - uCamPos);
    if (dist >= uShadowBox[3]) return 1.0;
    int c;
    if (dist < uShadowBox[0]) c = 0;
    else if (dist < uShadowBox[1]) c = 1;
    else if (dist < uShadowBox[2]) c = 2;
    else c = 3;
    vec4 sc = uShadowMatrix[c] * vec4(wp, 1.0);
    vec3 suv = sc.xyz / sc.w;
    if (suv.z <= 0.0 || suv.z >= 1.0 || suv.x <= 0.0 || suv.x >= 1.0 ||
        suv.y <= 0.0 || suv.y >= 1.0)
        return 1.0;
    if (c == 0) return point_vis(uShadowMap0, uShadowBias[0], suv);
    if (c == 1) return point_vis(uShadowMap1, uShadowBias[1], suv);
    if (c == 2) return point_vis(uShadowMap2, uShadowBias[2], suv);
    return point_vis(uShadowMap3, uShadowBias[3], suv);
}

// Cheap hash for step dithering (removes banding)
float fog_hash(vec2 p) {
    return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453);
}

// Terrain height from the heightmap (in meters) — same formula as in terrain.vs
float sample_height16(vec2 uv) {
    vec2 h = texture(uHeightmap, uv).rg;
#ifdef FR_HEIGHT_RG8
    return (h.r * 65280.0 + h.g * 255.0) / 65535.0;
#else
    return h.r;
#endif
}

float terrain_height(vec3 p) {
    vec2 uv = clamp(p.xz / uWorldSize, 0.0, 1.0);
    return mix(uHeightMin, uHeightMax, sample_height16(uv));
}

// Ground haze (rain): a thin gray layer ~uHazeH on the terrain, with noise.
// Density falls off with height; noise — img/noise.png, LARGE patches (~30 m)
// visible from afar + drift over time. Weak — so it doesn't burn the background white.
float haze_density(vec3 p) {
    if (uHaze <= 0.001) return 0.0;
    float ground = terrain_height(p);
    float up = p.y - ground;
    if (up < 0.0) return 0.0;            // under the terrain — none
    float h = 1.0 - smoothstep(0.0, uHazeH, up);
    if (h <= 0.001) return 0.0;
    // three noise scales + drift; puffs are small (noise texel ~2.5..0.12 m)
    vec2 nm = p.xz + vec2(uTime * 1.2, uTime * 0.8);
    float n = texture(uNoise, nm / 600.0).r * 0.55
            + texture(uNoise, nm / 150.0 + 13.7).r * 0.30
            + texture(uNoise, nm / 30.0  -  7.3).r * 0.15;
    return uHaze * h * (0.10 + 0.90 * n);
}

// Regular (classic) fog around the player in rain: uniform haze,
// grows exponentially with distance (fast up close, steady far away).
// Color — gray (base). Noticeable, but not white.
float rain_fog_density(float dist) {
    return uWetness * 0.03 * (1.0 - exp(-dist / 30.0));
}

// Rain "fog class": the base cascades (density_ground/field from map.json)
// fade out with wetness — in rain, light sky-driven fog (rain_fog_density)
// replaces the dense haze. Factor 0.10 at full rain: ~10x thinner.
float base_fog_scale() {
    return 1.0 - 0.90 * uWetness;
}

void main() {
    vec3 scene = texture(uScene, vUV).rgb;
    float depth = texture(uSceneDepth, vUV).r;

    // World point from depth (sky is far away; fog dies out at FOG_MAX)
    vec4 ndc = vec4(vUV * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 wp4 = uInvViewProj * ndc;
    vec3 wp = wp4.xyz / wp4.w;
    vec3 ray = wp - uCamPos;
    float march = min(length(ray), FOG_MAX);
    vec3 dir = ray / max(length(ray), 1e-5);

    // Adaptive steps: more steps on a long ray, at least 8
    float stepLen = march / clamp(march / 6.0, 8.0, 20.0);
    // Step dither is ANIMATED (phase changes every frame): a static pattern
    // read as grain on the semi-transparent fog; the eye averages temporal
    // noise over a couple of frames — fog stays smooth, no quantization bands.
    float jit = fog_hash(vUV * vec2(1920.0, 1080.0) + fract(uTime * 0.7311) * 61.7);

    float sunHit = max(dot(dir, uSunDir), 0.0);
    vec3 inscat = vec3(0.0);
    float trans = 1.0;
    // Scattering color: solar (warm) EVERYWHERE — even in the direction
    // opposite the sun (only slightly diluted by the base, 15%). In RAIN
    // (uWetness -> 1) there is no sun behind the clouds: fog is FLAT GRAY
    // (base color, NO bright-sky mix) — it doesn't burn out to white far away.
    vec3 stepCol = mix(uFogColorSun, uFogColor, 0.15 * (1.0 - sunHit));
    stepCol = mix(stepCol, uFogColor, uWetness);

    for (int i = 0; i < 20; i++) {
        float t = (float(i) + 0.5 + jit * 0.8) * stepLen;
        if (t >= march) break;
        vec3 p = uCamPos + dir * t;
        vec2 fd = fog_density(p);
        // base fog fades out in rain, water fog does not (evaporation over
        // water is stronger exactly in dampness)
        float db = (fd.x * base_fog_scale()
                  + haze_density(p) + rain_fog_density(t)) * uStrength;
        float dw = fd.y * uStrength;               // fog over water bodies
        float d = db + dw;
        if (d <= 0.001) continue;
        float od = d * stepLen;                    // optical depth
        float ext = exp(-od);                      // Beer-Lambert
        // sun shadow at the point: where wheat/trees block the sun,
        // light in the fog dies — hence crisp rays (god rays)
        float sunShadow = 1.0;
        if (uShadowOn > 0.5 && sunHit > 0.02)
            sunShadow = point_shadow(p);
        // Away from the sun — BRIGHT warm haze (0.55), independent of the
        // shadow. Near the sun — contrast: fog in shadow is dark (0.2), gaps
        // are bright (up to ~1.3). Rays stay visible, the sun doesn't burn out.
        // In RAIN the sun is hidden by clouds: contrast and halo fade with
        // wetness — fog glows evenly from the sky.
        float rainDim = 1.0 - uWetness;
        float raystr = uRayStrength * rainDim;
        float litAway = 0.55;
        float litSun  = 0.20 + 0.80 * raystr * sunShadow;
        float lit = mix(litAway, litSun, sunHit * rainDim);
        lit = max(lit, mix(0.0, 0.32, uWetness));   // in rain — gray sky light
        // halo right at the sun (Mie-like peak in fog)
        float glare = pow(sunHit, 8.0) * uGlare * sunShadow * rainDim;
        // Step color: base warm fog, and over water the water body's tint
        // (uWColor, slightly lighter) is mixed in — weighted by densities.
        vec3 stepCol2 = mix(stepCol, uWColor * 1.15, dw / max(d, 1e-4));
        // In-scattered light growth: correctly Beer-Lambert (1 - ext), NOT od.
        // od in a dense thin layer (low haze) is huge and would overestimate
        // the light -> white glow. (1 - ext) caps at 1, so no white.
        inscat += stepCol2 * (1.0 - ext) * trans * (lit + glare * (1.0 - trans));
        trans *= ext;
        if (trans < 0.01) break;                   // beyond this nothing is visible — enough
    }

    if (uFogOnly > 0.5) {
        // Half-res pass mode: march into a half-size texture, composite on
        // screen separately (fog_comp.fs) — the march is 4x cheaper
        FragColor = vec4(inscat, trans);
        return;
    }
    vec3 col = scene * trans + inscat;
    FragColor = vec4(col, 1.0);
}
