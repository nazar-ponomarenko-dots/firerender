// ============================================================================
//  ShadowVeg — вершинный шейдер карты теней (растительность)
// ----------------------------------------------------------------------------
//  Те же экземпляры, что в wheat.vs, но позиция в пространстве света —
//  трава и колосья бросают настоящие тени на землю. Высоты и уклоны уже
//  в атрибутах экземпляра (без выборки текстуры) — теневой проход дешёвый.
//  Прореживание теперь делается на CPU (vegetation.c): динамика — по этому
//  же правилу uThin ((id*2654435761)%100<50), статика — каждый 2-й.
//  uThin всегда 0; ветка оставлена для совместимости.
//  Ветер тот же, что в wheat.vs, — тени не отрываются от растений.
// ============================================================================

layout (location = 0) in vec3 aPos;    // локальная точка (x, 0..1, z)
layout (location = 1) in vec2 aUV;     // uv спрайта
layout (location = 2) in vec3 iPos;    // экземпляр: позиция растения
layout (location = 3) in vec2 iN;      // экземпляр: нормаль земли (nx, nz)
layout (location = 4) in vec4 iAttr;   // экземпляр: yaw, масштаб, тон, слой
layout (location = 5) in float iLean;  // экземпляр: наклон вбок
layout (location = 6) in vec2 iSlope;  // экземпляр: уклон земли (dh/dx, dh/dz)

uniform mat4 uViewProj;                // вид * проекция ОТ СОЛНЦА
uniform int uThin;                     // 1 = прореживать (густая растительность)
uniform float uTime;                   // время симуляции (ветер)
uniform vec3 uCamPos;                  // игрок (для прогиба растительности)
uniform float uWalk;                   // 1 = ходьба (сильнее раздвигаем)
uniform float uDyn;                    // 1 = динамический слой (вокруг камеры)
uniform float uWindBoost;              // мокрость 0..1: в дождь ветер сильнее

out vec2 vUV;
flat out float vLayer;   // LOD целый для всего квада (не интерполировать)

// Виды (iAttr.w): 0/1/2 — три вида травы, 3 — одуванчик, 4 — пшеница
// (см. wheat.vs). Пшеница получает LOD 4..6, остальное — как есть.
#define WHEAT_BASE 4.0
#define WHEAT_MARK 3.5

void main() {
    // Густую растительность (70 шт/м2) в карте теней прореживаем на 40%:
    // плотные тени от пшеницы на пшенице должны быть цельными, а не
    // редкой сеткой из половин.
    if (uThin == 1 && (uint(gl_InstanceID) * 2654435761u) % 100u >= 50u) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);   // за пределами — отсечён
        return;
    }
    float ny = sqrt(max(1.0 - dot(iN, iN), 0.0));
    vec3 up = vec3(iN.x, ny, iN.y);
    float ca = cos(iAttr.x), sa = sin(iAttr.x);
    vec3 hvec = vec3(ca, 0.0, sa);
    // LOD пшеницы и ширина квада — как в wheat.vs (короткая полоса
    // crossfade 0.8 м, широкое плавное прореживание), тень повторяет
    // геометрию спрайта.
    float layer = iAttr.w;
    float wx = 128.0;
    if (layer > WHEAT_MARK) {
        float dist = length(iPos.xz - uCamPos.xz);
        float ss1 = smoothstep(4.6, 5.4, dist);
        float ss2 = smoothstep(24.5, 25.5, dist);
        // Динамический слой после lod1 не рисует (как в wheat.vs): иначе
        // lod2 виден дважды — плотное кольцо на краю динамики + статика
        // по всей карте. Тень должна совпадать с кадром.
        if (uDyn > 0.5 && ss2 > 0.5) {
            gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
            return;
        }
        layer = WHEAT_BASE + ss1 + ss2;   // непрерывный слой 4..5..6
        wx = mix(128.0, 152.0, ss1);
        // lod2: сразу максимальная ширина (x10) на всей дистанции — как
        // в wheat.vs, тень повторяет геометрию спрайта.
        if (uDyn < 0.5) wx = mix(wx, 1280.0, ss2);
        // ПРОРЕЖИВАНИЕ — как в wheat.vs: хэш позиции каждого растения
        // против keep (по шуму, без ячеек), те же пороги. Иначе в карте
        // теней остались бы растения, которых нет в кадре. lod1: 1 из 4;
        // lod2: 3 из 16 — одинаковая плотность на всей дистанции.
        float k1 = smoothstep(4.0, 8.0, dist);
        float k2 = smoothstep(22.0, 30.0, dist);
        float keep = 1.0;
        keep = mix(keep, 0.25, k1);       // lod1: 1 из 4
        keep = mix(keep, 0.25, k2);       // lod2: 1 из 4, везде одинакова
        float h = fract(sin(dot(iPos.xz + vec2(0.123, 0.456),
                                vec2(12.9898, 78.233))) * 43758.5453);
        if (h >= keep) {
            gl_Position = vec4(2.0, 2.0, 2.0, 1.0);   // за пределами — отсечён
            return;
        }
    }
    float wmul = wx / 128.0;
    // база на рельефе — как в wheat.vs (углы квада по уклону земли)
    float hw = aPos.x * iAttr.y * wmul;
    vec3 wp = iPos + hvec * hw;
    wp.y = iPos.y + (iSlope.x * hvec.x + iSlope.y * hvec.z) * hw;
    // пшеница (вид 4+) вытянута в высоту в 3 раза — как в wheat.vs
    float ht = iAttr.y * (iAttr.w > WHEAT_MARK ? 3.0 : 1.0);
    float lc = cos(iLean), ls = sin(iLean);
    vec3 lt = hvec * (aPos.y * ls * iAttr.y * wmul)
            + up  * (aPos.y * lc * ht);
    wp += lt;
    // тот же ветер и прогиб под игроком, что в wheat.vs, — тени не
    // отрываются от растений
    float ph = dot(iPos.xz, vec2(0.71, 0.39)) + iAttr.x * 2.5;
    float w1 = sin(uTime * 1.15 + ph);
    float w2 = sin(uTime * 2.35 + dot(iPos.xz * 1.7, vec2(0.53, -0.81))
                   + iAttr.w * 4.0);
    float amp = (0.7 + 0.6 * fract(iPos.x * 0.37 + iPos.z * 0.91))
              * (1.0 + 0.6 * uWindBoost);   // дождь = ветер +60% амплитуда
    float sway = (w1 * 0.07 + w2 * 0.035) * aPos.y * amp;
    wp.x += sway;
    wp.z += sway * 0.35 + w2 * 0.02 * aPos.y;
    vec2 toCam = iPos.xz - uCamPos.xz;
    float pd = length(toCam);
    float push = 1.0 - smoothstep(0.35, mix(1.0, 2.0, uWalk), pd);
    if (push > 0.0) {
        vec2 dir = toCam / max(pd, 0.0001);
        float bend = push * push * mix(0.85, 1.35, uWalk) * aPos.y * iAttr.y;
        wp.xz += dir * bend;
        wp.y -= push * push * mix(0.55, 0.85, uWalk) * aPos.y * iAttr.y;
    }
    vUV = aUV;
    // Холст 256x178, спрайты слева сверху. Кусок берём по текущей wx;
    // слой для альфа-теста в shadowveg.fs отсекается по GL_NEAREST —
    // переключение силуэта происходит в середине полосы, где в кадре
    // спрайт уже смешан наполовину.
    if (layer < WHEAT_MARK)  vUV *= vec2(128.0 / 256.0, 128.0 / 178.0);
    else                     vUV *= vec2(wx / 256.0, 1.0);
    vLayer = layer;
    gl_Position = uViewProj * vec4(wp, 1.0);
}
