// ============================================================================
//  Composite — сцена + glow + тонМАППИНГ (HDR)
// ----------------------------------------------------------------------------
//  Сцена в половинном цвете (HDR): сумма сцены и размытых ярких пикселей
//  может превышать 1.0. Здесь же — тонмаппинг (Рейнхард) в LDR + экспозиция,
//  чтобы яркость экрана держалась в [0,1] без засветов.
// ============================================================================

in vec2 vUV;

uniform sampler2D uScene;
uniform sampler2D uGlow;
uniform float uStrength;     // сила glow
uniform float uExposure;     // экспозиция (HDR)

out vec4 FragColor;

void main() {
    vec3 sc = texture(uScene, vUV).rgb;
    vec3 gl = texture(uGlow, vUV).rgb;
    vec3 c = sc + gl * uStrength;

    // Экспозиция + Рейнхард-тонмаппинг: HDR -> LDR [0,1)
    c *= uExposure;
    c = c / (c + 1.0);

    FragColor = vec4(c, 1.0);
}