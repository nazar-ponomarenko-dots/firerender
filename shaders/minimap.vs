// ============================================================================
//  Minimap — vertex shader for the minimap
// ----------------------------------------------------------------------------
//  The minimap is drawn in screen coordinates (NDC) without matrices:
//  vertex positions are already prepared in clip space by the Minimap node.
// ============================================================================

layout (location = 0) in vec2 aPos;   // position in NDC
layout (location = 1) in vec2 aUV;    // uv in the map texture

out vec2 vUV;

void main() {
    vUV = aUV;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
