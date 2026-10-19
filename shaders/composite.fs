// ============================================================================
//  Composite — scene + glow + light HDR + color grading
// ----------------------------------------------------------------------------
//  The scene is already gamma-encoded by the shaders (sRGB-like 0..1 values),
//  not linear HDR. So tone mapping is ONLY a soft "shoulder" roll-off of
//  highlights: it leaves mids and shadows untouched (no graying, no crushing).
//  The main work is saturation and contrast on top, to bring the punch back.
//
//  Underwater (uUnderwater > 0): wavy UV distortion (the picture "swims", as
//  through a body of water), tinting with the water color, desaturation and
//  darkening toward the frame edges — the world looks submerged.
// ============================================================================

in vec2 vUV;

uniform sampler2D uScene;
uniform sampler2D uGlow;
uniform float uStrength;     // glow strength
uniform float uExposure;     // exposure
uniform float uGamma;        // gamma (1 = unchanged)
uniform float uSat;          // saturation (>1 — punchier)
uniform float uContrast;     // contrast (1 = unchanged)
uniform float uWetness;      // wetness 0..1 — colors get duller in rain
uniform float uUnderwater;   // 0..1 — strength of underwater effects (smooth entry)
uniform vec3  uWaterTint;    // water body tint under the eyes
uniform float uTime;         // time (underwater wave animation)

out vec4 FragColor;

const vec3 luma = vec3(0.299, 0.587, 0.114);

void main() {
    vec2 uv = vUV;

    // Underwater: UVs "swim" — two low-frequency waves shift the scene
    // sample. The deeper, the stronger (uUnderwater) — near the surface it
    // barely trembles. Thinning toward the bottom: water is thicker there, distortion stronger.
    if (uUnderwater > 0.001) {
        float amp = 0.0105 * uUnderwater;
        vec2 w;
        w.x = sin(uv.y * 34.0 - uTime * 2.6) * amp
            + sin(uv.y * 57.0 + uv.x * 9.0 - uTime * 4.1) * amp * 0.4;
        w.y = sin(uv.x * 28.0 + uTime * 2.1
                  + uv.y * 12.0) * amp * 0.7
            + sin(uv.x * 45.0 - uv.y * 14.0 + uTime * 3.3) * amp * 0.3;
        uv += w * (0.5 + 0.5 * (1.0 - vUV.y));
    }

    vec3 sc = texture(uScene, uv).rgb;
    // Take the glow sample only when bloom is actually on (uStrength > 0).
    // With bloom off (potato preset, uStrength = 0) the glow would still be
    // multiplied by zero — a fullscreen glow-texture sample wasted. The skip is
    // byte-for-byte identical to the old path (sc + 0), and on iGPU it saves a
    // full pass over the glow buffer.
    vec3     c = sc;
    if (uStrength > 0.0)
        c += texture(uGlow, uv).rgb * uStrength;

    c *= uExposure;

    // Soft shoulder roll-off ONLY for highlights (HDR headroom). Up to ~1.0 —
    // identity (mids and shadows unbent), above 1.0 — a smooth bend into 1.
    // Not Reinhard: it would bend everything and wash out the colors, as before.
    c = c / (1.0 + smoothstep(1.0, 3.0, c) * 0.35);

    // Contrast on luminance around 0.5 — gently, so as not to crush shadows.
    float l = dot(c, luma);
    float target = (l - 0.5) * uContrast + 0.5;
    c *= max(target / max(l, 1e-5), 0.0);

    // Saturation: mix toward gray per Rec.709 (>1 — punchier).
    // In rain (uWetness) saturation drops slightly (not to gray).
    float sat = mix(uSat, 0.55, uWetness);
    // Underwater — colors leach out: light is filtered by the water body.
    if (uUnderwater > 0.001)
        sat = mix(sat, min(sat * 0.45, 0.55), uUnderwater);
    c = mix(vec3(dot(c, luma)), c, sat);

    // Extra gamma correction (default 1 — the scene is already gamma-encoded).
    // Can be raised in the console for a brighter look / lifted shadows.
    c = pow(max(c, 0.0), vec3(1.0 / uGamma));

    // --- Underwater tinting ------------------------------------------------------
    if (uUnderwater > 0.001) {
        // The water tint filters light: far and dark areas shift toward the body color.
        c = mix(c, uWaterTint * (0.25 + 0.75 * dot(c, luma)),
                uUnderwater * 0.55);
        // The bottom of the frame is deeper water — darker (light passed through more).
        c *= 1.0 - uUnderwater * 0.25 * (1.0 - vUV.y);
        // Vignette: darkening toward the edges (the water body "presses") and a
        // slight glow in the center — light from above, through the depth.
        float d = length(vUV - 0.5);
        c *= 1.0 - uUnderwater * 0.45 * smoothstep(0.22, 0.75, d);
        c += uWaterTint * uUnderwater * 0.05 * (1.0 - smoothstep(0.0, 0.5, d));
    }

    FragColor = vec4(c, 1.0);
}
