// ============================================================================
//  Mesh — vertex shader of a static mesh (.obj object, prop_static)
// ----------------------------------------------------------------------------
//  A real model matrix + vertex normals: unlike billboards, the object is
//  lit for real (normal-based diffuse).
// ============================================================================

layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec2 aUV;
layout (location = 3) in float aSnow;    // per-vertex "open to the sky from above"

uniform mat4 uViewProj;
uniform mat4 uModel;           // object translation + scale
uniform vec2 uUvTile;          // UV repeat (scaling)

out vec3 vWorldPos;
out vec3 vNormal;
out vec2 vUV;
out float vSnow;

void main() {
    vec4 wp = uModel * vec4(aPos, 1.0);
    vWorldPos = wp.xyz;
    vNormal = normalize(mat3(uModel) * aNormal);
    vUV = aUV * uUvTile;
    vSnow = aSnow;
    gl_Position = uViewProj * wp;
}