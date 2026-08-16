// ============================================================================
//  Water — фрагментный шейдер воды: лужи/озёра/реки/моря (см. water.c)
// ----------------------------------------------------------------------------
//  - Рисует только там, где рельеф ниже uLevel (глубина > 0): сухой берег
//    отбрасывается (discard). Глубина — из карты высот (uHeightmap, юнит 2).
//  - Нормали волн — аналитически из той же waveY, что и в VS (конечные
//    разности): гладко при любой сетке.
//  - Альфа растёт с глубиной (uDepthFade) и гаснет к берегу (vEdge).
//  - Псевдо-PBR как у рельефа: полусферный ambient + направленные/точечные/
//    прожекторные источники + блик (spec) + отражение неба по fresnel +
//    пена у берега + туман по дальности.
// ============================================================================

#define SUN_MAX_LIGHTS 4
#define SCENE_MAX_POINT 8
#define SCENE_MAX_SPOT  4

in vec3 vWorldPos;
in vec2 vUV;
in float vEdge;

// --- Общий набор сцены (SceneUniforms, render_ctx.h) -------------------------
uniform vec3 uCamPos;
uniform vec3 uAmbient;
uniform vec3 uSkyColor;
uniform vec3 uGroundColor;
uniform vec3 uFogColor;
uniform float uFogStart;
uniform float uFogEnd;
uniform float uFogOn;

uniform vec3 uLightDir[SUN_MAX_LIGHTS];
uniform vec3 uLightColor[SUN_MAX_LIGHTS];
uniform float uLightIntensity[SUN_MAX_LIGHTS];
uniform int uLightCount;
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

// --- Собственные uniform'ы воды (water_prog_init / water_instance_uniforms) --
uniform float uTime;
uniform float uLevel;
uniform float uWaveAmp;
uniform float uWaveSpeed;
uniform float uWaveLen;
uniform vec2  uFlowDir;
uniform vec3  uWaterColor;    // тон воды на глубине
uniform float uAlpha;         // базовая непрозрачность 0..1
uniform float uDepthFade;     // просвет дна сквозь воду, м
uniform float uFlowSpeed;     // скорость течения (сдвиг блика)
uniform float uFoam;          // сила пены у берега 0..1
uniform float uEdge;          // ширина кромки (water_edge_fade)
uniform float uWetness;       // мокрость 0..1 (дождь)
uniform sampler2D uHeightmap;  // карта высот (юнит 2, как в рельефе)
uniform vec2  uWorldSize;      // размер мира (для UV к карте высот)
uniform float uHeightMin;      // высота, соответств. 0 карты высот
uniform float uHeightMax;      // высота, соответств. 1 карты высот
uniform samplerCube uReflCube;   // кубомап отражения (юнит 7)

out vec4 FragColor;

// Волновая функция — та же, что в water.vs (нормаль — конечные разности).
float waveY(vec2 p, float t) {
    float k = 6.2831853 / uWaveLen;
    vec2 f = uFlowDir;
    if (dot(f, f) < 0.0001) f = vec2(1.0, 0.0);
    f = normalize(f);
    vec2 q = vec2(-f.y, f.x);
    return uWaveAmp * sin(dot(f, p) * k + t * uWaveSpeed)
         + uWaveAmp * 0.45 * sin(dot(q, p) * k * 0.62 + t * uWaveSpeed * 1.17)
         + uWaveAmp * 0.2  * sin((p.x + p.y) * k * 0.34 + t * uWaveSpeed * 0.8);
}

void main() {
    // Глубина: рельеф под водой (карта высот), вода есть, где ниже uLevel.
    float terrainH = mix(uHeightMin, uHeightMax,
                         texture(uHeightmap, vWorldPos.xz / uWorldSize).r);
    float waterDepth = uLevel - terrainH;
    if (waterDepth <= 0.02) discard;          // сухой берег

    // Нормаль волны — конечные разности той же функции, что в VS.
    vec2 p = vWorldPos.xz;
    float e = max(uWaveLen / 6.0, 0.1);
    float hL = waveY(p - vec2(e, 0.0), uTime);
    float hR = waveY(p + vec2(e, 0.0), uTime);
    float hD = waveY(p - vec2(0.0, e), uTime);
    float hU = waveY(p + vec2(0.0, e), uTime);
    vec3 N = normalize(vec3(hL - hR, 2.0 * e, hD - hU));
    vec3 V = normalize(uCamPos - vWorldPos);
    // Двусторонняя геометрия (как в mesh.fs): снизу (из-под воды) нормаль
    // переворачиваем к камере, иначе поверхность снизу была чёрной.
    if (dot(N, V) < 0.0) N = -N;

    // Кромка (0 у берега .. 1 внутри); у открытого моря vEdge = 1e6 -> 1.
    float edgeFade = clamp(vEdge / max(uEdge, 0.001), 0.0, 1.0);
    // Непрозрачность: растёт с глубиной и гаснет к берегу.
    float depthT = clamp(waterDepth / max(uDepthFade, 0.01), 0.0, 1.0);
    float alpha = uAlpha * depthT * edgeFade;

    // Освещение (как у рельефа/мешей): полусферный ambient.
    vec3 amb = mix(uGroundColor, uSkyColor, N.y * 0.5 + 0.5) * uAmbient;
    vec3 light = amb;
    for (int i = 0; i < uLightCount; i++) {
        vec3 li = normalize(uLightDir[i]);
        float diff = max(dot(N, li), 0.0);
        // Порог как у рельефа (шумыс 0.5+0.5 на матовое), но чуть больше
        // контраста — блеск воды.
        light += uLightColor[i] * uLightIntensity[i] * (0.45 + 0.55 * diff);
    }
    for (int i = 0; i < uPointCount; i++) {
        vec3 Lp = uPointPos[i] - vWorldPos;
        float dpl = length(Lp);
        vec3 lip = Lp / max(dpl, 0.001);
        float att = max(1.0 - dpl / uPointRange[i], 0.0);
        att *= att;
        float diff = max(dot(N, lip), 0.0);
        light += uPointColor[i] * uPointIntensity[i] * (0.45 + 0.55 * diff) * att;
    }
    for (int i = 0; i < uSpotCount; i++) {
        vec3 Lp = uSpotPos[i] - vWorldPos;
        float dpl = length(Lp);
        vec3 lip = Lp / max(dpl, 0.001);
        float att = max(1.0 - dpl / uSpotRange[i], 0.0);
        att *= att;
        float cone = max(dot(-lip, normalize(uSpotDir[i])), 0.0);
        float edg = smoothstep(uSpotAngle[i] - 0.08, uSpotAngle[i], cone);
        float diff = max(dot(N, lip), 0.0);
        light += uSpotColor[i] * uSpotIntensity[i] * (0.45 + 0.55 * diff) * att * edg;
    }
    light = min(light, vec3(1.0));

    vec3 col = uWaterColor * light;

    // Цвет воды всегда заметен: не даём вырождаться в чёрную яму или
    // сливаться с небом. Основа — вода, а фреснель-микс неба умеренный.
    col = max(col, uSkyColor * 0.10);

    // Отражение: вектор отражённого луча считаем по нормали волны и сэмплим
    // кубомап отражения. I — луч камера -> поверхность; reflect(I, N) даёт
    // направление, в котором «отражается» мир в точке воды.
    vec3 I = normalize(vWorldPos - uCamPos);
    vec3 reflDir = reflect(I, N);
    vec3 reflCol = texture(uReflCube, reflDir).rgb;
    // Френель-смешивание: база 0.10 (вода всегда что-то отражает) + рост на
    // гляных углах. pow5 с чистым нулём в базе делал отражения невидимыми
    // почти со всех углов зрения.
    float fres = 0.10 + 0.90 * pow(1.0 - max(dot(N, V), 0.0), 5.0);
    col = mix(col, reflCol, clamp(fres * 0.85 + 0.10, 0.0, 1.0));

    // Пена у берега (там, где vEdge стремится к 0).
    float foamAmt = uFoam * (1.0 - smoothstep(0.0, 2.0, vEdge));
    col += foamAmt * (uSkyColor + uWaterColor) * 0.5;

    // Туман по дальности (как в рельефе).
    if (uFogOn > 0.5) {
        float fog = smoothstep(uFogStart, uFogEnd, distance(uCamPos, vWorldPos));
        col = mix(col, uFogColor, fog);
    }

    FragColor = vec4(col, alpha);
}