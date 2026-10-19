// ============================================================================
//  PlayerBox — vertex shader for the character sprite and collision edges
// ----------------------------------------------------------------------------
//  Vertex positions are already in world space (built by the PlayerBox node):
//  the sprite quad stands feet-on the character's ground, the box edges are
//  12 line segments around it.
// ============================================================================

layout (location = 0) in vec3 aPos;   // vertex world position
layout (location = 1) in vec2 aUV;    // uv for the sprite (arbitrary for lines)

uniform mat4 uViewProj;               // view * projection (view camera)

out vec2 vUV;

void main() {
    vUV = aUV;
    gl_Position = uViewProj * vec4(aPos, 1.0);
}
