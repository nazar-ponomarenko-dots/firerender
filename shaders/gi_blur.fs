// ============================================================================
//  gi_blur.fs — spatial GI denoiser (bilateral, plane-based)
// ----------------------------------------------------------------------------
//  Screen GI is computed from a small ray count — noise remains. We kill it
//  with a wide blur, but ONLY within one surface.
//
//  A neighbour's weight comes not from the depth difference but from the
//  neighbour's distance to the TANGENT PLANE of the center pixel. The depth
//  difference alone is a poor criterion: on a slanted surface (floor at an
//  angle to the camera) it is large for neighbours in the same plane — the
//  filter degraded into "almost no blur", and the noise had to be hidden by
//  growing the radius, which smeared the corners. The plane weight keeps
//  neighbours of the same floor/wall together and cuts the link at a fold,
//  so light does not bleed across corners and silhouettes.
//
//  Also: GI samples are spread over pixels via IGN (see gi.fs), i.e.
//  neighbours hold DIFFERENT hemisphere directions. This filter sums them up —
//  effectively raising the ray count per pixel without any tracing cost.
//
//  The pass is separable: uDir = (1,0) — horizontal, (0,1) — vertical.
// ============================================================================

in vec2 vUV;

uniform sampler2D uGI;        // GI (unit 0): rgb indirect light, a = occlusion
uniform sampler2D uDepth;     // scene depth (unit 1)
uniform mat4 uInvViewProj;    // for reconstructing world positions
uniform vec3 uCamPos;
uniform vec2 uTexel;          // 1 / GI target resolution
uniform vec2 uDir;            // pass direction
uniform vec2 uZ;              // znear, zfar
uniform float uRadius;        // radius in texels

out vec4 FragColor;

float linz(float ndc_z) {
    float zn = uZ.x, zf = uZ.y;
    return (2.0 * zn * zf) / (zf + zn - ndc_z * (zf - zn));
}

vec3 wpos(vec2 uv, float depth) {
    vec4 ndc = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 wp = uInvViewProj * ndc;
    return wp.xyz / wp.w;
}

vec3 wnormal(vec2 uv, float d0, vec3 p0) {
    vec2 tx = vec2(uTexel.x, 0.0);
    vec2 ty = vec2(0.0, uTexel.y);
    float dR = texture(uDepth, uv + tx).r;
    float dL = texture(uDepth, uv - tx).r;
    bool useR = abs(dR - d0) < abs(dL - d0);
    vec3 px = wpos(uv + (useR ? tx : -tx), useR ? dR : dL);
    float dU = texture(uDepth, uv + ty).r;
    float dD = texture(uDepth, uv - ty).r;
    bool useU = abs(dU - d0) < abs(dD - d0);
    vec3 py = wpos(uv + (useU ? ty : -ty), useU ? dU : dD);
    vec3 n = cross(px - p0, py - p0);
    float l = length(n);
    if (l < 1e-6 || isnan(l) || isinf(l)) return vec3(0.0, 1.0, 0.0);
    n /= l;
    if (dot(n, uCamPos - p0) < 0.0) n = -n;
    return n;
}

void main() {
    float d0 = texture(uDepth, vUV).r;
    vec4 c0 = texture(uGI, vUV);
    if (d0 >= 0.9999 || d0 <= 1e-5) { FragColor = c0; return; }   // do not blur the sky

    vec3 P = wpos(vUV, d0);
    vec3 N = wnormal(vUV, d0, P);
    float z0 = linz(d0 * 2.0 - 1.0);

    // The plane tolerance grows with distance: far away one texel covers more
    // of the world, and "own" surface honestly drifts further from the plane.
    float tol = 0.02 + 0.02 * z0;
    // The radius too: at distance there are fewer rays (tracing LOD) and one GI
    // texel covers more screen, so the noise is coarser — the blur must be wider.
    // Up close — a narrow radius keeps contact shadows crisp.
    float lod = smoothstep(30.0, 150.0, z0);
    float rad = uRadius * (0.85 + 0.65 * lod);

    // 9-tap Gaussian; the weight is multiplied by the plane factor.
    const float w[5] = float[5](0.227, 0.194, 0.121, 0.054, 0.016);
    vec4 acc = c0 * w[0];
    float wsum = w[0];
    for (int i = 1; i < 5; i++) {
        vec2 off = uDir * uTexel * (float(i) * rad);
        for (int s = 0; s < 2; s++) {
            vec2 uv = s == 0 ? vUV + off : vUV - off;
            float d = texture(uDepth, uv).r;
            if (d >= 0.9999 || d <= 1e-5) continue;
            vec3 q = wpos(uv, d);
            float plane = abs(dot(q - P, N));           // deviation from the plane
            float bw = 1.0 - smoothstep(tol, tol * 4.0, plane);
            if (bw <= 0.0) continue;
            acc += texture(uGI, uv) * (w[i] * bw);
            wsum += w[i] * bw;
        }
    }
    FragColor = acc / max(wsum, 1e-4);
}
