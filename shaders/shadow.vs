// ============================================================================
//  Shadow — вершинный шейдер карты теней (земля)
// ----------------------------------------------------------------------------
//  Тот же рельеф, что в terrain.vs, но позиция сразу в пространстве
//  источника света (uViewProj = proj * view от солнца).
// ============================================================================

layout (location = 0) in vec2 aPos;   // x, z
layout (location = 1) in vec2 aUV;    // координата в карте высот

uniform mat4 uViewProj;               // вид * проекция ОТ СОЛНЦА
uniform sampler2D uHeightmap;
uniform float uHeightMin;
uniform float uHeightMax;

void main() {
    float t = texture(uHeightmap, aUV).r;
    float h = mix(uHeightMin, uHeightMax, t);
    gl_Position = uViewProj * vec4(aPos.x, h, aPos.y, 1.0);
}
