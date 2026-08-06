// decal.vs — квад на рельефе: вершины уже трансформированы в мире (CPU:
// высоты углов из карты). uViewProj — вид*проекция.
layout(location = 0) in vec3 aPos;   // x, y, z (уже с высотой рельефа)
layout(location = 1) in vec2 aUV;

uniform mat4 uViewProj;

out vec2 vUV;

void main()
{
    vUV = aUV;
    gl_Position = uViewProj * vec4(aPos, 1.0);
}