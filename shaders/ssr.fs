// ============================================================================
//  SSR — Screen Space Reflections (отражения в экранном пространстве)
// ----------------------------------------------------------------------------
//  Пост-проход после сцены: читает цвет и глубину сцены, реконструирует
//  мировую позицию и нормаль (из глубины соседей), отражает луч взгляда и
//  маршит в экранном пространстве. Шаг растёт с дистанцией (быстро уходя
//  вдаль), после находки — бинарное уточнение хита (без "сползания").
//  Промах = кубомап неба (uCube) — мокрые поверхности отражают небо даже
//  там, где экран пуст. Всё гейтится мокротой uWetness (дождь), ориентацией
//  вверх, френелем и маской мокрости рельефа (под кронами — сухо).
//
//  Выдаёт СЦЕНУ + ОТРАЖЕНИЕ (в свой FBO), дальше по пайплайну как обычно
//  (bloom -> туман).
// ============================================================================

in vec2 vUV;

uniform sampler2D uScene;       // цвет сцены (unit 0)
uniform sampler2D uDepth;       // глубина сцены (unit 1)
uniform samplerCube uCube;      // кубомап неба (unit 2) — фолбэк отражения
uniform sampler2D uWetMask;     // маска мокрости рельефа (unit 3)
uniform sampler2D uPrev;        // сцена+отражение прошлого кадра (unit 4, антимерцание)
uniform mat4 uViewProj;         // мир -> экран (проекция отражённого луча)
uniform mat4 uPrevViewProj;     // мир -> экран прошлого кадра (репроекция uPrev)
uniform mat4 uInvViewProj;      // экран -> мир (позиции и нормали)
uniform vec3 uCamPos;
uniform vec2 uTexel;            // 1 / разрешение экрана
uniform vec2 uWorldSize;        // размер мира (м): uv мокрость-маски
uniform vec4 uParams;           // x шаги, y дальность, z толщина, w шероховатость
uniform vec4 uMisc;             // x znear, y zfar, z фолбэк вкл, w маска вкл
uniform float uDebug;           // 0 выкл, 1 гейт, 2 хит/фолбэк, 3 вес, 4 цвет отражения
uniform float uWetness;         // мокрость 0..1 (гейт всего эффекта)
uniform float uStrength;        // сила отражения (0..1)
uniform float uBlend;           // темпоральный вес прошлого кадра (0 = выкл)

out vec4 FragColor;

// Мировая позиция по uv и глубинному пикселю (приём volumetric.fs)
vec3 wpos(vec2 uv, float depth) {
    vec4 ndc = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 wp = uInvViewProj * ndc;
    return wp.xyz / wp.w;
}

// Линейная (view-space) глубина из NDC-z: монотонна и в метрах —
// сравнивать луч и поверхность надёжнее, чем в сыром NDC.
float linz(float ndc_z) {
    float zn = uMisc.x, zf = uMisc.y;
    return (2.0 * zn * zf) / (zf + zn - ndc_z * (zf - zn));
}

// Нормаль из глубины: два касательных вектора к соседям. Берём ТОГО
// соседа, чья глубина ближе (по каждой оси) — на границах силуэтов
// нормаль не "разъезжается" streak'ами.
vec3 wnormal(vec2 uv, float d0, vec3 p0) {
    float dR = texture(uDepth, uv + vec2(uTexel.x, 0.0)).r;
    float dL = texture(uDepth, uv - vec2(uTexel.x, 0.0)).r;
    vec2 offx = (abs(dR - d0) < abs(dL - d0)) ? vec2(uTexel.x, 0.0) : vec2(-uTexel.x, 0.0);
    float dx = (abs(dR - d0) < abs(dL - d0)) ? dR : dL;
    float dU = texture(uDepth, uv + vec2(0.0, uTexel.y)).r;
    float dD = texture(uDepth, uv - vec2(0.0, uTexel.y)).r;
    vec2 offy = (abs(dU - d0) < abs(dD - d0)) ? vec2(0.0, uTexel.y) : vec2(0.0, -uTexel.y);
    float dy = (abs(dU - d0) < abs(dD - d0)) ? dU : dD;
    vec3 px = wpos(uv + offx, dx);
    vec3 py = wpos(uv + offy, dy);
    vec3 n = cross(px - p0, py - p0);
    float l = length(n);
    return l > 1e-6 ? n / l : vec3(0.0, 1.0, 0.0);
}

// Проекция точки луча (t — дистанция вдоль луча от начала). ok = 0, если
// точка за камерой или NDC вне фрустума. rayZ — линейная глубина точки.
vec2 marchUV(vec3 ro, vec3 rd, float t, out float rayZ, out float ok) {
    vec4 clip = uViewProj * vec4(ro + rd * t, 1.0);
    if (clip.w <= 0.02) { ok = 0.0; rayZ = 0.0; return vec2(0.0); }
    vec3 ndc = clip.xyz / clip.w;
    rayZ = linz(ndc.z);
    vec2 uv = ndc.xy * 0.5 + 0.5;
    ok = (abs(ndc.z) < 1.0 && uv.x > 0.0 && uv.x < 1.0 && uv.y > 0.0 && uv.y < 1.0)
         ? 1.0 : 0.0;
    return uv;
}

// Цвет отражения: при малой шероховатости ОДИН резкий тап (без блюра —
// трава читается в отражении), блюр появляется только на rough заметно выше.
vec3 blurCol(vec2 uv, float rough) {
    vec3 c = texture(uScene, uv).rgb;
    if (rough <= 0.10) return c;
    vec2 o = uTexel * rough * 5.0;
    c += texture(uScene, uv + vec2( o.x, 0.0)).rgb;
    c += texture(uScene, uv + vec2(-o.x, 0.0)).rgb;
    c += texture(uScene, uv + vec2(0.0,  o.y)).rgb;
    c += texture(uScene, uv + vec2(0.0, -o.y)).rgb;
    return c * 0.2;
}

void main() {
    vec3 scene = texture(uScene, vUV).rgb;
    float d0 = texture(uDepth, vUV).r;
    // Небо (depth ~ 1: скайбокс не пишет глубину) — отражать нечего
    if (d0 > 0.9999) {
        FragColor = vec4(mix(scene, vec3(0.0), step(0.5, uDebug)), 1.0);
        return;
    }

    vec3 P = wpos(vUV, d0);
    vec3 N = wnormal(vUV, d0, P);
    vec3 V = normalize(uCamPos - P);
    if (dot(N, V) < 0.0) N = -N;
    vec3 R = reflect(-V, N);

    // Мокрость: общая + маска рельефа (под кронами деревьев сухо)
    float wet = clamp(uWetness, 0.0, 1.0);
    if (uMisc.w > 0.5)
        wet *= smoothstep(0.2, 0.8, texture(uWetMask, P.xz / uWorldSize).r);

    // Гейты: поверхность смотрит вверх, луч ВЫХОДИТ из неё (не само-в-себя),
    // френель (к горизонту зеркальнее), края экрана гаснут.
    float up = smoothstep(0.15, 0.70, N.y);
    float outS = smoothstep(0.0, 0.20, dot(R, N));
    // Френель с ПОЛОМ 0.35 — мокрая поверхность заметно отражает даже под
    // прямым углом, а не только у горизонта (старый pow5 давал ~9%).
    float fres = mix(0.35, 1.0, pow(1.0 - clamp(dot(N, V), 0.0, 1.0), 5.0));
    float ef = smoothstep(0.0, 0.05, vUV.x) * smoothstep(1.0, 0.95, vUV.x)
             * smoothstep(0.0, 0.05, vUV.y) * smoothstep(1.0, 0.95, vUV.y);
    float gate = up * outS * ef * wet * uStrength;
    bool live = wet > 0.02 && gate > 0.003;
    if (!live) {
        if (uDebug > 0.5 && uDebug < 1.5) FragColor = vec4(vec3(0.0, gate, 0.0), 1.0);
        else FragColor = vec4(scene, 1.0);
        return;
    }

    // Марш в МИРОВОМ ПРОСТРАНСТВЕ с тестом пересечения: отражённый луч
    // стартует на поверхности и летит вверх-вдаль (dot(R,N) > 0 — гейт outS
    // это гарантирует). Хит — когда луч переходит ИЗ-НАД поверхности ПОД неё
    // (was above, now below) в мировой Y, затем бинарное уточнение —
    // отражение перевёрнуто по высоте и садится на силуэт, а не «сползает».
    // Защита от ложных хитов дальних объектов (лес за 500 м в луже у ног):
    // перспективное сжатие проецирует далёкое дерево на тот же uv, где луч
    // всего в нескольких метрах от P; поэтому хит принимается, только если
    // поверхность в точке марша лежит РЯДОМ с текущей позицией луча.
    // minPix (экранные пиксели) дополнительно отсекает самый ближний
    // самохит у источника.
    int steps = int(uParams.x);
    float maxd = uParams.y;
    float thick0 = uParams.z;
    float rough = uParams.w;
    float minPix = 3.0;

    float tPrev = 0.0;
    float t = 0.20;
    bool wasAbove = true;
    vec2 uvHit = vec2(-1.0);
    float fade = 0.0;
    float hitT = 0.0;
    for (int i = 0; i < steps; i++) {
        float ok, rayZ;
        vec2 suv = marchUV(P, R, t, rayZ, ok);
        if (ok < 0.5) break;                       // за экраном/камерой — промах
        float sd = texture(uDepth, suv).r;
        bool above;
        if (sd > 0.9999) {                          // небо: пусто, луч летит дальше
            above = true;
        } else {
            vec3 surf = wpos(suv, sd);
            vec3 rp = P + R * t;                    // мировая точка луча
            bool below = rp.y < surf.y - thick0 * 0.25;
            vec2 farPix = abs(suv - vUV) / uTexel;
            bool farOk = max(farPix.x, farPix.y) > minPix;
            // Поверхность должна быть рядом с лучом (не лесом за 500 м,
            // спроецированным на этот uv).
            bool nearOk = distance(surf, P) < max(4.0, t * 2.5);
            if (wasAbove && below && farOk && nearOk) {
                // Пересечение: бинарное уточнение в мировой Y (тем же тестом).
                float a = tPrev, b = t;
                vec2 buv = suv;
                for (int r2 = 0; r2 < 5; r2++) {
                    float m = (a + b) * 0.5;
                    float ok2, rz2;
                    vec2 muv = marchUV(P, R, m, rz2, ok2);
                    bool mBelow = false;
                    if (ok2 > 0.5) {
                        float sd2 = texture(uDepth, muv).r;
                        if (sd2 < 0.9999) {
                            vec3 s2 = wpos(muv, sd2);
                            mBelow = (P + R * m).y < s2.y - thick0 * 0.25
                                  && distance(s2, P) < max(4.0, m * 2.5);
                        }
                    }
                    if (mBelow) { b = m; buv = muv; } else { a = m; }
                }
                uvHit = buv;
                hitT = b;
                break;
            }
            above = !below;
        }
        wasAbove = above;
        tPrev = t;
        t += mix(0.20, 2.5, clamp(t / maxd, 0.0, 1.0)) * (1.0 + rough * 0.5);
    }

    vec3 refl;
    float mixw;
    if (uvHit.x >= 0.0) {
        // Хит в экране: резкий цвет сцены (один тап — трава читается в
        // отражении; блюр только на большой шероховатости) + фейды.
        refl = blurCol(uvHit, rough);
        float fade = 1.0 - smoothstep(maxd * 0.55, maxd, hitT);
        mixw = gate * (0.70 + 0.30 * fres) * fade * (1.0 - rough * 0.30);
    } else if (uMisc.z > 0.5) {
        // Промах: небо/окружение из кубомапа — лужа не чернеет, когда
        // отражаемого в экране нет (как вода через uReflCube).
        refl = texture(uCube, R).rgb;
        mixw = gate * (0.10 + 0.60 * fres);
    } else {
        FragColor = vec4(scene, 1.0);
        return;
    }

    float mixw_clamped = clamp(mixw, 0.0, 1.0);
    // Отладка: красим зоны вместо смешивания, чтобы видеть где SSR живёт
    if (uDebug > 0.5) {
        if (uDebug < 1.5) {
            // 1: сила гейта gate (зелёный) — где SSR вообще может рисовать
            FragColor = vec4(vec3(0.0, gate, 0.0), 1.0);
        } else if (uDebug < 2.5) {
            // 2: хит=красный, фолбэк(кубомап)=синий, ни то ни другое=чёрный
            vec3 dbg = uvHit.x >= 0.0 ? vec3(1.0, 0.0, 0.0)
                     : (uMisc.z > 0.5 ? vec3(0.0, 0.0, 1.0) : vec3(0.0));
            FragColor = vec4(dbg, 1.0);
        } else if (uDebug < 3.5) {
            // 3: итоговый вес отражения mixw (белый) — насколько будет видно
            FragColor = vec4(vec3(mixw_clamped), 1.0);
        } else {
            // 4: чистый цвет отражения — проверка «зеркальности»
            FragColor = vec4(refl, 1.0);
        }
        return;
    }

    vec3 outc = mix(scene, refl, mixw_clamped);

    // Темпоральное сглаживание (антимерцание): пересобираем мировую точку
    // пикселя и репроектируем её в прошлый кадр (uPrevViewProj). Там берём
    // прошлый кадр (сцена+отражение) и подмешиваем с весом uBlend. Репроджект
    // в мировой точке устойчив к мелкому дрожанию камеры/луча; если uv ушёл за
    // экран или история ещё не готова (uBlend=0) — просто текущий кадр.
    if (uBlend > 0.001) {
        vec4 pc = uPrevViewProj * vec4(P, 1.0);
        if (pc.w > 0.02) {
            vec2 puv = pc.xy / pc.w * 0.5 + 0.5;
            if (puv.x > 0.003 && puv.x < 0.997 && puv.y > 0.003 && puv.y < 0.997) {
                vec3 prev = texture(uPrev, puv).rgb;
                outc = mix(outc, prev, uBlend);
            }
        }
    }

    FragColor = vec4(outc, 1.0);
}
