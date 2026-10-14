// ============================================================================
//  Terrain — вершинный шейдер рельефа
// ----------------------------------------------------------------------------
//  Загружается с префиксом "#version 330 core" (десктоп) или
//  "#version 300 es" (GLES) — версия подставляется из кода (см. main.c).
//  Задача: взять плоскую сетку (x, z + uv), вытащить высоту из текстуры
//  высот и поднять вершину. Мир плоский, всё смещение по Y.
//
//  Текстура высот (GL_R8, сглажена в C-коде + билинейная фильтрация):
//  в ней УЖЕ применена кривая высот с игровой полосой (см. height_curve
//  в main.c) — шейдеру остаётся только линейно растянуть её в метры.
//  Карта (цвет) от высоты отделена: цвет земли рисует фрагментный шейдер.
// ============================================================================

layout (location = 0) in vec2 aPos;   // x,z вершины на плоскости
layout (location = 1) in vec2 aUV;    // координаты в карте 0..1
layout (location = 2) in float aDrop; // юбка: на сколько метров опустить вершину
                                      // ниже высоты рельефа (0 = поверхность,
                                      // >0 = нижняя кромка юбки на стыках LOD)

uniform mat4 uViewProj;               // вид * проекция (из узла Camera)
uniform sampler2D uHeightmap;         // высоты рельефа (0 = низ, 1 = пик)
uniform float uHeightMin;             // высота, соответствующая 0 (-100 м)
uniform float uHeightMax;             // высота, соответствующая 1 (+400 м)

out vec2 vUV;
out vec3 vWorldPos;

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
    float h = mix(uHeightMin, uHeightMax, t) - aDrop;
    vec3 pos = vec3(aPos.x, h, aPos.y);
    vUV = aUV;
    vWorldPos = pos;
    gl_Position = uViewProj * vec4(pos, 1.0);
}