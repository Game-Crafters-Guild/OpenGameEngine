#version 450

// Volumetric cloud raymarch (SebLague/Clouds port). Marches the camera ray
// through an axis-aligned container box, sampling baked shape/detail Worley
// noise, with a per-step light march toward the sun (Beer's law + two-lobe
// Henyey-Greenstein). Outputs the packed per-pixel march result
// (R=light energy, G=transmittance, B=mean cloud distance) that
// volumetric_clouds_resolve.frag temporally filters and
// volumetric_clouds_composite.frag folds over scene color.

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oClouds;

layout(set = 0, binding = 0) uniform sampler2D uDepth;
layout(set = 0, binding = 1) uniform sampler3D uShapeNoise;
layout(set = 0, binding = 2) uniform sampler3D uDetailNoise;

layout(set = 0, binding = 3, std140) uniform ViewParamsBlock
{
    // Full 464 B ViewParams block (ge_prevViewProj/ge_invView drive the
    // temporal reprojection). Relative include path into the Rendering module.
#define GE_VP_MAT4(name) mat4 name;
#define GE_VP_VEC4(name) vec4 name;
#include "../../Rendering/Shaders/Includes/view_params_fields.glsl"
#undef GE_VP_VEC4
#undef GE_VP_MAT4
} ViewParams;

layout(set = 0, binding = 4, std140) uniform CloudParamsBlock
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

#if defined(GE_COMPAT_PROFILE)
// The compat cook samples raw volumes because rgba16f storage cannot be read-write.
layout(set = 0, binding = 5, std430) readonly buffer ShapeNoiseMinMaxBlock
{
    int minMax[8];
} ShapeNoiseMinMax;

layout(set = 0, binding = 6, std430) readonly buffer DetailNoiseMinMaxBlock
{
    int minMax[8];
} DetailNoiseMinMax;

// Fixed-point scale the bake kernels use for their atomic min/max.
const int kMinMaxAccuracy = 1 << 22;

float RescaleNoise(float value, int packedMax, int packedMin)
{
    float maxVal = float(packedMax) / float(kMinMaxAccuracy);
    float minVal = 1.0 - float(packedMin) / float(kMinMaxAccuracy);
    float range = maxVal - minVal;
    return range > 1e-5 ? (value - minVal) / range : value;
}
#endif

layout(push_constant) uniform CloudsPC
{
    float shaderAnimationTime;
} pc;

const int kMaxMarchSteps = 256;
// Unity's _Time.x is seconds/20; upstream animation speeds are tuned to it.
const float kUnityTimeXScale = 1.0 / 20.0;
// Animation-clock units -> world units for the wind drift. With timeScale and
// baseSpeed at 1 this is 50 world units/second.
const float kWorldWind = 1000.0;

float Remap(float v, float minOld, float maxOld, float minNew, float maxNew)
{
    return minNew + (v - minOld) * (maxNew - minNew) / (maxOld - minOld);
}

// The cloud volume is a vertical cylinder (a disc in XZ, a slab in Y) centered
// on the viewer, so the layer has no corners and no reachable edge. Returns
// (dstToVolume, dstInsideVolume); the second is zero when the ray misses.
vec2 RayCloudVolumeDst(vec3 rayOrigin, vec3 rayDir, vec2 centerXZ, float radius,
                       float yMin, float yMax)
{
    const float kFar = 1e9;

    // Horizontal slab in Y.
    float tSlabMin = 0.0;
    float tSlabMax = kFar;
    if (abs(rayDir.y) < 1e-6)
    {
        if (rayOrigin.y < yMin || rayOrigin.y > yMax)
            return vec2(0.0);
    }
    else
    {
        float t0 = (yMin - rayOrigin.y) / rayDir.y;
        float t1 = (yMax - rayOrigin.y) / rayDir.y;
        tSlabMin = min(t0, t1);
        tSlabMax = max(t0, t1);
    }

    // Infinite vertical cylinder of the given radius.
    float tCylMin = 0.0;
    float tCylMax = kFar;
    vec2 oc = rayOrigin.xz - centerXZ;
    float a = dot(rayDir.xz, rayDir.xz);
    float c = dot(oc, oc) - radius * radius;
    if (a < 1e-8)
    {
        if (c > 0.0)
            return vec2(0.0); // straight up/down outside the disc
    }
    else
    {
        float b = 2.0 * dot(oc, rayDir.xz);
        float disc = b * b - 4.0 * a * c;
        if (disc < 0.0)
            return vec2(0.0);
        float sq = sqrt(disc);
        tCylMin = (-b - sq) / (2.0 * a);
        tCylMax = (-b + sq) / (2.0 * a);
    }

    float tEnter = max(max(tSlabMin, tCylMin), 0.0);
    float tExit = min(tSlabMax, tCylMax);
    return vec2(tEnter, max(0.0, tExit - tEnter));
}

float HenyeyGreenstein(float a, float g)
{
    float g2 = g * g;
    return (1.0 - g2) / (4.0 * 3.1415 * pow(1.0 + g2 - 2.0 * g * a, 1.5));
}

float Phase(float a)
{
    float blend = 0.5;
    float hgBlend = HenyeyGreenstein(a, CloudParams.phaseForward) * (1.0 - blend) +
                    HenyeyGreenstein(a, -CloudParams.phaseBack) * blend;
    return CloudParams.phaseBase + hgBlend * CloudParams.phaseFactor;
}

float InterleavedGradientNoise(vec2 pixel)
{
    return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}


float SampleDensity(vec3 rayPos)
{
    const float kBaseScale = 1.0 / 1000.0;
    const float kOffsetSpeed = 1.0 / 100.0;

    float thickness = max(CloudParams.thickness, 1.0);
    float radius = max(CloudParams.radius, 1.0);
    vec2 centerXZ = ViewParams.ge_cameraPosWS.xz;

    float time = pc.shaderAnimationTime * kUnityTimeXScale * CloudParams.timeScale;
    // Sampled in WORLD space: the disc follows the viewer, the cloud shapes
    // do not, so flying through the layer moves you past clouds instead of
    // dragging them along.
    vec3 uvw = rayPos * kBaseScale * CloudParams.cloudScale;
    vec3 shapeOffset = vec3(CloudParams.shapeOffsetX, CloudParams.shapeOffsetY, CloudParams.shapeOffsetZ);
    // Wind is a WORLD-space drift (kWorldWind converts the animation clock to
    // world units) folded into the sample position at the same scale as the
    // position itself, so changing CloudScale resizes the clouds without also
    // changing how fast they travel.
    vec3 windShape = vec3(time, time * 0.1, time * 0.2) * CloudParams.baseSpeed * kWorldWind;
    vec3 shapeSamplePos = (rayPos + windShape) * kBaseScale * CloudParams.cloudScale +
                          shapeOffset * kOffsetSpeed;

    // Radial falloff toward the rim of the disc, over its outer fifth — the
    // round counterpart of upstream's fixed 50-unit box-edge fade, and what
    // keeps the volume's boundary from reading as a wall.
    float distXZ = length(rayPos.xz - centerXZ);
    float edgeWeight = clamp((radius - distXZ) / max(radius * 0.2, 1.0), 0.0, 1.0);

    float gMin = 0.2;
    float gMax = 0.7;
    float heightPercent = (rayPos.y - CloudParams.altitude) / thickness;
    float heightGradient = clamp(Remap(heightPercent, 0.0, gMin, 0.0, 1.0), 0.0, 1.0) *
                           clamp(Remap(heightPercent, 1.0, gMax, 0.0, 1.0), 0.0, 1.0);
    heightGradient *= edgeWeight;

    vec4 shapeNoise = textureLod(uShapeNoise, shapeSamplePos, 0.0);
#if defined(GE_COMPAT_PROFILE)
    for (int channel = 0; channel < 4; ++channel)
    {
        shapeNoise[channel] = RescaleNoise(shapeNoise[channel],
                                           ShapeNoiseMinMax.minMax[channel * 2 + 0],
                                           ShapeNoiseMinMax.minMax[channel * 2 + 1]);
    }
#endif
    vec4 shapeWeights = vec4(CloudParams.shapeWeightR, CloudParams.shapeWeightG, CloudParams.shapeWeightB, CloudParams.shapeWeightA);
    vec4 normalizedShapeWeights = shapeWeights / max(dot(shapeWeights, vec4(1.0)), 1e-4);
    float shapeFBM = dot(shapeNoise, normalizedShapeWeights) * heightGradient;
    float baseShapeDensity = shapeFBM + CloudParams.densityOffset * 0.1;

    // Detail erosion only where the base shape has substance.
    if (baseShapeDensity > 0.0)
    {
        vec3 detailOffset = vec3(CloudParams.detailOffsetX, CloudParams.detailOffsetY, CloudParams.detailOffsetZ);
        vec3 windDetail = vec3(time * 0.4, -time, time * 0.1) * CloudParams.detailSpeed * kWorldWind;
        vec3 detailSamplePos = (rayPos + windDetail) * kBaseScale * CloudParams.cloudScale *
                                   CloudParams.detailNoiseScale +
                               detailOffset * kOffsetSpeed;
        vec3 detailNoise = textureLod(uDetailNoise, detailSamplePos, 0.0).rgb;
#if defined(GE_COMPAT_PROFILE)
        for (int channel = 0; channel < 3; ++channel)
        {
            detailNoise[channel] = RescaleNoise(detailNoise[channel],
                                                DetailNoiseMinMax.minMax[channel * 2 + 0],
                                                DetailNoiseMinMax.minMax[channel * 2 + 1]);
        }
#endif
        vec3 detailWeights = vec3(CloudParams.detailWeightR, CloudParams.detailWeightG, CloudParams.detailWeightB);
        vec3 normalizedDetailWeights = detailWeights / max(dot(detailWeights, vec3(1.0)), 1e-4);
        float detailFBM = dot(detailNoise, normalizedDetailWeights);

        // Edges erode more than the interior.
        float oneMinusShape = 1.0 - shapeFBM;
        float detailErodeWeight = oneMinusShape * oneMinusShape * oneMinusShape;
        float cloudDensity = baseShapeDensity - (1.0 - detailFBM) * detailErodeWeight * CloudParams.detailNoiseWeight;

        return cloudDensity * CloudParams.densityMultiplier * 0.1;
    }
    return 0.0;
}

// Proportion of sunlight reaching the given point through the cloud volume.
float LightMarch(vec3 position, vec3 dirToLight)
{
    float dstInsideVolume = RayCloudVolumeDst(position, dirToLight,
                                              ViewParams.ge_cameraPosWS.xz,
                                              max(CloudParams.radius, 1.0),
                                              CloudParams.altitude,
                                              CloudParams.altitude + max(CloudParams.thickness, 1.0)).y;

    int steps = max(CloudParams.numStepsLight, 1);
    float stepSize = dstInsideVolume / float(steps);
    float totalDensity = 0.0;

    for (int i = 0; i < steps; ++i)
    {
        position += dirToLight * stepSize;
        totalDensity += max(0.0, SampleDensity(position) * stepSize);
    }

    float transmittance = exp(-totalDensity * CloudParams.lightAbsorptionTowardSun);
    return CloudParams.darknessThreshold + transmittance * (1.0 - CloudParams.darknessThreshold);
}

vec2 ViewportUvToNdc(vec2 uv)
{
    return vec2(uv.x * 2.0 - 1.0, (1.0 - uv.y) * 2.0 - 1.0);
}

void main()
{

    // Reverse-Z: near -> 1.0, far/sky -> 0.0.
    float rawDepth = texture(uDepth, vUV).r;
    bool sky = rawDepth <= 0.000001;
    vec2 ndc = ViewportUvToNdc(vUV);
    vec4 pV = ViewParams.ge_invProj * vec4(ndc, sky ? 0.5 : rawDepth, 1.0);
    pV.xyz /= max(pV.w, 1e-6);
    float sceneDst = sky ? 1e12 : length(pV.xyz);

    vec3 viewDirV = normalize(pV.xyz);
    vec3 rayDir = normalize(mat3(ViewParams.ge_invView) * viewDirV);
    vec3 rayOrigin = ViewParams.ge_cameraPosWS.xyz;

    vec2 rayToVolume = RayCloudVolumeDst(rayOrigin, rayDir, ViewParams.ge_cameraPosWS.xz,
                                         max(CloudParams.radius, 1.0), CloudParams.altitude,
                                         CloudParams.altitude + max(CloudParams.thickness, 1.0));
    float dstToVolume = rayToVolume.x;
    float dstInsideVolume = rayToVolume.y;
    vec3 entryPoint = rayOrigin + rayDir * dstToVolume;

    // STATIC jittered march start (upstream's blue-noise offset): spatial
    // dither breaks up step banding while staying temporally stable. An
    // animated offset re-rolls every edge pixel's hit/miss each frame, and
    // the residual variance the history blend cannot remove reads as a
    // stippled fringe along cloud/sky boundaries.
    float randomOffset = InterleavedGradientNoise(gl_FragCoord.xy) *
                         CloudParams.rayOffsetStrength;

    vec3 dirToLight = normalize(vec3(CloudParams.sunDirX, CloudParams.sunDirY, CloudParams.sunDirZ));
    float cosAngle = dot(rayDir, dirToLight);
    float phaseVal = Phase(cosAngle);

    float dstTravelled = randomOffset;
    float dstLimit = min(sceneDst - dstToVolume, dstInsideVolume);
    // Widen the step rather than truncate when the container is deeper than
    // the iteration cap can cover at the authored step length.
    float stepSize = max(max(CloudParams.stepSize, 0.5), dstLimit / float(kMaxMarchSteps));

    float transmittance = 1.0;
    float lightEnergy = 0.0;
    // Transmittance-weighted mean cloud distance: the depth the temporal
    // reprojection uses. A fixed representative depth breaks down when the
    // camera orbits inside the volume — nearby wisps have far more parallax
    // than a box-fraction guess, which reads as streaking.
    float depthWeightSum = 0.0;
    float weightedDepth = 0.0;

    for (int i = 0; i < kMaxMarchSteps; ++i)
    {
        if (dstTravelled >= dstLimit)
            break;
        vec3 rayPos = entryPoint + rayDir * dstTravelled;
        float density = SampleDensity(rayPos);

        if (density > 0.0)
        {
            float lightTransmittance = LightMarch(rayPos, dirToLight);
            lightEnergy += density * stepSize * transmittance * lightTransmittance * phaseVal;
            float contribution = density * stepSize * transmittance;
            depthWeightSum += contribution;
            weightedDepth += contribution * (dstToVolume + dstTravelled);
            transmittance *= exp(-density * stepSize * CloudParams.lightAbsorptionThroughCloud);

            if (transmittance < 0.01)
                break;
        }
        dstTravelled += stepSize;
    }

    // Packed march output for the resolve pass (light energy is scalar until
    // the sun-color multiply, which the composite applies):
    //   R = accumulated light energy, G = transmittance,
    //   B = transmittance-weighted mean cloud distance (the reprojection
    //       depth; box-fraction fallback when the ray gathered no cloud).
    // Rays that never touch the container reproject as far-field (rotation-
    // only): a near fallback would give sky pixels maximal parallax error.
    float meanCloudDist = depthWeightSum > 1e-5
                              ? weightedDepth / depthWeightSum
                              : (dstInsideVolume <= 0.0
                                     ? 60000.0
                                     : dstToVolume + min(0.25 * dstInsideVolume,
                                                         0.5 * max(dstLimit, 0.0)));
    oClouds = vec4(lightEnergy, transmittance, min(meanCloudDist, 60000.0), 0.0);
}
