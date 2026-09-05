// ============================================================================
//  Mesh — вершинный шейдер статичного меша (объект из .obj, prop_static)
// ----------------------------------------------------------------------------
//  Настоящая модельная матрица + вершинные нормали: в отличие от биллбордов
//  объект освещается реально (диффуз по нормали).
// ============================================================================

layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec2 aUV;
layout (location = 3) in float aSnow;    // per-vertex "открыто небу сверху"

uniform mat4 uViewProj;
uniform mat4 uModel;           // перенос + масштаб объекта
uniform vec2 uUvTile;          // повтор UV (масштабирование)

out vec3 vWorldPos;
out vec3 vNormal;
out vec2 vUV;
out float vSnow;

void main() {
    vec4 wp = uModel * vec4(aPos, 1.0);
    vWorldPos = wp.xyz;
    vNormal = normalize(mat3(uModel) * aNormal);
    vUV = aUV * uUvTile;
    vSnow = aSnow;
    gl_Position = uViewProj * wp;
}