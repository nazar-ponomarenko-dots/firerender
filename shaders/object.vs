// ============================================================================
//  Object — vertex shader for object billboards (trees, etc.)
// ----------------------------------------------------------------------------
//  Each object is TWO STATIC CROSSED quads (an X): width axes
//  (0,0,1) and (1,0,0) — a tree looks volumetric from any side and does
//  NOT swivel to face the camera. Quad: x -0.5..0.5 (along uObjRight),
//  y 0..1 (up).
//
//  THE QUAD BOTTOM FOLLOWS THE TERRAIN: uObjPos.y is the ground height at
//  the centre, and the base corners are raised/lowered along the slope
//  (uObjSlope, computed in objects.c with the same sample_height as the
//  terrain). Without it a 12.5 m wide tree on a slope would hang in the air
//  at one corner and sink into the ground at another.
// ============================================================================

layout (location = 0) in vec2 aPos;    // local: x -0.5..0.5, y 0..1
layout (location = 1) in vec2 aUV;

uniform mat4 uViewProj;
uniform vec3 uObjPos;
uniform vec2 uObjSize;      // quad width and height, in meters
uniform vec3 uObjRight;     // quad width axis: (0,0,1) or (1,0,0) — the cross
uniform vec2 uObjSlope;     // ground slope under the object (dh/dx, dh/dz)

// Texture settings (render.texture): UV tiling/offset/animation
uniform vec2 uUvTile;
uniform vec2 uUvOffset;
uniform vec2 uUvScroll;
uniform float uTime;

out vec2 vUV;
out vec3 vWorldPos;

void main() {
    float hx = aPos.x * uObjSize.x;                  // offset along the width
    vec3 wp = uObjPos + uObjRight * hx
            + vec3(0.0, 1.0, 0.0) * (aPos.y * uObjSize.y);
    // Base corners follow the ground slope (as wheat.vs does for vegetation).
    wp.y += (uObjSlope.x * uObjRight.x + uObjSlope.y * uObjRight.z) * hx;
    // V is flipped: the top of a PNG is the tree crown (v=0), quad v=0 is the bottom
    vUV = vec2(aUV.x, 1.0 - aUV.y) * uUvTile + uUvOffset + uUvScroll * uTime;
    vWorldPos = wp;
    gl_Position = uViewProj * vec4(wp, 1.0);
}
