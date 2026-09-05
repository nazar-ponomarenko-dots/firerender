// ============================================================================
//  csm.glsl — общий каскадный теневой код (подключается через '#include "csm.glsl"')
// ----------------------------------------------------------------------------
//  Один источник правды для terrain/wheat/object/mesh/material/volumetric:
//  объявления uniform'ов каскадов, выборка одного каскада (аппаратный PCF:
//  GL_COMPARE + LINEAR — один texture() даёт гладкую полутень) и выбор
//  каскада по дистанции с плавным смешиванием на границах uShadowBox[0..2]
//  (uShadowBox[3] — жёсткий лимит дальности теней). Границы приходят
//  uniform'ом из RendererContext (renderer.c) — больше никаких захардкоженных
//  #define SHADOW_BOX*, рассинхронизированных с C.
//
//  Требует объявленными ДО include: uCamPos.
// ============================================================================

// Каскады — отдельные сэмплеры: индексация массивов сэмплеров запрещена
// в GLSL 330 на Mesa. uShadowMap* биндятся на юниты 3..6 (для volumetric —
// 5..8, статично при инициализации программы).
uniform sampler2DShadow uShadowMap0;   // 0 = у ног, 3 = далеко
uniform sampler2DShadow uShadowMap1;
uniform sampler2DShadow uShadowMap2;
uniform sampler2DShadow uShadowMap3;
uniform mat4 uShadowMatrix[4];         // мир -> NDC каскада (с bias 0.5)
uniform float uShadowBias[4];          // смещение глубины против acne
uniform float uShadowBox[4];           // границы смешивания; [3] = лимит теней
uniform float uShadowOn;               // 1 = тени включены

float csm_tap(sampler2DShadow map, float bias, vec3 suv) {
    return texture(map, vec3(suv.xy, suv.z - bias));
}

// Проекция точки мира в каскад c; вне его объёма — тени нет (1.0).
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

// Выбор каскада по дистанции до камеры: смешивание в зоне ~20% перед
// границей (без колец-полос между каскадами), дальше uShadowBox[3] — 1.0.
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

// Стандартный вариант: bias берётся из uShadowBias[каскад].
float csm_select_std(vec3 wp) {
    return csm_select(wp, uShadowBias[0], uShadowBias[1],
                      uShadowBias[2], uShadowBias[3]);
}
