// ============================================================================
//  Minimap — fragment shader of the minimap
// ----------------------------------------------------------------------------
//  One shader for everything the minimap draws:
//    uUseTex    = 1 -> the map (uBasemap);
//    uUseTex    = 0 -> solid color uColor (lines);
//    uUseMarker = 1 -> the player marker (uMarker — texture with its own alpha,
//                      rotated by yaw in minimap.c).
// ============================================================================

in vec2 vUV;

uniform sampler2D uBasemap;   // map texture (terrain base color)
uniform sampler2D uMarker;    // player marker (img/player.png)
uniform vec3 uColor;          // solid color for lines
uniform float uUseTex;        // 1 = map, 0 = color
uniform float uUseMarker;     // 1 = player marker (texture + alpha)

out vec4 FragColor;

void main() {
    vec4 map = texture(uBasemap, vUV);
    vec4 mk = texture(uMarker, vUV);
    vec3 col = mix(uColor, map.rgb, uUseTex);
    col = mix(col, mk.rgb, uUseMarker);
    float a = mix(1.0, mk.a, uUseMarker);
    FragColor = vec4(col, a);
}
