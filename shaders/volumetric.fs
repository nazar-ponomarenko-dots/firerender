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
uniform float uWetness;      // мокрость 0..1 — в дождь туман от неба, не от солнца
uniform vec3 uSkyColor;      // цвет неба (основной свет тумана)

// Дымка над землёй (дождь): серая, ~0.5 м, с шумом. Карта высот (unit 10)
// даёт точную высоту рельефа в каждой точке марша — слой лежит на склоне.
// Шум дымки — img/noise.png (unit 12): живая неравномерность.
uniform sampler2D uHeightmap;
uniform sampler2D uNoise;
uniform vec2 uWorldSize;
uniform float uHeightMin, uHeightMax;
uniform float uTime;
uniform float uHaze;         // сила дымки (0 = нет), растёт с мокротой
uniform float uHazeH;        // толщина слоя, м
uniform vec3 uWColor;        // тон тумана над водоёмами (средний цвет воды)

// CSM-тени солнца (отдельные сэмплеры — GLSL 330)
uniform sampler2DShadow uShadowMap0;
uniform sampler2DShadow uShadowMap1;
uniform sampler2DShadow uShadowMap2;
uniform sampler2DShadow uShadowMap3;
uniform mat4 uShadowMatrix[4];
uniform float uShadowBias[4];
uniform float uShadowOn;

out vec4 FragColor;
uniform float uFogOnly;          // 1 = вывести только туман (цвет+trans), без сцены

#define FOG_MAX 96.0f         // дальше тумана нет (самый дальний каскад до 80 м)
#define C0_HALF 8.0f          // половины боксов каскадов
#define C1_HALF 24.0f
#define C2_HALF 80.0f
#define SHADOW_BOX0 4.0f
#define SHADOW_BOX1 8.0f
#define SHADOW_BOX2 48.0f
#define SHADOW_LIMIT 256.0f

// Плотность в точке мира: каскад по дистанции от камеры.
// Возвращает vec2: x = базовая плотность (R), y = туман над водой (G).
vec2 fog_density(vec3 p) {
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
        return vec2(0.0);
    if (uvw.x < 0.0 || uvw.x > 1.0 || uvw.y < 0.0 || uvw.y > 1.0 ||
        uvw.z < 0.0 || uvw.z > 1.0)
        return vec2(0.0);
    if (dist < C0_HALF) return texture(uFogTex0, uvw).rg;
    if (dist < C1_HALF) return texture(uFogTex1, uvw).rg;
    return texture(uFogTex2, uvw).rg;
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

// Высота рельефа из карты высот (в метрах) — та же формула, что в terrain.vs
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

// Дымка над землёй (дождь): тонкий серый слой ~uHazeH на рельефе, с шумом.
// Плотность падает с высотой; шум — img/noise.png, КРУПНЫЕ пятна (~30 м)
// видны издалека + дрейф по времени. Слабая — чтобы не выжигать фон белым.
float haze_density(vec3 p) {
    if (uHaze <= 0.001) return 0.0;
    float ground = terrain_height(p);
    float up = p.y - ground;
    if (up < 0.0) return 0.0;            // под рельефом — нет
    float h = 1.0 - smoothstep(0.0, uHazeH, up);
    if (h <= 0.001) return 0.0;
    // три масштаба шума + дрейф; клубы мелкие (тексель шума ~2.5..0.12 м)
    vec2 nm = p.xz + vec2(uTime * 1.2, uTime * 0.8);
    float n = texture(uNoise, nm / 600.0).r * 0.55
            + texture(uNoise, nm / 150.0 + 13.7).r * 0.30
            + texture(uNoise, nm / 30.0  -  7.3).r * 0.15;
    return uHaze * h * (0.10 + 0.90 * n);
}

// Обычный (классический) туман вокруг игрока в дождь: равномерная дымка,
// нарастает с дистанцией экспоненциально (быстро вблизи, стабильно вдали).
// Цвет — серый (базовый). Заметный, но не белый.
float rain_fog_density(float dist) {
    return uWetness * 0.03 * (1.0 - exp(-dist / 30.0));
}

// Дождевой «класс тумана»: базовые каскады (density_ground/field из map.json)
// гаснут с мокротой — в дождь вместо плотной дымки идёт лёгкий туман от
// неба (rain_fog_density). Множитель 0.10 при полном дожде: в ~10 раз реже.
float base_fog_scale() {
    return 1.0 - 0.90 * uWetness;
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
    // Джиттер шагов АНИМИРОВАННЫЙ (фаза меняется каждый кадр): статический
    // узор читался как зерно на полупрозрачном тумане; временной шум глаз
    // усредняет за пару кадров — туман гладкий, полос от квантования нет.
    float jit = fog_hash(vUV * vec2(1920.0, 1080.0) + fract(uTime * 0.7311) * 61.7);

    float sunHit = max(dot(dir, uSunDir), 0.0);
    vec3 inscat = vec3(0.0);
    float trans = 1.0;
    // Цвет рассеяния: солнечный (тёплый) ВЕЗДЕ — и в противоположном от
    // солнца направлении (лишь чуть разбавлен базовым, 15%). В ДОЖДЬ
    // (uWetness -> 1) солнца за облаками нет: туман РОВНЫЙ СЕРЫЙ (базовый
    // цвет, БЕЗ примеси яркого неба) — вдали не выгорает в белое.
    vec3 stepCol = mix(uFogColorSun, uFogColor, 0.15 * (1.0 - sunHit));
    stepCol = mix(stepCol, uFogColor, uWetness);

    for (int i = 0; i < 20; i++) {
        float t = (float(i) + 0.5 + jit * 0.8) * stepLen;
        if (t >= march) break;
        vec3 p = uCamPos + dir * t;
        vec2 fd = fog_density(p);
        // базовый туман гаснет в дождь, водный — нет (испарение над водой
        // сильнее именно в сырости)
        float db = (fd.x * base_fog_scale()
                  + haze_density(p) + rain_fog_density(t)) * uStrength;
        float dw = fd.y * uStrength;               // туман над водоёмом
        float d = db + dw;
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
        // В ДОЖДЬ солнце скрыто облаками: контраст и ореол гаснут с
        // мокротой — туман светится равномерно от неба.
        float rainDim = 1.0 - uWetness;
        float raystr = uRayStrength * rainDim;
        float litAway = 0.55;
        float litSun  = 0.20 + 0.80 * raystr * sunShadow;
        float lit = mix(litAway, litSun, sunHit * rainDim);
        lit = max(lit, mix(0.0, 0.32, uWetness));   // в дождь — серый свет неба
        // ореол у самого солнца (Mie-подобный пик в тумане)
        float glare = pow(sunHit, 8.0) * uGlare * sunShadow * rainDim;
        // Цвет шага: базовый тёплый туман, а над водой подмешан тон
        // водоёма (uWColor, чуть светлее) — взвешено по плотностям.
        vec3 stepCol2 = mix(stepCol, uWColor * 1.15, dw / max(d, 1e-4));
        // Прирост рассеянного света: правильно Beer-Lambert (1 - ext), НЕ od.
        // od в плотном узком слое (низкая дымка) огромно и переоценивало бы
        // свет -> белое свечение. (1 - ext) упирается в 1, белого нет.
        inscat += stepCol2 * (1.0 - ext) * trans * (lit + glare * (1.0 - trans));
        trans *= ext;
        if (trans < 0.01) break;                   // дальше не видно — хватит
    }

    if (uFogOnly > 0.5) {
        // Полупроходной режим: марш в текстуру полразмера, композит на
        // экране отдельно (fog_comp.fs) — марш в 4 раза дешевле
        FragColor = vec4(inscat, trans);
        return;
    }
    vec3 col = scene * trans + inscat;
    FragColor = vec4(col, 1.0);
}
