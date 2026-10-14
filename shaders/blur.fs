// ============================================================================
//  Blur — separable Gaussian blur (11-tap kernel — soft glow)
// ----------------------------------------------------------------------------
//  Two passes: horizontal and vertical (uDir). Both at half resolution —
//  soft glow without an expensive full blur.
//  Weights — a normalized Gaussian with sigma = 2.4 (0.019..0.170).
//
//  LINEAR SAMPLING: the texture is filtered with GL_LINEAR, so two adjacent
//  kernel taps are taken with ONE bilinear sample at a weighted position
//  between texels. 11 logical taps are computed as 7 physical samples —
//  the result is the same (bilinear gives w1·t1+w2·t2 at position
//  (1·w1+2·w2)/(w1+w2)). Pairs: (±1,±2) and (±3,±4);
//  tap ±5 is taken separately (odd, far, small weight).
// ============================================================================

in vec2 vUV;

uniform sampler2D uTex;
uniform vec2 uDir;      // (1,0) or (0,1) — blur direction
uniform vec2 uTexel;    // 1/texture size

out vec4 FragColor;

// Merged weights/offsets of adjacent taps (derivation in the header):
// pair (1,2): w=0.15570+0.11995, off=(1·0.15570+2·0.11995)/w
// pair (3,4): w=0.07773+0.04234, off=(3·0.07773+4·0.04234)/w
const float W12 = 0.27565, O12 = 1.4351170;
const float W34 = 0.12007, O34 = 3.3526276;
const float W5  = 0.01939, O5  = 5.0;

void main() {
    vec2 off = uDir * uTexel;
    vec3 sum = texture(uTex, vUV).rgb * 0.16980;
    sum += texture(uTex, vUV + off * O12).rgb * W12;
    sum += texture(uTex, vUV - off * O12).rgb * W12;
    sum += texture(uTex, vUV + off * O34).rgb * W34;
    sum += texture(uTex, vUV - off * O34).rgb * W34;
    sum += texture(uTex, vUV + off * O5).rgb  * W5;
    sum += texture(uTex, vUV - off * O5).rgb  * W5;
    FragColor = vec4(sum, 1.0);
}
