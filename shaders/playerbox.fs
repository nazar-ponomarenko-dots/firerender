// ============================================================================
//  PlayerBox — fragment shader of the character collision box
// ----------------------------------------------------------------------------
//  Solid uColor with alpha (for translucent face fills).
// ============================================================================

uniform vec4 uColor;   // rgb + fill alpha

out vec4 FragColor;

void main() {
    FragColor = uColor;
}
