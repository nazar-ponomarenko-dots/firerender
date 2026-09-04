// ============================================================================
//  gi_blur.fs — пространственный денойзер GI (билатеральный, по плоскости)
// ----------------------------------------------------------------------------
//  Экранное GI считается малым числом лучей — остаётся зерно. Гасим его
//  широким размытием, но ТОЛЬКО внутри одной поверхности.
//
//  Вес соседа даёт не разница глубин, а расстояние соседа до КАСАТЕЛЬНОЙ
//  ПЛОСКОСТИ центрального пикселя. Разница глубин сама по себе плохой
//  критерий: на скошенной поверхности (пол под углом к камере) она большая у
//  соседей той же плоскости — фильтр вырождался в «почти без размытия», и
//  зерно приходилось прятать увеличением радиуса, что размазывало углы.
//  Плоскостной вес держит соседей одного пола/стены и рвёт связь на изломе,
//  поэтому свет не перетекает через углы и силуэты.
//
//  Ещё: сэмплы GI распределены по пикселям через IGN (см. gi.fs), то есть
//  соседи держат РАЗНЫЕ направления полусферы. Этот фильтр их и складывает —
//  фактически поднимает число лучей на пиксель без затрат на трассировку.
//
//  Проход разделяемый: uDir = (1,0) — горизонталь, (0,1) — вертикаль.
// ============================================================================

in vec2 vUV;

uniform sampler2D uGI;        // GI (unit 0): rgb непрямой свет, a = затенение
uniform sampler2D uDepth;     // глубина сцены (unit 1)
uniform mat4 uInvViewProj;    // для реконструкции мировых позиций
uniform vec3 uCamPos;
uniform vec2 uTexel;          // 1 / разрешение GI-таргета
uniform vec2 uDir;            // направление прохода
uniform vec2 uZ;              // znear, zfar
uniform float uRadius;        // радиус в текселях

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

vec3 wnormal(vec2 uv, float d0, vec3 p0) {
    vec2 tx = vec2(uTexel.x, 0.0);
    vec2 ty = vec2(0.0, uTexel.y);
    float dR = texture(uDepth, uv + tx).r;
    float dL = texture(uDepth, uv - tx).r;
    bool useR = abs(dR - d0) < abs(dL - d0);
    vec3 px = wpos(uv + (useR ? tx : -tx), useR ? dR : dL);
    float dU = texture(uDepth, uv + ty).r;
    float dD = texture(uDepth, uv - ty).r;
    bool useU = abs(dU - d0) < abs(dD - d0);
    vec3 py = wpos(uv + (useU ? ty : -ty), useU ? dU : dD);
    vec3 n = cross(px - p0, py - p0);
    float l = length(n);
    if (l < 1e-6 || isnan(l) || isinf(l)) return vec3(0.0, 1.0, 0.0);
    n /= l;
    if (dot(n, uCamPos - p0) < 0.0) n = -n;
    return n;
}

void main() {
    float d0 = texture(uDepth, vUV).r;
    vec4 c0 = texture(uGI, vUV);
    if (d0 >= 0.9999 || d0 <= 1e-5) { FragColor = c0; return; }   // небо не размываем

    vec3 P = wpos(vUV, d0);
    vec3 N = wnormal(vUV, d0, P);
    float z0 = linz(d0 * 2.0 - 1.0);

    // Допуск по плоскости растёт с дистанцией: далеко один тексель покрывает
    // больше мира, и «своя» поверхность честно уходит дальше от плоскости.
    float tol = 0.02 + 0.02 * z0;

    // Гаусс 9 тапов; вес умножается на плоскостной множитель.
    const float w[5] = float[5](0.227, 0.194, 0.121, 0.054, 0.016);
    vec4 acc = c0 * w[0];
    float wsum = w[0];
    for (int i = 1; i < 5; i++) {
        vec2 off = uDir * uTexel * (float(i) * uRadius);
        for (int s = 0; s < 2; s++) {
            vec2 uv = s == 0 ? vUV + off : vUV - off;
            float d = texture(uDepth, uv).r;
            if (d >= 0.9999 || d <= 1e-5) continue;
            vec3 q = wpos(uv, d);
            float plane = abs(dot(q - P, N));           // отход от плоскости
            float bw = 1.0 - smoothstep(tol, tol * 4.0, plane);
            if (bw <= 0.0) continue;
            acc += texture(uGI, uv) * (w[i] * bw);
            wsum += w[i] * bw;
        }
    }
    FragColor = acc / max(wsum, 1e-4);
}
