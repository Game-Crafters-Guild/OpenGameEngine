#include "Engine/Rendering/ShadowMapRenderFeature.h"
#include "Engine/Rendering/CameraUtils.h"
#include "Engine/Rendering/RenderOrigin.h"
#include "Engine/Rendering/RenderServices.h"

#include "Logger/Logger.h"
#include "Rendering/CameraDerivation.h"
#include "Rendering/Common/Frustum.h"
#include "Rendering/Core/BindlessResourceManager.h"
#include "Rendering/Core/GPUCulling.h"
#include "Rendering/Core/GPUScene.h"
#include "Rendering/Materials/ShaderProfileDefines.h"
#include "Mathematics/MatrixOps.h"
#include "Mathematics/Vector4.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <limits>

#include <glm/gtc/matrix_access.hpp>

namespace GameEngine
{
namespace Engine::Renderer
{
using namespace ::GameEngine::Rendering;

using Mathematics::AABB;
using Mathematics::Matrix4x4;
using Mathematics::Vector3;
using Mathematics::Vector4;

// Pull light position back by this factor (in units of bounding radius)
// so that shadow casters between the light and the cascade sphere are
// not clipped by the near plane.
static constexpr float kShadowFrustumExtension = 1.5f;

// Each cascade extends its depth slice by this fraction past both ends so
// adjacent cascades overlap (both hold valid shadow data in the read-side blend
// zone). Cascade 0 keeps the same overlap ahead of its start that the last
// cascade keeps past the measured far bound.
static constexpr float kCascadeOverlapFraction = 0.2f;

// The receivers' cascade blend band as a fraction of the cascade's own depth
// range, mirrored from kCascadeBlendFraction in shadow_cascade_blend.glsl: a
// fragment this close before a split also samples the next cascade, so that
// cascade must hold it. Bounded by the overlap above, which is a fraction of
// the NEXT cascade's range: a band reaching past what the next cascade's box
// holds reads lit there (CascadeReceiverFit.BoxesHoldTheAirInFrontOfTheReceivers
// fails from 0.2 at the shallow orthographic strategy camera). No world-unit
// floor: a band longer than the cascade makes the finer cascade never win.
static constexpr float kCascadeBlendFraction = 0.175f;

// The caster cull runs on the WORLD-space fit (CascadeFrameData::LightVP),
// whose translation carries ULP(|eye|) once the render origin is active, and
// the GPU cull moves its planes to the camera-relative origin in fp32 again.
// The footprint is widened by this many ULPs of the origin's magnitude: zero
// inside the activation radius, a few metres at Earth radius.
static constexpr int kCullPrecisionUlps = 4;

// How far a shadow lookup reaches around its receiver, in texels of the
// cascade, beyond the penumbra ceiling ReceiverMarginWorld already holds in
// world units: the blocker search's widest disk, 64 taps
// (kPcssSearchTexelsPerRootTap * sqrt(64) = 32 texels in shadow_sampling.glsl),
// plus the min/max pyramid query's 1.5-texel margin and a texel of rounding.
// The search is bounded by the kernel cap too, so this is an upper bound.
// A box fitted to measured receivers holds this much around them.
static constexpr float kReceiverFilterReachTexels = 35.0f;
// World units around a measured receiver on top of the project's normal offset
// and penumbra ceiling: a receiver that moved in the few frames the readback
// takes (a walking unit covers centimetres) and the gap between the reduce's
// one-in-four samples.
static constexpr float kReceiverMotionAllowanceWorld = 0.5f;

// ── Cascade ortho-extent snap (rotation stability) ──
// Cascade 0 uses tighter guard/snap bands to spend more of its fixed 2048²
// map on nearby receivers. Far cascades retain the wider stability budget.
// One lookup feeds both the refit and frozen-fit reuse paths so their coverage
// and shrink decisions cannot drift apart.
struct CascadeFitTuning
{
    float ExtentSnapBandFraction;
    float FreezeGuardBandFraction;
};

constexpr CascadeFitTuning GetCascadeFitTuning(uint32_t cascadeIndex)
{
    return cascadeIndex == 0
        ? CascadeFitTuning{1.0f / 16.0f, 1.0f / 16.0f}
        : CascadeFitTuning{1.0f / 8.0f, 1.0f / 8.0f};
}
// Shrink deadband: a raw half-extent up to this factor below the previously
// snapped value reuses the previous bucket, so SDSM's slow contraction can't
// oscillate across a band edge frame after frame.
static constexpr float kExtentShrinkHysteresis = 0.85f;

// ── Fit-freeze budgets (GE_SHADOW_FIT_FREEZE) ──
// Guard band applied on every refit: the raw light-space half-extent (and the
// far depth bound) are inflated by this fraction before snapping, so the
// frozen ortho box has at least this much slice travel budget per side before
// the coverage test forces a refit. Costs the same fraction in worldPerTexel
// (measured A/B against the per-frame refit baseline); buys tens of frames of
// byte-stable fit under typical orbit/fly speeds.
// Caster footprint drift allowance, as a fraction of the cascade half-extent
// (= half the NDC range). While frozen, the fresh footprint may drift this far
// outside the capture-time footprint; the cull adds the SAME fraction
// (converted to world units) to its slack, so every caster the drifted
// footprint can receive shadows from is present in the retained layer.
static constexpr float kFreezeFootprintDriftFraction = 0.10f;
// LOD drift bound: refit when the camera has travelled further than this
// fraction of the cascade's far slice distance (floored for near cascades)
// from the frozen LOD camera. Bounds the silhouette-LOD staleness of retained
// layers to a distance error that shadow LOD selection cannot resolve anyway.
static constexpr float kFreezeLodDriftFraction = 0.05f;
static constexpr float kFreezeLodDriftMinWorld = 2.0f;

namespace
{

// GE_SHADOW_CASTER_REDUCTION=0 forces the shadow caster culling planes back to
// the untightened baseline (the A/B lane and kill switch). Any other value (or
// unset) keeps the caster footprint tightening. Read once at process start
// so the branch is stable for the process lifetime; the baseline lane stays
// in-tree, deleted only after the flip soaks.
bool IsCasterReductionEnabled()
{
    static const bool s_Enabled = []()
    {
        const char* env = std::getenv("GE_SHADOW_CASTER_REDUCTION");
        return !env || std::strcmp(env, "0") != 0;
    }();
    return s_Enabled;
}

// Extract the 8 corners of a frustum defined by the inverse view-projection matrix.
// NDC corners in LH [1,0] reverse-Z depth: z=1 near, z=0 far.
void ExtractFrustumCornersWS(const Matrix4x4& invViewProj, Vector3 outCorners[8])
{
    static constexpr float ndcCorners[8][4] = {
        {-1, -1, 1, 1}, { 1, -1, 1, 1}, { 1,  1, 1, 1}, {-1,  1, 1, 1}, // near (reverse-Z: z=1)
        {-1, -1, 0, 1}, { 1, -1, 0, 1}, { 1,  1, 0, 1}, {-1,  1, 0, 1}, // far  (reverse-Z: z=0)
    };

    for (int i = 0; i < 8; ++i)
    {
        Vector4 ndc{ndcCorners[i][0], ndcCorners[i][1], ndcCorners[i][2], ndcCorners[i][3]};
        Vector4 ws = invViewProj.Transform(ndc);
        float invW = 1.0f / ws.w;
        outCorners[i] = Vector3{ws.x * invW, ws.y * invW, ws.z * invW};
    }
}

// Light view matrix from a rotation-only look-at plus an eye position, with the
// translation column accumulated in double and rounded to fp32 once on store.
// Never MakeLookAtLH(eye, eye + ld) for this: that reconstructs the forward
// vector as fl(eye + ld) − eye, quantizing the light DIRECTION at ULP(|eye|)
// (~0.2° of shadow-camera rotation wobble at |eye| 5e4, garbage at Earth
// radius). The rotation comes in already built from the light direction alone,
// so it never touches |eye|.
Matrix4x4 MakeLightViewAtEye(const Matrix4x4& rotOnly, double eyeX, double eyeY, double eyeZ)
{
    Matrix4x4 view = rotOnly;
    float* v = view.Data();
    v[12] = static_cast<float>(-(static_cast<double>(v[0]) * eyeX +
                                 static_cast<double>(v[4]) * eyeY +
                                 static_cast<double>(v[8]) * eyeZ));
    v[13] = static_cast<float>(-(static_cast<double>(v[1]) * eyeX +
                                 static_cast<double>(v[5]) * eyeY +
                                 static_cast<double>(v[9]) * eyeZ));
    v[14] = static_cast<float>(-(static_cast<double>(v[2]) * eyeX +
                                 static_cast<double>(v[6]) * eyeY +
                                 static_cast<double>(v[10]) * eyeZ));
    return view;
}

// Snap a light VP's clip-space translation to the shadow texel grid. The grid is
// anchored to whatever frame the VP's eye was expressed in, so a static receiver
// keeps sampling the same sub-texel position while the camera translates within
// that frame — the invariant that makes a fit step invisible rather than swim.
void SnapClipTranslationToTexels(Matrix4x4& lightVP, uint32_t resolution)
{
    // A power of two, so multiplying the rounded quotient back is exact in fp32.
    const float clipTexelSize = 2.0f / static_cast<float>(resolution);
    float* data = lightVP.Data();
    data[12] = std::round(data[12] / clipTexelSize) * clipTexelSize;
    data[13] = std::round(data[13] / clipTexelSize) * clipTexelSize;
}

// Up vector of the cascade light basis: world up, or world forward when the
// light is within ~8 degrees of vertical (a look-at needs a non-parallel up).
Vector3 CascadeLightUp(const Vector3& lightDirection)
{
    const Vector3 up{0.0f, 1.0f, 0.0f};
    return std::abs(Vector3::Dot(lightDirection, up)) > 0.99f ? Vector3{0.0f, 0.0f, 1.0f} : up;
}

// Light-NDC XY rectangle {xMin, yMin, xMax, yMax} of a light-space box under
// a cascade fit centred at (centerXLS, centerYLS) with `halfExtent`: exact up
// to the half-texel clip-translation snap the fit applies afterwards.
void LightSpaceBoxToNdc(const AABB& box, float centerXLS, float centerYLS, float halfExtent,
                        float outRect[4])
{
    const float invHalf = 1.0f / std::max(halfExtent, 1e-6f);
    outRect[0] = (box.min.x - centerXLS) * invHalf;
    outRect[1] = (box.min.y - centerYLS) * invHalf;
    outRect[2] = (box.max.x - centerXLS) * invHalf;
    outRect[3] = (box.max.y - centerYLS) * invHalf;
}

} // namespace

namespace
{
Rendering::BufferDesc SdsmSlotDesc()
{
    // Storage: the depth-reduce dispatch writes the slot directly. The memory
    // class is Readback because the CPU maps and copies the slot out once per
    // view per frame; the class never constrained vkUsage — that is built from
    // BufferDesc::usage — so Storage and Readback compose on the native
    // backends. On WebGPU they cannot, and the ring splits the slot into a
    // written buffer plus a mappable one; that is invisible here (see
    // RGReadbackRing::Init and the reduce pass's ResolveSdsmSlotRG).
    Rendering::BufferDesc bd{};
    bd.size = ShadowReceiverMeasurement::kResultBytes;
    bd.usage = static_cast<uint32_t>(Rendering::BufferUsage::Storage)
             | static_cast<uint32_t>(Rendering::BufferUsage::TransferDst);
    bd.memoryUsage = Rendering::BufferMemoryUsage::Readback;
    bd.flags = Rendering::BufferCreateFlags::PersistentlyMapped;
    return bd;
}
} // namespace

ShadowMapRenderFeature::ShadowMapRenderFeature()
    : m_SdsmRingRG(SdsmSlotDesc(), "SDSM_DepthBounds.RenderGraph.View")
{
    // Motion round-robin: the built-in default (cap 2 / maxAge 2, the measured
    // lane from the 2026-07 design gates) applies unless the environment
    // overrides it (runtime setters can still override; the env is the
    // launch-time A/B lane and kill switch). Cap 0 = off.
    if (const char* cap = std::getenv("GE_SHADOW_MOTION_CAP"))
        m_MotionCap = static_cast<uint32_t>(std::strtoul(cap, nullptr, 10));
    if (const char* age = std::getenv("GE_SHADOW_MOTION_MAX_AGE"))
        SetMotionMaxAge(static_cast<uint32_t>(std::strtoul(age, nullptr, 10)));
    // Fit freeze: default ON; GE_SHADOW_FIT_FREEZE=0 forces the per-frame
    // refit baseline (the A/B lane and kill switch).
    if (const char* freeze = std::getenv("GE_SHADOW_FIT_FREEZE"))
        m_FitFreezeEnabled = std::strcmp(freeze, "0") != 0;
}

ShadowMapRenderFeature::~ShadowMapRenderFeature()
{
    if (m_Device)
    {
        if (m_ShadowSampler.IsValid())
            m_Device->DestroySampler(m_ShadowSampler);
        if (m_MsmSampler.IsValid())
            m_Device->DestroySampler(m_MsmSampler);
        for (auto& [viewId, tex] : m_MsmMomentsByView)
        {
            if (tex.IsValid())
                m_Device->DestroyTexture(tex);
        }
        if (m_MsmMomentsStub.IsValid())
            m_Device->DestroyTexture(m_MsmMomentsStub);
        m_SdsmRingRG.Destroy(m_Device);
        for (auto& [viewId, adopted] : m_ShadowMapByView)
        {
            // Pool-owned physicals (RenderGraph adopt) are destroyed by the pool.
            if (adopted.Physical.IsValid() &&
                m_PoolOwnedShadowMapViews.find(viewId) == m_PoolOwnedShadowMapViews.end())
                m_Device->DestroyTexture(adopted.Physical);
        }
    }
}

bool ShadowMapRenderFeature::Initialize(Rendering::IDevice* device, const CascadedShadowConfig& config)
{
    if (m_Initialized)
        return true;

    m_Device = device;
    m_Config = config;
    if (m_ProjectResolutionOverride.has_value())
        m_Config.Resolution = *m_ProjectResolutionOverride;
    m_Config.NumCascades = std::min(config.NumCascades, kMaxShadowCascades);

    // Create depth comparison sampler for hardware PCF (shared across views).
    m_ShadowSampler = device->CreateSampler(Rendering::SamplerDesc::ShadowComparePCF("ShadowCascadePCF"));

    m_Initialized = m_ShadowSampler.IsValid();
    return m_Initialized;
}

void ShadowMapRenderFeature::SetResolution(uint32_t resolution)
{
    resolution = std::max(1u, resolution);
    if (m_Config.Resolution == resolution)
        return;

    m_Config.Resolution = resolution;
    // Frozen fits carry the resolution used for their texel snap. The reuse
    // gate would reject them individually, but clearing both fit histories
    // makes the resolution transition atomic for all views and cascades.
    m_FrozenFits.clear();
    m_PrevSnappedHalfExtent.clear();
}

void ShadowMapRenderFeature::SetProjectResolutionOverride(uint32_t resolution)
{
    static constexpr uint32_t kTiers[] = {1024u, 2048u, 4096u, 8192u};
    uint32_t snapped = kTiers[0];
    uint32_t bestDistance = resolution > snapped ? resolution - snapped : snapped - resolution;
    for (uint32_t tier : kTiers)
    {
        const uint32_t distance = resolution > tier ? resolution - tier : tier - resolution;
        if (distance < bestDistance)
        {
            snapped = tier;
            bestDistance = distance;
        }
    }

    m_ProjectResolutionOverride = snapped;
    SetResolution(snapped);
}

void ShadowMapRenderFeature::OnDeviceRebuilt(Rendering::IDevice* device)
{
    if (!device)
        return;
    m_Device = device;

    // The rebuild teardown freed every shadow GPU resource, but the cached handles
    // still read IsValid(). Drop the per-view resources WITHOUT destroying them
    // (pool-owned shadow maps are RG-freed; the rest are already dead) so the next
    // Declare re-allocates them, and drop the stale bindless refs (the bindless set
    // was recreated by slice 3a). None of these clears touches the device.
    m_ShadowMapByView.clear();
    m_PoolOwnedShadowMapViews.clear();
    m_MsmMomentsByView.clear();
    m_MsmMomentsDimsByView.clear();
    m_CachedFrameData.clear();
    // The RG SDSM readback ring's slots are persistently-mapped buffers the
    // rebuild freed; their cached mapped pointers dangle (same UAF class as the
    // exposure readback rings). Drop the rings so AcquireSdsmSlotRG lazily
    // re-creates + re-maps them; buffer destroys inside are generational
    // no-ops on the dead handles.
    m_SdsmRingRG.Destroy(device);
    m_PcssBindlessByView.clear();
    // Same for the pyramid's slots — the indices name descriptors in a bindless
    // set that no longer exists, and the pooled physical they viewed died with
    // the device. The pyramid's own device state (pipeline, reflected layout,
    // sampler) is dropped alongside, or its one-shot load would never rebuild
    // them.
    m_PyramidBindlessByView.clear();
    m_MinMaxPyramid.OnDeviceRebuilt();
    m_CascadeShadowCache.Reset();
    // Retained cascade layers died with the device; the motion round-robin
    // must not defer against (or upload fits for) content that no longer
    // exists.
    m_CascadeContentFit.clear();
    m_TintContentFit.clear();
    m_MotionPlanByView.clear();
    m_MotionScheduler.Reset();
    // Frozen fits are inputs, not content — but a device rebuild is a hard
    // reset everywhere else, and one refit per cascade is the cheapest way to
    // guarantee no pre-rebuild snapshot survives.
    m_FrozenFits.clear();

    // Lazily-created singletons (their getters recreate on !IsValid).
    m_MsmSampler     = {};
    m_MsmMomentsStub = {};

    // The comparison sampler is created eagerly in Initialize (its getter is not
    // lazy), so recreate it now over the dead handle.
    m_ShadowSampler = device->CreateSampler(Rendering::SamplerDesc::ShadowComparePCF("ShadowCascadePCF"));
}

Rendering::BufferHandle ShadowMapRenderFeature::AcquireSdsmSlotRG(
    const Rendering::RenderGraph::RGFrame& frame, Rendering::ViewId viewId,
    const ShadowReceiverMeasurement::Context& context)
{
    return m_SdsmRingRG.BeginWrite(m_Device, frame, viewId, context);
}

void ShadowMapRenderFeature::ResolveSdsmSlotRG(Rendering::ViewId viewId,
                                               Rendering::CommandList* cl,
                                               Rendering::BufferHandle slot)
{
    m_SdsmRingRG.RecordResolve(viewId, cl, slot);
}

void ShadowMapRenderFeature::OnFrameSubmittedRG(Rendering::RenderGraph::RGFrame& frame,
                                                     const Rendering::IDevice::GpuSyncToken& token)
{
    m_SdsmRingRG.OnFrameSubmitted(frame, token);
}

void ShadowMapRenderFeature::OnFrameStreamRetiredRG(Rendering::RenderGraph::RGFrame& frame)
{
    m_SdsmRingRG.OnFrameStreamRetired(frame);
}

bool ShadowMapRenderFeature::TryResolveSdsmRG(Rendering::ViewId viewId, ShadowReceiverReadback& out)
{
    static_assert(offsetof(ShadowReceiverReadback, MaxWords) ==
                      ShadowReceiverMeasurement::kWordsPerSide * sizeof(uint32_t),
                  "the two result sides are read back as one contiguous block");
    return m_SdsmRingRG.ReadNewestInto(m_Device, viewId, out.MinWords,
                                       ShadowReceiverMeasurement::kResultBytes, &out.Measured);
}

const ShadowMapRenderFeature::SDSMBounds& ShadowMapRenderFeature::GetSDSMBounds(Rendering::ViewId viewId) const
{
    static const SDSMBounds kInvalidBounds{};
    auto it = m_SDSMBoundsByView.find(viewId);
    return it != m_SDSMBoundsByView.end() ? it->second : kInvalidBounds;
}

ShadowMapRenderFeature::SDSMBounds ShadowMapRenderFeature::ResolveSDSMBounds(
    float minNdc, float maxNdc, float nearPlane, float farPlane, bool orthographic, bool stabilize)
{
    if (!std::isfinite(minNdc) || !std::isfinite(maxNdc) ||
        !std::isfinite(nearPlane) || !std::isfinite(farPlane) ||
        minNdc < 0.0f || maxNdc > 1.0f || maxNdc <= minNdc ||
        nearPlane <= 0.0f || farPlane <= nearPlane)
        return {};

    SDSMBounds bounds;
    bounds.nearDepth = LinearizeReverseZDepthLH_ZO(maxNdc, nearPlane, farPlane, orthographic);
    bounds.farDepth = LinearizeReverseZDepthLH_ZO(minNdc, nearPlane, farPlane, orthographic);
    if (stabilize)
    {
        // A fixed NDC quantum is unbounded in perspective distance: rounding
        // a far sample below 1/1024 to zero expands a nearby fit to the camera
        // far plane. Quantize linear distance instead. Power-of-two steps
        // preserve ten fractional bits within each distance octave, bounding
        // the padding to 1/1024 of that distance without temporal convergence.
        const auto quantum = [](float depth) { return std::ldexp(1.0f, std::ilogb(depth) - 10); };
        const float nearQuantum = quantum(bounds.nearDepth);
        const float farQuantum = quantum(bounds.farDepth);
        bounds.nearDepth = std::max(nearPlane, std::floor(bounds.nearDepth / nearQuantum) * nearQuantum);
        bounds.farDepth = std::min(farPlane, std::ceil(bounds.farDepth / farQuantum) * farQuantum);
    }
    bounds.valid = bounds.farDepth > bounds.nearDepth;
    return bounds;
}

Matrix4x4 ShadowMapRenderFeature::CascadeLightRotation(const Vector3& lightDirection)
{
    return Mathematics::MakeLookAtLH(Vector3{0.0f, 0.0f, 0.0f}, lightDirection,
                                     CascadeLightUp(lightDirection));
}

const ShadowReceiverMeasurement& ShadowMapRenderFeature::GetShadowReceivers(Rendering::ViewId viewId) const
{
    static const ShadowReceiverMeasurement kNone{};
    const auto it = m_ShadowReceiversByView.find(viewId);
    return it != m_ShadowReceiversByView.end() ? it->second : kNone;
}

void ShadowMapRenderFeature::UpdateShadowReceivers(Rendering::ViewId viewId,
                                                   const ShadowReceiverMeasurement& measured)
{
    m_ShadowReceiversByView[viewId] = measured;
}

void ShadowMapRenderFeature::UpdateSDSMBounds(Rendering::ViewId viewId, float newNear, float newFar)
{
    // The readback is already asynchronous and conservatively quantized.
    // EMA contraction adds visible refitting after a camera move has stopped.
    // Adopt each valid result directly, independently of contact-shadow state.
    SDSMBounds& bounds = m_SDSMBoundsByView[viewId];
    bounds.nearDepth = newNear;
    bounds.farDepth = newFar;
    bounds.valid = true;
}

float ShadowMapRenderFeature::CascadeTexelSizeFrom(float cascade0Texel, float ratio,
                                                   uint32_t cascadeIndex)
{
    return std::max(cascade0Texel, 1e-6f) *
           std::pow(std::max(ratio, 1.0f), static_cast<float>(cascadeIndex));
}

float ShadowMapRenderFeature::CascadeTexelSize(const CascadedShadowConfig& config,
                                               uint32_t cascadeIndex)
{
    return CascadeTexelSizeFrom(config.Cascade0TexelSize, config.CascadeTexelRatio, cascadeIndex);
}

float ShadowMapRenderFeature::CascadeChainReach(const CascadedShadowConfig& config,
                                                float cascade0Texel, float nearPlane,
                                                float tanHalfY, float aspect, float maxFar)
{
    float sliceNear = nearPlane;
    for (uint32_t i = 0; i < config.NumCascades && i < kMaxShadowCascades; ++i)
    {
        const float texel = CascadeTexelSizeFrom(cascade0Texel, config.CascadeTexelRatio, i);
        const float halfExtent = 0.5f * static_cast<float>(config.Resolution) * texel;
        sliceNear = SolveSliceFarForRadius(sliceNear, halfExtent, tanHalfY, aspect, maxFar);
    }
    return sliceNear;
}

float ShadowMapRenderFeature::SolveCascade0TexelForDistance(const CascadedShadowConfig& config,
                                                            float nearPlane, float targetDistance,
                                                            float tanHalfY, float aspect)
{
    if (targetDistance <= nearPlane)
        return 1e-4f;

    // Closed form, not a search. Past a few metres a slice's bounding sphere is
    // dominated by its far ring — SliceBoundingSphereRadius takes the
    // f * sqrt(k2) branch — so the LAST cascade's reach is
    // halfExtent_last / sqrt(k2) almost independently of where it starts. Invert
    // that:
    //
    //   halfExtent_last = 0.5 * Resolution * texel0 * ratio^(N-1)
    //   reach           = halfExtent_last / sqrt(k2)
    //   =>  texel0      = 2 * reach * sqrt(k2) / (Resolution * ratio^(N-1))
    //
    // A bisection was tried first and is the wrong tool here: it needs a bracket
    // wide enough for every resolution/ratio/distance combination, and when the
    // bracket is wrong it does not fail — it silently returns an endpoint, which
    // reads as "AUTO produces absurdly tiny cascades" rather than as an error.
    const float k2 = tanHalfY * tanHalfY * (1.0f + aspect * aspect);
    const float lastRatio =
        std::pow(std::max(config.CascadeTexelRatio, 1.0f),
                 static_cast<float>(std::max<uint32_t>(config.NumCascades, 1u) - 1u));
    const float denom = static_cast<float>(std::max(config.Resolution, 1u)) * lastRatio;
    const float texel0 = 2.0f * targetDistance * std::sqrt(std::max(k2, 1e-8f)) / denom;
    return std::clamp(texel0, 1e-5f, 10.0f);
}

float ShadowMapRenderFeature::CascadeHalfExtentForTexel(const CascadedShadowConfig& config,
                                                        uint32_t cascadeIndex)
{
    return 0.5f * static_cast<float>(config.Resolution) * CascadeTexelSize(config, cascadeIndex);
}

float ShadowMapRenderFeature::SliceBoundingSphereRadius(float nearDist, float farDist,
                                                        float tanHalfY, float aspect)
{
    // Corner offset factor: a slice corner at distance d sits at
    // (d*aspect*tanHalfY, d*tanHalfY, d), so its squared lateral offset is
    // d^2 * k2 with k2 = tanHalfY^2 * (1 + aspect^2).
    const float k2 = tanHalfY * tanHalfY * (1.0f + aspect * aspect);
    const float n = std::max(nearDist, 0.0f);
    const float f = std::max(farDist, n + 1e-4f);

    // Equidistant centre from the near and far corner rings sits at
    // c = (n+f)(k2+1)/2. When that lands beyond the far plane the far ring alone
    // bounds the slice, and the tightest sphere is centred ON the far plane.
    if (k2 >= (f - n) / (f + n))
        return f * std::sqrt(k2);

    const float c = 0.5f * (n + f) * (k2 + 1.0f);
    return std::sqrt(f * f * k2 + (f - c) * (f - c));
}

float ShadowMapRenderFeature::SolveSliceFarForRadius(float nearDist, float radius, float tanHalfY,
                                                     float aspect, float maxFar)
{
    if (radius <= 0.0f || maxFar <= nearDist)
        return std::max(maxFar, nearDist);

    // A slice that already exceeds the radius at maxFar simply reaches maxFar.
    if (SliceBoundingSphereRadius(nearDist, maxFar, tanHalfY, aspect) <= radius)
        return maxFar;

    float lo = nearDist;
    float hi = maxFar;
    // 40 halvings resolve any physically meaningful range to well under a
    // millimetre; the loop is bounded rather than tolerance-driven so it cannot
    // spin on a degenerate configuration.
    for (int i = 0; i < 40; ++i)
    {
        const float mid = 0.5f * (lo + hi);
        if (SliceBoundingSphereRadius(nearDist, mid, tanHalfY, aspect) <= radius)
            lo = mid;
        else
            hi = mid;
    }
    return lo;
}

void ShadowMapRenderFeature::ComputeSplits(float nearPlane, float farPlane, float tanHalfY,
                                                  float aspect, float worldTexelBase,
                                                  float outSplits[kMaxShadowCascades],
                                                  const SDSMBounds* sdsmBounds) const
{
    float effectiveNear = nearPlane;
    float effectiveFar = std::min(farPlane, m_Config.MaxShadowDistance);

    // WorldTexel derives the splits instead of authoring them: each cascade's
    // coverage is fixed by its authored texel size, so its reach is simply where
    // that box stops containing the frustum slice. SplitLambda has nothing to do
    // here, and SDSM must not participate — letting the measured depth bounds
    // move the splits would re-introduce exactly the per-frame drift an authored
    // texel size exists to remove.
    if (m_Config.Projection == ShadowProjection::WorldTexel)
    {
        float sliceNear = effectiveNear;
        for (uint32_t i = 0; i < m_Config.NumCascades && i < kMaxShadowCascades; ++i)
        {
            const float texel =
                CascadeTexelSizeFrom(worldTexelBase, m_Config.CascadeTexelRatio, i);
            const float halfExtent = 0.5f * static_cast<float>(m_Config.Resolution) * texel;
            const float far = SolveSliceFarForRadius(sliceNear, halfExtent, tanHalfY, aspect,
                                                     effectiveFar);
            // Strictly increasing even if a ladder rung cannot advance, so the
            // cascade select in the shader never sees a zero-width band.
            outSplits[i] = std::max(far, sliceNear + 1e-3f);
            sliceNear = outSplits[i];
        }
        return;
    }

    // Tighten range to SDSM-measured depth bounds if available.
    //
    // Close projection only. SDSM re-measures the visible depth range every
    // frame, so under Stable this is the single largest source of shimmer: the
    // splits chase the depth buffer, each slice resizes with them, and the ortho
    // extent — hence worldPerTexel — steps on almost every frame of camera
    // motion. Measured on a 4.8 m dolly: cascade 0's half-extent walked
    // 8 -> 7 -> 6 -> 5 -> 3 -> 22 -> 24, every step re-quantizing the whole map.
    // Stable pays for fixed splits with a looser fit; that is the trade.
    if (sdsmBounds && sdsmBounds->valid && m_Config.Projection == ShadowProjection::Close)
    {
        effectiveNear = std::max(nearPlane, sdsmBounds->nearDepth);
        effectiveFar = std::min(effectiveFar, sdsmBounds->farDepth);
        // Ensure valid range.
        if (effectiveFar <= effectiveNear)
            effectiveFar = effectiveNear + 1.0f;
    }

    const float lambda = m_Config.SplitLambda;
    const uint32_t n = m_Config.NumCascades;

    for (uint32_t i = 0; i < n; ++i)
    {
        float p = static_cast<float>(i + 1) / static_cast<float>(n);
        float logSplit = effectiveNear * std::pow(effectiveFar / effectiveNear, p);
        float uniformSplit = effectiveNear + (effectiveFar - effectiveNear) * p;
        outSplits[i] = lambda * logSplit + (1.0f - lambda) * uniformSplit;
    }
}

float ShadowMapRenderFeature::Cascade0SliceStart(const ShadowReceiverMeasurement* receivers,
                                                 float nearPlane, float split0,
                                                 const ShadowReceiverMeasurement::Context& current) const
{
    // Close only (UsableReceivers): the splits start at the measured near
    // bound there too.
    if (!UsableReceivers(receivers, current))
        return nearPlane;
    // The nearest receiver, pulled in by however far the camera has moved since
    // it was measured. The slice then extends ahead of it by the same overlap
    // fraction the last cascade keeps past the measured far bound.
    const float motion = ShadowReceiverMotionBound(*receivers, receivers->NearDepth, current);
    return std::clamp(receivers->NearDepth - motion, nearPlane, std::max(split0, nearPlane));
}

CascadeFrameData ShadowMapRenderFeature::ComputeCascades(
    const Rendering::CameraData& camera, float nearPlane, float farPlane,
    const Vector3& lightDir, const SDSMBounds* sdsmBounds,
    Rendering::ViewId viewId, const SceneBoundsRel* sceneBounds,
    const ShadowReceiverMeasurement* receivers)
{
    CascadeFrameData result{};
    result.NumCascades = m_Config.NumCascades;
    result.DepthBias = m_Config.DepthBias;
    result.NormalBias = m_Config.NormalBias;
    result.MaxShadowDistance = m_Config.MaxShadowDistance;

    // Camera shape for the WorldTexel split solve, read out of the projection so
    // it cannot disagree with what the frustum corners below are extracted from:
    // proj[1][1] = 1/tan(fovY/2) and proj[0][0] = 1/(aspect*tan(fovY/2)).
    // Guarded because an orthographic or degenerate projection has no fovY; the
    // fallbacks only ever feed the WorldTexel branch, which such a camera does
    // not meaningfully use.
    const float projYY = camera.proj[5];
    const float projXX = camera.proj[0];
    const float tanHalfY = (std::abs(projYY) > 1e-6f) ? (1.0f / std::abs(projYY)) : 1.0f;
    const float aspect = (std::abs(projXX) > 1e-6f) ? std::abs(projYY / projXX) : 1.0f;

    // Resolve AUTO here, per CALL, not into a member: several views run this
    // each frame with very different far planes, so a feature-level cache is
    // whichever view ran last.
    const float worldTexelBase =
        (m_Config.Cascade0TexelSize > 0.0f)
            ? m_Config.Cascade0TexelSize
            : SolveCascade0TexelForDistance(m_Config, nearPlane,
                                            std::min(farPlane, m_Config.MaxShadowDistance),
                                            tanHalfY, aspect);

    ComputeSplits(nearPlane, farPlane, tanHalfY, aspect, worldTexelBase, result.SplitDistances,
                  sdsmBounds);

    // Camera-relative rendering: the whole cascade fit runs in the
    // render-origin-relative frame, against the SAME origin the world pass uses.
    // Origin and rebased camera VP are derived HERE from the camera position (the
    // identical pure function ResolveCameraData applies on-read) — NOT read from
    // camera.renderOriginSector / camera.viewProjRel: callers that fetch the
    // camera through FindCameraData get the raw store, whose relative fields are
    // only filled on-read, so trusting them left this path permanently in the
    // sector-0 branch — LightVPRel stayed world-space while receivers sample
    // with render-origin-relative positions, and every cascade lookup missed
    // (shadows silently fully lit) the moment the origin activated.
    //
    // Fitting in the relative frame is a precision requirement, not tidiness.
    // The frustum corners come out of an fp32 inverse view-projection, so at
    // world magnitude each one carries ~ULP(|eye|) — 0.8 m at Earth radius,
    // which is 20+ texels of cascade 0. Fitting to corners that wrong both
    // mis-centres the ortho box and sizes it against the wrong geometry, and
    // since the box is the slice's own AABB rounded up to a 12.5% band, a pose
    // near a band edge has no margin to absorb it: the true slice then falls
    // outside the emitted box and receivers in that band sample past the
    // cascade (CascadeFitWorldMagnitude.EmittedFitContainsTrueSliceAtPlanetary-
    // Magnitude). In the relative frame the corners, the light-space AABB, the
    // centre texel snap and the shadow-camera eye all stay at local magnitude,
    // where fp32 resolves a shadow texel with room to spare.
    //
    // Origin (0,0,0) => ComputeRebasedView copies viewProj bit-for-bit, so every
    // value below — and LightVPRel — is byte-identical to the full-world path.
    // LightVP stays world-space for the world-space caster culling in
    // OnScheduleCulling, which tolerates ULP(|eye|) (its slack is in metres).
    // ComputeRebasedView is the one place the engine rebases a camera; only its
    // view-projection output is wanted here (the fit never needs the view alone).
    float cameraViewRelUnusedHere[16];
    float cameraViewProjRel[16];
    int32 originSector[4];
    ComputeRebasedView(camera.view, camera.proj, camera.viewProj, camera.cameraPos[0],
                       camera.cameraPos[1], camera.cameraPos[2], cameraViewRelUnusedHere,
                       cameraViewProjRel, originSector);
    result.RenderOriginSector[0] = originSector[0];
    result.RenderOriginSector[1] = originSector[1];
    result.RenderOriginSector[2] = originSector[2];

    // Inverse of the origin-relative camera VP, for extracting frustum corners.
    Matrix4x4 viewProjRel;
    std::memcpy(viewProjRel.Data(), cameraViewProjRel, 16 * sizeof(float));
    const Matrix4x4 invViewProjRel = Mathematics::Inverse(viewProjRel);

    // Full frustum corners, render-origin-relative.
    Vector3 fullCornersRel[8];
    ExtractFrustumCornersWS(invViewProjRel, fullCornersRel);

    // For each cascade, interpolate near/far frustum slices and compute light VP.
    // fullCornersRel span [nearPlane, farPlane] in view-space depth, so the interpolation
    // factor must normalize against (farPlane - nearPlane), NOT (effectiveFar - nearPlane).
    //
    // Each cascade extends kCascadeOverlapFraction of its depth range past both
    // split boundaries (clamped to [0, 1]) so adjacent cascades overlap. This
    // ensures both cascades have valid shadow data at the blend zone,
    // eliminating the "double shadow" artifact from mismatched shadow shapes.
    const float frustumRange = farPlane - nearPlane;
    const bool freeze = m_FitFreezeEnabled;
    const Vector3 ldNorm = lightDir.Normalize();
    const Vector3 camPos{camera.cameraPos[0], camera.cameraPos[1], camera.cameraPos[2]};
    ShadowReceiverMeasurement::Context currentCamera{};
    SetShadowReceiverCamera(camera, IsOrthographicProjectionLH_ZO(camera.proj), currentCamera);
    const float cascade0Start =
        Cascade0SliceStart(receivers, nearPlane, result.SplitDistances[0], currentCamera);
    // The receivers each cascade's box narrows to. A measurement taken under
    // another light direction or render origin (the sun moves, the readback
    // lands frames later) is carried into this frame's light space.
    const ShadowReceiverMeasurement* measured = UsableReceivers(receivers, currentCamera);
    const Matrix4x4 lightRot = CascadeLightRotation(ldNorm);
    Matrix4x4 measuredToCurrent;
    const Matrix4x4* measuredToCurrentPtr =
        measured && MeasuredToCurrentLightSpace(*measured, lightRot, result.RenderOriginSector,
                                                measuredToCurrent)
            ? &measuredToCurrent
            : nullptr;
    // The eye, render-origin-relative: the sun glare looks the cascades up there.
    const Vector3 eyeRel{
        static_cast<float>(static_cast<double>(camera.cameraPos[0]) -
                           static_cast<double>(result.RenderOriginSector[0]) * kSectorSize),
        static_cast<float>(static_cast<double>(camera.cameraPos[1]) -
                           static_cast<double>(result.RenderOriginSector[1]) * kSectorSize),
        static_cast<float>(static_cast<double>(camera.cameraPos[2]) -
                           static_cast<double>(result.RenderOriginSector[2]) * kSectorSize)};
    for (uint32_t c = 0; c < m_Config.NumCascades; ++c)
    {
        float cascadeNear = ((c == 0 ? cascade0Start : result.SplitDistances[c - 1]) - nearPlane) /
                            frustumRange;
        float cascadeFar = (result.SplitDistances[c] - nearPlane) / frustumRange;

        // Extend the frustum slice by the overlap fraction in both directions.
        float sliceRange = cascadeFar - cascadeNear;
        float overlap = sliceRange * kCascadeOverlapFraction;
        cascadeNear = std::max(0.0f, cascadeNear - overlap);
        cascadeFar = std::min(1.0f, cascadeFar + overlap);

        // Interpolate frustum corners for this cascade's slice.
        Vector3 cascadeCornersRel[8];
        for (int i = 0; i < 4; ++i)
        {
            Vector3 nearPt = fullCornersRel[i];
            Vector3 farPt = fullCornersRel[i + 4];
            Vector3 delta = farPt - nearPt;
            cascadeCornersRel[i] = nearPt + delta * cascadeNear;
            cascadeCornersRel[i + 4] = nearPt + delta * cascadeFar;
        }

        // What the box must hold: the slice, clamped to the scene, narrowed to
        // the receivers that can sample this cascade when SDSM measured them,
        // the visible surfaces and the visible air in front of them alike.
        // Cascade 0 keeps its slice, which starts at the nearest measured
        // surface, and adds the measured air in front of it: it is the
        // smallest box, so the few frames a camera move widens the lagging
        // measurement are a large share of it, and a box narrowed to them
        // changes its texel size as the camera moves.
        CascadeFitBounds bounds = SliceFitBounds(lightRot, cascadeCornersRel, sceneBounds);
        if (c == 0 && measured)
        {
            Vector3 fromNearCornersRel[8];
            for (int i = 0; i < 4; ++i)
            {
                fromNearCornersRel[i] = fullCornersRel[i];
                fromNearCornersRel[i + 4] = cascadeCornersRel[i + 4];
            }
            AABB fromNear = SliceFitBounds(lightRot, fromNearCornersRel, sceneBounds).Box;
            fromNear.Expand(lightRot.TransformPoint(eyeRel));
            HoldAirInFrontOfCascade0(*measured, cascade0Start, currentCamera, measuredToCurrentPtr,
                                     fromNear, bounds);
        }
        const bool receiverFit =
            measured && c > 0 &&
            NarrowFitToReceivers(*measured, result, c, currentCamera, measuredToCurrentPtr, bounds);
        if (!receiverFit)
        {
            // A box that keeps its slice serves receivers up to its edges, and
            // their lookups read past them: its casters are culled to the slice
            // box widened by that reach, sized in texels of the extent the
            // projection fits (the slice's sphere under Stable, the authored
            // extent under WorldTexel).
            const float fitHalfExtent =
                m_Config.Projection == ShadowProjection::Stable ? bounds.SphereRadius
                : m_Config.Projection == ShadowProjection::WorldTexel
                    ? 0.5f * static_cast<float>(m_Config.Resolution) *
                          CascadeTexelSizeFrom(worldTexelBase, m_Config.CascadeTexelRatio, c)
                    : std::max(bounds.Box.max.x - bounds.Box.min.x,
                               bounds.Box.max.y - bounds.Box.min.y) * 0.5f;
            const float reach = ReceiverReachWorld(fitHalfExtent);
            bounds.CasterBox = bounds.Box;
            bounds.CasterBox.Inflate(Vector3{reach, reach, 0.0f});
        }
        // The casters are culled to what the box was fitted to hold. Cascade 0
        // keeps its full box: a false cull there is the most visible.
        const bool footprintApplies = IsCasterReductionEnabled() && c > 0;

        // Fit freeze: while the frozen record still covers this frame's bounds
        // (coverage box, footprint drift, LOD drift — see FrozenCascadeFit),
        // re-emit it byte-for-byte together with its frozen cull-content
        // inputs, so every downstream consumer (cull planes, LOD selection,
        // static-cache key) sees inputs that only step on an actual refit.
        FrozenCascadeFit* frozen = nullptr;
        if (freeze)
            frozen = &m_FrozenFits[viewId][c];

        const float sliceFarDist = result.SplitDistances[c];
        CascadeFit fit;
        if (frozen && frozen->Valid &&
            CanReuseFrozenCascadeFit(*frozen, bounds, ldNorm, camPos, c, m_Config.Resolution,
                                     result.RenderOriginSector, footprintApplies,
                                     m_Config.Projection))
        {
            fit = frozen->Fit;
        }
        else
        {
            const CascadeFitTuning tuning = GetCascadeFitTuning(c);
            fit = ComputeCascadeLightVP(bounds, lightDir, m_Config.Resolution, viewId, c,
                                        result.RenderOriginSector, worldTexelBase,
                                        freeze ? tuning.FreezeGuardBandFraction : 0.0f);
            if (frozen)
            {
                frozen->Fit = fit;
                frozen->LightDir = ldNorm;
                frozen->CameraPos = camPos;
                std::memcpy(frozen->CullCameraViewProj.Data(), camera.viewProj,
                            16 * sizeof(float));
                std::memcpy(frozen->LodCameraPos, camera.cameraPos, sizeof(frozen->LodCameraPos));
                frozen->LodProjScaleY = std::abs(camera.proj[5]);
                frozen->SliceFarDist = sliceFarDist;
                frozen->Sector[0] = result.RenderOriginSector[0];
                frozen->Sector[1] = result.RenderOriginSector[1];
                frozen->Sector[2] = result.RenderOriginSector[2];
                frozen->Resolution = m_Config.Resolution;
                // Capture-time footprint through the new fit: the rectangle the
                // cull tightens to while this record is reused.
                LightSpaceBoxToNdc(bounds.CasterBox, fit.CenterXLS, fit.CenterYLS,
                                   fit.OrthoHalfExtent, frozen->Footprint);
                frozen->Valid = true;
            }
        }
        result.LightVP[c] = fit.VP;
        result.LightVPRel[c] = fit.VPRel;
        result.OrthoHalfExtent[c] = fit.OrthoHalfExtent;
        result.DepthSpan[c] = fit.DepthSpan;

        // Cull-content snapshot: frozen inputs under the freeze, this frame's
        // live values otherwise (the cache key reads these either way).
        if (frozen)
        {
            result.CullCameraViewProj[c] = frozen->CullCameraViewProj;
            std::memcpy(result.CasterFootprint[c], frozen->Footprint, sizeof(frozen->Footprint));
            std::memcpy(result.LodCameraPos[c], frozen->LodCameraPos,
                        sizeof(result.LodCameraPos[c]));
            result.LodProjScaleY[c] = frozen->LodProjScaleY;
        }
        else
        {
            std::memcpy(result.CullCameraViewProj[c].Data(), camera.viewProj, 16 * sizeof(float));
            LightSpaceBoxToNdc(bounds.CasterBox, fit.CenterXLS, fit.CenterYLS, fit.OrthoHalfExtent,
                               result.CasterFootprint[c]);
            std::memcpy(result.LodCameraPos[c], camera.cameraPos, sizeof(result.LodCameraPos[c]));
            result.LodProjScaleY[c] = std::abs(camera.proj[5]);
        }
        result.CasterFootprintApplies[c] = footprintApplies;
    }

    return result;
}

bool ShadowMapRenderFeature::CanReuseFrozenCascadeFit(
    const FrozenCascadeFit& frozen, const CascadeFitBounds& fresh, const Vector3& lightDir, const Vector3& cameraPos, uint32_t cascadeIndex, uint32_t resolution,
    const int32 originSector[3], bool casterReduction, ShadowProjection projection)
{
    if (!frozen.Valid || frozen.Resolution != resolution)
        return false;
    // Exact matches: the frozen VP bakes the light rotation and the origin
    // rebase — either changing means the retained rasterization is for a
    // different projection, not a drifted one. The sector compare also keeps
    // the frame honest: the frozen fit's light-space internals below are
    // origin-relative, so they are only comparable against fresh bounds
    // rebased against the SAME origin.
    if (frozen.Sector[0] != originSector[0] || frozen.Sector[1] != originSector[1] ||
        frozen.Sector[2] != originSector[2])
        return false;
    if (std::memcmp(&frozen.LightDir, &lightDir, 3 * sizeof(float)) != 0)
        return false;

    // Receiver coverage: the fresh bounds must sit inside the frozen ortho box
    // and depth window — every fragment the CURRENT camera can select into this
    // cascade then projects inside the retained layer. Exact containment, no
    // allowance: the guard band added at refit IS the travel budget. Same
    // render-origin-relative light basis ComputeCascadeLightVP fitted in
    // (guaranteed by the sector compare above). The bounds are the ones
    // ComputeCascadeLightVP fits (clamped, narrowed to the measured receivers);
    // judged against anything else the frozen record would look stale every
    // frame and refit continuously.
    const Vector3 minLS = fresh.Box.min;
    const Vector3 maxLS = fresh.Box.max;
    const float he = frozen.Fit.OrthoHalfExtent;
    if (minLS.x < frozen.Fit.CenterXLS - he || maxLS.x > frozen.Fit.CenterXLS + he ||
        minLS.y < frozen.Fit.CenterYLS - he || maxLS.y > frozen.Fit.CenterYLS + he)
        return false;
    if (minLS.z < frozen.Fit.NearZLS || maxLS.z > frozen.Fit.FarZLS)
        return false;

    // Shrink refit: containment alone would let an SDSM-contracted slice
    // reuse the frozen box forever, pinning worldPerTexel at capture size.
    // Compute the snapped half-extent a fresh refit would produce (same
    // guard band, same bands) and refit once the frozen box is more than one
    // band oversize AND the raw extent sits below the refit hysteresis
    // deadband — the refit then provably lands at least one band smaller
    // (the hysteresis can't cancel it), and slow SDSM breathing inside a
    // band or the deadband can never oscillate.
    //
    // WorldTexel has no shrink refit to do: the extent is authored, so a refit
    // would land on exactly the value already frozen and the test can only ever
    // cost a pointless re-render. Skipping it is required, not an optimisation —
    // the fresh-extent arithmetic below reconstructs a FITTED extent, which this
    // mode never emits, so running it would compare the frozen box against a
    // quantity the fit never produced and refit every frame.
    if (projection != ShadowProjection::WorldTexel)
    {
        const CascadeFitTuning tuning = GetCascadeFitTuning(cascadeIndex);
        // MUST mirror ComputeCascadeLightVP's extent source for the SAME projection
        // mode, or the frozen box is measured against a quantity the fit never
        // emitted and the shrink refit fires every frame.
        const float freshRaw = projection == ShadowProjection::Stable
                                   ? fresh.SphereRadius
                                   : std::max(maxLS.x - minLS.x, maxLS.y - minLS.y) * 0.5f;
        const float freshHalf = freshRaw * (1.0f + tuning.FreezeGuardBandFraction);
        const float magnitude = std::pow(2.0f, std::ceil(std::log2(std::max(freshHalf, 1e-3f))));
        const float band = magnitude * tuning.ExtentSnapBandFraction;
        const float freshSnapped = std::ceil(freshHalf / band) * band;
        if (he > freshSnapped + band && freshHalf <= he * kExtentShrinkHysteresis)
            return false;
    }

    // Footprint drift: the cull tightens to the capture-time footprint plus
    // exactly kFreezeFootprintDriftFraction (in NDC = fraction of half-extent)
    // of extra slack, so the fresh footprint may drift that far past it before
    // casters could go missing.
    if (casterReduction)
    {
        float footprint[4];
        LightSpaceBoxToNdc(fresh.CasterBox, frozen.Fit.CenterXLS, frozen.Fit.CenterYLS, he, footprint);
        const float allow = kFreezeFootprintDriftFraction;
        if (footprint[0] < frozen.Footprint[0] - allow || footprint[1] < frozen.Footprint[1] - allow ||
            footprint[2] > frozen.Footprint[2] + allow || footprint[3] > frozen.Footprint[3] + allow)
            return false;
    }

    // LOD drift: GPU LOD selection reads the frozen camera; bound the
    // distance error retained content can carry.
    const float lodBound =
        std::max(kFreezeLodDriftFraction * frozen.SliceFarDist, kFreezeLodDriftMinWorld);
    const Vector3 drift = cameraPos - frozen.CameraPos;
    if (Vector3::Dot(drift, drift) > lodBound * lodBound)
        return false;

    return true;
}

ShadowMapRenderFeature::CascadeFitBounds ShadowMapRenderFeature::SliceFitBounds(
    const Matrix4x4& lightRot, const Vector3 sliceCornersRel[8], const SceneBoundsRel* sceneBounds)
{
    // Clamped BEFORE any extent or Z snapping: the fit's minimum Z is what the
    // near-plane back-extension is measured from, so a slice reaching past the
    // geometry would otherwise anchor the whole fit — and the back-extension
    // with it — to empty space, leaving the casters outside.
    CascadeFitBounds bounds{};
    ComputeClampedLightSpaceBounds(lightRot, sliceCornersRel, sceneBounds, bounds.Box.min,
                                   bounds.Box.max);
    ComputeClampedLightSpaceSphere(lightRot, sliceCornersRel, sceneBounds, bounds.SphereRadius,
                                   bounds.SphereCenterXLS, bounds.SphereCenterYLS);
    return bounds;
}

const ShadowReceiverMeasurement* ShadowMapRenderFeature::UsableReceivers(
    const ShadowReceiverMeasurement* receivers,
    const ShadowReceiverMeasurement::Context& current) const
{
    // Close only: Stable and WorldTexel keep fits that do not follow the depth
    // buffer, which is the point of choosing them. Under another window the
    // measurement says nothing about the receivers at the new edges; the fit
    // keeps the slices until a measurement through this window lands.
    return receivers && receivers->Valid && m_Config.Projection == ShadowProjection::Close &&
                   ShadowReceiverSameWindow(receivers->Measured, current)
               ? receivers
               : nullptr;
}

bool ShadowMapRenderFeature::MeasuredToCurrentLightSpace(const ShadowReceiverMeasurement& measured,
                                                        const Matrix4x4& lightRot,
                                                        const int32 originSector[3],
                                                        Matrix4x4& out)
{
    const ShadowReceiverMeasurement::Context& context = measured.Measured;
    const Matrix4x4 measuredRot = CascadeLightRotation(context.LightDirection);
    const bool sameLight = std::memcmp(measuredRot.Data(), lightRot.Data(), 16 * sizeof(float)) == 0;
    const bool sameOrigin = context.RenderOriginSector[0] == originSector[0] &&
                            context.RenderOriginSector[1] == originSector[1] &&
                            context.RenderOriginSector[2] == originSector[2];
    if (sameLight && sameOrigin)
        return false;
    // Measured light space -> positions relative to the measuring origin ->
    // relative to this frame's origin -> this frame's light space. The sector
    // delta is an exact integer count of sectors.
    const Vector3 originShift{
        static_cast<float>(context.RenderOriginSector[0] - originSector[0]) * kSectorSize,
        static_cast<float>(context.RenderOriginSector[1] - originSector[1]) * kSectorSize,
        static_cast<float>(context.RenderOriginSector[2] - originSector[2]) * kSectorSize};
    out = lightRot * Mathematics::MakeTranslation(originShift) * Mathematics::Transpose(measuredRot);
    return true;
}

float ShadowMapRenderFeature::ReceiverMarginWorld() const
{
    // The receiver offsets its lookup along its normal by at most the authored
    // normal bias (GE_ShadowNormalBiasWorld), and PCSS filters up to the
    // penumbra ceiling around it (0 = the shader's automatic ceiling).
    const float penumbraCeiling =
        m_PcssMaxPenumbra > 0.0f ? m_PcssMaxPenumbra : kPcssMaxPenumbraWorldAuto;
    return std::max(m_Config.NormalBias, 0.0f) + penumbraCeiling + kReceiverMotionAllowanceWorld;
}

float ShadowMapRenderFeature::ReceiverReachWorld(float halfExtent) const
{
    // The filter's reach is in texels of the box being fitted; it is sized from
    // the extent the box is fitted to, which the fit only rounds up.
    const float marginWorld = ReceiverMarginWorld();
    const float texel =
        2.0f * (halfExtent + marginWorld) / static_cast<float>(std::max(m_Config.Resolution, 1u));
    return marginWorld + kReceiverFilterReachTexels * texel;
}

float ShadowMapRenderFeature::CascadeBlendBand(const CascadeFrameData& frame, uint32_t cascadeIndex)
{
    if (cascadeIndex + 1 >= frame.NumCascades)
        return 0.0f;
    const float start = cascadeIndex == 0 ? 0.0f : frame.SplitDistances[cascadeIndex - 1];
    return (frame.SplitDistances[cascadeIndex] - start) * kCascadeBlendFraction;
}

bool ShadowMapRenderFeature::NarrowFitToReceivers(const ShadowReceiverMeasurement& measured,
                                                  const CascadeFrameData& frame,
                                                  uint32_t cascadeIndex,
                                                  const ShadowReceiverMeasurement::Context& current,
                                                  const Matrix4x4* measuredToCurrent,
                                                  CascadeFitBounds& bounds) const
{
    // The view depths whose fragments sample this cascade, as the shader selects
    // them: its own range, plus the blend band at the end of the previous
    // cascade (cascade 0 from the camera, the last one to the shadow distance).
    const uint32_t last = frame.NumCascades - 1;
    float depthLo = 0.0f;
    if (cascadeIndex > 0)
        depthLo = frame.SplitDistances[cascadeIndex - 1] - CascadeBlendBand(frame, cascadeIndex - 1);
    const float depthHi =
        cascadeIndex == last ? m_Config.MaxShadowDistance : frame.SplitDistances[cascadeIndex];
    // Depths the reduce did not bin (the shadow distance grew since it
    // measured) hold receivers it never saw.
    if (depthHi > measured.Measured.BinFar * (1.0f + 1e-4f))
        return false;

    AABB receivers = GatherShadowReceivers(measured, depthLo, depthHi, current, measuredToCurrent);
    if (receivers.IsEmpty())
        return false;
    const float lateral = ReceiverReachWorld(
        std::max(receivers.max.x - receivers.min.x, receivers.max.y - receivers.min.y) * 0.5f);
    receivers.Inflate(Vector3{lateral, lateral, ReceiverMarginWorld()});
    const AABB narrowed = AABB::Intersection(bounds.Box, receivers);
    if (narrowed.IsEmpty())
        return false;
    bounds.Box = narrowed;
    // The casters are culled to the receivers' reach, not to the slice: a
    // receiver at the slice's edge reads past it.
    bounds.CasterBox = receivers;
    return true;
}

void ShadowMapRenderFeature::HoldAirInFrontOfCascade0(const ShadowReceiverMeasurement& measured,
                                                      float sliceStart,
                                                      const ShadowReceiverMeasurement::Context& current,
                                                      const Matrix4x4* measuredToCurrent,
                                                      const AABB& fromNear, CascadeFitBounds& bounds)
{
    // No filter reach around the air, as none around the slice: the box holds
    // what cascade 0 is selected for. The box a slice from the near plane fits,
    // and the eye, bound that air in the current frame, so a lagging
    // measurement widened by the camera's motion never grows the box past them.
    const AABB air = AABB::Intersection(
        fromNear, GatherShadowReceivers(measured, 0.0f, sliceStart, current, measuredToCurrent));
    bounds.Box.Expand(air);
}

ShadowMapRenderFeature::CascadeFit ShadowMapRenderFeature::ComputeCascadeLightVP(
    const CascadeFitBounds& bounds, const Vector3& lightDir,
    uint32_t resolution, Rendering::ViewId viewId, uint32_t cascadeIdx,
    const int32 originSector[3], float worldTexelBase, float guardBandFraction)
{
    // Build the light view matrix orientation. lightDir points from light
    // toward scene; the shadow camera looks down +ld in light space.
    const Vector3 ld = lightDir.Normalize();
    const Vector3 up = CascadeLightUp(ld);

    // `bounds` is what the box must hold, in light space (rotation-only, light
    // at the origin): a tight 2D AABB for the ortho extents and a Z range for
    // the depth extent. A tight AABB beats the bounding-sphere fit by 30-50% in
    // width x height — those texels were spent on empty area.
    //
    // Every light-space quantity is render-origin-relative, which is what keeps
    // the centre texel snap honest: it needs |centerLS| / texelWorldSize inside
    // fp32's exactly-representable integer range, and at world magnitude that
    // quotient reaches ~1.6e8, where one ULP is 16 texels.
    // CanReuseFrozenCascadeFit judges the frozen record against the same bounds.
    const Matrix4x4 lightRot = CascadeLightRotation(ld);
    const Vector3 minLS = bounds.Box.min;
    const Vector3 maxLS = bounds.Box.max;

    // Stable: the slice's bounding-SPHERE radius, invariant to camera rotation.
    // Close: a tight light-space AABB, squared with max() so worldPerTexel stays
    // uniform in X and Y and the per-cascade kernel scale
    // (ge_shadowPcssCascades.x) remains a single scalar.
    //
    // CanReuseFrozenCascadeFit MUST pick the same source. If it derives an AABB
    // half-extent while the fit emitted a sphere one, the frozen box looks
    // permanently oversize (sphere >= AABB always), the shrink refit trips every
    // frame, and the stability fix becomes continuous refitting.
    const float sphereRadius = bounds.SphereRadius;
    const float sphereCenterXLS = bounds.SphereCenterXLS;
    const float sphereCenterYLS = bounds.SphereCenterYLS;
    const bool stable = m_Config.Projection == ShadowProjection::Stable;
    const bool worldTexel = m_Config.Projection == ShadowProjection::WorldTexel;

    // WorldTexel: the extent is AUTHORED, so everything below that exists to stop
    // a fitted extent from drifting — the guard band, the power-of-two band snap,
    // the shrink hysteresis — has nothing to act on and is skipped. The extent is
    // already the most stable value it can be: a constant.
    float halfExtent;
    if (worldTexel)
    {
        // The SAME resolved base ComputeSplits used. Re-resolving AUTO here
        // would let the extents and the splits disagree whenever the solve's
        // inputs shifted between the two calls.
        halfExtent = 0.5f * static_cast<float>(resolution) *
                     CascadeTexelSizeFrom(worldTexelBase, m_Config.CascadeTexelRatio, cascadeIdx);
    }
    else
    {
        halfExtent = stable ? sphereRadius
                            : std::max((maxLS.x - minLS.x) * 0.5f, (maxLS.y - minLS.y) * 0.5f);

        // Fit-freeze guard band: inflate the raw extent before snapping so the
        // frozen box carries at least this fraction of slice travel budget per
        // side (0 under GE_SHADOW_FIT_FREEZE=0 — the exact pre-freeze fit).
        halfExtent *= (1.0f + guardBandFraction);
    }

    // Rotation-stability snap. Camera rotations rotate the frustum slice
    // corners in light space, which shifts the AABB extent. The previous
    // snap (1/16 m absolute) wasn't enough — for a cascade with a 5 m
    // halfExtent, rotation-induced extent drift of just a few cm crosses
    // multiple snap boundaries and produces visible shimmer downstream
    // (worldPerTexel changes => every shadow sample re-samples a different
    // world point => "slight rotations completely change the results").
    //
    // Use a fractional-power band: the band width is 12.5% of the next
    // power-of-two larger than halfExtent. This auto-scales per cascade
    // (cascade 0 ~5 m -> 1 m bands; cascade 3 ~150 m -> 32 m bands) so
    // rotation drifts within a band stay on the same snapped value, and
    // wasted-texel area is bounded to ~26.5% worst-case.
    //
    // Then layer hysteresis on top: if the new raw halfExtent is up to
    // 15% smaller than the previously-snapped value, reuse the previous
    // bucket. This prevents SDSM-driven slow extent shrinkage from
    // oscillating across a band edge frame after frame.
    if (!worldTexel)
    {
        const float magnitude = std::pow(2.0f, std::ceil(std::log2(std::max(halfExtent, 1e-3f))));
        const float band = magnitude * GetCascadeFitTuning(cascadeIdx).ExtentSnapBandFraction;
        float snapped = std::ceil(halfExtent / band) * band;

        if (cascadeIdx < kMaxShadowCascades)
        {
            auto& slot = m_PrevSnappedHalfExtent[viewId];
            const float prev = slot[cascadeIdx];
            // Only snap-DOWN with hysteresis. If raw is GROWING past the
            // previous bucket, take the new bucket immediately so cascades
            // that need to expand (camera moves to reveal more scene) don't
            // lag behind.
            if (prev > 0.0f && halfExtent <= prev && halfExtent > prev * kExtentShrinkHysteresis)
                snapped = prev;
            slot[cascadeIdx] = snapped;
        }
        halfExtent = snapped;
    }

    // Z extent snap: snap minLS.z DOWN and maxLS.z UP to a Z-quantum
    // proportional to halfExtent. Without this, sub-percent rotation-
    // induced drift in minLS.z / maxLS.z perturbs every stored depth
    // value through the polynomial encode (z, z², z³, z⁴) used by MSM4
    // and produces visible shimmer. PCF / PCSS tolerates this through
    // binary depth comparison; MSM does not. The X/Y halfExtent snap
    // above handles the orthographic frustum width; this is the matching
    // Z-axis stabilizer. Done before centerZLS computation so the center
    // is consistent with the snapped Z bounds.
    const float zQuantum = std::max(halfExtent * 0.0625f, 1e-3f);
    const float minZSnapped = std::floor(minLS.z / zQuantum) * zQuantum;
    // Freeze guard band on the far depth bound too: light-space Z of the
    // slice drifts under camera motion just like XY, and a refit forced by a
    // few centimetres of Z drift would defeat the XY budget.
    const float maxZSnapped =
        std::ceil((maxLS.z + halfExtent * guardBandFraction) / zQuantum) * zQuantum;

    // Frustum-slice center in light space. Snap to the light-space texel
    // grid to prevent sub-texel translation drift (shadow edge crawling).
    // Under Stable the centre is the SPHERE centre, not the AABB midpoint: the
    // extent is the sphere radius, and containment only holds if the box is
    // centred on the point that radius was measured from.
    // WorldTexel centres on the sphere centre for the same reason Stable does:
    // containment only holds if the box is centred on the point its extent was
    // measured against — and the derived splits sized this cascade so the slice
    // sphere fits inside the authored box.
    const bool useSphereCentre = stable || worldTexel;
    float centerXLS = useSphereCentre ? sphereCenterXLS : (minLS.x + maxLS.x) * 0.5f;
    float centerYLS = useSphereCentre ? sphereCenterYLS : (minLS.y + maxLS.y) * 0.5f;
    float centerZLS = (minZSnapped + maxZSnapped) * 0.5f;
    const float texelWorldSize = (2.0f * halfExtent) / static_cast<float>(resolution);
    centerXLS = std::round(centerXLS / texelWorldSize) * texelWorldSize;
    centerYLS = std::round(centerYLS / texelWorldSize) * texelWorldSize;

    // Convert the snapped light-space center back to the render-origin-relative
    // frame so we can place the shadow camera there.
    Matrix4x4 invRot = Mathematics::Inverse(lightRot);
    Vector4 snappedRel = invRot.Transform(Vector4{centerXLS, centerYLS, centerZLS, 1.0f});
    Vector3 center{snappedRel.x, snappedRel.y, snappedRel.z};

    // Pull the near plane back so casters that sit between the light and
    // the slice (off-screen casters) still write into the shadow map.
    // Scales with halfExtent so the back-pull stays proportional.
    float backExtension = halfExtent * kShadowFrustumExtension;
    float nearZLS = minZSnapped - backExtension;
    float farZLS  = maxZSnapped;
    float depthRange = farZLS - nearZLS;

    // Position the shadow camera at the light-space "near" of the slice
    // (after back-pull) along -ld and orient it toward +ld. Using halfExtent
    // as the camera-to-center back-distance keeps near=0 in light-clip space.
    // Render-origin-relative, like everything the fit derived.
    const Vector3 lightPosRel = center - ld * (centerZLS - nearZLS);

    Matrix4x4 lightProj = Mathematics::MakeOrthographicLH_ZO_ReverseZ(
        -halfExtent, halfExtent, -halfExtent, halfExtent, 0.0f, depthRange);

    const bool originActive =
        originSector[0] != 0 || originSector[1] != 0 || originSector[2] != 0;
    Matrix4x4 lightVP;
    Matrix4x4 lightVPRel;
    if (originActive)
    {
        // Two VPs from one fit, differing only in which frame their eye is
        // expressed in.
        //
        // The RELATIVE one is the fit's real output: its eye is local, so its
        // clip translation resolves the texel grid exactly, and both the depth
        // pass (caster raster) and ge_shadowVP (receiver sampling) consume it —
        // their agreement is exact by construction.
        //
        // The WORLD one exists for the world-space caster culling in
        // OnScheduleCulling (cull planes, scene-bounds AABB, the caster
        // footprint tighten, all of which work on world geometry). Its translation column
        // inherits ULP(|eye|) — 0.5 m at Earth radius — which the cull widens
        // its footprint by (kCullPrecisionUlps) and which is far above a shadow
        // texel, which is exactly why receivers must not use it.
        const double originX = static_cast<double>(originSector[0]) * kSectorSize;
        const double originY = static_cast<double>(originSector[1]) * kSectorSize;
        const double originZ = static_cast<double>(originSector[2]) * kSectorSize;
        lightVPRel = lightProj * MakeLightViewAtEye(lightRot, lightPosRel.x, lightPosRel.y,
                                                   lightPosRel.z);
        lightVP = lightProj * MakeLightViewAtEye(lightRot, lightPosRel.x + originX,
                                                 lightPosRel.y + originY,
                                                 lightPosRel.z + originZ);
        SnapClipTranslationToTexels(lightVPRel, resolution);
        SnapClipTranslationToTexels(lightVP, resolution);
    }
    else
    {
        // Origin inactive: the relative frame IS world space, so the legacy
        // construction stands and VPRel is a bit-for-bit copy — the dark-ship
        // gate. Every scene inside the activation radius renders exactly as it
        // did before camera-relative rendering existed.
        const Matrix4x4 lightView = Mathematics::MakeLookAtLH(lightPosRel, lightPosRel + ld, up);
        lightVP = lightProj * lightView;
        SnapClipTranslationToTexels(lightVP, resolution);
        lightVPRel = lightVP;
    }

    CascadeFit fit{lightVP, lightVPRel, halfExtent, depthRange};
    // Light-space internals for the fit-freeze coverage test, in the SAME
    // render-origin-relative light basis the freeze re-derives them in. Safe
    // across a rebase because CanReuseFrozenCascadeFit refuses reuse outright on
    // any sector change, so a frozen record's frame never shifts under it.
    fit.CenterXLS = centerXLS;
    fit.CenterYLS = centerYLS;
    fit.NearZLS = nearZLS;
    fit.FarZLS = farZLS;
    return fit;
}

void ShadowMapRenderFeature::SelectCascadeSamplingFit(const CascadeContentFit* fit,
                                                      const CascadeFrameData& frame,
                                                      uint32_t cascadeIndex, float resolution,
                                                      float outShadowVP[16],
                                                      float& outWorldPerTexel, float& outDepthSpan)
{
    const CascadeContentFit* cf = (fit && fit->Valid) ? fit : nullptr;
    // ge_shadowVP is the CAMERA-RELATIVE cascade VP: the receiver samples it
    // with the fragment's render-origin-relative position (vPosRel), matching
    // the depth pass which rasterized casters through the same relative VP.
    // Byte-identical to LightVP when the origin is inactive (dark-ship).
    if (cf)
    {
        // The retained layer's own rasterizing matrix, bit-for-bit — never
        // re-derived from the world-space fit, whose fp32 translation at
        // planetary magnitude is coarser than a shadow texel (#660 shadow
        // follow-up). A sector step during the deferral window re-anchors it
        // with the delta exact in double from the integer sectors.
        Matrix4x4 contentVPRel = cf->LightVPRel;
        if (cf->Sector[0] != frame.RenderOriginSector[0] ||
            cf->Sector[1] != frame.RenderOriginSector[1] ||
            cf->Sector[2] != frame.RenderOriginSector[2])
            RebaseTranslationColumnBySectorDelta(cf->LightVPRel.Data(), cf->Sector,
                                                 frame.RenderOriginSector, contentVPRel.Data());
        std::memcpy(outShadowVP, contentVPRel.Data(), 16 * sizeof(float));
    }
    else
    {
        std::memcpy(outShadowVP, frame.LightVPRel[cascadeIndex].Data(), 16 * sizeof(float));
    }
    // worldPerTexel is precomputed so the shader avoids a per-pixel divide;
    // depthSpan converts an NDC-depth delta into a world-space delta (depth is
    // linear under an orthographic projection). Both follow the content fit
    // for the same reason the matrix does.
    outWorldPerTexel =
        (2.0f * (cf ? cf->OrthoHalfExtent : frame.OrthoHalfExtent[cascadeIndex])) / resolution;
    outDepthSpan = cf ? cf->DepthSpan : frame.DepthSpan[cascadeIndex];
}

float ShadowMapRenderFeature::PcssWidestKernelTexels(Rendering::ViewId viewId,
                                                     const CascadeFrameData& frame,
                                                     uint32_t cascadeCount) const
{
    const float capWorld =
        GetPcssMaxPenumbra() > 0.0f ? GetPcssMaxPenumbra() : kPcssMaxPenumbraWorldAuto;
    const std::array<CascadeContentFit, kMaxShadowCascades>* contentFits = nullptr;
    if (const auto it = m_CascadeContentFit.find(viewId); it != m_CascadeContentFit.end())
        contentFits = &it->second;
    const float resolution = static_cast<float>(GetConfig().Resolution);
    float widest = 1.0f;
    for (uint32_t i = 0; i < cascadeCount && i < kMaxShadowCascades; ++i)
    {
        float shadowVP[16];
        float worldPerTexel = 0.0f;
        float depthSpan = 0.0f;
        SelectCascadeSamplingFit(contentFits ? &(*contentFits)[i] : nullptr, frame, i, resolution,
                                 shadowVP, worldPerTexel, depthSpan);
        if (worldPerTexel > 0.0f)
            widest = std::max(widest, capWorld / worldPerTexel);
    }
    return widest;
}

ShadowFilterQuality ShadowMapRenderFeature::ResolveRequestedFilterQuality(
    const RenderServices& rs, Rendering::ViewId viewId) const
{
    const auto* view = rs.Views().FindViewDesc(viewId);
    if (view)
    {
        const auto& settings = rs.GetWorldShadowSettings(view->worldId);
        if (settings.HasOverride)
        {
            static_assert(static_cast<int>(Components::DirectionalShadowFilter::DilatedPCF) ==
                              static_cast<int>(ShadowFilterQuality::DPCF),
                          "DirectionalShadowFilter must mirror ShadowFilterQuality values");
            return static_cast<ShadowFilterQuality>(static_cast<int>(settings.Filter));
        }
    }
    return GetFilterQuality();
}

ShadowFilterQuality ShadowMapRenderFeature::ResolveEffectiveFilterQuality(
    RenderServices& rs, Rendering::ViewId viewId) const
{
    // Raw depth comes through the bindless layer views, or under the compat shader
    // profile through ge_shadowMapRaw, a second read-only binding of the cascade
    // array (shadow_sampling.glsl).
    const bool rawDepthReadable = rs.Textures().IsBindlessEnabled() || Rendering::IsCompatShaderProfile();
    const bool preferStableFiltering =
        rs.GetDevice() && rs.GetDevice()->GetCapabilities().prefersStableShadowFiltering;

    ShadowFilterQuality quality = ResolveRequestedFilterQuality(rs, viewId);

    // MSM4 dispatches only with its moments texture allocated for this view;
    // without it the descriptor at slot 10 could be sampled unbound, so the
    // request falls through to the PCSS chain below. ShadowMapNode's
    // AddMsmPassesForView allocates moments unconditionally during BuildForView,
    // so by the time the world pass executes the same frame they exist.
    if (quality == ShadowFilterQuality::MSM4 && !GetMsmMomentsTexture(viewId).IsValid())
        quality = ShadowFilterQuality::PCSS;

    // PCSS and DPCF both read raw depth; without a way to, or on a device whose
    // driver prefers stable filtering, they are Poisson PCF. DPCF demotes straight to Poisson rather than to PCSS: it
    // exists to be CHEAPER than PCSS, so silently promoting it would invert the
    // reason a project selected it.
    if ((quality == ShadowFilterQuality::PCSS || quality == ShadowFilterQuality::DPCF) &&
        (!rawDepthReadable || preferStableFiltering))
        quality = ShadowFilterQuality::PoissonPCF;

    return quality;
}

void ShadowMapRenderFeature::BuildShadowDataGPU(const CascadeFrameData& cascadeFrame,
                                                     Rendering::ViewId viewId,
                                                     RenderServices& rs,
                                                     RenderGraph::RGFrame& rgFrame,
                                                     uint32_t frameIndex,
                                                     ShadowDataGPU& out)
{
    std::memset(&out, 0, sizeof(ShadowDataGPU));

    // "No pyramid" is NEGATIVE, not zero — zero is a legal-looking bindless
    // index. Written before anything can publish a real one, so every path out
    // of this function that does not register the pyramid leaves the shader
    // running the unaccelerated PCSS filter.
    for (uint32_t i = 0; i < kMaxShadowCascades; ++i)
        out.shadowPcssPyramid[i][0] = -1.0f;

    const float resolution = static_cast<float>(std::max(1u, m_Config.Resolution));

    // Content-fit invariant: receivers must transform by the fit of the depth
    // content ACTUALLY retained in each cascade layer — the snapshot committed
    // when the layer last rendered. On a frame where the cascade rendered (or
    // the static cache skipped it byte-identically) the snapshot IS this
    // frame's LightVPRel and uploads bit-for-bit, so the always-render path is
    // unchanged. Only a motion-deferred cascade diverges: its older rel fit is
    // re-anchored to the CURRENT origin (a sector step during the deferral
    // window is absorbed by the exact integer-sector delta) so the fragment's
    // vPosRel keys into the retained layer's texels instead of swimming with
    // the current fit. Slots with no snapshot (first frames, post-invalidate,
    // standalone test calls) fall back to the current fit — those slots render
    // this frame anyway (FirstRender is a correctness class). Split distances
    // stay CURRENT: they only select the cascade, and retained content covers
    // the old slice plus the fit's overlap margins.
    const std::array<CascadeContentFit, kMaxShadowCascades>* contentFits = nullptr;
    if (const auto cfIt = m_CascadeContentFit.find(viewId); cfIt != m_CascadeContentFit.end())
        contentFits = &cfIt->second;

    for (uint32_t i = 0; i < cascadeFrame.NumCascades && i < kMaxShadowCascades; ++i)
    {
        // x = world-per-texel, y = depth span (both content-fit-following, see
        // SelectCascadeSamplingFit).
        SelectCascadeSamplingFit(contentFits ? &(*contentFits)[i] : nullptr, cascadeFrame, i,
                                 resolution, out.shadowVP[i],
                                 out.shadowPcssCascades[i][0], out.shadowPcssCascades[i][1]);
        out.shadowSplits[i] = cascadeFrame.SplitDistances[i];
        // w = tan(half angular diameter), dimensionless. Uniform across
        //     cascades: a light's angular size does not depend on which cascade
        //     a fragment lands in. The shader multiplies it by a world-space
        //     depth delta to get the penumbra width.
        out.shadowPcssCascades[i][3] = cascadeFrame.ShadowTanHalfAngle;
    }

    out.shadowParams[0] = cascadeFrame.DepthBias;
    out.shadowParams[1] = cascadeFrame.NormalBias;
    out.shadowParams[2] = static_cast<float>(cascadeFrame.NumCascades);
    out.shadowParams[3] = cascadeFrame.MaxShadowDistance;

    // Sampling an unwritten reverse-Z cascade array (depth 1.0) fully shadows
    // the frame. If this frame declared no cascade pass and no layer has
    // retained content, publish numCascades=0 so the shader fail-safe returns
    // fully lit.
    if (rgFrame.FrameIndex() == m_CascadeDeclareFrameIndex &&
        m_DeclaredCascadePassesThisFrame == 0)
    {
        bool anyValidFit = false;
        if (const auto it = m_CascadeContentFit.find(viewId); it != m_CascadeContentFit.end())
        {
            for (const CascadeContentFit& fit : it->second)
            {
                if (fit.Valid)
                {
                    anyValidFit = true;
                    break;
                }
            }
        }
        if (!anyValidFit)
            out.shadowParams[2] = 0.0f;
    }

    out.shadowDebug[0] = static_cast<float>(static_cast<int>(GetDebugMode()));

    // Resolve final quality: user picks via FilterQuality; PCSS/MSM4 fall
    // back automatically when prerequisites aren't met. ResolveEffectiveFilterQuality
    // owns that chain and the pyramid declaration shares it, so the resource
    // build and the shader branch cannot disagree about the same frame.
    const ShadowFilterQuality effective = ResolveEffectiveFilterQuality(rs, viewId);
    int quality = static_cast<int>(effective);

    // Ahead of the quality branch and unconditional, because this call is the
    // pyramid's RELEASE point as much as its registration point. The pyramid is
    // a pooled persistent texture, and the pool DESTROYS one that has not been
    // imported for kPersistentMaxIdleFrames (300, ~5 s at 60 fps) — it does not
    // notify the consumers that registered views on it. Registered slots must
    // therefore never outlive the frames the pyramid is declared for: run this
    // only on PCSS frames and a switch away from PCSS strands four descriptors
    // pointing at views of an image the pool frees five seconds later.
    //
    // Reaching it on non-PCSS frames is what makes the release observe that,
    // since the state is zeroed once the pyramid stops declaring and the
    // stored physical no longer matches. Registering on a frame that then
    // demotes to Poisson costs four slots nothing samples for that frame, which
    // is the correct trade: the registration tracks the pyramid's EXISTENCE,
    // not the publish decision.
    const ShadowMinMaxPyramid::FrameState pyramid = m_MinMaxPyramid.StateFor(viewId, rgFrame);
    const PyramidBindlessIndices* pyramidIdx = EnsurePyramidBindlessTextures(viewId, rs, pyramid);

    if ((effective == ShadowFilterQuality::PCSS || effective == ShadowFilterQuality::DPCF) &&
        Rendering::IsCompatShaderProfile())
    {
        // The compat profile reads raw depth from ge_shadowMapRaw by cascade layer,
        // which the world pass binds whenever it binds the cascade array: no
        // indices to publish, and no pyramid (its reads are bindless too).
        out.shadowDebug[1] = 1.0f;
    }
    else if (effective == ShadowFilterQuality::PCSS || effective == ShadowFilterQuality::DPCF)
    {
        // Raw-depth bindless views exist for the two filters that need depth
        // VALUES via texelFetch rather than hardware compare results: the PCSS
        // blocker search, and DPCF's per-tap occluder-distance accumulation.
        // Register them (and publish the indices) only when one of those is
        // actually dispatched — every other quality mode filters purely through
        // the comparison sampler, and the raw layer views never need to exist.
        if (const PcssBindlessIndices* rawDepthIdx =
                EnsurePcssBindlessTextures(viewId, rs, rgFrame))
        {
            for (uint32_t i = 0; i < cascadeFrame.NumCascades && i < kMaxShadowCascades; ++i)
                out.shadowPcssCascades[i][2] = static_cast<float>(rawDepthIdx->Indices[i]);
            // Raw-depth-ready flag: gates BOTH the PCSS and DPCF shader branches.
            out.shadowDebug[1] = 1.0f;

            // The pyramid is a separate accelerator, not a consequence of PCSS
            // being active: an MSM4-without-moments promotion reaches here on a
            // frame the pyramid was never declared for, and the pyramid itself
            // declines whenever its cascade depth is unavailable. Both report
            // zero levels, and both must keep the sentinel.
            if (pyramidIdx)
            {
                for (uint32_t i = 0; i < pyramid.Layers && i < kMaxShadowCascades; ++i)
                {
                    out.shadowPcssPyramid[i][0] = static_cast<float>(pyramidIdx->Indices[i]);
                    out.shadowPcssPyramid[i][1] = static_cast<float>(pyramid.Levels);
                    out.shadowPcssPyramid[i][2] =
                        static_cast<float>(ShadowMinMaxPyramid::kBaseDownshift);
                }
            }
        }
        else
        {
            // Bindless registration failed — fall through to Poisson. This is
            // the one demotion ResolveEffectiveFilterQuality cannot predict, so
            // a pyramid may already have been built for this frame.
            quality = static_cast<int>(ShadowFilterQuality::PoissonPCF);
        }
    }

    out.shadowDebug[2] = static_cast<float>(frameIndex);
    out.shadowDebug[3] = static_cast<float>(quality);

    out.shadowPcss[0] = GetPoissonSoftness();
    // shadowPcss[1] = PCSS / Poisson disk tap count. Sent as float (cast to int
    // and clamped to [8, 64] in the shader).
    out.shadowPcss[1] = static_cast<float>(GetPcssTapCount());
    out.shadowPcss[2] = GetPcssMaxPenumbra();
    out.shadowPcss[3] = IsPcssReceiverPlaneBias() ? 1.0f : 0.0f;

    out.shadowFilterParams[0] = static_cast<float>(static_cast<int>(GetDitherBasis()));
    const auto* view = rs.Views().FindViewDesc(viewId);
    out.shadowFilterParams[1] = view
        ? rs.GetWorldShadowSettings(view->worldId).DistanceFadeFraction
        : Components::ShadowSettingsEffect{}.DistanceFadeFraction;
    out.shadowFilterParams[2] = 0.0f;
    out.shadowFilterParams[3] = 0.0f;

    WriteTerrainShadowMap(viewId, rs, rgFrame.FrameIndex(), out);
}

ShadowMapRenderFeature::PublishedTerrainShadow& ShadowMapRenderFeature::TerrainShadowEntry(ViewId viewId)
{
    for (PublishedTerrainShadow& entry : m_TerrainShadows)
        if (entry.View == viewId)
            return entry;
    m_TerrainShadows.push_back({});
    m_TerrainShadows.back().View = viewId;
    return m_TerrainShadows.back();
}

void ShadowMapRenderFeature::PublishTerrainShadowMap(ViewId viewId, uint64_t frameStamp,
                                                     const TerrainShadowMap& map, RenderServices& rs)
{
    PublishedTerrainShadow& entry = TerrainShadowEntry(viewId);
    entry.HasMap = true;
    entry.MapStamp = frameStamp;
    entry.Map = map;
    // ShadowData already built for this frame (ShadowMap declared before the terrain): write the
    // terrain's words into it now.
    if (entry.Upload && entry.UploadStamp == frameStamp)
        WriteTerrainShadowMap(viewId, rs, frameStamp, *entry.Upload);
}

void ShadowMapRenderFeature::AttachShadowDataUpload(ViewId viewId, uint64_t frameStamp, ShadowDataGPU* upload)
{
    PublishedTerrainShadow& entry = TerrainShadowEntry(viewId);
    entry.Upload = upload;
    entry.UploadStamp = frameStamp;
}

const TerrainShadowMap* ShadowMapRenderFeature::FindTerrainShadowMap(ViewId viewId,
                                                                     uint64_t frameStamp) const
{
    for (const PublishedTerrainShadow& entry : m_TerrainShadows)
        if (entry.View == viewId)
            return entry.HasMap && entry.MapStamp == frameStamp ? &entry.Map : nullptr;
    return nullptr;
}

void ShadowMapRenderFeature::WriteTerrainShadowMap(ViewId viewId, RenderServices& rs,
                                                   uint64_t frameStamp, ShadowDataGPU& out) const
{
    const TerrainShadowMap* published = FindTerrainShadowMap(viewId, frameStamp);
    if (!published)
        return;
    const TerrainShadowMap& map = *published;
    // The receivers shade in the render-origin-relative frame (vPosRel), so the grid is rebased the
    // same way, in double before the narrowing.
    const Rendering::CameraData cam = rs.Views().ResolveCameraData(viewId);
    const double originX = static_cast<double>(cam.renderOriginSector[0]) * kSectorSize;
    const double originY = static_cast<double>(cam.renderOriginSector[1]) * kSectorSize;
    const double originZ = static_cast<double>(cam.renderOriginSector[2]) * kSectorSize;
    const double centerX = map.TerrainX + 0.5 * static_cast<double>(map.TerrainSizeX);
    const double centerZ = map.TerrainZ + 0.5 * static_cast<double>(map.TerrainSizeZ);
    out.terrainShadowGrid0[0] = static_cast<float>(centerX - originX);
    out.terrainShadowGrid0[1] = static_cast<float>(centerZ - originZ);
    out.terrainShadowGrid0[2] = static_cast<float>(map.TerrainX - originX);
    out.terrainShadowGrid0[3] = static_cast<float>(map.TerrainZ - originZ);
    out.terrainShadowGrid1[0] = map.TerrainSizeX;
    out.terrainShadowGrid1[1] = map.TerrainSizeZ;
    out.terrainShadowGrid1[2] = map.SunX;
    out.terrainShadowGrid1[3] = map.SunZ;
    out.terrainShadowGrid2[0] = map.TanElevation;
    out.terrainShadowGrid2[1] = map.Texel;
    out.terrainShadowGrid2[2] = map.UMin;
    out.terrainShadowGrid2[3] = map.VMin;
    out.terrainShadowGrid3[0] = map.SamplesU;
    out.terrainShadowGrid3[1] = map.SamplesV;
    out.terrainShadowGrid3[2] = static_cast<float>(map.BaseY - originY);
    out.terrainShadowGrid3[3] = map.HeightScale;
    out.terrainShadowSource[0] = map.MapBindlessIndex;
    out.terrainShadowSource[1] = map.HeightBindlessIndex;
    out.terrainShadowSource[2] = 1u;
    out.terrainShadowSource[3] = map.MapSide;
}

Rendering::TextureHandle ShadowMapRenderFeature::GetShadowMapTexture(
    Rendering::ViewId viewId, const RenderGraph::RGFrame& frame) const
{
    auto it = m_ShadowMapByView.find(viewId);
    if (it == m_ShadowMapByView.end() || !it->second.AdoptedFor.IsFor(frame))
        return {};
    return it->second.Physical;
}

void ShadowMapRenderFeature::AdoptPooledShadowMap(Rendering::ViewId viewId,
                                                       Rendering::TextureHandle physical,
                                                       RenderServices& rs,
                                                       RenderGraph::RGFrame& frame)
{
    auto it = m_ShadowMapByView.find(viewId);
    if (it != m_ShadowMapByView.end() && it->second.Physical == physical)
    {
        // Same physical, new frame: refresh the stamp so the steady-state view
        // keeps serving this handle. Skipping this would expire the entry after
        // its first frame and silently drop every view's shadows.
        it->second.AdoptedFor.Stamp(frame);
        return;
    }

    // The handle changed: invalidate every cache keyed off the OLD handle,
    // EXCEPT the texture itself — the pool owns it and defer-destroys the old
    // physical on its own schedule. A first adopt (no prior entry) has nothing
    // stale — skipping the erase matters because the CSM node caches frame data
    // BEFORE the cascade arm imports/adopts.
    if (it != m_ShadowMapByView.end() && it->second.Physical.IsValid())
    {
        auto pcssIt = m_PcssBindlessByView.find(viewId);
        if (pcssIt != m_PcssBindlessByView.end())
        {
            // Release the OLD physical's bindless slots and the per-layer views
            // written into them. Dropping only our index map would park neither:
            // the slot would never be reissued and its descriptor would keep
            // pointing at a view of a texture the pool is about to free, and the
            // id-keyed bindless cache would hand the stale index straight back to
            // the re-registration below whenever the pool reissues the handle id.
            rs.Textures().InvalidateBindless(it->second.Physical);
            m_PcssBindlessByView.erase(pcssIt);
        }
        m_CachedFrameData.erase(viewId);
        // Retained cascade layers died with the old physical; the static-cache
        // records must not skip against a fresh Undefined image. (The tint
        // array can realloc on the same config-change frames; clearing the
        // whole view covers both slot families.)
        m_CascadeShadowCache.InvalidateView(viewId);
        // Same for the motion round-robin: no content fits to sample or defer
        // against — the fresh image renders (FirstRender) and reseeds.
        m_CascadeContentFit.erase(viewId);
        m_TintContentFit.erase(viewId);
        m_MotionPlanByView.erase(viewId);
    }

    if (physical.IsValid())
    {
        AdoptedShadowMap& adopted = m_ShadowMapByView[viewId];
        adopted.Physical = physical;
        adopted.AdoptedFor.Stamp(frame);
        m_PoolOwnedShadowMapViews.insert(viewId);
    }
    else if (it != m_ShadowMapByView.end())
    {
        m_ShadowMapByView.erase(it);
        m_PoolOwnedShadowMapViews.erase(viewId);
    }
}

Rendering::TextureHandle ShadowMapRenderFeature::GetMsmMomentsTexture(Rendering::ViewId viewId) const
{
    auto it = m_MsmMomentsByView.find(viewId);
    return (it != m_MsmMomentsByView.end()) ? it->second : Rendering::TextureHandle{};
}

// MSM4 moments storage. UNORM16 is the quality-preferred encoding; core
// WebGPU has no 16-bit UNORM color formats, so those devices store moments as
// FLOAT16 (same channel count, sampled as float either way).
static Rendering::TextureFormat MsmMomentsFormat(const Rendering::IDevice& device)
{
    return device.GetCapabilities().supportsUnorm16TextureFormats
               ? Rendering::TextureFormat::R16G16B16A16_UNORM
               : Rendering::TextureFormat::R16G16B16A16_FLOAT;
}

Rendering::TextureHandle ShadowMapRenderFeature::GetMsmMomentsStubTexture()
{
    if (!m_Device)
        return m_MsmMomentsStub;

    // The sampler MUST exist whenever the stub is bound at slot 10 — the
    // world-pass binding goes through UpdateCombinedImageSamplerBinding
    // and skips the write if either is invalid (which would itself trip
    // VUID-04007 territory). EnsureMsmMomentsForView only creates the
    // sampler when allocating real moments, so when MSM4 is off and only
    // the stub is in play the sampler also has to come from here.
    if (!m_MsmSampler.IsValid())
    {
        m_MsmSampler = m_Device->CreateSampler(
            Rendering::SamplerDesc::MaterialLinearClamp("MsmMomentsLinearClamp"));
    }

    if (m_MsmMomentsStub.IsValid())
        return m_MsmMomentsStub;

    // 1x1 moments-format array with NumCascades layers — minimum allocation
    // that satisfies a sampler2DArray descriptor binding for the world
    // pass while MSM4 isn't the active filter quality. The shader's
    // `if (quality == 4)` gate prevents any sampling, so the contents are
    // irrelevant. Total size is ~32 bytes per cascade × 4 = ~128 bytes.
    Rendering::TextureDesc td{};
    td.width = 1;
    td.height = 1;
    td.depth = 1;
    td.mipLevels = 1;
    td.arrayLayers = m_Config.NumCascades;
    td.format = static_cast<uint32_t>(MsmMomentsFormat(*m_Device));
    td.usage = static_cast<uint32_t>(Rendering::TextureUsage::ShaderResource);
    td.sampleCount = 1;
    td.flags = Rendering::TextureCreateFlags::ForceArrayView;
    td.persistent = true;
    td.initialState = Rendering::ResourceState::ShaderResource;
    td.debugName = "MsmMoments.Stub";
    m_MsmMomentsStub = m_Device->CreateTexture(td);
    return m_MsmMomentsStub;
}

Rendering::TextureHandle ShadowMapRenderFeature::GetMsmMomentsTextureForBinding(
    Rendering::ViewId viewId)
{
    auto live = GetMsmMomentsTexture(viewId);
    if (live.IsValid())
        return live;
    return GetMsmMomentsStubTexture();
}

Rendering::TextureHandle ShadowMapRenderFeature::EnsureMsmMomentsForView(Rendering::ViewId viewId,
                                                                              const RenderServices& rs,
                                                                              bool* outRecreated)
{
    if (outRecreated)
        *outRecreated = false;
    if (!m_Device)
        return {};

    // Lazy allocation: only create the per-view full-size moments array
    // when MSM4 is the active filter quality. When MSM4 is not active the
    // pass setups early-return on an invalid handle, the RG culls the MSM
    // write+blur passes via EnablePass(false), and the world pass's set-0
    // binding 10 falls back to GetMsmMomentsStubTexture(). Avoids burning
    // 32-128 MB per view (depending on MomentsResolution) when MSM4 isn't
    // selected. Toggling FilterQuality back to MSM4 reallocates on the
    // next EnsureMsmMomentsForView call.
    if (ResolveRequestedFilterQuality(rs, viewId) != ShadowFilterQuality::MSM4)
    {
        // If MSM4 was previously active for this view, free the live
        // moments texture so we don't keep paying the memory cost after
        // toggling away. The stub stays around — it's tiny and shared.
        auto liveIt = m_MsmMomentsByView.find(viewId);
        if (liveIt != m_MsmMomentsByView.end() && liveIt->second.IsValid())
        {
            m_Device->DestroyTexture(liveIt->second);
            m_MsmMomentsByView.erase(liveIt);
            m_MsmMomentsDimsByView.erase(viewId);
            if (outRecreated)
                *outRecreated = true; // signal callers (ShadowMapNode)
                                      // to drop their cached RG-import handle.
        }
        return {};
    }

    auto it = m_MsmMomentsByView.find(viewId);
    auto dimsIt = m_MsmMomentsDimsByView.find(viewId);
    if (it != m_MsmMomentsByView.end() && it->second.IsValid()
        && dimsIt != m_MsmMomentsDimsByView.end()
        && dimsIt->second.Resolution == m_Config.MomentsResolution
        && dimsIt->second.NumCascades == m_Config.NumCascades)
    {
        return it->second;
    }

    // Stale or missing — destroy + recreate. Deferred-destroy via the device
    // queue, erase parallel caches keyed off the texture handle.
    if (it != m_MsmMomentsByView.end() && it->second.IsValid())
    {
        m_Device->DestroyTexture(it->second);
        m_MsmMomentsByView.erase(it);
    }

    Rendering::TextureDesc td{};
    td.width = m_Config.MomentsResolution;
    td.height = m_Config.MomentsResolution;
    td.depth = 1;
    td.mipLevels = 1;
    td.arrayLayers = m_Config.NumCascades;
    td.format = static_cast<uint32_t>(MsmMomentsFormat(*m_Device));
    // RenderTarget kept for backwards compatibility with the graphics
    // msm_write/blur path; UnorderedAccess required for the fused
    // msm_cascade compute dispatch's imageStore writes; ShaderResource
    // for the lighting-pass sampling at binding 10.
    td.usage = static_cast<uint32_t>(Rendering::TextureUsage::RenderTarget)
             | static_cast<uint32_t>(Rendering::TextureUsage::UnorderedAccess)
             | static_cast<uint32_t>(Rendering::TextureUsage::ShaderResource);
    td.sampleCount = 1;
    td.flags = Rendering::TextureCreateFlags::ForceArrayView;
    td.persistent = true;
    td.initialState = Rendering::ResourceState::ShaderResource;
    std::string debugName = "MsmMoments.View" + std::to_string(static_cast<uint32_t>(viewId));
    td.debugName = debugName.c_str();

    auto tex = m_Device->CreateTexture(td);
    if (tex.IsValid())
    {
        m_MsmMomentsByView[viewId] = tex;
        m_MsmMomentsDimsByView[viewId] = MomentsDims{m_Config.MomentsResolution, m_Config.NumCascades};
        if (outRecreated)
            *outRecreated = true;
    }

    // Linear-clamp sampler is shared across views; create-once.
    if (!m_MsmSampler.IsValid())
    {
        m_MsmSampler = m_Device->CreateSampler(
            Rendering::SamplerDesc::MaterialLinearClamp("MsmMomentsLinearClamp"));
    }
    return tex;
}

void ShadowMapRenderFeature::CacheFrameData(Rendering::ViewId viewId, const CascadeFrameData& data)
{
    m_CachedFrameData[viewId] = data;
}

const CascadeFrameData* ShadowMapRenderFeature::GetCachedFrameData(Rendering::ViewId viewId) const
{
    auto it = m_CachedFrameData.find(viewId);
    return (it != m_CachedFrameData.end()) ? &it->second : nullptr;
}

const ShadowMapRenderFeature::PcssBindlessIndices*
ShadowMapRenderFeature::EnsurePcssBindlessTextures(
    Rendering::ViewId viewId, RenderServices& rs, const RenderGraph::RGFrame& frame)
{
    if (!rs.Textures().IsBindlessEnabled())
        return nullptr;

    // Liveness FIRST, above the cache hit: the cached indices name single-layer
    // views OF the adopted physical, so a handle that is not servable for THIS
    // frame invalidates a cache hit exactly as much as it blocks a fresh
    // registration. Checking only at the registration below would hand the
    // shader stale bindless slots on precisely the frames the guard exists for.
    auto shadowTex = GetShadowMapTexture(viewId, frame);
    if (!shadowTex.IsValid())
        return nullptr;

    auto it = m_PcssBindlessByView.find(viewId);
    if (it != m_PcssBindlessByView.end())
        return &it->second;

    // Register each cascade layer as a separate bindless Texture2D. The shadow map is
    // a 2D array texture, so each slot needs its own single-layer View2D — binding the
    // full array handle into a sampler2D slot yields undefined behavior on MoltenVK
    // (and likely other drivers), producing wrong/zero depth reads. arraySlice + a
    // non-Color aspect make TextureService create exactly that view and own it
    // alongside the slot; aspect=Depth is required so the blocker-search sampler2D
    // path reads the depth aspect (not Color) of the D32_FLOAT shadow texture. The
    // bindless allocator does NOT guarantee contiguous indices, so each cascade's
    // index is stored independently rather than computing as base+cascadeIdx.
    PcssBindlessIndices entry{};
    for (uint32_t c = 0; c < m_Config.NumCascades; ++c)
    {
        Rendering::BindlessTextureDesc desc{};
        desc.textureHandle = shadowTex;
        desc.type = Rendering::BindlessResourceType::Texture2D;
        desc.arraySlice = c;
        desc.aspect = Rendering::TextureAspect::Depth;
        desc.debugName = "ShadowCascadeRaw";
        uint32_t idx = rs.Textures().GetBindlessIndex(desc);
        if (idx == 0u)
            return nullptr;
        entry.Indices[c] = idx;
    }

    auto [insIt, inserted] = m_PcssBindlessByView.emplace(viewId, entry);
    return &insIt->second;
}

const ShadowMapRenderFeature::PcssBindlessIndices*
ShadowMapRenderFeature::GetPcssBindlessIndices(Rendering::ViewId viewId) const
{
    auto it = m_PcssBindlessByView.find(viewId);
    return (it != m_PcssBindlessByView.end()) ? &it->second : nullptr;
}

const ShadowMapRenderFeature::PyramidBindlessIndices*
ShadowMapRenderFeature::EnsurePyramidBindlessTextures(
    Rendering::ViewId viewId, RenderServices& rs,
    const ShadowMinMaxPyramid::FrameState& pyramid)
{
    if (!rs.Textures().IsBindlessEnabled())
        return nullptr;

    // Release BEFORE the liveness bail, so the slots are also dropped on the
    // frames where the pyramid stopped existing rather than merely moved.
    // Dropping the index map alone would park nothing: the slot would never be
    // reissued, its descriptor would keep pointing at a view of an image the
    // pool is about to free, and the id-keyed bindless cache would hand the
    // stale index straight back to the re-registration below whenever the pool
    // reissues the handle id.
    auto it = m_PyramidBindlessByView.find(viewId);
    if (it != m_PyramidBindlessByView.end() && it->second.Physical != pyramid.Physical)
    {
        rs.Textures().InvalidateBindless(it->second.Physical);
        m_PyramidBindlessByView.erase(it);
        it = m_PyramidBindlessByView.end();
    }

    if (!pyramid.Physical.IsValid() || pyramid.Levels <= 0 || pyramid.Layers == 0u)
        return nullptr;

    if (it != m_PyramidBindlessByView.end())
        return &it->second;

    // One Texture2D per cascade layer, spanning EVERY mip level. mipCount = 0 is
    // "all remaining": the early-out picks a level by penumbra radius and reads
    // it with texelFetch(.., lod), which a single-level view cannot serve. The
    // loop is bounded by the pyramid's own layer count, not the view's cascade
    // count — a view built past the last layer fails to create and would silently
    // fall back to the whole-array view.
    PyramidBindlessIndices entry{};
    entry.Physical = pyramid.Physical;
    for (uint32_t c = 0; c < pyramid.Layers && c < kMaxShadowCascades; ++c)
    {
        Rendering::BindlessTextureDesc desc{};
        desc.textureHandle = pyramid.Physical;
        desc.type = Rendering::BindlessResourceType::Texture2D;
        desc.mipCount = 0;
        desc.arraySlice = c;
        desc.debugName = "ShadowMinMaxPyramid";
        const uint32_t idx = rs.Textures().GetBindlessIndex(desc);
        if (idx == 0u)
            return nullptr;
        entry.Indices[c] = idx;
    }

    auto [insIt, inserted] = m_PyramidBindlessByView.emplace(viewId, entry);
    return &insIt->second;
}

void ShadowMapRenderFeature::ComputeCascadeWorldAABB(
    const Mathematics::Matrix4x4& lightVP,
    Mathematics::Vector3& outMin,
    Mathematics::Vector3& outMax)
{
    // Reverse-Z NDC corners: near plane is z=1, far is z=0; XY is the
    // canonical [-1,1] square. 8 corners of the ortho box in NDC.
    static const Mathematics::Vector4 ndcCorners[8] = {
        Mathematics::Vector4{-1.0f, -1.0f, 1.0f, 1.0f},
        Mathematics::Vector4{ 1.0f, -1.0f, 1.0f, 1.0f},
        Mathematics::Vector4{ 1.0f,  1.0f, 1.0f, 1.0f},
        Mathematics::Vector4{-1.0f,  1.0f, 1.0f, 1.0f},
        Mathematics::Vector4{-1.0f, -1.0f, 0.0f, 1.0f},
        Mathematics::Vector4{ 1.0f, -1.0f, 0.0f, 1.0f},
        Mathematics::Vector4{ 1.0f,  1.0f, 0.0f, 1.0f},
        Mathematics::Vector4{-1.0f,  1.0f, 0.0f, 1.0f},
    };

    const Mathematics::Matrix4x4 invVP = Mathematics::Inverse(lightVP);
    const float kInf = std::numeric_limits<float>::infinity();
    outMin = Mathematics::Vector3{ kInf,  kInf,  kInf};
    outMax = Mathematics::Vector3{-kInf, -kInf, -kInf};
    for (const Mathematics::Vector4& ndc : ndcCorners)
    {
        const Mathematics::Vector4 w = invVP.Transform(ndc);
        if (std::fabs(w.w) < 1e-12f)
            continue;
        const float invW = 1.0f / w.w;
        const Mathematics::Vector3 p{w.x * invW, w.y * invW, w.z * invW};
        outMin.x = std::min(outMin.x, p.x);
        outMin.y = std::min(outMin.y, p.y);
        outMin.z = std::min(outMin.z, p.z);
        outMax.x = std::max(outMax.x, p.x);
        outMax.y = std::max(outMax.y, p.y);
        outMax.z = std::max(outMax.z, p.z);
    }
}

bool ShadowMapRenderFeature::AABBsIntersect(
    const Mathematics::Vector3& aMin,
    const Mathematics::Vector3& aMax,
    const Mathematics::Vector3& bMin,
    const Mathematics::Vector3& bMax)
{
    return aMin.x <= bMax.x && aMax.x >= bMin.x
        && aMin.y <= bMax.y && aMax.y >= bMin.y
        && aMin.z <= bMax.z && aMax.z >= bMin.z;
}

void ShadowMapRenderFeature::ComputeExtendedSliceCornersWS(
    const Matrix4x4& cameraViewProj, float nearPlane, float farPlane,
    float sliceNearDist, float sliceFarDist, Vector3 outCorners[8])
{
    const Matrix4x4 invViewProj = Mathematics::Inverse(cameraViewProj);
    Vector3 fullCorners[8];
    ExtractFrustumCornersWS(invViewProj, fullCorners);

    // Mirror ComputeCascades' slice interpolation exactly (the corners the
    // cascade was actually fit to). Fractions are normalized against the full
    // camera range; cascade 0's near fraction collapses to 0 when the caller
    // passes nearPlane as sliceNearDist.
    const float frustumRange = std::max(farPlane - nearPlane, 1e-4f);
    float cascadeNear = (sliceNearDist - nearPlane) / frustumRange;
    float cascadeFar = (sliceFarDist - nearPlane) / frustumRange;

    const float sliceFrac = cascadeFar - cascadeNear;
    const float overlap = sliceFrac * kCascadeOverlapFraction;
    cascadeNear = std::max(0.0f, cascadeNear - overlap);
    cascadeFar = std::min(1.0f, cascadeFar + overlap);

    for (int i = 0; i < 4; ++i)
    {
        const Vector3 nearPt = fullCorners[i];
        const Vector3 farPt = fullCorners[i + 4];
        const Vector3 delta = farPt - nearPt;
        outCorners[i] = nearPt + delta * cascadeNear;
        outCorners[i + 4] = nearPt + delta * cascadeFar;
    }
}

void ShadowMapRenderFeature::BuildCascadeCasterCullPlanes(
    const Matrix4x4& lightVP, Vector4 outPlanes[6])
{
    Rendering::ExtractFrustumPlanes(lightVP, outPlanes);
    // Retire the near plane (index 4 — reverse-Z near is row3 - row2). Mirror of
    // the degenerate plane used to kill a cascade outright: w = -1e38f makes every
    // sphere test fail, so w = +1e38f makes every sphere test pass. The unit
    // normal is kept so any downstream renormalization is a no-op.
    //
    // See the header for why a directional cascade's near plane must not gate the
    // caster set, and why DepthClampEnable on the depth pass is the other half.
    constexpr float kAlwaysPassW = 1.0e38f;
    outPlanes[4] = Vector4{1.0f, 0.0f, 0.0f, kAlwaysPassW};
}

void ShadowMapRenderFeature::TightenCascadeSidePlanes(const Matrix4x4& lightVP,
                                                      const float footprintNdc[4],
                                                      float orthoHalfExtent, float slackWorld,
                                                      Vector4 planes[6])
{
    // Under an orthographic light, NDC.xy is an affine function of light-space
    // XY and independent of depth, so the footprint bounds the receivers it was
    // built from at every depth — the zero-false-cull core (correctness F9c).
    float xMin = footprintNdc[0];
    float yMin = footprintNdc[1];
    float xMax = footprintNdc[2];
    float yMax = footprintNdc[3];

    // Inflate by the world-space slack (NDC ±1 spans ±orthoHalfExtent world
    // units on both axes for the square ortho), then intersect with the
    // existing ortho extent so the tightened planes are never looser than the
    // original [-1, 1] side planes (can only cull more, never break near/far).
    if (!(orthoHalfExtent > 1e-4f))
        return;
    const float slackNdc = slackWorld / orthoHalfExtent;
    xMin = std::max(-1.0f, xMin - slackNdc); xMax = std::min(1.0f, xMax + slackNdc);
    yMin = std::max(-1.0f, yMin - slackNdc); yMax = std::min(1.0f, yMax + slackNdc);
    if (xMin >= xMax || yMin >= yMax)
        return; // collapsed footprint — keep the full frustum (never over-cull).

    // Rebuild the four side planes from the VP rows at the footprint bounds,
    // generalizing ExtractFrustumPlanes' (row3 ± row{0,1}) for the [-1, 1] box
    // to arbitrary [xMin, xMax] x [yMin, yMax]:
    //   left   (ndc.x >= xMin): row0 - xMin*row3
    //   right  (ndc.x <= xMax): xMax*row3 - row0
    //   bottom (ndc.y >= yMin): row1 - yMin*row3
    //   top    (ndc.y <= yMax): yMax*row3 - row1
    const glm::mat4& m = lightVP.GetGLM();
    const glm::vec4 r0 = glm::row(m, 0);
    const glm::vec4 r1 = glm::row(m, 1);
    const glm::vec4 r3 = glm::row(m, 3);
    auto storeNormalized = [&planes](int idx, const glm::vec4& v)
    {
        const float len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
        const float inv = len > 0.0f ? 1.0f / len : 1.0f;
        planes[idx] = Vector4(v.x * inv, v.y * inv, v.z * inv, v.w * inv);
    };
    storeNormalized(0, r0 - xMin * r3);
    storeNormalized(1, xMax * r3 - r0);
    storeNormalized(2, r1 - yMin * r3);
    storeNormalized(3, yMax * r3 - r1);
    // planes[4] (near) and planes[5] (far) are left as extracted — the depth
    // extent and the light back-extension are unchanged.
}

void ShadowMapRenderFeature::ComputeClampedLightSpaceSphere(
    const Matrix4x4& lightRot, const Vector3 cornersRel[8], const SceneBoundsRel* sceneBoundsRel,
    float& outRadius, float& outCenterXLS, float& outCenterYLS)
{
    const bool clamp = sceneBoundsRel != nullptr &&
                       sceneBoundsRel->Min.x <= sceneBoundsRel->Max.x &&
                       sceneBoundsRel->Min.y <= sceneBoundsRel->Max.y &&
                       sceneBoundsRel->Min.z <= sceneBoundsRel->Max.z;

    Vector3 ls[8];
    Vector3 centre{0.0f, 0.0f, 0.0f};
    for (int i = 0; i < 8; ++i)
    {
        Vector3 c = cornersRel[i];
        if (clamp)
        {
            c.x = std::clamp(c.x, sceneBoundsRel->Min.x, sceneBoundsRel->Max.x);
            c.y = std::clamp(c.y, sceneBoundsRel->Min.y, sceneBoundsRel->Max.y);
            c.z = std::clamp(c.z, sceneBoundsRel->Min.z, sceneBoundsRel->Max.z);
        }
        const Vector4 p = lightRot.Transform(Vector4{c.x, c.y, c.z, 1.0f});
        ls[i] = Vector3{p.x, p.y, p.z};
        centre = Vector3{centre.x + p.x, centre.y + p.y, centre.z + p.z};
    }
    centre = Vector3{centre.x * 0.125f, centre.y * 0.125f, centre.z * 0.125f};

    // FULL 3D radius, deliberately — not the radius of the XY projection.
    //
    // Only the 3D radius is rotation-invariant. The corner set is rigid, so
    // distances within it are fixed no matter where the camera looks. The XY
    // projection of that set is NOT: projecting a rotating 3D offset onto the
    // light's fixed XY plane changes its length, which is the same reason a
    // light-space AABB breathes. Using a 3D radius to bound a 2D extent is what
    // the invariance costs — measured at ~19% coarser texels than the mean AABB,
    // ~9% against the AABB's own worst case after the band snap rounds up.
    float r2 = 0.0f;
    for (const Vector3& p : ls)
    {
        const float dx = p.x - centre.x;
        const float dy = p.y - centre.y;
        const float dz = p.z - centre.z;
        r2 = std::max(r2, dx * dx + dy * dy + dz * dz);
    }
    outRadius = std::sqrt(r2);
    outCenterXLS = centre.x;
    outCenterYLS = centre.y;
}

void ShadowMapRenderFeature::ComputeClampedLightSpaceBounds(
    const Matrix4x4& lightRot, const Vector3 cornersRel[8], const SceneBoundsRel* sceneBoundsRel,
    Vector3& outMinLS, Vector3& outMaxLS)
{
    // A degenerate scene box would clamp every corner onto a point, so treat it
    // as "no bounds" rather than over-tightening. Never under-covers.
    const bool clamp = sceneBoundsRel != nullptr &&
                       sceneBoundsRel->Min.x <= sceneBoundsRel->Max.x &&
                       sceneBoundsRel->Min.y <= sceneBoundsRel->Max.y &&
                       sceneBoundsRel->Min.z <= sceneBoundsRel->Max.z;

    outMinLS = Vector3{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                       std::numeric_limits<float>::max()};
    outMaxLS = Vector3{std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
                       std::numeric_limits<float>::lowest()};

    for (int i = 0; i < 8; ++i)
    {
        Vector3 c = cornersRel[i];
        if (clamp)
        {
            // Clamp in WORLD space, before the light rotation. The constraint
            // being expressed is "the slice reaches below the ground", which is
            // a world-axis fact; after projection onto the light direction the
            // scene's lateral span swamps it and the intersection is a no-op.
            c.x = std::clamp(c.x, sceneBoundsRel->Min.x, sceneBoundsRel->Max.x);
            c.y = std::clamp(c.y, sceneBoundsRel->Min.y, sceneBoundsRel->Max.y);
            c.z = std::clamp(c.z, sceneBoundsRel->Min.z, sceneBoundsRel->Max.z);
        }
        const Vector4 ls = lightRot.Transform(Vector4{c.x, c.y, c.z, 1.0f});
        outMinLS.x = std::min(outMinLS.x, ls.x); outMaxLS.x = std::max(outMaxLS.x, ls.x);
        outMinLS.y = std::min(outMinLS.y, ls.y); outMaxLS.y = std::max(outMaxLS.y, ls.y);
        outMinLS.z = std::min(outMinLS.z, ls.z); outMaxLS.z = std::max(outMaxLS.z, ls.z);
    }
}

void ShadowMapRenderFeature::OnScheduleCulling(const FeatureCullingContext& ctx)
{
    if (ctx.CullingPipeline == nullptr || ctx.Views == nullptr || ctx.Cameras == nullptr)
        return;

    // Snapshot the scene's world-space AABB once per frame. Cascades whose
    // light-space frustum doesn't overlap any live geometry are represented
    // by degenerate frustum planes so every sphere test fails and the
    // slice ends up all-zero — equivalent to the old "skip this dispatch"
    // path but inside the fused cascade-group dispatch.
    Mathematics::Vector3 sceneMin{}, sceneMax{};
    const bool hasSceneBounds =
        ctx.Scene != nullptr && ctx.Scene->GetInstancesWorldBounds(sceneMin, sceneMax);

    // Caster-set reduction (default ON; GE_SHADOW_CASTER_REDUCTION=0 is the
    // kill switch). Read once so the branch is stable for the process lifetime;
    // the =0 lane leaves the planes verbatim.
    const bool casterReduction = IsCasterReductionEnabled();

    auto findCamera = [ctx](Rendering::CameraId id) -> const Rendering::CameraData*
    {
        for (const Rendering::CameraInfo& cam : *ctx.Cameras)
        {
            if (cam.id == id)
                return &cam.data;
        }
        return nullptr;
    };

    for (const Rendering::ViewDesc& view : *ctx.Views)
    {
        // ActiveRenderLayerMask() (not the raw renderLayerMask) so an OnDemand
        // view that has lapsed out of participation this frame drops its shadow
        // cascade culling too — matching the per-view frustum/HZB loop and the
        // area/spot/point shadow gates. Without this a dormant scene view (e.g.
        // a hidden editor quad pane) keeps dispatching fused-cascade culling.
        if (view.cameraId == 0 || view.ActiveRenderLayerMask() == 0u)
            continue;

        const Rendering::CameraData* camData = findCamera(view.cameraId);
        if (camData == nullptr)
            continue;

        // This frame's fit, computed here so the casters are culled to the
        // same fit the cascades rasterize with (the ShadowMap node declares
        // from it). Without services, the last declared fit.
        const CascadeFrameData* frameData = ctx.Services
                                                ? FitViewCascadesForCulling(*ctx.Services, view, *camData)
                                                : GetCachedFrameData(view.id);
        if (frameData == nullptr || frameData->NumCascades == 0)
            continue;

        const Rendering::CameraDerivedData camera = Rendering::DeriveCameraData(*camData);

        // One SubmitCascadeGroup per shadow view: the kViewCount=NumCascades
        // PSO variant fans the candidate range into all cascade slices in
        // one dispatch.
        Rendering::CascadeCullingGroup group{};
        group.viewId          = view.id;
        group.cascadeCount    = std::min<uint32_t>(frameData->NumCascades,
                                                   Rendering::kMaxCullingViewsPerDispatch);
        group.cameraPosition  = camera.Position;
        group.cameraForward   = camera.Forward;
        // Camera-relative shadow-caster culling (Earth-scale precision): the same
        // render origin the world pass + cascade VPs use, derived from the camera
        // position (the stored renderOriginSector is zero here — it is filled only
        // on-read by ResolveCameraData), so a planetary caster's boundingCenter
        // differences small against the (world-space) cascade planes. Inactive
        // (sector 0) => (0,0,0) => byte-identical world cull.
        float originMagnitude = 0.0f;
        {
            const auto originSector = ComputeRenderOriginSector(
                camData->cameraPos[0], camData->cameraPos[1], camData->cameraPos[2]);
            float ox, oy, oz;
            SectorToWorld(originSector, ox, oy, oz);
            group.cameraRelativeOrigin = Vector3{ox, oy, oz};
            originMagnitude = std::max({std::abs(ox), std::abs(oy), std::abs(oz)});
        }
        // The world-space planes carry ULP(|origin|) (see kCullPrecisionUlps).
        const float precisionSlack =
            originMagnitude * static_cast<float>(kCullPrecisionUlps) * std::numeric_limits<float>::epsilon();
        group.nearPlane       = camera.NearPlane;
        group.farPlane        = camera.FarPlane;
        group.firstInstance   = 0;
        group.instanceCount   = ctx.InstanceCount;
        group.renderLayerMask = view.ActiveRenderLayerMask();
        group.frameIndex      = ctx.FrameIndex;
        group.deltaTime       = ctx.DeltaTime;

        // Degenerate plane: any sphere test of the form
        //   dot(center, normal) + w < -radius
        // is trivially true for w = -1e38f, so TestSphereFrustum returns false
        // on the very first plane and the cascade culls every instance.
        constexpr float kDegenerateW = -1.0e38f;
        const Mathematics::Vector4 kDegeneratePlane{1.0f, 0.0f, 0.0f, kDegenerateW};

        bool anyLiveCascade = false;
        for (uint32_t c = 0; c < group.cascadeCount; ++c)
        {
            bool cascadeLive = true;
            if (hasSceneBounds)
            {
                Mathematics::Vector3 cascadeMin{}, cascadeMax{};
                ComputeCascadeWorldAABB(frameData->LightVP[c], cascadeMin, cascadeMax);
                cascadeLive = AABBsIntersect(sceneMin, sceneMax, cascadeMin, cascadeMax);
            }

            group.lightVP[c] = frameData->LightVP[c];

            if (cascadeLive)
            {
                Mathematics::Vector4 planes[6]{};
                BuildCascadeCasterCullPlanes(frameData->LightVP[c], planes);

                // Tighten the four side planes to the cascade's caster footprint
                // (CascadeFrameData::CasterFootprint): the receivers it was fitted
                // to hold. Far (plane 5) is the fit's own; near (plane 4) was
                // retired above. The fit is this frame's, so the slack only
                // covers the footprint drift the fit freeze allows and the
                // world-space planes' precision.
                if (casterReduction && frameData->CasterFootprintApplies[c])
                {
                    const float freezeDrift =
                        m_FitFreezeEnabled ? kFreezeFootprintDriftFraction * frameData->OrthoHalfExtent[c]
                                           : 0.0f;
                    TightenCascadeSidePlanes(frameData->LightVP[c], frameData->CasterFootprint[c],
                                             frameData->OrthoHalfExtent[c],
                                             freezeDrift + precisionSlack, planes);
                }

                for (int p = 0; p < 6; ++p)
                    group.frustumPlanes[c][p] = planes[p];
                anyLiveCascade = true;
            }
            else
            {
                for (int p = 0; p < 6; ++p)
                    group.frustumPlanes[c][p] = kDegeneratePlane;
            }
        }

        // If EVERY cascade is empty, skip the group entirely — there is no
        // slice the bucketer / depth pass could meaningfully consume.
        if (!anyLiveCascade)
            continue;

        ctx.CullingPipeline->SubmitCascadeGroup(group);
    }
}

} // namespace Engine::Renderer
} // namespace GameEngine
