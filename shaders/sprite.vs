// ============================================================================
//  Sprite3D — вершинный шейдер спрайта, прицепляемого к любому узлу/косточке
// ----------------------------------------------------------------------------
//  Один квад (x,y в -0.5..0.5). Два режима:
//    uBillboard = 1 — квад разворачивается к камере (базис uCamRight/uCamUp),
//                     центр берётся из МИРОВОЙ матрицы узла (uModel);
//    uBillboard = 0 — плоский квад в системе узла: полностью подчиняется
//                     матрице (в т.ч. повороту кости, к которой прицеплен).
//  Размер задаётся в метрах (uSize) и умножается на масштаб узла.
//  UV: кадр из атласа (uUvTile/uUvOffset) + прокрутка (uUvScroll * uTime).
// ============================================================================

layout (location = 0) in vec2 aPos;   // квад: -0.5..0.5
layout (location = 1) in vec2 aUV;    // 0..1

uniform mat4 uViewProj;
uniform mat4 uModel;          // мировая матрица узла (scene.c)
uniform vec2 uSize;           // ширина/высота, м
uniform float uBillboard;     // 1 = лицом к камере
uniform vec3 uCamRight;
uniform vec3 uCamUp;
uniform vec2 uUvTile;
uniform vec2 uUvOffset;
uniform vec2 uUvScroll;
uniform float uTime;

out vec2 vUV;
out vec3 vWorldPos;
out vec3 vNormal;

void main() {
    // Масштаб узла: длины столбцов модельной матрицы (узел мог быть
    // прицеплен к костям с масштабом — спрайт должен ехать вместе с ними).
    float sx = length(uModel[0].xyz);
    float sy = length(uModel[1].xyz);
    vec3 center = uModel[3].xyz;
    vec3 wp;
    vec3 nrm;
    if (uBillboard > 0.5) {
        wp = center + uCamRight * (aPos.x * uSize.x * sx)
                    + uCamUp    * (aPos.y * uSize.y * sy);
        nrm = normalize(cross(uCamRight, uCamUp));
    } else {
        // Плоский квад в системе узла: ось X — вправо, Y — вверх узла.
        vec3 right = uModel[0].xyz / max(sx, 1e-5);
        vec3 up    = uModel[1].xyz / max(sy, 1e-5);
        wp = center + right * (aPos.x * uSize.x * sx)
                    + up    * (aPos.y * uSize.y * sy);
        nrm = normalize(cross(right, up));
    }
    vUV = aUV * uUvTile + uUvOffset + uUvScroll * uTime;
    vWorldPos = wp;
    vNormal = nrm;
    gl_Position = uViewProj * vec4(wp, 1.0);
}
