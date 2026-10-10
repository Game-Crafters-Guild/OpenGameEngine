#version 450

#include "Includes/compat_profile.glsl"

// Temporal resolve for the cloud march (TAA-grade, mirroring the structure of
// volumetric_fog_temporal.comp / taa_resolve.comp): reprojects this pass's own
// previous output through the previous view-proj at the march's per-pixel mean
// cloud distance, clamps it against the 3x3 neighborhood of the CURRENT march
// (variance rejection — the piece an inline single-sample blend cannot have),
// and blends. Packed layout in and out: R=light energy, G=transmittance,
// B=mean cloud distance.

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oResolved;

layout(set = 0, binding = 0) uniform sampler2D uMarch;
layout(set = 0, binding = 1) uniform sampler2D uHistory;

layout(set = 0, binding = 2, std140) uniform ViewParamsBlock
{
#define GE_VP_MAT4(name) mat4 name;
#define GE_VP_VEC4(name) vec4 name;
#include "../../Rendering/Shaders/Includes/view_params_fields.glsl"
#undef GE_VP_VEC4
#undef GE_VP_MAT4
} ViewParams;

layout(set = 0, binding = 3, std140) uniform CloudResolveParamsBlock
{
    float radius;
    float altitude;
    float thickness;
    int numStepsLight;
    float stepSize;
    float rayOffsetStrength;
    float cloudScale;
    float densityMultiplier;
    float densityOffset;
    float shapeOffsetX;
    float shapeOffsetY;
    float shapeOffsetZ;
    float shapeWeightR;
    float shapeWeightG;
    float shapeWeightB;
    float shapeWeightA;
    float detailNoiseScale;
    float detailNoiseWeight;
    float detailWeightR;
    float detailWeightG;
    float detailWeightB;
    float detailOffsetX;
    float detailOffsetY;
    float detailOffsetZ;
    float lightAbsorptionThroughCloud;
    float lightAbsorptionTowardSun;
    float darknessThreshold;
    float phaseForward;
    float phaseBack;
    float phaseBase;
    float phaseFactor;
    float timeScale;
    float baseSpeed;
    float detailSpeed;
    float sunDirX;
    float sunDirY;
    float sunDirZ;
    float sunColorR;
    float sunColorG;
    float sunColorB;
    float sunIntensity;
    float historyWeight;
} CloudParams;

layout(push_constant) uniform CloudsResolvePC
{
    float cloudsHistoryValid;
} pc;

// 5-tap optimized Catmull-Rom (taa_resolve_upscale.comp) — bilinear history
// resampling blurs a little every frame, which reads as smear under camera
// rotation. extent.xy = size, extent.zw = 1/size.
//
// The compatibility cook samples these mip-less render targets at explicit
// level 0: WGSL rejects implicit derivatives behind the early exits and in the
// neighbourhood loop. GE_TAP_LOD0 retains native sampling for other cooks.
vec4 SampleCatmullRom(sampler2D tex, vec2 uv, vec4 extent)
{
    vec2 texelSize = extent.zw;
    vec2 samplePos = uv * extent.xy;
    vec2 texPos1 = floor(samplePos - 0.5) + 0.5;
    vec2 f = samplePos - texPos1;
    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    vec2 w3 = f * f * (-0.5 + 0.5 * f);
    vec2 w12 = w1 + w2;
    vec2 offset12 = w2 / max(w12, vec2(1e-5));
    vec2 texPos0 = (texPos1 - 1.0) * texelSize;
    vec2 texPos3 = (texPos1 + 2.0) * texelSize;
    vec2 texPos12 = (texPos1 + offset12) * texelSize;
    vec4 result = GE_TAP_LOD0(tex, vec2(texPos12.x, texPos0.y)) * (w12.x * w0.y) +
                  GE_TAP_LOD0(tex, vec2(texPos0.x, texPos12.y)) * (w0.x * w12.y) +
                  GE_TAP_LOD0(tex, vec2(texPos12.x, texPos12.y)) * (w12.x * w12.y) +
                  GE_TAP_LOD0(tex, vec2(texPos3.x, texPos12.y)) * (w3.x * w12.y) +
                  GE_TAP_LOD0(tex, vec2(texPos12.x, texPos3.y)) * (w12.x * w3.y);
    float weightSum = (w12.x * w0.y) + (w0.x * w12.y) + (w12.x * w12.y) +
                      (w3.x * w12.y) + (w12.x * w3.y);
    return max(result / max(weightSum, 1e-5), vec4(0.0));
}

vec2 ViewportUvToNdc(vec2 uv)
{
    return vec2(uv.x * 2.0 - 1.0, (1.0 - uv.y) * 2.0 - 1.0);
}

void main()
{
    vec4 current = GE_TAP_LOD0(uMarch, vUV);

    if (pc.cloudsHistoryValid < 0.5)
    {
        oResolved = current;
        return;
    }

    // Reconstruct this pixel's world-space ray and reproject the march's mean
    // cloud point through the previous frame's view-proj.
    vec2 ndc = ViewportUvToNdc(vUV);
    vec4 pV = ViewParams.ge_invProj * vec4(ndc, 0.5, 1.0);
    pV.xyz /= max(pV.w, 1e-6);
    vec3 rayDir = normalize(mat3(ViewParams.ge_invView) * normalize(pV.xyz));
    vec3 repPoint = ViewParams.ge_cameraPosWS.xyz + rayDir * max(current.b, 1.0);

    vec4 prevClip = ViewParams.ge_prevViewProj * vec4(repPoint, 1.0);
    if (prevClip.w <= 1e-4)
    {
        oResolved = current;
        return;
    }
    vec2 prevNdc = prevClip.xy / prevClip.w + ViewParams.ge_taaJitter.zw;
    vec2 prevUV = vec2(prevNdc.x * 0.5 + 0.5, 1.0 - (prevNdc.y * 0.5 + 0.5));

    // Guard band: the Catmull-Rom kernel reaches 2 texels out, and
    // edge-clamped taps smear border texels into streaks during rotation.
    vec2 guard = 2.0 * ViewParams.ge_screenSize.zw;
    if (any(lessThan(prevUV, guard)) || any(greaterThan(prevUV, vec2(1.0) - guard)))
    {
        oResolved = current;
        return;
    }

    vec4 history = SampleCatmullRom(uHistory, prevUV,
                                    vec4(ViewParams.ge_screenSize.xy,
                                         ViewParams.ge_screenSize.zw));

    // Neighborhood variance clamp over the CURRENT march's 3x3 (the
    // volumetric_fog_temporal.comp recipe): stale history is bounded by what
    // the scene can currently produce here, so trails die in a frame or two
    // while a converged still image is untouched by the clamp.
    vec2 texel = ViewParams.ge_screenSize.zw;
    vec2 minLumT = current.rg;
    vec2 maxLumT = current.rg;
    for (int y = -1; y <= 1; ++y)
    for (int x = -1; x <= 1; ++x)
    {
        vec2 n = GE_TAP_LOD0(uMarch, vUV + vec2(x, y) * texel).rg;
        minLumT = min(minLumT, n);
        maxLumT = max(maxLumT, n);
    }
    vec2 range = maxLumT - minLumT;
    vec2 padLumT = max(range * 0.35, vec2(0.015, 0.01));
    history.rg = clamp(history.rg, minLumT - padLumT, maxLumT + padLumT);
    history.g = clamp(history.g, 0.0, 1.0);

    float w = clamp(CloudParams.historyWeight, 0.0, 0.98);
    vec2 blended = mix(current.rg, history.rg, w);

    // Depth is not blended — the current march's mean distance is the truth
    // the NEXT frame should reproject with.
    oResolved = vec4(blended.x, blended.y, current.b, 0.0);
}
