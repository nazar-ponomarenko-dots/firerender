// ============================================================================
//  snow.glsl — shared snow-cover block (included in ALL surface shaders:
//  terrain.fs, mesh.fs, object.fs, wheat.fs).
// ----------------------------------------------------------------------------
//  A single "flexible" scheme: uSnowCover 0..1 grows during a snowfall (fx.c),
//  uSnowMask is a baked "sky visibility" map (unit 8, RG8: R = top is open).
//  Noise from img/noise.png (uNoise) defines accumulation patches: snow falls
//  UNEVENLY, but as uSnowCover grows the threshold drops and the cover
//  gradually (slowly, minutes) fills the ENTIRE open ground.
//
//  Each shader sets before calling: wp = world point, up = "upness" 0..1
//  (N.y for volumetric surfaces, vUV "up" for billboards). The rest is shared.
// ============================================================================

// Returns the snow cover weight 0..1 for point wp (up — "upness" 0..1).
float snow_amount(vec3 wp, float up) {
    if (uSnowCover <= 0.001) return 0.0;
    // Openness to the sky: R channel of the mask (baked overhanging canopies).
    float smask = texture(uSnowMask, wp.xz / uWorldSize).r;
    // Three noise frequencies from img/noise.png: large drifts (~40 m), medium
    // patches (~8 m), fine edges (~1.6 m). Uneven deposition.
    float sn1 = texture(uNoise, wp.xz / 40.0).r;
    float sn2 = texture(uNoise, wp.xz / 8.0 + 12.34).r;
    float sn3 = texture(uNoise, wp.xz / 1.6 - 5.7).r;
    // The accumulation threshold drops as uSnowCover grows: first snow lies
    // only in "drift-sized" holes (sn1>0.6), then (uSnowCover->1) the threshold
    // goes below zero — the ENTIRE open area gets covered.
    float thr = 0.85 - uSnowCover * 1.55;      // 0.85 .. -0.70
    float acc = smoothstep(thr, thr + 0.45, sn1 * 0.72 + sn2 * 0.28);
    // Patch edges waver with the fine noise and "creep" when melting.
    acc *= smoothstep(thr + 0.05, thr + 0.5, sn3);
    // Snow lies only on up-oriented faces and where the sky is open.
    return acc * up * smask;
}

// Snow color: slightly cold, with noisy micro-relief (drifts are brighter).
vec3 snow_color(vec3 base, vec3 wp, float a) {
    float sn1 = texture(uNoise, wp.xz / 40.0).r;
    float sn3 = texture(uNoise, wp.xz / 1.6 - 5.7).r;
    vec3 col = vec3(0.92, 0.94, 1.00) * (0.86 + 0.10 * sn1 + 0.05 * sn3);
    // Do not fully smother the underlying surface: a thin snow film keeps its
    // tint (grass stays green under the first dusting).
    return mix(base, mix(base * 0.85, col, 0.94), a);
}
