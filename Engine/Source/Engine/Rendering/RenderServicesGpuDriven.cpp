// RenderServicesGpuDriven.cpp
// Part of the RenderServices implementation — split by concern from the
// former single RenderServices.cpp. All files define members of the same
// RenderServices class; shared file-scope helpers live in RenderServicesDetail.h.
#include "Engine/Rendering/RenderServices.h"
#include "Core/CpuProfiler.h"
#include "Engine/Rendering/IRenderFeature.h"
#include "Engine/Rendering/RTShadowMaskService.h"
#include "Rendering/Core/PassPhase.h"

#include "Core/DebugMetrics.h"
#include "Core/Time.h"
#include "ECSModules/Rendering/SkeletonStore.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MaterialBinder.h"
#include "Engine/Rendering/MaterialCompiler.h"
#include "Engine/Rendering/ShaderGraphMaterial.h"
#include "Engine/Rendering/PointShadowLodSelection.h"
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
#include "Engine/Rendering/ShadowMapRenderFeature.h"
#include "Engine/Rendering/RenderOrigin.h"
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
#include "Rendering/Core/GPUInstanceWorldKey.h"
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

// Build per-view LOD selection params from a resolved camera. Disables LOD
// (forces LOD0) for editor 2D ortho views (cameraPos.w == 1). projScaleY is
// |proj[1][1]| (column-major proj[5]) — the focal term used to estimate the
// bounding sphere's screen-space coverage in draw_command_scatter.comp. The
// SSE scales convert that coverage into projected-error pixels for the
// SSE-normalized threshold slots (MeshLODThresholds.h); shadow buckets pass
// their camera view's scales so shadow and view silhouettes pick the same
// level under the same budget.
static Rendering::GPUDrawStreamBuilder::ViewLODParams MakeViewLODParams(
    const Rendering::CameraData& cam, float globalBias, uint32_t forceLevel,
    float smallCullCoverage, RenderServices::ViewSseScales sseScales, float crossfadeDuration,
    float lodHysteresisBand)
{
    Rendering::GPUDrawStreamBuilder::ViewLODParams p{};
    p.cameraPos[0] = cam.cameraPos[0];
    p.cameraPos[1] = cam.cameraPos[1];
    p.cameraPos[2] = cam.cameraPos[2];
    const bool is2DOrtho = cam.cameraPos[3] == 1.0f;
    p.projScaleY    = is2DOrtho ? 0.0f : std::abs(cam.proj[5]);
    p.lodBiasGlobal = globalBias;
    p.forceLod      = forceLevel;
    p.smallCullCoverage = smallCullCoverage;
    p.sseThresholdToCoverage      = sseScales.Default;
    p.sseThresholdToCoverageTight = sseScales.Tight;
    p.crossfadeDuration           = crossfadeDuration;
    p.lodHysteresisBand           = lodHysteresisBand;
    return p;
}

RenderServices::ViewSseScales RenderServices::ComputeViewSseScales(
    Rendering::ViewId viewId) const
{
    // Last published world-pass render-target height (one frame stale across
    // a resize; 0 before the first world pass falls back inside
    // LodSseThresholdToCoverage). The budget is spent in render-target pixels, so
    // a reduced-scale view budgets in its render resolution.
    const auto* pv = m_ViewRegistry.FindPerView(viewId);
    const uint32_t viewportH = pv ? pv->WorldViewportHeight : 0u;
    // The view's class selects the budget override layered over the global
    // budget. A view with no registered desc (tools, tests) and a class whose
    // override is disabled both spend the global budget unchanged.
    const Rendering::ViewDesc* desc = m_ViewRegistry.FindViewDesc(viewId);
    const float budgetPx =
        desc != nullptr
            ? Rendering::ResolveLodBudgetPx(
                  m_LODErrorBudgetPx,
                  m_LODViewBudgetOverrides[static_cast<size_t>(desc->purpose)])
            : m_LODErrorBudgetPx;
    ViewSseScales scales;
    scales.Default = Rendering::LodSseThresholdToCoverage(viewportH, budgetPx);
    scales.Tight = Rendering::LodSseThresholdToCoverage(
        viewportH, budgetPx * m_LODSkinnedBudgetScale);
    return scales;
}

void RenderServices::SetLODViewBudgetOverride(
    Rendering::ViewPurpose purpose, const Rendering::LodViewBudgetOverride& classOverride)
{
    m_LODViewBudgetOverrides[static_cast<size_t>(purpose)] = classOverride;
}

const Rendering::LodViewBudgetOverride& RenderServices::GetLODViewBudgetOverride(
    Rendering::ViewPurpose purpose) const
{
    return m_LODViewBudgetOverrides[static_cast<size_t>(purpose)];
}

// --- RenderServices world draw builder API ---

WorldDrawBuilder& RenderServices::GetWorldDrawBuilder()
{
    return m_WorldDrawBuilder;
}

RenderServices::ScatterGroupMaps RenderServices::MeshPoolGroupMapsForScatter()
{
    // Refresh-at-schedule: the maps must cover every mesh the registry snapshot
    // (captured by the same call) can reference. Append-only GROUP ids keep
    // earlier calls' published ranges and later record-time consumer lookups
    // in agreement even when a mid-frame registration (thumbnail seam) grows
    // the map between calls; the ORDERED ranks renumber freely — each schedule
    // call uploads the span it keyed its tables on (one-snapshot discipline).
    if (!Rendering::MeshPoolGroupPlan::ConsolidationEnabled() || !m_GpuScene)
        return {};
    m_MeshPoolGroups.Refresh(m_MeshGPURegistry,
                             static_cast<uint32_t>(m_GpuScene->GetMeshes().size()));
    return {m_MeshPoolGroups.MeshToOrderedSpan(), m_MeshPoolGroups.OrderedToGroupSpan()};
}

std::span<const uint32_t> RenderServices::MeshPoolGroupSpanForDraws() const
{
    // Draw consolidation folds several batches into one indirect draw. The
    // compat path issues one direct DrawIndexed per batch, which cannot span
    // meshes, so there is nothing to consolidate and the grouping would only
    // hide batches behind a representative key.
    if (m_Profile.IsCompat())
        return {};
    if (!Rendering::MeshPoolGroupPlan::ConsolidationEnabled())
        return {};
    return m_MeshPoolGroups.MeshToGroupSpan();
}

void RenderServices::BuildWorldBatchKeys()
{
    // Refresh the P2 color-class map (no-op unless the material set changed or
    // the merge flag is off) so the derived keys, the scatter color table, and
    // the scatter routing SSBO all key on one consistent per-frame snapshot.
    // MaterialColorClassSpan() ensures the map is current before returning it.
    // A2 STEP-0 sort bracket: time the batch-key derivation + per-view std::sort
    // only (the EmitProducer* tail below is a separate concern). Always-on; one
    // steady_clock pair per app frame, no allocations. FrameOrchestrator reads
    // these last-frame values once per app frame to update the windowed means.
    const auto sortBegin = std::chrono::steady_clock::now();
    // Compat: the colour-class merge exists to share one scatter-produced draw
    // range between same-PSO materials. The compat path draws each batch with
    // its own direct draw and shares no range, and an identity map keeps
    // colorClassId == materialIndex so the CPU instance lists and the batch
    // keys agree on one key identity.
    const std::span<const uint32_t> colorClassMap =
        m_Profile.IsCompat() ? std::span<const uint32_t>{}
                             : m_MaterialSystem.MaterialColorClassSpan();
    const uint32_t sortViewCount =
        m_WorldDrawBuilder.BuildBatchKeys(GetMeshGPURegistry(), colorClassMap);
    m_RenderTimelineStats.SortMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - sortBegin).count();
    m_RenderTimelineStats.SortViewCount = sortViewCount;

    for (const auto& view : m_ViewRegistry.GetViews())
        RaiseAnimatedVertexModifierContentSignals(view.id);

    // Compat profile: the scatter never runs, so the culling and the
    // per-batch instance lists all resolve here instead. Keyed on the same
    // (materialIndex, meshIndex) identity the batch keys just derived.
    if (m_Profile.IsCompat() && m_GpuScene)
    {
        m_CpuDrawStream.Build(m_WorldDrawBuilder, GetMeshGPURegistry(), *m_GpuScene, m_ViewRegistry,
                              &GameEngine::EngineCore::GetInstance().GetJobSystem());
    }

    const uint64_t frameIdx = m_GpuScene ? static_cast<uint64_t>(m_GpuScene->GetFrameIndex()) : 0ull;
    m_WorldDrawListsBuiltFrameIndex = frameIdx;

    EmitProducerForwardCommands();
    EmitProducerDepthCommands();
}

void RenderServices::ScheduleWorldBucketerDispatches(Rendering::RenderGraph::RGFrame& frame)
{
    // Compat profile: the scatter's output is only reachable through a device
    // address, so its records would be produced and never read. The recorders
    // draw from the CPU instance lists instead.
    if (m_Profile.IsCompat())
        return;

    // Owner-spine call: exactly once per app frame (the global-stages gate).
    // Resets the scatter arena + range map — later ScheduleUnifiedScatter
    // calls in this app frame (thumbnail seam, per-view arm) APPEND. Reset
    // even when the guards below refuse: consumers then see an empty range
    // map and skip, instead of drawing stale prior-frame ranges.
    if (m_DrawStreamBuilder)
        m_DrawStreamBuilder->BeginArenaFrame();

    // Culling must have been scheduled into THIS frame first (the threaded
    // values are frame-local — the contract the GPUCullingDone tag faked).
    if (!m_FrameRG.For.IsFor(frame) || !m_FrameRG.Visibility.IsValid() ||
        !m_FrameRG.SceneInstances.IsValid() || !m_FrameRG.SceneMeshes.IsValid())
        return;
    // Cross-arm guard (symmetric to the old arm's): a later old-graph
    // EndFrame in the same frame would have re-published ranges whose
    // offsets index the per-slot buffer, not this arm's unified one.
    if (m_GpuCullingPipeline->LastPublishArm() !=
        Rendering::GPUCullingPipeline::PublishArm::RenderGraph)
        return;
    const uint32_t instanceCount = ScheduleWorldBucketerCommon();
    if (instanceCount == 0u)
        return;

    // A recycled instance slot must have its crossfade state cleared before its
    // view next reads it, or the new tenant dissolves in from the previous
    // tenant's level. The culling pipeline owns the once-per-frame drain of
    // GPUScene's recycled set (its occlusion history needs the same
    // invalidation), so read the frame's set from there rather than draining
    // twice. Owner-spine call: once per app frame, ahead of every schedule call.
    if (m_GpuCullingPipeline)
        m_DrawStreamBuilder->OnInstanceSlotsRecycled(
            m_GpuCullingPipeline->RecycledSlotsThisFrame());

    // The rendered level and phase history needs a wider invalidation than the
    // recycled set: a slot whose payload changed keeps its tenant, so a consumer
    // that pairs this frame's surface with the previous frame's must be told
    // even though nothing was recycled. GPUScene queues every slot whose
    // continuity stamp moved — new tenants included — so this one drain covers
    // both. Drained on every frame that reaches here, so the queue cannot
    // accumulate while no view holds a history.
    m_DrawStreamBuilder->OnInstanceContinuityBroken(m_GpuScene->DrainContinuityResetSlots());

    // Captured snapshot: the spine flushed GPUScene just before this call
    // (BuildFrameGraph gate), so the registry snapshot matches the instance
    // buffer content this call's dispatches read.
    RefreshScatterElisionContext();
    const ScatterGroupMaps groupMaps = MeshPoolGroupMapsForScatter();
    m_FrameRG.DrawStreamOrdering = m_DrawStreamBuilder->ScheduleUnifiedScatter(
        frame, "World", static_cast<int32_t>(Rendering::PassPhase::kEarlySetup),
        m_FrameRG.Visibility, m_FrameRG.SceneInstances, m_FrameRG.SceneScatterHot,
        m_FrameRG.SceneMeshes, m_GpuScene->GetBatchRegistry(),
        m_MaterialSystem.MaterialDepthClassSpan(),
        m_MaterialSystem.MaterialColorClassSpan(), groupMaps.MeshOrdered,
        groupMaps.OrderedToGroup, instanceCount,
        static_cast<uint32_t>(m_GpuScene->GetMeshes().size()));
}

uint32_t RenderServices::ResolveViewForceLOD(Rendering::ViewId viewId) const
{
    // Blend the per-view override (registry) with the global LOD force level.
    const uint32_t* over = m_ViewRegistry.FindViewForcedLOD(viewId);
    return over ? *over : m_LODForceLevel;
}

PointShadowLodKey RenderServices::ResolvePointShadowLodKey(Rendering::ViewId viewId) const
{
    // Authored knobs only — no camera, no viewport. The face slices select from
    // the light, so nothing here steps per frame and the atlas cache can key on
    // exact equality (see PointShadowLodKey).
    PointShadowLodKey k{};
    k.Bias = m_LODGlobalBias + m_ShadowLODBias;
    k.ForceLevel = ResolveViewForceLOD(viewId);
    k.SelectionMode = static_cast<uint32_t>(m_MeshGPURegistry.GetLodSelectionMode());
    k.ErrorBudgetPx = m_LODErrorBudgetPx;
    k.SkinnedBudgetScale = m_LODSkinnedBudgetScale;
    return k;
}

void RenderServices::UpdateIdleElisionFrameState()
{
    // Kill switches, read once (house latch idiom): GE_IDLE_ELISION=0 masters
    // everything off; GE_IDLE_ELISION_{CULL,SCATTER,CLUSTER,SDSM}=0 disable one
    // family. Default ON — every gate is exact-input and fail-recompute.
    // Telemetry caveat: a disabled gate still counts its would-be causes, but
    // the scatter gates key on the cull family's VisibilityWriteEpoch — with
    // cull disabled (or the master off) culling executes and bumps it every
    // frame, so the scatter windows report ~0% achievable. Measuring
    // scatter's own achievable rate needs GE_IDLE_ELISION_SCATTER=0 with the
    // cull family left ON. Every line those measurements read — windows and
    // engage/disengage edges — is opt-in: GE_IDLE_ELISION_LOG=1
    // (IdleElisionLoggingEnabled).
    static const bool kMaster = []
    {
        const char* v = std::getenv("GE_IDLE_ELISION");
        return !v || std::strcmp(v, "0") != 0;
    }();
    auto familyEnabled = [](const char* name)
    {
        const char* v = std::getenv(name);
        return !v || std::strcmp(v, "0") != 0;
    };
    static const bool kCull = familyEnabled("GE_IDLE_ELISION_CULL");
    static const bool kScatter = familyEnabled("GE_IDLE_ELISION_SCATTER");
    static const bool kCluster = familyEnabled("GE_IDLE_ELISION_CLUSTER");
    static const bool kSdsm = familyEnabled("GE_IDLE_ELISION_SDSM");

    m_IdleElision.AllowCull = kMaster && kCull;
    m_IdleElision.AllowScatter = kMaster && kScatter;
    m_IdleElision.AllowCluster = kMaster && kCluster;
    m_IdleElision.AllowSdsm = kMaster && kSdsm;

    // Content epoch: Σ over worlds of BOTH content versions. Monotonic per
    // world, so the sums are too. RenderContentVersion's bump condition is a
    // superset of the shadow version's; summing both keeps the gate honest if
    // that ever drifts.
    uint64_t content = 0;
    for (const auto& [worldId, version] : m_RenderContentVersions)
        content += version;
    for (const auto& [worldId, changes] : m_ShadowCasterChanges)
        content += changes.Version;
    m_IdleElision.ContentEpoch = content;

    uint64_t lights = 0;
    for (const auto& [worldId, version] : m_WorldLightVersions)
        lights += version;
    m_IdleElision.LightEpoch = lights;

    // Depth-dynamic term: content that rasters depth outside the GPU-driven
    // instance path has no epoch tracking its per-frame output, so its
    // activity must pin every depth-derived family to recompute (DepthMinMax /
    // ClusteredLightCull / SDSM / HZB-P2 all derive from rasterized depth).
    // Sources feeding the tick:
    //   1. Registered contributor draw producers whose contract says they can
    //      WRITE DEPTH (grass; blend-only producers like smoke billboards
    //      animate color only — invisible to those families). Honest
    //      superset: a genuinely static depth-writing producer also blocks
    //      elision; per-producer content epochs are the tracked refinement.
    //   2. Skinned bone palettes (NotifySkinPaletteContentChanged): the
    //      palette ring is re-uploaded every frame and extraction compares
    //      only instance offsets, never palette bytes — a playing in-place
    //      skeletal/morph animation changes depth with no other key moving.
    //      Exact-input at the source: paused animators compare equal and
    //      keep eliding.
    //   3. Feature-rendered depth writers that register NO producer (ocean,
    //      CBT terrain build their draws inside their own pipeline nodes,
    //      invisible to both the producer registry and the extraction
    //      digest's record walk). Each reports IRenderFeature::
    //      WritesDynamicDepth from state its own ECS systems maintained
    //      earlier this frame, and pins the families the same way a
    //      registered depth-writing producer does. The CBT terrain reports
    //      only while its update is not at rest (CBTRenderFeature); the ocean
    //      only while its surface or FFT spectrum blocks change with water in
    //      some view (OceanRenderFeature::PublishDepthClaim).
    bool anyDepthWritingProducer = !m_DrawProducers->Depth.empty();
    for (const ForwardProducerEntry& e : m_DrawProducers->Forward)
        anyDepthWritingProducer = anyDepthWritingProducer || e.WritesDepth;
    const bool skinPaletteChanged = m_SkinPaletteContentChanged;
    m_SkinPaletteContentChanged = false;
    bool anyDynamicDepthFeature = false;
    ForEachFeature([&](const IRenderFeature& feature)
                   { anyDynamicDepthFeature = anyDynamicDepthFeature || feature.WritesDynamicDepth(); });
    if (anyDepthWritingProducer || skinPaletteChanged || anyDynamicDepthFeature)
        ++m_IdleElisionDepthDynamicEpoch;
    m_IdleElision.DepthDynamicEpoch = m_IdleElisionDepthDynamicEpoch;

    if (m_GpuCullingPipeline)
    {
        Rendering::ElisionFrameContext ctx{};
        ctx.AllowElision = m_IdleElision.AllowCull;
        ctx.ContentEpoch = m_IdleElision.ContentEpoch;
        ctx.LightEpoch = m_IdleElision.LightEpoch;
        ctx.DepthDynamicEpoch = m_IdleElision.DepthDynamicEpoch;
        m_GpuCullingPipeline->SetElisionFrameContext(ctx);
    }
    RefreshScatterElisionContext();
}

void RenderServices::RefreshScatterElisionContext()
{
    if (!m_DrawStreamBuilder)
        return;
    Rendering::ElisionFrameContext ctx{};
    ctx.AllowElision = m_IdleElision.AllowScatter;
    // No DepthDynamicEpoch here: scatter inputs are visibility + instances +
    // tables; a depth-dynamic scene keeps the CULL family recomputing, whose
    // VisibilityWriteEpoch bump chains the recompute into every scatter gate.
    ctx.ContentEpoch = m_IdleElision.ContentEpoch;
    ctx.GpuSceneEpoch = m_GpuScene ? m_GpuScene->GetContentEpoch() : 0;
    ctx.VisibilityWriteEpoch =
        m_GpuCullingPipeline ? m_GpuCullingPipeline->GetVisibilityWriteEpoch() : 0;

    // Crossfade vs. idle elision. Every other scatter rule is a fixed point, so
    // skipping the dispatch once its inputs settle reproduces the retained
    // records exactly. A fade is not: a scatter that stops running mid-transition
    // leaves the last frame's record pair in place and the object stays
    // permanently half-dissolved. Suppression lifts on an OBSERVED empty tail
    // rather than on a clock, so a settle that started no transition — which is
    // most of them — stops paying for the possibility that it did.
    const Rendering::GPUDrawStreamBuilder::LiveTailObservation tail =
        m_DrawStreamBuilder->GetLiveTailObservation();
    Renderer::LodCrossfadeTailObservation observation{};
    observation.Valid          = tail.valid;
    observation.TailRecords    = tail.tailRecords;
    observation.RecomputeCount = tail.recomputeCount;
    // Nothing in pass DECLARATION reads the verdict: both raster passes draw the
    // tails, so no attachment's access mode depends on whether a fade is live.
    if (ResolveCrossfadeLiveness(m_LODCrossfadeDuration, observation,
                                 m_DrawStreamBuilder->GetScatterRecomputeCount(),
                                 m_CullingFrameIndex, m_CrossfadeLiveness))
        ctx.AllowElision = false;

    m_DrawStreamBuilder->SetElisionFrameContext(ctx);
    // The fade weight is derived from (now - per-instance start), so the clock
    // travels with the elision context rather than inside any per-view struct.
    m_DrawStreamBuilder->SetFrameTimeSeconds(
        static_cast<float>(Time::GetCumulativeSeconds()));
}

SceneAccelerationStructureService* RenderServices::EnsureSceneAccelerationStructureService()
{
    if (!m_Device || !m_Device->GetCapabilities().supportsRayQuery)
        return nullptr;
    if (!m_SceneAS)
    {
        m_SceneAS = std::make_unique<SceneAccelerationStructureService>(
            m_Device, &GetMeshGPURegistry(), m_GpuScene.get());
    }
    return m_SceneAS.get();
}

void RenderServices::ScheduleRTShadowMask(Rendering::RenderGraph::RGFrame& frame)
{
    // Live setting: extraction republishes each world's resolved
    // ShadowSettingsEffect every frame, so this reads the CURRENT frame's
    // authored mode. Any world asking for RayTraced schedules the (global)
    // AS build; when none does, an already-created service releases its claim
    // on the shared pool (TickInactive) and the pool decides when its BLASes go
    // (SceneAccelerationStructureService::BeginFrame) — through the backend's
    // deferred-destruction queue, so AS teardown never races in-flight
    // frames.
    bool anyRayTraced = false;
    for (const auto& [worldId, settings] : m_WorldShadowSettings)
    {
        if (settings.Mode == Components::DirectionalShadowMode::RayTraced)
        {
            anyRayTraced = true;
            break;
        }
    }
    if (!anyRayTraced)
    {
        // Null service = RT never enabled on a ray-query device: this whole
        // branch is one pointer test, keeping the shipping-path cost at zero.
        if (m_RTShadowMask)
            m_RTShadowMask->TickInactive();
        return;
    }

    if (!m_Device || !m_Device->GetCapabilities().supportsRayQuery)
    {
        static bool s_Warned = false;
        if (!s_Warned)
        {
            s_Warned = true;
            Logger::Log::Warning(
                "Directional shadow mode RayTraced requested but the device does not "
                "support ray query (extension/feature missing or fallback device path) "
                "— keeping cascades");
        }
        return;
    }

    SceneAccelerationStructureService* sceneAS = EnsureSceneAccelerationStructureService();
    if (!sceneAS)
        return;  // supportsRayQuery already checked above; defensive only
    if (!m_RTShadowMask)
        m_RTShadowMask = std::make_unique<RTShadowMaskService>(sceneAS, m_GpuScene.get());
    // Combined caster epoch: the sum over per-world ShadowCasterContentVersion
    // (monotonic, so the sum is too). The TLAS is global across worlds — any
    // world's caster change must dirty it; the cascade static cache keys on
    // the same per-world signal.
    uint64_t casterEpoch = 0;
    for (const auto& [worldId, changes] : m_ShadowCasterChanges)
        casterEpoch += changes.Version;
    m_RTShadowMask->Schedule(frame, casterEpoch);
}

uint32_t RenderServices::WorldKeyForView(Rendering::ViewId viewId) const
{
    const Rendering::ViewDesc* v = m_ViewRegistry.FindViewDesc(viewId);
    if (!v || v->worldId == 0)
        return 0u; // untagged or unknown view: match any
    return Rendering::PackWorldKey16(v->worldId);
}

uint32_t RenderServices::ScheduleWorldBucketerCommon()
{
    if (!m_DrawStreamBuilder || !m_GpuScene || !m_GpuCullingPipeline)
        return 0u;

    const uint32_t instanceCount = m_GpuScene->GetInstanceCount();
    if (instanceCount == 0u)
        return 0u;

    // Look up the per-view visibility-buffer slice offset from GPU culling.
    // The visibility buffer is shared across all views, with each view's
    // visibility flags packed at a distinct [offset, offset+count) range.
    // The bucketer must bind the buffer at THIS view's offset so it reads
    // the right slice; without this, every view's bucketer reads view 0's
    // results (sphere+plane render because they're at indices 0/1 in
    // every view, but newly-spawned entities at index 2+ in the main
    // scene view get bogus visibility=0 readings).
    const auto& visRanges = m_GpuCullingPipeline->GetViewVisibilityRanges();
    auto visOffsetBytesForView = [&visRanges](Rendering::ViewId vid,
                                              uint8_t cascadeIdx) -> uint32_t {
        for (const auto& r : visRanges)
            if (r.viewId == vid && r.cascadeIndex == cascadeIdx && r.slicePhase == 0u)
                return r.visibilityOffset * static_cast<uint32_t>(sizeof(uint32_t));
        return 0u;
    };
    auto hasVisRangeForCascade = [&visRanges](Rendering::ViewId vid,
                                              uint8_t cascadeIdx) -> bool {
        for (const auto& r : visRanges)
            if (r.viewId == vid && r.cascadeIndex == cascadeIdx && r.slicePhase == 0u)
                return true;
        return false;
    };

    // Schedule a bucketer dispatch for a (view, cascade) pair across all
    // visible (mat, mesh) batches. Shared between the main-view fan-out (one
    // pass per view, cascadeIndex = kCascadeIndexNone) and the per-cascade
    // fan-out below (one pass per (view, cascade), cascadeIndex = 0..3).
    // Phase 5b-iii intentionally reuses the main view's batch-key set for
    // all cascades — the same materials and meshes draw into shadow as into
    // color; cascade visibility filters out invisible ones at the bucketer
    // level instead of CPU-side.
    // Compute the per-view worldKey for the bucketer to filter against the
    // upper half of GPUInstance.flags. RenderExtractionSystem packs the same
    // hash, so this keeps each view's bucketer scoped to its world's entities
    // and stops editor thumbnail / preview-world models from leaking into
    // the main scene's indirect draw stream.
    auto worldKeyForView = [this](Rendering::ViewId vid) -> uint32_t
    { return WorldKeyForView(vid); };

    // One SLICE registration per (view, cascade) pair — the scatter pass
    // resolves batches per-thread from the GPU batch table, so there is no
    // per-(mat, mesh) fan-out here anymore (SCALE-1/4: was B×C buckets, each
    // a transient descriptor set + a full-N filter dispatch).
    // lodOverride replaces the camera-derived params entirely (point-shadow faces,
    // which select from their light — see MakePointFaceLodParams).
    auto registerViewBatches = [&](Rendering::ViewId viewId, uint8_t cascadeIdx,
                                   const Rendering::GPUDrawStreamBuilder::ViewLODParams*
                                       lodOverride = nullptr)
    {
        // Per-view LOD params; cascade and spot/area shadow slices reuse the
        // view's camera so color and shadow silhouettes pick the same level by
        // default. The shadow bias would let shadow maps render coarser LODs
        // than the view (negative log2 coverage = earlier switch to lower
        // detail), but its only writer today is the set_lod debug handler, so it
        // is 0 in every shipped path — see that handler for why a negative value
        // is not a free win. Point-shadow faces do NOT reuse the camera: they
        // pass an override built from the light, and take this same bias through
        // it.
        const bool isShadowBucket = cascadeIdx != Rendering::GPUDrawStreamBuilder::kCascadeIndexNone;
        const float bucketBias = m_LODGlobalBias + (isShadowBucket ? m_ShadowLODBias : 0.0f);

        Rendering::GPUDrawStreamBuilder::SliceRegistration s{};
        s.viewId                 = static_cast<uint32_t>(viewId);
        s.cascadeIndex           = cascadeIdx;
        s.table                  = isShadowBucket
                                       ? Rendering::GPUDrawStreamBuilder::SliceTable::Shadow
                                       : Rendering::GPUDrawStreamBuilder::SliceTable::Color;
        s.visibilityOffsetBytes  = visOffsetBytesForView(viewId, cascadeIdx);
        s.disableVisibilityCheck = !hasVisRangeForCascade(viewId, cascadeIdx);
        s.viewWorldKey           = worldKeyForView(viewId);
        // Shadow buckets never small-object cull: a culled caster's shadow can
        // be far larger than the caster's own on-screen coverage.
        // Shadow buckets never crossfade either: a dithered caster punches holes
        // in its shadow map, which PCF averages into a uniformly lighter shadow
        // — a larger artifact than the silhouette snap the fade would hide.
        // Point-shadow face overrides are shadow slices too: MakePointFaceLodParams
        // leaves the crossfade duration at its zero default.
        // They also take no LOD dwell band — the measured thrash is a camera-view
        // effect, and a per-cascade history would multiply the persistent buffers
        // by the slice count (point lights alone are 6 faces each).
        s.lod = lodOverride
                    ? *lodOverride
                    : MakeViewLODParams(m_ViewRegistry.ResolveCameraData(viewId), bucketBias,
                                        ResolveViewForceLOD(viewId),
                                        isShadowBucket ? 0.0f : m_SmallObjectCullCoverage,
                                        ComputeViewSseScales(viewId),
                                        isShadowBucket ? 0.0f : m_LODCrossfadeDuration,
                                        isShadowBucket ? 0.0f : m_LODHysteresisBand);
        // The deforming-motion producer reads this view's rendered level and
        // phase history for per-instance validity, so the camera slices of a
        // view keep it while an arm is enabled. Shadow buckets never do: no
        // producer draws into a cascade. With every arm off no view asks, the
        // scatter allocates nothing and binds its stand-in, which is the state
        // the history shipped in.
        if (!isShadowBucket && m_DeformationMotionArm != DeformationMotionArm::None)
            s.lod.renderedLevelHistoryRequested = true;
        // Fit-freeze lane: a directional cascade slice selects LODs with the
        // FROZEN camera captured at that cascade's last refit (the value the
        // static-cache key holds via CullCameraViewProj), so a retained layer
        // and a hypothetical re-render pick identical LODs and the cache's
        // skip contract stays exact under camera motion.
        if (isShadowBucket && cascadeIdx < kMaxShadowCascades)
        {
            if (const auto* feature = GetFeature<ShadowMapRenderFeature>();
                feature && feature->IsFitFreezeEnabled())
            {
                if (const CascadeFrameData* fd = feature->GetCachedFrameData(viewId);
                    fd && cascadeIdx < fd->NumCascades)
                {
                    s.lod.cameraPos[0] = fd->LodCameraPos[cascadeIdx][0];
                    s.lod.cameraPos[1] = fd->LodCameraPos[cascadeIdx][1];
                    s.lod.cameraPos[2] = fd->LodCameraPos[cascadeIdx][2];
                    const bool is2DOrtho = fd->LodCameraPos[cascadeIdx][3] == 1.0f;
                    s.lod.projScaleY = is2DOrtho ? 0.0f : fd->LodProjScaleY[cascadeIdx];
                }
            }
        }
        m_DrawStreamBuilder->RegisterSlice(s);
    };

    for (const auto& view : m_ViewRegistry.GetViews())
    {
        // A view with no active layer declares no pass that reads a slice (the
        // culling loop and the pipeline skip it too); its cull-less slice would
        // still scatter every instance of its world.
        if (view.ActiveRenderLayerMask() == 0u)
            continue;

        // Main-view / thumbnail / picking slice (cascadeIndex = none).
        registerViewBatches(view.id,
                            Rendering::GPUDrawStreamBuilder::kCascadeIndexNone);

        // Per-cascade dispatches. ShadowMapRenderFeature::OnScheduleCulling
        // submits one CascadeCullingGroup per shadow view; GPUCullingPipeline
        // expands it into per-cascade ViewVisibilityRange entries (one per
        // cascade). We iterate those slices here so each cascade gets its
        // own (mat, mesh) bucketer output.
        if (ViewNeedsShadowCascadePasses(view.id))
        {
            for (uint8_t c = 0; c < 4u; ++c)
            {
                if (!hasVisRangeForCascade(view.id, c))
                    continue;
                registerViewBatches(view.id, c);
            }
        }

        if (hasVisRangeForCascade(view.id, kAreaShadowCullingIndex))
            registerViewBatches(view.id, kAreaShadowCullingIndex);
        if (hasVisRangeForCascade(view.id, kSpotShadowCullingIndex))
            registerViewBatches(view.id, kSpotShadowCullingIndex);
        // M1: one batch slice per (slot, face) that the cull scheduler produced a
        // visibility range for (same shared atlas assignment; ScheduleCulling ran
        // earlier, so hasVisRangeForCascade reflects the scheduled slices).
        if (HasShadowCasters(view.id))
        {
            const auto& assignment = EnsurePointShadowAssignment(
                view.id, view.worldId, m_ViewRegistry.ResolveCameraData(view.id));
            // The knobs the planner keyed this frame's cache decisions on. Same
            // call, same values — see ResolvePointShadowLodKey.
            const PointShadowLodKey lodKnobs = ResolvePointShadowLodKey(view.id);
            for (const PointShadowFrameInfo& fi : assignment.Slots)
            {
                if (fi.shadowSlot < 0)
                    continue;
                // L1a: a cached light declares no passes, so it needs no batch
                // slices. (Its slices also have no scheduled visibility range, so
                // the hasVisRangeForCascade gate below would skip them anyway — this
                // is the explicit, order-independent form.)
                if (!fi.needsRender)
                    continue;
                const uint32_t slot = static_cast<uint32_t>(fi.shadowSlot);
                // Light-relative selection: all six faces of a slot share one
                // params block (same light, same tile), so build it per slot.
                const Rendering::GPUDrawStreamBuilder::ViewLODParams faceLod =
                    MakePointFaceLodParams(fi.position, fi.tileResolution, lodKnobs);
                for (uint32_t face = 0; face < kPointShadowFaceCount; ++face)
                {
                    if ((fi.faceMask & (1u << face)) == 0u)
                        continue;
                    const uint8_t cullingIndex =
                        PointShadowCullingIndex(PointShadowSlotFaceLayer(slot, face));
                    if (hasVisRangeForCascade(view.id, cullingIndex))
                        registerViewBatches(view.id, cullingIndex, &faceLod);
                }
            }
        }
    }

    return instanceCount;
}

bool RenderServices::ScheduleBucketerDispatchesForView(Rendering::RenderGraph::RGFrame& frame,
                                                       Rendering::ViewId viewId)
{
    const uint8_t cascadeNone[1] = {Rendering::GPUDrawStreamBuilder::kCascadeIndexNone};
    return ScheduleBucketerDispatchesForViewSlices(frame, viewId, cascadeNone);
}

bool RenderServices::ScheduleBucketerDispatchesForViewSlices(
    Rendering::RenderGraph::RGFrame& frame, Rendering::ViewId viewId,
    std::span<const uint8_t> cascadeIndices)
{
    namespace RenderGraph = Rendering::RenderGraph;
    if (cascadeIndices.empty())
        return false;

    // Compat profile: the scatter's records are only reachable through a device
    // address, so there are no bucketer dispatches to schedule — this view's
    // recorders draw from the CPU instance lists, which need nothing from here.
    // That is success, not deferral: the return contract is "this view's draws
    // will be delivered this frame", and both callers (probe capture, model
    // thumbnails) treat false as defer-and-retry. Returning false here parked
    // them in a permanent re-arm loop on WebGPU — the env cube never left its
    // seed, which read as a white washed scene with dead reflections.
    if (m_Profile.IsCompat())
        return true;

    // RenderGraph twin of the old arm above — same mid-frame seam: the thumbnail
    // handler extracts into the window's frame AFTER BuildFrameGraph ran, so
    // a second idempotent GPUScene flush is load-bearing (see the old arm's
    // rationale) and the buffers must be re-resolved post-flush.
    if (!m_DrawStreamBuilder || !m_GpuScene || !m_GpuCullingPipeline)
        return false;

    // The spine must have stamped THIS frame incarnation (owner or non-owner
    // path) — no-spine frames (old-arm excursions) refuse and the caller
    // defers the render rather than baking a blank or garbage-skinned slot.
    if (!m_FrameRG.For.IsFor(frame))
        return false;

    // Visibility binding: prefer this frame's culling value. A spine frame
    // with ZERO views (scene tab inactive, assets-only tool window — 8e)
    // runs no culling, so fall back to re-importing the unified physical:
    // its CONTENT is irrelevant here (views without a vis slice dispatch
    // with disableVisibilityCheck) but the buffer must exist to bind. It is
    // invalid only until the first culling of the session ever ran — defer
    // until then.
    Rendering::RenderGraph::RGBuffer visRG = m_FrameRG.Visibility;
    if (!visRG.IsValid())
        visRG = m_GpuCullingPipeline->ImportVisibility(frame);
    if (!visRG.IsValid())
        return false;

    if (m_GpuScene->IsDirty())
        m_GpuScene->FlushGPUBuffers();

    const auto keys = m_WorldDrawBuilder.GetBatchKeys(viewId);
    if (keys.empty())
        return true; // nothing to schedule — a clear-only render is legitimate

    const uint32_t instanceCount = m_GpuScene->GetInstanceCount();
    if (instanceCount == 0u)
        return false;

    // Post-flush re-resolve: a mid-frame growth realloc yields NEW physicals;
    // dedup-by-physical returns the spine's ids when unchanged and fresh
    // first-touch-synced ids when changed. Visibility has no flush concern —
    // the spine's frame-local value is used directly (its CONTENT is
    // irrelevant for views without a visibility slice ⇒ disableVisibilityCheck
    // below, but the dispatch still binds the buffer).
    const Rendering::GPUScene::GPUSceneFrameRG sceneRG = m_GpuScene->ImportFrameResources(frame);
    if (!sceneRG.Instances.IsValid() || !sceneRG.Meshes.IsValid())
        return false;

    const auto& visRanges = m_GpuCullingPipeline->GetViewVisibilityRanges();
    auto visOffsetBytesForView = [&visRanges](Rendering::ViewId vid,
                                              uint8_t cascadeIdx) -> uint32_t {
        for (const auto& r : visRanges)
            if (r.viewId == vid && r.cascadeIndex == cascadeIdx && r.slicePhase == 0u)
                return r.visibilityOffset * static_cast<uint32_t>(sizeof(uint32_t));
        return 0u;
    };
    auto hasVisRangeForCascade = [&visRanges](Rendering::ViewId vid,
                                              uint8_t cascadeIdx) -> bool {
        for (const auto& r : visRanges)
            if (r.viewId == vid && r.cascadeIndex == cascadeIdx && r.slicePhase == 0u)
                return true;
        return false;
    };

    // One Color-table slice per requested cascade index, all selecting LODs
    // from the view's camera; the scatter resolves batches from the table.
    // APPENDS to the app frame's arena (BeginArenaFrame ran on the spine).
    // No dwell band on utility views (planar reflection, raster depth capture,
    // probe faces): they have no measured pop to fix. Note the World spine also
    // registers a cascade-None slice for every view with an active layer, so a utility
    // view registering cascade None here ends up with two slices under one
    // range-map key; whichever call runs later wins the published range. A
    // fan-out (probe faces at their own indices) never collides with it.
    // Keeping these band-less means the utility path adds no history of its
    // own, but it does not by itself guarantee a stateless pick for these views.
    const Rendering::GPUDrawStreamBuilder::ViewLODParams lod = MakeViewLODParams(
        m_ViewRegistry.ResolveCameraData(viewId), m_LODGlobalBias, ResolveViewForceLOD(viewId),
        m_SmallObjectCullCoverage, ComputeViewSseScales(viewId), m_LODCrossfadeDuration, 0.0f);
    const uint32_t viewWorldKey = WorldKeyForView(viewId);
    for (const uint8_t cascadeIdx : cascadeIndices)
    {
        Rendering::GPUDrawStreamBuilder::SliceRegistration s{};
        s.viewId                 = static_cast<uint32_t>(viewId);
        s.cascadeIndex           = cascadeIdx;
        s.table                  = Rendering::GPUDrawStreamBuilder::SliceTable::Color;
        s.visibilityOffsetBytes  = visOffsetBytesForView(viewId, cascadeIdx);
        s.disableVisibilityCheck = !hasVisRangeForCascade(viewId, cascadeIdx);
        s.viewWorldKey           = viewWorldKey;
        s.lod                    = lod;
        m_DrawStreamBuilder->RegisterSlice(s);
    }

    char passSuffix[64];
    std::snprintf(passSuffix, sizeof(passSuffix), "View%u", static_cast<uint32_t>(viewId));
    // Captured snapshot: this arm's own FlushGPUBuffers ran above (the
    // mid-frame thumbnail seam), so the registry matches the buffer content.
    RefreshScatterElisionContext();
    const ScatterGroupMaps groupMaps = MeshPoolGroupMapsForScatter();
    const RenderGraph::RGBuffer ordering = m_DrawStreamBuilder->ScheduleUnifiedScatter(
        frame, passSuffix, static_cast<int32_t>(Rendering::PassPhase::kEarlySetup),
        visRG, sceneRG.Instances, sceneRG.ScatterHot, sceneRG.Meshes,
        m_GpuScene->GetBatchRegistry(), m_MaterialSystem.MaterialDepthClassSpan(),
        m_MaterialSystem.MaterialColorClassSpan(), groupMaps.MeshOrdered,
        groupMaps.OrderedToGroup,
        instanceCount, static_cast<uint32_t>(m_GpuScene->GetMeshes().size()));
    if (!ordering.IsValid())
        return false;
    // Thread the ordering proxy: dedup-by-physical lands on the spine's id
    // when the owner's bucketer already ran this frame, so this assignment is
    // identity there.
    m_FrameRG.DrawStreamOrdering = ordering;
    return true;
}

bool RenderServices::ScheduleWorldOcclusionRecoverScatterForView(
    Rendering::RenderGraph::RGFrame& frame, Rendering::ViewId viewId)
{
    namespace RenderGraph = Rendering::RenderGraph;

    // Compat profile: no GPU occlusion recovery — the phase-B scatter feeds a
    // consumer this profile does not have.
    if (m_Profile.IsCompat())
        return false;
    if (!m_DrawStreamBuilder || !m_GpuScene || !m_GpuCullingPipeline)
        return false;
    // The spine must have stamped THIS frame incarnation (culling laid out the
    // phase-B reservation into m_FrameRG this frame) — otherwise the phase-B
    // slice offset would index a foreign frame's visibility buffer.
    if (!m_FrameRG.For.IsFor(frame) || !m_FrameRG.Visibility.IsValid() ||
        !m_FrameRG.SceneInstances.IsValid() || !m_FrameRG.SceneMeshes.IsValid())
        return false;
    if (m_GpuCullingPipeline->LastPublishArm() !=
        Rendering::GPUCullingPipeline::PublishArm::RenderGraph)
        return false;

    // The phase-B visibility slice this view reserved (HzbCullingStrategy sets
    // ViewCullingInput::reserveOcclusionSlice; EndFrameRG published it with
    // slicePhase==1). Absent ⇒ the view is not an HZB view — nothing to
    // recover, which is the default for every view until the strategy opts in.
    const uint8_t cascadeIdx = Rendering::GPUDrawStreamBuilder::kCascadeIndexNone;
    const uint32_t viewWorldKey = WorldKeyForView(viewId);

    const auto& visRanges = m_GpuCullingPipeline->GetViewVisibilityRanges();
    bool hasPhaseB = false;
    uint32_t phaseBOffsetBytes = 0u;
    for (const auto& r : visRanges)
    {
        if (r.viewId == viewId && r.cascadeIndex == cascadeIdx && r.slicePhase == 1u)
        {
            hasPhaseB = true;
            phaseBOffsetBytes = r.visibilityOffset * static_cast<uint32_t>(sizeof(uint32_t));
            break;
        }
    }
    if (!hasPhaseB)
        return false;

    const auto keys = m_WorldDrawBuilder.GetBatchKeys(viewId);
    if (keys.empty())
        return true; // nothing to recover — a legitimate empty phase-B generation

    if (m_GpuScene->IsDirty())
        m_GpuScene->FlushGPUBuffers();

    const uint32_t instanceCount = m_GpuScene->GetInstanceCount();
    if (instanceCount == 0u)
        return false;

    const Rendering::GPUScene::GPUSceneFrameRG sceneRG = m_GpuScene->ImportFrameResources(frame);
    if (!sceneRG.Instances.IsValid() || !sceneRG.Meshes.IsValid())
        return false;

    // Phase-B slice: same view/worldKey/LOD as the phase-A slice, bound to the
    // reserved phase-B visibility offset. phase = B tags the published ranges
    // (range-map key bit 63) so the phase-B consumers look up their own
    // generation, disjoint from phase A's.
    Rendering::GPUDrawStreamBuilder::SliceRegistration s{};
    s.viewId                 = static_cast<uint32_t>(viewId);
    s.cascadeIndex           = cascadeIdx;
    s.phase                  = Rendering::GPUDrawStreamBuilder::SlicePhase::B;
    s.visibilityOffsetBytes  = phaseBOffsetBytes;
    s.disableVisibilityCheck = false; // the phase-B slice always has a real culling generation
    s.viewWorldKey           = viewWorldKey;
    // Phase B takes the SAME band as phase A and shares this view's history
    // buffer. Sound because the two visibility slices are disjoint — phase B
    // sees only `visible && !drawnInA` — so no instance is selected twice in a
    // frame, and an instance recovered here leaves the level phase A resumes from.
    s.lod = MakeViewLODParams(m_ViewRegistry.ResolveCameraData(viewId), m_LODGlobalBias,
                              ResolveViewForceLOD(viewId), m_SmallObjectCullCoverage,
                              ComputeViewSseScales(viewId), m_LODCrossfadeDuration,
                              m_LODHysteresisBand);
    // Phase B shares this view's history buffer with phase A (the comment
    // above), so it asks for it on the same condition.
    if (m_DeformationMotionArm != DeformationMotionArm::None)
        s.lod.renderedLevelHistoryRequested = true;
    m_DrawStreamBuilder->RegisterSlice(s);

    // LATE pass phase (after world render): PassPhase is a scheduling tiebreak,
    // and this scatter must never be pulled ahead of the phase-A raster its HZB
    // input derives from when the graph edges leave the order free (§5-A1).
    // Context refresh is load-bearing here: a P2 dispatch just advanced the
    // visibility-write epoch, and this phase-B scatter's gate must see it.
    RefreshScatterElisionContext();
    const ScatterGroupMaps groupMaps = MeshPoolGroupMapsForScatter();
    const RenderGraph::RGBuffer ordering = m_DrawStreamBuilder->ScheduleUnifiedScatter(
        frame, "World.B", static_cast<int32_t>(Rendering::PassPhase::kPostProcess),
        m_FrameRG.Visibility, sceneRG.Instances, sceneRG.ScatterHot, sceneRG.Meshes,
        m_GpuScene->GetBatchRegistry(), m_MaterialSystem.MaterialDepthClassSpan(),
        m_MaterialSystem.MaterialColorClassSpan(), groupMaps.MeshOrdered,
        groupMaps.OrderedToGroup,
        instanceCount, static_cast<uint32_t>(m_GpuScene->GetMeshes().size()));
    return ordering.IsValid();
}

void RenderServices::EnsureAnimComputePass()
{
    // Lazy-init the GPU compute skinning pass (shared by both arms). Uses
    // ResolveShaderPath + ReadFile rather than LoadShaderFile so an absent shader
    // is an empty blob to degrade on instead of a hard error.
    if (m_AnimComputePass)
        return;
    // A configured resolver answering "not found" is terminal: cooked shaders
    // do not appear mid-session, and re-probing the filesystem every frame is
    // the retry-storm class — on web each probe is also a main-thread OPFS
    // roulette (see ResolveEngineShaderPath). Retry only while the resolver
    // itself has not been configured yet.
    if (m_AnimShaderResolvedMissing)
        return;
    // Both cases return empty, and they mean opposite things: no resolver yet means
    // nobody has looked (early frames, headless tests) and the next frame must ask
    // again; a resolver that found nothing is the terminal answer this latches on.
    const bool resolverAnswered = Rendering::Utils::HasShaderPathResolver();
    std::vector<uint8_t> spirv;
    {
        // Spelled relative to the assets root the resolver searches, like every other
        // engine shader request: the bare name resolves nowhere and left this load
        // riding its install-root fallback on every host.
        const std::string resolved =
            Rendering::Utils::ResolveShaderPath("Shaders/animation_skinning.comp.spv");
        if (!resolved.empty())
            spirv = Rendering::Utils::ReadFile(resolved);
    }
    if (spirv.empty())
    {
        spirv = Rendering::Utils::ReadFile(
            (PathUtils::GetInstallAssetsRoot() / "Shaders" / "animation_skinning.comp.spv").string());
    }
    if (spirv.empty() && resolverAnswered)
    {
        m_AnimShaderResolvedMissing = true;
        Logger::Log::Info("[GPUAnim] animation_skinning.comp.spv not found — GPU skinning "
                          "stays off this session");
    }

    if (!spirv.empty())
    {
        m_AnimComputePass = std::make_unique<AnimationComputePass>();
        m_AnimComputePass->SetComputeShader(spirv);
        m_AnimComputePass->SetDataStore(&m_GPUAnimDataStore);
        Logger::Log::Info("[GPUAnim] Compute skinning shader loaded ({} bytes)", spirv.size());
    }
}

void RenderServices::ScheduleGpuSkinningAndRetarget(Rendering::RenderGraph::RGFrame& frame)
{
    if (m_FrameRG.For.Frame && !m_FrameRG.For.IsFor(frame))
    {
        m_FrameRG = {}; // a different frame stream/incarnation took over — stale values die
        m_ViewFrameRG.clear();
    }

    EnsureAnimComputePass();
    const auto atlasBuf = m_SkinPaletteAtlas.GetBuffer();
    if (!atlasBuf.IsValid())
        return;
    m_FrameRG.For.Stamp(frame);
    // Per-frame import naturally tracks BOTH the PerFrameWritePool rotation
    // and grow-recreate — the entire rebind dance has no equivalent here.
    // Slice-3 contract: world/depth/shadow/area/selection-mask passes MUST
    // declare Read(m_FrameRG.SkinPaletteAtlas) — the VS binds the atlas
    // descriptor-direct, so only that declared read orders skinning→VS (the
    // first-ever real compute→vertex barrier; the old graph had phase order).
    m_FrameRG.SkinPaletteAtlas = frame.ImportExternalBuffer("SkinPaletteAtlas", atlasBuf);

    if (m_AnimComputePass && !m_GPUAnimDataStore.IsDispatchScheduledThisFrame())
    {
        // Default ON: skinning early-outs for runtimes whose entities are
        // off-screen in every view (main, thumbnails, shadow cascades — the
        // union/aggregate passes run unconditionally and cover all slices).
        // Runtime ids beyond kRuntimeVisibleCapacity bypass the gate and
        // always animate, so overflow degrades to the pre-gate behavior.
        // GE_SKINNING_VISIBILITY_GATE=0 is the kill-switch.
        static const bool kSkinningGateEnabled = []()
        {
            if (const char* env = std::getenv("GE_SKINNING_VISIBILITY_GATE"))
                return env[0] != '0';
            return true;
        }();
        // Spawn-frame guard: the gate consumes the PREVIOUS frame's
        // visibility aggregate (deliberate WAR lag), so a runtime created
        // this frame — new or free-list reused — has a stale word. Gating on
        // it would skip the new instance's skinning while the draw still
        // samples its reserved (unwritten) palette region: one garbage-pose
        // frame per spawn. Skin everything on frames that created a runtime.
        const uint64_t runtimeCreateEpoch = SkeletonStore::Instance().GetRuntimeCreateEpoch();
        const bool runtimeSpawnedThisFrame = runtimeCreateEpoch != m_LastRuntimeCreateEpoch;
        m_LastRuntimeCreateEpoch = runtimeCreateEpoch;

        Rendering::RenderGraph::RGBuffer runtimeVis{};
        if (kSkinningGateEnabled && !runtimeSpawnedThisFrame && m_GpuCullingPipeline)
        {
            runtimeVis = m_GpuCullingPipeline->ImportRuntimeVisible(frame);
            m_FrameRG.RuntimeVisible = runtimeVis;
            m_AnimComputePass->SetRuntimeVisibility(
                true, m_GpuCullingPipeline->GetRuntimeVisibleCapacity());
        }
        else
        {
            // Disarm explicitly: without this the pass would keep gating
            // across spawn frames or if the culling pipeline goes away.
            m_AnimComputePass->SetRuntimeVisibility(false, 0);
        }
        m_AnimComputePass->Declare(frame, m_FrameRG.SkinPaletteAtlas, runtimeVis);
        m_GPUAnimDataStore.MarkDispatchScheduledThisFrame();
    }

    if (auto* retargetFeat = GetFeature<RetargetRenderFeature>();
        retargetFeat && retargetFeat->IsInitialized() && retargetFeat->IsGpuEnabled() &&
        retargetFeat->GetDataStore().GetActiveCharacterCount() > 0)
    {
        // SAME RGBuffer value the skinning pass writes — one resource id, so
        // RenderGraph orders the two atlas writers.
        retargetFeat->BuildPasses(frame, m_FrameRG.SkinPaletteAtlas, /*deltaTime*/ 1.0f / 60.0f);
    }
}

void RenderServices::ScheduleViewCullingDispatches(Rendering::RenderGraph::RGFrame& frame, float deltaTime)
{
    // Same idempotence + view loop as the old arm above (duplicated so the
    // old path stays untouched until the step-5 delete).
    if (m_CullingScheduledThisFrame)
        return;
    if (m_FrameRG.For.Frame && !m_FrameRG.For.IsFor(frame))
    {
        m_FrameRG = {}; // identity reset (plan §1f) — foreign-frame ids must not leak
        m_ViewFrameRG.clear();
    }

    auto* scene = GetGPUScene();
    if (scene == nullptr || m_GpuCullingPipeline == nullptr)
        return;

    static Rendering::FrustumCullingStrategy s_DefaultFrustum;

    m_CullingScheduledThisFrame = true;
    ++m_CullingFrameIndex;

    // Once per app frame (this function is the owner-spine culling entry):
    // refresh the elision epochs + kill-switches and push the gate contexts.
    UpdateIdleElisionFrameState();

    m_GpuCullingPipeline->BeginFrame(&frame, scene);

    const std::vector<Rendering::CameraInfo>& cameras = m_ViewRegistry.GetCameras();
    auto findCamera = [&cameras](Rendering::CameraId id) -> const Rendering::CameraData*
    {
        for (const Rendering::CameraInfo& cam : cameras)
        {
            if (cam.id == id)
                return &cam.data;
        }
        return nullptr;
    };

    const uint32_t instanceCount = scene->GetInstanceCount();
    const uint32_t thisFrame = m_CullingFrameIndex;

    for (const Rendering::ViewDesc& view : m_ViewRegistry.GetViews())
    {
        if (view.cameraId == 0)
            continue;
        if (view.ActiveRenderLayerMask() == 0u)
            continue;

        const Rendering::CameraData* camData = findCamera(view.cameraId);
        if (camData == nullptr)
            continue;

        const Rendering::CameraDerivedData camera = Rendering::DeriveCameraData(*camData);

        Rendering::ViewCullingContext ctx{};
        ctx.Id = view.id;
        ctx.CascadeIndex = Rendering::kCullingCascadeIndexNone;
        ctx.ViewMatrix = camera.ViewMatrix;
        ctx.ProjMatrix = camera.ProjMatrix;
        ctx.ViewProjMatrix = camera.ViewProjMatrix;
        Rendering::ExtractFrustumPlanes(camera.ViewProjMatrix, ctx.FrustumPlanes);
        ctx.CameraPosition = camera.Position;
        ctx.CameraForward = camera.Forward;
        // Camera-relative culling origin (Earth-scale precision): the SAME render
        // origin ViewRegistry rebases the world pass against. Derive it from the
        // camera position exactly as ResolveCameraData does — the stored
        // CameraData::renderOriginSector is filled only on-read by ResolveCameraData
        // and is zero here, so we must recompute, not read it. The frustum test
        // differences boundingCenter against this so a planetary center stays
        // small-magnitude. Inactive (sector 0) => origin (0,0,0) => the GPU test is
        // byte-identical to the world-space one (dark-ship).
        {
            const auto originSector = ComputeRenderOriginSector(
                camData->cameraPos[0], camData->cameraPos[1], camData->cameraPos[2]);
            float ox, oy, oz;
            SectorToWorld(originSector, ox, oy, oz);
            ctx.CameraRelativeOrigin = Mathematics::Vector3{ox, oy, oz};
        }
        ctx.NearPlane = camera.NearPlane;
        ctx.FarPlane = camera.FarPlane;
        ctx.FirstInstance = 0;
        ctx.InstanceCount = instanceCount;
        ctx.RenderLayerMask = view.ActiveRenderLayerMask();
        ctx.FrameIndex = thisFrame;
        ctx.DeltaTime = deltaTime;
        ctx.CullingPipeline = m_GpuCullingPipeline.get();
        ctx.Scene = scene;

        Rendering::ICullingStrategy* strategy =
            view.cullingStrategy ? view.cullingStrategy.get() : &s_DefaultFrustum;

        if (view.worldId == 0u && view.cullingStrategy == nullptr)
        {
            if (m_ViewRegistry.MarkWorldlessViewWarned(view.id))
            {
                Logger::Log::Warning(
                    "RenderServices: view '{}' (id={}) is dispatched with the "
                    "default frustum culling strategy but has worldId == 0. "
                    "Call SetViewWorldId after AllocateView so multi-world "
                    "rendering filters this view correctly.",
                    view.debugName ? view.debugName : "<unnamed>",
                    static_cast<uint32_t>(view.id));
            }
        }

        strategy->ScheduleCulling(ctx);

        const auto areaShadow = BuildAreaShadowFrameInfo(GetWorldLights(view.worldId));
        if (areaShadow.valid && view.ActiveRenderLayerMask() != 0u && HasShadowCasters(view.id))
        {
            Rendering::ViewCullingContext areaCtx{};
            areaCtx.Id = view.id;
            areaCtx.CascadeIndex = kAreaShadowCullingIndex;
            areaCtx.ViewMatrix = areaShadow.lightView;
            areaCtx.ProjMatrix = areaShadow.lightProj;
            areaCtx.ViewProjMatrix = areaShadow.lightVP;
            Rendering::ExtractFrustumPlanes(areaShadow.lightVP, areaCtx.FrustumPlanes);
            areaCtx.CameraPosition = areaShadow.position;
            areaCtx.CameraForward = areaShadow.direction;
            areaCtx.NearPlane = areaShadow.nearPlane;
            areaCtx.FarPlane = areaShadow.farPlane;
            areaCtx.FirstInstance = 0;
            areaCtx.InstanceCount = instanceCount;
            areaCtx.RenderLayerMask = view.ActiveRenderLayerMask();
            areaCtx.FrameIndex = thisFrame;
            areaCtx.DeltaTime = deltaTime;
            areaCtx.CullingPipeline = m_GpuCullingPipeline.get();
            areaCtx.Scene = scene;
            s_DefaultFrustum.ScheduleCulling(areaCtx);
        }

        const auto spotShadow = BuildSpotShadowFrameInfo(GetWorldLights(view.worldId));
        if (spotShadow.valid && view.ActiveRenderLayerMask() != 0u && HasShadowCasters(view.id))
        {
            Rendering::ViewCullingContext spotCtx{};
            spotCtx.Id = view.id;
            spotCtx.CascadeIndex = kSpotShadowCullingIndex;
            spotCtx.ViewMatrix = spotShadow.lightView;
            spotCtx.ProjMatrix = spotShadow.lightProj;
            spotCtx.ViewProjMatrix = spotShadow.lightVP;
            Rendering::ExtractFrustumPlanes(spotShadow.lightVP, spotCtx.FrustumPlanes);
            spotCtx.CameraPosition = spotShadow.position;
            spotCtx.CameraForward = spotShadow.direction;
            spotCtx.NearPlane = spotShadow.nearPlane;
            spotCtx.FarPlane = spotShadow.farPlane;
            spotCtx.FirstInstance = 0;
            spotCtx.InstanceCount = instanceCount;
            spotCtx.RenderLayerMask = view.ActiveRenderLayerMask();
            spotCtx.FrameIndex = thisFrame;
            spotCtx.DeltaTime = deltaTime;
            spotCtx.CullingPipeline = m_GpuCullingPipeline.get();
            spotCtx.Scene = scene;
            s_DefaultFrustum.ScheduleCulling(spotCtx);
        }

        // M1: schedule one cull context per (slot, face) of the SHARED atlas
        // assignment. Two lights' faces see different casters, so each needs its
        // own frustum + survivor bucket. EnsurePointShadowAssignment is idempotent
        // per (view, frame) — the declaration path reads the identical result, so
        // the per-face mask and slot set cannot diverge (they are one computation,
        // not two pure re-derives). Casters stay culled to the LIGHT frustum.
        if (view.ActiveRenderLayerMask() != 0u && HasShadowCasters(view.id))
        {
            const auto& assignment = EnsurePointShadowAssignment(
                view.id, view.worldId, m_ViewRegistry.ResolveCameraData(view.id));
            for (const PointShadowFrameInfo& fi : assignment.Slots)
            {
                if (fi.shadowSlot < 0)
                    continue;
                // L1a: a cached light renders no faces this frame, so it needs no
                // GPU-cull contexts. Skipping here (and in pass declaration, which
                // reads this same idempotent assignment) keeps the M2a survivor
                // slice set consistent — a cached light contributes no slice, so
                // its (slot,face) survivor rows are simply absent (read as unknown).
                if (!fi.needsRender)
                    continue;
                const uint32_t slot = static_cast<uint32_t>(fi.shadowSlot);
                for (uint32_t face = 0; face < kPointShadowFaceCount; ++face)
                {
                    if ((fi.faceMask & (1u << face)) == 0u)
                        continue; // camera-culled face: no GPU cull context, no draws
                    Rendering::ViewCullingContext pointCtx{};
                    pointCtx.Id = view.id;
                    pointCtx.CascadeIndex = PointShadowCullingIndex(PointShadowSlotFaceLayer(slot, face));
                    pointCtx.ViewMatrix = fi.lightView[face];
                    pointCtx.ProjMatrix = fi.lightProj;
                    pointCtx.ViewProjMatrix = fi.lightVP[face];
                    Rendering::ExtractFrustumPlanes(fi.lightVP[face], pointCtx.FrustumPlanes);
                    pointCtx.CameraPosition = fi.position;
                    pointCtx.CameraForward = fi.direction[face];
                    pointCtx.NearPlane = fi.nearPlane;
                    pointCtx.FarPlane = fi.farPlane;
                    pointCtx.FirstInstance = 0;
                    pointCtx.InstanceCount = instanceCount;
                    pointCtx.RenderLayerMask = view.ActiveRenderLayerMask();
                    pointCtx.FrameIndex = thisFrame;
                    pointCtx.DeltaTime = deltaTime;
                    pointCtx.CullingPipeline = m_GpuCullingPipeline.get();
                    pointCtx.Scene = scene;
                    s_DefaultFrustum.ScheduleCulling(pointCtx);
                }
            }
        }
    }

    FeatureCullingContext featCtx{};
    featCtx.Frame = &frame;
    featCtx.CullingPipeline = m_GpuCullingPipeline.get();
    featCtx.Scene = scene;
    featCtx.Services = this;
    featCtx.Views = &m_ViewRegistry.GetViews();
    featCtx.Cameras = &cameras;
    featCtx.InstanceCount = instanceCount;
    featCtx.FrameIndex = thisFrame;
    featCtx.DeltaTime = deltaTime;
    ForEachFeature([&](IRenderFeature& feature) { feature.OnScheduleCulling(featCtx); });

    m_GpuCullingPipeline->EndFrame();

    // Publish the frame-local values the bucketer (and slices 3-4) consume.
    m_FrameRG.For.Stamp(frame);
    const GPUScene::GPUSceneFrameRG sceneRG = scene->ImportFrameResources(frame); // dedup'd
    m_FrameRG.SceneInstances = sceneRG.Instances;
    m_FrameRG.SceneScatterHot = sceneRG.ScatterHot;
    m_FrameRG.SceneMeshes = sceneRG.Meshes;
    m_FrameRG.Visibility = m_GpuCullingPipeline->GetVisibilityRG();
}

void RenderServices::BuildWorldBatchKeysForView(Rendering::ViewId viewId)
{
    // IMPORTANT:
    // This must NOT perform any global material/pipeline switching (e.g. Forward+ default world material),
    // otherwise editor-driven one-shot views (thumbnails, previews) can perturb Scene/Game views mid-frame.
    // MaterialColorClassSpan() ensures the map is current before returning it.
    // Compat keeps colorClassId == materialIndex so the batch keys and the CPU
    // instance lists share one key identity (see BuildWorldBatchKeys).
    m_WorldDrawBuilder.BuildBatchKeysForView(
        viewId, GetMeshGPURegistry(),
        m_Profile.IsCompat() ? std::span<const uint32_t>{}
                             : m_MaterialSystem.MaterialColorClassSpan());

    RaiseAnimatedVertexModifierContentSignals(viewId);

    if (m_Profile.IsCompat() && m_GpuScene)
    {
        m_CpuDrawStream.BuildForView(viewId, m_WorldDrawBuilder, GetMeshGPURegistry(), *m_GpuScene,
                                     m_ViewRegistry,
                                     &GameEngine::EngineCore::GetInstance().GetJobSystem());
    }

    const uint64_t frameIdx = m_GpuScene ? static_cast<uint64_t>(m_GpuScene->GetFrameIndex()) : 0ull;
    m_WorldDrawListsBuiltFrameIndex = frameIdx;
}

void RenderServices::RaiseAnimatedVertexModifierContentSignals(Rendering::ViewId viewId)
{
    if (!m_WorldDrawBuilder.HasAnimatedVertexModifierSubmissions(viewId))
        return;
    const Rendering::ViewDesc* view = m_ViewRegistry.FindViewDesc(viewId);
    if (!view)
        return;

    const bool castsShadows = m_WorldDrawBuilder.HasAnimatedVertexModifierCasterSubmissions(viewId);
    AnimatedVertexModifierWorld* entry = nullptr;
    for (AnimatedVertexModifierWorld& candidate : m_AnimatedVertexModifierWorlds)
    {
        if (candidate.WorldId == view->worldId)
        {
            entry = &candidate;
            break;
        }
    }
    if (entry && (entry->CastsShadows || !castsShadows))
        return;
    if (!entry)
    {
        m_AnimatedVertexModifierWorlds.push_back({view->worldId, false});
        entry = &m_AnimatedVertexModifierWorlds.back();
        // The vertex stage displaces this surface from the shared animation
        // clock, so its rasterized depth differs from last frame's while every
        // instance record stays byte-identical. The depth-derived elision
        // families have no other signal for it.
        NotifyRenderContentChanged(view->worldId);
    }
    if (castsShadows)
    {
        entry->CastsShadows = true;
        // Same argument one step further: the shadow caches would retain a
        // cascade layer rasterized at an older deformation, and the canopy
        // would sway while its shadow held still. The advance is unattributed:
        // the deforming casters are not named by a sphere set here, so the
        // atlas planner treats every light in the world as dirty.
        NotifyShadowCasterContentChanged(
            view->worldId, std::span<const ShadowCasterChangeSphere>{}, /*unattributed=*/true);
    }
}

void RenderServices::SubmitWorldSubmissions(std::span<const WorldSubmissionRecord> records)
{
    m_WorldDrawBuilder.Submit(records);
    m_WorldSubmissionCountThisFrame.fetch_add(static_cast<uint32_t>(records.size()), std::memory_order_relaxed);
}

std::span<const WorldDrawBuilder::BatchKey>
RenderServices::GetEntityBatchKeys(Rendering::ViewId viewId) const
{
    return m_WorldDrawBuilder.GetBatchKeys(viewId);
}

} // namespace Engine::Renderer
} // namespace GameEngine
