// ============================================================================
//  Composite — сцена + glow
// ----------------------------------------------------------------------------
//  Складывает отрендеренную сцену с размытым буфером ярких пикселей
//  (glow апсемплится билинейно с половинного разрешения).
// ============================================================================

in vec2 vUV;

uniform sampler2D uScene;
uniform sampler2D uGlow;
uniform float uStrength;     // сила glow

out vec4 FragColor;

void main() {
    vec3 sc = texture(uScene, vUV).rgb;
    vec3 gl = texture(uGlow, vUV).rgb;
    FragColor = vec4(sc + gl * uStrength, 1.0);
}
