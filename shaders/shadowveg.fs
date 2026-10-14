// ============================================================================
//  ShadowVeg — fragment shader of the shadow map (vegetation)
// ----------------------------------------------------------------------------
//  Transparent sprite pixels are discarded, the rest writes depth.
// ============================================================================

in vec2 vUV;
flat in float vLayer;   // from wheat.vs — one LOD value for the whole quad

uniform sampler2DArray uVeg;

void main() {
    if (texture(uVeg, vec3(vUV, vLayer)).a < 0.5) discard;
}
