// ============================================================================
//  SSR — Screen Space Reflections
// ----------------------------------------------------------------------------
//  Post pass after the scene: reads scene color and depth, reconstructs the
//  world position and normal (from neighbor depths), reflects the view ray and
//  marches in screen space. The step grows with distance (racing off far away
//  quickly); after a find — binary refinement of the hit (no «sliding»).
//  A miss = the sky cubemap (uCube) — wet surfaces reflect the sky even where
//  the screen is empty. Everything is gated by wetness uWetness (rain),
//  up-orientation, Fresnel and the terrain wetness mask (under canopies — dry).
//
//  Outputs SCENE + REFLECTION (into its own FBO), then through the pipeline
//  as usual (bloom -> fog).
// ============================================================================

in vec2 vUV;

uniform sampler2D uScene;       // scene color (unit 0)
uniform sampler2D uDepth;       // scene depth (unit 1)
uniform samplerCube uCube;      // sky cubemap (unit 2) — reflection fallback
uniform sampler2D uWetMask;     // terrain wetness mask (unit 3)
uniform sampler2D uPrev;        // previous frame scene+reflection (unit 4, anti-flicker)
uniform mat4 uViewProj;         // world -> screen (projecting the reflected ray)
uniform mat4 uPrevViewProj;     // world -> screen of the previous frame (reprojection of uPrev)
uniform mat4 uInvViewProj;      // screen -> world (positions and normals)
uniform vec3 uCamPos;
uniform vec2 uTexel;            // 1 / screen resolution
uniform vec2 uWorldSize;        // world size (m): uv of the wetness mask
uniform vec4 uParams;           // x steps, y range, z thickness, w roughness
uniform vec4 uMisc;             // x znear, y zfar, z fallback on, w mask on
uniform float uDebug;           // 0 off, 1 gate, 2 hit/fallback, 3 weight, 4 reflection color
uniform float uWetness;         // wetness 0..1 (gates the whole effect)
uniform float uStrength;        // reflection strength (0..1)
uniform float uBlend;           // temporal weight of the previous frame (0 = off)

out vec4 FragColor;

// Sanitizer: NaN/Inf/negative values in the RGBA16F target. Their source is
// division by ~0 in the normal/position reconstruction right at the near plane
// (under the player's feet) and in the march. Without this guard, garbage got
// into the temporal smoothing HISTORY and spread across the screen as black
// spots every frame (turning the camera «healed» it, because the history reset).
vec3 sane(vec3 c) {
    c = mix(c, vec3(0.0), vec3(isnan(c.x) || isinf(c.x),
                               isnan(c.y) || isinf(c.y),
                               isnan(c.z) || isinf(c.z)));
    return clamp(c, vec3(0.0), vec3(64.0));
}

// World position from uv and a depth pixel (technique from volumetric.fs)
vec3 wpos(vec2 uv, float depth) {
    vec4 ndc = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 wp = uInvViewProj * ndc;
    return wp.xyz / wp.w;
}

// Linear (view-space) depth from NDC-z: monotonic and in meters —
// comparing the ray and the surface is more reliable than in raw NDC.
float linz(float ndc_z) {
    float zn = uMisc.x, zf = uMisc.y;
    return (2.0 * zn * zf) / (zf + zn - ndc_z * (zf - zn));
}

// Normal from depth: two tangent vectors to the neighbors. We take THE neighbor
// whose depth is closer (per axis) — on silhouette boundaries the normal does
// not smear into streaks.
vec3 wnormal(vec2 uv, float d0, vec3 p0) {
    float dR = texture(uDepth, uv + vec2(uTexel.x, 0.0)).r;
    float dL = texture(uDepth, uv - vec2(uTexel.x, 0.0)).r;
    vec2 offx = (abs(dR - d0) < abs(dL - d0)) ? vec2(uTexel.x, 0.0) : vec2(-uTexel.x, 0.0);
    float dx = (abs(dR - d0) < abs(dL - d0)) ? dR : dL;
    float dU = texture(uDepth, uv + vec2(0.0, uTexel.y)).r;
    float dD = texture(uDepth, uv - vec2(0.0, uTexel.y)).r;
    vec2 offy = (abs(dU - d0) < abs(dD - d0)) ? vec2(0.0, uTexel.y) : vec2(0.0, -uTexel.y);
    float dy = (abs(dU - d0) < abs(dD - d0)) ? dU : dD;
    vec3 px = wpos(uv + offx, dx);
    vec3 py = wpos(uv + offy, dy);
    vec3 n = cross(px - p0, py - p0);
    float l = length(n);
    return l > 1e-6 ? n / l : vec3(0.0, 1.0, 0.0);
}

// Project a ray point (t — distance along the ray from the origin). ok = 0 if
// the point is behind the camera or NDC is outside the frustum. rayZ — the point's linear depth.
vec2 marchUV(vec3 ro, vec3 rd, float t, out float rayZ, out float ok) {
    vec4 clip = uViewProj * vec4(ro + rd * t, 1.0);
    if (clip.w <= 0.02) { ok = 0.0; rayZ = 0.0; return vec2(0.0); }
    vec3 ndc = clip.xyz / clip.w;
    rayZ = linz(ndc.z);
    vec2 uv = ndc.xy * 0.5 + 0.5;
    ok = (abs(ndc.z) < 1.0 && uv.x > 0.0 && uv.x < 1.0 && uv.y > 0.0 && uv.y < 1.0)
         ? 1.0 : 0.0;
    return uv;
}

// Reflection color: at low roughness a SINGLE sharp tap (no blur — grass is
// readable in the reflection), blur appears only at rough well above that.
vec3 blurCol(vec2 uv, float rough) {
    vec3 c = texture(uScene, uv).rgb;
    if (rough <= 0.10) return c;
    vec2 o = uTexel * rough * 5.0;
    c += texture(uScene, uv + vec2( o.x, 0.0)).rgb;
    c += texture(uScene, uv + vec2(-o.x, 0.0)).rgb;
    c += texture(uScene, uv + vec2(0.0,  o.y)).rgb;
    c += texture(uScene, uv + vec2(0.0, -o.y)).rgb;
    return c * 0.2;
}

void main() {
    vec3 scene = sane(texture(uScene, vUV).rgb);
    float d0 = texture(uDepth, vUV).r;
    // Sky (depth ~ 1: the skybox writes no depth) — nothing to reflect
    if (d0 > 0.9999) {
        FragColor = vec4(mix(scene, vec3(0.0), step(0.5, uDebug)), 1.0);
        return;
    }
    // Pixels right at the near plane (underfoot, «point-blank»): reconstructing
    // the world point and normal from depth degenerates there — nan/inf and black
    // dots. Reflections on such a surface are invisible anyway.
    if (d0 < 1e-5) {
        FragColor = vec4(scene, 1.0);
        return;
    }

    vec3 P = wpos(vUV, d0);
    vec3 N = wnormal(vUV, d0, P);
    vec3 V = normalize(uCamPos - P);
    if (dot(N, V) < 0.0) N = -N;
    vec3 R = reflect(-V, N);

    // Wetness: global + terrain mask (under tree canopies it is dry)
    float wet = clamp(uWetness, 0.0, 1.0);
    if (uMisc.w > 0.5)
        wet *= smoothstep(0.2, 0.8, texture(uWetMask, P.xz / uWorldSize).r);

    // Gates: the surface faces up, the ray LEAVES it (not self-into-itself),
    // Fresnel (more mirror-like toward the horizon), screen edges fade out.
    float up = smoothstep(0.15, 0.70, N.y);
    float outS = smoothstep(0.0, 0.20, dot(R, N));
    // Fresnel with a FLOOR of 0.35 — a wet surface visibly reflects even at a
    // straight-on angle, not only near the horizon (the old pow5 gave ~9%).
    float fres = mix(0.35, 1.0, pow(1.0 - clamp(dot(N, V), 0.0, 1.0), 5.0));
    float ef = smoothstep(0.0, 0.05, vUV.x) * (1.0 - smoothstep(0.95, 1.0, vUV.x))
             * smoothstep(0.0, 0.05, vUV.y) * (1.0 - smoothstep(0.95, 1.0, vUV.y));
    float gate = up * outS * ef * wet * uStrength;
    bool live = wet > 0.02 && gate > 0.003;
    if (!live) {
        if (uDebug > 0.5 && uDebug < 1.5) FragColor = vec4(vec3(0.0, gate, 0.0), 1.0);
        else FragColor = vec4(scene, 1.0);
        return;
    }

    // March in WORLD SPACE with an intersection test: the reflected ray
    // starts on the surface and flies up-and-away (dot(R,N) > 0 — the outS gate
    // guarantees that). A hit — when the ray crosses from ABOVE the surface to
    // BELOW it (was above, now below) in world Y, then binary refinement —
    // the reflection is flipped vertically and lands on the silhouette instead
    // of «sliding».
    // Protection against false hits of distant objects (a forest 500 m away in
    // a puddle at your feet): perspective compression projects a far tree onto
    // the same uv where the ray is only a few meters from P; so a hit is
    // accepted only if the surface at the march point lies NEAR the current ray
    // position.
    // minPix (screen pixels) additionally trims the closest self-hit at the source.
    int steps = int(uParams.x);
    float maxd = uParams.y;
    float thick0 = uParams.z;
    float rough = uParams.w;
    float minPix = 3.0;

    float tPrev = 0.0;
    float t = 0.20;
    bool wasAbove = true;
    vec2 uvHit = vec2(-1.0);
    float fade = 0.0;
    float hitT = 0.0;
    for (int i = 0; i < steps; i++) {
        float ok, rayZ;
        vec2 suv = marchUV(P, R, t, rayZ, ok);
        if (ok < 0.5) break;                       // off screen/camera — a miss
        float sd = texture(uDepth, suv).r;
        bool above;
        if (sd > 0.9999) {                          // sky: empty, the ray keeps flying
            above = true;
        } else {
            vec3 surf = wpos(suv, sd);
            vec3 rp = P + R * t;                    // world point of the ray
            bool below = rp.y < surf.y - thick0 * 0.25;
            vec2 farPix = abs(suv - vUV) / uTexel;
            bool farOk = max(farPix.x, farPix.y) > minPix;
            // The surface must be near the ray (not a forest 500 m away
            // projected onto this uv).
            bool nearOk = distance(surf, P) < max(4.0, t * 2.5);
            if (wasAbove && below && farOk && nearOk) {
                // Intersection: binary refinement in world Y (same test).
                float a = tPrev, b = t;
                vec2 buv = suv;
                for (int r2 = 0; r2 < 5; r2++) {
                    float m = (a + b) * 0.5;
                    float ok2, rz2;
                    vec2 muv = marchUV(P, R, m, rz2, ok2);
                    bool mBelow = false;
                    if (ok2 > 0.5) {
                        float sd2 = texture(uDepth, muv).r;
                        if (sd2 < 0.9999) {
                            vec3 s2 = wpos(muv, sd2);
                            mBelow = (P + R * m).y < s2.y - thick0 * 0.25
                                  && distance(s2, P) < max(4.0, m * 2.5);
                        }
                    }
                    if (mBelow) { b = m; buv = muv; } else { a = m; }
                }
                uvHit = buv;
                hitT = b;
                break;
            }
            above = !below;
        }
        wasAbove = above;
        tPrev = t;
        t += mix(0.20, 2.5, clamp(t / maxd, 0.0, 1.0)) * (1.0 + rough * 0.5);
    }

    vec3 refl;
    float mixw;
    if (uvHit.x >= 0.0) {
        // On-screen hit: sharp scene color (one tap — grass is readable in the
        // reflection; blur only at high roughness) + fades.
        refl = blurCol(uvHit, rough);
        float fade = 1.0 - smoothstep(maxd * 0.55, maxd, hitT);
        mixw = gate * (0.70 + 0.30 * fres) * fade * (1.0 - rough * 0.30);
    } else if (uMisc.z > 0.5) {
        // Miss: sky/environment from the cubemap — a puddle does not go black
        // when there is nothing to reflect on screen (like water via uReflCube).
        refl = texture(uCube, R).rgb;
        mixw = gate * (0.10 + 0.60 * fres);
    } else {
        FragColor = vec4(scene, 1.0);
        return;
    }

    float mixw_clamped = clamp(mixw, 0.0, 1.0);
    // Debug: tint zones instead of blending, to see where SSR lives
    if (uDebug > 0.5) {
        if (uDebug < 1.5) {
            // 1: gate strength (green) — where SSR can draw at all
            FragColor = vec4(vec3(0.0, gate, 0.0), 1.0);
        } else if (uDebug < 2.5) {
            // 2: hit=red, fallback(cubemap)=blue, neither=black
            vec3 dbg = uvHit.x >= 0.0 ? vec3(1.0, 0.0, 0.0)
                     : (uMisc.z > 0.5 ? vec3(0.0, 0.0, 1.0) : vec3(0.0));
            FragColor = vec4(dbg, 1.0);
        } else if (uDebug < 3.5) {
            // 3: final reflection weight mixw (white) — how much will be visible
            FragColor = vec4(vec3(mixw_clamped), 1.0);
        } else {
            // 4: pure reflection color — checking the «mirror-ness»
            FragColor = vec4(refl, 1.0);
        }
        return;
    }

    vec3 outc = mix(scene, sane(refl), mixw_clamped);

    // Temporal smoothing (anti-flicker): rebuild the pixel's world point and
    // reproject it into the previous frame (uPrevViewProj). There take the
    // previous frame (scene+reflection) and blend with weight uBlend. Reprojecting
    // in world space is robust to small camera/ray jitter; if uv left the screen
    // or the history is not ready yet (uBlend=0) — just the current frame.
    // The history is ALSO sanitized: without this one garbage pixel spread
    // across the screen (each frame blended with itself).
    if (uBlend > 0.001) {
        vec4 pc = uPrevViewProj * vec4(P, 1.0);
        if (pc.w > 0.02) {
            vec2 puv = pc.xy / pc.w * 0.5 + 0.5;
            if (puv.x > 0.003 && puv.x < 0.997 && puv.y > 0.003 && puv.y < 0.997) {
                vec3 prev = sane(texture(uPrev, puv).rgb);
                // Depth gate: the history must be the same surface,
                // otherwise moving objects dragged a trail behind them.
                float pd = texture(uDepth, puv).r;
                float ok = (pd < 0.9999 && pd > 1e-5) ? 1.0 : 0.0;
                outc = mix(outc, prev, uBlend * ok);
            }
        }
    }

    FragColor = vec4(sane(outc), 1.0);
}
