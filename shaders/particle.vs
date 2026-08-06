// particle.vs — instanced-спрайт частицы, биллборд к камере. Квад в -0.5..0.5,
// инстансы: attr1: pos(xyz), attr2: size(xy), attr3: color(rgb), attr4: alpha.
// Режим блендинга (альфа/аддитив) выбирается по draw call (см. particles.c).
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec3 aInstPos;
layout(location = 2) in vec2 aInstSize;
layout(location = 3) in vec3 aInstColor;
layout(location = 4) in float aInstAlpha;

uniform mat4 uViewProj;
uniform vec3 uCamRight;
uniform vec3 uCamUp;

out vec2 vUV;
out vec3 vColor;
out float vAlpha;

void main()
{
    vUV = aPos + vec2(0.5, 0.5);
    vColor = aInstColor;
    vAlpha = aInstAlpha;
    vec3 world = aInstPos
               + uCamRight * (aPos.x * aInstSize.x)
               + uCamUp    * (aPos.y * aInstSize.y);
    gl_Position = uViewProj * vec4(world, 1.0);
}
