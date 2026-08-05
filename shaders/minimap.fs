// ============================================================================
//  Minimap — фрагментный шейдер мини-карты
// ----------------------------------------------------------------------------
//  Один шейдер на всё, что рисует мини-карта:
//    uUseTex    = 1 -> карта (uBasemap);
//    uUseTex    = 0 -> сплошной цвет uColor (линии);
//    uUseMarker = 1 -> маркер игрока (uMarker — текстура со своей альфой,
//                      повёрнутая на yaw в minimap.c).
// ============================================================================

in vec2 vUV;

uniform sampler2D uBasemap;   // текстура карты (базовый цвет рельефа)
uniform sampler2D uMarker;    // маркер игрока (img/player.png)
uniform vec3 uColor;          // сплошной цвет для линий
uniform float uUseTex;        // 1 = карта, 0 = цвет
uniform float uUseMarker;     // 1 = маркер игрока (текстура + альфа)

out vec4 FragColor;

void main() {
    vec4 map = texture(uBasemap, vUV);
    vec4 mk = texture(uMarker, vUV);
    vec3 col = mix(uColor, map.rgb, uUseTex);
    col = mix(col, mk.rgb, uUseMarker);
    float a = mix(1.0, mk.a, uUseMarker);
    FragColor = vec4(col, a);
}
