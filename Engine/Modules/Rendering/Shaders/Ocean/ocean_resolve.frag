#version 450

// MSAA -> single-sample resolve copy. Averages every sample of a multisampled
// colour target into a single-sample output, so the ocean's single-sample grab /
// overlay passes can read the scene when MSAA is enabled. Without this a sampler2D
// can't read MSAA memory, so those passes used to decline (no refraction / no
// underwater look with MSAA on). Pairs with ocean_fullscreen.vert.

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2DMS uSrc;

void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy);
    int n = textureSamples(uSrc);
    vec4 c = vec4(0.0);
    for (int i = 0; i < n; ++i)
        c += texelFetch(uSrc, p, i);
    outColor = c / float(max(n, 1));
}
