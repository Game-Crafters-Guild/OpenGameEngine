#version 450

layout(location = 0) in vec3 vColor;
layout(location = 0) out vec4 outColor;

// Depth-only prepass variant:
// - Keep a valid color output for backends that expect Location0 under dynamic rendering,
//   but the pipeline will have colorWriteMask=0 so this is effectively a no-op.
void main()
{
    outColor = vec4(vColor, 1.0);
}

