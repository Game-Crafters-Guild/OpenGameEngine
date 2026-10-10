#version 450

layout(location = 0) out vec2 vUV;

// Fullscreen triangle without vertex inputs.
// vUV carries ViewportUV: (0,0) top-left, (1,1) bottom-right.
void main()
{
    // NV/AMD friendly big-triangle pattern
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    vUV = vec2(p.x, 1.0 - p.y);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
