// ============================================================================
//  Field — vertex shader for the wheat field frame
// ----------------------------------------------------------------------------
//  The rectangle is drawn as a line of 5 vertices on the terrain surface.
// ============================================================================

layout (location = 0) in vec3 aPos;   // frame point in world space

uniform mat4 uViewProj;               // view * projection (from the Camera node)

void main() {
    gl_Position = uViewProj * vec4(aPos, 1.0);
}
