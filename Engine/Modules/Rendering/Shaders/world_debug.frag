// Fragment shader for minimal world-space debug rendering.

#version 450

layout(location = 0) in vec3 vColor;
layout(location = 0) out vec4 outColor;

void main()
{
    outColor = vec4(vColor, 1.0);
}
