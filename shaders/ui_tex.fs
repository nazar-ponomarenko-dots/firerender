// ============================================================================
//  UI texture — fragment shader for HUD buttons/icons
// ----------------------------------------------------------------------------
//  A quad with an RGBA texture (PNG from img/) multiplied by uColor — tint
//  and alpha. Not to be confused with text.fs: there alpha comes from the R
//  channel of the font atlas, here it is a proper RGBA button texture.
// ============================================================================

in vec2 vUV;

uniform sampler2D uTex;       // button texture (img/*.png)
uniform vec4 uColor;          // tint (rgb) + opacity (a)

out vec4 FragColor;

void main() {
    FragColor = texture(uTex, vUV) * uColor;
}
