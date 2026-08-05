// ============================================================================
//  Blur — раздельный гауссов блюр (11 тапов, широкое ядро — мягкое свечение)
// ----------------------------------------------------------------------------
//  Два прохода: горизонтальный и вертикальный (uDir). Оба на половинном
//  разрешении — мягкое свечение без дорогого полного блюра.
//  Веса — нормализованный гаусс sigma = 2.4 (0.019..0.170).
// ============================================================================

in vec2 vUV;

uniform sampler2D uTex;
uniform vec2 uDir;      // (1,0) или (0,1) — направление размытия
uniform vec2 uTexel;    // 1/размер текстуры

out vec4 FragColor;

void main() {
    vec2 off = uDir * uTexel;
    vec3 sum = texture(uTex, vUV).rgb * 0.16980;
    sum += texture(uTex, vUV + off).rgb * 0.15570;
    sum += texture(uTex, vUV - off).rgb * 0.15570;
    sum += texture(uTex, vUV + off * 2.0).rgb * 0.11995;
    sum += texture(uTex, vUV - off * 2.0).rgb * 0.11995;
    sum += texture(uTex, vUV + off * 3.0).rgb * 0.07773;
    sum += texture(uTex, vUV - off * 3.0).rgb * 0.07773;
    sum += texture(uTex, vUV + off * 4.0).rgb * 0.04234;
    sum += texture(uTex, vUV - off * 4.0).rgb * 0.04234;
    sum += texture(uTex, vUV + off * 5.0).rgb * 0.01939;
    sum += texture(uTex, vUV - off * 5.0).rgb * 0.01939;
    FragColor = vec4(sum, 1.0);
}
