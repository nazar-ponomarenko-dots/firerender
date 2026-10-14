// decal.vs — quad on the terrain: vertices are already transformed in world
// space (CPU: corner heights from the map). uViewProj — view*projection.
layout(location = 0) in vec3 aPos;   // x, y, z (terrain height already included)
layout(location = 1) in vec2 aUV;

uniform mat4 uViewProj;

out vec2 vUV;

void main()
{
    vUV = aUV;
    gl_Position = uViewProj * vec4(aPos, 1.0);
}