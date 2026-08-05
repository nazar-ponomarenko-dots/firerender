// ============================================================================
//  Terrain — фрагментный шейдер рельефа: материал земли
// ----------------------------------------------------------------------------
//  Псевдо-PBR в стиле Half-Life 2: base map + normal map + Blinn-Phong
//  спекуляр + дешёвый fresnel + туман по дальности.
//  Базовая текстура — img/ground.png, тайлится по всей карте (uTexScale),
//  фильтруется без сглаживания (NEAREST, пиксельная — как просили).
//  Свет (направление, цвет, сила) приходит от узла Sun, позиция камеры
//  и матрицы — от узла Camera.
// ============================================================================

#define SUN_MAX_LIGHTS 4

in vec2 vUV;
in vec3 vWorldPos;

uniform sampler2D uBasemap;      // текстура земли (img/ground.png)
uniform sampler2D uNormalmap;    // normal map, сгенерирована из высот
uniform vec2 uTexScale;          // повтор текстуры земли на всю карту
uniform vec4 uField;             // прямоугольник поля: x0, x1, z0, z1
uniform vec3 uFieldColor;        // золотистый тон поля пшеницы
uniform float uFieldBlend;       // сила смешивания (из map.json)

uniform vec3 uCamPos;            // позиция узла Camera

uniform vec3 uAmbient;           // базовое освещение (полусфера считается ниже)
uniform vec3 uSkyColor;          // цвет неба (свет сверху)
uniform vec3 uGroundColor;       // цвет земли (отражённый свет снизу)
uniform vec3 uFogColor;          // цвет тумана/неба
uniform float uFogStart;
uniform float uFogEnd;
uniform float uFogOn;           // 1 = туман включён (cvar r_fog_mode)
uniform float uSpecPower;        // "металличность" блика
uniform float uSpecIntensity;    // сила блика
uniform sampler2DShadow uShadowMap0;   // каскады теней: 0 = у ног, 1-2 = средне, 3 = далеко
uniform mat4 uShadowMatrix0;
uniform float uShadowBias0;
uniform sampler2D uShadowDepth0;       // те же карты, но сырая глубина (для градиента)
uniform float uShadowGrad0;            // глубина до полной темноты (NDC)
uniform sampler2DShadow uShadowMap1;
uniform mat4 uShadowMatrix1;
uniform float uShadowBias1;
uniform sampler2D uShadowDepth1;
uniform float uShadowGrad1;
uniform sampler2DShadow uShadowMap2;
uniform mat4 uShadowMatrix2;
uniform float uShadowBias2;
uniform sampler2D uShadowDepth2;
uniform float uShadowGrad2;
uniform sampler2DShadow uShadowMap3;
uniform mat4 uShadowMatrix3;
uniform float uShadowBias3;
uniform sampler2D uShadowDepth3;
uniform float uShadowGrad3;
uniform float uShadowOn;         // 1 = тени включены (клавиша [)

// Статические тени (режим 2): baked-карта на весь мир
uniform sampler2D uStaticShadow;   // R8: 1 = свет, ~0.25 = тень
uniform float uStaticOn;           // 1 = применять статическую карту
uniform vec2 uWorldSize;           // размер мира (для UV)

// --- Источники света: все направленные из map.json ---------------------------
// uLightDir[0] — солнце (единственный с каскадными тенями), дальше — фоновые
// света.
uniform vec3 uLightDir[SUN_MAX_LIGHTS];
uniform vec3 uLightColor[SUN_MAX_LIGHTS];
uniform float uLightIntensity[SUN_MAX_LIGHTS];
uniform int uLightCount;          // реальное число источников (1..4)
#define SCENE_MAX_POINT 8
#define SCENE_MAX_SPOT  4
// Точечные источники (map.json lights[]: point) — локальный свет с
// квадратичным затуханием по дистанции, в каскадную тень не гаснет.
uniform vec3 uPointPos[SCENE_MAX_POINT];
uniform vec3 uPointColor[SCENE_MAX_POINT];
uniform float uPointIntensity[SCENE_MAX_POINT];
uniform float uPointRange[SCENE_MAX_POINT];
uniform int uPointCount;
// Прожекторы (spot): как точечные, но в конусе направления (см. карта)
uniform vec3 uSpotPos[SCENE_MAX_SPOT];
uniform vec3 uSpotDir[SCENE_MAX_SPOT];
uniform vec3 uSpotColor[SCENE_MAX_SPOT];
uniform float uSpotIntensity[SCENE_MAX_SPOT];
uniform float uSpotRange[SCENE_MAX_SPOT];
uniform float uSpotAngle[SCENE_MAX_SPOT];
uniform int uSpotCount;

// границы каскадов — ПОЛОВИНЫ боксов: фрагмент на дистанции d гарантированно
// в боксе каскада. На границах каскады СМЕШИВАЮТСЯ (зона ~20% от границы) —
// иначе bias/мягкость прыгают и появляются кольца-полосы и дыры.
#define SHADOW_BOX0 4.0f
#define SHADOW_BOX1 8.0f
#define SHADOW_BOX2 48.0f
#define SHADOW_LIMIT 256.0f     // дальше — теней нет вообще

out vec4 FragColor;

// Дешёвый хеш — лёгкий разброс цвета земли, чтобы поле не было ровной заливкой
float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

// Тень одного каскада: чистый АППАРАТНЫЙ PCF (GL_COMPARE + GL_LINEAR) —
// один texture() даёт гладкую полутень без полос. Градиент глубины убран:
// на картах 1024² он квантовался в видимые ступени ("полосы" в тенях).
float shadow_cascade(sampler2DShadow map, mat4 m, float biasu,
                     vec3 wp, vec3 n) {
    vec4 sc = m * vec4(wp, 1.0);
    vec3 suv = sc.xyz / sc.w;
    // за пределами бокса каскада (в т.ч. suv.z <= 0 — геометрия ближе света,
    // чем бокс, например рельеф далеко внизу при полёте) — тени нет
    if (suv.z <= 0.0 || suv.z >= 1.0 || suv.x <= 0.0 || suv.x >= 1.0 ||
        suv.y <= 0.0 || suv.y >= 1.0)
        return 1.0;
    // bias против acne ("волосы" на склонах): ~4 латеральных текселя,
    // растёт с уклоном рельефа (наклонная земля сама себя затеняет —
    // смещение по нормали должно быть заметно больше глубины квантования).
    float bias = biasu * (1.5 - n.y) + biasu * 0.25;
    return texture(map, vec3(suv.xy, suv.z - bias));
}

// Тень по дистанции: выбор каскада с ПЛАВНЫМ смешиванием на границах
// (зона ~20% перед границей) — убирает кольца-полосы и дыры между
// каскадами. Дальше SHADOW_LIMIT — тени нет.
float shadow_at(vec3 wp, vec3 n) {
    float dist = distance(uCamPos, wp);
    if (dist >= SHADOW_LIMIT) return 1.0;
    if (dist < SHADOW_BOX0) {
        float t = smoothstep(SHADOW_BOX0 * 0.8, SHADOW_BOX0, dist);
        return mix(shadow_cascade(uShadowMap0, uShadowMatrix0, uShadowBias0, wp, n),
                   shadow_cascade(uShadowMap1, uShadowMatrix1, uShadowBias1, wp, n), t);
    }
    if (dist < SHADOW_BOX1) {
        float t = smoothstep(SHADOW_BOX1 * 0.8, SHADOW_BOX1, dist);
        return mix(shadow_cascade(uShadowMap1, uShadowMatrix1, uShadowBias1, wp, n),
                   shadow_cascade(uShadowMap2, uShadowMatrix2, uShadowBias2, wp, n), t);
    }
    if (dist < SHADOW_BOX2) {
        float t = smoothstep(SHADOW_BOX2 * 0.8, SHADOW_BOX2, dist);
        return mix(shadow_cascade(uShadowMap2, uShadowMatrix2, uShadowBias2, wp, n),
                   shadow_cascade(uShadowMap3, uShadowMatrix3, uShadowBias3, wp, n), t);
    }
    return shadow_cascade(uShadowMap3, uShadowMatrix3, uShadowBias3, wp, n);
}

void main() {
    vec3 base = texture(uBasemap, vUV * uTexScale).rgb;
    // на месте поля пшеницы земля — золотистая почва (не плоская трава)
    vec2 wp = vWorldPos.xz;
    float fx = smoothstep(uField.x - 8.0, uField.x, wp.x)
             * (1.0 - smoothstep(uField.y, uField.y + 8.0, wp.x));
    float fz = smoothstep(uField.z - 8.0, uField.z, wp.y)
             * (1.0 - smoothstep(uField.w, uField.w + 8.0, wp.y));
    base = mix(base, uFieldColor, fx * fz * uFieldBlend);
    base *= 0.94 + 0.12 * hash12(vWorldPos.xz * 0.5);
    vec3 N = normalize(texture(uNormalmap, vUV).rgb * 2.0 - 1.0);
    vec3 V = normalize(uCamPos - vWorldPos);

    // дешёвый fresnel — лёгкое "env-отражение" неба на склонах
    float fres = pow(1.0 - max(dot(N, V), 0.0), 5.0);

    // полусферический ambient: сверху — небо, снизу — отражение от земли
    vec3 amb = mix(uGroundColor, uSkyColor, N.y * 0.5 + 0.5) * uAmbient;

    // Тень от солнца: каскадные карты из камеры на источнике света.
    // Каскад выбирается ЖЁСТКО по расстоянию (без смешивания): сетки
    // текселей каскадов выровнены по миру, PCF одинаковый — края теней
    // совпадают, а смешивание при ходьбе давало бы мерцание.
    float csm = 1.0;
    if (uShadowOn > 0.5)
        csm = shadow_at(vWorldPos, N);
    // Режим 2 (статика): карта теней на весь мир, а ДИНАМИКА (CSM) — только
    // вблизи игрока (2..3 м); дальше — статика. В режиме 1 (CSM) статика
    // выключена (uStaticOn = 0) — тени чистые каскадные на всю дистанцию.
    float shadow = csm;
    if (uStaticOn > 0.5) {
        float st = texture(uStaticShadow, vWorldPos.xz / uWorldSize).r;
        shadow = mix(csm, st, smoothstep(2.0, 3.0, distance(uCamPos, vWorldPos)));
    }

    // Прямой свет: КАЖДЫЙ направленный источник из map.json освещает
    // землю ОТДЕЛЬНО (солнце = индекс 0, дальше — фоновые света).
    // Максимальная темнота глубже: в тени полусферный свет гаснет до 1/4.
    vec3 light = amb * mix(0.25, 1.0, shadow);
    for (int i = 0; i < uLightCount; i++) {
        vec3 li = normalize(uLightDir[i]);
        float diff = max(dot(N, li), 0.0);
        float sh = (i == 0) ? shadow : 1.0;      // CSM-тени только у солнца
        // диффуз почти на 0.5: земля матовая, без резкой половинчатости
        light += uLightColor[i] * uLightIntensity[i]
               * (0.55 + 0.45 * diff) * sh;
        if (i == 0) {
            // rim-контур против света и тёплый ореол вокруг солнца
            float rim = pow(1.0 - max(dot(N, V), 0.0), 4.0);
            light += uLightColor[0] * uLightIntensity[0]
                   * (rim * 0.35 + pow(diff, 8.0) * 0.18 * sh);
        }
    }
    // Точечные источники: затухание по дистанции, локальный свет
    for (int i = 0; i < uPointCount; i++) {
        vec3 Lp = uPointPos[i] - vWorldPos;
        float dpl = length(Lp);
        vec3 lip = Lp / max(dpl, 0.001);
        float att = max(1.0 - dpl / uPointRange[i], 0.0);
        att *= att;
        float diff = max(dot(N, lip), 0.0);
        light += uPointColor[i] * uPointIntensity[i] * (0.5 + 0.5 * diff) * att;
    }
    // Прожекторы: точечный + конус с смягчённым краем
    for (int i = 0; i < uSpotCount; i++) {
        vec3 Lp = uSpotPos[i] - vWorldPos;
        float dpl = length(Lp);
        vec3 lip = Lp / max(dpl, 0.001);
        float att = max(1.0 - dpl / uSpotRange[i], 0.0);
        att *= att;
        float cone = max(dot(-lip, normalize(uSpotDir[i])), 0.0);
        float edge = smoothstep(uSpotAngle[i] - 0.08, uSpotAngle[i], cone);
        float diff = max(dot(N, lip), 0.0);
        light += uSpotColor[i] * uSpotIntensity[i] * (0.5 + 0.5 * diff) * att * edge;
    }
    light = min(light, vec3(1.0));
    vec3 col = base * light;

    col += fres * uFogColor * 0.15;                      // блик "от неба"

    // туман по дальности — скрывает край мира. При uFogOn = 0 (r_fog_mode 0)
    // ветка не выполняется: ни smoothstep, ни mix на каждый пиксель.
    if (uFogOn > 0.5) {
        float fog = smoothstep(uFogStart, uFogEnd, distance(uCamPos, vWorldPos));
        col = mix(col, uFogColor, fog);
    }
    FragColor = vec4(col, 1.0);
}
