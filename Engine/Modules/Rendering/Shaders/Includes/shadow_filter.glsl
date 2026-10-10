#ifndef GE_SHADOW_FILTER_GLSL
#define GE_SHADOW_FILTER_GLSL

#include "compat_profile.glsl"
#include "shadow_receiver_plane.glsl"

// A separable [1,4,6,4,1] kernel convolved with bilinear reconstruction
// touches six texels on each axis. Pair their weights (a,b): one hardware
// sample at b/(a+b), weighted by a+b, reproduces that pair exactly.
// This direct derivation reduces 25 weighted PCF samples to nine.
void GE_ShadowPcfAxis(float phase, out vec3 offsets, out vec3 weights)
{
    vec3 left = vec3(1.0 - phase, 6.0 - 2.0 * phase, 1.0 + 3.0 * phase);
    vec3 right = vec3(4.0 - 3.0 * phase, 4.0 + 2.0 * phase, phase);
    weights = left + right;
    offsets = vec3(-2.0, 0.0, 2.0) + right / weights;
}

// Vogel disk: sample positions on a unit disk, spaced by the golden angle with
// radius sqrt((i + 0.5) / N), so any tap count covers the disk evenly.
// `sc` is the per-pixel rotation (cos a, sin a) applied on top of the spiral.
vec2 GE_VogelDisk(int sampleIndex, int totalSamples, vec2 sc)
{
    const float goldenAngle = 2.39996323; // 137.5° in radians
    float r = sqrt((float(sampleIndex) + 0.5) / float(totalSamples));
    float theta = float(sampleIndex) * goldenAngle;
    float ct = cos(theta);
    float st = sin(theta);
    return r * vec2(ct * sc.x - st * sc.y, ct * sc.y + st * sc.x);
}

// Poisson PCF sizes its radius in world units, so in texels it shrinks with the
// cascade texel and, once a fitted range makes a texel project to about a screen
// pixel, falls below the texel itself: the edge is then the unfiltered texel
// staircase, which crawls as the receiver or light turns. The floor keeps the
// kernel at least kShadowFilterFloorPixels of radius at the receiver; a uniform
// disk's 10-90 % edge is 1.37 radii, so about 2.7 px, which covers the
// one-to-two-pixel steps a texel near a pixel in size leaves.
//
// kShadowFilterFloorMaxTexels bounds it. The floor is set by the longer screen
// axis, which at a grazing view spans many texels per pixel; unbounded, the disk
// grows with it and reaches across the receiver's own slope in light space. Eight
// texels still floors a texel that projects to a quarter of a pixel, past which
// the edge's steps are sub-pixel already.
//
// The floor is the compat (web) profile's: it renders without TAA, so nothing
// else hides the staircase. The desktop profile keeps its kernels unchanged
// (#3548 owns whether desktop wants one). A shader that tests the floor defines
// GE_SHADOW_FILTER_FLOOR itself.
#if defined(GE_COMPAT_PROFILE) && !defined(GE_SHADOW_FILTER_FLOOR)
#define GE_SHADOW_FILTER_FLOOR 1
#endif
const float kShadowFilterFloorPixels = 2.0;
const float kShadowFilterFloorMaxTexels = 8.0;

// The floor's radius in shadow texels, 0 without GE_SHADOW_FILTER_FLOOR. uvDx and
// uvDy are the receiver's shadow-UV change per screen pixel; the longer one sets
// the floor, as a mip selection does, so the kernel spans the floor in every
// screen direction up to the bound.
float GE_ShadowFilterFloorTexels(vec2 uvDx, vec2 uvDy, vec2 size)
{
#if defined(GE_SHADOW_FILTER_FLOOR)
    float texelsPerPixel = max(length(uvDx * size), length(uvDy * size));
    return min(kShadowFilterFloorPixels * texelsPerPixel, kShadowFilterFloorMaxTexels);
#else
    return 0.0;
#endif
}

// The floor's radius in world units for the normal-bias footprint, from the
// receiver's world-position change per screen pixel, bounded as the kernel is.
// 0 without GE_SHADOW_FILTER_FLOOR.
float GE_ShadowFilterFloorWorld(vec3 receiverDx, vec3 receiverDy, float texelWorld)
{
#if defined(GE_SHADOW_FILTER_FLOOR)
    float worldPerPixel = max(length(receiverDx), length(receiverDy));
    return min(kShadowFilterFloorPixels * worldPerPixel, kShadowFilterFloorMaxTexels * max(texelWorld, 0.0));
#else
    return 0.0;
#endif
}

// The depth a floored disk tap compares at: the receiver's plane carried to the
// tap's offset, plus the bilinear compare's one-texel reach (as the grids'
// GE_ShadowReceiverPlaneDepth). A kernel the floor did not widen compares every
// tap at the centre's depth.
float GE_FlooredKernelTapDepth(bool floorBinds, float refDepth, vec2 offsetUV, vec2 receiverGradient, float texelSize)
{
    return floorBinds ? GE_ShadowReceiverPlaneDepth(refDepth, offsetUV, receiverGradient, texelSize) : refDepth;
}

float GE_ShadowPcf5x5(sampler2DArrayShadow shadowMap, vec2 uv, float layer, float depth, vec2 receiverGradient)
{
    vec2 size = vec2(textureSize(shadowMap, 0).xy);
    vec2 texelPosition = uv * size - 0.5;
    vec2 base = floor(texelPosition);
    vec3 offsetsX, offsetsY, weightsX, weightsY;
    GE_ShadowPcfAxis(fract(texelPosition.x), offsetsX, weightsX);
    GE_ShadowPcfAxis(fract(texelPosition.y), offsetsY, weightsY);

    float visibility = 0.0;
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 3; ++x)
        {
            vec2 sampleUV = (base + vec2(offsetsX[x], offsetsY[y]) + 0.5) / size;
            // ShadowComparePCF supplies the reverse-Z lit border (depth zero).
            // Let the sampler blend border texels too: rejecting the whole
            // bilinear sample at the UV boundary would change the kernel.
            visibility += weightsX[x] * weightsY[y] *
                GE_SHADOW_TAP(shadowMap, vec4(sampleUV, layer, depth +
                    dot(sampleUV - uv, receiverGradient) + dot(abs(receiverGradient), 1.0 / size)));
        }
    return visibility / 256.0;
}

float GE_ShadowPcf5x5(sampler2DArrayShadow shadowMap, vec2 uv, float layer, float depth)
{
    return GE_ShadowPcf5x5(shadowMap, uv, layer, depth, vec2(0.0));
}

// The authored world offset remains an upper bound. Limit normal displacement
// to the cascade's reconstruction footprint so fine near shadows stay attached
// to small receivers. Blend texelWorld between cascades BEFORE calling this
// function so both maps sample the same biased position through the transition.
// floorWorld is the Poisson kernel's floored radius (GE_ShadowFilterFloorWorld),
// 0 where no floor applies.
float GE_ShadowNormalBiasWorld(float authoredWorld, float texelWorld, float referenceTexelWorld,
                               int quality, float poissonSoftness, float cosTheta,
                               float maxPenumbraWorld, bool receiverPlaneBias, float floorWorld)
{
    float cosine = clamp(cosTheta, 0.0, 1.0);
    float authoredOffset = max(authoredWorld, 0.0) * (1.0 - cosine);
    // Moment filtering has its own preblur footprint; preserve its authored bias.
    if (quality == 4)
        return authoredOffset;
    float footprintWorld = 2.0 * max(texelWorld, 0.0);
    if (quality == 0)
        footprintWorld = 3.0 * max(texelWorld, 0.0);
    else if (quality == 2)
        footprintWorld = max(footprintWorld,
            max(1.5 * max(poissonSoftness, 0.0) * max(referenceTexelWorld, 0.0), floorWorld) + texelWorld);
    else if (quality == 5 || (quality == 3 && !receiverPlaneBias))
        footprintWorld = max(footprintWorld, max(maxPenumbraWorld, 0.0) + texelWorld);
    // A plane tilted by theta needs radius*sin(theta) normal displacement to
    // cover a radius*tan(theta) depth change across the filter. Using 1-cos for
    // this geometric bound under-biases gentle slopes. Never exceed the user's
    // original angular offset, even when they deliberately author a small value.
    return min(authoredOffset, footprintWorld * sqrt(max(1.0 - cosine * cosine, 0.0)));
}

#endif // GE_SHADOW_FILTER_GLSL
