// ============================================================================
//  taa.fs — временное сглаживание (TAA)
// ----------------------------------------------------------------------------
//  Рендер идёт с субпиксельным джиттером проекции (8-фазный Halton, см.
//  taa_jitter): геометрия каждый кадр чуть сдвинута, и край объекта
//  «размазывается» по времени. Здесь кадр совмещается с историей:
//
//    1) мировая точка пикселя восстанавливается из глубины и ПЕРЕПРОЕЦИРУЕТСЯ
//       матрицей прошлого кадра — так находится её прошлое положение;
//    2) история зажимается в min/max 3x3 окрестности ТЕКУЩЕГО кадра (variance
//       clip): всё, что не подтверждается текущим кадром (дизокклюзии,
//       уехавшая трава), гасится за 1-2 кадра — без шлейфов;
//    3) итог = mix(текущий, история, uBlend) — шум джиттера и временные
//       эффекты (джиттер тумана, GI) усредняются, картинка гладкая.
//
//  Без истории (первый кадр / смена размера) — текущий кадр как есть.
// ============================================================================

in vec2 vUV;

uniform sampler2D uScene;      // текущий кадр (unit 0, после MSAA-resolve)
uniform sampler2D uDepth;      // глубина (unit 1)
uniform sampler2D uHistory;    // TAA прошлого кадра (unit 2)
uniform mat4 uInvViewProj;     // текущий viewproj (джиттерованный)
uniform mat4 uPrevViewProj;    // viewproj прошлого кадра
uniform vec3 uCamPos;
uniform vec2 uTexel;           // 1 / разрешение
uniform vec2 uZ;               // znear, zfar
uniform float uBlend;          // вес истории 0..0.95

out vec4 FragColor;

float linz(float ndc_z) {
    float zn = uZ.x, zf = uZ.y;
    return (2.0 * zn * zf) / (zf + zn - ndc_z * (zf - zn));
}

vec3 wpos(vec2 uv, float depth) {
    vec4 ndc = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 wp = uInvViewProj * ndc;
    return wp.xyz / wp.w;
}

vec3 sane3(vec3 c) {
    c = mix(c, vec3(0.0), vec3(isnan(c.x) || isinf(c.x),
                               isnan(c.y) || isinf(c.y),
                               isnan(c.z) || isinf(c.z)));
    return clamp(c, vec3(0.0), vec3(64.0));
}

void main() {
    vec3 cur = sane3(texture(uScene, vUV).rgb);
    float d = texture(uDepth, vUV).r;

    // Перепроекция: мировая точка -> экран прошлого кадра
    vec3 P = wpos(vUV, d);
    vec4 pc = uPrevViewProj * vec4(P, 1.0);
    vec3 hist = cur;
    float ok = 0.0;
    if (pc.w > 0.02) {
        vec2 puv = (pc.xyz / pc.w).xy * 0.5 + 0.5;
        if (puv.x > 0.001 && puv.x < 0.999 && puv.y > 0.001 && puv.y < 0.999) {
            hist = sane3(texture(uHistory, puv).rgb);
            // Проверка глубины: точка на месте? (дизокклюзия/движение)
            float pd = texture(uDepth, puv).r;
            if (pd < 0.9999 && pd > 1e-5) {
                float pz = linz(d * 2.0 - 1.0);
                float pzz = linz(pd * 2.0 - 1.0);
                ok = 1.0 - smoothstep(0.05 * pz + 0.10, 0.35 * pz + 0.8,
                                      abs(pzz - pz));
            }
        }
    }

    // Variance clip: история не может покидать диапазон 3x3 окрестности
    // текущего кадра (плюс маленький запас на сжатие диапазона сглаживанием).
    vec3 mn = cur - 0.02;
    vec3 mx = cur + 0.02;
    for (int i = 0; i < 8; i++) {
        vec2 o = vec2(float(i & 1), float((i >> 1) & 1)) * 2.0 - 1.0;
        vec2 o2 = vec2(float((i >> 1) & 1), float(i & 1)) * 2.0 - 1.0;
        vec3 c1 = sane3(texture(uScene, vUV + o * uTexel).rgb);
        vec3 c2 = sane3(texture(uScene, vUV + o2 * uTexel).rgb);
        mn = min(mn, min(c1, c2));
        mx = max(mx, max(c1, c2));
    }
    hist = clamp(hist, mn, mx);

    FragColor = vec4(mix(cur, hist, uBlend * ok), 1.0);
}
