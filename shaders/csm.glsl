// ============================================================================
//  csm.glsl — shared cascaded shadow code (included via '#include "csm.glsl"')
// ----------------------------------------------------------------------------
//  Single source of truth for terrain/wheat/object/mesh/material/volumetric:
//  cascade uniform declarations, single-cascade sampling (hardware PCF:
//  GL_COMPARE + LINEAR — one texture() yields a smooth penumbra) and cascade
//  selection by distance with smooth blending at the uShadowBox[0..2] borders
//  (uShadowBox[3] — hard shadow range limit). The borders arrive as a uniform
//  from RendererContext (renderer.c) — no more hardcoded #define SHADOW_BOX*
//  out of sync with C.
//
//  Requires uCamPos to be declared BEFORE the include.
// ============================================================================

// Cascades are separate samplers: indexing sampler arrays is forbidden
// in GLSL 330 on Mesa. uShadowMap* bind to units 3..6 (for volumetric —
// 5..8, static at program initialization).
uniform sampler2DShadow uShadowMap0;   // 0 = at your feet, 3 = far away
uniform sampler2DShadow uShadowMap1;
uniform sampler2DShadow uShadowMap2;
uniform sampler2DShadow uShadowMap3;
uniform mat4 uShadowMatrix[4];         // world -> cascade NDC (with 0.5 bias)
uniform float uShadowBias[4];          // depth offset against acne
uniform float uShadowBox[4];           // blend borders; [3] = shadow limit
uniform float uShadowOn;               // 1 = shadows enabled

float csm_tap(sampler2DShadow map, float bias, vec3 suv) {
    return texture(map, vec3(suv.xy, suv.z - bias));
}

// Project a world point into cascade c; outside its volume — no shadow (1.0).
float csm_sample_cascade(vec3 wp, int c, float bias) {
    vec4 sc = uShadowMatrix[c] * vec4(wp, 1.0);
    vec3 suv = sc.xyz / sc.w;
    if (suv.z <= 0.0 || suv.z >= 1.0 || suv.x <= 0.0 || suv.x >= 1.0 ||
        suv.y <= 0.0 || suv.y >= 1.0)
        return 1.0;
    if (c == 0) return csm_tap(uShadowMap0, bias, suv);
    if (c == 1) return csm_tap(uShadowMap1, bias, suv);
    if (c == 2) return csm_tap(uShadowMap2, bias, suv);
    return csm_tap(uShadowMap3, bias, suv);
}

float csm_pick_bias(int c) {
    if (c == 0) return uShadowBias[0];
    if (c == 1) return uShadowBias[1];
    if (c == 2) return uShadowBias[2];
    return uShadowBias[3];
}

// Cascade selection by distance to camera: blending in a ~20% zone before
// the border (no banding rings between cascades), beyond uShadowBox[3] — 1.0.
float csm_select(vec3 wp, float bias0, float bias1, float bias2, float bias3) {
    float dist = distance(uCamPos, wp);
    if (dist >= uShadowBox[3]) return 1.0;
    if (dist < uShadowBox[0]) {
        float t = smoothstep(uShadowBox[0] * 0.8, uShadowBox[0], dist);
        return mix(csm_sample_cascade(wp, 0, bias0),
                   csm_sample_cascade(wp, 1, bias1), t);
    }
    if (dist < uShadowBox[1]) {
        float t = smoothstep(uShadowBox[1] * 0.8, uShadowBox[1], dist);
        return mix(csm_sample_cascade(wp, 1, bias1),
                   csm_sample_cascade(wp, 2, bias2), t);
    }
    if (dist < uShadowBox[2]) {
        float t = smoothstep(uShadowBox[2] * 0.8, uShadowBox[2], dist);
        return mix(csm_sample_cascade(wp, 2, bias2),
                   csm_sample_cascade(wp, 3, bias3), t);
    }
    return csm_sample_cascade(wp, 3, bias3);
}

// Standard variant: bias comes from uShadowBias[cascade].
float csm_select_std(vec3 wp) {
    return csm_select(wp, uShadowBias[0], uShadowBias[1],
                      uShadowBias[2], uShadowBias[3]);
}
