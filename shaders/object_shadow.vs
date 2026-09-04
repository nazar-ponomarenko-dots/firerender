// ============================================================================
//  ObjectShadow — вершинный шейдер объектов в карту теней
// ----------------------------------------------------------------------------
//  Тот же крест из двух статичных квадов, что и в обычном рендере —
//  только позиция и UV (альфа-тест делает фрагментный шейдер). Основание
//  так же ложится по уклону земли (uObjSlope), иначе тень отрывалась бы от
//  дерева на склоне.
// ============================================================================

layout (location = 0) in vec2 aPos;    // локально: x -0.5..0.5, y 0..1
layout (location = 1) in vec2 aUV;

uniform mat4 uViewProj;
uniform vec3 uObjPos;
uniform vec2 uObjSize;
uniform vec3 uObjRight;
uniform vec2 uObjSlope;     // уклон земли под объектом (dh/dx, dh/dz)

out vec2 vUV;

void main() {
    float hx = aPos.x * uObjSize.x;
    vec3 wp = uObjPos + uObjRight * hx
            + vec3(0.0, 1.0, 0.0) * (aPos.y * uObjSize.y);
    wp.y += (uObjSlope.x * uObjRight.x + uObjSlope.y * uObjRight.z) * hx;
    vUV = vec2(aUV.x, 1.0 - aUV.y);
    gl_Position = uViewProj * vec4(wp, 1.0);
}
