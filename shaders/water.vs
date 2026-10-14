// ============================================================================
//  Water — water vertex shader: puddles/lakes/rivers/seas (see water.c)
// ----------------------------------------------------------------------------
//  The mesh is flat at uLevel; waves lift the vertices in the vertex
//  shader using the same waveY function the fragment shader derives normals
//  from via finite differences (smooth at any mesh density).
//
//  Attributes (water.c upload_geometry):
//    aPos  (0) vec2 — vertex x,z on the level plane
//    aUV   (1) vec2 — coords: world/4 (wave tile); for rivers x runs along the channel
//    aEdge (2) float — 0 at the shore, grows inward; open sea — 1e6 (no edge)
// ============================================================================

layout (location = 0) in vec2 aPos;
layout (location = 1) in vec2 aUV;
layout (location = 2) in float aEdge;

uniform mat4  uViewProj;      // view * projection (Camera node)
uniform float uTime;          // time (advances in water_node_update)
uniform float uLevel;         // water surface level, m
uniform float uWaveAmp;       // wave amplitude, m
uniform float uWaveSpeed;     // wave speed (time multiplier)
uniform float uWaveLen;      // wavelength, m
uniform vec2  uFlowDir;       // flow direction (river), unit vector

out vec3 vWorldPos;
out vec2 vUV;
out float vEdge;

// The wave function is SHARED by VS and FS (FS normals use that same one).
// The last term is fine ripple (short wave, small amplitude).
float waveY(vec2 p, float t) {
    float k = 6.2831853 / uWaveLen;
    vec2 f = uFlowDir;
    if (dot(f, f) < 0.0001) f = vec2(1.0, 0.0);   // sea/lake have no flow set
    f = normalize(f);
    vec2 q = vec2(-f.y, f.x);                     // across the flow
    return uWaveAmp * sin(dot(f, p) * k + t * uWaveSpeed)
         + uWaveAmp * 0.45 * sin(dot(q, p) * k * 0.62 + t * uWaveSpeed * 1.17)
         + uWaveAmp * 0.2  * sin((p.x + p.y) * k * 0.34 + t * uWaveSpeed * 0.8)
         + uWaveAmp * 0.12 * sin(dot(f, p) * k * 4.7 + dot(q, p) * k * 3.1
                                 + t * uWaveSpeed * 1.6);
}

void main() {
    vUV = aUV;
    vEdge = aEdge;
    float y = uLevel + waveY(aPos, uTime);
    vec3 world = vec3(aPos.x, y, aPos.y);
    vWorldPos = world;
    gl_Position = uViewProj * vec4(world, 1.0);
}