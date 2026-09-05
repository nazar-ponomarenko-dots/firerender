// ============================================================================
//  fog2d.fs — «Простой» туман: аналитический, БЕЗ 3D-текстур и марша
// ----------------------------------------------------------------------------
//  Замена прежнему упрощённому режиму: имитирует поведение объёмного тумана
//  при минимальной цене (замер ~0.1-0.2 мс GPU против ~0.2-0.5 мс + CPU
//  перестройки вокселей у объёмного; никакой памяти под 3D-каскады).
//
//  Физика в замкнутой форме:
//    * плотность тумана прижата к рельефу: ρ(t) = ρ0·exp(-(y(t)-ground)/H) —
//      высота земли берётся из карты высот в 4 точках вдоль луча (масштаб
//      «туман в лощинах, чисто на вершинах», как у объёмного);
//    * оптическая толщина — трапецеидальный интеграл по этим точкам,
//      ни одного шага марша;
//    * дождевой туман (мокрость) — аналитический интеграл равномерной
//      плотности (1 - exp(-t/30));
//    * солнечное рассеивание: тёплый цвет к солнцу, серый в дождь, ореол
//      (Mie-пик) у направления на солнце. Теневых запросов НЕТ — лучи от
//      деревьев остаются в объёмном режиме, здесь цена важнее.
// ============================================================================

in vec2 vUV;

uniform sampler2D uScene;        // цвет сцены (unit 0)
uniform sampler2D uSceneDepth;   // глубина (unit 1)
uniform mat4 uInvViewProj;
uniform vec3 uCamPos;
uniform vec2 uZ;                 // znear, zfar
uniform vec3 uSunDir;
uniform vec3 uFogColor;          // базовый цвет тумана
uniform vec3 uFogColorSun;       // тёплый цвет рассеяния у солнца
uniform vec3 uSkyColor;          // цвет неба (дождь)
uniform float uGlare;            // ореол у солнца
uniform float uRayStrength;      // «солнечность» рассеивания
uniform float uWetness;          // мокрость 0..1
uniform sampler2D uHeightmap;    // карта высот (unit 10)
uniform vec2 uWorldSize;
uniform float uHeightMin, uHeightMax;
uniform vec4 uFogP;              // x: ρ0 (плотность у земли, 1/м), y: H (высота
                                 // спада, м), z: множитель силы, w: дальность, м

out vec4 FragColor;

float linz(float ndc_z) {
    return (2.0 * uZ.x * uZ.y) / (uZ.y + uZ.x - ndc_z * (uZ.y - uZ.x));
}

vec3 wpos(vec2 uv, float depth) {
    vec4 ndc = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 wp = uInvViewProj * ndc;
    return wp.xyz / wp.w;
}

// Высота рельефа в мировой точке (та же формула, что в volumetric.fs)
float terrain_h(vec2 xz) {
    return mix(uHeightMin, uHeightMax, texture(uHeightmap, xz / uWorldSize).r);
}

void main() {
    vec3 scene = texture(uScene, vUV).rgb;
    float depth = texture(uSceneDepth, vUV).r;

    // Луч: из видовой камеры в мировую точку пикселя (небо — на дальность
    // тумана, как кап марша в объёмном режиме)
    vec4 ndc = vec4(vUV * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 wp4 = uInvViewProj * ndc;
    vec3 P = wp4.xyz / wp4.w;
    vec3 ray = P - uCamPos;
    float dist = length(ray);
    float L = min(dist, uFogP.w);
    vec3 dir = ray / max(dist, 1e-5);
    if (depth >= 0.9999) P = uCamPos + dir * L;

    // Плотность вдоль луча: 4 сэмпла (0.1, 0.4, 0.7, 0.95) — трапеция.
    // ρ = ρ0 * exp(-(высота над рельефом) / H), выше 60 м над землёй — ноль.
    float rho0 = uFogP.x * uFogP.z;
    float H = max(uFogP.y, 1.0);
    float od = 0.0;
    float wsum = 0.0;
    for (int i = 0; i < 4; i++) {
        float t = (0.10 + 0.2833 * float(i)) * L;
        vec3 p = uCamPos + dir * t;
        float above = p.y - terrain_h(p.xz);
        float rho = rho0 * exp(-max(above, 0.0) / H)
                  * (1.0 - smoothstep(40.0, 60.0, above));
        od += rho;
        wsum += 1.0;
    }
    od *= L / max(wsum, 1e-4);

    // Дождевой туман: равномерная плотность, аналитический интеграл
    od += uWetness * 0.03 * (L - 30.0 * (1.0 - exp(-L / 30.0)));

    float trans = exp(-od);

    // --- Цвет и подсветка тумана (как в volumetric.fs, без теневых запросов)
    float sunHit = max(dot(dir, uSunDir), 0.0);
    float rainDim = 1.0 - uWetness;
    vec3 fcol = mix(uFogColorSun, uFogColor, 0.15 * (1.0 - sunHit));
    fcol = mix(fcol, uFogColor, uWetness);
    // В стороне от солнца — тёплая дымка, к солнцу — ярче (god rays упрощённо
    // читаются как свечение сцены у солнца; тени лучей — только в объёмном).
    float lit = mix(0.55, 0.35 + 0.65 * uRayStrength, sunHit * rainDim);
    lit = max(lit, mix(0.0, 0.32, uWetness));
    // Ореол у самого солнца (Mie-пик)
    float glare = pow(sunHit, 8.0) * uGlare * rainDim;

    vec3 col = scene * trans + fcol * (1.0 - trans) * (lit + glare * trans);
    FragColor = vec4(col, 1.0);
}
