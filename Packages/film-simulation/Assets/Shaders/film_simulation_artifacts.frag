#version 450

#include "Includes/compat_profile.glsl"

// The compatibility cook uses explicit LOD for taps in non-uniform control
// flow, where WGSL cannot evaluate implicit derivatives. GE_TAP_LOD0 retains
// the native sampling form for other cooks.

// MIT-licensed GameEngine film-simulation package shader.

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;

layout(set = 0, binding = 0) uniform sampler2D uSceneColor;

layout(push_constant) uniform FilmSimulationArtifactsPC {
    int filmSimulationHairEnabled;
    float filmSimulationHairAmount;
    float filmSimulationHairIntensity;
    float filmSimulationHairWidth;
    float filmSimulationHairLength;
    float filmSimulationHairRandomSize;
    float filmSimulationHairCurl;
    float filmSimulationHairCurlRandomness;
    float filmSimulationFrameRate;
    int filmSimulationScratchesEnabled;
    float filmSimulationScratchAmount;
    float filmSimulationScratchIntensity;
    float filmSimulationScratchWidth;
    float filmSimulationScratchLength;
    int filmSimulationDustEnabled;
    float filmSimulationDustAmount;
    float filmSimulationDustIntensity;
    float filmSimulationDustSize;
    float filmSimulationDustRandomSize;
    int filmSimulationGateWeaveEnabled;
    float filmSimulationGateWeaveHorizontal;
    float filmSimulationGateWeaveVertical;
    float filmSimulationGateWeaveRotation;
    int filmSimulationGateMask;
    float filmSimulationGateMaskFeather;
    float filmSimulationGateMaskRoundness;
    float shaderAnimationTime;
} pc;

float Hash11(float p)
{
    p = fract(p * 0.1031);
    p *= p + 33.33;
    p *= p + p;
    return fract(p);
}

float Hash21(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

vec2 ApplyGateWeave(vec2 uv, vec2 resolution)
{
    if (pc.filmSimulationGateWeaveEnabled == 0)
        return uv;

    float frame = floor(pc.shaderAnimationTime * max(pc.filmSimulationFrameRate, 1.0));
    vec2 offsetPx = vec2(
        (Hash11(frame + 11.0) * 2.0 - 1.0) * pc.filmSimulationGateWeaveHorizontal,
        (Hash11(frame + 37.0) * 2.0 - 1.0) * pc.filmSimulationGateWeaveVertical);
    float angle = radians((Hash11(frame + 73.0) * 2.0 - 1.0) *
                          pc.filmSimulationGateWeaveRotation);

    float aspect = resolution.x / max(resolution.y, 1.0);
    vec2 p = uv - vec2(0.5);
    p.x *= aspect;
    float c = cos(angle);
    float s = sin(angle);
    p = mat2(c, -s, s, c) * p;
    p.x /= aspect;
    return p + vec2(0.5) + offsetPx / resolution;
}

float HairCoverage(vec2 pixel, vec2 resolution)
{
    if (pc.filmSimulationHairEnabled == 0 || pc.filmSimulationHairAmount <= 1e-5)
        return 0.0;

    float seed = floor(pc.shaderAnimationTime * max(pc.filmSimulationFrameRate, 1.0));
    float coverage = 0.0;
    for (int i = 0; i < 8; ++i)
    {
        float id = float(i) + seed * 19.0;
        if (Hash11(id + 3.0) > clamp(pc.filmSimulationHairAmount, 0.0, 1.0))
            continue;

        vec2 center = vec2(Hash11(id + 7.0), Hash11(id + 13.0)) * resolution;
        float angle = Hash11(id + 23.0) * 6.28318530718;
        vec2 p = pixel - center;
        vec2 axis = vec2(cos(angle), sin(angle));
        vec2 local = vec2(dot(p, axis), dot(p, vec2(-axis.y, axis.x)));
        float randomSize = clamp(pc.filmSimulationHairRandomSize, 0.0, 1.0);
        float sizeScale = mix(1.0, mix(0.45, 1.0, Hash11(id + 31.0)), randomSize);
        float halfLength = max(pc.filmSimulationHairLength, 8.0) * sizeScale * 0.5;
        float normalizedX = local.x / max(halfLength, 1.0);
        float curlRandomness = clamp(pc.filmSimulationHairCurlRandomness, 0.0, 1.0);
        float curlStrength = mix(1.0, Hash11(id + 47.0), curlRandomness);
        float curlDirection = sign(Hash11(id + 41.0) - 0.5);
        float curl = pc.filmSimulationHairCurl * curlStrength * halfLength * 0.35 *
                     normalizedX * normalizedX * curlDirection;
        float distanceToFiber = abs(local.y - curl);
        float width = max(pc.filmSimulationHairWidth * sizeScale, 0.05);
        float fiber = 1.0 - smoothstep(width, width + 1.25, distanceToFiber);
        float ends = 1.0 - smoothstep(0.82, 1.0, abs(normalizedX));
        coverage = max(coverage, fiber * ends);
    }
    return coverage;
}

float ScratchCoverage(vec2 pixel, vec2 resolution, out float polarity)
{
    polarity = 1.0;
    if (pc.filmSimulationScratchesEnabled == 0 || pc.filmSimulationScratchAmount <= 1e-5)
        return 0.0;

    float seed = floor(pc.shaderAnimationTime * max(pc.filmSimulationFrameRate, 1.0));
    float coverage = 0.0;
    for (int i = 0; i < 8; ++i)
    {
        float id = float(i) + seed * 29.0;
        if (Hash11(id + 5.0) > clamp(pc.filmSimulationScratchAmount, 0.0, 1.0))
            continue;

        float x = Hash11(id + 11.0) * resolution.x;
        float centerY = Hash11(id + 17.0) * resolution.y;
        float halfLength = resolution.y * clamp(pc.filmSimulationScratchLength, 0.05, 1.0) *
                           mix(0.35, 0.5, Hash11(id + 19.0));
        float localY = pixel.y - centerY;
        float wanderingX = x + sin(localY * 0.025 + Hash11(id + 31.0) * 6.28318530718) *
                           mix(0.15, 1.5, Hash11(id + 37.0));
        float width = max(pc.filmSimulationScratchWidth, 0.05);
        float line = 1.0 - smoothstep(width, width + 1.0, abs(pixel.x - wanderingX));
        float ends = 1.0 - smoothstep(halfLength * 0.82, halfLength, abs(localY));
        float candidate = line * ends;
        if (candidate > coverage)
        {
            coverage = candidate;
            polarity = Hash11(id + 43.0) < 0.78 ? 1.0 : -1.0;
        }
    }
    return coverage;
}

float DustCoverage(vec2 pixel, vec2 resolution, out float polarity)
{
    polarity = 1.0;
    if (pc.filmSimulationDustEnabled == 0 || pc.filmSimulationDustAmount <= 1e-5)
        return 0.0;

    float seed = floor(pc.shaderAnimationTime * max(pc.filmSimulationFrameRate, 1.0));
    float coverage = 0.0;
    for (int i = 0; i < 8; ++i)
    {
        float id = float(i) + seed * 53.0;
        if (Hash11(id + 9.0) > clamp(pc.filmSimulationDustAmount, 0.0, 1.0))
            continue;

        vec2 center = vec2(Hash11(id + 15.0), Hash11(id + 21.0)) * resolution;
        vec2 p = pixel - center;
        float randomSize = clamp(pc.filmSimulationDustRandomSize, 0.0, 1.0);
        float sizeScale = mix(1.0, mix(0.35, 1.0, Hash11(id + 27.0)), randomSize);
        float radius = max(pc.filmSimulationDustSize * sizeScale, 0.5);

        // Irregular speck outline: modulate the radius by a per-speck hash of
        // the quantized polar angle so specks read as blobs, not perfect discs.
        float angle = atan(p.y, p.x);
        float lobe = Hash21(vec2(floor(angle * 1.90985), id));
        float wobbled = radius * mix(0.65, 1.0, lobe);
        float speck = 1.0 - smoothstep(wobbled, wobbled + 1.25, length(p));
        if (speck > coverage)
        {
            coverage = speck;
            // Dust on the negative prints white ("sparkle"); dust on the print
            // blocks light and reads dark. Mostly dark, occasionally bright.
            polarity = Hash11(id + 33.0) < 0.7 ? 1.0 : -1.0;
        }
    }
    return coverage;
}

float RoundedBoxSdf(vec2 p, vec2 halfSize, float radius)
{
    vec2 q = abs(p) - halfSize + vec2(radius);
    return min(max(q.x, q.y), 0.0) + length(max(q, vec2(0.0))) - radius;
}

float GateMaskCoverage(vec2 uv, vec2 resolution)
{
    int mode = pc.filmSimulationGateMask;
    if (mode == 0)
        return 1.0;

    float feather = max(pc.filmSimulationGateMaskFeather, 0.25);
    float roundness = clamp(pc.filmSimulationGateMaskRoundness, 0.0, 1.0);
    vec2 pPx = (uv - vec2(0.5)) * resolution;
    float inset = min(resolution.x, resolution.y) * 0.015;
    vec2 halfSize;
    if (mode == 1)
    {
        halfSize = resolution * 0.5 - vec2(inset);
    }
    else
    {
        float targetAspect = mode == 2 ? 1.37 : (mode == 3 ? 1.85 : 2.39);
        vec2 availableSize = max(resolution - vec2(inset * 2.0), vec2(1.0));
        float frameAspect = availableSize.x / max(availableSize.y, 1.0);
        halfSize = availableSize * 0.5;
        if (frameAspect > targetAspect)
            halfSize.x = availableSize.y * targetAspect * 0.5;
        else
            halfSize.y = availableSize.x / targetAspect * 0.5;
    }
    float radius = roundness * min(halfSize.x, halfSize.y);
    float signedDistance = RoundedBoxSdf(pPx, halfSize, radius);
    return 1.0 - smoothstep(-feather, feather, signedDistance);
}

void main()
{
    vec2 resolution = vec2(textureSize(uSceneColor, 0));
    vec2 sampleUv = ApplyGateWeave(vUV, resolution);
    bool insideSource = all(greaterThanEqual(sampleUv, vec2(0.0))) &&
                        all(lessThanEqual(sampleUv, vec2(1.0)));
    vec4 src = insideSource ? GE_TAP_LOD0(uSceneColor, sampleUv) : vec4(0.0, 0.0, 0.0, 1.0);
    vec3 rgb = src.rgb;

    float hair = HairCoverage(gl_FragCoord.xy, resolution) *
                 clamp(pc.filmSimulationHairIntensity, 0.0, 1.0);
    rgb *= 1.0 - hair;

    float scratchPolarity;
    float scratch = ScratchCoverage(gl_FragCoord.xy, resolution, scratchPolarity) *
                    clamp(pc.filmSimulationScratchIntensity, 0.0, 1.0);
    rgb = scratchPolarity > 0.0 ? mix(rgb, vec3(1.0), scratch) : rgb * (1.0 - scratch);

    float dustPolarity;
    float dust = DustCoverage(gl_FragCoord.xy, resolution, dustPolarity) *
                 clamp(pc.filmSimulationDustIntensity, 0.0, 1.0);
    rgb = dustPolarity > 0.0 ? rgb * (1.0 - dust) : mix(rgb, vec3(1.0), dust);

    float gate = GateMaskCoverage(vUV, resolution);
    rgb *= gate;
    oColor = vec4(max(rgb, vec3(0.0)), src.a);
}
