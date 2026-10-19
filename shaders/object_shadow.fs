// ============================================================================
//  ObjectShadow — fragment shader of objects into the shadow map
// ----------------------------------------------------------------------------
//  The texture's transparent background is discarded (otherwise trees would
//  cast a shadow as the whole quad rectangle).
// ============================================================================

in vec2 vUV;

uniform sampler2D uTex;

void main() {
    if (texture(uTex, vUV).a < 0.5) discard;
}
