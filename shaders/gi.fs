// ============================================================================
//  gi.fs — dynamic global illumination (diffuse GI + AO), v7
// ----------------------------------------------------------------------------
//  Computed at reduced resolution into RGBA16F:
//    rgb = INDIRECT light: bounce off this frame's geometry (including lamp
//          and sun light — it is already baked into the scene color) + SKY
//          light on ray misses + specular reflection along the view direction,
//    a   = occlusion 0..1 (1 = open), two scales packed into one value.
//
//  v7 — SKY LIGHT (UE5 Skylight) and ANTI-FLICKER. A ray miss is no longer
//  empty: a ray that escapes into the sky carries the REAL panorama color for
//  its direction (same mapping as in sky.fs) — shade under a tree is bluish,
//  warm at sunset, grey in rain. This is the main source of "air" and light
//  play in Lumen. Below the horizon — a bounce off the ground beyond the
//  screen edge.
//   * history reprojection no longer discards itself on a depth mismatch:
//     a 12% weight floor remains (the trail is invisible, but the noise pulse
//     while the camera moves disappears — shadows stop flickering in motion);
//   * optimization: 6x10 rays (was 8x12, -40% tracing) — the hemisphere is
//     rebuilt by IGN pixel dithering + accumulation + denoise;
//   * optimization: the mirror ray is not marched when its weight < 0.05
//     (head-on view at a barely glossy surface) — up to 75% of its cost.
//
//  v6 — ENERGY CONSERVATION. All light returned by a hit point
//  (direct + accumulated indirect) is multiplied by ONE albedo GI_ALBEDO < 1
//  and divided by (1 + spread): indirect light physically CANNOT become
//  brighter than the input, while the bounce chain still works.
//
//  Performance (without image quality loss):
//   * the march starts right past the bias: previously the first quadratic
//     layout samples fell below the bias and were dropped via continue — up to
//     a third of the steps went to waste. Now EVERY step works (same cost,
//     better quality);
//   * spread is not sampled at far LOD (not visible there);
//   * the reflection marches 3/4 of the diffuse ray's steps.
// ============================================================================

in vec2 vUV;

uniform sampler2D uScene;        // scene color (unit 0), RGBA16F
uniform sampler2D uDepth;        // scene depth (unit 1)
uniform sampler2D uPrev;         // previous frame's GI (unit 2) — accumulation
uniform mat4 uViewProj;
uniform mat4 uInvViewProj;
uniform mat4 uPrevViewProj;
uniform vec3 uCamPos;
uniform vec2 uTexel;             // 1 / resolution of THIS target
uniform vec4 uParams;            // x rays, y steps, z GI radius (m), w znear
uniform vec4 uParams2;           // x zfar, y sky weight, z phase, w AO radius (m)
uniform vec4 uParams3;           // x saturation, y reflection, z spread
uniform vec3 uGroundColor;       // "underside" color (ray went down past all geometry)
uniform float uSnowCover;        // snow cover 0..1 — ground bounce whitens
uniform float uClamp;            // GI energy cap per pixel
uniform float uBlend;            // history weight 0..0.96
uniform sampler2D uSky;          // sky panorama (unit 3), same as in sky.fs
uniform vec3 uSkyTint;           // sky tint from map.json
uniform vec3 uSkyFog;            // horizon color (fog)
uniform vec2 uSkyLight;          // x sky strength, y panorama available (0/1)

out vec4 FragColor;

vec3 sane3(vec3 c) {
    c = mix(c, vec3(0.0), vec3(isnan(c.x) || isinf(c.x),
                               isnan(c.y) || isinf(c.y),
                               isnan(c.z) || isinf(c.z)));
    return clamp(c, vec3(0.0), vec3(32.0));
}
float sane1(float v) {
    return (isnan(v) || isinf(v)) ? 0.0 : clamp(v, 0.0, 1.0);
}

vec3 wpos(vec2 uv, float depth) {
    vec4 ndc = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 wp = uInvViewProj * ndc;
    return wp.xyz / wp.w;
}

float linz(float ndc_z) {
    float zn = uParams.w, zf = uParams2.x;
    return (2.0 * zn * zf) / (zf + zn - ndc_z * (zf - zn));
}

// Normal from depth: the depth-nearest neighbor along each axis (stable on
// silhouettes). The step is one texel of the GI target: at half resolution
// the scene neighbors are too close and the normal drowns in depth noise.
// Degenerate case -> "up".
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

// Normal at the HIT point: two forward samples instead of four. Precision
// matters less there (the result feeds a cosine), and it is called for every
// successful ray — the saving is noticeable.
vec3 wnormal_fast(vec2 uv, vec3 p0) {
    vec2 tx = vec2(uTexel.x, 0.0);
    vec2 ty = vec2(0.0, uTexel.y);
    vec3 px = wpos(uv + tx, texture(uDepth, uv + tx).r);
    vec3 py = wpos(uv + ty, texture(uDepth, uv + ty).r);
    vec3 n = cross(px - p0, py - p0);
    float l = length(n);
    if (l < 1e-6 || isnan(l) || isinf(l)) return vec3(0.0, 1.0, 0.0);
    n /= l;
    if (dot(n, uCamPos - p0) < 0.0) n = -n;
    return n;
}

// Interleaved gradient noise: gives near-uniformly distributed values across
// neighboring pixels (unlike white noise). This is exactly what lets the
// denoiser build a full hemisphere estimate from 4-8 neighbors.
float ign(vec2 p) {
    return fract(52.9829189 * fract(0.06711056 * p.x + 0.00583715 * p.y));
}

// Saturation: emphasizes the hue of reflected light while preserving luminance.
vec3 saturate_hue(vec3 c, float s) {
    float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
    return max(mix(vec3(l), c, s), vec3(0.0));
}

// --- Skylight ----------------------------------------------------------------
// Sky radiance along the ray direction. The sky panorama is the TEXTURE the
// skybox itself is drawn from (sky.fs), and the mapping is identical: shadows
// and sky match in color automatically in any weather and time of day. The
// sun disc is kept out of GI: it is already in the scene's direct light, and
// a bright dot in GI means fireflies. No panorama (main menu) — a gradient
// built from the fog color.
vec3 sky_radiance(vec3 dir) {
    float up = clamp(dir.y, -1.0, 1.0);
    if (uSkyLight.y < 0.5) {
        vec3 zen = uSkyFog * vec3(0.72, 0.82, 1.22);
        return mix(uSkyFog, zen, pow(clamp(up, 0.0, 1.0), 0.7));
    }
    float elev = asin(up);
    float u = 0.5 + atan(dir.z, dir.x) / 6.2831853;
    float v = clamp(0.5 - elev * 0.63661977, 0.0, 1.0);
    vec3 col = texture(uSky, vec2(u, v)).rgb * uSkyTint;
    // Above +45° there is no panorama — a dome from the top row (as in sky.fs).
    float zenT = smoothstep(0.785398163, 1.570796327, elev);
    col = mix(col, texture(uSky, vec2(u, 0.0)).rgb * uSkyTint, zenT);
    // Right at the horizon (and slightly below) — haze: distant land = fog color.
    col = mix(uSkyFog, col, smoothstep(-0.04, 0.06, dir.y));
    return col;
}

// Projects a world point onto the screen. false — point behind camera or off-frame.
bool project(vec3 wp, out vec2 uv, out float ray_z) {
    vec4 clip = uViewProj * vec4(wp, 1.0);
    uv = vec2(0.0);
    ray_z = 0.0;
    if (clip.w <= 0.02) return false;
    vec3 ndc = clip.xyz / clip.w;
    uv = ndc.xy * 0.5 + 0.5;
    ray_z = linz(ndc.z);
    return uv.x > 0.0 && uv.x < 1.0 && uv.y > 0.0 && uv.y < 1.0;
}

// March of a single ray through the depth buffer. Samples are distributed
// quadratically: dense near the surface (contact shadows) and out to the full
// radius at the end. On a successful step the interval is halved twice — the
// contact point lands on the surface.
//
// OPTIMIZATION: the layout starts RIGHT past the bias. Previously
// t = radius*f*f was measured from zero, and the first samples (at a small
// radius — up to a third of all steps) fell below the bias and were dropped
// via continue: wasted work, exactly where contact shadows are needed. Now
// the whole interval [bias, radius] is laid out — same cost, every step useful.
bool trace_ray(vec3 O, vec3 dir, float radius, int steps, float jit, float bias,
               out float hit_dist, out vec2 hit_uv, out float hit_sd) {
    hit_dist = radius;
    hit_uv = vec2(0.0);
    hit_sd = 0.0;
    float steps_f = float(steps);
    float span = max(radius - bias, 1e-3);
    float t_prev = bias;
    for (int s = 0; s < steps; s++) {
        float f = (float(s) + jit) / steps_f;
        float t = bias + span * f * f;
        vec2 uv; float ray_z;
        if (!project(O + dir * t, uv, ray_z)) return false;
        float sd = texture(uDepth, uv).r;
        if (sd < 0.9999 && sd > 1e-5) {
            float scene_z = linz(sd * 2.0 - 1.0);
            float diff = ray_z - scene_z;
            float front = 0.015 + 0.01 * scene_z;
            // The "thickness" window grows with the step length (otherwise the
            // ray skipped over the surface and the hit was discarded), but the
            // ceiling is tight: in grass and foliage the depth buffer is fine
            // detail, and a wide window turned "the ray passed beside a blade
            // of grass" into "the ray hit a wall". These false hits are what
            // blackened the field.
            float thick = min(max(0.10, (t - t_prev) * 1.6) + 0.02 * scene_z,
                              0.45 + 0.05 * scene_z);
            if (diff > front && diff < thick) {
                float lo = t_prev, hi = t;
                for (int k = 0; k < 2; k++) {
                    float mid = (lo + hi) * 0.5;
                    vec2 muv; float mz;
                    if (!project(O + dir * mid, muv, mz)) { hi = mid; continue; }
                    float msd = texture(uDepth, muv).r;
                    if (msd >= 0.9999 || msd <= 1e-5) { lo = mid; continue; }
                    if (mz - linz(msd * 2.0 - 1.0) > front) {
                        hi = mid;
                        uv = muv;
                        sd = msd;
                    } else {
                        lo = mid;
                    }
                }
                hit_dist = hi;
                hit_uv = uv;
                hit_sd = sd;
                return true;
            }
        }
        t_prev = t;
    }
    return false;
}

// Hit color with a brightness cap: otherwise a single overbright texel, after
// division by a small ray count, yields a persistent "firefly".
vec3 hit_color(vec2 uv) {
    vec3 hc = sane3(texture(uScene, uv).rgb);
    float hl = dot(hc, vec3(0.2126, 0.7152, 0.0722));
    return hl > uClamp ? hc * (uClamp / max(hl, 1e-4)) : hc;
}

// --- ENERGY CONSERVATION -----------------------------------------------------
// Diffuse bounce albedo. Real materials reflect 20..60% of light; 0.55 is the
// plausible upper bound. The factor is HARD < 1, so light only decreases with
// each bounce. It used to be 1.6 here (and spread was added on top) — the GI
// lit itself, hence "the whole scene floor glows".
#define GI_ALBEDO 0.55

// Light returned by the hit point: direct (scene) + accumulated indirect
// (the GI history at this point). The sum is normalized by (1 + spread), so
// adding the bounce chain does NOT increase total energy — it only
// redistributes it between "direct" and "arrived through the chain".
//   Steady-state value: G = alb*I / (1 + spread*(1 - alb)) < I.
// For no spread (0..1) value does indirect light ever become brighter than the input.
vec3 hit_radiance(vec2 huv, vec3 hp, float spread, float lod) {
    vec3 hc = hit_color(huv);
    // At far LOD we do not build the chain: it is invisible there, yet the sample costs.
    if (spread > 0.001 && lod < 0.6) {
        vec2 suv = huv;
        vec4 sp = uPrevViewProj * vec4(hp, 1.0);
        if (sp.w > 0.02) {
            vec2 t2 = (sp.xyz / sp.w).xy * 0.5 + 0.5;
            if (t2.x > 0.002 && t2.x < 0.998 &&
                t2.y > 0.002 && t2.y < 0.998) suv = t2;
        }
        hc = (hc + sane3(texture(uPrev, suv).rgb) * spread) / (1.0 + spread);
    }
    return hc * GI_ALBEDO;
}

void main() {
    float d0 = texture(uDepth, vUV).r;
    // Sky: carries no indirect light, no occlusion
    if (d0 >= 0.9999) { FragColor = vec4(0.0, 0.0, 0.0, 1.0); return; }
    // Near plane (right underfoot): reconstruction degenerates -> garbage
    if (d0 <= 1e-5)   { FragColor = vec4(0.0, 0.0, 0.0, 1.0); return; }

    vec3 P = wpos(vUV, d0);
    vec3 N = wnormal(vUV, d0, P);
    float pz = linz(d0 * 2.0 - 1.0);

    float radius = max(uParams.z, 0.1);
    float skyw = clamp(uParams2.y, 0.0, 1.0);
    float phase = uParams2.z;
    float aoRadius = max(uParams2.w, 0.05);
    float bounce = max(uParams3.x, 0.0);
    float reflw = max(uParams3.y, 0.0);
    float spread = max(uParams3.z, 0.0);

    // LOD by distance: full quality up close, cheaper far away. On screen this
    // is barely visible (perspective + fog), and it offloads the frame exactly
    // where there are the most pixels. The floors are lower than "honest" ones:
    // noise in the far field is finished off by the denoise (radius grows with
    // distance) and accumulation.
    float lod = smoothstep(25.0, 140.0, pz);
    int rays  = int(max(uParams.x * (1.0 - 0.5 * lod), 3.0));
    int steps = int(max(uParams.y * (1.0 - 0.4 * lod), 6.0));
    float rays_f = float(rays);

    // Hemisphere basis
    vec3 up = abs(N.y) < 0.95 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 T = normalize(cross(up, N));
    vec3 B = cross(N, T);

    // Ray start is offset along the normal: without it the first samples land
    // on the surface itself, "shadow" it, and the whole frame turns flat grey.
    float bias = 0.02 + 0.0015 * pz;
    vec3 O = P + N * bias;

    vec3 gi = vec3(0.0);
    float occ_near = 0.0;            // occlusion within the AO radius (contact)
    float occ_far = 0.0;             // occlusion at the full radius (large-scale)
    float wsum = 0.0;                // sum of cosine weights (normalization)

    // Rotation of the whole ray set: per pixel (IGN) + per frame (phase).
    float a0 = (ign(gl_FragCoord.xy) + phase * 0.618034) * 6.2831853;

    for (int r = 0; r < rays; r++) {
        float fr = (float(r) + 0.5) / rays_f;
        float ang = a0 + float(r) * 2.39996323;
        float rr = sqrt(fr);
        vec3 dir = normalize(T * (rr * cos(ang)) + B * (rr * sin(ang))
                             + N * sqrt(max(1.0 - fr, 0.0)));
        float ndl = max(dot(dir, N), 0.0);
        if (ndl <= 0.001) continue;
        wsum += ndl;

        // Jitter of the march's starting phase — removes banding rings.
        float jit = ign(gl_FragCoord.xy + vec2(float(r) * 23.0, phase * 7.0));

        float hd; vec2 huv; float hsd;
        if (trace_ray(O, dir, radius, steps, jit, bias, hd, huv, hsd)) {
            vec3 hp = wpos(huv, hsd);
            // Cosine on the RECEIVING side: a surface turned away from the
            // ray gives back no light. This is what makes indirect light
            // directional rather than a uniform tint. We blend instead of
            // taking outright — on silhouettes the depth-derived normal is
            // unreliable, and a hard cosine would carve holes in the lighting
            // there. Skipped at far LOD: 2 extra samples per ray don't pay off.
            float cosOut = 1.0;
            if (lod < 0.5)
                cosOut = mix(1.0, max(dot(wnormal_fast(huv, hp), -dir), 0.0), 0.7);

            // One diffuse bounce with albedo < 1 (see hit_radiance): an
            // occluded direction TRADES the sky light for the light reflected
            // off the surface, but returns STRICTLY LESS than the surface
            // itself received. The bounce chain (spread) is already inside and
            // normalized — total energy does not grow.
            gi += hit_radiance(huv, hp, spread, lod)
                * (ndl * cosOut / (1.0 + hd * 0.3));
            // Two occlusion scales from ONE march — no extra rays needed.
            occ_near += ndl * (1.0 - smoothstep(0.0, aoRadius, hd));
            occ_far  += ndl;
        } else {
            // MISS — skylight (UE5 Skylight). The shade under a tree is lit by
            // the sky color TOWARD the side the ray points at: this gives a
            // directionality that no ambient can. Below the horizon the ray
            // would "see" the ground beyond the screen edge — a bounce off it
            // (ground albedo ~0.3 x sun ~1.2; under snow the ground is bright
            // and cold — the bounce whitens with snowcover).
            vec3 sky = sky_radiance(dir);
            vec3 bounce = mix(uGroundColor * 1.35, vec3(0.75, 0.80, 0.92),
                              clamp(uSnowCover, 0.0, 1.0));
            vec3 miss = mix(bounce, sky, smoothstep(-0.12, 0.06, dir.y));
            gi += miss * (ndl * uSkyLight.x);
            // occ_far (openness) untouched: a miss = the direction is open.
        }
    }

    // Normalize by the actual sum of cosine weights + hue saturation: reflected
    // light must carry the source's color (green of grass, warmth of brick) —
    // that is exactly what reads as "light bounces". Saturation preserves
    // luminance (see saturate_hue), so it adds no energy.
    float inv = 1.0 / max(wsum, 1e-4);
    vec3 col = saturate_hue(sane3(gi * inv), bounce);

    // --- Mirror reflection ---------------------------------------------------
    // One ray along the reflected view. Unlike SSR it works on any surface
    // (SSR only kicks in with wetness) and costs a single ray.
    // Disabled at far LOD: the reflection is not readable there.
    // ENERGY: factor = reflw (<=1) * fresnel (<=0.95) * albedo inside
    // hit_radiance — the reflection cannot overbrighten the reflecting surface.
    // 3/4 of the diffuse steps: the reflection grazes along the surface and
    // hits quickly, the difference is not visible.
    if (reflw > 0.001 && lod < 0.7) {
        vec3 V = normalize(P - uCamPos);
        vec3 R = reflect(V, N);
        if (dot(R, N) > 0.05) {
            // Fresnel: grazing reflections are stronger, head-on weaker.
            float fres = pow(1.0 - max(dot(-V, N), 0.0), 3.0);
            // OPTIMIZATION: final reflection weight < 0.05 (head-on view at a
            // matte surface) — the ray is not marched at all. Most pixels are
            // like this, saving up to ~75% of the reflection's cost; the eye
            // cannot see the difference (weight below the visibility threshold).
            if (reflw * (0.25 + 0.70 * fres) > 0.05) {
                float jitr = ign(gl_FragCoord.xy + vec2(91.0, phase * 13.0));
                int rsteps = int(max(float(steps) * 0.75, 5.0));
                float hd; vec2 huv; float hsd;
                if (trace_ray(O, R, radius, rsteps, jitr, bias, hd, huv, hsd)) {
                    // The reflection joins the spread on equal terms: a
                    // reflection sees both the surface's direct light and the
                    // indirect accumulated on it — but in total no more than
                    // what is actually there.
                    vec3 rc = hit_radiance(huv, wpos(huv, hsd), spread, lod);
                    // Max 0.95 — a reflection is always darker than what it reflects.
                    col += rc * (reflw * (0.25 + 0.70 * fres)
                                 / (1.0 + hd * 0.12));
                }
            }
        }
    }
    // Final ceiling is exactly uClamp (it used to be uClamp * 3). Indirect
    // light is never brighter than the energy cap set by r_gi_clamp.
    col = min(col, vec3(max(uClamp, 0.0)));

    // Occlusion: contact × large-scale (sky openness, skyw).
    // The second factor gives the "light play" — niches and under-canopy are
    // darker than open spots. The depth is MODERATE (it used to be 0.9/0.8
    // with a floor of 0.06 — the scene collapsed into black): contact up to
    // 0.62, far up to 0.5, floor 0.25. The far term is square-rooted — it
    // saturates smoothly instead of cutting to shadow on the very first hits.
    // In grass and foliage the depth buffer overstates occlusion, hence the
    // ceilings are deliberately below one.
    float near_f = clamp(occ_near * inv, 0.0, 1.0) * 0.62;
    float far_f  = sqrt(clamp(occ_far * inv, 0.0, 1.0)) * 0.5;
    float ao = clamp((1.0 - near_f) * (1.0 - far_f * skyw), 0.25, 1.0);

    // --- Temporal accumulation -----------------------------------------------
    vec4 res = vec4(col, ao);
    if (uBlend > 0.001) {
        vec4 pc = uPrevViewProj * vec4(P, 1.0);
        if (pc.w > 0.02) {
            vec2 puv = (pc.xyz / pc.w).xy * 0.5 + 0.5;
            if (puv.x > 0.002 && puv.x < 0.998 && puv.y > 0.002 && puv.y < 0.998) {
                vec4 prev = texture(uPrev, puv);
                prev = vec4(sane3(prev.rgb), sane1(prev.a));
                float pd = texture(uDepth, puv).r;
                float ok = 0.0;
                if (pd < 0.9999 && pd > 1e-5) {
                    float pzz = linz(pd * 2.0 - 1.0);
                    ok = 1.0 - smoothstep(0.05 * pz + 0.10, 0.30 * pz + 0.6,
                                          abs(pzz - pz));
                }
                // ANTI-FLICKER: a depth mismatch (disocclusion) used to ZERO
                // the history — in motion the surface blinked with each frame's
                // "fresh" noise. Now the history keeps a 12% weight floor: an
                // 8-frame trail (invisible), and the noise pulse disappears.
                float w = uBlend * (ok > 0.0 ? max(ok, 0.12) : 0.0);
                // Clamp the history around the current estimate. The upper
                // bound was lowered (2.5 instead of 5.0): a wide window left
                // the accumulation channel a loophole for brightness growth —
                // history pulled the pixel up, the next frame took it as a
                // base, and the glow kept piling up. Noise is still averaged,
                // but a gross mismatch (disocclusion) no longer drags along
                // another surface's trail.
                prev.rgb = clamp(prev.rgb, res.rgb * 0.2 - 0.1,
                                 res.rgb * 2.5 + 0.05);
                prev.a = clamp(prev.a, res.a - 0.35, res.a + 0.35);
                res = mix(res, prev, w);
            }
        }
    }
    FragColor = vec4(sane3(res.rgb), sane1(res.a));
}
