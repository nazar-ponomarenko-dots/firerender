// ============================================================================
//  veg_cull.cs — GPU-driven отсечение растительности (compute pass)
// ----------------------------------------------------------------------------
//  Один инвариант на инстанс исходного буфера (Src). Для каждого растения:
//   1) отсечение по фрустуму СФЕРОЙ, описанной вокруг всего спрайта (а не
//      точкой основания!) — иначе травинка исчезала целиком, едва её база
//      уходила за кромку экрана, хотя сам спрайт ещё виден;
//   2) оценка экранного «следа»: если высота спрайта в пикселях меньше
//      uMinPix — растение меньше пикселя, его не видно, отбрасываем.
//  Выжившие КОМПАКТНО записываются в Dst (по 12 float, как в VBO), а их
//  число копится атомарным счётчиком прямо в поле instanceCount команды
//  DrawElementsIndirect (Indirect). Итог: число отрисованных вершин
//  ограничено ВИДИМЫМИ пикселями, а не плотностью.
//
//  Радиус сферы считается по ТОЙ ЖЕ логике, что растит квад wheat.vs:
//  пшеница вытянута втрое по высоте, квад укорочен до непрозрачной части
//  картинки, а на дальних LOD спрайт РАСТЯГИВАЕТСЯ по ширине до x10
//  (wx 128 -> 1280). Плюс запас на ветер и прогиб под игроком.
//  Консервативно: лучше нарисовать лишнее, чем получить дырку.
//
//  Формат инстанса (12 float, как в wheat.vs / veg_gen_chunk):
//   [0..2] pos.xyz, [3..4] normal nx,nz, [5] yaw, [6] scale, [7] tint,
//   [8] layer (>=3.5 => пшеница, x3 высота), [9] lean, [10..11] slope.
// ============================================================================

layout(local_size_x = 64) in;

// Исходные инстансы (только чтение). src[] — плоский массив 12 float/инстанс.
layout(std430, binding = 0) readonly buffer Src {
    float src[];
};
// Компактные выжившие (запись). Тот же формат, что у инстансного VBO.
layout(std430, binding = 1) writeonly buffer Dst {
    float dst[];
};
// Команда indirect-отрисовки. instanceCount (offset 4) — атомарный счётчик.
layout(std430, binding = 2) buffer Indirect {
    uint cmd_count;          // = 6 (индексов на квад)
    uint cmd_instanceCount;  // атомарно растёт -> сколько рисовать
    uint cmd_firstIndex;     // = 0
    uint cmd_baseVertex;     // = 0
    uint cmd_baseInstance;   // = 0
};

uniform mat4  uViewProj;     // вид * проекция (для оценки экранного следа)
uniform vec4  uPlanes[6];    // плоскости фрустума (мир, нормали внутрь)
uniform vec3  uCamPos;       // камера (LOD пшеницы = как в wheat.vs)
uniform float uViewH;        // высота вьюпорта в пикселях (для оценки px)
uniform float uTanHalfFov;   // tan(fov/2): мировые метры -> пиксели
uniform float uMinPix;       // мин. видимый след травинки (px), обычно 1.0
uniform int   uSrcCount;     // число инстансов В ЭТОМ вызове (поддиапазон)
uniform int   uOffset;       // базовый индекс в Src (для поддиапазона супер-чанка)

void main()
{
    uint gid = gl_GlobalInvocationID.x;
    if (int(gid) >= uSrcCount) return;
    uint i = uint(uOffset) + gid;     // абсолютный индекс в Src

    // Базовый адрес инстанса в плоском массиве (12 float).
    uint b = i * 12u;
    vec3 pos = vec3(src[b], src[b + 1u], src[b + 2u]);
    float scale = src[b + 6u];
    float layer = src[b + 8u];
    bool wheat = layer >= 3.5;

    // --- Габарит спрайта в мире (та же математика, что в wheat.vs) ----------
    // Полезная высота: квад укорочен до непрозрачной части картинки
    // (veg_content_frac в veg_common.glsl). Берём МАКСИМУМ доли по видам
    // (пшеница lod1 = 168/178 ≈ 0.944), чтобы сфера точно накрывала спрайт.
    float worldH = scale * (wheat ? 3.0 : 1.0) * 0.95;   // высота растения, м
    float wmul = 1.0;                                // растяжение ширины (LOD)
    if (wheat) {
        float dist = length(pos.xz - uCamPos.xz);
        float ss1 = smoothstep(4.6, 5.4, dist);
        float ss2 = smoothstep(24.5, 25.5, dist);
        float wx = mix(128.0, 152.0, ss1);
        wx = mix(wx, 1280.0, ss2);                   // lod2 растягивается x10
        wmul = wx / 128.0;
    }
    float halfW = 0.5 * scale * wmul;
    // Запас: ветер (~0.11 м) + прогиб под игроком. Прогиб теперь ПОВОРОТ на
    // угол <= BEND_MAX_A (0.8 рад) вокруг основания, а не сдвиг на метры:
    // максимум смещения = sin(0.8) * worldH ≈ 0.72 * worldH (было 1.4 * scale,
    // то есть больше самой травинки). Сфера стала теснее — меньше лишних
    // инстансов проходит отсечение.
    float pad = 0.15 + 0.72 * worldH;
    // Центр сферы — середина спрайта по высоте, радиус покрывает квад целиком.
    vec3 center = pos + vec3(0.0, worldH * 0.5, 0.0);
    float radius = sqrt(halfW * halfW + worldH * worldH * 0.25) + pad;

    // --- Отсечение фрустумом СФЕРОЙ ----------------------------------------
    for (int p = 0; p < 6; p++) {
        if (dot(uPlanes[p].xyz, center) + uPlanes[p].w < -radius) return;
    }

    // --- Экранный след спрайта ---------------------------------------------
    // px = worldH / (tan(fov/2) * (-z_view)) * (viewH/2); -z_view ~ clip.w.
    vec4 clip = uViewProj * vec4(center, 1.0);
    float wz = max(clip.w, 1e-3);
    float px = max(worldH, halfW * 2.0) * (uViewH * 0.5)
             / max(wz * uTanHalfFov, 1e-3);
    if (px < uMinPix) return;                  // субпиксельно — не рисуем

    // --- Выжил: компактно пишем и атомарно считаем ---------------------------
    uint idx = atomicAdd(cmd_instanceCount, 1u);
    uint o = idx * 12u;
    for (uint k = 0u; k < 12u; k++)
        dst[o + k] = src[b + k];
}
