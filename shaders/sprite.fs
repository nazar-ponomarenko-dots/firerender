// ============================================================================
//  Sprite3D — fragment shader for an attachable sprite
// ----------------------------------------------------------------------------
//  Lighting is simple and "in engine style": hemispheric ambient + flat
//  diffuse from directional lights + the sun's CSM shadow + fog. A sprite's
//  normal is nominal, so the diffuse is nearly flat (like vegetation
//  billboards) — the sprite doesn't "flash" when the camera rotates.
//  uUnlit = 1 — turn lighting off entirely (icons, in-world UI markers).
// ============================================================================

in vec2 vUV;
in vec3 vWorldPos;
in vec3 vNormal;

uniform sampler2D uTex;
uniform vec3 uCamPos;
uniform vec3 uTint;
uniform int uBlend;            // 0 alpha-test, 1 alpha, 2 additive, 3 opaque
uniform float uAlpha;
uniform float uUnlit;          // 1 = unlit (pure texture * tint)

#define SUN_MAX_LIGHTS 4
uniform vec3 uLightDir[SUN_MAX_LIGHTS];
uniform vec3 uLightColor[SUN_MAX_LIGHTS];
uniform float uLightIntensity[SUN_MAX_LIGHTS];
uniform int uLightCount;
#define SCENE_MAX_POINT 8
uniform vec3 uPointPos[SCENE_MAX_POINT];
uniform vec3 uPointColor[SCENE_MAX_POINT];
uniform float uPointIntensity[SCENE_MAX_POINT];
uniform float uPointRange[SCENE_MAX_POINT];
uniform int uPointCount;
uniform vec3 uAmbient;
uniform vec3 uSkyColor;
uniform vec3 uGroundColor;
uniform vec3 uFogColor;
uniform float uFogStart;
uniform float uFogEnd;
uniform float uFogOn;

uniform sampler2DShadow uShadowMap0;
uniform sampler2DShadow uShadowMap1;
uniform sampler2DShadow uShadowMap2;
uniform sampler2DShadow uShadowMap3;
uniform mat4 uShadowMatrix[4];
uniform float uShadowBias[4];
uniform float uShadowOn;

out vec4 FragColor;

float cascade(int c, vec3 wp) {
    vec4 sc = uShadowMatrix[c] * vec4(wp, 1.0);
    vec3 suv = sc.xyz / sc.w;
    if (suv.z <= 0.0 || suv.z >= 1.0 || suv.x <= 0.0 || suv.x >= 1.0 ||
        suv.y <= 0.0 || suv.y >= 1.0)
        return 1.0;
    float b = uShadowBias[c];
    if (c == 0) return texture(uShadowMap0, vec3(suv.xy, suv.z - b));
    if (c == 1) return texture(uShadowMap1, vec3(suv.xy, suv.z - b));
    if (c == 2) return texture(uShadowMap2, vec3(suv.xy, suv.z - b));
    return texture(uShadowMap3, vec3(suv.xy, suv.z - b));
}

float shadow_at(vec3 wp) {
    if (uShadowOn < 0.5) return 1.0;
    float d = distance(uCamPos, wp);
    if (d >= 256.0) return 1.0;
    if (d < 4.0)  return cascade(0, wp);
    if (d < 8.0)  return cascade(1, wp);
    if (d < 48.0) return cascade(2, wp);
    return cascade(3, wp);
}

void main() {
    vec4 c = texture(uTex, vUV);
    if (uBlend == 0 || uBlend == 3) {
        if (c.a < 0.5) discard;              // alpha-test
    }
    vec3 col = c.rgb * uTint;
    if (uUnlit < 0.5) {
        vec3 N = normalize(vNormal);
        // The sprite normal is nominal: tilt it "half upward" so the lighting
        // stays soft and stable while the camera rotates.
        vec3 Nl = normalize(N + vec3(0.0, 0.6, 0.0));
        vec3 amb = mix(uGroundColor, uSkyColor, Nl.y * 0.5 + 0.5) * uAmbient;
        float sh = shadow_at(vWorldPos);
        vec3 light = amb * mix(0.3, 1.0, sh);
        for (int i = 0; i < uLightCount; i++) {
            vec3 li = normalize(uLightDir[i]);
            float diff = 0.6 + 0.4 * max(dot(Nl, li), 0.0);
            light += uLightColor[i] * uLightIntensity[i] * diff
                   * (i == 0 ? sh : 1.0);
        }
        for (int i = 0; i < uPointCount; i++) {
            vec3 Lp = uPointPos[i] - vWorldPos;
            float dpl = length(Lp);
            float att = max(1.0 - dpl / max(uPointRange[i], 0.01), 0.0);
            att *= att;
            light += uPointColor[i] * uPointIntensity[i] * att;
        }
        col *= min(light, vec3(1.0));
    }
    float a = (uBlend == 0 || uBlend == 3) ? 1.0 : c.a * uAlpha;
    if (uFogOn > 0.5) {
        float fog = smoothstep(uFogStart, uFogEnd, distance(uCamPos, vWorldPos));
        col = mix(col, uFogColor, fog);
    }
    FragColor = vec4(col, a);
}
