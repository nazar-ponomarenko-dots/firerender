// ============================================================================
//  Skinned — вершинный шейдер скелетной анимации (glTF skinned mesh)
// ----------------------------------------------------------------------------
//  Кожа (skinning) считается на GPU: каждая вершина несёт до 4 костей
//  (aBoneIndices: 4×ubyte) и их веса (aBoneWeights: 4×float). Матрицы костей
//  (jointMatrix = world[joint] * inverseBind[joint]) передаются массивом
//  uBones[64]. На выходе те же vWorldPos/vNormal/vUV, что ждёт mesh.fs —
//  поэтому скиннинг использует тот же фрагментный шейдер, что и статика.
// ============================================================================

layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec2 aUV;
// Индексы костей держим ВЕЩЕСТВЕННЫМИ (vec4), хотя на CPU это байты: так
// их можно подать обычным glVertexAttribPointer (GL_FLOAT) в общем
// interleaved-буфере рядом с pos/normal/uv/weights, без отдельного
// glVertexAttribIPointer. Значения целые (0..255) — int() точен.
layout (location = 3) in vec4 aBoneIndices;
layout (location = 4) in vec4 aBoneWeights;
layout (location = 5) in float aSnow;    // per-vertex "открыто небу сверху" (снег)

uniform mat4 uViewProj;
uniform mat4 uModel;            // перенос + масштаб объекта
uniform vec2 uUvTile;

#define MAX_BONES 64
uniform mat4 uBones[MAX_BONES];
uniform int uHasSkin;           // 0 — не скиннинг (просто мат. трансформ)

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
        // нормализация: если сумма весов близка к нулю — оставить как есть
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
