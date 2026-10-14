// ============================================================================
//  Sky — vertex shader for the skybox
// ----------------------------------------------------------------------------
//  Fullscreen triangle: the fragment shader draws the whole sky, reconstructing
//  the ray direction from the camera basis. No spheres, no seams.
// ============================================================================

layout (location = 0) in vec2 aPos;   // NDC: (-1,-1), (3,-1), (-1,3)

out vec2 vPos;

void main() {
    vPos = aPos;
    gl_Position = vec4(aPos, 1.0, 1.0);
}
