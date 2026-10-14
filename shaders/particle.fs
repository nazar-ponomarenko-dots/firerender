// particle.fs — particle sprite. The mode (alpha/additive) is set by the draw call.
// For streaks (vIsStreak > 0.5): a droplet mask (bright in the middle, fading
// toward the tail/head) + vertical texture tiling for the droplet effect.
// For plain (vPlain > 0.5): NO texture — a bright disc with a soft edge
// (snowflakes), color = the pure preset color.
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
        // Clean disc: as white and bright as possible, no texture.
        float d = length(vUV - 0.5);
        if (d > 0.5) discard;
        float mask = 1.0 - smoothstep(0.28, 0.5, d);   // soft edge
        col = col * (1.25 + 0.5 * mask);         // slightly burned out to white
        FragColor = vec4(col * a * mask, a * mask);
        return;
    }
    if (vIsStreak > 0.5) {
        // vUV.y = 0..1 along the streak (0 = tail, 1 = head)
        float t = vUV.y;
        // The droplet is brighter near the head (t→1) and fades smoothly toward
        // the tail, so the streak reads as a flying drop rather than a symmetric
        // rigid rod.
        float dropMask = smoothstep(0.0, 0.15, t) *
                         (1.0 - smoothstep(0.55, 1.0, t));
        // Vertical tiling: with REPEAT the texture genuinely repeats along
        vec2 texUV = vec2(vUV.x, t * 4.0);
        vec4 tTex = texture(uTex, texUV);
        a *= (0.35 + 0.65 * tTex.a) * dropMask * 0.85;
        // wet grading: slightly brighten the center while keeping the color
        col = col * mix(0.8, 1.1, tTex.a * tTex.a);
    } else {
        vec4 tTex = texture(uTex, vUV);
        if (tTex.a < 0.01) discard;
        a *= tTex.a;
        col *= tTex.rgb;
    }
    FragColor = vec4(col * a, a);
}