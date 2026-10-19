// ============================================================================
//  Skinned — vertex shader for skeletal animation (glTF skinned mesh)
// ----------------------------------------------------------------------------
//  Skinning is computed on the GPU: each vertex carries up to 4 bones
//  (aBoneIndices: 4xubyte) and their weights (aBoneWeights: 4xfloat). Bone
//  matrices (jointMatrix = world[joint] * inverseBind[joint]) are passed
//  as the uBones[64] array. Output is the same vWorldPos/vNormal/vUV that
//  mesh.fs expects — so skinning reuses the same fragment shader as statics.
// ============================================================================

layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec2 aUV;
// Keep bone indices as REAL numbers (vec4) even though they are bytes on the
// CPU: then they can be supplied with a plain glVertexAttribPointer (GL_FLOAT)
// in the shared interleaved buffer next to pos/normal/uv/weights, without a
// separate glVertexAttribIPointer. Values are integral (0..255) — int() is exact.
layout (location = 3) in vec4 aBoneIndices;
layout (location = 4) in vec4 aBoneWeights;
layout (location = 5) in float aSnow;    // per-vertex "exposed to sky from above" (snow)

uniform mat4 uViewProj;
uniform mat4 uModel;            // object translation + scale
uniform vec2 uUvTile;

#define MAX_BONES 64
uniform mat4 uBones[MAX_BONES];
uniform int uHasSkin;           // 0 — not skinning (plain matrix transform)

out vec3 vWorldPos;
out vec3 vNormal;
out vec2 vUV;
out float vSnow;

void main() {
    vSnow = aSnow;
    vec3 pos = aPos;
    vec3 nrm = aNormal;
    if (uHasSkin > 0) {
        mat4 sk = mat4(0.0);
        float wsum = 0.0;
        for (int b = 0; b < 4; b++) {
            int j = int(aBoneIndices[b]);
            float w = aBoneWeights[b];
            if (j >= 0 && j < MAX_BONES && w > 0.0) {
                sk += uBones[j] * w;
                wsum += w;
            }
        }
        // normalization: if the weight sum is near zero — leave it as is
        if (wsum > 0.001) sk /= wsum;
        pos = vec3(sk * vec4(aPos, 1.0));
        nrm = mat3(sk) * aNormal;
    }
    vec4 wp = uModel * vec4(pos, 1.0);
    vWorldPos = wp.xyz;
    vNormal = normalize(mat3(uModel) * nrm);
    vUV = aUV * uUvTile;
    gl_Position = uViewProj * wp;
}
