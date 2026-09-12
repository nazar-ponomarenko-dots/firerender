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

float sample_height16(vec2 uv) {
    vec2 h = texture(uHeightmap, uv).rg;
#ifdef FR_HEIGHT_RG8
    return (h.r * 65280.0 + h.g * 255.0) / 65535.0;
#else
    return h.r;
#endif
}

void main() {
    float t = sample_height16(aUV);
    float h = mix(uHeightMin, uHeightMax, t);
    gl_Position = uViewProj * vec4(aPos.x, h, aPos.y, 1.0);
}
