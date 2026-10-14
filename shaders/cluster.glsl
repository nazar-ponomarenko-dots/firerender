// ============================================================================
//  cluster.glsl — clustered lighting in the fragment shader (Forward+)
// ----------------------------------------------------------------------------
//  Included into scene shaders (#include "cluster.glsl") and providing one function:
//      vec3 cluster_light(vec3 P, vec3 N, vec3 V, float rough)
//  It finds the pixel's voxel and sums up ONLY the light sources that actually
//  reach it. Light source types (see lights.h):
//    0 POINT — point light with a spherical radius (soft highlight by lamp size)
//    1 SPOT  — cone with a smooth inner/outer edge
//    2 RECT  — area rectangle: diffuse by the rectangle's "form factor",
//              specular — by the closest point on the rectangle (representative point)
//    3 TUBE  — linear: closest point on the segment (fluorescent lamp/neon)
//
//  Falloff — physical "inverse square with a window":
//      att = (1/(d²+r²)) * window(d/range)⁴
//  Such a window gives a soft fade to zero at the radius boundary without a
//  hard edge. Specular — GGX approximation (normalized Blinn-Phong by roughness),
//  so that highlights from area sources look like in UE, not like points.
//
//  A shader that includes this file MUST declare before the include:
//      uniform float uClusterOn;  uniform vec3 uClusterGrid;
//      uniform float uClusterZNear, uClusterZScale;  uniform vec2 uClusterScreen;
//      uniform float uLightDebug;
//  Buffers are declared here (binding 3/4/5 — set by lights_bind).
// ============================================================================

// Light source: 5 x vec4 (see lights.c)
struct ClusterLight {
    vec4 pos_type;     // xyz position, w type
    vec4 color;        // rgb color * intensity, w intensity
    vec4 dir_range;    // xyz direction, w influence radius
    vec4 shape;        // x source radius, y cos_outer, z cos_inner, w half_h
    vec4 up_size;      // xyz second axis, w half_w / half_len
};

layout(std430, binding = 3) readonly buffer ClusterLights {
    ClusterLight uLights[];
};
layout(std430, binding = 4) readonly buffer ClusterGrid {
    uint uGrid[];      // per voxel: offset, count
};
layout(std430, binding = 5) readonly buffer ClusterIndex {
    uint uLightIndex[];
};

// Voxel index from the screen position and view depth (meters).
int cluster_index(vec2 frag, float view_z) {
    ivec3 g = ivec3(uClusterGrid);
    vec2 uv = frag / max(uClusterScreen, vec2(1.0));
    int x = clamp(int(uv.x * float(g.x)), 0, g.x - 1);
    int y = clamp(int(uv.y * float(g.y)), 0, g.y - 1);
    float z = max(view_z, uClusterZNear);
    int s = clamp(int(log(z / uClusterZNear) * uClusterZScale), 0, g.z - 1);
    return (s * g.y + y) * g.x + x;
}

// Falloff: inverse square + soft window at the radius boundary.
float cluster_falloff(float dist2, float range, float src_r) {
    float att = 1.0 / (dist2 + src_r * src_r);
    float t = clamp(dist2 / (range * range), 0.0, 1.0);
    float win = 1.0 - t * t;
    return att * win * win;
}

// Normalized GGX-ish specular (Blinn-Phong with energy conservation):
// "roughness" rough 0..1 -> exponent.
float cluster_spec(vec3 N, vec3 H, float rough) {
    float a = max(rough * rough, 0.002);
    float power = 2.0 / (a * a) - 2.0;
    float nh = max(dot(N, H), 0.0);
    // (power+8)/8 — Blinn-Phong energy-conservation multiplier
    return pow(nh, power) * (power + 8.0) / 8.0;
}

// Closest point on a segment (for tube lights)
vec3 closest_on_segment(vec3 P, vec3 c, vec3 axis, float half_len) {
    float t = clamp(dot(P - c, axis), -half_len, half_len);
    return c + axis * t;
}

// Closest point on a rectangle (for area lights)
vec3 closest_on_rect(vec3 P, vec3 c, vec3 n, vec3 up, float hw, float hh) {
    vec3 right = normalize(cross(up, n));
    vec3 upn = normalize(cross(n, right));
    vec3 d = P - c;
    float x = clamp(dot(d, right), -hw, hw);
    float y = clamp(dot(d, upn), -hh, hh);
    return c + right * x + upn * y;
}

// Total contribution of the voxel's local light sources.
//   P — world point, N — normal, V — toward the camera, rough — roughness 0..1
//   spec_out (out) — specular part separately (it can be scaled)
vec3 cluster_light_ex(vec3 P, vec3 N, vec3 V, float rough, out vec3 spec_out) {
    spec_out = vec3(0.0);
    if (uClusterOn < 0.5) return vec3(0.0);
    // view_z: distance along the view. gl_FragCoord.w = 1/clip.w, and for a
    // perspective projection clip.w equals -z_view (in meters).
    float view_z = 1.0 / max(gl_FragCoord.w, 1e-6);
    int c = cluster_index(gl_FragCoord.xy, view_z);
    uint off = uGrid[c * 2u];
    uint cnt = uGrid[c * 2u + 1u];
    vec3 diff = vec3(0.0);
    for (uint k = 0u; k < cnt; k++) {
        ClusterLight L = uLights[uLightIndex[off + k]];
        int type = int(L.pos_type.w);
        vec3 lp = L.pos_type.xyz;
        float range = L.dir_range.w;
        float src_r = L.shape.x;
        vec3 col = L.color.rgb;

        // "Representative" point of the light source: for area/linear ones it
        // is the closest to P — this stretches the highlight along the source shape.
        vec3 rep = lp;
        if (type == 3) {
            rep = closest_on_segment(P, lp, L.up_size.xyz, L.up_size.w);
        } else if (type == 2) {
            rep = closest_on_rect(P, lp, L.dir_range.xyz, L.up_size.xyz,
                                  L.up_size.w, L.shape.w);
        }
        vec3 Lv = rep - P;
        float d2 = dot(Lv, Lv);
        if (d2 > range * range) continue;
        float d = sqrt(max(d2, 1e-8));
        vec3 Ld = Lv / d;

        float att = cluster_falloff(d2, range, src_r);
        if (att <= 0.0) continue;

        // Spotlight cone
        if (type == 1) {
            float cd = dot(-Ld, L.dir_range.xyz);
            float co = L.shape.y, ci = L.shape.z;
            float cone = clamp((cd - co) / max(ci - co, 1e-4), 0.0, 1.0);
            cone *= cone;                       // soft edge
            if (cone <= 0.0) continue;
            att *= cone;
        }
        // Area light: light comes only from the front side of the panel
        if (type == 2) {
            float face = dot(L.dir_range.xyz, -Ld);
            if (face <= 0.0) continue;
            att *= face;
        }

        // Diffuse. For area/linear sources N·L is "blurred" by the source size:
        // a wide panel wraps light around the terminator (wrap lighting).
        float wrap = (type == 2 || type == 3)
                   ? clamp(src_r / max(d, 0.05), 0.0, 0.5) : 0.0;
        float ndl = (dot(N, Ld) + wrap) / (1.0 + wrap);
        ndl = max(ndl, 0.0);
        diff += col * (att * ndl);

        // Specular from the representative point
        vec3 H = normalize(Ld + V);
        float s = cluster_spec(N, H, rough);
        spec_out += col * (att * s * max(dot(N, Ld), 0.0));
    }
    // Debug: heat map of the light count per voxel
    if (uLightDebug > 0.5) {
        float t = float(cnt) / 8.0;
        diff = mix(vec3(0.0, 0.2, 0.0), vec3(1.0, 0.0, 0.0), clamp(t, 0.0, 1.0));
        spec_out = vec3(0.0);
    }
    return diff;
}

vec3 cluster_light(vec3 P, vec3 N, vec3 V, float rough) {
    vec3 spec;
    vec3 d = cluster_light_ex(P, N, V, rough, spec);
    return d + spec;
}
