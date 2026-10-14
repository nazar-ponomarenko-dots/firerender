// ============================================================================
//  veg_cull.cs — GPU-driven vegetation culling (compute pass)
// ----------------------------------------------------------------------------
//  One instance of the source buffer (Src) per invariant. For each plant:
//   1) frustum culling by a SPHERE circumscribed around the whole sprite (not
//      by its base point!) — otherwise a grass blade vanished entirely as soon
//      as its base left the screen edge while the sprite was still visible;
//   2) screen footprint estimate: if the sprite height in pixels is below
//      uMinPix — the plant is smaller than a pixel, it is invisible, drop it.
//  Survivors are written COMPACTLY into Dst (12 float each, like in the VBO),
//  and their count accumulates in an atomic counter directly in the
//  instanceCount field of the DrawElementsIndirect command (Indirect). Result:
//  the number of drawn vertices is bounded by the VISIBLE pixels, not density.
//
//  The sphere radius is computed with the SAME logic that grows a quad in
//  wheat.vs: wheat is stretched 3x in height, the quad is shortened to the
//  opaque part of the picture, and at far LOD the sprite STRETCHES in width
//  up to x10 (wx 128 -> 1280). Plus margin for wind and bending under player.
//  Conservatively: better draw extra than get a hole.
//
//  Instance format (12 float, as in wheat.vs / veg_gen_chunk):
//   [0..2] pos.xyz, [3..4] normal nx,nz, [5] yaw, [6] scale, [7] tint,
//   [8] layer (>=3.5 => wheat, x3 height), [9] lean, [10..11] slope.
// ============================================================================

layout(local_size_x = 64) in;

// Source instances (read only). src[] — flat array, 12 float per instance.
layout(std430, binding = 0) readonly buffer Src {
    float src[];
};
// Compact survivors (write). Same format as the instanced VBO.
layout(std430, binding = 1) writeonly buffer Dst {
    float dst[];
};
// Indirect draw command. instanceCount (offset 4) — the atomic counter.
layout(std430, binding = 2) buffer Indirect {
    uint cmd_count;          // = 6 (indices per quad)
    uint cmd_instanceCount;  // grows atomically -> how many to draw
    uint cmd_firstIndex;     // = 0
    uint cmd_baseVertex;     // = 0
    uint cmd_baseInstance;   // = 0
};

uniform mat4  uViewProj;     // view * projection (for the screen footprint estimate)
uniform vec4  uPlanes[6];    // frustum planes (world space, normals inward)
uniform vec3  uCamPos;       // camera (wheat LOD = same as in wheat.vs)
uniform float uViewH;        // viewport height in pixels (for the px estimate)
uniform float uTanHalfFov;   // tan(fov/2): world meters -> pixels
uniform float uMinPix;
uniform float uLodShift;      // wheat LOD thresholds shift (like wheat.vs)       // min. visible grass-blade footprint (px), typically 1.0
uniform int   uSrcCount;     // number of instances IN THIS dispatch (sub-range)
uniform int   uOffset;       // base index into Src (for the super-chunk sub-range)
uniform int   uDstCapacity;   // Dst capacity in instances

void main()
{
    uint gid = gl_GlobalInvocationID.x;
    if (int(gid) >= uSrcCount) return;
    uint i = uint(uOffset) + gid;     // absolute index into Src

    // Instance base address in the flat array (12 float).
    uint b = i * 12u;
    vec3 pos = vec3(src[b], src[b + 1u], src[b + 2u]);
    float scale = src[b + 6u];
    float layer = src[b + 8u];
    bool wheat = layer >= 3.5;

    // --- Sprite extents in the world (same math as in wheat.vs) ----------
    // Useful height: the quad is shortened to the opaque part of the picture
    // (veg_content_frac in veg_common.glsl). Take the MAX of the fraction over
    // sprite kinds (wheat lod1 = 168/178 ~ 0.944) so the sphere covers the sprite.
    float worldH = scale * (wheat ? 3.0 : 1.0) * 0.95;   // plant height, m
    float wmul = 1.0;                                // width stretch (LOD)
    if (wheat) {
        // thresholds as in wheat.vs (uLodShift: low density — earlier LOD)
        float b1a = 4.6, b1b = 5.4, b2a = 24.5, b2b = 25.5;
        if (uLodShift >= 0.5) { b1a = 0.0; b1b = 0.9; }
        if (uLodShift >= 1.5) { b2a = 3.0; b2b = 5.0; }
        float dist = length(pos.xz - uCamPos.xz);
        float ss1 = smoothstep(b1a, b1b, dist);
        float ss2 = smoothstep(b2a, b2b, dist);
        float wx = mix(128.0, 152.0, ss1);
        wx = mix(wx, 1280.0, ss2);                   // lod2 stretches x10
        wmul = wx / 128.0;
    }
    float halfW = 0.5 * scale * wmul;
    // Margin: wind (~0.11 m) + bending under the player. Bending is now a ROTATION
    // by an angle <= BEND_MAX_A (0.8 rad) around the base, not a shift by meters:
    // max displacement = sin(0.8) * worldH ~ 0.72 * worldH (was 1.4 * scale, i.e.
    // bigger than the blade itself). The sphere got tighter — fewer extra
    // instances pass the cull.
    float pad = 0.15 + 0.72 * worldH;
    // Sphere center — mid height of the sprite, the radius covers the whole quad.
    vec3 center = pos + vec3(0.0, worldH * 0.5, 0.0);
    float radius = sqrt(halfW * halfW + worldH * worldH * 0.25) + pad;

    // --- FRUSTUM CULLING by a SPHERE ----------------------------------------
    for (int p = 0; p < 6; p++) {
        if (dot(uPlanes[p].xyz, center) + uPlanes[p].w < -radius) return;
    }

    // --- Sprite screen footprint ---------------------------------------------
    // px = worldH / (tan(fov/2) * (-z_view)) * (viewH/2); -z_view ~ clip.w.
    vec4 clip = uViewProj * vec4(center, 1.0);
    float wz = max(clip.w, 1e-3);
    float px = max(worldH, halfW * 2.0) * (uViewH * 0.5)
             / max(wz * uTanHalfFov, 1e-3);
    if (px < uMinPix) return;                  // subpixel — do not draw

    // --- Survived: write compactly and count atomically ---------------------------
    uint idx = atomicAdd(cmd_instanceCount, 1u);
    if (idx >= uint(uDstCapacity)) {
        atomicMin(cmd_instanceCount, uint(uDstCapacity));
        return;
    }
    uint o = idx * 12u;
    for (uint k = 0u; k < 12u; k++)
        dst[o + k] = src[b + k];
}
