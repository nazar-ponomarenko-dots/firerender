// ============================================================================
//  Fauna — фрагментный шейдер частиц
// ----------------------------------------------------------------------------
//  uMode 0: светлячок — glow-текстура, мерцание, тёплый тон (additive).
//  uMode 1: бабочка/птица — текстура с альфа-тестом, машущие крылья
//           (сжатие по X от времени: только для бабочек — flutter).
// ============================================================================

in vec2 vUV;
in vec3 vWorldPos;

uniform sampler2D uTex;
uniform vec3 uTint;
uniform int uMode;
uniform float uTime;

out vec4 FragColor;

void main() {
    vec4 c = texture(uTex, vUV);
    if (uMode == 1) {
        if (c.a < 0.5) discard;
        FragColor = vec4(c.rgb * uTint, 1.0);
        return;
    }
    // светлячок: мерцание (двойная синусоида от времени + случайная фаза
    // из позиции — иначе все мигали бы синхронно)
    float flick = 0.35 + 0.65 * pow(0.5 + 0.5 * sin(uTime * 2.2
                    + vWorldPos.x * 0.05 + vWorldPos.z * 0.07), 2.0);
    FragColor = vec4(c.rgb * uTint * flick, c.a * flick);
}
