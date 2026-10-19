// ============================================================================
//  MeshShadow — vertex shader of meshes/grids into the shadow map (see mesh.c, glb.c)
// ----------------------------------------------------------------------------
//  Generic depth pass: the position (attribute 0) is multiplied by the node's
//  model matrix (uModel) and by the SUN's view*projection (uViewProj). Mesh VAOs
//  and glb rigs already put the position in attribute 0, so one shader draws any
//  geometry (mesh/glb, including LOD levels) into RP_SHADOW.
// ============================================================================

layout (location = 0) in vec3 aPos;
layout (location = 2) in vec2 aUV;    // for alpha-test of leaves/grass in the FS

uniform mat4 uModel;       // instance (node) model matrix
uniform mat4 uViewProj;    // sun view * projection (rc->shadow_vp)

out vec2 vUV;

void main() {
    vUV = aUV;
    gl_Position = uViewProj * uModel * vec4(aPos, 1.0);
}
