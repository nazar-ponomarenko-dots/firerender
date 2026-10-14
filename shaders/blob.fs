// ============================================================================
//  Blob — fragment shader of the blob shadow (mode 3)
// ----------------------------------------------------------------------------
//  A soft dark ellipse: the texture alpha (fauna_glow — bright center) gives a
//  dense spot in the middle, transparent toward the edges.
// ============================================================================

in vec2 vUV;

uniform sampler2D uTex;
uniform vec4 uColor;        // spot color (usually black, alpha ~0.5)

out vec4 FragColor;

void main() {
    float a = texture(uTex, vUV).a;
    FragColor = vec4(uColor.rgb, uColor.a * a);
}
