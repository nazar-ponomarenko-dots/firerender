// particle.fs — спрайт частицы. Режим (альфа/аддитив) задаёт draw call.
// Для стриков (vIsStreak > 0.5): маска капли (ярко в середине, затухание
// к хвосту/голове) + вертикальный тайлинг текстуры для эффекта капель.
in vec2 vUV;
in vec3 vColor;
in float vAlpha;
in float vIsStreak;
in float vStreakLen;

uniform sampler2D uTex;

out vec4 FragColor;

void main()
{
    float a = vAlpha;
    vec3 col = vColor;
    if (vIsStreak > 0.5) {
        // vUV.y = 0..1 вдоль стрика (0 = хвост, 1 = голова)
        float t = vUV.y;
        // Маска капли: синусоида с затуханием к краям
        float dropMask = sin(t * 3.14159) * (1.0 - abs(t - 0.5) * 1.2);
        dropMask = clamp(dropMask, 0.0, 1.0);
        // Вертикальный тайлинг: текстура повторяется вдоль стрика
        vec2 texUV = vec2(vUV.x, t * 4.0);
        vec4 tTex = texture(uTex, texUV);
        a *= tTex.a * dropMask * 0.8;
        // Небольшое аддитивное свечение для мокрости
        col = col * tTex.rgb;
    } else {
        vec4 tTex = texture(uTex, vUV);
        if (tTex.a < 0.01) discard;
        a *= tTex.a;
        col *= tTex.rgb;
    }
    FragColor = vec4(col * a, a);
}