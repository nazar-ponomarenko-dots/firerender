// ============================================================================
//  Volumetric — полноэкранный проход воксельного объёмного тумана
// ----------------------------------------------------------------------------
//  Деферред: цвет+глубина сцены -> реконструкция мировой точки -> марш по
//  3D-каскадам плотности (как тени, но в 3D), Beer-Lambert накопление.
//  GOD RAYS: солнечное рассеяние в каждой точке затеняется CSM-тенями
//  солнца (резкая PCF-видимость — лучи чёткие), тени контрастны.
//  Цвет реалистичный: тёплый к солнцу, синеватый вбок, ореол у солнца.
//  Проход идёт НА ПОЛОВИННОМ РАЗРЕШЕНИИ (в 4 раза дешевле), потом
//  билинейный апсемпл (volumetric_up.fs).
// ============================================================================

in vec2 vUV;

uniform sampler2D uScene;        // цвет сцены (unit 0)
uniform sampler2D uSceneDepth;   // глубина сцены (unit 1)
uniform mat4 uInvViewProj;
uniform vec3 uCamPos;
uniform float uZNear, uZFar;

uniform sampler3D uFogTex0;      // 3D-каскады плотности (units 2-4)
uniform sampler3D uFogTex1;
uniform sampler3D uFogTex2;
uniform vec3 uFogCenter0, uFogCenter1, uFogCenter2;
uniform float uFogBox0, uFogBox1, uFogBox2;
uniform float uFogRes0, uFogRes1, uFogRes2;

uniform vec3 uSunDir;
uniform float uStrength;
uniform float uFogMode;        // 1 = дешёвый режим: только грубый каскад 2
uniform vec3 uFogColor;      // БАЗОВЫЙ цвет тумана (везде), из map.json
uniform vec3 uFogColorSun;   // тёплый цвет у солнца
uniform float uGlare;        // ореол у самого солнца
uniform float uRayStrength;  // контраст god rays (0 = нет, 2 = резкие)

// CSM-тени солнца (отдельные сэмплеры — GLSL 330)
uniform sampler2DShadow uShadowMap0;
uniform sampler2DShadow uShadowMap1;
uniform sampler2DShadow uShadowMap2;
uniform sampler2DShadow uShadowMap3;
uniform mat4 uShadowMatrix[4];
uniform float uShadowBias[4];
uniform float uShadowOn;

out vec4 FragColor;

#define FOG_MAX 96.0f         // дальше тумана нет (самый дальний каскад до 80 м)
#define C0_HALF 8.0f          // половины боксов каскадов
#define C1_HALF 24.0f
#define C2_HALF 80.0f
#define SHADOW_BOX0 4.0f
#define SHADOW_BOX1 8.0f
#define SHADOW_BOX2 48.0f
#define SHADOW_LIMIT 256.0f

// Плотность в точке мира: каскад по дистанции от камеры
float fog_density(vec3 p) {
    float dist = length(p - uCamPos);
    vec3 uvw;
    if (uFogMode > 0.5) {
        // Режим 2 (дешёвый): ОДИН грубый каскад (2, воксель 5 м) на всё —
        // без ближних каскадов: дешевле по памяти/CPU и в марше.
        uvw = (p - uFogCenter2) / uFogBox2 + 0.5;
    } else if (dist < C0_HALF)
        uvw = (p - uFogCenter0) / uFogBox0 + 0.5;
    else if (dist < C1_HALF)
        uvw = (p - uFogCenter1) / uFogBox1 + 0.5;
    else if (dist < C2_HALF)
        uvw = (p - uFogCenter2) / uFogBox2 + 0.5;
    else
        return 0.0;
    if (uvw.x < 0.0 || uvw.x > 1.0 || uvw.y < 0.0 || uvw.y > 1.0 ||
        uvw.z < 0.0 || uvw.z > 1.0)
        return 0.0;
    if (dist < C0_HALF) return texture(uFogTex0, uvw).r;
    if (dist < C1_HALF) return texture(uFogTex1, uvw).r;
    return texture(uFogTex2, uvw).r;
}

// Резкая видимость солнца (PCF, без градиента) — чёткие god rays
float point_vis(sampler2DShadow map, float bias, vec3 suv) {
    return texture(map, vec3(suv.xy, suv.z - bias));
}

float point_shadow(vec3 wp) {
    float dist = length(wp - uCamPos);
    if (dist >= SHADOW_LIMIT) return 1.0;
    int c;
    if (dist < SHADOW_BOX0) c = 0;
    else if (dist < SHADOW_BOX1) c = 1;
    else if (dist < SHADOW_BOX2) c = 2;
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

// Дешёвый хеш для джиттера шагов (убирает полосы)
float fog_hash(vec2 p) {
    return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453);
}

void main() {
    vec3 scene = texture(uScene, vUV).rgb;
    float depth = texture(uSceneDepth, vUV).r;

    // Мировая точка из глубины (небо — далеко, туман гаснет на FOG_MAX)
    vec4 ndc = vec4(vUV * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 wp4 = uInvViewProj * ndc;
    vec3 wp = wp4.xyz / wp4.w;
    vec3 ray = wp - uCamPos;
    float march = min(length(ray), FOG_MAX);
    vec3 dir = ray / max(length(ray), 1e-5);

    // Адаптивные шаги: больше шагов на длинном луче, минимум 8
    float stepLen = march / clamp(march / 6.0, 8.0, 20.0);
    float jit = fog_hash(vUV * vec2(1920.0, 1080.0));

    float sunHit = max(dot(dir, uSunDir), 0.0);
    vec3 inscat = vec3(0.0);
    float trans = 1.0;
    // Цвет рассеяния: солнечный (тёплый) ВЕЗДЕ — и в противоположном от
    // солнца направлении (лишь чуть разбавлен базовым, 15%).
    vec3 stepCol = mix(uFogColorSun, uFogColor, 0.15 * (1.0 - sunHit));

    for (int i = 0; i < 20; i++) {
        float t = (float(i) + 0.5 + jit * 0.8) * stepLen;
        if (t >= march) break;
        vec3 p = uCamPos + dir * t;
        float d = fog_density(p) * uStrength;
        if (d <= 0.001) continue;
        float od = d * stepLen;                    // оптическая толщина
        float ext = exp(-od);                      // Beer-Lambert
        // тень солнца в точке: где пшеница/деревья закрывают солнце,
        // свет в тумане гаснет — отсюда чёткие лучи (god rays)
        float sunShadow = 1.0;
        if (uShadowOn > 0.5 && sunHit > 0.02)
            sunShadow = point_shadow(p);
        // В стороне от солнца — ЯРКАЯ тёплая дымка (0.55), не зависит от
        // тени. У солнца — контраст: тень тумана тёмная (0.2), просветы
        // яркие (до ~1.3). Лучи видны, солнце не выжигает.
        float litAway = 0.55;
        float litSun  = 0.20 + 0.80 * uRayStrength * sunShadow;
        float lit = mix(litAway, litSun, sunHit);
        // ореол у самого солнца (Mie-подобный пик в тумане)
        float glare = pow(sunHit, 8.0) * uGlare * sunShadow;
        inscat += stepCol * od * trans * (lit + glare);
        trans *= ext;
        if (trans < 0.01) break;                   // дальше не видно — хватит
    }

    vec3 col = scene * trans + inscat;
    FragColor = vec4(col, 1.0);
}
