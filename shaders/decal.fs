// decal.fs — decal on the terrain: texture * color * alpha, alpha-blended.
in vec2 vUV;
out vec4 FragColor;

uniform sampler2D uTex;
uniform vec3 uColor;
uniform float uAlpha;

void main()
{
    vec4 t = texture(uTex, vUV);
    if (t.a < 0.05) discard;
    FragColor = vec4(uColor * t.rgb, t.a * uAlpha);
}