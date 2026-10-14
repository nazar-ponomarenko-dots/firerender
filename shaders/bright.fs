// ============================================================================
//  Bright — bright-pixel extraction for bloom (glow)
// ----------------------------------------------------------------------------
//  The threshold is soft (smoothstep): bright scene areas (sky, sunlit wheat
//  ears) go into a separate buffer, darkness goes to zero. That buffer is then
//  blurred and added back to the scene.
// ============================================================================

in vec2 vUV;

uniform sampler2D uTex;        // scene (full screen)
uniform float uThreshold;      // luminance threshold (0..1)
uniform float uIntensity;      // multiplier for bright pixels

out vec4 FragColor;

void main() {
    vec3 c = texture(uTex, vUV).rgb;
    float l = dot(c, vec3(0.299, 0.587, 0.114));
    float f = smoothstep(uThreshold, uThreshold + 0.25, l);
    FragColor = vec4(c * f * uIntensity, 1.0);
}
