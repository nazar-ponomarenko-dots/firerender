// ============================================================================
//  MeshShadow — fragment shader of meshes into the shadow map
// ----------------------------------------------------------------------------
//  Writes depth only (glDrawBuffer(GL_NONE)) but with an alpha-test: transparent
//  texture pixels (tree leaves, grass in glTF) must NOT cast a shadow with the
//  whole quad. uAlphaTest = 0 disables the test (fully opaque models, no need
//  to bind the texture).
// ============================================================================

in vec2 vUV;

uniform sampler2D uTex;
uniform int uAlphaTest;

void main() {
    if (uAlphaTest == 1 && texture(uTex, vUV).a < 0.5) discard;
}
