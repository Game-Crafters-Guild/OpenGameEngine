#version 450

// Alpha-aware selection mask draw.
//
// Writes only visible cutout pixels into the R8 selection mask so the Sobel
// outline follows leaf silhouettes instead of billboard card rectangles.

layout(set = 0, binding = 0) uniform sampler2D uAlbedoMap;

layout(location = 0) in vec2 vUV0;
layout(location = 0) out float oMask;

layout(push_constant) uniform PC
{
    mat4 uVPM;
    vec4 uMaskParams;   // x alpha cutoff, yz UV scale, w skin palette offset
    vec4 uMaskExtra;    // xy UV offset, z time seconds, w instance seed
    vec4 uWindStrength; // xyz local wind, w enabled
    vec4 uWindParams;   // x frequency, y spatial scale, z height, w base Y
} pc;

float LeafMaskAlpha(vec4 albedo)
{
    if (albedo.a < 0.999)
        return albedo.a;

    float maxChannel = max(max(albedo.r, albedo.g), albedo.b);
    float minChannel = min(min(albedo.r, albedo.g), albedo.b);
    float saturation = maxChannel - minChannel;
    float greenDominance = albedo.g - max(albedo.r, albedo.b) * 0.72;
    return smoothstep(0.035, 0.16, max(saturation * 0.75, greenDominance));
}

void main()
{
    vec4 albedo = texture(uAlbedoMap, vUV0);
    if (LeafMaskAlpha(albedo) < clamp(pc.uMaskParams.x, 0.0, 1.0))
        discard;

    oMask = 1.0;
}
