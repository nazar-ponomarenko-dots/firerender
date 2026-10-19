// ============================================================================
//  fog_temporal.fs — temporal accumulation for the half-res fog pass
// ----------------------------------------------------------------------------
//  The fog march (volumetric.fs) is jittered ANIMATEDLY (a new phase each
//  frame): in a single frame that is grain, and the eye sees it as flicker
//  on the semi-transparent fog. Here the previous fog frame is blended with
//  the current one — the noise averages out over several frames and the fog
//  becomes stable.
//  Anti-ghosting: the history is CLAMPED into the AABB of the CURRENT frame's
//  3x3 neighbours — when the camera/scene moves, the trail never escapes the
//  local color spread.
//  RGBA8 texture of the same half size (fog color + translucency in alpha).
// ============================================================================

in vec2 vUV;

uniform sampler2D uCur;    // this frame's fog march (half res)
uniform sampler2D uPrev;   // accumulated previous frame
uniform float uBlend;      // history weight (0.8 = ~5 frames of averaging; 0 = off)

out vec4 FragColor;

void main() {
    vec4 c = texture(uCur, vUV);
    vec2 ts = 1.0 / vec2(textureSize(uCur, 0));
    // AABB of the current frame's neighbours (3x3): allowed range for the history
    vec4 mn = c, mx = c;
    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            if (x == 0 && y == 0) continue;
            vec4 s = texture(uCur, vUV + vec2(float(x), float(y)) * ts);
            mn = min(mn, s);
            mx = max(mx, s);
        }
    }
    vec4 h = clamp(texture(uPrev, vUV), mn, mx);
    FragColor = mix(c, h, uBlend);
}
