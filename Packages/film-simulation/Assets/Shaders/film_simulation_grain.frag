#version 450

// The FidelityFX Fast path and PCG3D16 generator are adapted from AMD
// FidelityFX Lens 1.1. Copyright (C) 2024 Advanced Micro Devices, Inc.
// Licensed under MIT; see ../../LICENSE.txt.

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;
layout(set = 0, binding = 0) uniform sampler2D uSceneColor;

layout(push_constant) uniform FilmGrainPC
{
    int filmSimulationGrainMode;
    float filmSimulationGrainIntensity;
    float filmSimulationGrainSize;
    int filmSimulationGrainSmooth;
    float filmSimulationGrainDensity;
    float filmSimulationFrameRate;
    float filmSimulationGrainShadowResponse;
    float filmSimulationGrainMidtoneResponse;
    float filmSimulationGrainHighlightResponse;
    int filmSimulationGrainColored;
    float shaderAnimationTime;
} pc;

float Luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

uvec3 Pcg3d16(uvec3 v)
{
    v = v * 12829u + 47989u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    return v >> 16u;
}

float Hash(vec2 p, float seed)
{
    ivec2 cell = ivec2(floor(p));
    uvec3 random = Pcg3d16(uvec3(uvec2(cell), uint(max(seed, 0.0))));
    return float(random.x) * (1.0 / 65536.0);
}

vec2 SimplexCell(vec2 p)
{
    const float F2 = 0.36602540378;
    const float G2 = 0.21132486540;
    float skew = (p.x + p.y) * F2;
    vec2 simplex = round(p + skew);
    float unskew = (simplex.x + simplex.y) * G2;
    return p - (simplex - unskew);
}

float SampleFidelityFxGrain(vec2 pixel, float seed, float sizePixels)
{
    // FidelityFX hashes on scale / 8. Map the authored pixel size back into
    // that domain so a value of N produces an approximately N-pixel PCG cell
    // instead of regenerating the fine random offset at every pixel.
    float scale = max(sizePixels * 8.0, 2.0);
    uvec3 randomBits = Pcg3d16(uvec3(uvec2(pixel / (scale / 8.0)), uint(max(seed, 0.0))));
    vec2 fineRandom = vec2(randomBits.xy) * (1.0 / 65536.0) - 0.5;
    vec2 simplexPosition = SimplexCell(pixel / scale + fineRandom);
    return 1.0 - 2.0 * exp2(-length(simplexPosition) * 3.0);
}

vec3 GrainValue(vec2 cell, float seed, bool colored)
{
    float mono = Hash(cell + vec2(13.0, 41.0), seed + 17.0) * 2.0 - 1.0;
    if (!colored)
        return vec3(mono);
    return vec3(mono,
                Hash(cell + vec2(59.0, 7.0), seed + 31.0) * 2.0 - 1.0,
                Hash(cell + vec2(23.0, 73.0), seed + 47.0) * 2.0 - 1.0);
}

vec3 SampleGrain(vec2 pixel, float seed, float size, float density, bool smoothGrain, bool colored)
{
    // Grain centers remain on the original fixed two-pixel lattice. Size
    // changes only each grain's footprint, not the complete noise pattern.
    const float cellSpacing = 2.0;
    vec2 base = floor(pixel / cellSpacing);
    float radius = max(size * 0.5, 0.125);
    vec3 sum = vec3(0.0);
    float sumSq = 0.0;
    for (int y = -2; y <= 2; ++y)
    for (int x = -2; x <= 2; ++x)
    {
        vec2 cell = base + vec2(x, y);
        if (Hash(cell + vec2(71.0, 29.0), seed + 83.0) < 1.0 - density)
            continue;
        vec2 jitter = vec2(Hash(cell + vec2(5.0, 97.0), seed + 101.0),
                           Hash(cell + vec2(89.0, 3.0), seed + 127.0)) - 0.5;
        vec2 delta = pixel - ((cell + 0.5) * cellSpacing + jitter);
        float w = smoothGrain ? 1.0 - smoothstep(0.15, 1.0, length(delta) / radius)
                              : 1.0 - step(radius, max(abs(delta.x), abs(delta.y)));
        sum += GrainValue(cell, seed, colored) * w;
        sumSq += w * w;
    }
    return sum / max(sqrt(sumSq), 1.0);
}

void main()
{
    vec4 src = texture(uSceneColor, vUV);
    float intensity = clamp(pc.filmSimulationGrainIntensity, 0.0, 4.0);
    if (intensity <= 1e-5)
    {
        oColor = src;
        return;
    }
    float seed = floor(pc.shaderAnimationTime * max(pc.filmSimulationFrameRate, 1.0));
    float grainSize = clamp(pc.filmSimulationGrainSize, 0.25, 8.0);
    vec3 noise;
    if (pc.filmSimulationGrainMode == 0)
    {
        // FidelityFX grain is slightly dark-biased. Keep its intensity match
        // independent of grain scale so Size controls frequency only.
        const float fidelityFxMeanCorrection = 0.10;
        const float fidelityFxIntensityMatch = 1.60;
        float fastNoise = SampleFidelityFxGrain(gl_FragCoord.xy, seed, grainSize);
        noise = vec3((fastNoise + fidelityFxMeanCorrection) * fidelityFxIntensityMatch);
    }
    else
    {
        noise = SampleGrain(gl_FragCoord.xy, seed, grainSize,
            clamp(pc.filmSimulationGrainDensity, 0.0, 1.0),
            pc.filmSimulationGrainSmooth != 0, pc.filmSimulationGrainColored != 0);
    }
    float luma = clamp(Luma(src.rgb), 0.0, 1.0);
    float shadows = 1.0 - smoothstep(0.15, 0.45, luma);
    float highlights = smoothstep(0.55, 0.9, luma);
    float midtones = max(1.0 - shadows - highlights, 0.0);
    float response = shadows * pc.filmSimulationGrainShadowResponse +
                     midtones * pc.filmSimulationGrainMidtoneResponse +
                     highlights * pc.filmSimulationGrainHighlightResponse;
    // The distance to the nearest display boundary keeps grain from clipping
    // either black or white. Our tonal response remains an artistic multiplier.
    vec3 envelope = max(min(src.rgb, vec3(1.0) - src.rgb), vec3(0.0));
    float strength = clamp(intensity * max(response, 0.0) * 0.22, 0.0, 1.0);
    oColor = vec4(src.rgb + clamp(noise, vec3(-1.0), vec3(1.0)) * envelope * strength, src.a);
}
