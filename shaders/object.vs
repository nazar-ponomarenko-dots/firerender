// ============================================================================
//  Object — вершинный шейдер биллборда объекта (дерево и пр.)
// ----------------------------------------------------------------------------
//  Каждый объект — ДВА СКРЕЩЁННЫХ СТАТИЧНЫХ квада (крест): оси ширин
//  (0,0,1) и (1,0,0) — дерево выглядит объёмным с любой стороны и НЕ
//  крутится за камерой. Квад: x -0.5..0.5 (вдоль uObjRight), y 0..1 (вверх).
// ============================================================================

layout (location = 0) in vec2 aPos;    // локально: x -0.5..0.5, y 0..1
layout (location = 1) in vec2 aUV;

uniform mat4 uViewProj;
uniform vec3 uObjPos;
uniform vec2 uObjSize;      // ширина, высота квада в метрах
uniform vec3 uObjRight;     // ось ширины квада: (0,0,1) или (1,0,0) — крест

// Настройка текстуры (render.texture): повтор/сдвиг/анимация UV
uniform vec2 uUvTile;
uniform vec2 uUvOffset;
uniform vec2 uUvScroll;
uniform float uTime;

out vec2 vUV;
out vec3 vWorldPos;

void main() {
    vec3 wp = uObjPos
            + uObjRight * (aPos.x * uObjSize.x)
            + vec3(0.0, 1.0, 0.0) * (aPos.y * uObjSize.y);
    // V перевёрнут: у PNG верх картинки — крона (v=0), у квада v=0 — низ
    vUV = vec2(aUV.x, 1.0 - aUV.y) * uUvTile + uUvOffset + uUvScroll * uTime;
    vWorldPos = wp;
    gl_Position = uViewProj * vec4(wp, 1.0);
}
