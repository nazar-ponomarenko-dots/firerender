// ============================================================================
//  Material — vertex shader of the material system (see matprog.c)
// ----------------------------------------------------------------------------
//  Same as mesh.vs plus material UV animation (uUvOffset/uUvScroll — the
//  material sends them via material_bind). The system uses this file as the
//  base for ALL permutations: fragment features (blending/specular/wetness/
//  emission) are compiled via #defines in material.fs, the vertex path is shared.
// ============================================================================

layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec2 aUV;

uniform mat4 uViewProj;
uniform mat4 uModel;           // object translation + scale
uniform vec2 uUvTile;          // UV repeat (scaling)
uniform vec2 uUvOffset;        // UV shift (tiling)
uniform vec2 uUvScroll;        // UV animation speed (tiles/sec)
uniform float uTime;           // time (for scroll)

out vec3 vWorldPos;
out vec3 vNormal;
out vec2 vUV;

void main() {
    vec4 wp = uModel * vec4(aPos, 1.0);
    vWorldPos = wp.xyz;
    vNormal = normalize(mat3(uModel) * aNormal);
    vUV = aUV * uUvTile + uUvOffset + uUvScroll * uTime;
    gl_Position = uViewProj * wp;
}
