// ============================================================================
//  Text — fragment shader for HUD text
// ----------------------------------------------------------------------------
//  One shader for both the panel and the letters:
//    uUseTex = 0 -> solid uColor (translucent HUD panel);
//    uUseTex = 1 -> glyph: uColor, alpha from the font atlas.
// ============================================================================

in vec2 vUV;

uniform sampler2D uFont;      // font atlas: glyph coverage in R (GL_R8)
uniform vec4 uColor;          // text/panel color (rgb + alpha)
uniform float uUseTex;        // 1 = glyph, 0 = solid color

out vec4 FragColor;

void main() {
    vec4 t = texture(uFont, vUV);
    FragColor = mix(uColor, vec4(uColor.rgb, uColor.a * t.r), uUseTex);
}
