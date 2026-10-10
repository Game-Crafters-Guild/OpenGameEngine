#version 450

// Infinite grid vertex shader: generates a fullscreen triangle in clip space.
// Passes NDC to the fragment shader which does the full unproject per-fragment
// for maximum precision (interpolating world positions causes grid line drift).

layout(location = 0) out vec2 vNDC;

void main()
{
    vec2 ndc = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    ndc = ndc * 2.0 - 1.0;

    gl_Position = vec4(ndc, 0.0, 1.0);
    vNDC = ndc;
}
