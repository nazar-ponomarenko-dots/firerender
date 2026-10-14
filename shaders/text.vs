// ============================================================================
//  Text — vertex shader for HUD text
// ----------------------------------------------------------------------------
//  Like the minimap: vertex positions are already prepared in NDC (computed by
//  the DebugHUD node in main.c), no matrices needed.
// ============================================================================

layout (location = 0) in vec2 aPos;   // position in NDC
layout (location = 1) in vec2 aUV;    // uv in the font atlas

out vec2 vUV;

void main() {
    vUV = aUV;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
