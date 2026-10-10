#include "Engine/Rendering/SceneReflectionProbeEnvironmentSource.h"

#include "Engine/Rendering/ImageBasedLightingFeature.h"
#include "Engine/Rendering/IRenderFeature.h"
#include "Engine/Rendering/RenderOrigin.h"
#include "Engine/Rendering/RenderServices.h"
#include "RenderServicesDetail.h"

#include "Logger/Logger.h"
#include "Mathematics/Matrix4x4.h"
#include "Mathematics/MatrixOps.h"
#include "Mathematics/Vector3.h"
#include "Rendering/CameraDerivation.h"
#include "Rendering/Common/Frustum.h"
#include "Rendering/Common/Utils.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/GPUCulling.h"
#include "Rendering/Core/PassPhase.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Core/SpecializationConstants.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace GameEngine
{
namespace Engine::Renderer
{
using namespace GameEngine::Rendering;

namespace
{
// Probe-prefixed to stay distinct from SkyEnvironmentSource.cpp's anonymous-namespace
// twins when a unity build merges both files into one translation unit.
constexpr uint32_t kProbeConvolveGroup = 8;
constexpr float kHalfPi = 1.57079632679489661923f;
constexpr float kFallbackDeltaTimeSeconds = 1.0f / 60.0f;
constexpr float kCaptureNearPlane = 0.005f;
constexpr uint32_t kAllFacesMask = (1u << ImageBasedLightingFeature::kNumCaptureFaces) - 1u;
static_assert(ImageBasedLightingFeature::kNumCaptureFaces == kProbeFaceCount,
              "the probe-face cull index block is sized to the capture cube's face count");

struct ProbeCubeFaceBasis
{
    float up[3];
    float forward[3];
};

constexpr ProbeCubeFaceBasis kProbeCaptureFaces[ImageBasedLightingFeature::kNumCaptureFaces] = {
    {{0.0f, 1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}},
    {{0.0f, 1.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}},
    {{0.0f, 0.0f, -1.0f}, {0.0f, 1.0f, 0.0f}},
    {{0.0f, 0.0f, 1.0f}, {0.0f, -1.0f, 0.0f}},
    {{0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}},
    {{0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, -1.0f}},
};


void HashBytes(uint64_t& h, const void* data, size_t bytes)
{
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < bytes; ++i)
    {
        h ^= p[i];
        h *= 1099511628211ull;
    }
}

uint64_t MixDigest(uint64_t base, uint32_t generation)
{
    uint64_t h = base != 0 ? base : 14695981039346656037ull;
    HashBytes(h, &generation, sizeof(generation));
    return h != 0 ? h : 1;
}
} // namespace

SceneReflectionProbeEnvironmentSource::SceneReflectionProbeEnvironmentSource(RenderServices& services)
    : m_Services(&services),
      m_SkyBackground(services)
{
}

SceneReflectionProbeEnvironmentSource::~SceneReflectionProbeEnvironmentSource()
{
    if (!m_Services)
        return;

    if (m_ViewId != 0)
    {
        m_Services->Views().ReleaseView(m_ViewId);
        m_ViewId = 0;
    }
    if (m_CameraId != 0)
    {
        m_Services->Views().ReleaseCamera(m_CameraId);
        m_CameraId = 0;
    }
}

void SceneReflectionProbeEnvironmentSource::SetProbe(const SceneReflectionProbeEnvironmentDesc& desc)
{
    const bool worldChanged = desc.WorldId != m_Desc.WorldId;
    m_Desc = desc;
    // Keep the sky background's probe overrides current from frame one:
    // BaseDigest() hashes m_SkyBackground.InputDigest(), whose effective
    // settings fold these in — pushing them only at capture time would shift
    // the digest after the first bake and schedule one spurious rebake.
    SkyEnvironmentProbeOverrides skyOverrides{};
    skyOverrides.Intensity = 1.0f;
    skyOverrides.ExposureEV = m_Desc.ExposureEV;
    skyOverrides.RotationRadians = m_Desc.RotationRadians;
    skyOverrides.LowerHemisphereDarkness = m_Desc.LowerHemisphereDarkness;
    m_SkyBackground.SetProbeOverrides(skyOverrides);
    EnsureViews();

    // Called every frame by ReflectionProbeSystem. A world retarget mid-cycle
    // re-arms: the views must wait for the new world's extraction before any
    // bake.
    if (worldChanged && m_CaptureState != CaptureState::Idle)
        ArmViews();
}

bool SceneReflectionProbeEnvironmentSource::EnsureViews()
{
    if (m_ViewsAllocated)
        return true;
    if (!m_Services)
        return false;

    m_CameraId = m_Services->Views().AllocateCamera("Scene Reflection Probe Camera");
    // OnDemand: the view costs nothing until a bake requests it. One view
    // serves all six faces; the camera is re-pointed per face at declaration.
    m_ViewId = m_Services->Views().AllocateView("Scene Reflection Probe", m_CameraId,
                                                Rendering::ViewPurpose::UtilityCapture,
                                                Rendering::ViewParticipation::OnDemand);
    m_Services->Views().SetViewActiveRenderPipeline(m_ViewId, false);
    m_Services->Views().SetViewRenderLayerMask(m_ViewId, m_Desc.CullMask);
    m_Services->Views().SetViewWorldId(m_ViewId, m_Desc.WorldId);

    m_ViewsAllocated = true;
    return true;
}

void SceneReflectionProbeEnvironmentSource::ArmFaces(uint32_t faceMask)
{
    if (!m_Services || !m_ViewsAllocated || m_ViewId == 0)
        return;
    m_Services->Views().RequestViewFrame(m_ViewId);
    m_Services->Views().SetViewRenderLayerMask(m_ViewId, m_Desc.CullMask);
    m_Services->Views().SetViewWorldId(m_ViewId, m_Desc.WorldId);
    m_ArmedFaceMask = faceMask & kAllFacesMask;
}

void SceneReflectionProbeEnvironmentSource::ArmViews()
{
    ArmFaces(kAllFacesMask);
    m_CaptureState = CaptureState::Arming;
}

void SceneReflectionProbeEnvironmentSource::ScheduleCulling(const FeatureCullingContext& ctx)
{
    using GameEngine::Mathematics::Vector3;

    // Only the frame(s) the arm requested: the view is fed by extraction then,
    // and the bake that consumes these slices declares later this frame.
    if (!ctx.CullingPipeline || !ctx.Views || m_ViewId == 0 || m_ArmedFaceMask == 0 ||
        m_CaptureState == CaptureState::Idle || ctx.InstanceCount == 0)
        return;
    const Rendering::ViewDesc* view = nullptr;
    for (const Rendering::ViewDesc& v : *ctx.Views)
    {
        if (v.id == m_ViewId)
        {
            view = &v;
            break;
        }
    }
    if (!view || view->ActiveRenderLayerMask() == 0u)
        return;

    // Every face shares the eye and the projection, so one derived camera
    // describes the group; only the frustum planes differ per face. The
    // camera-relative origin is derived from the eye exactly as the spine
    // does for its per-view dispatch (see ScheduleViewCullingDispatches).
    const Rendering::CameraDerivedData shared = Rendering::DeriveCameraData(MakeFaceCamera(0));
    Vector3 origin{};
    {
        const auto originSector =
            ComputeRenderOriginSector(m_Desc.Position[0], m_Desc.Position[1], m_Desc.Position[2]);
        SectorToWorld(originSector, origin.x, origin.y, origin.z);
    }

    // Armed faces pack into runs of consecutive faces, at most
    // kMaxCullingViewsPerDispatch per group: slice c of a run started at face
    // f publishes at ProbeFaceCullingIndex(f + c). All six faces = {0-3, 4-5}.
    uint32_t face = 0;
    while (face < ImageBasedLightingFeature::kNumCaptureFaces)
    {
        if ((m_ArmedFaceMask & (1u << face)) == 0u)
        {
            ++face;
            continue;
        }
        Rendering::CascadeCullingGroup group{};
        group.viewId = m_ViewId;
        group.cascadeIndexBase = ProbeFaceCullingIndex(face);
        group.shadowCasterDispatch = false; // color capture: keep non-casters
        group.cameraPosition = shared.Position;
        group.cameraForward = shared.Forward;
        group.nearPlane = shared.NearPlane;
        group.farPlane = shared.FarPlane;
        group.cameraRelativeOrigin = origin;
        group.firstInstance = 0;
        group.instanceCount = ctx.InstanceCount;
        group.renderLayerMask = view->ActiveRenderLayerMask();
        group.frameIndex = ctx.FrameIndex;
        group.deltaTime = ctx.DeltaTime;
        uint32_t count = 0;
        while (face < ImageBasedLightingFeature::kNumCaptureFaces &&
               count < Rendering::kMaxCullingViewsPerDispatch &&
               (m_ArmedFaceMask & (1u << face)) != 0u)
        {
            const Rendering::CameraDerivedData faceCamera =
                Rendering::DeriveCameraData(MakeFaceCamera(face));
            group.lightVP[count] = faceCamera.ViewProjMatrix;
            Rendering::ExtractFrustumPlanes(faceCamera.ViewProjMatrix, group.frustumPlanes[count]);
            ++count;
            ++face;
        }
        group.cascadeCount = count;
        ctx.CullingPipeline->SubmitCascadeGroup(group);
    }
}

uint64_t SceneReflectionProbeEnvironmentSource::BaseDigest() const
{
    if (!HasActiveContent())
        return 0;

    uint64_t h = 14695981039346656037ull;
    constexpr char kTag[] = "SceneReflectionProbeEnvironmentSource";
    HashBytes(h, kTag, sizeof(kTag) - 1);
    HashBytes(h, &m_Desc.SourceKey, sizeof(m_Desc.SourceKey));
    HashBytes(h, &m_Desc.WorldId, sizeof(m_Desc.WorldId));
    HashBytes(h, m_Desc.Position, sizeof(m_Desc.Position));
    HashBytes(h, &m_Desc.InfluenceRadius, sizeof(m_Desc.InfluenceRadius));
    HashBytes(h, &m_Desc.BoxProjection, sizeof(m_Desc.BoxProjection));
    HashBytes(h, m_Desc.BoxCenter, sizeof(m_Desc.BoxCenter));
    HashBytes(h, m_Desc.BoxHalfExtents, sizeof(m_Desc.BoxHalfExtents));
    HashBytes(h, m_Desc.BoxAxisX, sizeof(m_Desc.BoxAxisX));
    HashBytes(h, m_Desc.BoxAxisY, sizeof(m_Desc.BoxAxisY));
    HashBytes(h, m_Desc.BoxAxisZ, sizeof(m_Desc.BoxAxisZ));
    HashBytes(h, &m_Desc.MaxDistance, sizeof(m_Desc.MaxDistance));
    HashBytes(h, &m_Desc.CullMask, sizeof(m_Desc.CullMask));
    HashBytes(h, &m_Desc.ExposureEV, sizeof(m_Desc.ExposureEV));
    HashBytes(h, &m_Desc.RotationRadians, sizeof(m_Desc.RotationRadians));
    HashBytes(h, &m_Desc.CaptureResolution, sizeof(m_Desc.CaptureResolution));
    HashBytes(h, &m_Desc.UpdateMode, sizeof(m_Desc.UpdateMode));
    HashBytes(h, &m_Desc.RealtimeUpdateInterval, sizeof(m_Desc.RealtimeUpdateInterval));
    // The capture's background is the live sky rendered through m_SkyBackground,
    // so the sky's bake inputs are this probe's bake inputs too: a time-of-day or
    // atmosphere edit invalidates the captured cube. Without this, a probe keeps
    // serving the old sky's lighting until some probe field happens to change
    // ("Once" means not continuous, not immune to a changed environment).
    const uint64_t skyDigest = m_SkyBackground.InputDigest();
    HashBytes(h, &skyDigest, sizeof(skyDigest));
    // ALSO hash the sky state the sky-view LUT actually CONTAINS: a bake fired
    // on the change frame can sample the not-yet-recomputed LUT (the recompute
    // may land a frame later or in another window's stream) and silently keep
    // the previous sky. When the LUT catches up this digest moves again and
    // the corrective rebake fires — convergence instead of a stale capture.
    const uint64_t lutDigest = m_SkyBackground.MaterializedLutDigest();
    HashBytes(h, &lutDigest, sizeof(lutDigest));
    // The capture photographs the world's CONTENT too: fold in the extraction
    // pass's renderable-membership digest so adding, removing, hiding, or
    // re-assigning the material of scene geometry recaptures a "Once" probe,
    // the same way a sky edit does. Membership only — transforms and material
    // PARAMETER edits are excluded upstream, so moving objects don't thrash
    // rebakes (and a tweaked albedo doesn't recapture until something else
    // does).
    const uint64_t contentDigest = m_Services
        ? m_Services->GetWorldRenderContentDigest(m_Desc.WorldId)
        : 0;
    HashBytes(h, &contentDigest, sizeof(contentDigest));
    return h != 0 ? h : 1;
}

uint64_t SceneReflectionProbeEnvironmentSource::InputDigest() const
{
    return MixDigest(BaseDigest(), m_CaptureGeneration);
}

SceneProbeWorldEpochs SceneReflectionProbeEnvironmentSource::WorldEpochs() const
{
    // World-scoped, not range-scoped: a change anywhere in the probe's world
    // counts (SceneProbeWorldEpochs lists what moves them and what does not).
    return m_Services ? SceneProbeWorldEpochs::Read(*m_Services, m_Desc.WorldId) : SceneProbeWorldEpochs{};
}

void SceneReflectionProbeEnvironmentSource::TickBakeClock(float deltaTimeSeconds)
{
    m_Schedule.Tick(deltaTimeSeconds > 0.0f ? deltaTimeSeconds : kFallbackDeltaTimeSeconds);
}

float SceneReflectionProbeEnvironmentSource::RealtimeInterval() const
{
    return m_Desc.UpdateMode == SceneReflectionProbeUpdateMode::Realtime
               ? std::max(0.0f, m_Desc.RealtimeUpdateInterval)
               : -1.0f;
}

bool SceneReflectionProbeEnvironmentSource::BakeDue() const
{
    return m_Schedule.Due(BaseDigest(), WorldEpochs(), RealtimeInterval());
}

bool SceneReflectionProbeEnvironmentSource::CapturesEveryFrame() const
{
    return m_Desc.UpdateMode == SceneReflectionProbeUpdateMode::Realtime &&
           m_Desc.RealtimeUpdateInterval <= 0.0f;
}

uint64_t SceneReflectionProbeEnvironmentSource::NextBakeDigest()
{
    const uint64_t base = BaseDigest();
    if (base == 0)
        return 0;

    if (BakeDue())
    {
        // A capture photographs the SCENE, and the scene is still lit by the
        // PREVIOUS bake's cubes — one bake after an environment change is one
        // step of a fixed-point iteration, not the answer (live-observed: a
        // noon->midnight switch needs ~3 bakes before the water stops carrying
        // noon ambient). The schedule owes convergence bakes after a change:
        // Once probes take them spaced by the feature's rebake throttle,
        // realtime probes by their interval.
        m_Schedule.ConsumeBake(base, WorldEpochs(),
                               m_Desc.UpdateMode == SceneReflectionProbeUpdateMode::Realtime);
        ++m_CaptureGeneration;
        if (m_CaptureGeneration == 0)
            m_CaptureGeneration = 1;
    }

    return MixDigest(base, m_CaptureGeneration);
}

EnvironmentLocalReflectionData SceneReflectionProbeEnvironmentSource::LocalReflectionData() const
{
    EnvironmentLocalReflectionData data{};
    const bool validBox = m_Desc.BoxProjection &&
                          m_Desc.BoxHalfExtents[0] > 0.0001f &&
                          m_Desc.BoxHalfExtents[1] > 0.0001f &&
                          m_Desc.BoxHalfExtents[2] > 0.0001f;
    if (!validBox)
        return data;

    data.BoxProjection = true;
    std::memcpy(data.ProbePositionWS, m_Desc.Position, sizeof(data.ProbePositionWS));
    std::memcpy(data.BoxCenterWS, m_Desc.BoxCenter, sizeof(data.BoxCenterWS));
    std::memcpy(data.BoxHalfExtentsWS, m_Desc.BoxHalfExtents, sizeof(data.BoxHalfExtentsWS));
    std::memcpy(data.BoxAxisXWS, m_Desc.BoxAxisX, sizeof(data.BoxAxisXWS));
    std::memcpy(data.BoxAxisYWS, m_Desc.BoxAxisY, sizeof(data.BoxAxisYWS));
    std::memcpy(data.BoxAxisZWS, m_Desc.BoxAxisZ, sizeof(data.BoxAxisZWS));
    return data;
}

bool SceneReflectionProbeEnvironmentSource::EnsurePipelines(IDevice& device,
                                                            const ImageBasedLightingFeature& feature)
{
    const uint32_t captureResolution = feature.GetEnvCaptureSize();
    const uint32_t prefilterMipCount = feature.GetPrefilterMipCount();
    if (m_PipelinesReady &&
        m_PipelineCaptureResolution == captureResolution &&
        m_PipelinePrefilterMipCount == prefilterMipCount)
        return true;

    std::vector<uint8_t> csDownsample = Utils::LoadShaderFile("Shaders/sky_env_mip_downsample.comp.spv");
    std::vector<uint8_t> csDiffuse = Utils::LoadShaderFile("Shaders/sky_diffuse_convolve.comp.spv");
    std::vector<uint8_t> csSpecular = Utils::LoadShaderFile("Shaders/sky_specular_prefilter.comp.spv");
    if (csDownsample.empty() || csDiffuse.empty() || csSpecular.empty())
    {
        static bool warned = false;
        if (!warned)
        {
            warned = true;
            Logger::Log::Warning("SceneReflectionProbeEnvironmentSource: missing bake shader SPIR-V; IBL will keep the current fallback");
        }
        return false;
    }

    const DescriptorSetLayoutId convolveLayout = device.InternDescriptorSetLayout(ImageBasedLightingFeature::ConvolveSet0());

    ComputePipelineDesc md{};
    md.ComputeShader = std::make_shared<const std::vector<uint8_t>>(std::move(csDownsample));
    md.DescriptorSetLayouts.push_back(convolveLayout);
    md.DebugName = "IBL_SceneProbeEnvMipDownsample";
    m_MipDownsamplePipelineId = device.InternComputePipeline(md);

    ComputePipelineDesc dd{};
    dd.ComputeShader = std::make_shared<const std::vector<uint8_t>>(std::move(csDiffuse));
    dd.DescriptorSetLayouts.push_back(convolveLayout);
    dd.DebugName = "IBL_SceneProbeDiffuseConvolve";
    SpecializationConstants diffuseSpecConstants;
    diffuseSpecConstants.AddConstant<uint32_t>(0u, captureResolution, "kEnvCaptureSize");
    dd.Specialization = std::move(diffuseSpecConstants);
    m_DiffusePipelineId = device.InternComputePipeline(dd);

    ComputePipelineDesc sd{};
    sd.ComputeShader = std::make_shared<const std::vector<uint8_t>>(std::move(csSpecular));
    sd.DescriptorSetLayouts.push_back(convolveLayout);
    sd.DebugName = "IBL_SceneProbeSpecularPrefilter";
    SpecializationConstants specConstants;
    specConstants.AddConstant<uint32_t>(0u, feature.GetPrefilterSize(), "kSpecBaseSize");
    specConstants.AddConstant<uint32_t>(1u, prefilterMipCount, "kSpecMipCount");
    sd.Specialization = std::move(specConstants);
    m_SpecularPipelineId = device.InternComputePipeline(sd);

    m_PipelinesReady = m_MipDownsamplePipelineId.IsValid() && m_DiffusePipelineId.IsValid() &&
                       m_SpecularPipelineId.IsValid();
    if (m_PipelinesReady)
    {
        m_PipelineCaptureResolution = captureResolution;
        m_PipelinePrefilterMipCount = prefilterMipCount;
    }
    return m_PipelinesReady;
}

CameraData SceneReflectionProbeEnvironmentSource::MakeFaceCamera(uint32_t face) const
{
    using GameEngine::Mathematics::MakeLookAtLH;
    using GameEngine::Mathematics::MakePerspectiveLH_ZO_ReverseZ;
    using GameEngine::Mathematics::Matrix4x4;
    using GameEngine::Mathematics::Vector3;

    const ProbeCubeFaceBasis& basis = kProbeCaptureFaces[face % ImageBasedLightingFeature::kNumCaptureFaces];
    const Vector3 eye(m_Desc.Position[0], m_Desc.Position[1], m_Desc.Position[2]);
    const Vector3 forward(basis.forward[0], basis.forward[1], basis.forward[2]);
    const Vector3 up(basis.up[0], basis.up[1], basis.up[2]);
    const Vector3 target(eye.x + forward.x, eye.y + forward.y, eye.z + forward.z);
    const Matrix4x4 view = MakeLookAtLH(eye, target, up);
    float fallbackFar = m_Desc.InfluenceRadius;
    if (m_Desc.BoxProjection)
    {
        const float rel[3] = {
            m_Desc.Position[0] - m_Desc.BoxCenter[0],
            m_Desc.Position[1] - m_Desc.BoxCenter[1],
            m_Desc.Position[2] - m_Desc.BoxCenter[2]};
        const float local[3] = {
            rel[0] * m_Desc.BoxAxisX[0] + rel[1] * m_Desc.BoxAxisX[1] + rel[2] * m_Desc.BoxAxisX[2],
            rel[0] * m_Desc.BoxAxisY[0] + rel[1] * m_Desc.BoxAxisY[1] + rel[2] * m_Desc.BoxAxisY[2],
            rel[0] * m_Desc.BoxAxisZ[0] + rel[1] * m_Desc.BoxAxisZ[1] + rel[2] * m_Desc.BoxAxisZ[2]};
        const float dx = m_Desc.BoxHalfExtents[0] + std::fabs(local[0]);
        const float dy = m_Desc.BoxHalfExtents[1] + std::fabs(local[1]);
        const float dz = m_Desc.BoxHalfExtents[2] + std::fabs(local[2]);
        fallbackFar = std::max(fallbackFar, std::sqrt(dx * dx + dy * dy + dz * dz));
    }
    const float requestedFar = m_Desc.MaxDistance > 0.0f ? m_Desc.MaxDistance : fallbackFar;
    const float farZ = std::max(requestedFar, 1.0f);
    const Matrix4x4 proj = MakePerspectiveLH_ZO_ReverseZ(kHalfPi, 1.0f, kCaptureNearPlane, farZ);
    const Matrix4x4 viewProj = proj * view;

    CameraData out{};
    std::memcpy(out.view, view.Data(), sizeof(float) * 16u);
    std::memcpy(out.proj, proj.Data(), sizeof(float) * 16u);
    std::memcpy(out.viewProj, viewProj.Data(), sizeof(float) * 16u);
    out.cameraPos[0] = eye.x;
    out.cameraPos[1] = eye.y;
    out.cameraPos[2] = eye.z;
    out.cameraPos[3] = 0.0f;
    return out;
}

void SceneReflectionProbeEnvironmentSource::RecordMipDownsample(RenderGraph::RGContext& ctx,
                                                                ImageBasedLightingFeature& feature,
                                                                uint32_t dstMip) const
{
    auto* cl = ctx.Cmd;
    auto* dev = ctx.GetDevice();
    if (!cl || !dev || dstMip == 0)
        return;

    TextureViewHandle store = feature.GetEnvCaptureStoreView(dstMip);
    // Single-mip source view: the sampled descriptor's claim must cover ONLY
    // the RG-declared Read(mip dstMip-1) — the whole-chain view would claim
    // ShaderReadOnly for the GENERAL-resident destination mip too (VUID-00344).
    TextureViewHandle source = feature.GetEnvCaptureMipSampleView(dstMip - 1);
    if (!store.IsValid() || !source.IsValid())
        return;

    PipelineHandle pipe = ctx.GetOrCreatePipelineVariant(m_MipDownsamplePipelineId);
    if (!pipe.IsValid())
        return;
    cl->SetPipeline(pipe);

    DescriptorSetDesc dsd{};
    dsd.layout = ImageBasedLightingFeature::ConvolveSet0();
    dsd.transient = true;
    dsd.debugName = "IBL_SceneProbeEnvMipDownsample_DS0";
    DescriptorSetHandle ds = dev->CreateDescriptorSet(dsd);
    dev->UpdateCombinedImageSamplerBinding(ds, 0, source, feature.GetCubeSampler());
    dev->UpdateStorageImageBinding(ds, 1, store);
    cl->BindDescriptorSet(0, ds, pipe);

    const uint32_t size = feature.GetEnvCaptureSize() >> dstMip;
    const uint32_t g = ((size > 0 ? size : 1u) + kProbeConvolveGroup - 1) / kProbeConvolveGroup;
    cl->Dispatch(g, g, ImageBasedLightingFeature::kNumCaptureFaces);
}

void SceneReflectionProbeEnvironmentSource::RecordDiffuseConvolve(RenderGraph::RGContext& ctx,
                                                                  ImageBasedLightingFeature& feature) const
{
    auto* cl = ctx.Cmd;
    auto* dev = ctx.GetDevice();
    if (!cl || !dev)
        return;

    PipelineHandle pipe = ctx.GetOrCreatePipelineVariant(m_DiffusePipelineId);
    if (!pipe.IsValid())
        return;
    cl->SetPipeline(pipe);

    DescriptorSetDesc dsd{};
    dsd.layout = ImageBasedLightingFeature::ConvolveSet0();
    dsd.transient = true;
    dsd.debugName = "IBL_SceneProbeDiffuseConvolve_DS0";
    DescriptorSetHandle ds = dev->CreateDescriptorSet(dsd);
    dev->UpdateCombinedImageSamplerBinding(ds, 0, feature.GetEnvCaptureCubeView(), feature.GetCubeSampler());
    dev->UpdateStorageImageBinding(ds, 1, feature.GetIrradianceStoreView());
    cl->BindDescriptorSet(0, ds, pipe);

    const uint32_t g = (ImageBasedLightingFeature::kIrradianceSize + kProbeConvolveGroup - 1) / kProbeConvolveGroup;
    cl->Dispatch(g, g, ImageBasedLightingFeature::kNumCaptureFaces);
}

void SceneReflectionProbeEnvironmentSource::RecordSpecularPrefilter(RenderGraph::RGContext& ctx,
                                                                    ImageBasedLightingFeature& feature,
                                                                    uint64_t bakedDigest) const
{
    auto* cl = ctx.Cmd;
    auto* dev = ctx.GetDevice();
    if (!cl || !dev)
        return;

    PipelineHandle pipe = ctx.GetOrCreatePipelineVariant(m_SpecularPipelineId);
    if (!pipe.IsValid())
        return;
    cl->SetPipeline(pipe);

    for (uint32_t mip = 0; mip < feature.GetPrefilterMipCount(); ++mip)
    {
        TextureViewHandle store = feature.GetPrefilterStoreView(mip);
        if (!store.IsValid())
            continue;

        DescriptorSetDesc dsd{};
        dsd.layout = ImageBasedLightingFeature::ConvolveSet0();
        dsd.transient = true;
        dsd.debugName = "IBL_SceneProbeSpecularPrefilter_DS0";
        DescriptorSetHandle ds = dev->CreateDescriptorSet(dsd);
        dev->UpdateCombinedImageSamplerBinding(ds, 0, feature.GetEnvCaptureCubeView(), feature.GetCubeSampler());
        dev->UpdateStorageImageBinding(ds, 1, store);
        cl->BindDescriptorSet(0, ds, pipe);

        const uint32_t size = feature.GetPrefilterSize() >> mip;
        const uint32_t g = (size + kProbeConvolveGroup - 1) / kProbeConvolveGroup;
        cl->Dispatch(g, g, ImageBasedLightingFeature::kNumCaptureFaces);
    }

    feature.SetLastBakedDigest(bakedDigest);
    feature.ResetFramesSinceBake();
}

bool SceneReflectionProbeEnvironmentSource::DeclareCaptureFrame(const EnvironmentBakeContext& ctx,
                                                                uint32_t faceMask)
{
    // The slices select LODs from the view's camera: every face shares the
    // eye and the projection, so any armed face's camera describes them all.
    uint8_t sliceIndices[ImageBasedLightingFeature::kNumCaptureFaces];
    uint32_t sliceCount = 0;
    for (uint32_t face = 0; face < ImageBasedLightingFeature::kNumCaptureFaces; ++face)
    {
        if ((faceMask & (1u << face)) != 0u)
            sliceIndices[sliceCount++] = ProbeFaceCullingIndex(face);
    }
    if (sliceCount == 0)
        return false;

    const uint32_t firstFace = static_cast<uint32_t>(sliceIndices[0] - kProbeFaceCullingIndexBase);
    m_Services->Views().SetCameraData(m_CameraId, MakeFaceCamera(firstFace));
    m_Services->Views().SetViewRenderLayerMask(m_ViewId, m_Desc.CullMask);
    m_Services->Views().SetViewWorldId(m_ViewId, m_Desc.WorldId);

    m_Services->WriteViewLightBuffer(m_ViewId);
    m_Services->BuildWorldBatchKeysForView(m_ViewId);
    return m_Services->ScheduleBucketerDispatchesForViewSlices(
        *ctx.Frame, m_ViewId, std::span<const uint8_t>(sliceIndices, sliceCount));
}

bool SceneReflectionProbeEnvironmentSource::DeclareFaceCapture(
    const EnvironmentBakeContext& ctx, RenderGraph::RGTexture envCube, uint32_t face)
{
    RenderGraph::RGFrame& frame = *ctx.Frame;
    ImageBasedLightingFeature& feature = *ctx.Feature;

    const bool hasSkyFace = m_SkyBackground.ScheduleCaptureFacesOnly(
        ctx, "IBLGen.SceneProbeSky", PassPhase::kEarlySetup, face);

    // Per-face registry state, consumed at declaration by this face's forward
    // emit and world pass (camera upload, clear ops), so re-pointing the one
    // view per face is exact.
    m_Services->Views().SetCameraData(m_CameraId, MakeFaceCamera(face));

    Rendering::ViewClearConfig clear{};
    clear.clearColor = !hasSkyFace;
    clear.clearColorValue[0] = 0.0f;
    clear.clearColorValue[1] = 0.0f;
    clear.clearColorValue[2] = 0.0f;
    clear.clearColorValue[3] = 1.0f;
    clear.clearDepth = true;
    clear.clearDepthValue = 0.0f;
    m_Services->Views().SetViewClearConfig(m_ViewId, clear);

    TextureDesc depthDesc{};
    depthDesc.width = feature.GetEnvCaptureSize();
    depthDesc.height = feature.GetEnvCaptureSize();
    depthDesc.depth = 1;
    depthDesc.mipLevels = 1;
    depthDesc.arrayLayers = 1;
    depthDesc.sampleCount = 1;
    depthDesc.format = static_cast<uint32_t>(m_Services->GetDepthFormat());
    depthDesc.usage = static_cast<uint32_t>(TextureUsage::DepthStencil | TextureUsage::ShaderResource);
    depthDesc.debugName = "IBL.SceneProbe.Depth";
    const std::string depthName = "IBL.SceneProbe.Depth.Face" + std::to_string(face);
    const RenderGraph::RGTexture depth = frame.ImportPersistentTexture(depthName.c_str(), depthDesc);
    if (!depth.IsValid())
        return false;

    RenderServices::WorldPassTargetsRG targets{};
    targets.Color = envCube;
    targets.Depth = depth;
    targets.ColorRange.BaseMip = 0;
    targets.ColorRange.MipCount = 1;
    targets.ColorRange.BaseLayer = face;
    targets.ColorRange.LayerCount = 1;

    // Forward commands are per view and replaced on every emit, so the pass
    // must directly follow its own face's emit.
    m_Services->EmitProducerForwardCommandsForView(
        m_ViewId, ForwardEmitPurpose::ReflectionProbeCapture);
    // Break the capture->cube->capture feedback: unless the probe opts in, the
    // capture face binds a zero-intensity EnvData so it never samples the cube
    // it is baking. Scoped to exactly this world pass declaration.
    m_Services->SetWorldPassExcludeEnvironment(!m_Desc.CaptureEnvironment);
    const auto world = m_Services->AddWorldPassForView(
        frame, m_ViewId, targets, MaterialKeyword::Instanced,
        RenderServices::WorldPassDrawScope::All, ProbeFaceCullingIndex(face));
    m_Services->SetWorldPassExcludeEnvironment(false);
    return world.Pass.IsValid();
}

void SceneReflectionProbeEnvironmentSource::DeclareFinalizePasses(
    const EnvironmentBakeContext& ctx, RenderGraph::RGTexture envCube,
    RenderGraph::RGTexture irrCube, RenderGraph::RGTexture prefCube, uint64_t bakedDigest)
{
    RenderGraph::RGFrame& frame = *ctx.Frame;
    ImageBasedLightingFeature& feature = *ctx.Feature;
    const SceneReflectionProbeEnvironmentSource* self = this;

    for (uint32_t mip = 1; mip < feature.GetEnvCaptureMipCount(); ++mip)
    {
        const std::string name = "IBLGen.SceneProbeEnvMipDownsample.Mip" + std::to_string(mip);
        frame.AddComputePass(
            name.c_str(), PassPhase::kWorldRender + 1,
            [envCube, mip](RenderGraph::RGPassBuilder& p) {
                RenderGraph::RGRange srcRange = RenderGraph::RGRange::All();
                srcRange.BaseMip = mip - 1;
                srcRange.MipCount = 1;
                RenderGraph::RGRange dstRange = RenderGraph::RGRange::All();
                dstRange.BaseMip = mip;
                dstRange.MipCount = 1;
                p.Read(envCube, RenderGraph::RGTextureRead::Sampled, srcRange);
                p.Write(envCube, RenderGraph::RGTextureWrite::Storage, dstRange);
                p.PreventCulling();
            },
            [self, feature = &feature, mip](RenderGraph::RGContext& c) {
                self->RecordMipDownsample(c, *feature, mip);
            });
    }

    frame.AddComputePass(
        "IBLGen.SceneProbeDiffuseConvolve", PassPhase::kWorldRender + 1,
        [envCube, irrCube](RenderGraph::RGPassBuilder& p) {
            p.Read(envCube, RenderGraph::RGTextureRead::Sampled);
            p.Write(irrCube, RenderGraph::RGTextureWrite::Storage);
            p.PreventCulling();
        },
        [self, feature = &feature](RenderGraph::RGContext& c) {
            self->RecordDiffuseConvolve(c, *feature);
        });

    frame.AddComputePass(
        "IBLGen.SceneProbeSpecularPrefilter", PassPhase::kWorldRender + 1,
        [envCube, prefCube](RenderGraph::RGPassBuilder& p) {
            p.Read(envCube, RenderGraph::RGTextureRead::Sampled);
            p.Write(prefCube, RenderGraph::RGTextureWrite::Storage);
            p.PreventCulling();
        },
        [self, feature = &feature, bakedDigest](RenderGraph::RGContext& c) {
            self->RecordSpecularPrefilter(c, *feature, bakedDigest);
        });
}

void SceneReflectionProbeEnvironmentSource::ScheduleBake(const EnvironmentBakeContext& ctx)
{
    if (!ctx.Frame || !ctx.Feature || !ctx.Device || !m_Services || !HasActiveContent())
        return;
    if (!ctx.Feature->EnsureCaptureResolution(m_Desc.CaptureResolution))
        return;
    if (!EnsureViews() || !EnsurePipelines(*ctx.Device, *ctx.Feature))
        return;

    TickBakeClock(ctx.DeltaTimeSeconds);

    RenderGraph::RGFrame& frame = *ctx.Frame;
    ImageBasedLightingFeature& feature = *ctx.Feature;
    feature.TickFramesSinceBake();

    // Realtime REBAKES time-slice (one face per frame); the first-ever bake,
    // Once probes, and every-frame probes take the single-frame path so the
    // cube never pops in half-baked.
    const bool everyFrame = CapturesEveryFrame();
    const bool sliceRebakes =
        m_Desc.UpdateMode == SceneReflectionProbeUpdateMode::Realtime &&
        !everyFrame && feature.GetLastBakedDigest() != 0;

    switch (m_CaptureState)
    {
        case CaptureState::Idle:
            // Idle: the OnDemand capture view has no live request, so
            // extraction, batch keys, and the GPU culling loop all skip it.
            // Request one frame ahead of a due bake; requests auto-expire
            // right after their capture frame. The frame throttle keeps a
            // continuously-changing sky (animated time-of-day, slider drags)
            // from re-arming a full bake every frame — same policy as the sky
            // source's own rebake gate. An every-frame probe asked for exactly
            // that, so it bypasses the throttle.
            if (BakeDue() &&
                (everyFrame ||
                 feature.GetFramesSinceBake() >= ImageBasedLightingFeature::kIblRebakeIntervalFrames))
            {
                if (!everyFrame)
                    Logger::Log::Debug("SceneProbe: bake armed ({})",
                                       sliceRebakes ? "sliced" : "single-frame");
                if (sliceRebakes)
                {
                    m_SliceFace = 0;
                    m_SliceDigest = 0;
                    ArmFaces(1u << 0);
                    m_CaptureState = CaptureState::Slicing;
                }
                else
                {
                    ArmViews();
                }
            }
            return;
        case CaptureState::Arming:
        case CaptureState::Slicing:
            // The request covers exactly THIS frame: extraction and culling
            // ran with the armed faces fed, and it lapses next frame.
            break;
    }

    const RenderGraph::RGTexture envCube = frame.ImportExternalTexture(
        "IBL_EnvCapture", feature.GetEnvCaptureTex(), ResourceState::ShaderResource,
        TextureFormat::R16G16B16A16_FLOAT, feature.GetEnvCaptureMipCount(),
        ImageBasedLightingFeature::kNumCaptureFaces);

    if (m_CaptureState == CaptureState::Slicing)
    {
        if (m_SliceFace == 0)
        {
            // Consume the digest once per cycle and hold it: the whole cycle
            // bakes FOR this snapshot; a sun still moving starts the next one.
            m_SliceDigest = NextBakeDigest();
            if (m_SliceDigest == 0 || m_SliceDigest == feature.GetLastBakedDigest())
            {
                m_CaptureState = CaptureState::Idle; // armed for nothing — request lapses
                return;
            }
        }

        if (!DeclareCaptureFrame(ctx, 1u << m_SliceFace) ||
            !DeclareFaceCapture(ctx, envCube, m_SliceFace))
        {
            Logger::Log::Debug("SceneProbe: sliced face {} declare failed — aborting cycle",
                               m_SliceFace);
            // A face abort can leave real writes behind: DeclareFaceCapture
            // declares the sky-background face pass FIRST (PreventCulling — it
            // renders even when the world pass never declares), so the face
            // ends the frame in ColorAttachment. Restore the resting state
            // here too, or every following frame's ShaderResource import is a
            // lie and samplers of the cube trip VUID-09600 until the next
            // successful cycle heals the layout.
            frame.MarkOutput(envCube, RenderGraph::RGImageLayout::ShaderReadOnly);
            m_CaptureState = CaptureState::Idle; // abort; the next due bake restarts
            return;
        }

        ++m_SliceFace;
        if (m_SliceFace < ImageBasedLightingFeature::kNumCaptureFaces)
        {
            // Mid-cycle the face render is this frame's LAST touch of the env
            // cube, which would end the frame with that face in ColorAttachment
            // while every frame's import re-declares ShaderResource. Restore
            // the resting state explicitly so the cross-frame import contract
            // stays true (VUID-09600: sampled-while-attached on layers of a
            // half-baked cycle). The finalize frame needs no export — the
            // convolve reads already end the frame at ShaderResource (and an
            // export there would stamp a final layout onto compute-written mips).
            frame.MarkOutput(envCube, RenderGraph::RGImageLayout::ShaderReadOnly);
            ArmFaces(1u << m_SliceFace); // feed the next face next frame
            return;
        }

        const RenderGraph::RGTexture irrCube = frame.ImportExternalTexture(
            "IBL_Irradiance", feature.GetIrradianceCube(), ResourceState::ShaderResource,
            TextureFormat::R16G16B16A16_FLOAT, 1, ImageBasedLightingFeature::kNumCaptureFaces);
        const RenderGraph::RGTexture prefCube = frame.ImportExternalTexture(
            "IBL_Prefilter", feature.GetPrefilterCube(), ResourceState::ShaderResource,
            TextureFormat::R16G16B16A16_FLOAT, feature.GetPrefilterMipCount(),
            ImageBasedLightingFeature::kNumCaptureFaces);
        Logger::Log::Debug("SceneProbe: sliced cycle finalized (digest {:x})", m_SliceDigest);
        DeclareFinalizePasses(ctx, envCube, irrCube, prefCube, m_SliceDigest);
        m_CaptureState = CaptureState::Idle; // cycle complete — the last request lapses
        return;
    }

    // Single-frame full bake (first bake / Once probes / world change re-arm).
    const uint64_t digest = NextBakeDigest();
    if (digest == 0 || digest == feature.GetLastBakedDigest())
    {
        m_CaptureState = CaptureState::Idle; // armed for nothing — the requests lapse on their own
        return;
    }

    const RenderGraph::RGTexture irrCube = frame.ImportExternalTexture(
        "IBL_Irradiance", feature.GetIrradianceCube(), ResourceState::ShaderResource,
        TextureFormat::R16G16B16A16_FLOAT, 1, ImageBasedLightingFeature::kNumCaptureFaces);
    const RenderGraph::RGTexture prefCube = frame.ImportExternalTexture(
        "IBL_Prefilter", feature.GetPrefilterCube(), ResourceState::ShaderResource,
        TextureFormat::R16G16B16A16_FLOAT, feature.GetPrefilterMipCount(),
        ImageBasedLightingFeature::kNumCaptureFaces);

    // Shared prelude once, then one pass per face. A declare failure
    // (transient bucketer/import pressure) re-arms: bare-returning would retry
    // NEXT frame while still Arming — against a lapsed view request, whose
    // empty batch keys "succeed" as clear-only faces and commit a black cube.
    // Re-arm so the retry runs with a freshly fed view; the digest was already
    // consumed, so NextBakeDigest() hands the retry the same snapshot. Faces
    // already declared this frame (sky background included) still render, so
    // restore the cube's resting layout for the retry frame's import.
    if (!DeclareCaptureFrame(ctx, kAllFacesMask))
    {
        Logger::Log::Debug("SceneProbe: full-bake bucketer declare failed — re-arming");
        frame.MarkOutput(envCube, RenderGraph::RGImageLayout::ShaderReadOnly);
        ArmViews();
        return;
    }
    for (uint32_t face = 0; face < ImageBasedLightingFeature::kNumCaptureFaces; ++face)
    {
        if (!DeclareFaceCapture(ctx, envCube, face))
        {
            Logger::Log::Debug("SceneProbe: full-bake face {} declare failed — re-arming", face);
            frame.MarkOutput(envCube, RenderGraph::RGImageLayout::ShaderReadOnly);
            ArmViews();
            return;
        }
    }

    if (!everyFrame)
        Logger::Log::Debug("SceneProbe: full bake finalized (digest {:x})", digest);
    DeclareFinalizePasses(ctx, envCube, irrCube, prefCube, digest);
    if (everyFrame)
    {
        // Feed the view again for NEXT frame's extraction and culling so the
        // following bake declares without an idle arming frame in between.
        ArmViews();
        return;
    }
    m_CaptureState = CaptureState::Idle; // capture scheduled — the view request lapses on its own
}

} // namespace Engine::Renderer
} // namespace GameEngine
