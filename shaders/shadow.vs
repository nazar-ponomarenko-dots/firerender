// ============================================================================
//  Shadow — vertex shader of the shadow map (terrain)
// ----------------------------------------------------------------------------
//  Same relief as in terrain.vs, but the position goes straight into the light
//  source's space (uViewProj = proj * view from the sun).
// ============================================================================

layout (location = 0) in vec2 aPos;   // x, z
layout (location = 1) in vec2 aUV;    // coordinate in the heightmap

uniform mat4 uViewProj;               // view * projection from the SUN
uniform sampler2D uHeightmap;
uniform float uHeightMin;
uniform float uHeightMax;

float sample_height16(vec2 uv) {
    vec2 h = texture(uHeightmap, uv).rg;
#ifdef FR_HEIGHT_RG8
    return (h.r * 65280.0 + h.g * 255.0) / 65535.0;
#else
    return h.r;
#endif
}

void main() {
    float t = sample_height16(aUV);
    float h = mix(uHeightMin, uHeightMax, t);
    gl_Position = uViewProj * vec4(aPos.x, h, aPos.y, 1.0);
}
