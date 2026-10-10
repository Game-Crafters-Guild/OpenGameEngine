#include "Rendering/Core/GPUCulling.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <limits>

#include "Logger/Logger.h"
#include "Rendering/Core/BarrierMapping.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/GPUScene.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"

namespace GameEngine
{
namespace Rendering
{

void MakeFrustumPlanesCameraRelative(Vector4* planes, const Vector3& origin)
{
    for (int p = 0; p < 6; ++p)
        planes[p].w += planes[p].x * origin.x + planes[p].y * origin.y + planes[p].z * origin.z;
}

// Phase 6-ii: visibility-union shader loader (set once at engine init).
namespace
{
GPUCullingPipeline::VisibilityUnionShaderLoaderFunc g_VisibilityUnionShaderLoader = nullptr;
GPUCullingPipeline::RuntimeVisibilityShaderLoaderFunc g_RuntimeVisibilityShaderLoader = nullptr;
GPUCullingPipeline::HzbCullingShaderLoaderFunc g_HzbCullingShaderLoader = nullptr;

struct VisibilityUnionPC
{
    uint32_t instanceCount;
    uint32_t viewCount;
};

struct RuntimeVisibilityPC
{
    uint32_t instanceCount;
    uint32_t runtimeVisibleCap;
};

// hzb_culling.comp push-constant block.
struct HzbCullPC
{
    uint32_t instanceCount; // dispatch bound (candidate count for this slice)
    uint32_t sliceOffset;   // element base into the shared visibility buffer
};

// hzb_culling modes (mirrors the shader's GPUCullingData.hzbMode switch).
constexpr uint32_t kHzbModePhaseA = 1u; // frustum ∧ prevVisible
constexpr uint32_t kHzbModePhaseB = 2u; // full HZB test + prevVisible rewrite

// Name codes for the (viewId, code) pass-name scheme — kept distinct from the
// frustum path's codes (0xFE fused cascades, 0xFF single views) so RenderDoc /
// perf-CSV names stay unambiguous.
constexpr uint32_t kOcclusionPhaseANameCode = 0xFCu; // "GPUCulling.View{n}.C252"
constexpr uint32_t kOcclusionPhaseBNameCode = 0xFDu; // "GPUCulling.View{n}.C253"

// set 0 for hzb_culling.comp: instance/cullingData/visibility/prevVisible SSBOs,
// the HZB combined image sampler, and the stats SSBO.
DescriptorSetLayoutDesc MakeHzbCullingDescriptorSetLayout()
{
    DescriptorSetLayoutDesc layout{};
    layout.debugName = "HzbCulling_SetLayout";
    auto addStorage = [&](uint32_t binding, const char* name)
    {
        DescriptorBinding b{};
        b.binding = binding;
        b.type = DescriptorType::StorageBuffer;
        b.count = 1;
        b.shaderStages = kShaderStageCompute;
        b.debugName = name;
        layout.bindings.push_back(b);
    };
    addStorage(0, "InstanceBuffer");
    addStorage(1, "CullingDataBuffer");
    addStorage(2, "VisibilityBuffer");
    addStorage(3, "PrevVisibleBuffer");
    DescriptorBinding hzb{};
    hzb.binding = 4;
    hzb.type = DescriptorType::CombinedImageSampler;
    hzb.count = 1;
    hzb.shaderStages = kShaderStageCompute;
    hzb.debugName = "HZB";
    layout.bindings.push_back(hzb);
    addStorage(5, "OcclusionStatsBuffer");
    return layout;
}

// Shared between EndFrameRG's per-view dispatch and the deferred phase-B
// occlusion dispatch — one fill site, no drift between the generations.
void FillCullingDataFromView(const ViewCullingInput& view, uint32_t sliceInstanceCount,
                             GPUCullingData& cd)
{
    cd.viewMatrix = view.viewMatrix;
    cd.projMatrix = view.projMatrix;
    cd.viewProjMatrix = view.viewProjMatrix;
    for (int p = 0; p < 6; ++p)
        cd.frustumPlanes[0][p] = view.frustumPlanes[p];
    // Camera-relative frustum test (Earth-scale precision). cameraPosition carries
    // the render origin the shader subtracts from boundingCenter; the planes are
    // translated by the same origin so the test stays small-magnitude. Origin
    // (0,0,0) => both are no-ops and this is byte-identical to the world-space test.
    cd.cameraPosition = view.cameraRelativeOrigin;
    MakeFrustumPlanesCameraRelative(cd.frustumPlanes[0], view.cameraRelativeOrigin);
    cd.nearPlane = view.nearPlane;
    cd.cameraForward = view.cameraForward;
    cd.farPlane = view.farPlane;
    cd.firstInstance = view.firstInstance;
    cd.instanceCount = sliceInstanceCount;
    cd.renderLayerMask = view.renderLayerMask;
    cd.frameIndex = view.frameIndex;
    cd.deltaTime = view.deltaTime;
    // Shadow dispatches (area light = kAreaShadowCullingIndex, or any per-view
    // cascade) must drop non-casters; the main camera / reflection / thumbnail
    // views (kCullingCascadeIndexNone) keep everything.
    cd.cullShadowCasters = (view.cascadeIndex != kCullingCascadeIndexNone) ? 1u : 0u;
    // Single-view dispatches (main / probe / area / spot / point faces)
    // stay conservative — a false cull here pops on screen.
    for (uint32_t m = 0; m < kMaxViewsPerCullingDispatch; ++m)
        cd.cullMargins[m] = kCullMarginConservative;
}
} // namespace

void GPUCullingPipeline::SetVisibilityUnionShaderLoader(VisibilityUnionShaderLoaderFunc loader)
{
    g_VisibilityUnionShaderLoader = loader;
}

void GPUCullingPipeline::SetRuntimeVisibilityShaderLoader(RuntimeVisibilityShaderLoaderFunc loader)
{
    g_RuntimeVisibilityShaderLoader = loader;
}

void GPUCullingPipeline::SetHzbCullingShaderLoader(HzbCullingShaderLoaderFunc loader)
{
    g_HzbCullingShaderLoader = loader;
}

GPUCullingPipeline::GPUCullingPipeline(IDevice* device, const GPUCullingConfig& config)
    : m_Device(device), m_Config(config)
{
    // GPU buffers are created lazily by the RenderGraph arm (EndFrameRG /
    // ScheduleVisibilityUnion / ImportRuntimeVisible) — a pipeline that never
    // culls owns no GPU memory.
}

GPUCullingPipeline::~GPUCullingPipeline()
{
    if (!m_Device)
    {
        return;
    }

    if (m_VisibilityBufferU.IsValid())
        m_Device->DestroyBuffer(m_VisibilityBufferU);
    if (m_AnyViewVisibleU.IsValid())
        m_Device->DestroyBuffer(m_AnyViewVisibleU);
    if (m_RuntimeVisibleU.IsValid())
        m_Device->DestroyBuffer(m_RuntimeVisibleU);
    for (auto& [viewId, pv] : m_PrevVisible)
        if (pv.buffer.IsValid())
            m_Device->DestroyBuffer(pv.buffer);
    if (m_OcclusionStatsU.IsValid())
        m_Device->DestroyBuffer(m_OcclusionStatsU);
    if (m_HzbSentinel.IsValid())
        m_Device->DestroyTexture(m_HzbSentinel);
    if (m_HzbSampler.IsValid())
        m_Device->DestroySampler(m_HzbSampler);
}

void GPUCullingPipeline::ReprovisionAfterDeviceRebuild()
{
    // Drop the dead device handles WITHOUT Destroy* — the in-place rebuild
    // teardown already freed these VkObjects, so a Destroy here would double-free
    // a recycled slot. The lazy Ensure*/Import* paths recreate them on next use.
    m_VisibilityBufferU = {};
    m_VisibilityBufferUBytes = 0;
    m_AnyViewVisibleU = {};
    m_AnyViewVisibleUBytes = 0;
    m_RuntimeVisibleU = {};
    m_OcclusionStatsU = {};
    m_HzbSentinel = {};
    m_HzbSampler = {};
    // Per-view occlusion history: re-import first-touches to all-visible.
    m_PrevVisible.clear();

    // Reset the one-shot pipeline guards so Ensure*/Create* recompiles the PSOs
    // (the concrete pipeline cache was cleared; SPIR-V + interned descs survive).
    m_VisibilityUnionPipeline = INVALID_PIPELINE_HANDLE;
    m_VisibilityUnionPipelineAttempted = false;
    m_VisibilityUnionLoadFailureLogged = false;
    m_RuntimeVisibilityPipeline = INVALID_PIPELINE_HANDLE;
    m_RuntimeVisibilityPipelineAttempted = false;
    m_RuntimeVisibilityLoadFailureLogged = false;
    m_HzbCullingPipeline = INVALID_PIPELINE_HANDLE;
    m_HzbCullingPipelineAttempted = false;
    m_HzbCullingLoadFailureLogged = false;

    // The rebuilt device holds none of the retained GPU content the elision
    // gate's record describes — recompute unconditionally next frame (the
    // recreated handle bits would also break equality; this is the explicit
    // belt over that coincidence).
    m_ElisionGate.ForceRecompute();
}

bool GPUCullingPipeline::CreateVisibilityUnionPipeline()
{
    if (m_VisibilityUnionPipeline.IsValid())
        return true;
    if (m_VisibilityUnionPipelineAttempted)
        return false;

    if (!m_Device)
        return false;

    // Missing bytes are not latched — retried on a later call once the
    // loader environment is ready; the failure logs once.
    std::string loadErr;
    std::vector<uint8_t> shaderBytes = LoadComputeStageBytes("Shaders/visibility_union.shaderpkg",
                                                             m_Device->PreferredShaderSource(),
                                                             g_VisibilityUnionShaderLoader, &loadErr);
    if (shaderBytes.empty())
    {
        if (!m_VisibilityUnionLoadFailureLogged)
        {
            std::cerr << "GPUCullingPipeline: visibility_union.shaderpkg unavailable (" << loadErr
                      << "); visibility union path deferred" << std::endl;
            m_VisibilityUnionLoadFailureLogged = true;
        }
        return false;
    }
    m_VisibilityUnionPipelineAttempted = true;

    // 3 SSBO bindings — match visibility_union.comp set 0:
    //   0: VisibilityBuffer  (read)
    //   1: AnyVisibleFlags   (write)
    //   2: ViewOffsets       (read)
    DescriptorSetLayoutDesc layout{};
    layout.debugName = "VisibilityUnion_SetLayout";
    for (uint32_t i = 0; i < 3; ++i)
    {
        DescriptorBinding b{};
        b.binding      = i;
        b.type         = DescriptorType::StorageBuffer;
        b.count        = 1;
        b.shaderStages = kShaderStageCompute;
        layout.bindings.push_back(b);
    }

    ComputePipelineDesc cd{};
    cd.ComputeShader = std::make_shared<const std::vector<uint8_t>>(std::move(shaderBytes));
    cd.DescriptorSetLayouts.push_back(m_Device->InternDescriptorSetLayout(layout));
    cd.PushConstants.Size      = sizeof(VisibilityUnionPC);
    cd.PushConstants.StageMask = kShaderStageCompute;
    cd.DebugName               = "GPUCulling_VisibilityUnion";

    const auto id = m_Device->InternComputePipeline(cd);
    m_VisibilityUnionPipeline = m_Device->GetOrCreateComputePipeline(id);
    if (!m_VisibilityUnionPipeline.IsValid())
    {
        std::cerr << "GPUCullingPipeline: failed to create visibility_union compute pipeline"
                  << std::endl;
        return false;
    }
    return true;
}

void GPUCullingPipeline::SubmitView(const ViewCullingInput& input)
{
    // Arm-agnostic for real: EITHER arm's BeginFrame opens submissions.
    if (!m_CurrentFrame || !m_CurrentScene)
    {
        return;
    }

    m_PendingViews.push_back(input);
}

void GPUCullingPipeline::SubmitCascadeGroup(const CascadeCullingGroup& group)
{
    if (!m_CurrentFrame || !m_CurrentScene)
        return;
    if (group.cascadeCount == 0u || group.instanceCount == 0u)
        return;
    if (group.cascadeCount > kMaxCullingViewsPerDispatch)
        return;
    m_PendingCascadeGroups.push_back(group);
}

void GPUCullingPipeline::EndFrame()
{
    if (!m_CurrentFrame)
    {
        // No open frame: clear submissions and bail.
        m_PendingViews.clear();
        m_PendingCascadeGroups.clear();
        return;
    }
    EndFrameRG();
}

bool GPUCullingPipeline::CreateRuntimeVisibilityPipeline()
{
    if (m_RuntimeVisibilityPipeline.IsValid())
        return true;
    if (m_RuntimeVisibilityPipelineAttempted)
        return false;

    if (!m_Device)
        return false;

    // Missing bytes are not latched — retried on a later call once the
    // loader environment is ready; the failure logs once.
    std::string loadErr;
    std::vector<uint8_t> shaderBytes = LoadComputeStageBytes(
        "Shaders/runtime_visibility_aggregate.shaderpkg", m_Device->PreferredShaderSource(),
        g_RuntimeVisibilityShaderLoader, &loadErr);
    if (shaderBytes.empty())
    {
        if (!m_RuntimeVisibilityLoadFailureLogged)
        {
            std::cerr << "GPUCullingPipeline: runtime_visibility_aggregate.shaderpkg unavailable ("
                      << loadErr << "); runtime-visibility-gated skinning deferred" << std::endl;
            m_RuntimeVisibilityLoadFailureLogged = true;
        }
        return false;
    }
    m_RuntimeVisibilityPipelineAttempted = true;

    // 3 SSBO bindings — match runtime_visibility_aggregate.comp set 0:
    //   0: AnyVisibleFlags    (read)
    //   1: InstanceBuffer     (read)
    //   2: RuntimeVisibleBuffer (atomic write)
    DescriptorSetLayoutDesc layout{};
    layout.debugName = "RuntimeVisibility_SetLayout";
    for (uint32_t i = 0; i < 3; ++i)
    {
        DescriptorBinding b{};
        b.binding      = i;
        b.type         = DescriptorType::StorageBuffer;
        b.count        = 1;
        b.shaderStages = kShaderStageCompute;
        layout.bindings.push_back(b);
    }

    ComputePipelineDesc cd{};
    cd.ComputeShader = std::make_shared<const std::vector<uint8_t>>(std::move(shaderBytes));
    cd.DescriptorSetLayouts.push_back(m_Device->InternDescriptorSetLayout(layout));
    cd.PushConstants.Size      = sizeof(RuntimeVisibilityPC);
    cd.PushConstants.StageMask = kShaderStageCompute;
    cd.DebugName               = "GPUCulling_RuntimeVisibility";

    const auto id = m_Device->InternComputePipeline(cd);
    m_RuntimeVisibilityPipeline = m_Device->GetOrCreateComputePipeline(id);
    if (!m_RuntimeVisibilityPipeline.IsValid())
    {
        std::cerr << "GPUCullingPipeline: failed to create runtime_visibility_aggregate compute pipeline"
                  << std::endl;
        return false;
    }
    return true;
}

bool GPUCullingPipeline::EnsureHzbCullingPipeline()
{
    if (m_HzbCullingPipeline.IsValid())
        return true;
    if (m_HzbCullingPipelineAttempted)
        return false;
    if (!m_Device)
        return false;

    // Missing bytes are not latched — retried once the loader environment is
    // ready; the failure logs once.
    std::string loadErr;
    std::vector<uint8_t> shaderBytes = LoadComputeStageBytes("Shaders/hzb_culling.shaderpkg",
                                                             m_Device->PreferredShaderSource(),
                                                             g_HzbCullingShaderLoader, &loadErr);
    if (shaderBytes.empty())
    {
        if (!m_HzbCullingLoadFailureLogged)
        {
            std::cerr << "GPUCullingPipeline: hzb_culling.shaderpkg unavailable (" << loadErr
                      << "); two-phase HZB occlusion deferred (falls back to frustum-only)"
                      << std::endl;
            m_HzbCullingLoadFailureLogged = true;
        }
        return false;
    }
    m_HzbCullingPipelineAttempted = true;

    ComputePipelineDesc cd{};
    cd.ComputeShader = std::make_shared<const std::vector<uint8_t>>(std::move(shaderBytes));
    cd.DescriptorSetLayouts.push_back(
        m_Device->InternDescriptorSetLayout(MakeHzbCullingDescriptorSetLayout()));
    cd.PushConstants.Size = sizeof(HzbCullPC);
    cd.PushConstants.StageMask = kShaderStageCompute;
    cd.DebugName = "GPUCulling_HzbCulling";

    const auto id = m_Device->InternComputePipeline(cd);
    m_HzbCullingPipeline = m_Device->GetOrCreateComputePipeline(id);
    if (!m_HzbCullingPipeline.IsValid())
    {
        std::cerr << "GPUCullingPipeline: failed to create hzb_culling compute pipeline" << std::endl;
        return false;
    }
    return true;
}

SamplerHandle GPUCullingPipeline::EnsureHzbSampler()
{
    if (m_HzbSampler.IsValid())
        return m_HzbSampler;
    if (!m_Device)
        return {};
    // Point + clamp: the HZB test reads exact texel depths and gathers rect
    // corners itself — no bilinear blend, no wrap.
    m_HzbSampler = m_Device->CreateSampler(SamplerDesc::PointClamp("HZBCullSampler"));
    return m_HzbSampler;
}

RenderGraph::RGTexture GPUCullingPipeline::ImportHzbSentinel(RenderGraph::RGFrame& frame)
{
    if (!m_Device)
        return {};
    if (!m_HzbSentinel.IsValid())
    {
        TextureDesc td{};
        td.width = 1u;
        td.height = 1u;
        td.depth = 1u;
        td.mipLevels = 1u;
        td.arrayLayers = 1u;
        td.sampleCount = 1u;
        td.format = static_cast<uint32_t>(TextureFormat::R32_FLOAT);
        td.usage = static_cast<uint32_t>(TextureUsage::ShaderResource);
        td.debugName = "GPUCulling_HzbSentinel";
        m_HzbSentinel = m_Device->CreateTexture(td);
    }
    if (!m_HzbSentinel.IsValid())
        return {};
    // Common is honest every frame: mode 1 never samples this texture, so its
    // contents are always discardable (mirrors IBLGenNode's import). The read
    // declaration below transitions it to ShaderResource.
    return frame.ImportExternalTexture("GPUCulling.HzbSentinel", m_HzbSentinel, ResourceState::Common,
                                       TextureFormat::R32_FLOAT, 1u, 1u);
}

RenderGraph::RGBuffer GPUCullingPipeline::ImportPrevVisible(RenderGraph::RGFrame& frame, ViewId viewId,
                                                            uint32_t instanceCount)
{
    if (!m_Device)
        return {};
    PrevVisibleView& pv = m_PrevVisible[viewId];
    // Sized for the global instance index the shader addresses (prevVisible[i]);
    // grown like m_VisibilityBufferU.
    const size_t kMinBytes = 4096;
    const size_t needBytes =
        std::max(kMinBytes, static_cast<size_t>(instanceCount) * sizeof(uint32_t));
    bool freshlyCreated = false;
    if (!pv.buffer.IsValid() || pv.bytes < needBytes)
    {
        if (pv.buffer.IsValid())
            m_Device->DestroyBuffer(pv.buffer); // deferred-destroy device-side
        BufferDesc d{};
        d.size = needBytes;
        d.usage = static_cast<uint32_t>(BufferUsage::Storage | BufferUsage::TransferDst);
        d.memoryUsage = BufferMemoryUsage::DeviceLocal;
        pv.importName = "GPUCulling.PrevVisible.V" + std::to_string(static_cast<uint32_t>(viewId));
        d.debugName = pv.importName.c_str();
        pv.buffer = m_Device->CreateBuffer(d);
        pv.bytes = pv.buffer.IsValid() ? needBytes : 0;
        freshlyCreated = true;
    }
    if (!pv.buffer.IsValid())
        return {};

    const RenderGraph::RGBuffer rg = frame.ImportExternalBuffer(pv.importName.c_str(), pv.buffer);
    if (freshlyCreated)
    {
        // The full-buffer 0xFFFFFFFF init below supersedes any per-view pending
        // resets, so register the view and drop them (also registers a brand-new
        // view so subsequent frames' recycled slots accumulate for it).
        m_PrevVisibleResetTracker.OnBufferInitialized(viewId);
        // First-touch init to ALL-ONES: unknown visibility must mean VISIBLE so
        // frame 1 phase A draws everything (and grow/camera-cut both self-heal
        // in one frame via phase B). Declared BEFORE phase A reads it (RAW
        // orders the fill first). Mirrors ImportRuntimeVisible's first-touch.
        const RenderGraph::RGBuffer pvRG = rg;
        const uint32_t fillBytes = static_cast<uint32_t>(pv.bytes);
        frame.AddPass("GPUCulling.PrevVisibleInit", static_cast<int32_t>(PassPhase::kEarlySetup),
                      [&](RenderGraph::RGPassBuilder& p)
                      { p.Write(pvRG, RenderGraph::RGBufferWrite::CopyDst); },
                      [pvRG, fillBytes](RenderGraph::RGContext& ctx)
                      { ctx.Cmd->FillBuffer(ctx.GetBuffer(pvRG), 0, fillBytes, 0xFFFFFFFFu); });
    }
    return rg;
}

void GPUCullingPipeline::SchedulePrevVisibleResetPass(
    RenderGraph::RGFrame& frame, RenderGraph::RGBuffer prevVisible, uint32_t prevVisibleBytes,
    const std::vector<std::pair<uint32_t, uint32_t>>& runsIn)
{
    if (runsIn.empty() || !prevVisible.IsValid())
        return;

    // Copy the runs into the exec closure (the caller's scratch is reused per
    // view). The exec clamps to prevVisibleBytes so an add-into-recycled slot
    // that is then removed-and-trimmed the same frame — pushing its index past
    // the current buffer — never fills out of range; the grow-fill self-heals it.
    std::vector<std::pair<uint32_t, uint32_t>> runs = runsIn;
    frame.AddPass("GPUCulling.PrevVisibleReset", static_cast<int32_t>(PassPhase::kEarlySetup),
                  [&](RenderGraph::RGPassBuilder& p)
                  { p.Write(prevVisible, RenderGraph::RGBufferWrite::CopyDst); },
                  [prevVisible, runs = std::move(runs), prevVisibleBytes](RenderGraph::RGContext& ctx)
                  {
                      for (const auto& [startElement, countElements] : runs)
                      {
                          const uint64_t offset =
                              static_cast<uint64_t>(startElement) * sizeof(uint32_t);
                          if (offset >= prevVisibleBytes)
                              continue;
                          uint64_t bytes = static_cast<uint64_t>(countElements) * sizeof(uint32_t);
                          if (offset + bytes > prevVisibleBytes)
                              bytes = static_cast<uint64_t>(prevVisibleBytes) - offset;
                          ctx.Cmd->FillBuffer(ctx.GetBuffer(prevVisible),
                                              static_cast<uint32_t>(offset),
                                              static_cast<uint32_t>(bytes), 0xFFFFFFFFu);
                      }
                  });
}

RenderGraph::RGBuffer GPUCullingPipeline::ImportOcclusionStats(RenderGraph::RGFrame& frame)
{
    if (!m_Device)
        return {};
    if (!m_OcclusionStatsU.IsValid())
    {
        BufferDesc d{};
        d.size = sizeof(OcclusionStats);
        d.usage = static_cast<uint32_t>(BufferUsage::Storage | BufferUsage::TransferDst);
        // The GPU writes it and ReadOcclusionStats maps and copies it out, so it
        // is a readback: it wants a host-cached type, not a write-combined one.
        //
        // Storage is the half that must survive: the phase-B dispatch binds
        // this buffer, and a backend that cannot pair Storage with a mappable
        // class (WebGPU) would otherwise reject the binding and take occlusion
        // culling down with it. The stats are a diagnostic counter, so there
        // the memory class yields and ReadOcclusionStats reports zeroes.
        const bool mappable = m_Device->GetCapabilities().supportsMappableStorageBuffers;
        d.memoryUsage = mappable ? BufferMemoryUsage::Readback : BufferMemoryUsage::DeviceLocal;
        d.debugName = "GPUCulling_OcclusionStats";
        m_OcclusionStatsU = m_Device->CreateBuffer(d);
        // Zero so ReadOcclusionStats reads clean values before the first
        // phase-B dispatch; every mode-2 dispatch resets it again.
        if (m_OcclusionStatsU.IsValid() && mappable)
            if (void* mapped = m_Device->MapBuffer(m_OcclusionStatsU))
            {
                std::memset(mapped, 0, sizeof(OcclusionStats));
                m_Device->UnmapBuffer(m_OcclusionStatsU);
            }
    }
    if (!m_OcclusionStatsU.IsValid())
        return {};
    return frame.ImportExternalBuffer("GPUCulling.OcclusionStats", m_OcclusionStatsU);
}

void GPUCullingPipeline::ScheduleHzbCullPass(RenderGraph::RGFrame& frame,
                                             RenderGraph::RGBuffer instances, uint32_t instanceBytes,
                                             RenderGraph::RGBuffer visibility,
                                             RenderGraph::RGBuffer prevVisible,
                                             uint32_t prevVisibleBytes, RenderGraph::RGBuffer stats,
                                             RenderGraph::RGTexture hzb,
                                             const GPUCullingData& cullingData,
                                             uint32_t sliceOffsetElements, uint32_t firstInstance,
                                             uint32_t sliceInstanceCount, uint64_t sliceStableKey)
{
    if (!m_Device || sliceInstanceCount == 0u)
        return;
    if (!EnsureHzbCullingPipeline())
        return;
    const SamplerHandle sampler = EnsureHzbSampler();
    if (!sampler.IsValid() || !instances.IsValid() || !visibility.IsValid() ||
        !prevVisible.IsValid() || !stats.IsValid() || !hzb.IsValid())
        return;

    // Per-dispatch culling params via the frame upload ring (same as the
    // frustum path). firstInstance/instanceCount overridden to this slice.
    auto cd = frame.AllocUpload<GPUCullingData>();
    if (!cd.Valid())
        return;
    *cd.Ptr = cullingData;
    cd.Ptr->firstInstance = firstInstance;
    cd.Ptr->instanceCount = sliceInstanceCount;

    const bool isPhaseB = (cullingData.hzbMode == kHzbModePhaseB);

    char passName[64];
    std::snprintf(passName, sizeof(passName), "GPUCulling.View%u.C%u",
                  static_cast<uint32_t>(sliceStableKey >> 16),
                  static_cast<uint32_t>(sliceStableKey & 0xFFFF));

    HzbCullPC pc{};
    pc.instanceCount = sliceInstanceCount;
    pc.sliceOffset = sliceOffsetElements;

    const PipelineHandle pipeline = m_HzbCullingPipeline;
    const uint32_t visBytes = static_cast<uint32_t>(m_VisibilityBufferUBytes);
    const uint32_t statsBytes = static_cast<uint32_t>(sizeof(OcclusionStats));
    // Phase A sits with the early frustum culls; phase B is genuinely
    // mid-pipeline (after the phase-A raster + HZB build — its RAW/WAR edges
    // enforce that regardless of phase, this only keeps the schedule readable).
    const int32_t phase =
        static_cast<int32_t>(isPhaseB ? PassPhase::kDefault : PassPhase::kEarlySetup);

    frame.AddPass(
        passName, phase,
        [&](RenderGraph::RGPassBuilder& p)
        {
            p.Read(instances);
            p.Read(hzb, RenderGraph::RGTextureRead::SampledCompute);
            p.Write(visibility);
            if (isPhaseB)
            {
                // Mode 2 reads the OLD prevVisible then rewrites it; the write
                // is the meaningful hazard (WAR after phase A's read, and the
                // cross-frame first-touch orders next frame's phase A after it).
                p.Write(prevVisible);
                p.Write(stats);
                p.Write(stats, RenderGraph::RGBufferWrite::CopyDst); // the FillBuffer reset
            }
            else
            {
                p.Read(prevVisible);
            }
        },
        [pipeline, sampler, instances, visibility, prevVisible, stats, hzb, cdBuf = cd.Buffer,
         cdOff = cd.Offset, pc, instanceBytes, visBytes, prevVisibleBytes, statsBytes, isPhaseB,
         instanceCount = sliceInstanceCount](RenderGraph::RGContext& ctx)
        {
            DescriptorSetDesc setDesc{};
            setDesc.layout = MakeHzbCullingDescriptorSetLayout();
            setDesc.transient = true;
            setDesc.debugName = "GPUCulling_HzbCull_DS0";
            DescriptorSetHandle set = ctx.GetDevice()->CreateDescriptorSet(setDesc);
            if (!set.IsValid())
                return;

            if (isPhaseB)
            {
                // Zero the stats before the atomic-accumulate; FillBuffer is
                // Transfer, the dispatch is Compute — intra-pass hazard, so the
                // manual barrier stays (RG auto-barriers are inter-pass).
                ctx.Cmd->FillBuffer(ctx.GetBuffer(stats), 0, statsBytes, 0u);
                ResourceBarrier clearBarrier = ResourceBarrier::CreateMemoryBarrier(
                    static_cast<uint64_t>(PipelineStageMask::Transfer),
                    static_cast<uint64_t>(PipelineStageMask::ComputeShader),
                    static_cast<uint64_t>(ResourceAccessMask::TransferWrite),
                    static_cast<uint64_t>(ResourceAccessMask::ShaderRead) |
                        static_cast<uint64_t>(ResourceAccessMask::ShaderWrite));
                ctx.Cmd->Barrier(clearBarrier);
            }

            ctx.GetDevice()->UpdateStorageBufferBinding(set, 0, ctx.GetBuffer(instances), 0,
                                                        instanceBytes);
            ctx.GetDevice()->UpdateStorageBufferBinding(set, 1, cdBuf, cdOff, sizeof(GPUCullingData));
            ctx.GetDevice()->UpdateStorageBufferBinding(set, 2, ctx.GetBuffer(visibility), 0, visBytes);
            ctx.GetDevice()->UpdateStorageBufferBinding(set, 3, ctx.GetBuffer(prevVisible), 0,
                                                        prevVisibleBytes);
            ctx.GetDevice()->UpdateCombinedImageSamplerBinding(set, 4, ctx.GetTexture(hzb), sampler);
            ctx.GetDevice()->UpdateStorageBufferBinding(set, 5, ctx.GetBuffer(stats), 0, statsBytes);

            ctx.Cmd->SetPipeline(pipeline);
            ctx.Cmd->BindDescriptorSet(0, set, pipeline);
            ctx.Cmd->SetPushConstants(pc);
            ctx.Cmd->Dispatch((instanceCount + 63u) / 64u, 1, 1);
        });
}

GPUCullingPipeline::OcclusionStats GPUCullingPipeline::ReadOcclusionStats() const
{
    OcclusionStats out{};
    if (!m_Device || !m_OcclusionStatsU.IsValid())
        return out;
    // Device-local where Storage and a mappable class don't compose (see
    // ImportOcclusionStats): zeroes, not a failed map every frame.
    if (!m_Device->GetCapabilities().supportsMappableStorageBuffers)
        return out;
    if (void* mapped = m_Device->MapBuffer(m_OcclusionStatsU))
    {
        std::memcpy(&out, mapped, sizeof(out));
        m_Device->UnmapBuffer(m_OcclusionStatsU);
    }
    return out;
}

void GPUCullingPipeline::BeginFrame(RenderGraph::RGFrame* frame, GPUScene* scene)
{
    ++m_ElisionFrameStamp;
    m_ElidedThisFrame = false;
    m_CurrentFrame = frame;
    m_CurrentScene = scene;
    m_PendingViews.clear();
    m_PendingCascadeGroups.clear();
    m_ViewVisibilityRanges.clear();
    m_ViewVisibilityLayout.clear();
    m_OcclusionReservations.clear();
    m_FrameVisTotalBytes = 0;
    m_LastPublishArm = PublishArm::None;
    m_FrameRGOwner = frame;
    m_FrameVisRG = {};
    m_FrameAnyVisRG = {};
    m_FrameRuntimeVisRG = {};
}

void GPUCullingPipeline::BuildCullingElisionBlob(ElisionInputBlob& blob,
                                                 const GPUScene& scene) const
{
    // Field-by-field appends (never whole structs — padding must not enter the
    // blob). Every input the culling / union / aggregate / phase-B dispatches
    // consume is here, EXCEPT the documented exclusions:
    //  - ViewCullingInput::frameIndex / deltaTime: forwarded into
    //    GPUCullingData but consumed by no culling shader (frustum_culling.comp
    //    / hzb_culling.comp reference neither), so they must not dirty the key.
    //  - Per-slot GPUScene buffer handles: they cycle every frame by design;
    //    content identity rides GetContentEpoch (stable only once the whole
    //    slot ring converged).
    auto appendVec3 = [&blob](const Vector3& v)
    {
        blob.Append(v.x);
        blob.Append(v.y);
        blob.Append(v.z);
    };
    auto appendMat = [&blob](const Matrix4x4& m) { blob.AppendBytes(m.Data(), sizeof(float) * 16); };
    auto appendPlanes = [&blob](const Vector4* planes, size_t count)
    {
        for (size_t i = 0; i < count; ++i)
        {
            blob.Append(planes[i].x);
            blob.Append(planes[i].y);
            blob.Append(planes[i].z);
            blob.Append(planes[i].w);
        }
    };

    // Engine-injected scene epochs + module-side content generations.
    blob.Append(m_ElisionCtx.ContentEpoch);
    blob.Append(m_ElisionCtx.LightEpoch);
    blob.Append(m_ElisionCtx.DepthDynamicEpoch);
    blob.Append(scene.GetContentEpoch());
    blob.Append(scene.GetInstanceCount());
    blob.Append(scene.GetMaxInstances());

    // Physical identities: a realloc/recreate yields fresh (undefined or
    // first-touch-filled) GPU content under otherwise matching inputs.
    blob.Append(m_VisibilityBufferU.id);
    blob.Append(static_cast<uint64_t>(m_VisibilityBufferUBytes));
    blob.Append(m_AnyViewVisibleU.id);
    blob.Append(static_cast<uint64_t>(m_AnyViewVisibleUBytes));
    blob.Append(m_RuntimeVisibleU.id);
    blob.Append(m_OcclusionStatsU.id);
    blob.Append(m_HzbSentinel.id);
    // Lazily-created pipelines: an invalid→valid transition changes which
    // dispatch path a slice takes (HZB fallback, union/aggregate presence).
    blob.Append(m_VisibilityUnionPipeline.IsValid());
    blob.Append(m_RuntimeVisibilityPipeline.IsValid());
    blob.Append(m_HzbCullingPipeline.IsValid());
    // GPUScene's own culling PSOs: ScheduleCullingPassFor* silently declares
    // nothing while a PSO is unavailable, yet the gate counts the frame as
    // executed — without this term the gate settles over no-op frames
    // (startup async shader load; frustum_culling.comp hot-reload) and keeps
    // skipping after the PSO becomes available.
    blob.Append(scene.CullingPipelineValidityMask());

    blob.Append(static_cast<uint32_t>(m_PendingViews.size()));
    for (size_t i = 0; i < m_PendingViews.size(); ++i)
    {
        const ViewCullingInput& v = m_PendingViews[i];
        blob.Append(static_cast<uint32_t>(v.viewId));
        blob.Append(v.cascadeIndex);
        appendMat(v.viewMatrix);
        appendMat(v.projMatrix);
        appendMat(v.viewProjMatrix);
        appendPlanes(v.frustumPlanes, 6);
        appendVec3(v.cameraPosition);
        blob.Append(v.nearPlane);
        appendVec3(v.cameraForward);
        blob.Append(v.farPlane);
        appendVec3(v.cameraRelativeOrigin);
        blob.Append(v.firstInstance);
        blob.Append(v.instanceCount);
        blob.Append(v.renderLayerMask);
        blob.Append(v.reserveOcclusionSlice);
        if (i < m_ViewVisibilityLayout.size())
        {
            blob.Append(m_ViewVisibilityLayout[i].offset);
            blob.Append(m_ViewVisibilityLayout[i].capacity);
        }
        // HZB views additionally bind their per-view occlusion history — its
        // identity (and first-touch creation) must break equality.
        // Residual (tracked): the phase-B HZB pyramid texture and its mip
        // dims are bound by the DEFERRED phase-B schedule and are not
        // visible here, so a same-aspect window resize keeps this blob
        // byte-stable while the pyramid changes — retained P2 visibility
        // from the old resolution persists until any camera/content wake.
        if (v.reserveOcclusionSlice)
        {
            const auto it = m_PrevVisible.find(v.viewId);
            blob.Append(it != m_PrevVisible.end() ? it->second.buffer.id : 0ull);
            blob.Append(it != m_PrevVisible.end() ? static_cast<uint64_t>(it->second.bytes) : 0ull);
        }
    }

    blob.Append(static_cast<uint32_t>(m_PendingCascadeGroups.size()));
    for (const CascadeCullingGroup& g : m_PendingCascadeGroups)
    {
        blob.Append(static_cast<uint32_t>(g.viewId));
        blob.Append(g.cascadeCount);
        blob.Append(g.cascadeIndexBase);
        blob.Append(g.shadowCasterDispatch);
        for (uint32_t c = 0; c < g.cascadeCount && c < kMaxCullingViewsPerDispatch; ++c)
        {
            appendMat(g.lightVP[c]);
            appendPlanes(g.frustumPlanes[c], 6);
        }
        appendVec3(g.cameraPosition);
        blob.Append(g.nearPlane);
        appendVec3(g.cameraForward);
        blob.Append(g.farPlane);
        appendVec3(g.cameraRelativeOrigin);
        blob.Append(g.firstInstance);
        blob.Append(g.instanceCount);
        blob.Append(g.renderLayerMask);
    }

    blob.Append(static_cast<uint32_t>(m_OcclusionReservations.size()));
    for (const OcclusionSliceReservation& r : m_OcclusionReservations)
    {
        blob.Append(static_cast<uint32_t>(r.input.viewId));
        blob.Append(r.visibilityOffset);
        blob.Append(r.sliceInstanceCount);
    }
}

void GPUCullingPipeline::EndFrameRG()
{
    RenderGraph::RGFrame* frame = m_CurrentFrame;
    GPUScene* scene = m_CurrentScene;
    assert(frame && "EndFrameRG without a paired BeginFrame(RGFrame*)");
    m_CurrentFrame = nullptr;
    m_CurrentScene = nullptr;
    // Cleared before any early-out, so RecycledSlotsThisFrame never reports a
    // previous frame's set on a frame that did not reach the drain below.
    m_RecycledSlotsThisFrame.clear();
    if (!frame || !scene || !m_Device)
    {
        m_PendingViews.clear();
        m_PendingCascadeGroups.clear();
        return;
    }

    // Drain the slots GPUScene recycled this frame and fan them into EVERY
    // tracked HZB view's pending set — BEFORE the empty-views early-out and the
    // per-view loop, so a persistent view that is not submitted this frame still
    // accumulates the resets and applies them when it returns (design C1). Each
    // submitted view flushes only its own set in the phase-A branch below. Empty
    // in steady state (no removes → no recycled adds), so the bench stays
    // byte-identical.
    scene->DrainPrevVisibleResetSlots(m_PrevVisibleResetScratch);
    m_PrevVisibleResetTracker.OnSlotsRecycled(m_PrevVisibleResetScratch);
    // OnSlotsRecycled leaves the set sorted + deduped; keep it for this frame so
    // the draw stream's crossfade history can apply the same invalidation
    // without a second (impossible) drain.
    m_RecycledSlotsThisFrame = m_PrevVisibleResetScratch;

    if (m_PendingViews.empty() && m_PendingCascadeGroups.empty())
        return;

    // Per-view slice layout over one shared visibility buffer (aligned so each
    // slice can bind as a storage range — same math as the old arm).
    m_ViewVisibilityLayout.clear();
    m_ViewVisibilityLayout.reserve(m_PendingViews.size());
    uint64_t totalCapacity = 0;
    uint32_t alignElements = 1;
    {
        const size_t alignBytes = m_Device->GetCapabilities().minStorageBufferOffsetAlignment;
        if (alignBytes > sizeof(uint32_t))
            alignElements = static_cast<uint32_t>((alignBytes + sizeof(uint32_t) - 1) / sizeof(uint32_t));
    }
    auto alignUp = [alignElements](uint64_t v) -> uint64_t
    {
        if (alignElements <= 1)
            return v;
        const uint64_t a = alignElements;
        return ((v + a - 1) / a) * a;
    };
    for (const ViewCullingInput& view : m_PendingViews)
    {
        PerViewVisibilityInfo info;
        info.viewId = view.viewId;
        const uint64_t off = alignUp(totalCapacity);
        info.offset = static_cast<uint32_t>(off);
        info.capacity = view.instanceCount;
        totalCapacity = off + view.instanceCount;
        m_ViewVisibilityLayout.push_back(info);
    }
    std::vector<uint32_t> groupFirstLayoutIdx;
    groupFirstLayoutIdx.reserve(m_PendingCascadeGroups.size());
    for (const CascadeCullingGroup& g : m_PendingCascadeGroups)
    {
        groupFirstLayoutIdx.push_back(static_cast<uint32_t>(m_ViewVisibilityLayout.size()));
        for (uint32_t c = 0; c < g.cascadeCount; ++c)
        {
            PerViewVisibilityInfo info;
            info.viewId = g.viewId;
            const uint64_t off = alignUp(totalCapacity);
            info.offset = static_cast<uint32_t>(off);
            info.capacity = g.instanceCount;
            totalCapacity = off + g.instanceCount;
            m_ViewVisibilityLayout.push_back(info);
        }
    }
    // Phase-B occlusion reservations (R2.1): one extra slice per opted-in
    // view, laid out after every phase-A slice, sized into the same buffer —
    // published below but dispatched later by ScheduleOcclusionCullPass.
    for (const ViewCullingInput& view : m_PendingViews)
    {
        if (!view.reserveOcclusionSlice || view.instanceCount == 0u)
            continue;
        OcclusionSliceReservation r;
        r.input = view;
        const uint64_t off = alignUp(totalCapacity);
        r.visibilityOffset = static_cast<uint32_t>(off);
        r.sliceInstanceCount = view.instanceCount;
        totalCapacity = off + view.instanceCount;
        m_OcclusionReservations.push_back(r);
    }

    // ONE visibility buffer (the per-slot ring collapses — RenderGraph's first-touch
    // import dependency provides the cross-frame WAR ordering).
    const size_t kMinVisBytes = 4096;
    size_t requiredBytes =
        std::max(kMinVisBytes, static_cast<size_t>(std::min<uint64_t>(
                                   totalCapacity, std::numeric_limits<uint32_t>::max())) *
                                   sizeof(uint32_t));
    if (!m_VisibilityBufferU.IsValid() || m_VisibilityBufferUBytes < requiredBytes)
    {
        if (m_VisibilityBufferU.IsValid())
            m_Device->DestroyBuffer(m_VisibilityBufferU); // deferred-destroy device-side
        BufferDesc d{};
        d.size = requiredBytes;
        d.usage = static_cast<uint32_t>(BufferUsage::Storage | BufferUsage::TransferSrc |
                                        BufferUsage::TransferDst);
        d.memoryUsage = BufferMemoryUsage::DeviceLocal;
        d.debugName = "GPUCulling_Visibility.RenderGraph";
        m_VisibilityBufferU = m_Device->CreateBuffer(d);
        m_VisibilityBufferUBytes = m_VisibilityBufferU.IsValid() ? requiredBytes : 0;
    }
    if (!m_VisibilityBufferU.IsValid())
        return;
    const uint32_t visTotalBytes = static_cast<uint32_t>(m_VisibilityBufferUBytes);
    m_FrameVisTotalBytes = visTotalBytes; // deferred phase-B dispatch reads this

    m_FrameVisRG = frame->ImportExternalBuffer("GPUCulling.Visibility", m_VisibilityBufferU);
    const GPUScene::GPUSceneFrameRG sceneRG = scene->ImportFrameResources(*frame);

    // ── Idle recompute elision (lever #2): when the complete dispatch input
    // set is byte-identical for kElisionSettleFrames consecutive frames, the
    // persistent visibility buffer already holds exactly what a re-run would
    // write — skip every dispatch declaration and retain it. Layout + range
    // publication below still run (deterministic CPU; consumers re-read per
    // frame). Pending prevVisible resets force a recompute: their fill passes
    // must be declared before phase A reads the buffer. ──
    for (const ViewCullingInput& view : m_PendingViews)
    {
        if (view.reserveOcclusionSlice &&
            m_PrevVisibleResetTracker.PendingCount(view.viewId) != 0)
        {
            m_ElisionGate.ForceRecompute();
            break;
        }
    }
    ElisionInputBlob elisionInputs;
    BuildCullingElisionBlob(elisionInputs, *scene);
    const RecomputeElisionGate::Decision elide =
        m_ElisionGate.Evaluate(m_ElisionFrameStamp, std::move(elisionInputs),
                               m_ElisionCtx.AllowElision, kElisionSettleFrames);
    m_ElidedThisFrame = elide.Skip;
    // Periodic cause window (CascadeCache-log style): one line per ~600
    // evaluations for a window that lost elision, so a run that never engages
    // still names its blocker while a settled one stays quiet.
    if (m_ElisionGate.ShouldReportWindow(600) && IdleElisionLoggingEnabled())
    {
        const RecomputeElisionGate::Stats& st = m_ElisionGate.GetStats();
        Logger::Log::Info(
            "[IdleElision] Cull window: eval {} skip {} | first {} forced {} gap {} changed {} "
            "unsettled {} | epochs: content {} light {} depth {} scene {} viswrite {}",
            st.Evaluated, st.Skipped,
            st.CauseCounts[static_cast<size_t>(ElisionCause::FirstEvaluate)],
            st.CauseCounts[static_cast<size_t>(ElisionCause::Forced)],
            st.CauseCounts[static_cast<size_t>(ElisionCause::EvaluationGap)],
            st.CauseCounts[static_cast<size_t>(ElisionCause::InputsChanged)],
            st.CauseCounts[static_cast<size_t>(ElisionCause::NotSettled)],
            m_ElisionCtx.ContentEpoch, m_ElisionCtx.LightEpoch, m_ElisionCtx.DepthDynamicEpoch,
            scene->GetContentEpoch(), m_VisibilityWriteEpoch);
    }
    // Settle/unsettle edges are the same opt-in instrument as the window above.
    if (IdleElisionLoggingEnabled() && elide.Skip != m_ElisionLogState)
    {
        if (elide.Skip)
            Logger::Log::Info("[IdleElision] Cull engaged: {} view slice(s) + {} cascade group(s) "
                              "elided (inputs settled {} frames)",
                              m_PendingViews.size(), m_PendingCascadeGroups.size(),
                              m_ElisionGate.ConsecutiveMatches());
        else
            Logger::Log::Info("[IdleElision] Cull disengaged: {}",
                              ElisionDisengageReason(elide.Cause, elide.Skip));
        m_ElisionLogState = elide.Skip;
    }
    if (elide.Skip)
    {
        // Phase-B reservations are pre-consumed: ScheduleOcclusionCullPass
        // then no-ops and the phase-B slice keeps its retained content — the
        // exact bits the last executed P2 wrote for these same inputs.
        for (OcclusionSliceReservation& r : m_OcclusionReservations)
            r.consumed = true;
    }

    // Per-view dispatches.
    const size_t kMinInstanceBufferSize = 1024;
    const uint32_t instanceBytes = static_cast<uint32_t>(std::max(
        kMinInstanceBufferSize, static_cast<size_t>(scene->GetMaxInstances()) * sizeof(GPUInstance)));
    for (size_t i = 0; !elide.Skip && i < m_PendingViews.size(); ++i)
    {
        const ViewCullingInput& view = m_PendingViews[i];
        const PerViewVisibilityInfo& layout = m_ViewVisibilityLayout[i];
        uint32_t sliceInstanceCount = std::min(view.instanceCount, layout.capacity);
        if (sliceInstanceCount == 0u)
            continue;

        GPUCullingData cd{};
        FillCullingDataFromView(view, sliceInstanceCount, cd);

        // Phase A (mode 1) for the HZB view: frustum ∧ prevVisible, writing the
        // view's own slice. Only the reserved (non-cascade) generation opts in;
        // every frustum-only view stays on the untouched frustum path below.
        // Falls back to plain frustum if the HZB cull pipeline / prevVisible are
        // unavailable, so a missing hzb_culling.shaderpkg never blocks the view.
        if (view.reserveOcclusionSlice && EnsureHzbCullingPipeline())
        {
            const RenderGraph::RGBuffer prevVis =
                ImportPrevVisible(*frame, view.viewId, scene->GetInstanceCount());
            const RenderGraph::RGBuffer statsRG = ImportOcclusionStats(*frame);
            const RenderGraph::RGTexture sentinel = ImportHzbSentinel(*frame);
            const auto pvIt = m_PrevVisible.find(view.viewId);
            if (prevVis.IsValid() && statsRG.IsValid() && sentinel.IsValid() &&
                pvIt != m_PrevVisible.end())
            {
                cd.hzbMode = kHzbModePhaseA;
                cd.hzbMipCount = 1u; // unused in mode 1 (never samples the sentinel)
                // Flush THIS view's pending resets to 0xFFFFFFFF BEFORE phase A
                // reads prevVisible (the RG RAW edge orders this write first), so
                // a slot a new instance reused is treated as visible-unknown,
                // never inheriting the prior tenant's occlusion state (C1).
                // Consuming only this view's set clears it, leaving other views'
                // pending intact until they next schedule phase A. No-op when
                // this view had nothing pending.
                m_PrevVisibleResetTracker.TakeViewPending(view.viewId, m_PrevVisibleResetScratch);
                PrevVisibleResetTracker::CoalesceResetRuns(m_PrevVisibleResetScratch,
                                                          m_PrevVisibleResetRuns);
                SchedulePrevVisibleResetPass(*frame, prevVis,
                                             static_cast<uint32_t>(pvIt->second.bytes),
                                             m_PrevVisibleResetRuns);
                const uint64_t keyA =
                    (static_cast<uint64_t>(view.viewId) << 16) | kOcclusionPhaseANameCode;
                ScheduleHzbCullPass(*frame, sceneRG.Instances, instanceBytes, m_FrameVisRG, prevVis,
                                    static_cast<uint32_t>(pvIt->second.bytes), statsRG, sentinel, cd,
                                    layout.offset, view.firstInstance, sliceInstanceCount, keyA);
                continue;
            }
        }

        const uint64_t key = (static_cast<uint64_t>(view.viewId) << 16) |
                             static_cast<uint64_t>(view.cascadeIndex);
        scene->ScheduleCullingPassForRange(*frame, sceneRG, m_FrameVisRG, visTotalBytes,
                                           layout.offset, view.firstInstance, sliceInstanceCount,
                                           cd, key);
    }

    // Fused cascade-group dispatches.
    for (size_t gi = 0; !elide.Skip && gi < m_PendingCascadeGroups.size(); ++gi)
    {
        const CascadeCullingGroup& g = m_PendingCascadeGroups[gi];
        if (g.instanceCount == 0u || g.cascadeCount == 0u)
            continue;

        GPUCullingData cd{};
        cd.viewProjMatrix = g.lightVP[0]; // RenderDoc legibility only
        for (uint32_t c = 0; c < g.cascadeCount; ++c)
            for (int p = 0; p < 6; ++p)
                cd.frustumPlanes[c][p] = g.frustumPlanes[c][p];
        // Camera-relative shadow-caster culling (Earth-scale precision): the same
        // origin the world pass uses, translating each live cascade's planes and
        // fed to the shader via cameraPosition. Origin (0,0,0) => byte-identical.
        cd.cameraPosition = g.cameraRelativeOrigin;
        for (uint32_t c = 0; c < g.cascadeCount; ++c)
            MakeFrustumPlanesCameraRelative(cd.frustumPlanes[c], g.cameraRelativeOrigin);
        cd.nearPlane = g.nearPlane;
        cd.cameraForward = g.cameraForward;
        cd.farPlane = g.farPlane;
        cd.firstInstance = g.firstInstance;
        cd.instanceCount = g.instanceCount;
        cd.renderLayerMask = g.renderLayerMask;
        cd.frameIndex = g.frameIndex;
        cd.deltaTime = g.deltaTime;
        // Shadow groups drop non-casters; a color fan-out (probe faces) keeps
        // everything, matching the single-view dispatch above.
        cd.cullShadowCasters = g.shadowCasterDispatch ? 1u : 0u;
        // Cascade 0 keeps the conservative margin (closest shadows, edge pops
        // most visible); distant cascades 1..3 run tight — the 50% inflation
        // was passing ~50% extra casters into every cascade slice (SCALE-11).
        // Color fan-outs stay conservative on every slice — a false cull pops.
        for (uint32_t m = 0; m < kMaxViewsPerCullingDispatch; ++m)
            cd.cullMargins[m] = (m == 0u || !g.shadowCasterDispatch) ? kCullMarginConservative
                                                                    : kCullMarginTightCascade;

        // Pass-name key: the directional cascades keep the 0xFE tag; a based
        // fan-out names itself by its first slice so two groups of one view
        // (probe faces 0-3 and 4-5) stay distinguishable in a capture.
        const uint64_t groupTag = g.cascadeIndexBase != 0 ? g.cascadeIndexBase : 0xFEull;
        const uint64_t key = (static_cast<uint64_t>(g.viewId) << 16) | groupTag;
        const uint32_t firstIdx = groupFirstLayoutIdx[gi];
        uint32_t sliceOffsets[kMaxCullingViewsPerDispatch] = {};
        for (uint32_t c = 0; c < g.cascadeCount; ++c)
            sliceOffsets[c] = m_ViewVisibilityLayout[firstIdx + c].offset;

        scene->ScheduleCullingPassForCascadeGroup(*frame, sceneRG, m_FrameVisRG, visTotalBytes,
                                                  sliceOffsets, g.cascadeCount, g.firstInstance,
                                                  g.instanceCount, cd, key);
    }

    // Dispatches were declared: the visibility content generation advances so
    // downstream scatter gates recompute against the fresh bits.
    if (!elide.Skip)
        ++m_VisibilityWriteEpoch;

    // Per-view ranges — the (viewId, cascadeIndex) → buffer-slice table the
    // bucketer and depth passes consume.
    m_ViewVisibilityRanges.clear();
    for (size_t i = 0; i < m_PendingViews.size(); ++i)
    {
        const ViewCullingInput& view = m_PendingViews[i];
        const PerViewVisibilityInfo& layout = m_ViewVisibilityLayout[i];
        ViewVisibilityRange range;
        range.viewId = view.viewId;
        range.cascadeIndex = view.cascadeIndex;
        range.visibilityOffset = layout.offset;
        range.visibilityCount = std::min(view.instanceCount, layout.capacity);
        m_ViewVisibilityRanges.push_back(range);
    }
    for (size_t gi = 0; gi < m_PendingCascadeGroups.size(); ++gi)
    {
        const CascadeCullingGroup& g = m_PendingCascadeGroups[gi];
        const uint32_t firstIdx = groupFirstLayoutIdx[gi];
        for (uint32_t c = 0; c < g.cascadeCount; ++c)
        {
            const PerViewVisibilityInfo& layout = m_ViewVisibilityLayout[firstIdx + c];
            ViewVisibilityRange range;
            range.viewId = g.viewId;
            range.cascadeIndex = static_cast<uint8_t>(g.cascadeIndexBase + c);
            range.visibilityOffset = layout.offset;
            range.visibilityCount = std::min(g.instanceCount, layout.capacity);
            m_ViewVisibilityRanges.push_back(range);
        }
    }
    for (const OcclusionSliceReservation& r : m_OcclusionReservations)
    {
        ViewVisibilityRange range;
        range.viewId = r.input.viewId;
        range.cascadeIndex = r.input.cascadeIndex;
        range.slicePhase = 1u;
        range.visibilityOffset = r.visibilityOffset;
        range.visibilityCount = r.sliceInstanceCount;
        m_ViewVisibilityRanges.push_back(range);
    }

    m_LastPublishArm = PublishArm::RenderGraph;
}

void GPUCullingPipeline::ScheduleOcclusionCullPass(RenderGraph::RGFrame& frame, GPUScene* scene,
                                                   ViewId viewId, RenderGraph::RGTexture hzb,
                                                   uint32_t hzbMipCount)
{
    // Same owner-frame rule as the union/aggregate epilogue passes: dense
    // RGBuffer ids from frame A are usually in-range (and silently wrong) in
    // frame B.
    if (!m_Device || !scene || m_FrameRGOwner != &frame || !m_FrameVisRG.IsValid())
        return;
    // No HZB pyramid (node unwired / no prior HZBBuild) or no HZB cull pipeline:
    // leave the reservation UNCONSUMED and no-op. Phase B's slice stays as phase
    // A left it, and a later call with a valid pyramid can still dispatch.
    if (!hzb.IsValid() || !EnsureHzbCullingPipeline())
        return;

    for (OcclusionSliceReservation& r : m_OcclusionReservations)
    {
        if (r.input.viewId != viewId || r.consumed)
            continue;
        r.consumed = true;

        // Phase B (mode 2): the real occlusion test against the fresh pyramid.
        // Draws only what phase A didn't and rewrites prevVisible (§5-A2). The
        // seam's ordering is real: this WRITES the visibility buffer the phase-A
        // scatter READ (WAR after), READS the HZB (RAW after HZBBuild), and the
        // phase-B scatter READS this (RAW before).
        const RenderGraph::RGBuffer prevVis =
            ImportPrevVisible(frame, r.input.viewId, scene->GetInstanceCount());
        const RenderGraph::RGBuffer statsRG = ImportOcclusionStats(frame);
        const auto pvIt = m_PrevVisible.find(r.input.viewId);
        if (!prevVis.IsValid() || !statsRG.IsValid() || pvIt == m_PrevVisible.end())
            return;

        GPUCullingData cd{};
        FillCullingDataFromView(r.input, r.sliceInstanceCount, cd);
        cd.hzbMode = kHzbModePhaseB;
        cd.hzbMipCount = std::max(1u, hzbMipCount);

        const GPUScene::GPUSceneFrameRG sceneRG = scene->ImportFrameResources(frame); // dedup'd
        const size_t kMinInstanceBufferSize = 1024;
        const uint32_t instanceBytes = static_cast<uint32_t>(
            std::max(kMinInstanceBufferSize,
                     static_cast<size_t>(scene->GetMaxInstances()) * sizeof(GPUInstance)));
        const uint64_t key =
            (static_cast<uint64_t>(r.input.viewId) << 16) | kOcclusionPhaseBNameCode;
        ScheduleHzbCullPass(frame, sceneRG.Instances, instanceBytes, m_FrameVisRG, prevVis,
                            static_cast<uint32_t>(pvIt->second.bytes), statsRG, hzb, cd,
                            r.visibilityOffset, r.input.firstInstance, r.sliceInstanceCount, key);
        // Phase B writes the visibility buffer (and rewrites prevVisible):
        // advance the content generation so the phase-B scatter gate — whose
        // call follows this dispatch in declaration order — recomputes.
        ++m_VisibilityWriteEpoch;
    }
}

RenderGraph::RGBuffer GPUCullingPipeline::ImportRuntimeVisible(RenderGraph::RGFrame& frame)
{
    if (!m_Device)
        return {};
    const bool freshlyCreated = !m_RuntimeVisibleU.IsValid();
    if (freshlyCreated)
    {
        BufferDesc d{};
        d.size = kRuntimeVisibleCapacity * sizeof(uint32_t);
        d.usage = static_cast<uint32_t>(BufferUsage::Storage | BufferUsage::TransferDst);
        d.memoryUsage = BufferMemoryUsage::DeviceLocal;
        d.debugName = "GPUCulling_RuntimeVisible.RenderGraph";
        m_RuntimeVisibleU = m_Device->CreateBuffer(d);
    }
    if (!m_RuntimeVisibleU.IsValid())
        return {};
    m_FrameRuntimeVisRG = frame.ImportExternalBuffer("GPUCulling.RuntimeVisible", m_RuntimeVisibleU);
    if (freshlyCreated)
    {
        // First-touch init, declared BEFORE the skinning pass reads the
        // buffer (RAW orders it first): the skinning gate consumes the
        // PREVIOUS frame's aggregate (deliberate WAR lag), so on the very
        // first gated frame the words are undefined device-local memory.
        // Unknown visibility must mean VISIBLE (skin everything — the
        // pre-gate behavior), not a skipped dispatch that leaves the
        // instance's reserved palette region unwritten for the draw.
        const RenderGraph::RGBuffer rvRG = m_FrameRuntimeVisRG;
        const uint32_t cap = kRuntimeVisibleCapacity;
        frame.AddPass("GPUCulling.RuntimeVisibleInit",
                      static_cast<int32_t>(PassPhase::kEarlySetup),
                      [&](RenderGraph::RGPassBuilder& p)
                      { p.Write(rvRG, RenderGraph::RGBufferWrite::CopyDst); },
                      [rvRG, cap](RenderGraph::RGContext& ctx)
                      { ctx.Cmd->FillBuffer(ctx.GetBuffer(rvRG), 0, cap * sizeof(uint32_t), 1u); });
    }
    return m_FrameRuntimeVisRG;
}

RenderGraph::RGBuffer GPUCullingPipeline::ImportVisibility(RenderGraph::RGFrame& frame)
{
    // Re-import for a NON-OWNER frame stream (slice 8a): same physical +
    // same name as EndFrameRG's owner import, so an owner-frame call dedups
    // to the id EndFrameRG minted. Deliberately does NOT touch m_FrameVisRG —
    // that value belongs to the owner frame and the union/aggregate
    // declarations key on it.
    if (!m_VisibilityBufferU.IsValid())
        return {};
    return frame.ImportExternalBuffer("GPUCulling.Visibility", m_VisibilityBufferU);
}

void GPUCullingPipeline::ScheduleVisibilityUnion(RenderGraph::RGFrame& frame, GPUScene* scene)
{
    if (!scene || !m_Device || m_ViewVisibilityRanges.empty() || !m_FrameVisRG.IsValid())
        return;
    if (&frame != m_FrameRGOwner)
        return; // frame-local ids belong to another RGFrame — never declare with them
    // Idle elision: the visibility content is retained this frame, so the
    // union it feeds is retained too (anyViewVisible is a pure function of
    // it). The union/aggregate buffer identities are part of the culling
    // blob, so a pending (re)creation always disengages the skip first.
    if (m_ElidedThisFrame)
        return;
    const uint32_t instanceCount = scene->GetInstanceCount();
    if (instanceCount == 0u)
        return;
    if (!m_VisibilityUnionPipeline.IsValid() && !CreateVisibilityUnionPipeline())
        return;

    const size_t flagsBytes = std::max<size_t>(256, static_cast<size_t>(instanceCount) * sizeof(uint32_t));
    if (!m_AnyViewVisibleU.IsValid() || m_AnyViewVisibleUBytes < flagsBytes)
    {
        if (m_AnyViewVisibleU.IsValid())
            m_Device->DestroyBuffer(m_AnyViewVisibleU);
        BufferDesc d{};
        d.size = flagsBytes;
        d.usage = static_cast<uint32_t>(BufferUsage::Storage | BufferUsage::TransferDst);
        d.memoryUsage = BufferMemoryUsage::DeviceLocal;
        d.debugName = "GPUCulling_AnyViewVisible.RenderGraph";
        m_AnyViewVisibleU = m_Device->CreateBuffer(d);
        m_AnyViewVisibleUBytes = m_AnyViewVisibleU.IsValid() ? flagsBytes : 0;
    }
    if (!m_AnyViewVisibleU.IsValid())
        return;

    // Per-view offsets via the upload ring, written at declaration (replaces
    // the per-slot ViewOffsets buffers + exec-coupled UpdateBuffer).
    uint32_t viewCount = 0;
    for (const auto& r : m_ViewVisibilityRanges)
        if (r.visibilityCount >= instanceCount)
            ++viewCount;
    if (viewCount == 0)
        return;
    const RenderGraph::RGUploadRing::Alloc offs =
        frame.AllocUpload(static_cast<uint64_t>(viewCount) * sizeof(uint32_t));
    if (!offs.Ptr)
        return;
    {
        uint32_t* dst = static_cast<uint32_t*>(offs.Ptr);
        uint32_t w = 0;
        for (const auto& r : m_ViewVisibilityRanges)
            if (r.visibilityCount >= instanceCount)
                dst[w++] = r.visibilityOffset;
    }

    m_FrameAnyVisRG = frame.ImportExternalBuffer("GPUCulling.AnyViewVisible", m_AnyViewVisibleU);

    const PipelineHandle pipeline = m_VisibilityUnionPipeline;
    const RenderGraph::RGBuffer visRG = m_FrameVisRG;
    const RenderGraph::RGBuffer anyRG = m_FrameAnyVisRG;
    const uint32_t visBytes = static_cast<uint32_t>(m_VisibilityBufferUBytes);
    const uint32_t flagsBytesU = static_cast<uint32_t>(m_AnyViewVisibleUBytes);
    const uint32_t offsBytes = viewCount * static_cast<uint32_t>(sizeof(uint32_t));

    frame.AddPass("GPUCulling.VisibilityUnion", static_cast<int32_t>(PassPhase::kEarlySetup),
                  [&](RenderGraph::RGPassBuilder& p)
                  {
                      // Declared at the spine tail (RenderServices step 5-6), AFTER
                      // the pipeline's per-view Declare — so the deferred phase-B
                      // occlusion rewrite (ScheduleOcclusionCullPass, kWorldRender)
                      // is RECORDED before this read. The RGSchedule hazard pass
                      // derives a RAW edge (phase-B write -> this read) from that
                      // recording order, and dependency order wins over the Phase
                      // tiebreak: this read lands after phase B even though the
                      // union sits at kEarlySetup and phase B at kWorldRender. So
                      // the union observes the POST-B visibility for an HZB view —
                      // the phase-B recovered slice (h & !drawnInA, §5-A2) OR'd with
                      // phase A's drawnInA (= frustum & prevVisible), whose union is
                      // the true visible set h = frustum & HZB (reverse-Z MIN test),
                      // NOT the stale phase-A generation alone. The skinning gate
                      // consumes the aggregate this feeds one frame later; hoisting
                      // this read in front of phase B would hand it the pre-occlusion
                      // set and snap a just-revealed skinned actor's frozen palette.
                      // Locked by RGGpuDriven.VisibilityUnionReadsPostPhaseBSliceForHzbView.
                      p.Read(visRG);
                      p.Write(anyRG);
                  },
                  [pipeline, visRG, anyRG, offsBuf = offs.Buffer, offsOff = offs.Offset, visBytes,
                   flagsBytesU, offsBytes, instanceCount, viewCount](RenderGraph::RGContext& ctx)
                  {
                      DescriptorSetDesc setDesc{};
                      DescriptorSetLayoutDesc layoutDesc{};
                      layoutDesc.debugName = "VisibilityUnion_SetLayout";
                      for (uint32_t i = 0; i < 3; ++i)
                      {
                          DescriptorBinding b{};
                          b.binding = i;
                          b.type = DescriptorType::StorageBuffer;
                          b.count = 1;
                          b.shaderStages = kShaderStageCompute;
                          layoutDesc.bindings.push_back(b);
                      }
                      setDesc.layout = layoutDesc;
                      setDesc.transient = true;
                      setDesc.debugName = "VisibilityUnion_DS0";
                      DescriptorSetHandle set = ctx.GetDevice()->CreateDescriptorSet(setDesc);
                      if (!set.IsValid())
                          return;
                      ctx.GetDevice()->UpdateStorageBufferBinding(set, 0, ctx.GetBuffer(visRG), 0, visBytes);
                      ctx.GetDevice()->UpdateStorageBufferBinding(set, 1, ctx.GetBuffer(anyRG), 0, flagsBytesU);
                      ctx.GetDevice()->UpdateStorageBufferBinding(set, 2, offsBuf, offsOff, offsBytes);
                      ctx.Cmd->SetPipeline(pipeline);
                      ctx.Cmd->BindDescriptorSet(0, set, pipeline);
                      VisibilityUnionPC pc{};
                      pc.instanceCount = instanceCount;
                      pc.viewCount = viewCount;
                      ctx.Cmd->SetPushConstants(pc);
                      ctx.Cmd->Dispatch((instanceCount + 63u) / 64u, 1, 1);
                  });
}

void GPUCullingPipeline::ScheduleRuntimeVisibilityAggregate(RenderGraph::RGFrame& frame, GPUScene* scene)
{
    if (!scene || !m_Device || !m_FrameAnyVisRG.IsValid())
        return;
    if (&frame != m_FrameRGOwner)
        return; // frame-local ids belong to another RGFrame — never declare with them
    // Idle elision: retained visibility ⇒ retained aggregate (runtimeVisible
    // is a pure function of anyViewVisible + the instance table, both keyed
    // in the culling blob). The skinning gate keeps reading the retained
    // buffer through its own per-frame import.
    if (m_ElidedThisFrame)
        return;
    // BeginFrame(RGFrame*) clears the frame-locals AFTER the skinning arm's
    // ImportRuntimeVisible ran (contractual order: skinning before culling) —
    // re-import here; dedup-by-handle returns the SAME id skinning read, so
    // the WAR edge still pins skinning before this write.
    if (!m_FrameRuntimeVisRG.IsValid())
        ImportRuntimeVisible(frame);
    if (!m_FrameRuntimeVisRG.IsValid())
        return;
    const uint32_t instanceCount = scene->GetInstanceCount();
    if (instanceCount == 0u)
        return;
    if (!m_RuntimeVisibilityPipeline.IsValid() && !CreateRuntimeVisibilityPipeline())
        return;

    const GPUScene::GPUSceneFrameRG sceneRG = scene->ImportFrameResources(frame); // dedup'd
    const PipelineHandle pipeline = m_RuntimeVisibilityPipeline;
    const RenderGraph::RGBuffer anyRG = m_FrameAnyVisRG;
    const RenderGraph::RGBuffer rvRG = m_FrameRuntimeVisRG;
    const uint32_t cap = kRuntimeVisibleCapacity;
    const uint32_t flagsBytesU = static_cast<uint32_t>(m_AnyViewVisibleUBytes);
    const size_t instBytes =
        std::max<size_t>(1024, static_cast<size_t>(scene->GetMaxInstances()) * sizeof(GPUInstance));

    frame.AddPass("GPUCulling.RuntimeVisibilityAggregate",
                  static_cast<int32_t>(PassPhase::kEarlySetup),
                  [&](RenderGraph::RGPassBuilder& p)
                  {
                      p.Read(anyRG);
                      p.Read(sceneRG.Instances);
                      p.Write(rvRG); // recorded AFTER skinning's Read — the WAR pins order
                      // The exec OPENS with a Transfer-stage FillBuffer: the
                      // CopyDst union puts Transfer in the barrier dst scope,
                      // so the WAR against skinning's read (and the cross-
                      // frame first-touch dep) covers the fill, not just the
                      // compute dispatch. The intra-pass Transfer→Compute
                      // barrier in the exec handles fill-before-dispatch.
                      p.Write(rvRG, RenderGraph::RGBufferWrite::CopyDst);
                  },
                  [pipeline, anyRG, rvRG, instances = sceneRG.Instances, cap, flagsBytesU, instBytes,
                   instanceCount](RenderGraph::RGContext& ctx)
                  {
                      BufferHandle rv = ctx.GetBuffer(rvRG);
                      // Zero before atomic-ORs accumulate; FillBuffer is Transfer
                      // stage, the dispatch is Compute — intra-pass hazard, so the
                      // manual barrier stays (RG auto-barriers are inter-pass).
                      ctx.Cmd->FillBuffer(rv, 0, cap * sizeof(uint32_t), 0u);
                      ResourceBarrier clearBarrier = ResourceBarrier::CreateMemoryBarrier(
                          static_cast<uint64_t>(PipelineStageMask::Transfer),
                          static_cast<uint64_t>(PipelineStageMask::ComputeShader),
                          static_cast<uint64_t>(ResourceAccessMask::TransferWrite),
                          static_cast<uint64_t>(ResourceAccessMask::ShaderRead) |
                              static_cast<uint64_t>(ResourceAccessMask::ShaderWrite));
                      ctx.Cmd->Barrier(clearBarrier);

                      DescriptorSetDesc setDesc{};
                      DescriptorSetLayoutDesc layoutDesc{};
                      layoutDesc.debugName = "RuntimeVisibility_SetLayout";
                      for (uint32_t i = 0; i < 3; ++i)
                      {
                          DescriptorBinding b{};
                          b.binding = i;
                          b.type = DescriptorType::StorageBuffer;
                          b.count = 1;
                          b.shaderStages = kShaderStageCompute;
                          layoutDesc.bindings.push_back(b);
                      }
                      setDesc.layout = layoutDesc;
                      setDesc.transient = true;
                      setDesc.debugName = "RuntimeVisibility_DS0";
                      DescriptorSetHandle set = ctx.GetDevice()->CreateDescriptorSet(setDesc);
                      if (!set.IsValid())
                          return;
                      ctx.GetDevice()->UpdateStorageBufferBinding(set, 0, ctx.GetBuffer(anyRG), 0, flagsBytesU);
                      ctx.GetDevice()->UpdateStorageBufferBinding(set, 1, ctx.GetBuffer(instances), 0, instBytes);
                      ctx.GetDevice()->UpdateStorageBufferBinding(set, 2, rv, 0, cap * sizeof(uint32_t));
                      ctx.Cmd->SetPipeline(pipeline);
                      ctx.Cmd->BindDescriptorSet(0, set, pipeline);
                      RuntimeVisibilityPC pc{};
                      pc.instanceCount = instanceCount;
                      pc.runtimeVisibleCap = cap;
                      ctx.Cmd->SetPushConstants(pc);
                      ctx.Cmd->Dispatch((instanceCount + 63u) / 64u, 1, 1);
                      // Make atomicOr results visible to the downstream skinning
                      // compute (same-pass tail barrier, kept verbatim).
                      ResourceBarrier readyBarrier = ResourceBarrier::CreateMemoryBarrier(
                          static_cast<uint64_t>(PipelineStageMask::ComputeShader),
                          static_cast<uint64_t>(PipelineStageMask::ComputeShader),
                          static_cast<uint64_t>(ResourceAccessMask::ShaderWrite),
                          static_cast<uint64_t>(ResourceAccessMask::ShaderRead));
                      ctx.Cmd->Barrier(readyBarrier);
                  });
}

std::unique_ptr<GPUCullingPipeline> GPUCullingFactory::CreateBalanced(IDevice* device)
{
    GPUCullingConfig config{};
    config.enableFrustumCulling = true;
    config.enableOcclusionCulling = false;
    config.maxCullingThreads = 64;
    return std::make_unique<GPUCullingPipeline>(device, config);
}

std::unique_ptr<GPUCullingPipeline> GPUCullingFactory::CreateHighPerformance(IDevice* device)
{
    GPUCullingConfig config{};
    config.enableFrustumCulling = true;
    config.enableOcclusionCulling = true;
    config.maxCullingThreads = 128;
    return std::make_unique<GPUCullingPipeline>(device, config);
}

std::unique_ptr<GPUCullingPipeline> GPUCullingFactory::CreateQuality(IDevice* device)
{
    GPUCullingConfig config{};
    config.enableFrustumCulling = true;
    config.enableOcclusionCulling = true;
    config.maxCullingThreads = 64;
    return std::make_unique<GPUCullingPipeline>(device, config);
}

std::unique_ptr<GPUCullingPipeline> GPUCullingFactory::CreateCustom(IDevice* device, const GPUCullingConfig& config)
{
    return std::make_unique<GPUCullingPipeline>(device, config);
}

} // namespace Rendering
} // namespace GameEngine
