// ============================================================================
//  veg_common.glsl — shared vegetation geometry
// ----------------------------------------------------------------------------
//  Included from wheat.vs (color pass) and shadowveg.vs (shadow map).
//  Both shaders used to contain BYTE-FOR-IDENTICAL bending blocks for the
//  player; any divergence made the shadow slide off the plant. Now the
//  numbers live in a single place.
// ============================================================================

// ---------------------------------------------------------------------------
//  1) USEFUL HEIGHT OF THE QUAD (optimization without changing the picture)
// ---------------------------------------------------------------------------
//  On all vegetation sprites the TOP of the image is transparent and the
//  bottom is cropped at the last opaque pixel (in grass.png the first 41 rows
//  of 128 are empty, in wheat_lod0 — 18 of 178). The quad was built at full
//  image size, so the top 32% of its area rasterized only to hit discard.
//
//  veg_content_frac() returns the opaque fraction by height. Multiplying the
//  local vertex height by it (ay = aPos.y * cf) shortens the quad exactly to
//  the visible content: same pixels on screen, minus ~30% of grass fragments
//  and ~10% of wheat. The sprite bottom stays at the plant base —
//  the images have no transparent rows at the bottom.
//
//  Measurements over PNG alpha (threshold 0.5), rows first..last:
//    grass.png      41..127 of 128 -> 87/128
//    grass1.png     42..127        -> 86/128
//    grass2.png     37..127        -> 91/128
//    dandelions.png 26..127        -> 102/128
//    wheat_lod0     18..177 of 178 -> 160/178
//    wheat_lod1     10..177        -> 168/178
//    wheat_lod2     12..177        -> 166/178
float veg_content_frac(float layer)
{
    if (layer < 3.5) {                       // grass and dandelion: whole layer
        if (layer < 0.5) return 87.0  / 128.0;
        if (layer < 1.5) return 86.0  / 128.0;
        if (layer < 2.5) return 91.0  / 128.0;
        return 102.0 / 128.0;
    }
    // Wheat: the layer is CONTINUOUS (4..6, crossfade between LODs) — interpolate,
    // otherwise the ear height would jump at the transition.
    float t = clamp(layer - 4.0, 0.0, 2.0);
    return t < 1.0 ? mix(160.0 / 178.0, 168.0 / 178.0, t)
                   : mix(168.0 / 178.0, 166.0 / 178.0, t - 1.0);
}

// ---------------------------------------------------------------------------
//  2) BENDING UNDER THE PLAYER
// ---------------------------------------------------------------------------
//  The previous version shifted the vertex by METERS (up to 1.35 * scale),
//  so tall grass slid sideways further than its own length: the quad lay
//  flat and "splayed" into a fan. This defines a TILT ANGLE, and the vertex
//  travels along an arc around the base — the stem length is preserved.
#define BEND_MAX_A  0.8    // maximum angle from vertical, rad (~46°)
#define BEND_R_IN   0.30   // full-strength radius, m (player body)
#define BEND_R_FLY  0.75   // falloff radius while flying, m
#define BEND_R_WALK 1.15   // falloff radius while walking, m
// Flexibility by height: THE TALLER THE PLANT, THE STRONGER IT BENDS.
// The low stiff brush under the foot barely moves, a tall stem bows
// completely — that is how real vegetation behaves.
#define BEND_H_LO   0.10   // below this height (m) — minimum reaction
#define BEND_H_HI   0.80   // above this height — full reaction
#define BEND_H_MIN  0.12   // residual reaction for the shortest plants
// Vertical gate: full strength at the feet, no reaction above BEND_V_FAR. Without
// it, flying 50 m over the field would still trample the grass (the test was
// purely 2D).
#define BEND_V_NEAR 1.2
#define BEND_V_FAR  2.6

// Deflection angle of the plant from vertical for this vertex.
//   inst_xz/inst_y — plant base (world), cam — player,
//   vis            — VISIBLE plant height, m (affects flexibility),
//   ay             — local vertex height 0..1 (0 = base),
//   walk           — 1 when walking, 0 when flying.
// Returns 0 if there is no reaction: the caller skips the rotation.
float veg_bend_angle(vec2 inst_xz, float inst_y, vec3 cam, float vis, float ay,
                     float walk, out vec2 dir)
{
    vec2 to = inst_xz - cam.xz;
    float pd = length(to);
    dir = to / max(pd, 0.0001);
    if (ay <= 0.0) return 0.0;
    float push = 1.0 - smoothstep(BEND_R_IN, mix(BEND_R_FLY, BEND_R_WALK, walk),
                                  pd);
    if (push <= 0.0) return 0.0;
    float vgate = 1.0 - smoothstep(BEND_V_NEAR, BEND_V_FAR,
                                   abs(cam.y - inst_y));
    if (vgate <= 0.0) return 0.0;
    float flex = BEND_H_MIN + (1.0 - BEND_H_MIN)
               * smoothstep(BEND_H_LO, BEND_H_HI, vis);
    return BEND_MAX_A * push * push * vgate * flex * mix(0.7, 1.0, walk) * ay;
}
