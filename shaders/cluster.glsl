// ============================================================================
//  cluster.glsl — кластерное освещение во фрагментном шейдере (Forward+)
// ----------------------------------------------------------------------------
//  Включается в шейдеры сцены (#include "cluster.glsl") и даёт одну функцию:
//      vec3 cluster_light(vec3 P, vec3 N, vec3 V, float rough)
//  Она находит фроксель пикселя и суммирует ТОЛЬКО те источники, что реально
//  до него достают. Типы источников (см. lights.h):
//    0 POINT — точечный со сферическим радиусом (мягкий блик по размеру лампы)
//    1 SPOT  — конус с плавной кромкой inner/outer
//    2 RECT  — площадной прямоугольник: диффуз по «форм-фактору» площадки,
//              блик — по ближайшей точке прямоугольника (representative point)
//    3 TUBE  — линейный: ближайшая точка отрезка (люминесцентная лампа/неон)
//
//  Затухание — физичное «обратный квадрат с окном»:
//      att = (1/(d²+r²)) * window(d/range)⁴
//  Такое окно даёт мягкий уход в ноль на границе радиуса без резкой кромки.
//  Спекуляр — GGX-приближение (нормализованный Blinn-Phong по roughness),
//  чтобы блики от площадных источников выглядели как в UE, а не как точки.
//
//  Шейдер, который это включает, ОБЯЗАН объявить до include:
//      uniform float uClusterOn;  uniform vec3 uClusterGrid;
//      uniform float uClusterZNear, uClusterZScale;  uniform vec2 uClusterScreen;
//      uniform float uLightDebug;
//  Буферы объявлены здесь (binding 3/4/5 — их ставит lights_bind).
// ============================================================================

// Источник: 5 x vec4 (см. lights.c)
struct ClusterLight {
    vec4 pos_type;     // xyz позиция, w тип
    vec4 color;        // rgb цвет * интенсивность, w интенсивность
    vec4 dir_range;    // xyz направление, w радиус влияния
    vec4 shape;        // x радиус источника, y cos_outer, z cos_inner, w half_h
    vec4 up_size;      // xyz вторая ось, w half_w / half_len
};

layout(std430, binding = 3) readonly buffer ClusterLights {
    ClusterLight uLights[];
};
layout(std430, binding = 4) readonly buffer ClusterGrid {
    uint uGrid[];      // на фроксель: offset, count
};
layout(std430, binding = 5) readonly buffer ClusterIndex {
    uint uLightIndex[];
};

// Индекс фрокселя по экранной позиции и view-глубине (метры).
int cluster_index(vec2 frag, float view_z) {
    ivec3 g = ivec3(uClusterGrid);
    vec2 uv = frag / max(uClusterScreen, vec2(1.0));
    int x = clamp(int(uv.x * float(g.x)), 0, g.x - 1);
    int y = clamp(int(uv.y * float(g.y)), 0, g.y - 1);
    float z = max(view_z, uClusterZNear);
    int s = clamp(int(log(z / uClusterZNear) * uClusterZScale), 0, g.z - 1);
    return (s * g.y + y) * g.x + x;
}

// Затухание: обратный квадрат + мягкое окно на границе радиуса.
float cluster_falloff(float dist2, float range, float src_r) {
    float att = 1.0 / (dist2 + src_r * src_r);
    float t = clamp(dist2 / (range * range), 0.0, 1.0);
    float win = 1.0 - t * t;
    return att * win * win;
}

// Нормализованный GGX-ish спекуляр (Blinn-Phong с энергосохранением):
// «шероховатость» rough 0..1 -> показатель степени.
float cluster_spec(vec3 N, vec3 H, float rough) {
    float a = max(rough * rough, 0.002);
    float power = 2.0 / (a * a) - 2.0;
    float nh = max(dot(N, H), 0.0);
    // (power+8)/8 — множитель энергосохранения Blinn-Phong
    return pow(nh, power) * (power + 8.0) / 8.0;
}

// Ближайшая точка отрезка (для трубчатых источников)
vec3 closest_on_segment(vec3 P, vec3 c, vec3 axis, float half_len) {
    float t = clamp(dot(P - c, axis), -half_len, half_len);
    return c + axis * t;
}

// Ближайшая точка прямоугольника (для площадных источников)
vec3 closest_on_rect(vec3 P, vec3 c, vec3 n, vec3 up, float hw, float hh) {
    vec3 right = normalize(cross(up, n));
    vec3 upn = normalize(cross(n, right));
    vec3 d = P - c;
    float x = clamp(dot(d, right), -hw, hw);
    float y = clamp(dot(d, upn), -hh, hh);
    return c + right * x + upn * y;
}

// Суммарный вклад локальных источников фрокселя.
//   P — мировая точка, N — нормаль, V — на камеру, rough — шероховатость 0..1
//   spec_out (out) — спекулярная часть отдельно (её можно масштабировать)
vec3 cluster_light_ex(vec3 P, vec3 N, vec3 V, float rough, out vec3 spec_out) {
    spec_out = vec3(0.0);
    if (uClusterOn < 0.5) return vec3(0.0);
    // view_z: расстояние вдоль взгляда. gl_FragCoord.w = 1/clip.w, а clip.w
    // для перспективной проекции равен -z_view (в метрах).
    float view_z = 1.0 / max(gl_FragCoord.w, 1e-6);
    int c = cluster_index(gl_FragCoord.xy, view_z);
    uint off = uGrid[c * 2u];
    uint cnt = uGrid[c * 2u + 1u];
    vec3 diff = vec3(0.0);
    for (uint k = 0u; k < cnt; k++) {
        ClusterLight L = uLights[uLightIndex[off + k]];
        int type = int(L.pos_type.w);
        vec3 lp = L.pos_type.xyz;
        float range = L.dir_range.w;
        float src_r = L.shape.x;
        vec3 col = L.color.rgb;

        // «Представительная» точка источника: для площадных/линейных она
        // ближайшая к P — так блик вытягивается по форме источника.
        vec3 rep = lp;
        if (type == 3) {
            rep = closest_on_segment(P, lp, L.up_size.xyz, L.up_size.w);
        } else if (type == 2) {
            rep = closest_on_rect(P, lp, L.dir_range.xyz, L.up_size.xyz,
                                  L.up_size.w, L.shape.w);
        }
        vec3 Lv = rep - P;
        float d2 = dot(Lv, Lv);
        if (d2 > range * range) continue;
        float d = sqrt(max(d2, 1e-8));
        vec3 Ld = Lv / d;

        float att = cluster_falloff(d2, range, src_r);
        if (att <= 0.0) continue;

        // Конус прожектора
        if (type == 1) {
            float cd = dot(-Ld, L.dir_range.xyz);
            float co = L.shape.y, ci = L.shape.z;
            float cone = clamp((cd - co) / max(ci - co, 1e-4), 0.0, 1.0);
            cone *= cone;                       // мягкая кромка
            if (cone <= 0.0) continue;
            att *= cone;
        }
        // Площадной: свет идёт только с лицевой стороны панели
        if (type == 2) {
            float face = dot(L.dir_range.xyz, -Ld);
            if (face <= 0.0) continue;
            att *= face;
        }

        // Диффуз. Для площадных/линейных N·L «размывается» размером источника:
        // у широкой панели свет заворачивает за терминатор (wrap lighting).
        float wrap = (type == 2 || type == 3)
                   ? clamp(src_r / max(d, 0.05), 0.0, 0.5) : 0.0;
        float ndl = (dot(N, Ld) + wrap) / (1.0 + wrap);
        ndl = max(ndl, 0.0);
        diff += col * (att * ndl);

        // Спекуляр по представительной точке
        vec3 H = normalize(Ld + V);
        float s = cluster_spec(N, H, rough);
        spec_out += col * (att * s * max(dot(N, Ld), 0.0));
    }
    // Отладка: тепловая карта числа источников на фроксель
    if (uLightDebug > 0.5) {
        float t = float(cnt) / 8.0;
        diff = mix(vec3(0.0, 0.2, 0.0), vec3(1.0, 0.0, 0.0), clamp(t, 0.0, 1.0));
        spec_out = vec3(0.0);
    }
    return diff;
}

vec3 cluster_light(vec3 P, vec3 N, vec3 V, float rough) {
    vec3 spec;
    vec3 d = cluster_light_ex(P, N, V, rough, spec);
    return d + spec;
}
