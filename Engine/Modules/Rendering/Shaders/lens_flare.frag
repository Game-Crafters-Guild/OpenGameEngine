#version 450

// Samples the flare atlas and modulates by the per-instance color. The color
// channel remains additive (One, One) with depth test/write off, so flare light
// is summed onto the scene — order-independent, no sorting needed. Alpha uses
// the sprite's coverage so RGB-only black-backed atlases can still be composed
// transparently by the editor UI.

layout(set = 0, binding = 0) uniform sampler2D uAtlas;
layout(set = 0, binding = 2) uniform sampler2D uSceneDepth;

layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vColor;
layout(location = 2) in vec3 vProbe; // xy = source UV, z = source NDC depth (<0 = skip)

layout(location = 0) out vec4 outColor;

layout(push_constant) uniform LensFlarePC
{
    int atlasHasAuthoredAlpha;
} pc;

// Pixel-accurate source occlusion. Fetch the exact scene-depth texel containing
// the projected light source and use a hard reverse-Z comparison (near = 1,
// far = 0). Avoid filtered sampling, a multi-pixel probe radius, and a fixed raw
// depth bias: all three can leak visibility across silhouettes, while a fixed
// bias also makes distant reverse-Z occluders look like background.
float ProbeVisibility()
{
    if (vProbe.z < 0.0)
        return 1.0; // occlusion disabled for this flare

    ivec2 depthSize = textureSize(uSceneDepth, 0);
    ivec2 sourcePixel = ivec2(vProbe.xy * vec2(depthSize));
    sourcePixel = clamp(sourcePixel, ivec2(0), depthSize - ivec2(1));

    float sceneDepth = texelFetch(uSceneDepth, sourcePixel, 0).r;
    return sceneDepth > vProbe.z ? 0.0 : 1.0;
}

void main()
{
    vec4 texel = texture(uAtlas, vUV);
    float visibility = ProbeVisibility();

    // RGB atlases are uploaded with alpha=1, so use luminous RGB as coverage.
    // RGBA atlases keep their authored alpha, including fully opaque dark texels.
    float coverage = pc.atlasHasAuthoredAlpha != 0
        ? texel.a
        : max(texel.rgb.r, max(texel.rgb.g, texel.rgb.b));

    vec3 flareRgb = texel.rgb * texel.a * vColor.rgb * visibility;
    float modulation = clamp(max(vColor.r, max(vColor.g, vColor.b)), 0.0, 1.0);
    float flareAlpha = clamp(coverage * modulation * visibility, 0.0, 1.0);
    outColor = vec4(flareRgb, flareAlpha);
}
