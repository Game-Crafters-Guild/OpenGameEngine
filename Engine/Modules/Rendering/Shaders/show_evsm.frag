#version 450

layout(location = 0) in vec2 vUV;           // from fullscreen_noinput.vert
layout(location = 0) out vec4 oColor;
layout(set = 0, binding = 0) uniform sampler2D uTex; // EVSM moments (RGBA)

void main()
{
    vec4 m = texture(uTex, vUV);
    // Visualize moments channels; this will likely saturate, but confirms content
    oColor = vec4(m.r, m.g, m.b, 1.0);
}

