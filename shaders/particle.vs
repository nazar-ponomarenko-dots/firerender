// particle.vs — instanced-спрайт частицы, биллборд к камере. Квад в -0.5..0.5,
// инстансы: attr1: pos(xyz), attr2: size(xy), attr3: color(rgb), attr4: alpha,
// attr5: vel(xyz). Если vel != 0 — это стрик (дождь/капли): квад растягивается
// вдоль направления скорости на длину size.y.
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
out float vIsStreak;
out float vStreakLen;

void main()
{
    vUV = aPos + vec2(0.5, 0.5);
    vColor = aInstColor;
    vAlpha = aInstAlpha;
    vIsStreak = (length(aInstVel) > 0.01) ? 1.0 : 0.0;
    vec3 world;
    if (vIsStreak > 0.5) {
        vec3 v = normalize(aInstVel);
        float streakLen = aInstSize.y;
        vStreakLen = streakLen;
        world = aInstPos + v * (aPos.y * streakLen)
               + uCamRight * (aPos.x * aInstSize.x);
        // Для стрика: U = ширина биллборда, V = позиция вдоль стрика (0..1)
        vUV = vec2(aPos.x + 0.5, aPos.y + 0.5);
    } else {
        vStreakLen = 0.0;
        world = aInstPos
               + uCamRight * (aPos.x * aInstSize.x)
               + uCamUp    * (aPos.y * aInstSize.y);
    }
    gl_Position = uViewProj * vec4(world, 1.0);
}