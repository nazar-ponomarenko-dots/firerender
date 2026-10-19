// ============================================================================
//  Sky — skybox fragment shader
// ----------------------------------------------------------------------------
//  img/skybox.png — a 360x90 panorama (512x128, 4:1): horizon in the middle
//  of the picture, sky above, ground below. From the fragment NDC coordinates
//  and the camera basis (uForward/uRight/uUp) we reconstruct the ray direction
//  and take latitude/longitude from the panorama. The vertical coverage is only
//  90 degrees, so above +45° and below -45° there is no texture data: zenith
//  and nadir are painted with smooth gradients (no flat "disc" from the top row
//  and no hard seam at +45°). The longitude seam is handled by GL_REPEAT, and
//  the "ground" from the panorama (lower half) is muted with fog — real terrain
//  renders below the horizon. LINEAR filtering — smooth gradients without bands.
// ============================================================================

in vec2 vPos;

uniform sampler2D uSky;       // sky panorama
uniform vec3 uTint;           // sky tint (from map.json: sky.tint)
uniform vec3 uForward;        // camera basis (view)
uniform vec3 uRight;
uniform vec3 uUp;
uniform float uTanFov;        // tan(half FOV)
uniform float uAspect;        // window aspect ratio
uniform vec3 uFogColor;       // horizon color (terrain fog)
uniform vec3 uSunDir;         // direction to the sun (Sun node)
uniform vec3 uSunColor;       // sun color
uniform float uSunGlow;       // sun disc visibility 0..1 (fades out in rain)
// Procedural clouds: 2 noise octaves on a plane 500 m above the camera
uniform sampler2D uNoise;     // img/noise.png (unit 1)
uniform vec2 uCamXZ;          // camera XZ position (clouds live in the world, not on the camera)
uniform float uTime;          // cloud drift
uniform float uClouds;        // coverage 0..1 (cvar r_clouds)

out vec4 FragColor;

void main() {
    vec3 rd = normalize(uForward
                        + uRight * vPos.x * uTanFov * uAspect
                        + uUp * vPos.y * uTanFov);
    float u = 0.5 + atan(rd.z, rd.x) / 6.2831853;        // longitude
    float elev = asin(clamp(rd.y, -1.0, 1.0));           // angle above the horizon
    // The panorama covers ±45°: v=1 (bottom) = -45°, v=0 (top) = +45°.
    float v = clamp(0.5 - elev * 0.63661977, 0.0, 1.0);
    vec3 col = texture(uSky, vec2(u, v)).rgb * uTint;
    // Dome: above +45° there is no texture (it spans 90°) — a smooth gradient
    // toward the zenith color so the dome reads as volume, not as a flat filled
    // disc from a single texture row (and no hard seam right at +45°).
    float zenithT = smoothstep(0.785398163, 1.570796327, elev); // 0 at +45°, 1 at zenith
    // The second panorama sample (zenith, v=0) and the dome gradient are taken only
    // above +45° (zenithT > 0). Below it — mix yields exactly col, the zenith has no
    // effect: a byte-for-byte pass with the previous path for all presets, while the
    // lower ~2/3 of the sky (the whole sector up to +45°, horizon included) get away
    // with one sample instead of two.
    if (zenithT > 0.0) {
        float dark = 1.0 - 0.18 * smoothstep(0.785398163, 1.570796327, elev);
        vec3 zenith = texture(uSky, vec2(u, 0.0)).rgb * uTint;
        col = mix(col, zenith * dark, zenithT);
    }
    // sun disc and halo — where the light source looks;
    // in rain (uSunGlow -> 0) the sun is hidden by clouds
    float s = dot(rd, normalize(uSunDir));
    col += uSunColor * uSunGlow * (pow(max(s, 0.0), 600.0) * 4.0
                                 + pow(max(s, 0.0), 10.0) * 0.35);
    // --- Procedural clouds ------------------------------------------------
    // DOME MAPPING, not planar: uv = (azimuth, elevation) of the ray direction.
    // With planar projection (rd.xz/rd.y) the sample point flew along azimuth
    // near the horizon — clouds stretched, "wrapped" downward and tore. On the
    // dome there is nothing to stretch: the pattern is stable at any height.
    // Drift — wind along longitude. The noise panorama tiles, no seam at 2π.
    if (uClouds > 0.001 && rd.y > 0.0) {
        // Isotropy over angle: longitude gives 360°/128 cells ≈ 2.8°, so along
        // elevation (rd.y ~ 90°) we need 0.25 of the texture for the whole dome.
        vec2 duv = vec2(atan(rd.z, rd.x) * 0.15915494 + uTime * 0.0011,
                        rd.y * 0.25 + uTime * 0.0004);
        float n = texture(uNoise, duv).r * 0.65
                + texture(uNoise, duv * 2.6 + vec2(0.013, 0.0)).r * 0.35;
        // Coverage threshold lower: at 15% opacity clouds are readable only if
        // there are MANY of them (sparse patches are invisible at low mix).
        float cov = smoothstep(0.58 - uClouds * 0.50, 0.80 - uClouds * 0.38, n);
        cov *= smoothstep(0.02, 0.18, rd.y);          // they sink at the horizon
        if (cov > 0.003) {
            float warm = pow(max(s, 0.0), 3.0);        // sunward side
            // Contrast higher: at a 15% mix the silhouette is visible if the
            // cloud is noticeably darker/lighter than the sky (range ~0.55..1.10)
            vec3 cc = mix(vec3(0.55, 0.58, 0.66),          // shaded side
                          vec3(1.10, 1.05, 0.96),          // lighter toward the sun
                          0.20 + 0.65 * warm);
            cc *= 0.75 + 0.25 * uSunGlow;              // darker in rain
            col = mix(col, cc, cov * 0.15);            // ~15% — delicate
        }
    }
    // below the horizon the panorama is ground, but we have real terrain: mute it
    col = mix(col, uFogColor, smoothstep(0.46, 0.50, v));
    FragColor = vec4(col, 1.0);
}
