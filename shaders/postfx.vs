// ============================================================================
//  PostFX — shared vertex shader for post-processing
// ----------------------------------------------------------------------------
//  Fullscreen triangle: a single draw with 3 vertices covers the whole screen
//  (the classic fullscreen quad trick). Positions in NDC, uv pass-through.
// ============================================================================

layout (location = 0) in vec2 aPos;   // NDC
layout (location = 1) in vec2 aUV;    // texture uv

out vec2 vUV;

void main() {
    vUV = aUV;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
