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

// --- Поверхности (splat): грунт / песок / щебень / асфальт --------------------
//  uSplat (юнит 12) — RGBA-маска на весь мир (splat.c): R грунт, G песок,
//  B щебень/тропинка, A асфальт. Базовая земля видна там, где все каналы 0.
//  Слои — обычные тайлящиеся текстуры (юниты 13..16), у каждой свой масштаб
//  (метров на тайл) и своя «шероховатость» для мокрого блика.
uniform sampler2D uSplat;
uniform sampler2D uLayer0;       // грунт
uniform sampler2D uLayer1;       // песок
uniform sampler2D uLayer2;       // щебень / тропинка
uniform sampler2D uLayer3;       // асфальт
uniform vec4 uLayerScale;        // метров на тайл для слоёв 0..3
uniform float uSplatOn;          // 1 = маска запечена (иначе только база)

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
uniform float uWetness;          // мокрость 0..1 (fx.c)
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
uniform float uShadowOn;         // 1 = тени включены (клавиша [])
uniform float uDetailDist;       // LOD деталей: дальше — чистая базовая карта
                                 // (0 = полный сплаттинг везде, как было)

// Статические тени (режим 2): baked-карта на весь мир
uniform sampler2D uStaticShadow;   // R8: 1 = свет, ~0.25 = тень
uniform float uStaticOn;           // 1 = применять статическую карту
uniform vec2 uWorldSize;           // размер мира (для UV)

// Маска мокрости: запечённая R8 текстура, 0 = под нависающим (сухо), 1 = открытое небо
uniform sampler2D uWetnessMask;

// Снежный покров: общий блок snow.glsl (uSnowCover, маска unit 8, шум).
// Террейн: снег лежит на горизонтальных открытых участках. Включение в
// конце uniform-блока (нужны uNoise/uWorldSize), реализация — snow.glsl.
uniform float uSnowCover;
uniform sampler2D uSnowMask;

// Кубомап отражений воды (юнит 9, scene_uniforms_bind): мокрая земля
// становится глянцевой и отражает окружение (небо, горы, деревья).
uniform samplerCube uReflection;

// Шум мокрости (img/noise.png, юнит 11): пятна "здесь мокрее, тут почти сухо"
uniform sampler2D uNoise;

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

#include "snow.glsl"

// Дешёвый хеш — лёгкий разброс цвета земли, чтобы поле не было ровной заливкой
float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

// Тень одного каскада: чистый АППАРАТНЫЙ PCF (GL_COMPARE + GL_LINEAR) —
// один texture() даёт гладкую полутень без полос. Градиент глубины убран:
// на картах 1024² он квантовался в видимые ступени ("полосы" в тенях).
// soft — радиус полутени в текселях карты: < 0.05 = жёсткая тень одного тапа.
float shadow_cascade(sampler2DShadow map, mat4 m, float biasu,
                     vec3 wp, vec3 n, float soft) {
    vec4 sc = m * vec4(wp, 1.0);
    vec3 suv = sc.xyz / sc.w;
    // за пределами бокса каскада (в т.ч. suv.z <= 0 — геометрия ближе света,
    // чем бокс, например рельеф далеко внизу при полёте) — тени нет
    if (suv.z <= 0.0 || suv.z >= 1.0 || suv.x <= 0.0 || suv.x >= 1.0 ||
        suv.y <= 0.0 || suv.y >= 1.0)
        return 1.0;
    // bias против acne ("волосы"/полосы на склонах и пологих землях): масштаб
    // по углу падения света, а не по мировой вертикали — при низком солнце
    // ПЛОСКАЯ земля почти параллельна свету и квантование глубины на тексель
    // каскада даёт ровные полосы-контуры. (1 - N·L) растёт именно там, где
    // поверхностные тексели «ложатся» вдоль луча.
    float nl = clamp(dot(n, normalize(uLightDir[0])), 0.0, 1.0);
    float bias = biasu * (0.6 + 1.8 * (1.0 - nl));
    float s = texture(map, vec3(suv.xy, suv.z - bias));
    if (soft < 0.05) return s;
    // Лёгкая полутень: 4 досэмпла крестом с ФИКСИРОВАННЫМ поворотом.
    // Вращение по хешу пикселя даёт рябь на краю тени при движении камеры,
    // стабильное ядро — нет. Радиус мал (до 2.5 текселя, только вдаль):
    // край остаётся читким, размывается лишь ступенчатость дальних карт.
    vec2 texel = 1.0 / vec2(textureSize(map, 0));
    vec2 t2 = texel * (soft * 0.5);
    vec2 d1 = vec2(0.8370, 0.5472) * t2;
    vec2 d2 = vec2(-0.5472, 0.8370) * t2;
    float acc = s;
    acc += texture(map, vec3(suv.xy + d1, suv.z - bias));
    acc += texture(map, vec3(suv.xy - d1, suv.z - bias));
    acc += texture(map, vec3(suv.xy + d2, suv.z - bias));
    acc += texture(map, vec3(suv.xy - d2, suv.z - bias));
    return acc * 0.2;
}

// Радиус полутени: у ног (до ~10 м) — жёсткая тень (резкий край), дальше
// плавный рост до 2.5 текселей у горизонта — только чтобы сгладить
// ступенчатость дальних карт; край и там остаётся читким.
float shadow_soft(float dist) {
    return clamp((dist - 10.0) * 0.012, 0.0, 2.5);
}

// Тень по дистанции: выбор каскада с ПЛАВНЫМ смешиванием на границах
// (зона ~20% перед границей) — убирает кольца-полосы и дыры между
// каскадами. Дальше SHADOW_LIMIT — тени нет.
float shadow_at(vec3 wp, vec3 n) {
    float dist = distance(uCamPos, wp);
    if (dist >= SHADOW_LIMIT) return 1.0;
    float soft = shadow_soft(dist);
    if (dist < SHADOW_BOX0) {
        float t = smoothstep(SHADOW_BOX0 * 0.8, SHADOW_BOX0, dist);
        return mix(shadow_cascade(uShadowMap0, uShadowMatrix0, uShadowBias0, wp, n, soft),
                   shadow_cascade(uShadowMap1, uShadowMatrix1, uShadowBias1, wp, n, soft), t);
    }
    if (dist < SHADOW_BOX1) {
        float t = smoothstep(SHADOW_BOX1 * 0.8, SHADOW_BOX1, dist);
        return mix(shadow_cascade(uShadowMap1, uShadowMatrix1, uShadowBias1, wp, n, soft),
                   shadow_cascade(uShadowMap2, uShadowMatrix2, uShadowBias2, wp, n, soft), t);
    }
    if (dist < SHADOW_BOX2) {
        float t = smoothstep(SHADOW_BOX2 * 0.8, SHADOW_BOX2, dist);
        return mix(shadow_cascade(uShadowMap2, uShadowMatrix2, uShadowBias2, wp, n, soft),
                   shadow_cascade(uShadowMap3, uShadowMatrix3, uShadowBias3, wp, n, soft), t);
    }
    return shadow_cascade(uShadowMap3, uShadowMatrix3, uShadowBias3, wp, n, soft);
}

void main() {
    // LOD деталей (uDetailDist > 0, режим «КАРТОШКА»): дальше дистанции
    // детали не видны — шум нормалей и сплат-слои фейдятся в ноль, остаётся
    // базовая карта (в неё уже вшита дальняя трава/дороги) + туман. Это
    // срезает 6-8 текстурных выборок на БОЛЬШУЮ часть экрана.
    float det_f = 0.0;
    if (uDetailDist > 0.5) {
        float dcam = distance(uCamPos, vWorldPos);
        det_f = smoothstep(uDetailDist * 0.55, uDetailDist, dcam);
    }
    vec3 base = texture(uBasemap, vUV * uTexScale).rgb;
    vec3 N = normalize(texture(uNormalmap, vUV).rgb * 2.0 - 1.0);
    // Детальные нормали на основе шума: у мелкомасштабного рельефа
    // (почва, камни, тропинки) поверх базовой нормали добавляем возмущение
    // из img/noise.png — без лишней текстуры, живой микрогеометрный рельеф.
    // Две частоты шума: мелкая (~2 м) и чуть крупнее (~8 м). Амплитуда
    // промодулирована ТРЕТЬей, крупной (~48 м) маской: без неё две решётки
    // шума дают интерференцию — регулярную диагональную «рябь» на плоских
    // пространствах (у берега читалась как фальшивые дюны). С маской рябь
    // живёт пятнами, как настоящая microgeometry.
    if (det_f < 0.999) {
        vec2 nd = vWorldPos.xz;
        float mask = texture(uNoise, nd / 48.0 + 1.37).r;
        float amp = 0.25 * (0.25 + 0.75 * mask * mask) * (1.0 - det_f);
        float w1 = texture(uNoise, nd / 2.0 + 7.11).r;
        float w2 = texture(uNoise, nd / 8.0 + 3.77).r;
        vec2 perturb = vec2(w1 * 2.0 - 1.0, w2 * 2.0 - 1.0);
        N = normalize(N + vec3(perturb * amp, 0.0));
    }
    // --- Слои поверхности (splat): грунт/песок/щебень/асфальт ----------------
    // Маска запечена splat.c: дороги, пятна покраски, авто-щебень на обрывах.
    // smooth_layer — насколько поверхность «гладкая»: асфальт блестит после
    // дождя сильнее травы, песок почти не блестит. rough_detail гасит
    // шумовое возмущение нормали на асфальте (он ровный).
    float smooth_layer = 0.0;
    float layer_sum = 0.0;
    if (uSplatOn > 0.5 && det_f < 0.999) {
        vec4 sp = texture(uSplat, vWorldPos.xz / uWorldSize);
        layer_sum = min(sp.r + sp.g + sp.b + sp.a, 1.0) * (1.0 - det_f);
        if (layer_sum > 0.002) {
            vec3 c0 = texture(uLayer0, vWorldPos.xz / max(uLayerScale.x, 0.01)).rgb;
            vec3 c1 = texture(uLayer1, vWorldPos.xz / max(uLayerScale.y, 0.01)).rgb;
            vec3 c2 = texture(uLayer2, vWorldPos.xz / max(uLayerScale.z, 0.01)).rgb;
            vec3 c3 = texture(uLayer3, vWorldPos.xz / max(uLayerScale.w, 0.01)).rgb;
            // Взвешенная сумма слоёв, нормированная своим весом: базовая
            // земля остаётся в остатке (1 - layer_sum).
            vec3 mixed = c0 * sp.r + c1 * sp.g + c2 * sp.b + c3 * sp.a;
            mixed /= max(sp.r + sp.g + sp.b + sp.a, 1e-4);
            base = mix(base, mixed, layer_sum);
            // Асфальт гладкий (0.9), щебень средний, грунт/песок матовые.
            smooth_layer = (sp.r * 0.15 + sp.g * 0.05 + sp.b * 0.35
                          + sp.a * 0.95) / max(sp.r + sp.g + sp.b + sp.a, 1e-4);
            smooth_layer *= layer_sum;
            // На ровном покрытии шумовую детализацию нормали убираем:
            // асфальт не должен «бугриться».
            N = normalize(mix(N, vec3(0.0, 1.0, 0.0), smooth_layer * 0.55));
        }
    }
    // мокрость: затемнение альбедо и более сильный блик. В ясную погоду
    // (uWetness ~ 0, 99% времени) блок пропускается ЦЕЛИКОМ — 3 текстурных
    // выборки (маска + двухчастотный шум) на каждый пиксель рельефа
    // тратить незачем (uniform-ветвление бесплатно).
    float wet = clamp(uWetness, 0.0, 1.0);
    float wet_eff = 0.0;
    if (wet > 0.004) {
        // Маска мокрости: 0 = под крышей (сухо), 1 = открытое небо
        float wet_mask = texture(uWetnessMask, vWorldPos.xz / uWorldSize).r;
        // Вода скатывается: на вертикальных поверхностях эффект слабее
        float slope_factor = clamp(N.y, 0.0, 1.0);
        // Неоднородность мокрости: img/noise.png (REPEAT). Тексель шума
        // ≈ 6 м / 1.25 м — мелкие пятна "мокрее / почти сухо".
        float noise = texture(uNoise, vWorldPos.xz / 1600.0).r * 0.7
                    + texture(uNoise, vWorldPos.xz / 320.0 + 0.6311).r * 0.3;
        noise = noise * noise;                 // контраст: резче мокрее/суше
        wet_eff = wet * wet_mask * slope_factor * mix(0.05, 1.8, noise);
        // Гладкие покрытия держат воду лужами: мокрость на асфальте ровнее и сильнее
        wet_eff = mix(wet_eff, wet * wet_mask * slope_factor, smooth_layer);
        base = mix(base, base * 0.6, wet_eff * 0.7);
    }
    // на месте поля пшеницы земля — золотистая почва (не плоская трава).
    // Под дорогой/покрытием тон поля не проступает (layer_sum гасит).
    vec2 wp = vWorldPos.xz;
    float fx = smoothstep(uField.x - 8.0, uField.x, wp.x)
             * (1.0 - smoothstep(uField.y, uField.y + 8.0, wp.x));
    float fz = smoothstep(uField.z - 8.0, uField.z, wp.y)
             * (1.0 - smoothstep(uField.w, uField.w + 8.0, wp.y));
    base = mix(base, uFieldColor, fx * fz * uFieldBlend * (1.0 - layer_sum));
    base *= 0.94 + 0.12 * hash12(vWorldPos.xz * 0.5);
    // Снежный покров (общий блок snow.glsl): открытые небу пологие участки.
    {
        float a = snow_amount(vWorldPos, smoothstep(0.35, 0.70, N.y));
        if (a > 0.001) base = snow_color(base, vWorldPos, a);
    }
    vec3 V = normalize(uCamPos - vWorldPos);

    // дешёвый fresnel — лёгкое "env-отражение" неба на склонах
    float fres = pow(1.0 - max(dot(N, V), 0.0), 5.0);

    // полусферический ambient: сверху — небо, снизу — отражение от земли
    vec3 amb = mix(uGroundColor, uSkyColor, N.y * 0.5 + 0.5) * uAmbient;

    // Тень от солнца: каскадные карты из камеры на источнике света.
    float csm = 1.0;
    if (uShadowOn > 0.5)
        csm = shadow_at(vWorldPos, N);
    // ЗАТУХАНИЕ К ГОРИЗОНТУ: за ~140 м тень плавно сводится к нулю —
    // на пределе дальнобойности каскадов тень всё равно субтексельная
    // и при низком солнце шумит полосами; далёкие склоны честнее оставить
    // просто освещёнными.
    csm = mix(1.0, csm, 1.0 - smoothstep(140.0, 250.0,
                                         distance(uCamPos, vWorldPos)));
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
            // rim-контур против света и тёплый ореол вокруг солнца.
            // В дождь (wet > 0) — ВЫКЛЮЧЕНЫ: мокрая сцена без контровой
            // обводки (см. request).
            float rim = pow(1.0 - max(dot(N, V), 0.0), 4.0) * (1.0 - wet);
            light += uLightColor[0] * uLightIntensity[0]
                   * (rim * 0.35 + pow(diff, 8.0) * 0.18 * sh * (1.0 - wet));
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

    // мокрость: добавить спекулярный блик на влажных поверхностях
    if (wet > 0.0) {
        // wet_eff уже вычислен выше (с slope_factor и шумом)
        float wet_spec = wet_eff * uSpecIntensity * 3.0;
        for (int i = 0; i < uLightCount; i++) {
            vec3 li = normalize(uLightDir[i]);
            vec3 H = normalize(li + V);
            float s = pow(max(dot(N, H), 0.0), uSpecPower + 30.0);
            col += uLightColor[i] * uLightIntensity[i] * s * wet_spec;
        }
        // Френель-усиление: сильный блик под острым углом
        col += uSkyColor * wet_eff * fres * 0.4;
    }

    col += fres * uFogColor * 0.15;                      // блик "от неба"

    // Мокрая земля — глянец: кубомап отражений растёт с мокротой и
    // гляным углом (Fresnel). Только открытое небо (маска мокрости) и
    // только поверхности, повёрнутые вверх (slope_factor) — вода скатывается.
    if (wet_eff > 0.001) {
        vec3 reflDir = reflect(-V, N);
        vec3 reflCol = texture(uReflection, reflDir).rgb;
        col = mix(col, reflCol, wet_eff * (0.18 + 0.55 * fres));
    }

    // туман по дальности — скрывает край мира. При uFogOn = 0 (r_fog_mode 0)
    // ветка не выполняется: ни smoothstep, ни mix на каждый пиксель.
    if (uFogOn > 0.5) {
        float fog = smoothstep(uFogStart, uFogEnd, distance(uCamPos, vWorldPos));
        col = mix(col, uFogColor, fog);
    }
    FragColor = vec4(col, 1.0);
}
