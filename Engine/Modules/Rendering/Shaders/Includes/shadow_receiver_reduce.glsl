#ifndef GE_SHADOW_RECEIVER_REDUCE_GLSL
#define GE_SHADOW_RECEIVER_REDUCE_GLSL

// SDSM over the view's final depth, after every surface that receives the
// directional cascades has written it. Three results per view, read back by the
// CPU a few frames later (ShadowReceiverMeasurement.h owns the layout):
//
//   1. the nearest and farthest visible depth (reverse-Z NDC bits), which
//      narrow the cascade split range;
//   2. per view-depth bin, the light-space AABB of the surfaces in that bin
//      (Lauritzen et al. 2011, "Sample Distribution Shadow Maps", partition
//      bounds);
//   3. per view-depth bin, the light-space AABB of the ray parameter of the
//      samples whose surface is in that bin, and one more record for the
//      samples whose ray reaches past every bin (sky, or a surface beyond the
//      last bin). From them the CPU bounds the visible air in front of the
//      surfaces, where volumetric fog, transparent surfaces and the sun glare
//      look the cascades up.
//
// Each cascade's box and caster set are fitted to the surfaces and the air
// together instead of to the camera frustum slice.
//
// The includer defines GE_RECEIVER_DEPTH(pixel) to fetch one depth sample:
// sample 0 of a multisampled depth, or the single sample of a resolved one.
//
// Reverse-Z: the clear value is 0.0 and is skipped (sky). The buffer's MIN
// depth is the FARTHEST sample and its MAX the NEAREST.

#include "screen_position.glsl"

// Bins are log-spaced in view depth between the camera near plane and the
// farthest depth a cascade can be sampled at; must equal
// ShadowReceiverMeasurement::kDepthBins.
#define GE_RECEIVER_DEPTH_BINS 64
// Ray records: one per bin, then the rays that reach past every bin; must equal
// ShadowReceiverMeasurement::kRayRecords.
#define GE_RECEIVER_RAY_RECORDS (GE_RECEIVER_DEPTH_BINS + 1)
// Words per side of the result buffer: the depth word, three light-space axes
// per bin's surface box, then three per ray record.
#define GE_RECEIVER_WORDS (1 + 3 * GE_RECEIVER_DEPTH_BINS + 3 * GE_RECEIVER_RAY_RECORDS)
#define GE_RECEIVER_RAYS_BASE (1u + 3u * uint(GE_RECEIVER_DEPTH_BINS))

layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;

// Spatial stride between sampled pixels (one pixel in four). A receiver
// between two samples lies within a pixel footprint of a sampled one; the CPU
// margins absorb that.
const uint kSamplingStride = 2u;
// Each thread reduces kSamplesPerThread x kSamplesPerThread neighbouring
// samples in registers, merging the ones that fall into the same depth bin
// before it touches shared memory: neighbours mostly share a bin, and at a
// close camera nearly every sample lands in a few bins whose words the shared
// atomics would otherwise serialise on. The dispatch covers
// kSamplingStride * kSamplesPerThread pixels per thread on each axis
// (ShadowReceiverReduce::DispatchGroups).
const uint kSamplesPerThread = 2u;

// Every word is an order-preserving encoding, so one atomicMin / atomicMax per
// word merges any number of workgroups. The pass fills MinWords with
// 0xFFFFFFFF and MaxWords with 0 before the dispatch.
layout(set = 0, binding = 1, std430) buffer ReceiverBoundsBuffer
{
    uint MinWords[GE_RECEIVER_WORDS];
    uint MaxWords[GE_RECEIVER_WORDS];
};

layout(set = 0, binding = 2, std140) uniform ReceiverReduceParams
{
    // Render-origin-relative inverse camera view-projection: NDC -> position.
    mat4 InvViewProjRel;
    // Rows of the light's rotation-only view basis (the cascade fit's light
    // space), applied to render-origin-relative positions.
    vec4 LightRow0;
    vec4 LightRow1;
    vec4 LightRow2;
    // xyz: render-origin-relative camera position.
    vec4 CameraPosRel;
    // xyz: unit camera forward; w: 1 under an orthographic projection.
    vec4 CameraForward;
    // x: first bin's near depth; y: bins per natural-log unit of depth;
    // z: last bin's far depth (a surface beyond it reaches no cascade; its ray
    // passes every bin).
    vec4 BinParams;
    // xy: depth extent in pixels.
    uvec4 Extent;
};

const uint kMinNeutral = 0xFFFFFFFFu;
const uint kMaxNeutral = 0u;

// Workgroup-local copy of the result, flushed with one global atomic per word
// a sample reached.
shared uint sMin[GE_RECEIVER_WORDS];
shared uint sMax[GE_RECEIVER_WORDS];

// Order-preserving float -> uint: a larger float maps to a larger uint for
// either sign, so unsigned atomicMin / atomicMax order light-space coordinates.
uint EncodeOrdered(float f)
{
    uint bits = floatBitsToUint(f);
    return (bits & 0x80000000u) != 0u ? ~bits : (bits | 0x80000000u);
}

// Folds a light-space box into the workgroup's copy at word `base`.
void FlushBox(uint base, vec3 lo, vec3 hi)
{
    atomicMin(sMin[base + 0u], EncodeOrdered(lo.x));
    atomicMin(sMin[base + 1u], EncodeOrdered(lo.y));
    atomicMin(sMin[base + 2u], EncodeOrdered(lo.z));
    atomicMax(sMax[base + 0u], EncodeOrdered(hi.x));
    atomicMax(sMax[base + 1u], EncodeOrdered(hi.y));
    atomicMax(sMax[base + 2u], EncodeOrdered(hi.z));
}

// Folds one bin's surface box and ray box into the workgroup's copy.
void FlushBin(uint bin, vec3 lo, vec3 hi, vec3 rayLo, vec3 rayHi)
{
    FlushBox(1u + 3u * bin, lo, hi);
    FlushBox(GE_RECEIVER_RAYS_BASE + 3u * bin, rayLo, rayHi);
}

vec3 ToLightSpace(vec3 p)
{
    return vec3(dot(LightRow0.xyz, p), dot(LightRow1.xyz, p), dot(LightRow2.xyz, p));
}

// The ray parameter of the light-space point `ls` at view depth `viewDepth`:
// the ray's direction per unit view depth under a perspective camera, its
// offset from the camera under an orthographic one
// (ShadowReceiverMeasurement::RayPointsAt).
vec3 RayParameter(vec3 ls, float viewDepth, vec3 cameraLs, vec3 forwardLs)
{
    vec3 fromCamera = ls - cameraLs;
    return CameraForward.w > 0.5 ? fromCamera - viewDepth * forwardLs : fromCamera / viewDepth;
}

void main()
{
    uint tid = gl_LocalInvocationIndex;
    for (uint w = tid; w < uint(GE_RECEIVER_WORDS); w += 256u)
    {
        sMin[w] = kMinNeutral;
        sMax[w] = kMaxNeutral;
    }
    barrier();

    vec3 cameraLs = ToLightSpace(CameraPosRel.xyz);
    vec3 forwardLs = ToLightSpace(CameraForward.xyz);

    // This thread's running results: the depth extremes, the surface and ray
    // boxes of the bin its last samples fell into (kNoBin until one did), and
    // the ray box of its samples that reach past every bin.
    const uint kNoBin = 0xFFFFFFFFu;
    uint depthMin = kMinNeutral;
    uint depthMax = kMaxNeutral;
    uint runBin = kNoBin;
    vec3 runLo = vec3(0.0);
    vec3 runHi = vec3(0.0);
    vec3 runRayLo = vec3(0.0);
    vec3 runRayHi = vec3(0.0);
    bool passed = false;
    vec3 passLo = vec3(0.0);
    vec3 passHi = vec3(0.0);
    uvec2 firstSample = gl_GlobalInvocationID.xy * kSamplesPerThread;
    for (uint i = 0u; i < kSamplesPerThread * kSamplesPerThread; ++i)
    {
        uvec2 samplePos = (firstSample + uvec2(i % kSamplesPerThread, i / kSamplesPerThread)) * kSamplingStride;
        if (samplePos.x >= Extent.x || samplePos.y >= Extent.y)
            continue;
        float d = GE_RECEIVER_DEPTH(ivec2(samplePos));
        bool sky = d <= 0.0;
        if (!sky)
        {
            uint bits = floatBitsToUint(d);
            depthMin = min(depthMin, bits);
            depthMax = max(depthMax, bits);
        }

        // The sky's ray is taken at the near plane (reverse-Z NDC depth 1).
        vec2 viewportUV = (vec2(samplePos) + 0.5) / vec2(Extent.xy);
        vec4 h = InvViewProjRel * vec4(GE_ViewportUVToYUpNdc(viewportUV), sky ? 1.0 : d, 1.0);
        vec3 p = h.xyz / h.w;
        float viewDepth = dot(p - CameraPosRel.xyz, CameraForward.xyz);
        if (viewDepth <= 0.0)
            continue;
        vec3 ls = ToLightSpace(p);
        vec3 ray = RayParameter(ls, viewDepth, cameraLs, forwardLs);
        if (sky || viewDepth > BinParams.z)
        {
            passLo = passed ? min(passLo, ray) : ray;
            passHi = passed ? max(passHi, ray) : ray;
            passed = true;
            continue;
        }
        // Depths in front of the first bin's near edge fold into bin 0.
        float t = log(max(viewDepth, BinParams.x) / BinParams.x) * BinParams.y;
        uint bin = min(uint(t), uint(GE_RECEIVER_DEPTH_BINS - 1));
        if (bin == runBin)
        {
            runLo = min(runLo, ls);
            runHi = max(runHi, ls);
            runRayLo = min(runRayLo, ray);
            runRayHi = max(runRayHi, ray);
            continue;
        }
        if (runBin != kNoBin)
            FlushBin(runBin, runLo, runHi, runRayLo, runRayHi);
        runBin = bin;
        runLo = ls;
        runHi = ls;
        runRayLo = ray;
        runRayHi = ray;
    }
    if (runBin != kNoBin)
        FlushBin(runBin, runLo, runHi, runRayLo, runRayHi);
    if (passed)
        FlushBox(GE_RECEIVER_RAYS_BASE + 3u * uint(GE_RECEIVER_DEPTH_BINS), passLo, passHi);
    if (depthMin != kMinNeutral)
    {
#if defined(GE_COMPAT_PROFILE)
        // The SPIR-V front end of naga 29.x (the cook's pin in
        // Tools/ShaderCook/toolchain/manifest.json) does not carry a loop's
        // running value into a later block when an atomic takes it as its
        // operand, and rejects the module the cook's `glslc -O` emits for the
        // arm below #else. Every non-sky sample updates both, so depthMin <=
        // depthMax here and the min/max are exact integer identities computed
        // in this block, which -O does not fold away. The arm can go once the
        // pinned naga cooks the #else arm: WebBuiltinShaderMetadata passes
        // either way, so check by cooking without it.
        atomicMin(sMin[0], min(depthMin, depthMax));
        atomicMax(sMax[0], max(depthMax, depthMin));
#else
        atomicMin(sMin[0], depthMin);
        atomicMax(sMax[0], depthMax);
#endif
    }
    barrier();

    // Flush only the words a sample reached: an empty bin costs no global
    // atomic, and the buffer's neutral fill survives for the CPU to read as empty.
    for (uint w = tid; w < uint(GE_RECEIVER_WORDS); w += 256u)
    {
        if (sMin[w] != kMinNeutral)
            atomicMin(MinWords[w], sMin[w]);
        if (sMax[w] != kMaxNeutral)
            atomicMax(MaxWords[w], sMax[w]);
    }
}

#endif // GE_SHADOW_RECEIVER_REDUCE_GLSL
