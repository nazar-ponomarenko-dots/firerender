// ============================================================================
//  Vegetation — фрагментный шейдер растительности: материал травы и пшеницы
// ----------------------------------------------------------------------------
//  Слой из 2D-массива (0 = трава, 1 = пшеница) выбирает спрайт; жёсткий
//  альфа-тест. Освещение:
//    - полусферический ambient (сверху небо, снизу отражение травы/почвы);
//    - мягкий диффуз с wrap — без резкой границы на теневой стороне;
//    - КАСКАДНЫЕ ТЕНИ (CSM): растения принимают тени от рельефа и друг
//      друга (PCF ~1.5 текселя, bias 2 текселя — тонкие квады);
//    - блик на колосьях пшеницы (у травы поверхность матовая);
//    - градиент по высоте: основание затемнено оклюзией соседей,
//      верхушка ловит больше света;
//    - КОНТРОВОЙ СВЕТ (специальный режим материала): когда солнце за
//      растением, тело НЕ просвечивает — оно затемняется в силуэт,
//      а по краям силуэта вспыхивает тёплая светящаяся обводка.
//  Туман по дальности — как у земли.
// ============================================================================

in vec2 vUV;
in vec2 vUVBase;    // UV [0,1] без выреза — из wheat.vs
in vec3 vWorldPos;
in vec3 vNormal;
in float vTint;
flat in float vLayer;   // из wheat.vs — LOD целый для квада
in float vHeight;      // высота растения в метрах (из wheat.vs)

uniform sampler2DArray uVeg;      // массив спрайтов: 0=трава, 1-3=LOD пшеницы
uniform vec3 uCamPos;             // позиция узла Camera
// ВСЕ направленные источники из map.json: индекс 0 — солнце (с тенями),
// дальше — фоновые света. Растения светятся ото всех по чуть-чуть.
#define SUN_MAX_LIGHTS 4
uniform vec3 uLightDir[SUN_MAX_LIGHTS];
uniform vec3 uLightColor[SUN_MAX_LIGHTS];
uniform float uLightIntensity[SUN_MAX_LIGHTS];
uniform int uLightCount;
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
uniform vec3 uAmbient;            // фоновый свет (полусфера считается ниже)
uniform vec3 uSkyColor;           // цвет неба (свет сверху)
uniform vec3 uGroundColor;        // цвет травы/почвы (отражённый снизу)
uniform vec3 uFogColor;           // туман (как у земли)
uniform float uFogStart;
uniform float uFogEnd;
uniform float uFogOn;            // 1 = туман включён (cvar r_fog_mode)

uniform sampler2DShadow uShadowMap0;  // каскады теней — ОТДЕЛЬНЫЕ сэмплеры:
uniform sampler2DShadow uShadowMap1;  // динамическая индексация массивов
uniform sampler2DShadow uShadowMap2;  // сэмплеров запрещена в GLSL 330
uniform sampler2DShadow uShadowMap3;  // (Mesa не компилирует, NVIDIA прощал)
uniform mat4 uShadowMatrix[4];        // матрицы/числа — массивы (можно)
uniform float uShadowBias[4];
uniform sampler2D uShadowDepth0;
uniform sampler2D uShadowDepth1;
uniform sampler2D uShadowDepth2;
uniform sampler2D uShadowDepth3;
uniform float uShadowGrad[4];         // глубина до полной темноты (NDC)
uniform float uShadowOn;          // 1 = тени включены (клавиша [)
uniform float uWetness;        // мокрость 0..1 (fx.c)

// Растения светятся прямым светом и CSM-тенями солнца; никакого AO —
// поле остаётся золотым, тени от растений на землю кладут каскады.

out vec4 FragColor;

// Точные куски холста 256x178 для слоёв: трава 128x128, пшеница
// lod0 128x178, lod1 152x178, lod2 256x178 (все слева сверху).
vec2 grass_uv(vec2 u) { return u * vec2(128.0 / 256.0, 128.0 / 178.0); }
vec2 lod0_uv(vec2 u)  { return u * vec2(128.0 / 256.0, 1.0); }
vec2 lod1_uv(vec2 u)  { return u * vec2(152.0 / 256.0, 1.0); }
vec2 lod2_uv(vec2 u)  { return u; }

// Спрайт с плавным LOD: vLayer непрерывен (1..2..3, flat — один LOD на
// весь квад). В короткой полосе перехода (0.8 м) соседние слои плавно
// смешиваются по цвету И альфе — переход виден, но полоса призраков
// узкая, края остаются чёткими. Вне полос — один тексель.
vec4 veg_sample(vec2 base, float lf, out vec2 uv_out) {
    if (lf < 0.5) { uv_out = grass_uv(base); return texture(uVeg, vec3(uv_out, 0.0)); }
    float li = floor(lf);
    float w = lf - li;
    vec2 ua = li < 1.5 ? lod0_uv(base) : (li < 2.5 ? lod1_uv(base) : lod2_uv(base));
    vec4 a = texture(uVeg, vec3(ua, li < 1.5 ? 1.0 : (li < 2.5 ? 2.0 : 3.0)));
    uv_out = ua;
    if (w > 0.001) {
        vec2 ub = li < 1.5 ? lod1_uv(base) : (li < 2.5 ? lod2_uv(base) : lod2_uv(base));
        vec4 b = texture(uVeg, vec3(ub, li < 2.5 ? 2.0 : 3.0));
        uv_out = mix(ua, ub, w);
        return mix(a, b, w);
    }
    return a;
}

// границы каскадов — ПОЛОВИНЫ боксов (см. terrain.fs). На границах
// каскады смешиваются, дальше SHADOW_LIMIT теней нет.
#define SHADOW_BOX0 4.0f
#define SHADOW_BOX1 8.0f
#define SHADOW_BOX2 48.0f
#define SHADOW_LIMIT 256.0f

// Тень одного каскада = ОБЪЕДИНЕНИЕ: аппаратный PCF (vis — плотное поле
// под кроной тёмное, ближайший стебель в ~5 см) + ГРАДИЕНТ ГЛУБИНЫ вдоль
// 3D-луча света (deep: у растения d≈0, у конца тени d≈3.6 м). Тёмно,
// если хоть одна мера тёмная: 1 − (1−vis)·(1−deep). Поле — ровное тёмное,
// открытые тени — с видимым градиентом от светлого края к основанию.
// Одна каскадная тень: чистый аппаратный PCF. Градиент глубины убран —
// на картах 1024² он квантовался в видимые полосы.
float cascade_tap(sampler2DShadow map, float bias, vec3 suv) {
    return texture(map, vec3(suv.xy, suv.z - bias));
}

float sample_cascade(vec3 wp, int c) {
    vec4 sc = uShadowMatrix[c] * vec4(wp, 1.0);
    vec3 suv = sc.xyz / sc.w;
    // за пределами бокса каскада — тени нет
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

float veg_shadow(vec3 wp) {
    if (uShadowOn < 0.5) return 1.0;
    float dist = distance(uCamPos, wp);
    if (dist >= SHADOW_LIMIT) return 1.0;
    // выбор каскада со смешиванием на границах (как в terrain.fs)
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
    vec2 suv;
    vec4 c = veg_sample(vUVBase, vLayer, suv);
    if (c.a < 0.5) discard;              // прозрачный фон спрайта
    vec3 N = normalize(vNormal);
    vec3 V = normalize(uCamPos - vWorldPos);
    float wet = clamp(uWetness, 0.0, 1.0);
    c.rgb = mix(c.rgb, c.rgb * 0.7, wet * 0.5);

    // полусферический ambient: сверху — небо, снизу — отражение от земли
    vec3 amb = mix(uGroundColor, uSkyColor, N.y * 0.5 + 0.5) * uAmbient;

    // Диффуз почти РОВНЫЙ: у биллборда случайный yaw, и освещение по
    // нормали квада режет "половину" травы в 2 раза темнее (N·L=0 у
    // спрайтов, отвернутых от солнца). Для плоского спрайта нормаль
    // бессмысленна — оставляем лишь лёгкое разнообразие от ориентации.
    // База приглушена, чтобы растения не светились.

    // блик на колосьях пшеницы (слой 1); трава — матовая
    float layer = step(0.5, vLayer);

//    // тень от рельефа и соседних растений
    float shadow = veg_shadow(vWorldPos);

    // AO нет: поле пшеницы остаётся золотым, тени на землю кладут каскады.

    // Статического вертикального градиента НЕТ: он резал каждое растение
    // пополам (низ тёмный / верх светлый). Тон задаёт только tint.
    float grad = 1.0;

    // дистанция до камеры — нужна и контрсвету (ближние метры), и туману
    float d = distance(uCamPos, vWorldPos);

    // Всё освещение копится в light и ОГРАНИЧЕНО сверху: даже от
    // 100000% света растение НЕ выгорает в белый — ярче полного
    // альбедо не бывает, цвет колоса всегда виден.
    // Светит КАЖДЫЙ направленный источник из map.json; CSM-тень
    // принимает только солнце (индекс 0). Максимальная темнота глубже:
    // в тени полусферный свет гаснет до 1/4.
    vec3 light = amb * mix(0.25, 1.0, shadow);
    vec3 L = vec3(0.0);
    for (int i = 0; i < uLightCount; i++) {
        vec3 li = normalize(uLightDir[i]);
        float diff = 0.55 + 0.25 * max(dot(N, li), 0.0);
        float sh = (i == 0) ? shadow : 1.0;
        light += uLightColor[i] * uLightIntensity[i] * diff * sh;
        if (i == 0) L = li;
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
    vec3 col = c.rgb * vTint * grad * light;
    if (uLightCount > 0 && length(L) > 0.5) {
        vec3 H = normalize(L + V);
        float spec = pow(max(dot(N, H), 0.0), 48.0) * layer;
        col += uLightColor[0] * spec * 0.5 * shadow;

        // мокрость: лёгкий влажный блеск на колосях (слой 1). Слабый —
        // чтобы в дождь колос не "выгорал" в белый; приглушён с высотой.
        if (wet > 0.0) {
            float wet_spec = wet * layer * 0.18;
            col += uLightColor[0] * wet_spec * shadow;
        }
    }

    // ПОДПОВЕРХНОСТНОЕ РАССЕИВАНИЕ — всем растениям по умолчанию: когда
    // свет за растением (смотрим против солнца), часть света проходит
    // сквозь стебель/колос и светит изнутри — тёплое свечение как у
    // настоящей листвы. Верхушки тоньше — просвечивают сильнее. Тень
    // уважается: растение в тени не светится. В ДОЖДЬ — выключено:
    // мокрая листва не просвечивает (нет контрового солнца).
    if (uLightCount > 0 && length(L) > 0.5) {
        float trans = max(dot(-L, V), 0.0);          // 1 = свет строго за растением
        float tip = smoothstep(0.25, 1.0, suv.y);    // верхушка тоньше — ярче
        float sss = trans * trans * (0.25 + 0.75 * tip) * shadow * (1.0 - wet);
        col += c.rgb * vTint * uLightColor[0] * uLightIntensity[0] * sss * 0.7;
    }

    // КОНТРОВОЙ СВЕТ: солнце ЗА растением (смотрим с теневой стороны,
    // L·V < 0). Низ затемняется тем СИЛЬНЕЕ, чем ВЫШЕ растение (высокая
    // пшеница затеняет собственное основание, низкая трава — почти нет),
    // к верху градиентом сходит на нет. Только в ближних метрах (до ~55 м).
    // В ДОЖДЬ весь контровой эффект выключен (wet) — без rim-обводок.
    vec3 lit = col;                          // освещённый цвет до затемнения
    float near_fade = 1.0 - smoothstep(15.0, 55.0, d);
    float backlit = (1.0 - smoothstep(-0.1, 0.55, dot(L, V))) * near_fade
                  * (1.0 - wet);
    float hf = clamp(vHeight / 0.9, 0.0, 1.0);   // 0 = низкая трава, 1 = пшеница
    float bottom = mix(0.85, 0.5, hf);           // затемнение у самого низа
    float shade = mix(bottom, 1.0, suv.y);       // низ → верх
    col *= mix(1.0, shade, backlit);

    // СВЕТЯЩАЯСЯ ОБВОДКА по силуэту спрайта: где рядом прозрачный пиксель.
    // Два слоя: слабое резкое кольцо (~1.5 текселя спрайта) + широкий
    // мягкий ореол (~3 текселя) — получается плавное свечение, не резкая
    // линия. Цвет — освещённый цвет объекта (не белый), гаснет вдали.
    // Ветка: при backlit == 0 результат обнуляется, 8 текстурных выборок
    // были бы впустую (весь дальний колос и трава "не подсвечены").
    if (backlit > 0.001) {
        vec2 tv = 1.5 / vec2(textureSize(uVeg, 0).xy);
        vec2 tv2 = tv * 2.0;
        float o1 = max(max(1.0 - texture(uVeg, vec3(suv + vec2(tv.x, 0.0), vLayer)).a,
                           1.0 - texture(uVeg, vec3(suv - vec2(tv.x, 0.0), vLayer)).a),
                       max(1.0 - texture(uVeg, vec3(suv + vec2(0.0, tv.y), vLayer)).a,
                           1.0 - texture(uVeg, vec3(suv - vec2(0.0, tv.y), vLayer)).a));
        float o2 = max(max(1.0 - texture(uVeg, vec3(suv + vec2(tv2.x, 0.0), vLayer)).a,
                           1.0 - texture(uVeg, vec3(suv - vec2(tv2.x, 0.0), vLayer)).a),
                       max(1.0 - texture(uVeg, vec3(suv + vec2(0.0, tv2.y), vLayer)).a,
                           1.0 - texture(uVeg, vec3(suv - vec2(0.0, tv2.y), vLayer)).a));
        float glow = (smoothstep(0.2, 0.7, o1) * 0.42
                    + smoothstep(0.1, 0.45, o2) * 0.26) * 0.6;   // -40% свечение
        col += lit * backlit * glow;
    }

    // туман по дальности. При uFogOn = 0 (r_fog_mode 0) — без затрат.
    if (uFogOn > 0.5) {
        float fog = smoothstep(uFogStart, uFogEnd, d);
        col = mix(col, uFogColor, fog);
    }
    FragColor = vec4(col, 1.0);
}
