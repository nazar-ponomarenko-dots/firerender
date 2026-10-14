// ============================================================================
//  ObjectFlat — fragment shader of an object WITHOUT lighting
// ----------------------------------------------------------------------------
//  Color = texture * uTint. Used by the built-in "shader": "flat" mode
//  (icons, plaques, UI objects) and serves as a template for YOUR OWN shader:
//  declare only the uniforms you need, the engine finds them by name and
//  passes the value.
// ============================================================================

in vec2 vUV;
in vec3 vWorldPos;

uniform sampler2D uTex;
uniform vec3 uTint;

out vec4 FragColor;

void main() {
    vec4 c = texture(uTex, vUV);
    if (c.a < 0.5) discard;
    FragColor = vec4(c.rgb * uTint, 1.0);
}
