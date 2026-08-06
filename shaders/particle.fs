// particle.fs — спрайт частицы. Режим (альфа/аддитив) задаёт draw call в
// particles.c через blend function; здесь собираем premultiplied-цвет.
in vec2 vUV;
in vec3 vColor;
in float vAlpha;

uniform sampler2D uTex;

out vec4 FragColor;

void main()
{
    vec4 t = texture(uTex, vUV);
    if (t.a < 0.01) discard;
    float a = t.a * vAlpha;
    FragColor = vec4(vColor * a, a);
}