// ============================================================================
//  Object — фрагментный шейдер биллборда объекта (дерево и пр.)
// ----------------------------------------------------------------------------
//  Материал ПОЧТИ ОДИН В ОДИН как у пшеницы (wheat.fs): альфа-отсечение,
//  полусферический ambient, ровный диффуз (нормаль биллборда бессмысленна),
//  все направленные источники из map.json, CSM-тени солнца с градиентом
//  глубины, tint, ограничение света сверху, туман.
// ============================================================================

in vec2 vUV;
in vec3 vWorldPos;

uniform sampler2D uTex;
uniform vec3 uCamPos;
uniform vec3 uTint;              // тон объекта (по умолчанию 1)

// Режим отображения (render.blend/alpha):
//   0 = альфа-тест (прозрачный фон отбрасывается),
//   1 = обычный блендинг, 2 = аддитивный, 3 = непрозрачный.
uniform int uBlend;
uniform float uAlpha;

// Свет: все направленные из map.json (индекс 0 — солнце, у него тени)
#define SUN_MAX_LIGHTS 4
uniform vec3 uLightDir[SUN_MAX_LIGHTS];
uniform vec3 uLightColor[SUN_MAX_LIGHTS];
uniform float uLightIntensity[SUN_MAX_LIGHTS];
uniform int uLightCount;
uniform vec3 uAmbient;
uniform vec3 uSkyColor;
uniform vec3 uGroundColor;
uniform vec3 uFogColor;
uniform float uFogStart;
uniform float uFogEnd;
uniform float uFogOn;           // 1 = туман включён (cvar r_fog_mode)

uniform sampler2DShadow uShadowMap0;  // каскады теней — ОТДЕЛЬНЫЕ сэмплеры:
uniform sampler2DShadow uShadowMap1;  // индексация массивов сэмплеров
uniform sampler2DShadow uShadowMap2;  // запрещена в GLSL 330 (Mesa)
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

// Одна каскадная тень: PCF + градиент глубины (как в wheat.fs)
// Чистый PCF: градиент глубины убран (на 1024² давал полосы)
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

#define SHADOW_BOX0 4.0f
#define SHADOW_BOX1 8.0f
#define SHADOW_BOX2 48.0f
#define SHADOW_LIMIT 256.0f

float object_shadow(vec3 wp) {
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
    // Альфа-тест и непрозрачный: прозрачный фон отбрасываем;
    // alpha/additive — полупрозрачность через uAlpha.
    if (uBlend == 0 || uBlend == 3) {
        if (c.a < 0.5) discard;
    }
    vec3 N = normalize(uCamPos - vWorldPos);   // биллборд смотрит на камеру
    vec3 V = N;

    // полусферический ambient: сверху — небо, снизу — отражение от земли
    vec3 amb = mix(uGroundColor, uSkyColor, N.y * 0.5 + 0.5) * uAmbient;
    float shadow = object_shadow(vWorldPos);

    // Диффуз почти РОВНЫЙ, как у пшеницы: у биллборда нормаль бессмысленна.
    // Светит КАЖДЫЙ направленный источник; CSM-тень — только солнце (0).
    // В тени полусферный свет гаснет до 1/4 (максимальная темнота глубже).
    vec3 light = amb * mix(0.25, 1.0, shadow);
    vec3 L = vec3(0.0);
    for (int i = 0; i < uLightCount; i++) {
        vec3 li = normalize(uLightDir[i]);
        float diff = 0.55 + 0.25 * max(dot(N, li), 0.0);
        float sh = (i == 0) ? shadow : 1.0;
        light += uLightColor[i] * uLightIntensity[i] * diff * sh;
        if (i == 0) L = li;
    }
    light = min(light, vec3(1.0));       // не выгорает в белый, как у пшеницы
    vec3 col = c.rgb * uTint * light;

    // СВЕТЯЩАЯСЯ ОБВОДКА при КОНТРОВОМ СВЕТЕ (как у пшеницы): смотрим
    // на дерево с теневой стороны, солнце за ним — по силуэту светится
    // обводка. Два слоя: резкое кольцо (~1.5 текселя) + мягкий ореол
    // (~3 текселя). Действует до 180 м (деревья стоят далеко от спавна).
    vec3 lit = col;
    float dist = distance(uCamPos, vWorldPos);
    if (uLightCount > 0 && length(L) > 0.5) {
        float near_fade = 1.0 - smoothstep(40.0, 180.0, dist);
        float backlit = (1.0 - smoothstep(-0.1, 0.55, dot(L, V))) * near_fade;
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
            // В 2+ раза сильнее: ярче кольцо и ореол
            float glow = smoothstep(0.2, 0.7, o1) * 0.9
                       + smoothstep(0.1, 0.45, o2) * 0.55;
            col += lit * backlit * glow;
        }
    }

    // туман по дальности. При uFogOn = 0 (r_fog_mode 0) — без затрат.
    if (uFogOn > 0.5) {
        float fog = smoothstep(uFogStart, uFogEnd, dist);
        col = mix(col, uFogColor, fog);
    }
    float a = (uBlend == 0 || uBlend == 3) ? 1.0 : c.a * uAlpha;
    FragColor = vec4(col, a);
}
