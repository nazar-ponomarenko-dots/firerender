// ============================================================================
//  Blob — flat spot-quad on the ground (shadow mode 3)
// ----------------------------------------------------------------------------
//  The quad lies in the XZ plane: aPos.x runs along uObjRight, aPos.y along
//  uObjFwd. The base (center) is uObjPos.
// ============================================================================

layout (location = 0) in vec2 aPos;    // x -0.5..0.5, y -0.5..0.5
layout (location = 1) in vec2 aUV;

uniform mat4 uViewProj;
uniform vec3 uObjPos;
uniform vec2 uObjSize;
uniform vec3 uObjRight;
uniform vec3 uObjFwd;

out vec2 vUV;

void main() {
    vec3 wp = uObjPos
            + uObjRight * (aPos.x * uObjSize.x)
            + uObjFwd  * (aPos.y * uObjSize.y);
    vUV = vec2(aUV.x, 1.0 - aUV.y);
    gl_Position = uViewProj * vec4(wp, 1.0);
}
