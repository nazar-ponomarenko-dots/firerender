// particle.fs — спрайт частицы. Режим (альфа/аддитив) задаёт draw call.
// Для стриков (vIsStreak > 0.5): маска капли (ярко в середине, затухание
// к хвосту/голове) + вертикальный тайлинг текстуры для эффекта капель.
// Для plain (vPlain > 0.5): БЕЗ текстуры — яркий круг с мягкой кромкой
// (снежинки), цвет = чистый цвет пресета.
in vec2 vUV;
in vec3 vColor;
in float vAlpha;
in float vIsStreak;
in float vStreakLen;
in float vPlain;

uniform sampler2D uTex;

out vec4 FragColor;

void main()
{
    float a = vAlpha;
    vec3 col = vColor;
    if (vPlain > 0.5) {
        // Чистый диск: максимально белый и яркий, без текстуры.
        float d = length(vUV - 0.5);
        if (d > 0.5) discard;
        float mask = 1.0 - smoothstep(0.28, 0.5, d);   // мягкая кромка
        col = col * (1.25 + 0.5 * mask);         // слегка выжигаем в белое
        FragColor = vec4(col * a * mask, a * mask);
        return;
    }
    if (vIsStreak > 0.5) {
        // vUV.y = 0..1 вдоль стрика (0 = хвост, 1 = голова)
        float t = vUV.y;
        // Капля ярче у головы (t→1), плавно затухает к хвосту, чтобы стрик
        // читался как летящая капля, а не симметричный жёсткий стержень.
        float dropMask = smoothstep(0.0, 0.15, t) *
                         (1.0 - smoothstep(0.55, 1.0, t));
        // Вертикальный тайлинг: с REPEAT текстура реально повторяется вдоль
        vec2 texUV = vec2(vUV.x, t * 4.0);
        vec4 tTex = texture(uTex, texUV);
        a *= (0.35 + 0.65 * tTex.a) * dropMask * 0.85;
        // мокрый грейдинг: слегка подсвечиваем центр, сохраняя цвет
        col = col * mix(0.8, 1.1, tTex.a * tTex.a);
    } else {
        vec4 tTex = texture(uTex, vUV);
        if (tTex.a < 0.01) discard;
        a *= tTex.a;
        col *= tTex.rgb;
    }
    FragColor = vec4(col * a, a);
}