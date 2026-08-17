// ============================================================================
//  Water — вершинный шейдер воды: лужи/озёра/реки/моря (см. water.c)
// ----------------------------------------------------------------------------
//  Сетка плоская на уровне uLevel; волны поднимают вершины вершинным
//  шейдером по той же функции waveY, по которой фрагментный считает нормали
//  конечными разностями (гладкие при любой плотности сетки).
//
//  Атрибуты (water.c upload_geometry):
//    aPos  (0) vec2 — x,z вершины на плоскости уровня
//    aUV   (1) vec2 — координаты: мировая/4 (тайл волн); у реки x — вдоль русла
//    aEdge (2) float — 0 у берега, растёт внутрь; у открытого моря — 1e6 (края нет)
// ============================================================================

layout (location = 0) in vec2 aPos;
layout (location = 1) in vec2 aUV;
layout (location = 2) in float aEdge;

uniform mat4  uViewProj;      // вид * проекция (узел Camera)
uniform float uTime;          // время (растёт в water_node_update)
uniform float uLevel;         // уровень поверхности воды, м
uniform float uWaveAmp;       // амплитуда волны, м
uniform float uWaveSpeed;     // скорость волны (множитель времени)
uniform float uWaveLen;      // длина волны, м
uniform vec2  uFlowDir;       // направление течения (река), норм. вектор

out vec3 vWorldPos;
out vec2 vUV;
out float vEdge;

// Волновая функция — ОДНА на VS и FS (нормали в FS считают по ней же).
// Последний слагаемый — мелкая рябь (короткая волна, малая амплитуда).
float waveY(vec2 p, float t) {
    float k = 6.2831853 / uWaveLen;
    vec2 f = uFlowDir;
    if (dot(f, f) < 0.0001) f = vec2(1.0, 0.0);   // у моря/озера поток не задан
    f = normalize(f);
    vec2 q = vec2(-f.y, f.x);                     // поперёк течения
    return uWaveAmp * sin(dot(f, p) * k + t * uWaveSpeed)
         + uWaveAmp * 0.45 * sin(dot(q, p) * k * 0.62 + t * uWaveSpeed * 1.17)
         + uWaveAmp * 0.2  * sin((p.x + p.y) * k * 0.34 + t * uWaveSpeed * 0.8)
         + uWaveAmp * 0.12 * sin(dot(f, p) * k * 4.7 + dot(q, p) * k * 3.1
                                 + t * uWaveSpeed * 1.6);
}

void main() {
    vUV = aUV;
    vEdge = aEdge;
    float y = uLevel + waveY(aPos, uTime);
    vec3 world = vec3(aPos.x, y, aPos.y);
    vWorldPos = world;
    gl_Position = uViewProj * vec4(world, 1.0);
}