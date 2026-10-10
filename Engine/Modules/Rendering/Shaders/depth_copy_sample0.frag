#version 450

// Single-sample copy of a multisampled depth buffer at the same extent: sample 0 of each pixel,
// written through gl_FragDepth. Runs as a fullscreen triangle (fullscreen_noinput.vert) with
// depth-write on, compare Always (DepthUpsamplePass). Sample 0 is the sample every single-sample
// reader of the view depth takes (depth_resolve.comp).

layout(location = 0) in vec2 vUV;

layout(set = 0, binding = 0) uniform sampler2DMS uDepth;

void main()
{
    ivec2 extent = textureSize(uDepth);
    ivec2 p = clamp(ivec2(gl_FragCoord.xy), ivec2(0), extent - 1);
    gl_FragDepth = texelFetch(uDepth, p, 0).r;
}
