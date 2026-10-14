// ============================================================================
//  Water — water fragment shader: puddles/lakes/rivers/sea (see water.c)
// ----------------------------------------------------------------------------
//  - Draws only where the terrain is below uLevel (depth > 0): dry shore is
//    discarded (discard). Depth comes from the heightmap (uHeightmap, unit 2).
//  - Wave normals are analytic, from the same waveY as in the VS (finite
//    differences): smooth at any grid resolution.
//  - Alpha grows with depth (uDepthFade) and fades toward the shore (vEdge).
//  - Pseudo-PBR like the terrain: hemispherical ambient + directional/point/
//    spot lights + specular + sky reflection via fresnel + shore foam +
//    distance fog.
// ============================================================================

#define SUN_MAX_LIGHTS 4
#define SCENE_MAX_POINT 8
#define SCENE_MAX_SPOT  4

in vec3 vWorldPos;
in vec2 vUV;
in float vEdge;

// --- Shared scene set (SceneUniforms, render_ctx.h) --------------------------
uniform vec3 uCamPos;
uniform vec3 uAmbient;
uniform vec3 uSkyColor;
uniform vec3 uGroundColor;
uniform vec3 uFogColor;
uniform float uFogStart;
uniform float uFogEnd;
uniform float uFogOn;

uniform vec3 uLightDir[SUN_MAX_LIGHTS];
uniform vec3 uLightColor[SUN_MAX_LIGHTS];
uniform float uLightIntensity[SUN_MAX_LIGHTS];
uniform int uLightCount;
uniform vec3 uPointPos[SCENE_MAX_POINT];
uniform vec3 uPointColor[SCENE_MAX_POINT];
uniform float uPointIntensity[SCENE_MAX_POINT];
uniform float uPointRange[SCENE_MAX_POINT];
uniform int uPointCount;
uniform vec3 uSpotPos[SCENE_MAX_SPOT];
uniform vec3 uSpotDir[SCENE_MAX_SPOT];
uniform vec3 uSpotColor[SCENE_MAX_SPOT];
uniform float uSpotIntensity[SCENE_MAX_SPOT];
uniform float uSpotRange[SCENE_MAX_SPOT];
uniform float uSpotAngle[SCENE_MAX_SPOT];
uniform int uSpotCount;

// --- Water's own uniforms (water_prog_init / water_instance_uniforms) --
uniform float uTime;
uniform float uLevel;
uniform float uWaveAmp;
uniform float uWaveSpeed;
uniform float uWaveLen;
uniform vec2  uFlowDir;
uniform vec3  uWaterColor;    // water tint at depth
uniform float uAlpha;         // base opacity 0..1
uniform float uDepthFade;     // how far the bottom shows through the water, m
uniform float uFlowSpeed;     // flow speed (shifts the specular)
uniform float uFoam;          // shore foam strength 0..1
uniform float uEdge;          // edge width (water_edge_fade)
uniform float uWetness;       // wetness 0..1 (rain)
uniform sampler2D uHeightmap;  // heightmap (unit 2, same as terrain)
uniform vec2  uWorldSize;      // world size (for heightmap UVs)
uniform float uHeightMin;      // height corresponding to heightmap value 0
uniform float uHeightMax;      // height corresponding to heightmap value 1
uniform samplerCube uReflCube;   // reflection cubemap (unit 7)

out vec4 FragColor;

// Wave function — same as in water.vs (normal via finite differences).
// The last term is fine ripple (short wave, small amplitude): the surface
// looks alive, not like smooth glass.
float waveY(vec2 p, float t) {
    float k = 6.2831853 / uWaveLen;
    vec2 f = uFlowDir;
    if (dot(f, f) < 0.0001) f = vec2(1.0, 0.0);
    f = normalize(f);
    vec2 q = vec2(-f.y, f.x);
    return uWaveAmp * sin(dot(f, p) * k + t * uWaveSpeed)
         + uWaveAmp * 0.45 * sin(dot(q, p) * k * 0.62 + t * uWaveSpeed * 1.17)
         + uWaveAmp * 0.2  * sin((p.x + p.y) * k * 0.34 + t * uWaveSpeed * 0.8)
         + uWaveAmp * 0.12 * sin(dot(f, p) * k * 4.7 + dot(q, p) * k * 3.1
                                 + t * uWaveSpeed * 1.6);
}

float sample_height16(vec2 uv) {
    vec2 h = texture(uHeightmap, uv).rg;
#ifdef FR_HEIGHT_RG8
    return (h.r * 65280.0 + h.g * 255.0) / 65535.0;
#else
    return h.r;
#endif
}

void main() {
    // Depth: terrain under the water (heightmap); water exists where below uLevel.
    float terrainH = mix(uHeightMin, uHeightMax,
                         sample_height16(vWorldPos.xz / uWorldSize));
    float waterDepth = uLevel - terrainH;
    if (waterDepth <= 0.02) discard;          // dry shore

    // Wave normal — finite differences of the same function as in the VS.
    vec2 p = vWorldPos.xz;
    float e = max(uWaveLen / 6.0, 0.1);
    float hL = waveY(p - vec2(e, 0.0), uTime);
    float hR = waveY(p + vec2(e, 0.0), uTime);
    float hD = waveY(p - vec2(0.0, e), uTime);
    float hU = waveY(p + vec2(0.0, e), uTime);
    vec3 N = vec3(hL - hR, 2.0 * e, hD - hU);

    // Fine ripple runs along the flow (high-frequency waves the finite
    // differences cannot resolve — added to the normal analytically). The
    // surface shimmers in the sun instead of looking like smooth glass.
    {
        float rk = 6.2831853 / max(uWaveLen * 0.22, 0.6);
        vec2 f = uFlowDir;
        if (dot(f, f) < 0.0001) f = vec2(1.0, 0.0);
        f = normalize(f);
        vec2 q = vec2(-f.y, f.x);
        float rt = uTime * uWaveSpeed * 1.4;
        float rp = uWaveAmp * 0.30;
        // two sets of short waves at angles — crossing grooves
        N.xy += rp * rk * 0.10 * vec2(
            cos(dot(f, p) * rk + rt) + cos((dot(q, p) * 0.7 + dot(f, p) * 0.5) * rk - rt * 1.3),
            cos(dot(q, p) * rk * 0.9 - rt) + cos((dot(f, p) * 0.6 - dot(q, p) * 0.4) * rk + rt * 0.9));
    }
    N = normalize(N);
    vec3 V = normalize(uCamPos - vWorldPos);
    // Two-sided geometry (as in mesh.fs): from below (underwater) the normal
    // is flipped toward the camera, otherwise the underside would be black.
    if (dot(N, V) < 0.0) N = -N;

    // Edge (0 at the shore .. 1 inside); on the open sea vEdge = 1e6 -> 1.
    float edgeFade = clamp(vEdge / max(uEdge, 0.001), 0.0, 1.0);
    // Opacity: earlier alpha grew LINEARLY over the full depth uDepthFade
    // (6 m for the sea) — at 1 m depth the water was nearly glass, the bottom
    // showed through. Now the curve is sharp (smoothstep over 30% of depth)
    // with a 45% minimum floor: from outside the shore strip looks like water,
    // not a film.
    float depthT = clamp(waterDepth / max(uDepthFade, 0.01), 0.0, 1.0);
    float alphaDepth = smoothstep(0.0, 0.30, depthT);
    float alpha = uAlpha * mix(0.45, 1.0, alphaDepth) * edgeFade;

    // Lighting (as for terrain/meshes): hemispherical ambient.
    vec3 amb = mix(uGroundColor, uSkyColor, N.y * 0.5 + 0.5) * uAmbient;
    vec3 light = amb;
    for (int i = 0; i < uLightCount; i++) {
        vec3 li = normalize(uLightDir[i]);
        float diff = max(dot(N, li), 0.0);
        // Threshold like the terrain (0.5+0.5 for matte), but a bit more
        // contrast — water gloss.
        light += uLightColor[i] * uLightIntensity[i] * (0.45 + 0.55 * diff);
    }
    for (int i = 0; i < uPointCount; i++) {
        vec3 Lp = uPointPos[i] - vWorldPos;
        float dpl = length(Lp);
        vec3 lip = Lp / max(dpl, 0.001);
        float att = max(1.0 - dpl / uPointRange[i], 0.0);
        att *= att;
        float diff = max(dot(N, lip), 0.0);
        light += uPointColor[i] * uPointIntensity[i] * (0.45 + 0.55 * diff) * att;
    }
    for (int i = 0; i < uSpotCount; i++) {
        vec3 Lp = uSpotPos[i] - vWorldPos;
        float dpl = length(Lp);
        vec3 lip = Lp / max(dpl, 0.001);
        float att = max(1.0 - dpl / uSpotRange[i], 0.0);
        att *= att;
        float cone = max(dot(-lip, normalize(uSpotDir[i])), 0.0);
        float edg = smoothstep(uSpotAngle[i] - 0.08, uSpotAngle[i], cone);
        float diff = max(dot(N, lip), 0.0);
        light += uSpotColor[i] * uSpotIntensity[i] * (0.45 + 0.55 * diff) * att * edg;
    }
    light = min(light, vec3(1.0));

    // Water color changes with depth: in shallow water the bottom is visible
    // (blended toward a warm light tone), at depth — saturated dark water color.
    // depthT already exists above: 0 at the shore, 1 at depth uDepthFade.
    vec3 shallow = uWaterColor * 1.35 + uSkyColor * 0.25;   // lighter and warmer
    vec3 deep    = uWaterColor * 0.55;                      // darker and denser
    vec3 col = mix(shallow, deep, depthT) * light;

    // Water color must always stand out: don't let it degenerate into a black
    // hole or blend into the sky. The base is water; the fresnel sky-mix stays
    // moderate.
    col = max(col, uSkyColor * 0.10);

    // Sun glints on the waves: a wide soft path + narrow sparks.
    // The path (spec) runs along the crests; the sparks come from fine ripple —
    // short flashes when the ripple normal exactly catches the reflection.
    if (uLightCount > 0) {
        vec3 li = normalize(uLightDir[0]);   // sun = light 0
        vec3 H = normalize(li + V);
        float diff0 = max(dot(li, N), 0.0);
        float spec = pow(max(dot(N, H), 0.0), 70.0);       // path
        float spark = pow(max(dot(N, H), 0.0), 260.0);     // sparks
        vec3 specCol = uLightColor[0] * uLightIntensity[0]
                     * (0.5 + 0.5 * diff0);
        col += specCol * spec * 0.9;
        col += specCol * spark * 2.2;
    }

    // Reflection: compute the reflected ray from the wave normal and sample
    // the reflection cubemap. I is the ray camera -> surface; reflect(I, N) gives
    // the direction in which the world "reflects" at the water point.
    vec3 I = normalize(vWorldPos - uCamPos);
    vec3 reflDir = reflect(I, N);
    vec3 reflCol = texture(uReflCube, reflDir).rgb;
    // Fresnel blending: base 0.10 (water always reflects something) + growth at
    // grazing angles. pow5 with a clean zero base made reflections invisible
    // from almost all viewing angles.
    float fres = 0.10 + 0.90 * pow(1.0 - max(dot(N, V), 0.0), 5.0);
    col = mix(col, reflCol, clamp(fres * 0.85 + 0.10, 0.0, 1.0));

    // Foam at the shore: not a solid band but living moving patches —
    // the "surf edge" crawls along the flow direction and tears apart by noise.
    {
        float shoreT = 1.0 - smoothstep(0.0, 2.0, vEdge);   // 0..2 m band
        vec2 fp = p * 0.7 + uFlowDir * (uTime * uFlowSpeed * 2.0);
        float fr = fract(sin(dot(floor(fp), vec2(127.1, 311.7))) * 43758.5453);
        float wob = sin(uTime * 2.2 + fr * 6.283 + vEdge * 3.0) * 0.5 + 0.5;
        float foamAmt = uFoam * shoreT * smoothstep(fr, fr + 0.3, wob);
        col = mix(col, uSkyColor * 0.9 + 0.1, foamAmt);
    }

    // Distance fog (as in the terrain).
    if (uFogOn > 0.5) {
        float fog = smoothstep(uFogStart, uFogEnd, distance(uCamPos, vWorldPos));
        col = mix(col, uFogColor, fog);
    }

    FragColor = vec4(col, alpha);
}