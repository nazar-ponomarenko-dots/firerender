// ============================================================================
//  gi.fs — динамическое глобальное освещение (диффузное GI + AO), v5
// ----------------------------------------------------------------------------
//  Считается на пониженном разрешении в RGBA16F:
//    rgb = НЕПРЯМОЙ свет: диффузное переотражение от геометрии кадра
//          + зеркальный отблеск по направлению взгляда + подсветка снизу
//          цветом земли (небо НЕ добавляем — оно уже в ambient'е сцены),
//    a   = затенение 0..1 (1 = открыто), два масштаба в одном значении.
//
//  Трассировка: косинусные лучи в полусфере нормали, марш по буферу глубины.
//
//  v6 — ЗАКОН СОХРАНЕНИЯ ЭНЕРГИИ. Раньше переотражение имело множитель 1.6,
//  распространение (spread) добавлялось СВЕРХУ, а отблеск мог дать ещё до x3.
//  Суммарное усиление за отражение доходило до ~4.5 — GI подсвечивал сам
//  себя, и половина сцены «светилась» ярче источников. Теперь:
//   * весь свет, отданный точкой попадания (прямой + накопленный непрямой),
//     умножается на ОДНО альбедо GI_ALBEDO < 1 и делится на (1 + spread).
//     Установившееся значение: G = alb*I / (1 + spread*(1 - alb)) < I —
//     непрямой свет ФИЗИЧЕСКИ не может стать ярче входного при любых
//     значениях cvar'ов, а цепочка переотражений всё равно работает;
//   * отблеск живёт в том же бюджете: френель <= 0.95, reflw <= 1;
//   * итог зажат в uClamp (был uClamp * 3).
//
//  Производительность (без потери картинки):
//   * марш начинается сразу за bias: раньше первые сэмплы квадратичной
//     раскладки падали под bias и отбрасывались через continue — до трети
//     шагов уходило впустую. Теперь ВСЕ шаги работают (та же цена, выше
//     качество);
//   * распространение не сэмплируется на дальнем LOD (не видно);
//   * отблеск марширует 3/4 шагов диффузного луча.
// ============================================================================

in vec2 vUV;

uniform sampler2D uScene;        // цвет сцены (unit 0), RGBA16F
uniform sampler2D uDepth;        // глубина сцены (unit 1)
uniform sampler2D uPrev;         // GI прошлого кадра (unit 2) — накопление
uniform mat4 uViewProj;
uniform mat4 uInvViewProj;
uniform mat4 uPrevViewProj;
uniform vec3 uCamPos;
uniform vec2 uTexel;             // 1 / разрешение ЭТОГО таргета
uniform vec4 uParams;            // x лучи, y шаги, z радиус GI (м), w znear
uniform vec4 uParams2;           // x zfar, y вес неба, z фаза, w радиус AO (м)
uniform vec4 uParams3;           // x насыщение, y отблеск, z распространение
uniform vec3 uGroundColor;       // цвет «низа» (луч ушёл вниз мимо геометрии)
uniform float uClamp;            // потолок энергии GI на пиксель
uniform float uBlend;            // вес истории 0..0.96

out vec4 FragColor;

vec3 sane3(vec3 c) {
    c = mix(c, vec3(0.0), vec3(isnan(c.x) || isinf(c.x),
                               isnan(c.y) || isinf(c.y),
                               isnan(c.z) || isinf(c.z)));
    return clamp(c, vec3(0.0), vec3(32.0));
}
float sane1(float v) {
    return (isnan(v) || isinf(v)) ? 0.0 : clamp(v, 0.0, 1.0);
}

vec3 wpos(vec2 uv, float depth) {
    vec4 ndc = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 wp = uInvViewProj * ndc;
    return wp.xyz / wp.w;
}

float linz(float ndc_z) {
    float zn = uParams.w, zf = uParams2.x;
    return (2.0 * zn * zf) / (zf + zn - ndc_z * (zf - zn));
}

// Нормаль из глубины: ближайший по глубине сосед по каждой оси (устойчиво
// на силуэтах). Шаг — тексель GI-таргета: на полкадре соседи сцены слишком
// близко и нормаль тонет в шуме глубины. Вырожденный случай -> «вверх».
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

// Нормаль в точке ПОПАДАНИЯ: две выборки вперёд вместо четырёх. Точность там
// нужна ниже (результат идёт в косинус), а вызывается она на каждый удачный
// луч — экономия заметная.
vec3 wnormal_fast(vec2 uv, vec3 p0) {
    vec2 tx = vec2(uTexel.x, 0.0);
    vec2 ty = vec2(0.0, uTexel.y);
    vec3 px = wpos(uv + tx, texture(uDepth, uv + tx).r);
    vec3 py = wpos(uv + ty, texture(uDepth, uv + ty).r);
    vec3 n = cross(px - p0, py - p0);
    float l = length(n);
    if (l < 1e-6 || isnan(l) || isinf(l)) return vec3(0.0, 1.0, 0.0);
    n /= l;
    if (dot(n, uCamPos - p0) < 0.0) n = -n;
    return n;
}

// Interleaved gradient noise: даёт по соседним пикселям почти равномерно
// распределённые значения (в отличие от белого шума). Именно это позволяет
// денойзеру собирать из 4-8 соседей полноценную оценку полусферы.
float ign(vec2 p) {
    return fract(52.9829189 * fract(0.06711056 * p.x + 0.00583715 * p.y));
}

// Насыщенность: подчёркивает оттенок отражённого света, сохраняя яркость.
vec3 saturate_hue(vec3 c, float s) {
    float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
    return max(mix(vec3(l), c, s), vec3(0.0));
}

// Проецирует мировую точку в экран. false — точка за камерой или вне кадра.
bool project(vec3 wp, out vec2 uv, out float ray_z) {
    vec4 clip = uViewProj * vec4(wp, 1.0);
    uv = vec2(0.0);
    ray_z = 0.0;
    if (clip.w <= 0.02) return false;
    vec3 ndc = clip.xyz / clip.w;
    uv = ndc.xy * 0.5 + 0.5;
    ray_z = linz(ndc.z);
    return uv.x > 0.0 && uv.x < 1.0 && uv.y > 0.0 && uv.y < 1.0;
}

// Марш одного луча по буферу глубины. Сэмплы распределены квадратично: густо
// у поверхности (контактные тени) и до полного радиуса к концу. На удачном
// шаге интервал делится дважды — точка контакта садится на поверхность.
//
// ОПТИМИЗАЦИЯ: раскладка начинается СРАЗУ за bias. Раньше t = radius*f*f
// считался от нуля, и первые сэмплы (при малом radius — до трети всех шагов)
// оказывались меньше bias и выбрасывались через continue: работа впустую,
// причём именно там, где нужны контактные тени. Теперь интервал [bias, radius]
// раскладывается целиком — та же цена, все шаги полезные.
bool trace_ray(vec3 O, vec3 dir, float radius, int steps, float jit, float bias,
               out float hit_dist, out vec2 hit_uv, out float hit_sd) {
    hit_dist = radius;
    hit_uv = vec2(0.0);
    hit_sd = 0.0;
    float steps_f = float(steps);
    float span = max(radius - bias, 1e-3);
    float t_prev = bias;
    for (int s = 0; s < steps; s++) {
        float f = (float(s) + jit) / steps_f;
        float t = bias + span * f * f;
        vec2 uv; float ray_z;
        if (!project(O + dir * t, uv, ray_z)) return false;
        float sd = texture(uDepth, uv).r;
        if (sd < 0.9999 && sd > 1e-5) {
            float scene_z = linz(sd * 2.0 - 1.0);
            float diff = ray_z - scene_z;
            float front = 0.015 + 0.01 * scene_z;
            // Окно «толщины» растёт с длиной шага (иначе луч перескакивал
            // поверхность и попадание отбрасывалось), но потолок тугой: в
            // траве и листве буфер глубины — частая мелочь, и широкое окно
            // превращало «луч прошёл рядом с травинкой» в «луч упёрся в
            // стену». Именно эти ложные попадания чернили поле.
            float thick = min(max(0.10, (t - t_prev) * 1.6) + 0.02 * scene_z,
                              0.45 + 0.05 * scene_z);
            if (diff > front && diff < thick) {
                float lo = t_prev, hi = t;
                for (int k = 0; k < 2; k++) {
                    float mid = (lo + hi) * 0.5;
                    vec2 muv; float mz;
                    if (!project(O + dir * mid, muv, mz)) { hi = mid; continue; }
                    float msd = texture(uDepth, muv).r;
                    if (msd >= 0.9999 || msd <= 1e-5) { lo = mid; continue; }
                    if (mz - linz(msd * 2.0 - 1.0) > front) {
                        hi = mid;
                        uv = muv;
                        sd = msd;
                    } else {
                        lo = mid;
                    }
                }
                hit_dist = hi;
                hit_uv = uv;
                hit_sd = sd;
                return true;
            }
        }
        t_prev = t;
    }
    return false;
}

// Цвет попадания с потолком яркости: иначе один пересвеченный тексель после
// деления на малое число лучей даёт стабильного «светлячка».
vec3 hit_color(vec2 uv) {
    vec3 hc = sane3(texture(uScene, uv).rgb);
    float hl = dot(hc, vec3(0.2126, 0.7152, 0.0722));
    return hl > uClamp ? hc * (uClamp / max(hl, 1e-4)) : hc;
}

// --- ЗАКОН СОХРАНЕНИЯ ЭНЕРГИИ -----------------------------------------------
// Диффузное альбедо переотражения. Реальные материалы отражают 20..60% света;
// 0.55 — верх правдоподобного. Множитель ЖЁСТКО < 1, поэтому свет с каждым
// отражением только убывает. Раньше здесь стояло 1.6 (и spread добавлялся
// сверху) — GI подсвечивал сам себя, отсюда и «пол сцены светится».
#define GI_ALBEDO 0.55

// Свет, отданный точкой попадания: прямой (сцена) + накопленный непрямой
// (история GI в этой точке). Сумма нормирована на (1 + spread), поэтому
// добавление цепочки переотражений НЕ увеличивает суммарную энергию — оно
// перераспределяет её между «прямым» и «пришедшим по цепочке».
//   Установившееся значение: G = alb*I / (1 + spread*(1 - alb)) < I.
// Ни при каком spread (0..1) непрямой свет не станет ярче входного.
vec3 hit_radiance(vec2 huv, vec3 hp, float spread, float lod) {
    vec3 hc = hit_color(huv);
    // На дальнем LOD цепочку не собираем: её там не видно, а выборка стоит.
    if (spread > 0.001 && lod < 0.6) {
        vec2 suv = huv;
        vec4 sp = uPrevViewProj * vec4(hp, 1.0);
        if (sp.w > 0.02) {
            vec2 t2 = (sp.xyz / sp.w).xy * 0.5 + 0.5;
            if (t2.x > 0.002 && t2.x < 0.998 &&
                t2.y > 0.002 && t2.y < 0.998) suv = t2;
        }
        hc = (hc + sane3(texture(uPrev, suv).rgb) * spread) / (1.0 + spread);
    }
    return hc * GI_ALBEDO;
}

void main() {
    float d0 = texture(uDepth, vUV).r;
    // Небо: непрямого света не несёт, затенения нет
    if (d0 >= 0.9999) { FragColor = vec4(0.0, 0.0, 0.0, 1.0); return; }
    // Near-план (в упор под ногами): реконструкция вырождается -> мусор
    if (d0 <= 1e-5)   { FragColor = vec4(0.0, 0.0, 0.0, 1.0); return; }

    vec3 P = wpos(vUV, d0);
    vec3 N = wnormal(vUV, d0, P);
    float pz = linz(d0 * 2.0 - 1.0);

    float radius = max(uParams.z, 0.1);
    float skyw = clamp(uParams2.y, 0.0, 1.0);
    float phase = uParams2.z;
    float aoRadius = max(uParams2.w, 0.05);
    float bounce = max(uParams3.x, 0.0);
    float reflw = max(uParams3.y, 0.0);
    float spread = max(uParams3.z, 0.0);

    // LOD по дистанции: вблизи полное качество, вдали — вдвое дешевле. На
    // экране это почти не видно (перспектива + туман), а кадр разгружает
    // именно там, где пикселей больше всего.
    float lod = smoothstep(25.0, 140.0, pz);
    int rays  = int(max(uParams.x * (1.0 - 0.5 * lod), 4.0));
    int steps = int(max(uParams.y * (1.0 - 0.4 * lod), 6.0));
    float rays_f = float(rays);

    // Базис полусферы
    vec3 up = abs(N.y) < 0.95 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 T = normalize(cross(up, N));
    vec3 B = cross(N, T);

    // Старт луча смещён по нормали: без этого первые сэмплы попадают в саму
    // поверхность, «затеняют» её и весь кадр становится ровно-серым.
    float bias = 0.02 + 0.0015 * pz;
    vec3 O = P + N * bias;

    vec3 gi = vec3(0.0);
    float occ_near = 0.0;            // затенение в радиусе AO (контактное)
    float occ_far = 0.0;             // затенение в полном радиусе (крупное)
    float wsum = 0.0;                // сумма косинусных весов (нормировка)

    // Поворот всего набора лучей: по пикселю (IGN) + по кадру (фаза).
    float a0 = (ign(gl_FragCoord.xy) + phase * 0.618034) * 6.2831853;

    for (int r = 0; r < rays; r++) {
        float fr = (float(r) + 0.5) / rays_f;
        float ang = a0 + float(r) * 2.39996323;
        float rr = sqrt(fr);
        vec3 dir = normalize(T * (rr * cos(ang)) + B * (rr * sin(ang))
                             + N * sqrt(max(1.0 - fr, 0.0)));
        float ndl = max(dot(dir, N), 0.0);
        if (ndl <= 0.001) continue;
        wsum += ndl;

        // Джиттер стартовой фазы марша — убирает кольца бандинга.
        float jit = ign(gl_FragCoord.xy + vec2(float(r) * 23.0, phase * 7.0));

        float hd; vec2 huv; float hsd;
        if (trace_ray(O, dir, radius, steps, jit, bias, hd, huv, hsd)) {
            vec3 hp = wpos(huv, hsd);
            // Косинус на ПРИНИМАЮЩЕЙ стороне: поверхность, отвёрнутая от
            // луча, света не отдаёт. Это то, что делает непрямой свет
            // направленным, а не однородной подкраской. Смешиваем, а не
            // берём начисто — на силуэтах нормаль из глубины ненадёжна, и
            // жёсткий косинус выгрызал бы там дыры в освещении. На дальнем
            // LOD пропускаем: 2 лишние выборки на луч там не окупаются.
            float cosOut = 1.0;
            if (lod < 0.5)
                cosOut = mix(1.0, max(dot(wnormal_fast(huv, hp), -dir), 0.0), 0.7);

            // Одно диффузное переотражение с альбедо < 1 (см. hit_radiance):
            // закрытое направление ОБМЕНИВАЕТ свет неба на отражённый от
            // поверхности, но отдаёт СТРОГО МЕНЬШЕ, чем получила сама
            // поверхность. Цепочка переотражений (spread) уже внутри и
            // нормирована — суммарная энергия не растёт.
            gi += hit_radiance(huv, hp, spread, lod)
                * (ndl * cosOut / (1.0 + hd * 0.3));
            // Два масштаба затенения из ОДНОГО марша — лишних лучей не надо.
            occ_near += ndl * (1.0 - smoothstep(0.0, aoRadius, hd));
            occ_far  += ndl;
        } else {
            // Луч ушёл мимо геометрии кадра. Вверх — там небо, оно уже в
            // ambient'е сцены, не добавляем. Вниз — там земля за пределами
            // экрана: её отражённый свет иначе теряется, и низ объектов
            // выходит мёртво-плоским. Даём его цветом uGroundColor, тем же
            // альбедо, что и остальные переотражения.
            gi += uGroundColor
                * (ndl * max(-dir.y, 0.0) * skyw * 0.5 * GI_ALBEDO);
        }
    }

    // Нормировка на фактическую сумму косинусных весов + насыщение оттенка:
    // отражённый свет должен нести цвет источника (зелень травы, тепло
    // кирпича) — именно это читается как «рефлексы света». Насыщение сохраняет
    // яркость (см. saturate_hue), поэтому энергию не добавляет.
    float inv = 1.0 / max(wsum, 1e-4);
    vec3 col = saturate_hue(sane3(gi * inv), bounce);

    // --- Зеркальный отблеск -------------------------------------------------
    // Один луч по отражению взгляда. В отличие от SSR работает на любых
    // поверхностях (SSR включается только по мокрости) и стоит один луч.
    // На дальнем LOD выключен: там отблеск не читается.
    // ЭНЕРГИЯ: множитель = reflw (<=1) * френель (<=0.95) * альбедо внутри
    // hit_radiance — отблеск не может пересветить отражаемую поверхность.
    // Шагов 3/4 от диффузного: отблеск бьёт вдоль поверхности и упирается
    // быстро, разницы не видно.
    if (reflw > 0.001 && lod < 0.7) {
        vec3 V = normalize(P - uCamPos);
        vec3 R = reflect(V, N);
        if (dot(R, N) > 0.05) {
            float jitr = ign(gl_FragCoord.xy + vec2(91.0, phase * 13.0));
            int rsteps = int(max(float(steps) * 0.75, 5.0));
            float hd; vec2 huv; float hsd;
            if (trace_ray(O, R, radius, rsteps, jitr, bias, hd, huv, hsd)) {
                // Отблеск участвует в распространении на общих правах:
                // отражение видит и прямой свет поверхности, и накопленный
                // на ней непрямой — но в сумме не больше, чем там есть.
                vec3 rc = hit_radiance(huv, wpos(huv, hsd), spread, lod);
                // Френель: вдоль поверхности отблеск сильнее, в упор слабее.
                // Максимум 0.95 — отражение всегда темнее отражаемого.
                float fres = pow(1.0 - max(dot(-V, N), 0.0), 3.0);
                col += rc * (reflw * (0.25 + 0.70 * fres) / (1.0 + hd * 0.12));
            }
        }
    }
    // Итоговый потолок — ровно uClamp (был uClamp * 3). Непрямой свет не
    // ярче потолка энергии, который задаёт r_gi_clamp.
    col = min(col, vec3(max(uClamp, 0.0)));

    // Затенение: контактное × крупномасштабное (закрытость небосвода, skyw).
    // Второй множитель даёт «игру света» — ниши и подкронье темнее открытых
    // мест. Глубина УМЕРЕНА (было 0.9/0.8 при поле 0.06 — сцена проваливалась
    // в чёрный): контактное до 0.62, дальнее до 0.5, пол 0.25. Дальнее
    // берётся с корнем — насыщается плавно, а не обрывается в тень при первых
    // же попаданиях. В траве и листве буфер глубины завышает закрытость,
    // поэтому потолки принципиально ниже единицы.
    float near_f = clamp(occ_near * inv, 0.0, 1.0) * 0.62;
    float far_f  = sqrt(clamp(occ_far * inv, 0.0, 1.0)) * 0.5;
    float ao = clamp((1.0 - near_f) * (1.0 - far_f * skyw), 0.25, 1.0);

    // --- Темпоральное накопление -------------------------------------------
    vec4 res = vec4(col, ao);
    if (uBlend > 0.001) {
        vec4 pc = uPrevViewProj * vec4(P, 1.0);
        if (pc.w > 0.02) {
            vec2 puv = (pc.xyz / pc.w).xy * 0.5 + 0.5;
            if (puv.x > 0.002 && puv.x < 0.998 && puv.y > 0.002 && puv.y < 0.998) {
                vec4 prev = texture(uPrev, puv);
                prev = vec4(sane3(prev.rgb), sane1(prev.a));
                float pd = texture(uDepth, puv).r;
                float ok = 0.0;
                if (pd < 0.9999 && pd > 1e-5) {
                    float pzz = linz(pd * 2.0 - 1.0);
                    ok = 1.0 - smoothstep(0.05 * pz + 0.10, 0.30 * pz + 0.6,
                                          abs(pzz - pz));
                }
                // Кламп истории вокруг текущей оценки. Верх опущен (2.5
                // вместо 5.0): широкое окно оставляло каналу накопления
                // лазейку для роста яркости — история подтягивала пиксель
                // вверх, следующий кадр брал её за основу, и свечение
                // копилось. Шум всё ещё усредняется, но грубое несовпадение
                // (дизокклюзия) не тянет шлейф чужой поверхности.
                prev.rgb = clamp(prev.rgb, res.rgb * 0.2 - 0.1,
                                 res.rgb * 2.5 + 0.05);
                prev.a = clamp(prev.a, res.a - 0.35, res.a + 0.35);
                res = mix(res, prev, uBlend * ok);
            }
        }
    }
    FragColor = vec4(sane3(res.rgb), sane1(res.a));
}
