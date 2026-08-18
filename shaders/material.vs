// ============================================================================
//  Material — вершинный шейдер материальной системы (см. matprog.c)
// ----------------------------------------------------------------------------
//  Как mesh.vs, плюс UV-анимация материала (uUvOffset/uUvScroll — материал
//  шлёт их через material_bind). Файл используется матсистемой как база для
//  ВСЕХ пермутаций: фрагментные фичи (блендинг/спекуляр/мокрость/эмиссия)
//  компилируются #define'ами в material.fs, вершинный путь общий.
// ============================================================================

layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec2 aUV;

uniform mat4 uViewProj;
uniform mat4 uModel;           // перенос + масштаб объекта
uniform vec2 uUvTile;          // повтор UV (масштабирование)
uniform vec2 uUvOffset;        // сдвиг UV (тайлы)
uniform vec2 uUvScroll;        // скорость UV-анимации (тайлов/сек)
uniform float uTime;           // время (для scroll)

out vec3 vWorldPos;
out vec3 vNormal;
out vec2 vUV;

void main() {
    vec4 wp = uModel * vec4(aPos, 1.0);
    vWorldPos = wp.xyz;
    vNormal = normalize(mat3(uModel) * aNormal);
    vUV = aUV * uUvTile + uUvOffset + uUvScroll * uTime;
    gl_Position = uViewProj * wp;
}
