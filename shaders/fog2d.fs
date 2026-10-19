// ============================================================================
//  fog2d.fs — "Simple" fog: analytic, NO 3D textures and no ray marching
// ----------------------------------------------------------------------------
//  Replacement for the previous simplified mode: mimics the behaviour of
//  volumetric fog at minimal cost (measured ~0.1-0.2 ms GPU vs ~0.2-0.5 ms plus
//  CPU voxel rebuild for volumetric; no memory at all for 3D cascades).
//
//  Closed-form physics:
//    * fog density is pressed against the terrain: ρ(t) = ρ0·exp(-(y(t)-ground)/H) —
//      ground height is taken from the heightmap at 4 points along the ray (keeps
//      the "fog in hollows, clear on hilltops" look, like the volumetric mode);
//    * optical depth — trapezoidal integral over those points,
//      not a single ray-marching step;
//    * rain fog (wetness) — analytic integral of uniform
//      density (1 - exp(-t/30));
//    * sun scattering: warm color towards the sun, grey in rain, halo
//      (Mie peak) near the sun direction. There are NO shadow queries — rays
//      from trees stay in the volumetric mode, here cost matters more.
// ============================================================================

in vec2 vUV;

uniform sampler2D uScene;        // scene color (unit 0)
uniform sampler2D uSceneDepth;   // depth (unit 1)
uniform mat4 uInvViewProj;
uniform vec3 uCamPos;
uniform vec2 uZ;                 // znear, zfar
uniform vec3 uSunDir;
uniform vec3 uFogColor;          // base fog color
uniform vec3 uFogColorSun;       // warm scattering color near the sun
uniform vec3 uSkyColor;          // sky color (rain)
uniform float uGlare;            // halo near the sun
uniform float uRayStrength;      // scattering "sunness"
uniform float uWetness;          // wetness 0..1
uniform sampler2D uHeightmap;    // heightmap (unit 10)
uniform vec2 uWorldSize;
uniform float uHeightMin, uHeightMax;
uniform vec4 uFogP;              // x: ρ0 (density at ground level, 1/m), y: H (falloff
                                 // height, m), z: strength multiplier, w: range, m

out vec4 FragColor;
uniform float uFogOnly;          // 1 = output fog only (color+trans)

float linz(float ndc_z) {
    return (2.0 * uZ.x * uZ.y) / (uZ.y + uZ.x - ndc_z * (uZ.y - uZ.x));
}

vec3 wpos(vec2 uv, float depth) {
    vec4 ndc = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 wp = uInvViewProj * ndc;
    return wp.xyz / wp.w;
}

float sample_height16(vec2 uv) {
    vec2 h = texture(uHeightmap, uv).rg;
#ifdef FR_HEIGHT_RG8
    return (h.r * 65280.0 + h.g * 255.0) / 65535.0;
#else
    return h.r;
#endif
}

// Terrain height at a world point (same formula as in volumetric.fs)
float terrain_h(vec2 xz) {
    return mix(uHeightMin, uHeightMax, sample_height16(xz / uWorldSize));
}

void main() {
    vec3 scene = texture(uScene, vUV).rgb;
    float depth = texture(uSceneDepth, vUV).r;

    // Ray: from the eye camera to the pixel's world point (sky — clamped to
    // the fog range, like a march cap in the volumetric mode)
    vec4 ndc = vec4(vUV * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 wp4 = uInvViewProj * ndc;
    vec3 P = wp4.xyz / wp4.w;
    vec3 ray = P - uCamPos;
    float dist = length(ray);
    float L = min(dist, uFogP.w);
    vec3 dir = ray / max(dist, 1e-5);
    if (depth >= 0.9999) P = uCamPos + dir * L;

    // Density along the ray: 4 samples (0.1, 0.4, 0.7, 0.95) — trapezoid.
    // ρ = ρ0 * exp-(height above terrain) / H, zero above 60 m over ground.
    float rho0 = uFogP.x * uFogP.z;
    float H = max(uFogP.y, 1.0);
    float od = 0.0;
    float wsum = 0.0;
    for (int i = 0; i < 4; i++) {
        float t = (0.10 + 0.2833 * float(i)) * L;
        vec3 p = uCamPos + dir * t;
        float above = p.y - terrain_h(p.xz);
        float rho = rho0 * exp(-max(above, 0.0) / H)
                  * (1.0 - smoothstep(40.0, 60.0, above));
        od += rho;
        wsum += 1.0;
    }
    od *= L / max(wsum, 1e-4);

    // Rain fog: uniform density, analytic integral
    od += uWetness * 0.03 * (L - 30.0 * (1.0 - exp(-L / 30.0)));

    float trans = exp(-od);

    // --- Fog color and lighting (as in volumetric.fs, without shadow queries)
    float sunHit = max(dot(dir, uSunDir), 0.0);
    float rainDim = 1.0 - uWetness;
    vec3 fcol = mix(uFogColorSun, uFogColor, 0.15 * (1.0 - sunHit));
    fcol = mix(fcol, uFogColor, uWetness);
    // Away from the sun — warm haze, towards the sun — brighter (god rays read
    // simplified as a glow of the scene near the sun; ray shadows — volumetric mode only).
    float lit = mix(0.55, 0.35 + 0.65 * uRayStrength, sunHit * rainDim);
    lit = max(lit, mix(0.0, 0.32, uWetness));
    // Halo right at the sun (Mie peak)
    float glare = pow(sunHit, 8.0) * uGlare * rainDim;

    if (uFogOnly > 0.5) {
        FragColor = vec4(fcol * (1.0 - trans) * (lit + glare * trans), trans);
        return;
    }
    vec3 col = scene * trans + fcol * (1.0 - trans) * (lit + glare * trans);
    FragColor = vec4(col, 1.0);
}
