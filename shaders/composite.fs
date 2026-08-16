// ============================================================================
//  Composite — сцена + glow + тонМАППИНГ (HDR) + ГАММА / цвето-коррекция
// ----------------------------------------------------------------------------
//  Сцена в половинном цвете (HDR): сумма сцены и размытых ярких пикселей
//  может превышать 1.0. Здесь же:
//    - тонмаппинг (Рейнхард) -> [0,1) без засветов,
//    - гамма-кодирование (uGamma),
//    - насыщенность (uSat), контраст (uContrast),
//  чтобы вернуть сочность после HDR и не дать сцене "посереть".
// ============================================================================

in vec2 vUV;

uniform sampler2D uScene;
uniform sampler2D uGlow;
uniform float uStrength;     // сила glow
uniform float uExposure;     // экспозиция (HDR)
uniform float uGamma;        // гамма-коррекция (энкодинг в sRGB)
uniform float uSat;          // насыщенность (>1 — сочнее)
uniform float uContrast;     // контраст (1 = без изменений)

out vec4 FragColor;

// Яркость (Rec.709). Для насыщенности и контраста.
vec3 luma_vec = vec3(0.299, 0.587, 0.114);

void main() {
    vec3 sc = texture(uScene, vUV).rgb;
    vec3 gl = texture(uGlow, vUV).rgb;
    vec3 c = sc + gl * uStrength;

    // Экспозиция + Рейнхард-тонмаппинг: HDR -> [0,1)
    c *= uExposure;
    c = c / (c + 1.0);

    // Контраст (вокруг 0.5, до гаммы — чтобы тени/света не убить)
    float l = dot(c, luma_vec);
    l = (l - 0.5) * uContrast + 0.5;
    c *= max(0.0, l / max(dot(c, luma_vec), 1e-5));

    // Насыщенность: mix к серому по Rec.709
    float gray = dot(c, luma_vec);
    c = mix(vec3(gray), c, uSat);

    // Гамма-энкодинг в sRGB-подобное пространство (дисплей ждёт ~2.2)
    c = pow(max(c, 0.0), vec3(1.0 / uGamma));

    FragColor = vec4(c, 1.0);
}