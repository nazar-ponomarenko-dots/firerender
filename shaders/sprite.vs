// ============================================================================
//  Sprite3D — vertex shader for a sprite attached to any node/bone
// ----------------------------------------------------------------------------
//  One quad (x,y in -0.5..0.5). Two modes:
//    uBillboard = 1 — the quad turns to face the camera (uCamRight/uCamUp
//                     basis), its centre comes from the node's WORLD matrix
//                     (uModel);
//    uBillboard = 0 — flat quad in the node's frame: fully obeys the matrix
//                     (incl. the rotation of the bone it is attached to).
//  Size is given in meters (uSize) and multiplied by the node's scale.
//  UV: an atlas frame (uUvTile/uUvOffset) + scrolling (uUvScroll * uTime).
// ============================================================================

layout (location = 0) in vec2 aPos;   // quad: -0.5..0.5
layout (location = 1) in vec2 aUV;    // 0..1

uniform mat4 uViewProj;
uniform mat4 uModel;          // node world matrix (scene.c)
uniform vec2 uSize;           // width/height, m
uniform float uBillboard;     // 1 = face the camera
uniform vec3 uCamRight;
uniform vec3 uCamUp;
uniform vec2 uUvTile;
uniform vec2 uUvOffset;
uniform vec2 uUvScroll;
uniform float uTime;

out vec2 vUV;
out vec3 vWorldPos;
out vec3 vNormal;

void main() {
    // Node scale: lengths of the model matrix columns (the node may be
    // attached to scaled bones — the sprite must ride along with them).
    float sx = length(uModel[0].xyz);
    float sy = length(uModel[1].xyz);
    vec3 center = uModel[3].xyz;
    vec3 wp;
    vec3 nrm;
    if (uBillboard > 0.5) {
        wp = center + uCamRight * (aPos.x * uSize.x * sx)
                    + uCamUp    * (aPos.y * uSize.y * sy);
        nrm = normalize(cross(uCamRight, uCamUp));
    } else {
        // Flat quad in the node's frame: X axis is the node's right, Y its up.
        vec3 right = uModel[0].xyz / max(sx, 1e-5);
        vec3 up    = uModel[1].xyz / max(sy, 1e-5);
        wp = center + right * (aPos.x * uSize.x * sx)
                    + up    * (aPos.y * uSize.y * sy);
        nrm = normalize(cross(right, up));
    }
    vUV = aUV * uUvTile + uUvOffset + uUvScroll * uTime;
    vWorldPos = wp;
    vNormal = nrm;
    gl_Position = uViewProj * vec4(wp, 1.0);
}
