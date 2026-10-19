// ============================================================================
//  Field — fragment shader for the wheat field frame
// ============================================================================

uniform vec3 uColor;                  // frame color

out vec4 FragColor;

void main() {
    FragColor = vec4(uColor, 1.0);
}
