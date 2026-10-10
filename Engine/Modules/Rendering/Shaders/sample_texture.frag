#version 450

layout(set = 0, binding = 0) uniform sampler2D uTex;
layout(location = 0) out vec4 outColor;

void main()
{
    // Sample center of the texture to produce a constant color across the screen
    outColor = texture(uTex, vec2(0.5, 0.5));
}

