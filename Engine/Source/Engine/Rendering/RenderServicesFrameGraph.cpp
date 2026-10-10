// RenderServicesFrameGraph.cpp
// Part of the RenderServices implementation — split by concern from the
// former single RenderServices.cpp. All files define members of the same
// RenderServices class; shared file-scope helpers live in RenderServicesDetail.h.
#include "Engine/Rendering/RenderServices.h"
#include "UI/UITextureSpace.h"
#include "Core/Time.h"
#include "Core/CpuProfiler.h"
#include "Engine/Rendering/IRenderFeature.h"

#include "Core/DebugMetrics.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MaterialBinder.h"
#include "Engine/Rendering/MaterialCompiler.h"
#include "Engine/Rendering/ShaderGraphMaterial.h"
#include "Rendering/Geometry/VertexLayoutBuilder.h"
#include "Rendering/Materials/MaterialBuildService.h"
#include "Rendering/Materials/MaterialParamsLayout.h"
#include "Rendering/Materials/MaterialBuilder.h"
#include "Assets/AssetManager.h"
#include "Assets/BinaryAsset.h"
#include "Assets/MaterialAsset.h"
#include "Assets/ModelAsset.h"
#include "Assets/TextureAsset.h"
#include "Engine/Rendering/EmbeddedImageDecoder.h"
#include "Core/Application.h"
#include "Core/Engine.h"
#include "Engine/Rendering/ImageBasedLightingFeature.h"
#include "Engine/Rendering/IEnvironmentSource.h"
#include "Engine/Rendering/SkyRenderFeature.h"
#include "Engine/Rendering/RetargetRenderFeature.h"
#include "Engine/Rendering/Pipeline/RenderPipeline.h"
#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"
#include "Logger/Logger.h"
#include "Rendering/Common/Utils.h"
#include "Rendering/Core/BindlessResourceManager.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/DeviceFormatting.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/CameraDerivation.h"
#include "Rendering/Core/CullingStrategy.h"
#include "Rendering/Core/FrustumCullingStrategy.h"
#include "Rendering/Core/GPUCulling.h"
#include "Rendering/Core/GPUDrawStreamBuilder.h"
#include "Rendering/Core/GPUScene.h"
#include "Rendering/Materials/MaterialHelper.h"
#include "Rendering/Materials/MaterialBuilder.h"
#include "Rendering/Materials/ShaderReflection.h"
#include "Rendering/Common/Utils.h"
#include "Rendering/Passes/SRGBEncodePass.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include "Rendering/Utils/BufferHelpers.h"
#include "Rendering/Utils/TextureUploadHelpers.h"
#include "Rendering/Utils/CubeLutFileParser.h"
#include "Rendering/Utils/CubeLutGpuUpload.h"
#include "Rendering/Core/NamedPushConstantWriter.h"
#include "Types/StringId.h"
#include "Mathematics/Matrix4x4.h"
#include "Mathematics/MatrixOps.h"
#include "Rendering/Common/Frustum.h"
#include "Rendering/Common/MatrixUtils.h"

#include "Rendering/Core/ThreadingUtils.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cassert>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "RenderServicesDetail.h"

namespace GameEngine
{
namespace Engine::Renderer
{
using namespace ::GameEngine::Rendering;

namespace
{
// Wrap window for the bounded scroll clock (uTimeParams.y). A whole-second period
// keeps tile-periodic UV panners seamless across the wrap (speed * period lands on
// whole tiles for scroll speeds authored to millisecond granularity), while the
// bounded magnitude preserves fp32 sub-frame precision indefinitely — an unbounded
// accumulator's ULP eventually exceeds a frame's delta and steps/stalls the scroll.
constexpr float kScrollAnimationPeriodSeconds = 1000.0f;

} // namespace

// Shader animation is part of the captured frame, so it reads the engine frame
// clock rather than wall time (movie capture uses a fixed recording delta, and
// backpressure can stall rendering beyond one frame — wall time would make wind
// jump between adjacent recorded frames). Derived from the process-global
// Time::GetCumulativeSeconds() so animation phase persists across a
// RenderServices destroy/recreate (device-lost recovery, window rebuild)
// instead of resetting to zero.
float RenderServices::GetShaderAnimationTimeSeconds() const
{
    return static_cast<float>(Time::GetCumulativeSeconds());
}

float RenderServices::GetScrollAnimationTimeSeconds() const
{
    return static_cast<float>(
        std::fmod(Time::GetCumulativeSeconds(), static_cast<double>(kScrollAnimationPeriodSeconds)));
}

Rendering::RenderGraph::RGTexture RenderServices::GetShadowMapArrayRG(Rendering::RenderGraph::RGFrame& frame,
                                                              Rendering::ViewId viewId) const
{
    const ViewFrameRG* vfr = FindViewFrameRGFor(frame, viewId);
    return vfr ? vfr->ShadowMapArray : Rendering::RenderGraph::RGTexture{};
}

RenderServices::LocalShadowInputsRG RenderServices::GetLocalShadowInputsRG(
    Rendering::RenderGraph::RGFrame& frame, Rendering::ViewId viewId) const
{
    LocalShadowInputsRG inputs{};
    if (const auto* vfr = FindViewFrameRGFor(frame, viewId))
    {
        if (vfr->SpotShadowMap.IsValid() && vfr->SpotShadowData.Valid())
        {
            inputs.SpotMap = vfr->SpotShadowMap;
            inputs.SpotData = vfr->SpotShadowData.Buffer;
            inputs.SpotOffset = vfr->SpotShadowData.Offset;
        }
        if (vfr->PointShadowMap.IsValid() && vfr->PointShadowData.Valid())
        {
            inputs.PointMap = vfr->PointShadowMap;
            inputs.PointData = vfr->PointShadowData.Buffer;
            inputs.PointOffset = vfr->PointShadowData.Offset;
            inputs.PointBytes = vfr->PointShadowData.Bytes;
        }
    }
    return inputs;
}

RenderServices::PipelineOutputRG RenderServices::GetPipelineOutputRG(
    Rendering::RenderGraph::RGFrame& frame, Rendering::ViewId viewId) const
{
    PipelineOutputRG result{};
    auto* instance = m_FrameOrchestrator.PipelineInstanceForFrame(frame);
    if (!instance)
        return result;
    // Frame-identity guard: a blackboard declared against another frame (or
    // a stale incarnation of this one) holds dead frame-local ids.
    if (!instance->FrameResourcesFor(&frame))
        return result;
    result.Out = instance->GetOutputRG(viewId, "FinalColor");
    if (result.Out.IsValid())
        result.Physical = frame.PhysicalTexture(result.Out);
    return result;
}

::GameEngine::UI::UITextureSpace RenderServices::GetPipelineOutputSpaceRG() const
{
    // Producer stamp (#767): the same device mode the pipeline's Tonemap keys
    // its outEncoding on this frame (tonemap.frag: SDR arms emit
    // display-referred linear, the HDR arm passes paper-white-relative linear
    // through). The mode is frame-stable, so reading it on the producing side
    // states what the pipeline actually wrote for the frame being declared —
    // never what a later consumer's display is doing. This is the one place
    // that turns an output mode into a space; consumers carry or store the
    // stamp.
    return m_Device && Rendering::IsHdrOutputModeActive(m_Device->GetActiveHdrOutputMode())
               ? ::GameEngine::UI::UITextureSpace::HdrLinear()
               : ::GameEngine::UI::UITextureSpace::DisplayLinearSdr();
}

void RenderServices::AddFrameMover(uint32_t instanceIndex, Rendering::MeshGPUHandle meshHandle,
                                   uint32_t skinPaletteOffset, uint32_t prevSkinPaletteOffset)
{
    std::lock_guard lock(m_FrameMoversMutex);
    m_FrameMovers.push_back(
        {instanceIndex, meshHandle, skinPaletteOffset, prevSkinPaletteOffset});
}

void RenderServices::AddFrameMovers(std::span<const FrameMoverRecord> movers)
{
    if (movers.empty())
        return;
    std::lock_guard lock(m_FrameMoversMutex);
    m_FrameMovers.insert(m_FrameMovers.end(), movers.begin(), movers.end());
}

void RenderServices::NotifyUnversionedMotion(Rendering::ViewId viewId)
{
    std::lock_guard lock(m_FrameMoversMutex);
    if (std::find(m_UnversionedMotionViews.begin(), m_UnversionedMotionViews.end(), viewId) ==
        m_UnversionedMotionViews.end())
        m_UnversionedMotionViews.push_back(viewId);
}

bool RenderServices::HasUnversionedMotion(Rendering::ViewId viewId) const
{
    const auto reported = [viewId](const std::vector<Rendering::ViewId>& views)
    { return std::find(views.begin(), views.end(), viewId) != views.end(); };
    return reported(m_UnversionedMotionViews) || reported(m_PreviousUnversionedMotionViews);
}

void RenderServices::BeginWorldDrawFrame()
{
    // Per-view per-frame resets coalesce into ViewRegistry::BeginFrame at the
    // step-1 position: OnDemand participation aging (expiring lapsed requests
    // before extraction), the pipeline-view flag sets, per-view WorldPassKeywords,
    // and the per-view shadow-resource map. CONTRACT: BeginWorldDrawFrame runs
    // exactly once per app frame (RenderingLoop::Update) and NEVER from the
    // per-window BuildFrameGraph — moving these resets there would let window 1
    // wipe window 0's per-frame view flags mid-frame.
    m_ViewRegistry.BeginFrame();

    // Fresh movers list for this frame's extraction (TAA motion-vector pass).
    {
        std::lock_guard lock(m_FrameMoversMutex);
        m_FrameMovers.clear();
        m_PreviousUnversionedMotionViews.swap(m_UnversionedMotionViews);
        m_UnversionedMotionViews.clear();
    }

    // Rotate GPUScene's frame slot at the top of the per-frame ECS tick so
    // every buffer accessor (GetInstanceBuffer / GetMeshBuffer / etc.)
    // returns the current frame's physical buffer for the remainder of the
    // frame. Mutations from extraction land into the new slot's CPU vector
    // and the upload step (in BuildFrameGraph after BuildWorldBatchKeys)
    // pushes them to GPU. Idempotent within a device frame: AdvanceFrameSlot
    // reads m_Device->GetFrameIndex() so multiple worlds / RGs ticking in
    // the same device frame all produce the same slot.
    if (m_GpuScene)
        m_GpuScene->AdvanceFrameSlot();

    // App-frame tick (spine half): epoch bump + blackboard ABA resets + the
    // three RS-resident frame-local clears (culling gate, m_FrameRG, m_ViewFrameRG).
    m_FrameOrchestrator.BeginAppFrame();
    // The per-frame pipeline-view flag clears + per-view WorldPassKeywords reset
    // moved into ViewRegistry::BeginFrame (called at the top of this function),
    // coalesced with the OnDemand participation aging and the shadow-resource
    // clear — all observation-equivalent at the step-1 position.

    // Run deferred asset invalidations first: evictions queue rebinds that
    // the upload flush below then services in the same frame.
    DrainPendingAssetInvalidations();
    m_Textures->FlushPendingUploads();

    // Material frame reset (A1.3 §1.2): variant-cache eviction drain (stale rows
    // keyed on freed Material* addresses must not survive into this frame's pass
    // record) then binder frame-begin (set-0/set-1 handles from the prior frame's
    // transient pool must not be handed out). Both run before any draw work, in
    // the same relative order as before the facade move.
    m_MaterialSystem.BeginFrame();

    const uint64_t frameIdx = m_GpuScene ? static_cast<uint64_t>(m_GpuScene->GetFrameIndex()) : 0ull;
    m_WorldBeginFrameIndex = frameIdx;
    m_WorldDrawListsBuiltFrameIndex = 0xFFFFFFFFFFFFFFFFull;
    m_WorldSubmissionCountThisFrame.store(0, std::memory_order_relaxed);
    // Rebuilt from this frame's submissions; a world that stops submitting
    // deforming content must stop forcing its caches to recompute.
    m_AnimatedVertexModifierWorlds.clear();
    for (auto& [worldId, lights] : m_WorldLights)
        lights.clear();
    m_WorldPostProcess.clear();
    // Cleared with the other per-world extraction state so destroyed worlds
    // (thumbnail/preview churn) can't accumulate entries; every live world's
    // extraction republishes before probes read it later the same frame.
    m_WorldRenderContentDigests.clear();
    m_WorldVolumetricFogVolumes.clear();
    m_WorldShadowSettings.clear();
    // m_ViewShadowResources.clear() moved into ViewRegistry::BeginFrame (step 8).
    m_WorldDrawBuilder.BeginFrame();

    // Advance the MeshGPURegistry frame counter so its bucket pools'
    // deferred-free queues can retire ranges that have aged out beyond
    // the in-flight window. Without this call the pools would
    // monotonically grow as meshes hot-reload over time.
    m_MeshGPURegistry.BeginFrame(frameIdx);

    // Forward DrawCommand streams reset every frame; producers refill them
    // during the emit step that runs after BuildWorldBatchKeys.
    ClearForwardCommands();
    ClearDepthCommands();
}

RenderServices::ViewFrameRG& RenderServices::ViewFrameRGFor(Rendering::RenderGraph::RGFrame& frame,
                                                            Rendering::ViewId viewId)
{
    auto& vfr = m_ViewFrameRG[viewId];
    if (!vfr.For.IsFor(frame))
    {
        vfr = ViewFrameRG{}; // foreign/stale-incarnation entry: ids are dead
        vfr.For.Stamp(frame);
    }
    return vfr;
}

const RenderServices::ViewFrameRG* RenderServices::FindViewFrameRGFor(
    Rendering::RenderGraph::RGFrame& frame, Rendering::ViewId viewId) const
{
    const auto it = m_ViewFrameRG.find(viewId);
    if (it == m_ViewFrameRG.end() || !it->second.For.IsFor(frame))
        return nullptr; // absent or stale incarnation: treat as empty
    return &it->second;
}

RenderServices::ViewFrameRG::UploadedBuffer RenderServices::UploadCompatInstanceList(
    Rendering::RenderGraph::RGFrame& frame, Rendering::ViewId viewId)
{
    ViewFrameRG& vfr = ViewFrameRGFor(frame, viewId);
    if (vfr.CompatInstanceList.Valid())
        return vfr.CompatInstanceList;
    const std::span<const uint32_t> indices = m_CpuDrawStream.GetIndexList(viewId);
    if (indices.empty())
        return {};
    const uint64_t bytes = indices.size_bytes();
    const Rendering::RenderGraph::RGUploadRing::Alloc alloc = frame.AllocUpload(bytes);
    if (!alloc.Valid())
    {
        // RGUploadRing chains an overflow block rather than drop an allocation,
        // so only a lost mapping lands here; the view's entity draws then read
        // the one-entry fallback list instead of their own and draw wrongly.
        Logger::Log::Error("RenderServices: the upload ring could not map view {}'s compat instance "
                           "list ({} bytes); its entity draws read the fallback list this frame",
                           static_cast<uint32_t>(viewId), bytes);
        return {};
    }
    std::memcpy(alloc.Ptr, indices.data(), bytes);
    vfr.CompatInstanceList = {alloc.Buffer, alloc.Offset, bytes};
    return vfr.CompatInstanceList;
}

} // namespace Engine::Renderer
} // namespace GameEngine
