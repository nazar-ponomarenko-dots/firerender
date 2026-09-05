// ============================================================================
//  Material — фрагментный шейдер материальной системы (см. matprog.c)
// ----------------------------------------------------------------------------
//  Тело одно, а ВАРИАНТЫ (пермутации) собираются #define'ами, которые
//  матсистема подставляет перед компиляцией (как шейдерные пермутации в
//  Source: COMPILE_宏 в LeakNet, или варианты Cg в Far Cry). Фича, не
//  нужная материалу, исключается из шейдера целиком — фрагментный этап
//  дешевле (нет веток по uSpecular/uWetness, нет mtime у ненужных текстур):
//
//    MAT_SPECULAR    спекуляр (Blinn-Phong) + мокрый блик
//    MAT_WETNESS     мокрость: затемнение, шум карты, кубомап-глянец
//    MAT_EMISSIVE    эмиссия (uEmissive * альбедо) поверх света
//    MAT_ALPHATEST   альфа-тест (discard) — только для alphatest-материалов
//
//  Остальное (тени CSM, туман, точечные/прожекторные источники) — общее,
//  тело идентично mesh.fs, чтобы картинка совпадала пиксель-в-пиксель.
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
uniform float uGloss;          // жёсткость блика (0 = дефолт 32)
uniform vec3 uEmissive;        // эмиссия (MAT_EMISSIVE)
uniform float uWetness;        // мокрость 0..1 (fx.c)
uniform samplerCube uReflection;
uniform sampler2D uNoise;

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

#include "csm.glsl"

out vec4 FragColor;

float mesh_shadow(vec3 wp) {
    if (uShadowOn < 0.5) return 1.0;
    return csm_select_std(wp);
}

void main() {
    float gloss = uGloss > 0.5 ? uGloss : 32.0;
    vec4 c = texture(uTex, vUV);
#ifdef MAT_ALPHATEST
    if (c.a < 0.5) discard;
#endif
    vec3 N = normalize(vNormal);
    vec3 V = normalize(uCamPos - vWorldPos);
    float wet = 0.0;
#ifdef MAT_WETNESS
    wet = clamp(uWetness, 0.0, 1.0);
    float noise = texture(uNoise, vWorldPos.xz / 1600.0).r * 0.7
                + texture(uNoise, vWorldPos.xz / 320.0 + 0.6311).r * 0.3;
    wet *= mix(0.05, 1.8, noise * noise);
    c.rgb = mix(c.rgb, c.rgb * 0.6, wet * 0.7);
#endif

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
#ifdef MAT_SPECULAR
        if (uSpecular > 0.001) {
            vec3 H = normalize(L + V);
            spec += uLightColor[i] * uLightIntensity[i] * sh
                  * pow(max(dot(N, H), 0.0), gloss);
        }
#endif
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
    vec3 col = c.rgb * uTint * light;
#ifdef MAT_SPECULAR
    col += spec * uSpecular;
#endif

#ifdef MAT_EMISSIVE
    col += c.rgb * uEmissive;
#endif

#ifdef MAT_SPECULAR
    if (wet > 0.0 && uSpecular > 0.001) {
        float wet_spec = wet * uSpecular * 2.0;
        for (int i = 0; i < uLightCount; i++) {
            vec3 L = normalize(-uLightDir[i]);
            vec3 H = normalize(L + V);
            float s = pow(max(dot(N, H), 0.0), gloss);
            col += uLightColor[i] * uLightIntensity[i] * s * wet_spec;
        }
    }
#endif

#ifdef MAT_WETNESS
    if (wet > 0.001) {
        vec3 reflDir = reflect(-V, N);
        vec3 reflCol = texture(uReflection, reflDir).rgb;
        float fres = pow(1.0 - max(dot(N, V), 0.0), 3.0);
        col = mix(col, reflCol, wet * (0.06 + 0.30 * fres));
    }
#endif

    float dist = distance(uCamPos, vWorldPos);
    if (uFogOn > 0.5) {
        float fog = smoothstep(uFogStart, uFogEnd, dist);
        col = mix(col, uFogColor, fog);
    }
    float a = (uBlend == 0 || uBlend == 3) ? 1.0 : c.a * uAlpha;
    FragColor = vec4(col, a);
}
