// ============================================================================
//  Mesh — фрагментный шейдер статичного меша (.obj prop_static)
// ----------------------------------------------------------------------------
//  Как object.fs, но нормаль — НАСТОЯЩАЯ вершинная, поэтому полноценный
//  Ламбертов диффуз + лёгкий спекуляр. Тени CSM, туман, tint — как у объектов.
// ============================================================================

in vec3 vWorldPos;
in vec3 vNormal;
in vec2 vUV;

uniform sampler2D uTex;
uniform vec3 uCamPos;
uniform vec3 uTint;
uniform int uBlend;
uniform float uAlpha;
uniform float uSpecular;       // сила спекуляра (0 = матовый)

#define SUN_MAX_LIGHTS 4
uniform vec3 uLightDir[SUN_MAX_LIGHTS];
uniform vec3 uLightColor[SUN_MAX_LIGHTS];
uniform float uLightIntensity[SUN_MAX_LIGHTS];
uniform int uLightCount;
#define SCENE_MAX_POINT 8
#define SCENE_MAX_SPOT  4
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
uniform float uShadowGrad[4];
uniform float uShadowOn;

out vec4 FragColor;

float cascade_tap(sampler2DShadow map, float bias, vec3 suv) {
    return texture(map, vec3(suv.xy, suv.z - bias));
}

float sample_cascade(vec3 wp, int c) {
    vec4 sc = uShadowMatrix[c] * vec4(wp, 1.0);
    vec3 suv = sc.xyz / sc.w;
    if (suv.z <= 0.0 || suv.z >= 1.0 || suv.x <= 0.0 || suv.x >= 1.0 ||
        suv.y <= 0.0 || suv.y >= 1.0)
        return 1.0;
    if (c == 0) return cascade_tap(uShadowMap0, uShadowBias[0], suv);
    if (c == 1) return cascade_tap(uShadowMap1, uShadowBias[1], suv);
    if (c == 2) return cascade_tap(uShadowMap2, uShadowBias[2], suv);
    return cascade_tap(uShadowMap3, uShadowBias[3], suv);
}

#define SHADOW_BOX0 4.0f
#define SHADOW_BOX1 8.0f
#define SHADOW_BOX2 48.0f
#define SHADOW_LIMIT 256.0f

float mesh_shadow(vec3 wp) {
    if (uShadowOn < 0.5) return 1.0;
    float dist = distance(uCamPos, wp);
    if (dist >= SHADOW_LIMIT) return 1.0;
    if (dist < SHADOW_BOX0) {
        float t = smoothstep(SHADOW_BOX0 * 0.8, SHADOW_BOX0, dist);
        return mix(sample_cascade(wp, 0), sample_cascade(wp, 1), t);
    }
    if (dist < SHADOW_BOX1) {
        float t = smoothstep(SHADOW_BOX1 * 0.8, SHADOW_BOX1, dist);
        return mix(sample_cascade(wp, 1), sample_cascade(wp, 2), t);
    }
    if (dist < SHADOW_BOX2) {
        float t = smoothstep(SHADOW_BOX2 * 0.8, SHADOW_BOX2, dist);
        return mix(sample_cascade(wp, 2), sample_cascade(wp, 3), t);
    }
    return sample_cascade(wp, 3);
}

void main() {
    vec4 c = texture(uTex, vUV);
    if (uBlend == 0 || uBlend == 3) {
        if (c.a < 0.5) discard;
    }
    vec3 N = normalize(vNormal);
    vec3 V = normalize(uCamPos - vWorldPos);

    // двухсторонняя геометрия: переворачиваем нормаль к камере, чтобы
    // внутренние грани (и дубли вершин без нормалей) не выглаживались в тьму
    if (dot(N, V) < 0.0) N = -N;

    vec3 amb = mix(uGroundColor, uSkyColor, N.y * 0.5 + 0.5) * uAmbient;
    float shadow = mesh_shadow(vWorldPos);

    vec3 light = amb * mix(0.25, 1.0, shadow);
    vec3 spec = vec3(0.0);
    for (int i = 0; i < uLightCount; i++) {
        vec3 L = normalize(-uLightDir[i]);
        float d = max(dot(N, L), 0.0);
        float sh = (i == 0) ? shadow : 1.0;
        light += uLightColor[i] * uLightIntensity[i] * d * sh;
        if (uSpecular > 0.001) {
            vec3 H = normalize(L + V);
            spec += uLightColor[i] * uLightIntensity[i] * sh
                  * pow(max(dot(N, H), 0.0), 32.0);
        }
    }
    for (int i = 0; i < uPointCount; i++) {
        vec3 Lp = uPointPos[i] - vWorldPos;
        float dpl = length(Lp);
        vec3 L = Lp / max(dpl, 0.001);
        float att = max(1.0 - dpl / uPointRange[i], 0.0);
        att *= att;
        float d = max(dot(N, L), 0.0);
        light += uPointColor[i] * uPointIntensity[i] * d * att;
    }
    for (int i = 0; i < uSpotCount; i++) {
        vec3 Lp = uSpotPos[i] - vWorldPos;
        float dpl = length(Lp);
        vec3 L = Lp / max(dpl, 0.001);
        float att = max(1.0 - dpl / uSpotRange[i], 0.0);
        att *= att;
        float cone = max(dot(-L, normalize(uSpotDir[i])), 0.0);
        float edge = smoothstep(uSpotAngle[i] - 0.08, uSpotAngle[i], cone);
        float d = max(dot(N, L), 0.0);
        light += uSpotColor[i] * uSpotIntensity[i] * d * att * edge;
    }
    light = min(light, vec3(1.0));
    vec3 col = c.rgb * uTint * light + spec * uSpecular;

    float dist = distance(uCamPos, vWorldPos);
    if (uFogOn > 0.5) {
        float fog = smoothstep(uFogStart, uFogEnd, dist);
        col = mix(col, uFogColor, fog);
    }
    float a = (uBlend == 0 || uBlend == 3) ? 1.0 : c.a * uAlpha;
    FragColor = vec4(col, a);
}