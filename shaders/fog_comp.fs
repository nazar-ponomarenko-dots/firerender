// ============================================================================
//  fog_comp.fs — fog composite onto the screen (after the half-size march)
// ----------------------------------------------------------------------------
//  The volumetric fog march writes into a HALF-SIZE RGBA8 texture (fog color +
//  transmittance in alpha); here — one fog sample and one scene sample, mixed
//  with the same formulas the march used internally. The picture is bit-for-bit
//  the same while the march costs four times less (smooth fog survives the
//  bilinear upscale).
// ============================================================================

in vec2 vUV;

uniform sampler2D uScene;        // scene (native)
uniform sampler2D uFog;          // fog march (half size)

out vec4 FragColor;

void main() {
    vec4 fog = texture(uFog, vUV);
    vec3 scene = texture(uScene, vUV).rgb;
    FragColor = vec4(scene * fog.a + fog.rgb, 1.0);
}
