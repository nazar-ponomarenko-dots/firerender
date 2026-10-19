// ============================================================================
//  SkinnedShadow — vertex shader of SKINNED meshes into the shadow map
// ----------------------------------------------------------------------------
//  Same skinning as in skinned.vs (up to 4 bones per vertex, uBones[64] —
//  jointMatrix), but the output is only the position projected from the SUN:
//  animated models cast a shadow in the current frame's pose.
//  UV goes to the FS (alpha-test of the model's transparent parts).
// ============================================================================

layout (location = 0) in vec3 aPos;
layout (location = 2) in vec2 aUV;
layout (location = 3) in vec4 aBoneIndices;   // same as skinned.vs: vec4
layout (location = 4) in vec4 aBoneWeights;

uniform mat4 uModel;            // node's world matrix
uniform mat4 uViewProj;         // sun view * projection (rc->shadow_vp)

#define MAX_BONES 64
uniform mat4 uBones[MAX_BONES];
uniform int uHasSkin;           // 0 — no skinning (plain matrix transform)

out vec2 vUV;

void main() {
    vec3 pos = aPos;
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
        if (wsum > 0.001) sk /= wsum;
        pos = vec3(sk * vec4(aPos, 1.0));
    }
    vUV = aUV;
    gl_Position = uViewProj * uModel * vec4(pos, 1.0);
}
