#version 450

// Fullscreen triangle for the ocean's post/composite passes (the underwater
// overlay). No vertex inputs — the three corners are generated from
// gl_VertexIndex. vUV carries ViewportUV: (0,0) top-left, (1,1) bottom-right,
// matching the engine's stock fullscreen_noinput.vert so the depth/scene-colour
// lookups line up with every other screen-space pass.
//
// Lives in the Ocean shader dir (not the staged Shaders/ root) because the
// Ocean module compiles its programs at runtime via ShaderCompileService,
// reading sources from here rather than the build-time .shaderpkg set.

layout(location = 0) out vec2 vUV;

void main()
{
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    vUV = vec2(p.x, 1.0 - p.y);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
