// ============================================================================
//  SkinnedShadow — вершинный шейдер СКИННЕННЫХ мешей в карту теней
// ----------------------------------------------------------------------------
//  Та же кожа, что в skinned.vs (до 4 костей на вершину, uBones[64] —
//  jointMatrix), но на выходе только позиция в проекцию ОТ СОЛНЦА:
//  анимированные модели отбрасывают тень в позе текущего кадра.
//  UV — в FS (alpha-test прозрачных частей модели).
// ============================================================================

layout (location = 0) in vec3 aPos;
layout (location = 2) in vec2 aUV;
layout (location = 3) in vec4 aBoneIndices;   // как в skinned.vs: vec4
layout (location = 4) in vec4 aBoneWeights;

uniform mat4 uModel;            // мировая матрица узла
uniform mat4 uViewProj;         // вид * проекция от солнца (rc->shadow_vp)

#define MAX_BONES 64
uniform mat4 uBones[MAX_BONES];
uniform int uHasSkin;           // 0 — не скиннинг (просто мат. трансформ)

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
