// ============================================================================
//  Terrain — terrain vertex shader
// ----------------------------------------------------------------------------
//  Loaded with a "#version 330 core" (desktop) or "#version 300 es"
//  (GLES) prefix — the version is injected from code (see main.c).
//  Job: take a flat mesh (x, z + uv), read the height out of the heightmap
//  texture and lift the vertex. The world is flat, all displacement is on Y.
//
//  Heightmap texture (GL_R8, smoothed in the C code + bilinear filtering):
//  the height curve with the gameplay band is ALREADY applied in it
//  (see height_curve in main.c) — the shader only stretches it linearly into
//  meters. Color is decoupled from height: the fragment shader paints ground
//  color.
// ============================================================================

layout (location = 0) in vec2 aPos;   // vertex x,z on the plane
layout (location = 1) in vec2 aUV;    // coordinates in the map, 0..1
layout (location = 2) in float aDrop; // skirt: how many meters to drop the
                                      // vertex below the terrain height
                                      // (0 = surface, >0 = bottom skirt edge
                                      // at LOD seams)

uniform mat4 uViewProj;               // view * projection (from the Camera node)
uniform sampler2D uHeightmap;         // terrain heights (0 = low, 1 = peak)
uniform float uHeightMin;             // height that 0 maps to (-100 m)
uniform float uHeightMax;             // height that 1 maps to (+400 m)

out vec2 vUV;
out vec3 vWorldPos;

float sample_height16(vec2 uv) {
    vec2 h = texture(uHeightmap, uv).rg;
#ifdef FR_HEIGHT_RG8
    return (h.r * 65280.0 + h.g * 255.0) / 65535.0;
#else
    return h.r;
#endif
}

void main() {
    float t = sample_height16(aUV);
    float h = mix(uHeightMin, uHeightMax, t) - aDrop;
    vec3 pos = vec3(aPos.x, h, aPos.y);
    vUV = aUV;
    vWorldPos = pos;
    gl_Position = uViewProj * vec4(pos, 1.0);
}