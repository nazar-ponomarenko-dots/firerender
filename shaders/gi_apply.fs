// ============================================================================
//  gi_apply.fs — applying GI to the scene (with bilateral upsample)
// ----------------------------------------------------------------------------
//  The scene is already lit by direct light and hemispheric ambient. GI adds:
//    * indirect REFLECTED light (bounce from grass, from asphalt, from walls);
//    * shading — contact and large-scale (sky visibility).
//
//      add   = albedo(scene) * gi * strength
//      add   = min(add, max(scene, neighbors) + FLOOR)  <-- energy conservation
//      color = scene * ao + add
//
//  MAIN RULE (v7): the addition is limited by the LOCAL brightness of the
//  neighborhood, not of the pixel itself. Previously the cap was "pixel
//  brightness + threshold" — and a lamp reflection in shadow (a dark pixel
//  next to a lit wall) got cut down to the threshold: re-reflections of light
//  sources died out. Now the cap takes the max brightness from 4 neighbors of GI
//  size: light CAN flow from a lit surface into an adjacent shadow (physics!), but
//  not from a bright spot into the center of a dark room (global glow is still impossible).
//
//  albedo — the surface tint, normalized to ~0.5 (typical diffuse albedo).
//  The falloff on bright pixels is soft and not to zero, otherwise indirect
//  light disappears exactly where it creates the "play of light".
//
//  Shading is applied to direct light PARTIALLY: screen-space GI in dense
//  geometry (grass, field, foliage) always overestimates occlusion, and full
//  strength inks lit surfaces. On a bright pixel the shading is weakened; in
//  niches and crevices it works at full strength.
//
//  GI lives at half resolution (r_gi_scale), and plain bilinear sampling
//  drags light across silhouettes: a halo one GI-target texel wide appears
//  around objects (at 0.5 that is 2 screen pixels). Instead of it here is a
//  joint bilateral upsample: the 4 nearest GI texels are weighted by depth
//  match at FULL resolution. It costs 4 depth samples per pixel, and an
//  object edge receives GI only from its "own" side of the silhouette.
// ============================================================================

in vec2 vUV;

uniform sampler2D uScene;    // scene (unit 0)
uniform sampler2D uGI;       // GI: rgb indirect light, a = shading (unit 1)
uniform sampler2D uDepth;    // depth (unit 2)
uniform vec2 uGiTexel;       // 1 / GI target resolution
uniform vec2 uZ;             // znear, zfar
uniform float uStrength;     // indirect light strength
uniform float uAoStrength;   // shading strength 0..1
uniform float uDebug;        // 0 off, 1 GI only, 2 shading only

out vec4 FragColor;

#define LUMA vec3(0.2126, 0.7152, 0.0722)

vec3 sane3(vec3 c) {
    c = mix(c, vec3(0.0), vec3(isnan(c.x) || isinf(c.x),
                               isnan(c.y) || isinf(c.y),
                               isnan(c.z) || isinf(c.z)));
    return clamp(c, vec3(0.0), vec3(64.0));
}

float linz(float ndc_z) {
    float zn = uZ.x, zf = uZ.y;
    return (2.0 * zn * zf) / (zf + zn - ndc_z * (zf - zn));
}

// 4 nearest GI texels with a depth-match weight. Weights are bilinear times
// the bilateral factor; on complete mismatch (silhouette) the nearest texel
// remains, not a blend of two surfaces.
vec4 upsample(vec2 uv, float z0) {
    vec2 tc = uv / uGiTexel - 0.5;
    vec2 base = floor(tc);
    vec2 f = tc - base;
    vec4 acc = vec4(0.0);
    float wsum = 0.0;
    float best_w = -1.0;
    vec4 best = vec4(0.0);
    for (int i = 0; i < 4; i++) {
        vec2 o = vec2(float(i & 1), float(i >> 1));
        vec2 suv = (base + o + 0.5) * uGiTexel;
        float bw = (o.x == 0.0 ? 1.0 - f.x : f.x) *
                   (o.y == 0.0 ? 1.0 - f.y : f.y);
        float sd = texture(uDepth, suv).r;
        float dz = (sd >= 0.9999 || sd <= 1e-5)
                   ? 1e9 : abs(linz(sd * 2.0 - 1.0) - z0);
        // The tolerance grows with distance: far away a texel covers more world.
        float tol = 0.05 + 0.03 * z0;
        float w = bw * (1.0 - smoothstep(tol, tol * 5.0, dz));
        vec4 c = texture(uGI, suv);
        if (bw > best_w) { best_w = bw; best = c; }   // fallback: nearest
        acc += c * w;
        wsum += w;
    }
    return wsum > 1e-4 ? acc / wsum : best;
}

void main() {
    vec3 sc = sane3(texture(uScene, vUV).rgb);
    float d = texture(uDepth, vUV).r;
    if (d >= 0.9999 || d <= 1e-5) { FragColor = vec4(sc, 1.0); return; }

    vec4 gi = upsample(vUV, linz(d * 2.0 - 1.0));
    vec3 ind = sane3(gi.rgb);
    float ao = (isnan(gi.a) || isinf(gi.a)) ? 1.0 : clamp(gi.a, 0.0, 1.0);
    // Shading strength up to 1.5: values above 1 push the shadow deeper than
    // zero (in niches — to pure black); the result is clamped to 0..1.
    ao = clamp(mix(1.0, ao, clamp(uAoStrength, 0.0, 1.5)), 0.0, 1.0);

    if (uDebug > 1.5) { FragColor = vec4(vec3(ao), 1.0); return; }
    if (uDebug > 0.5) { FragColor = vec4(ind * uStrength, 1.0); return; }

    // Albedo proxy: the surface tint with brightness ~0.5. A reflection is
    // tinted with the material color and stays physical in energy.
    float l = max(dot(sc, vec3(0.2126, 0.7152, 0.0722)), 1e-4);
    vec3 albedo = clamp(sc * (0.5 / l), vec3(0.0), vec3(1.0));
    // Falloff on bright pixels — soft and not to zero: indirect light stays
    // visible on lit surfaces too (that is where it creates the "play of light").
    float room = mix(1.0, 0.35, smoothstep(0.5, 3.0, l));

    // Shading is applied to direct light almost entirely. The weakening on
    // BRIGHT pixels is minimal (0.45 -> 0.25 in v7.1): strong relaxation made
    // contact shadows barely visible in sunlight. The remainder is needed only
    // against occlusion overestimation in dense grass/foliage.
    float lit = smoothstep(0.15, 0.9, l);
    float ao_direct = mix(ao, mix(ao, 1.0, 0.25), lit);

    // --- ENERGY CONSERVATION LAW (local) --------------------------------
    // Cap of the addition = the brightest of the neighborhood (4 neighbors of
    // GI size around the pixel) scaled down + FLOOR. Light from a lamp/sun,
    // re-reflected by a lit surface into an adjacent shadow, passes entirely
    // (neighbors are bright), while "glow from nowhere" in a dark room — does
    // not (all neighbors are dark). FLOOR = 0.10 lifts deep shadows: they show
    // sky light and colored bounce instead of a black hole.
    // The limit is on BRIGHTNESS (preserving the tint of the addition): a
    // colored reflection stays colored, not cut to gray.
    const float GI_FLOOR = 0.10;
    vec3 add = albedo * ind * (uStrength * room);
    float add_l = dot(add, LUMA);
    float cap = max(l, 0.0);
    for (int i = 0; i < 4; i++) {
        vec2 o = vec2(float(i & 1) * 2.0 - 1.0, float(i >> 1) * 2.0 - 1.0);
        vec3 nc = sane3(texture(uScene, vUV + o * uGiTexel * 1.5).rgb);
        cap = max(cap, dot(nc, LUMA));
    }
    cap = cap * 0.85 + GI_FLOOR;
    if (add_l > cap) add *= cap / add_l;

    vec3 col = sc * ao_direct + add;
    FragColor = vec4(sane3(col), 1.0);
}
