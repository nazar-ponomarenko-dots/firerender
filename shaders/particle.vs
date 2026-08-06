// particle.vs — instanced-спрайт частицы, биллборд к камере. Квад в -0.5..0.5,
// инстансы: attr1: pos(xyz), attr2: size(xy), attr3: color(rgb), attr4: alpha,
// attr5: vel(xyz). Если vel != 0 — это стрик (дождь/капли): квад растягивается
// вдоль направления скорости на длину size.y. Режим блендинга (альфа/аддитив)
// выбирается по draw call (см. particles.c).
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec3 aInstPos;
layout(location = 2) in vec2 aInstSize;
layout(location = 3) in vec3 aInstColor;
layout(location = 4) in float aInstAlpha;
layout(location = 5) in vec3 aInstVel;

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
    vec3 world;
    if (length(aInstVel) > 0.01) {
        // стрик: вдоль скорости (в мировых координатах), ширина по биллборду
        vec3 v = normalize(aInstVel);
        world = aInstPos + v * (aPos.y * aInstSize.y)
               + uCamRight * (aPos.x * aInstSize.x);
        // v-координата UV для стрика: 0 у "хвоста", 1 у "головы"
        vUV = vec2(aPos.x + 0.5, aPos.y + 0.5);
    } else {
        world = aInstPos
               + uCamRight * (aPos.x * aInstSize.x)
               + uCamUp    * (aPos.y * aInstSize.y);
    }
    gl_Position = uViewProj * vec4(world, 1.0);
}
