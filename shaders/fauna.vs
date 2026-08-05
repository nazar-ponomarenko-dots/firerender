// ============================================================================
//  Fauna — вершинный шейдер частиц (светлячки/бабочки/птицы)
// ----------------------------------------------------------------------------
//  Квад центрирован (x -0.5..0.5, y -0.5..0.5), ось ширины uObjRight —
//  перпендикуляр к камере (биллборд). uMode в FS решает тип отрисовки.
// ============================================================================

layout (location = 0) in vec2 aPos;    // локально: x -0.5..0.5, y -0.5..0.5
layout (location = 1) in vec2 aUV;

uniform mat4 uViewProj;
uniform vec3 uObjPos;
uniform vec2 uObjSize;
uniform vec3 uObjRight;
uniform float uTime;

out vec2 vUV;
out vec3 vWorldPos;

void main() {
    vec3 wp = uObjPos
            + uObjRight * (aPos.x * uObjSize.x)
            + vec3(0.0, 1.0, 0.0) * (aPos.y * uObjSize.y);
    vUV = vec2(aUV.x, 1.0 - aUV.y);
    vWorldPos = wp;
    gl_Position = uViewProj * vec4(wp, 1.0);
}
