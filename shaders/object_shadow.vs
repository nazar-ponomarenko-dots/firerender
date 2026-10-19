// ============================================================================
//  ObjectShadow — vertex shader of objects into the shadow map
// ----------------------------------------------------------------------------
//  The same cross of two static quads as in the regular render — position and
//  UV only (the fragment shader does the alpha-test). The base follows the
//  ground slope just the same (uObjSlope), otherwise the shadow would detach
//  from the tree on a hill.
// ============================================================================

layout (location = 0) in vec2 aPos;    // local: x -0.5..0.5, y 0..1
layout (location = 1) in vec2 aUV;

uniform mat4 uViewProj;
uniform vec3 uObjPos;
uniform vec2 uObjSize;
uniform vec3 uObjRight;
uniform vec2 uObjSlope;     // ground slope under the object (dh/dx, dh/dz)

out vec2 vUV;

void main() {
    float hx = aPos.x * uObjSize.x;
    vec3 wp = uObjPos + uObjRight * hx
            + vec3(0.0, 1.0, 0.0) * (aPos.y * uObjSize.y);
    wp.y += (uObjSlope.x * uObjRight.x + uObjSlope.y * uObjRight.z) * hx;
    vUV = vec2(aUV.x, 1.0 - aUV.y);
    gl_Position = uViewProj * vec4(wp, 1.0);
}
