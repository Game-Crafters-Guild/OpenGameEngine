// RenderServicesDepthShadowPasses.cpp
// Part of the RenderServices implementation — split by concern from the
// former single RenderServices.cpp. All files define members of the same
// RenderServices class; shared file-scope helpers live in RenderServicesDetail.h.
#include "Engine/Rendering/RenderServices.h"
#include "Core/CpuProfiler.h"
#include "Engine/Rendering/DepthDrawRecorder.h"
#include "Engine/Rendering/Pipeline/ViewMotionVectors.h"
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
#include "Engine/Rendering/ShadowMapRenderFeature.h"
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
// L1a kill switch (GE_SHADOW_CACHE=0 forces every shadowed point light to
// re-render its faces each frame). ON-unless-"0", read once — the standard
// GE_ flag idiom (mirrors IsCasterReductionEnabled / GE_SHADOW_WALK_EARLYOUT).
bool IsPointShadowCacheEnabled()
{
    static const bool s_Enabled = []()
    {
        const char* env = std::getenv("GE_SHADOW_CACHE");
        return !env || std::strcmp(env, "0") != 0;
    }();
    return s_Enabled;
}

// L1a content signature: everything that makes a slot's cached depth stale
// EXCEPT the committed tier and the global caster epoch (folded in by the
// planner). Hashes the light's WORLD position + range + the 6-bit camera face
// mask — never a camera-relative value (that would re-hash every camera move
// and defeat the cache). FNV-1a over the raw float bits so an unmoved light
// produces a byte-stable key frame to frame.
uint64_t HashPointShadowContent(const float positionWS[3], float range, uint8_t faceMask, uint32_t renderLayerMask)
{
    constexpr uint64_t kFnvOffset = 14695981039346656037ull;
    constexpr uint64_t kFnvPrime = 1099511628211ull;
    uint64_t h = kFnvOffset;
    const auto mix32 = [&](uint32_t v)
    {
        for (int byte = 0; byte < 4; ++byte)
        {
            h ^= (v >> (byte * 8)) & 0xFFu;
            h *= kFnvPrime;
        }
    };
    mix32(std::bit_cast<uint32_t>(positionWS[0]));
    mix32(std::bit_cast<uint32_t>(positionWS[1]));
    mix32(std::bit_cast<uint32_t>(positionWS[2]));
    mix32(std::bit_cast<uint32_t>(range));
    mix32(renderLayerMask);
    h ^= faceMask;
    h *= kFnvPrime;
    return h;
}

const char* PointShadowDirtyCauseName(PointShadowDirtyCause cause)
{
    switch (cause)
    {
    case PointShadowDirtyCause::Cached:         return "Cached";
    case PointShadowDirtyCause::FirstRender:    return "FirstRender";
    case PointShadowDirtyCause::SlotReTenant:   return "ReTenant";
    case PointShadowDirtyCause::LodSelectionChanged: return "LodChanged";
    case PointShadowDirtyCause::CasterMoved:    return "CasterMoved";
    case PointShadowDirtyCause::TierChanged:    return "TierChanged";
    case PointShadowDirtyCause::ContentChanged: return "ContentChanged";
    case PointShadowDirtyCause::BudgetDeferred: return "BudgetDeferred";
    case PointShadowDirtyCause::CauseCount:     break;
    }
    return "?";
}

// L1a instrumentation: per-view rendered/cached slot counts with dirty-cause
// tags, logged only when the picture changes for that view (a steady static
// scene logs once, then stays silent).
void LogPointShadowCacheStats(Rendering::ViewId viewId,
                              const PointShadowAtlasPlanner::PlanResult& plan, uint32_t rendered,
                              uint32_t cached, std::unordered_map<Rendering::ViewId, uint64_t>& logState)
{
    constexpr size_t kCauseCount = static_cast<size_t>(PointShadowDirtyCause::CauseCount);
    std::array<uint32_t, kCauseCount> causes{};
    for (uint32_t slot = 0; slot < plan.SlotCount; ++slot)
    {
        const auto& sa = plan.Slots[slot];
        if (sa.TileResolution == 0u)
            continue;
        causes[static_cast<size_t>(sa.DirtyCause)] += 1u;
    }

    uint64_t sig = (static_cast<uint64_t>(rendered) << 32) ^ cached;
    for (uint32_t count : causes)
        sig = sig * 1099511628211ull + count;

    const auto [it, inserted] = logState.try_emplace(viewId, sig + 1u); // +1 => first call logs
    if (!inserted && it->second == sig)
        return;
    it->second = sig;
    const uint32_t key = static_cast<uint32_t>(viewId);

    std::string causeStr;
    for (size_t i = 1; i < kCauseCount; ++i) // skip Cached (index 0)
    {
        if (causes[i] == 0u)
            continue;
        if (!causeStr.empty())
            causeStr += ' ';
        causeStr += PointShadowDirtyCauseName(static_cast<PointShadowDirtyCause>(i));
        causeStr += '=';
        causeStr += std::to_string(causes[i]);
    }
    Logger::Log::Info("[PointShadow] view {}: rendered={} cached={}{}{}", key, rendered, cached,
                      causeStr.empty() ? "" : " | ", causeStr);
}
} // namespace

bool RenderServices::HasShadowCasters(Rendering::ViewId viewId) const
{
    // GPU-driven casters reach a view only as submissions: extraction (and every
    // package producer) submits each instance to each view of its world whose
    // layers it overlaps, and the shadow cull drops instances outside the
    // view's layers, so an instance with no submission here casts nothing here.
    return m_WorldDrawBuilder.HasShadowCastingSubmissions(viewId) ||
           HasDepthCommands(viewId, DepthPassType::ShadowCascade) ||
           HasDepthCommands(viewId, DepthPassType::AreaShadow) ||
           HasDepthCommands(viewId, DepthPassType::SpotShadow) ||
           HasDepthCommands(viewId, DepthPassType::PointShadow);
}

bool RenderServices::ViewNeedsShadowCascadePasses(Rendering::ViewId viewId) const
{
    for (const auto& v : m_ViewRegistry.GetViews())
    {
        if (v.id == viewId)
            return v.ActiveRenderLayerMask() != 0u && HasShadowCasters(viewId);
    }
    return false;
}

FeatureDeclareContext RenderServices::MakeFeatureDeclareContext(
    Rendering::RenderGraph::RGFrame& frame, Rendering::ViewId viewId,
    uint32_t cascadeCount, uint32_t punctualResolution,
    const float* directionalLightDirWS, std::function<std::string(const char*)> passName)
{
    FeatureDeclareContext ctx;
    ctx.View = viewId;
    ctx.CascadeCount = cascadeCount;
    ctx.PunctualResolution = punctualResolution;
    ctx.DirectionalLightDirWS = directionalLightDirWS;

    // Frame-validated spine snapshots. The IsFor guard is applied HERE, once, so
    // a feature's `if (ctx.SkinPaletteAtlas.IsValid())` matches the pre-move
    // `if (m_FrameRG.For.IsFor(...) && m_FrameRG.X.IsValid())` byte for byte — and a
    // torn-off window's declare can never snapshot the main window's spine values
    // (Q7 multi-window safety).
    const bool frameValid = m_FrameRG.For.IsFor(frame);
    if (frameValid && m_FrameRG.SkinPaletteAtlas.IsValid())
        ctx.SkinPaletteAtlas = m_FrameRG.SkinPaletteAtlas;
    if (frameValid && m_FrameRG.DrawStreamOrdering.IsValid())
        ctx.DrawStreamOrdering = m_FrameRG.DrawStreamOrdering;
    ctx.HasPendingSkinnedInstances = m_GPUAnimDataStore.GetInstanceCount() != 0;
    ctx.HasPendingBucketerSlices =
        m_DrawStreamBuilder && m_DrawStreamBuilder->GetPendingSliceCount() != 0;

    ctx.WorldAlreadyDeclared = ViewFrameRGFor(frame, viewId).WorldDeclared;
    ctx.ViewNeedsCascades = ViewNeedsShadowCascadePasses(viewId);
    ctx.HasTransmissiveCaster = HasTransmissiveCasterInView(viewId);

    // Punctual (area/spot/point) snapshots resolved ONCE per view and frame.
    // Present only under the gate: active render layers + shadow casters + a
    // valid light of that family. GetWorldLights is read once here.
    if (const auto* view = m_ViewRegistry.FindViewDesc(viewId);
        view && view->ActiveRenderLayerMask() != 0u && HasShadowCasters(viewId))
    {
        const auto lights = GetWorldLights(view->worldId);
        if (auto area = BuildAreaShadowFrameInfo(lights); area.valid)
            ctx.AreaShadow = area;
        if (auto spot = BuildSpotShadowFrameInfo(lights); spot.valid)
            ctx.SpotShadow = spot;
        // M1: the budgeted multi-light atlas assignment (tiers + hysteresis + slot
        // allocation) is STATEFUL across frames, so unlike the pure area/spot
        // builders it is computed ONCE per (view, device frame) and shared with the
        // GPU cull scheduler, batch registration, and light upload via
        // EnsurePointShadowAssignment (which advances the planner exactly once).
        const auto& assignment =
            EnsurePointShadowAssignment(viewId, view->worldId, m_ViewRegistry.ResolveCameraData(viewId));
        ctx.PointShadows = assignment.Slots;
        ctx.PointShadowBudget = m_PointShadowBudget;
    }

    ctx.PassName = std::move(passName);
    return ctx;
}

SamplerHandle RenderServices::GetAreaShadowSampler()
{
    if (!m_AreaShadowSampler.IsValid() && m_Device)
        m_AreaShadowSampler = m_Device->CreateSampler(SamplerDesc::ShadowComparePCF("AreaShadowPCF"));
    return m_AreaShadowSampler;
}

SamplerHandle RenderServices::GetAreaShadowRawSampler()
{
    if (!m_AreaShadowRawSampler.IsValid() && m_Device)
        m_AreaShadowRawSampler = m_Device->CreateSampler(SamplerDesc::ShadowClampNearest("AreaShadowRaw"));
    return m_AreaShadowRawSampler;
}

SamplerHandle RenderServices::GetSpotShadowSampler()
{
    if (!m_SpotShadowSampler.IsValid() && m_Device)
        m_SpotShadowSampler = m_Device->CreateSampler(SamplerDesc::ShadowComparePCF("SpotShadowPCF"));
    return m_SpotShadowSampler;
}

SamplerHandle RenderServices::GetPointShadowSampler()
{
    if (!m_PointShadowSampler.IsValid() && m_Device)
        m_PointShadowSampler = m_Device->CreateSampler(SamplerDesc::ShadowComparePCF("PointShadowPCF"));
    return m_PointShadowSampler;
}

SamplerHandle RenderServices::GetCascadeShadowSampler()
{
    if (!m_CascadeShadowSampler.IsValid() && m_Device)
        m_CascadeShadowSampler = m_Device->CreateSampler(SamplerDesc::ShadowComparePCF("CascadeShadowPCF"));
    return m_CascadeShadowSampler;
}

Rendering::RenderGraph::RGTexture RenderServices::ImportShadowMapArrayRG(
    Rendering::RenderGraph::RGFrame& frame, Rendering::ViewId viewId, ShadowDeclareSeam)
{
    auto& vfr = ViewFrameRGFor(frame, viewId);
    if (vfr.ShadowMapArray.IsValid())
        return vfr.ShadowMapArray;

    auto* feature = GetFeature<ShadowMapRenderFeature>();
    if (!feature || !feature->IsInitialized())
        return {};
    // Past every gate: this call WILL import, so the world pass is entitled to a
    // live array and may report its absence. Recorded here rather than re-derived
    // by the consumer because this is the only place that knows it — the gates
    // upstream (six in ShadowMapNode::DeclareForView, plus ViewNeedsCascades and
    // the directional-light resolve in the feature) are not observable from the
    // world pass, and a predicate that mirrors a subset of them fires on the rest.
    vfr.CascadeArrayOwed = true;
    const auto& cfg = feature->GetConfig();

    // Mirrors the old per-view shadow desc — minus `persistent` and
    // `initialState`: the pool + import-state write-back own lifetime and
    // state truth (a desc change = pool realloc + fresh Undefined physical,
    // which replaces the feature's recreate block structurally).
    // ACCEPTED EDGES (slice-4 decisions): (1) on the config-change frame the
    // world samples the fresh Undefined-cleared array for ONE frame (matches
    // the old recreate hiccup); (2) pool idle-eviction of a view's array frees
    // the image while the PCSS bindless slots still hold single-layer views of
    // it — the pool releases pooled physicals without notifying the consumers
    // that registered views on them, so the views outlive their image until the
    // view re-imports and the adopt invalidates them. That invalidation keys on
    // the handle CHANGING, and StrongHandle carries only an 8-bit generation
    // (Handle.h), so a reissue that wraps to the same id after 256 cycles is
    // indistinguishable from "unchanged" and the stale views survive.
    // Pre-existing and independent of the frame stamp below.
    //
    // Eviction is NOT confined to hidden views: a VISIBLE view whose casters go
    // away (a terrain-only scene) stops importing here, because all three import
    // sites are gated on ViewNeedsCascades, while its world pass keeps sampling
    // ge_shadowMapArray every frame. What makes that safe is the adopt frame
    // stamp: the feature serves the physical only to the frame it was adopted
    // for, so a view that stopped importing reports ABSENCE and the world pass
    // binds the typed 1x1 cascade fallback instead of a freed image. The
    // destroy-order exposure for already-registered bindless views remains;
    // closing it needs a pool release-notification and is tracked as its own
    // slice, not worked around here.
    Rendering::TextureDesc td{};
    td.width = cfg.Resolution;
    td.height = cfg.Resolution;
    td.depth = 1;
    td.mipLevels = 1;
    td.arrayLayers = cfg.NumCascades;
    td.format = static_cast<uint32_t>(Rendering::TextureFormat::D32_FLOAT);
    td.usage = static_cast<uint32_t>(Rendering::TextureUsage::DepthStencil)
             | static_cast<uint32_t>(Rendering::TextureUsage::ShaderResource);
    td.sampleCount = 1;
    td.flags = Rendering::TextureCreateFlags::ForceArrayView;
    const std::string name =
        "ShadowMapArray.View" + std::to_string(static_cast<uint32_t>(viewId));
    td.debugName = name.c_str();

    vfr.ShadowMapArray = frame.ImportPersistentTexture(name.c_str(), td);
    if (vfr.ShadowMapArray.IsValid())
        feature->AdoptPooledShadowMap(viewId, frame.PhysicalTexture(vfr.ShadowMapArray), *this,
                                      frame);
    return vfr.ShadowMapArray;
}

Rendering::RenderGraph::RGTexture RenderServices::ImportTransmittanceShadowArrayRG(
    Rendering::RenderGraph::RGFrame& frame, Rendering::ViewId viewId, ShadowDeclareSeam)
{
    auto& vfr = ViewFrameRGFor(frame, viewId);
    if (vfr.TransmittanceShadowArray.IsValid())
        return vfr.TransmittanceShadowArray;

    auto* feature = GetFeature<ShadowMapRenderFeature>();
    if (!feature || !feature->IsInitialized())
        return {};
    const auto& cfg = feature->GetConfig();

    // Colour twin of the depth shadow array: same resolution + cascade layers,
    // RGBA8 render target. The pool owns lifetime; a desc change reallocs (the
    // tint is re-rendered every frame, so a one-frame fresh-clear is harmless).
    Rendering::TextureDesc td{};
    td.width = cfg.Resolution;
    td.height = cfg.Resolution;
    td.depth = 1;
    td.mipLevels = 1;
    td.arrayLayers = cfg.NumCascades;
    td.format = static_cast<uint32_t>(Rendering::TextureFormat::RGBA8_UNORM);
    td.usage = static_cast<uint32_t>(Rendering::TextureUsage::RenderTarget)
             | static_cast<uint32_t>(Rendering::TextureUsage::ShaderResource);
    td.sampleCount = 1;
    td.flags = Rendering::TextureCreateFlags::ForceArrayView;
    char name[32]; // stack buffer (no per-frame heap alloc); matches the cascade-pass naming
    snprintf(name, sizeof(name), "GlassShadowTint.View%u", static_cast<uint32_t>(viewId));
    td.debugName = name;

    vfr.TransmittanceShadowArray = frame.ImportPersistentTexture(name, td);
    return vfr.TransmittanceShadowArray;
}

ResolvedPassResources RenderServices::BuildShadowPassResources(
    ShadowDeclareSeam, Rendering::RenderGraph::RGFrame& frame, Rendering::ViewId viewId,
    Rendering::BufferHandle camBuf, uint64_t camOffset)
{
    // Depth/shadow tables are keyword-less (no Shadows block) and have no world
    // targets — the narrow subset a shadow producer needs. Forwards to the
    // general private builder so the resolution logic stays in one place.
    return BuildPassResourcesRG(frame, viewId, Rendering::MaterialKeyword::None, nullptr, camBuf,
                                camOffset);
}

void RenderServices::PublishAreaShadow(ShadowDeclareSeam, Rendering::RenderGraph::RGFrame& frame,
                                       Rendering::ViewId viewId,
                                       Rendering::RenderGraph::RGTexture map,
                                       Rendering::RenderGraph::RGFrame::TypedUpload<AreaShadowDataGPU> data)
{
    auto& vfr = ViewFrameRGFor(frame, viewId);
    vfr.AreaShadowMap = map;
    vfr.AreaShadowData = data;
}

void RenderServices::PublishSpotShadow(ShadowDeclareSeam, Rendering::RenderGraph::RGFrame& frame,
                                       Rendering::ViewId viewId,
                                       Rendering::RenderGraph::RGTexture map,
                                       Rendering::RenderGraph::RGFrame::TypedUpload<SpotShadowDataGPU> data)
{
    auto& vfr = ViewFrameRGFor(frame, viewId);
    vfr.SpotShadowMap = map;
    vfr.SpotShadowData = data;
}

void RenderServices::PublishPointShadow(ShadowDeclareSeam, Rendering::RenderGraph::RGFrame& frame,
                                        Rendering::ViewId viewId,
                                        Rendering::RenderGraph::RGTexture map,
                                        Rendering::BufferHandle slotBuffer, uint64_t slotBufferOffset,
                                        uint64_t slotBufferBytes)
{
    auto& vfr = ViewFrameRGFor(frame, viewId);
    vfr.PointShadowMap = map;
    vfr.PointShadowData = {slotBuffer, slotBufferOffset, slotBufferBytes};
}

void RenderServices::PublishPcssPyramid(ShadowDeclareSeam, Rendering::RenderGraph::RGFrame& frame,
                                        Rendering::ViewId viewId,
                                        Rendering::RenderGraph::RGTexture pyramid)
{
    ViewFrameRGFor(frame, viewId).ShadowPcssPyramid = pyramid;
}

void RenderServices::SetPointShadowBudget(uint32_t budget)
{
    m_PointShadowBudget = std::clamp(budget, 1u, kMaxPointShadowSlots);
}

void RenderServices::NotifyShadowCasterContentChanged(
    uint64 worldId, std::span<const ShadowCasterChangeSphere> changedCasterSpheres,
    bool unattributed)
{
    ShadowCasterChanges& changes = m_ShadowCasterChanges[worldId];
    ++changes.Version;
    changes.Unattributed = unattributed;
    // The stored set explains exactly the advance to `Version`; the planner
    // applies it only across that single step (any gap globalizes there).
    changes.Spheres.assign(changedCasterSpheres.begin(), changedCasterSpheres.end());
}

const RenderServices::PointShadowAssignment& RenderServices::EnsurePointShadowAssignment(
    Rendering::ViewId viewId, uint64 worldId, const Rendering::CameraData& camData)
{
    const auto* viewDesc = m_ViewRegistry.FindViewDesc(viewId);
    const uint32_t renderLayerMask = viewDesc ? viewDesc->ActiveRenderLayerMask() : 0xFFFFFFFFu;
    PointShadowAssignment& entry = m_PointShadowAssignments[viewId];
    const uint64_t deviceFrame = m_Device ? m_Device->GetFrameIndex() : 0u;
    // Idempotent per (view, device frame): the first consumer this frame advances
    // the planner; the rest hit this early-return. The device frame index is
    // constant between the cull-scheduling phase and RG declaration of one app
    // frame, so both see the same value and the hysteresis ticks exactly once.
    if (entry.Computed && entry.Frame == deviceFrame)
        return entry;

    // A view presenting a different world restarts its planner: the caster epoch
    // is per-world, and one-step attribution across a rebind would compare
    // counters from different worlds (review hardening — a stale serve would
    // additionally need a SortId+ContentHash collision, but make it structural).
    if (entry.WorldId != worldId)
    {
        if (entry.WorldId != 0)
            m_PointShadowPlanners[viewId].Reset();
        entry.WorldId = worldId;
    }

    entry.Frame = deviceFrame;
    entry.Computed = true;
    entry.Slots.clear();
    entry.SlotOfCluster.clear();

    // No shadow-casting geometry in the view => no atlas is declared (the
    // declaration / cull / batch-reg gates all check this), so leave every light
    // unshadowed here too — otherwise a light could carry a shadowSlot the world
    // pass has no atlas SSBO to back.
    if (!HasShadowCasters(viewId))
        return entry;

    const auto lights = GetWorldLights(worldId);
    const auto pointCull = MakePointShadowCullInputs(camData);
    const PointShadowCameraCull* cull = pointCull ? &*pointCull : nullptr;

    // Enumerate shadow-casting point candidates (clusterable index matches the
    // LightUploadNode packing order), building each one's S1 geometry up front so
    // the admitted slots can be stamped without a re-derive.
    struct Candidate
    {
        PointShadowAtlasPlanner::Candidate Plan;
        PointShadowFrameInfo Geometry;
    };
    std::vector<Candidate> candidates;
    uint32_t clusterableIndex = 0;
    for (const auto& light : lights)
    {
        if (light.type == Components::LightType::Directional ||
            light.type == Components::LightType::Ambient)
            continue;
        const uint32_t cluster = clusterableIndex++;
        if (light.type != Components::LightType::Point || light.castsLight == 0 ||
            light.castsShadows == 0)
            continue;

        const Mathematics::Vector3 pos{light.positionWS[0], light.positionWS[1],
                                       light.positionWS[2]};
        Candidate c{};
        PopulatePointShadowGeometry(c.Geometry, pos, light.range, light.shadowResolutionTier, cluster,
                                    cull);
        if (!c.Geometry.valid)
            continue; // off-screen (S1 light-level reject) or every face culled
        c.Plan.SortId = light.SortId;
        c.Plan.ClusterIndex = cluster;
        // No cull inputs (degenerate camera / thumbnail) => treat as maximally
        // important so the light is admitted and rendered at the top tier.
        c.Plan.CoverageRadiusPx =
            cull ? PointShadowScreenCoverageRadiusPx(camData, pos, light.range)
                 : static_cast<float>(kPointAtlasTileResolution);
        c.Plan.ExplicitTier = light.shadowResolutionTier;
        c.Plan.InheritResolution = kPointAtlasTileResolution; // committed tier is never Inherit
        // L1a: the camera face mask is part of the cached key — a face becoming
        // visible must re-render the light (its layer was culled/never drawn).
        c.Plan.ContentHash =
            HashPointShadowContent(light.positionWS, light.range, c.Geometry.faceMask,
                                   renderLayerMask);
        // L1b: world-space influence sphere for caster-proximity keying (same
        // world-space source as the content hash — never camera-relative).
        c.Plan.InfluenceSphere[0] = light.positionWS[0];
        c.Plan.InfluenceSphere[1] = light.positionWS[1];
        c.Plan.InfluenceSphere[2] = light.positionWS[2];
        c.Plan.InfluenceSphere[3] = light.range;
        candidates.push_back(std::move(c));
    }
    entry.SlotOfCluster.assign(clusterableIndex, -1);

    // L1a atlas physical-lifetime guard (see m_PointShadowAtlasLiveness). If the
    // persistent atlas may have been recreated fresh since this view last rendered
    // — an IMPORT GAP (a frame where the assignment went empty => the atlas was not
    // imported => the pool aged it toward idle-eviction) or a BUDGET CHANGE
    // (arrayLayers = budget*6 realloc) — the planner's per-slot render cache is
    // stale-against-a-fresh-physical, so a "cached" slot would sample an Undefined
    // texture. Drop the render cache HERE, before Plan() decides, so the re-entry
    // frame itself re-renders every slot (glitch-free — a reactive post-import
    // "created fresh" signal would land a frame after cull scheduling already
    // skipped the cached slots). A monotonic GPUScene frame is used (IDevice's
    // GetFrameIndex is a cycling frame slot, not a counter).
    if (!candidates.empty())
    {
        const uint64_t monotonicFrame =
            GetGPUScene() ? GetGPUScene()->GetFrameIndex() : 0u;
        PointShadowAtlasLiveness& live = m_PointShadowAtlasLiveness[viewId];
        const bool importGap = !live.EverActive || live.LastActiveFrame + 1u != monotonicFrame;
        const bool reallocated = live.LastBudget != m_PointShadowBudget;
        if (importGap || reallocated)
            m_PointShadowPlanners[viewId].InvalidateRenderCache();
        live.EverActive = true;
        live.LastActiveFrame = monotonicFrame;
        live.LastBudget = m_PointShadowBudget;
    }

    std::vector<PointShadowAtlasPlanner::Candidate> planInputs;
    planInputs.reserve(candidates.size());
    for (const auto& c : candidates)
        planInputs.push_back(c.Plan);

    PointShadowAtlasPlanner::CacheInputs cache{};
    cache.CasterEpoch = ShadowCasterContentVersion(worldId);
    cache.RefreshBudget = kDefaultPointShadowRefreshBudget;
    cache.Enabled = IsPointShadowCacheEnabled();
    // The LOD selection knobs the point-face slices will register with. The
    // bucketer calls this same resolver and builds their ViewLODParams from its
    // result, so the key cannot describe knobs the slices did not use.
    cache.Lod = ResolvePointShadowLodKey(viewId);
    // L1b: hand the planner the changed casters explaining the latest epoch
    // advance. The span aliases the per-world store, which the single-writer
    // discipline keeps stable for the rest of this frame; Plan() does not
    // retain it.
    if (const auto csIt = m_ShadowCasterChanges.find(worldId); csIt != m_ShadowCasterChanges.end())
    {
        cache.ChangedCasters = csIt->second.Spheres;
        cache.Unattributed = csIt->second.Unattributed;
    }

    const auto plan = m_PointShadowPlanners[viewId].Plan(
        planInputs, m_PointShadowBudget, kDefaultPointShadowCooldownFrames, deviceFrame, cache);

    uint32_t renderedCount = 0;
    uint32_t cachedCount = 0;
    for (uint32_t slot = 0; slot < plan.SlotCount; ++slot)
    {
        const auto& sa = plan.Slots[slot];
        if (sa.TileResolution == 0u)
            continue; // gap left by an eviction
        const auto it = std::find_if(candidates.begin(), candidates.end(),
                                     [&](const Candidate& c) { return c.Plan.SortId == sa.SortId; });
        if (it == candidates.end())
            continue;
        PointShadowFrameInfo geo = it->Geometry;
        geo.shadowSlot = static_cast<int32_t>(slot);
        geo.tileResolution = sa.TileResolution;
        geo.needsRender = sa.NeedsRender;
        entry.Slots.push_back(geo);
        if (sa.ClusterIndex < entry.SlotOfCluster.size())
            entry.SlotOfCluster[sa.ClusterIndex] = static_cast<int32_t>(slot);
        (sa.NeedsRender ? renderedCount : cachedCount) += 1u;
    }

    LogPointShadowCacheStats(viewId, plan, renderedCount, cachedCount, m_PointShadowCacheLogState);
    return entry;
}

std::span<const int32_t> RenderServices::PointShadowSlotOfCluster(
    Rendering::ViewId viewId, uint64 worldId, const Rendering::CameraData& camData)
{
    return EnsurePointShadowAssignment(viewId, worldId, camData).SlotOfCluster;
}

DepthDrawServices RenderServices::MakeDepthDrawServices()
{
    return DepthDrawServices{
        m_WorldDrawBuilder, m_ViewRegistry, m_GpuScene.get(), m_MeshGPURegistry,
        m_DrawStreamBuilder.get(), m_MaterialSystem, m_Device, m_CullMode, m_FrontFace,
        MeshPoolGroupSpanForDraws(), &DepthUnderDraw(),
        m_Profile.IsCompat() ? &m_CpuDrawStream : nullptr};
}

DepthUnderDrawTracker& RenderServices::DepthUnderDraw()
{
    // Created eagerly in the constructor: this accessor is reached from
    // parallel-record exec workers via MakeDepthDrawServices, so a lazy
    // make_unique here would race.
    return *m_DepthUnderDraw;
}

Rendering::RenderGraph::RGPass RenderServices::AddWorldDepthPrepassForView(
    Rendering::RenderGraph::RGFrame& frame, ViewId viewId,
    Rendering::RenderGraph::RGTexture depth, float clearDepthValue)
{
    return AddWorldDepthPrepassImpl(frame, viewId, depth, clearDepthValue,
                                    Rendering::GPUDrawStreamBuilder::SlicePhase::A, PrepassHeads::Occluding);
}

Rendering::RenderGraph::RGPass RenderServices::AddWorldNonOccludingDepthPrepassForView(
    Rendering::RenderGraph::RGFrame& frame, ViewId viewId,
    Rendering::RenderGraph::RGTexture depth)
{
    const auto* pv = m_ViewRegistry.FindPerView(viewId);
    if (!pv || pv->NonOccludingPrepassHeads.empty() || pv->NonOccludingPrepassDeclared)
        return {};
    // Before the world pass, which depth-tests the heads: a later pass would draw them after the colour
    // draw that needs them. The camera prepass draws them instead (NonOccludingPrepassDeclared false).
    if (ViewFrameRGFor(frame, viewId).WorldDeclared)
        return {};
    // Only on top of this frame's camera prepass of the same depth: without it the colour pass
    // writes its own depth and there is nothing to load.
    const Rendering::RenderGraph::RGTexture prepassDepth = ViewFrameRGFor(frame, viewId).DepthPrepassDepth;
    if (!prepassDepth.IsValid() || prepassDepth.Id != depth.Id)
        return {};
    // clearDepthValue is unused: the pass LOADS the camera prepass's depth.
    const Rendering::RenderGraph::RGPass pass = AddWorldDepthPrepassImpl(
        frame, viewId, depth, 0.0f, Rendering::GPUDrawStreamBuilder::SlicePhase::A, PrepassHeads::NonOccluding);
    if (pass.IsValid())
        m_ViewRegistry.PerView(viewId).NonOccludingPrepassDeclared = true;
    return pass;
}

Rendering::RenderGraph::RGPass RenderServices::AddWorldDepthRecoverPassForView(
    Rendering::RenderGraph::RGFrame& frame, ViewId viewId,
    Rendering::RenderGraph::RGTexture depth)
{
    // clearDepthValue is unused for phase B — the pass LOADS the phase-A depth.
    return AddWorldDepthPrepassImpl(frame, viewId, depth, 0.0f,
                                    Rendering::GPUDrawStreamBuilder::SlicePhase::B, PrepassHeads::Occluding);
}

Rendering::RenderGraph::RGPass RenderServices::AddDeformationMotionPassForView(
    Rendering::RenderGraph::RGFrame& frame, ViewId viewId,
    Rendering::RenderGraph::RGTexture motionTarget, Rendering::RenderGraph::RGTexture depth,
    const DeformationMotionEndpoint& endpoint, bool clearTarget,
    Rendering::GPUDrawStreamBuilder::SlicePhase phase)
{
    namespace RenderGraph = Rendering::RenderGraph;

    if (!motionTarget.IsValid() || !depth.IsValid())
        return {};
    // Single-sample only, by design. Attachments of one pass must agree on
    // sample count, and a hardware average of this payload would mix two
    // surfaces' motion — or a sentinel with a real vector — into a value
    // describing neither. Multisampling and temporal anti-aliasing are
    // mutually exclusive here and the reflection consumer withholds itself
    // under multisampling, so no consumer of this target exists on such a
    // view; the guard is what keeps that a refusal rather than a mismatch.
    if (frame.Graph().ResourceDesc(depth.Id).SampleCount != 1)
    {
        static bool warnedOnce = false;
        if (!warnedOnce)
        {
            warnedOnce = true;
            Logger::Log::Warning(
                "Deformation motion: view {} renders multisampled depth, and the motion payload "
                "has no correct multisample resolve — no deforming motion is produced for it.",
                static_cast<uint32_t>(viewId));
        }
        return {};
    }

    std::string passName = "DeformationMotion[View#";
    passName += std::to_string(static_cast<uint32_t>(viewId));
    passName += "]";
    if (phase == Rendering::GPUDrawStreamBuilder::SlicePhase::B)
        passName += "B";

    DepthOnlyPassParamsRG params{};
    params.ViewId = viewId;
    params.PassType = DepthPassType::DeformationMotion;
    params.Phase = phase;
    params.DepthBiasEnable = false;
    params.RasterizationSamples = 1;
    const ViewLetterbox letterbox = m_ViewRegistry.GetViewLetterbox(viewId);
    if (letterbox.active)
    {
        params.ViewportX = letterbox.x;
        params.ViewportY = letterbox.y;
        params.ViewportWidth = letterbox.width;
        params.ViewportHeight = letterbox.height;
    }
    else
    {
        const auto& d = frame.Graph().ResourceDesc(motionTarget.Id);
        params.ViewportWidth = d.Width;
        params.ViewportHeight = d.Height;
    }

    // The raster domain is the prepass's, jitter included: this pass tests
    // GreaterOrEqual against the depth that prepass wrote, so a clip position
    // that differs by so much as the jitter offset would fail the test on the
    // silhouette. The payload takes the jitter back off in the fragment, from
    // the block below, rather than by rasterizing somewhere else.
    auto cam = frame.AllocUpload<CameraData>();
    if (cam.Valid())
        *cam.Ptr = m_ViewRegistry.ResolveJitteredCameraData(
            viewId, frame.FrameIndex(), params.ViewportWidth, params.ViewportHeight);

    // The per-view block both stages read. Its raster half is built here,
    // from the viewport this pass is about to set and from the jitter the
    // camera above was just frozen with — reading either anywhere else would
    // let the fragment's reconstruction disagree with its own gl_FragCoord.
    // A view whose endpoint pair is not differenceable produces no pass at
    // all: the discontinuity policy is "no draw, and the sentinel stands",
    // never a zero vector.
    DeformationMotionRaster raster{};
    raster.Viewport = Mathematics::Rect{static_cast<float>(params.ViewportX),
                                        static_cast<float>(params.ViewportY),
                                        static_cast<float>(params.ViewportWidth),
                                        static_cast<float>(params.ViewportHeight)};
    if (const auto* antiAliasing = m_ViewRegistry.FindViewAntiAliasing(viewId);
        antiAliasing != nullptr && antiAliasing->Enabled)
    {
        raster.NdcJitterX = antiAliasing->NdcJitterX;
        raster.NdcJitterY = antiAliasing->NdcJitterY;
    }
    DeformationMotionParamsGPU motionParams{};
    if (!BuildDeformationMotionParams(endpoint, raster, motionParams))
        return {};

    // Written into the per-(view, frame) slot the resolved table binds by
    // name, so the phase-B pass reuses the phase-A upload rather than
    // allocating a second one.
    ViewFrameRG& vfr = ViewFrameRGFor(frame, viewId);
    if (!vfr.DeformationMotion.Valid())
    {
        vfr.DeformationMotion = frame.AllocUpload<DeformationMotionParamsGPU>();
        if (!vfr.DeformationMotion.Valid())
            return {};
        *vfr.DeformationMotion.Ptr = motionParams;
    }

    params.Resources = BuildPassResourcesRG(frame, viewId, Rendering::MaterialKeyword::None,
                                            nullptr, cam.Buffer, cam.Offset);

    return frame.AddPass(
        passName.c_str(), Rendering::PassPhase::kDefault,
        [&](RenderGraph::RGPassBuilder& p)
        {
            RenderGraph::RGAttachmentOps colorOps{};
            // One owner clears this target. The producer runs first and owns
            // the clear; the mover lane that follows loads, so neither erases
            // the other's writes. Phase B always loads — it completes phase A's
            // image rather than replacing it.
            colorOps.Load = clearTarget ? RenderGraph::RGLoadOp::Clear
                                        : RenderGraph::RGLoadOp::Load;
            colorOps.Store = RenderGraph::RGStoreOp::Store;
            colorOps.Clear.Color[0] = Pipeline::kMotionVectorSentinelDelta;
            colorOps.Clear.Color[1] = Pipeline::kMotionVectorSentinelDelta;
            p.AttachColor(0, motionTarget, colorOps);
            RenderGraph::RGAttachmentOps depthOps{};
            depthOps.Load = RenderGraph::RGLoadOp::Load;
            p.AttachDepth(depth, depthOps, RenderGraph::RGDepthAccess::ReadOnly);
            if (m_FrameRG.For.IsFor(frame) && m_FrameRG.DrawStreamOrdering.IsValid())
                p.Read(m_FrameRG.DrawStreamOrdering, RenderGraph::RGBufferRead::Indirect);
        },
        [this, params = std::move(params)](RenderGraph::RGContext& ctx) mutable
        { RecordDepthOnlyPass(ctx, std::move(params), MakeDepthDrawServices()); });
}

Rendering::RenderGraph::RGPass RenderServices::AddWorldDepthPrepassImpl(
    Rendering::RenderGraph::RGFrame& frame, ViewId viewId,
    Rendering::RenderGraph::RGTexture depth, float clearDepthValue,
    Rendering::GPUDrawStreamBuilder::SlicePhase phase, PrepassHeads heads)
{
    namespace RenderGraph = Rendering::RenderGraph;

    const bool isRecover = phase == Rendering::GPUDrawStreamBuilder::SlicePhase::B;
    // The non-occluding prepass loads the camera prepass's depth and adds the non-occluding heads.
    const bool isNonOccluding = !isRecover && heads == PrepassHeads::NonOccluding;

    if (!depth.IsValid())
        return {};
    const ViewDesc* view = nullptr;
    for (const auto& v : m_ViewRegistry.GetViews())
    {
        if (v.id == viewId)
        {
            view = &v;
            break;
        }
    }
    if (!view)
        return {};

    // Phase A precedes the phase-A world pass (it clears; the world loads and
    // depth-tests) — declaring it after the world would silently derive WAR
    // and lose clear suppression, so the tripwire fires. Phase B is the HZB
    // recover pass: it LEGITIMATELY follows the phase-A world raster (its HZB
    // input derives from that depth), so the same tripwire must not fire for
    // it. The phase-B misorder guard (vs the phase-B world color pass) lands
    // with the P2 color double-record.
    if (!isRecover && ViewFrameRGFor(frame, viewId).WorldDeclared)
        Logger::Log::Error(
            "[RenderGraph] AddWorldDepthPrepassForView(view {}): the world pass already declared — a "
            "later prepass derives WAR (the world depth-tests frame N−1) and loses clear "
            "suppression; declare producers BEFORE the world pass",
            viewId);

    std::string passName = "DepthPrepass[";
    if (view->debugName && view->debugName[0] != '\0')
        passName += view->debugName;
    else
        passName += "View";
    passName += "#";
    passName += std::to_string(static_cast<uint32_t>(viewId));
    passName += "]";
    if (isRecover)
        passName += "B";
    if (isNonOccluding)
        passName += "NonOccluding";

    DepthOnlyPassParamsRG params{};
    params.ViewId = viewId;
    params.PassType = DepthPassType::Prepass;
    params.Phase = phase;
    params.Heads = isNonOccluding ? PrepassHeads::NonOccluding : PrepassHeads::Occluding;
    params.DepthBiasEnable = false;
    params.RasterizationSamples = 0;
    const ViewLetterbox letterbox = m_ViewRegistry.GetViewLetterbox(viewId);
    if (letterbox.active)
    {
        params.ViewportX = letterbox.x;
        params.ViewportY = letterbox.y;
        params.ViewportWidth = letterbox.width;
        params.ViewportHeight = letterbox.height;
    }
    else
    {
        const auto& d = frame.Graph().ResourceDesc(depth.Id);
        params.ViewportWidth = d.Width;
        params.ViewportHeight = d.Height;
    }

    // Raster domain: TAA views rasterize with the jittered projection (no-op
    // when TAA is off — identical to ResolveCameraData).
    auto cam = frame.AllocUpload<CameraData>();
    if (cam.Valid())
        *cam.Ptr = m_ViewRegistry.ResolveJitteredCameraData(
            viewId, frame.FrameIndex(), params.ViewportWidth, params.ViewportHeight);

    // Table built keyword-less: depth tables never carry the Shadows block —
    // the only keyword the table builder consults. The Instanced narrowing
    // for variants/BeginPass happens at exec (see DepthOnlyPassParamsRG).
    params.Resources = BuildPassResourcesRG(frame, viewId, Rendering::MaterialKeyword::None,
                                            nullptr, cam.Buffer, cam.Offset);

    const RenderGraph::RGPass pass = frame.AddPass(
        passName.c_str(), Rendering::PassPhase::kWorldRender,
        [&](RenderGraph::RGPassBuilder& p)
        {
            // A2.4-P0-R: the depth prepass exec is self-contained (own
            // viewport/scissor + per-draw binds) — eligible for secondary record.
            p.RecordInSecondary();
            // The skin palette and the draw-stream ordering feed the entity batches only. The
            // non-occluding prepass draws none, and its heads bind neither, so it declares neither
            // read (each would add a graph edge from the skinning or the scatter into it).
            if (!isNonOccluding)
            {
                if (m_FrameRG.For.IsFor(frame) && m_FrameRG.SkinPaletteAtlas.IsValid())
                    p.Read(m_FrameRG.SkinPaletteAtlas);
                else if (m_GPUAnimDataStore.GetInstanceCount() != 0)
                    Logger::Log::Error(
                        "[RenderGraph] AddWorldDepthPrepassForView(view {}): skinned instances pending "
                        "but no SkinPaletteAtlas value is published — declare "
                        "ScheduleGpuSkinningAndRetarget(frame) BEFORE the depth prepass",
                        viewId);
                if (m_FrameRG.For.IsFor(frame) && m_FrameRG.DrawStreamOrdering.IsValid())
                    p.Read(m_FrameRG.DrawStreamOrdering, RenderGraph::RGBufferRead::Indirect);
                else if (m_DrawStreamBuilder && m_DrawStreamBuilder->GetPendingSliceCount() != 0)
                    Logger::Log::Error(
                        "[RenderGraph] AddWorldDepthPrepassForView(view {}): buckets are registered but "
                        "no DrawStreamOrdering value is published — declare "
                        "ScheduleWorldBucketerDispatches(frame) BEFORE the depth prepass",
                        viewId);
            }
            // The buffers the forward producers' prepass heads read (indirect args and the
            // vertex-stage SSBOs their compute wrote this frame): declaring them orders that
            // compute before this pass and lands the barriers. Phase A draws the heads; the
            // recover pass draws none.
            if (!isRecover)
            {
                for (const auto& bufferRead : m_ViewRegistry.PerView(viewId).ForwardSampledBufferRG)
                {
                    if (bufferRead.Readers == ForwardBufferReaders::WorldPassAndPrepass &&
                        bufferRead.For.IsFor(frame) && bufferRead.Buffer.IsValid())
                        p.Read(bufferRead.Buffer, bufferRead.Access);
                }
            }

            RenderGraph::RGAttachmentOps dops{};
            // Phase B loads the phase-A depth and depth-tests newly-revealed
            // fragments against it (GreaterOrEqual pipeline default); the
            // non-occluding prepass loads it and adds its heads; phase A
            // clears to the far value.
            dops.Load = (isRecover || isNonOccluding) ? RenderGraph::RGLoadOp::Load : RenderGraph::RGLoadOp::Clear;
            dops.Store = RenderGraph::RGStoreOp::Store;
            dops.Clear.Depth = clearDepthValue;
            p.AttachDepth(depth, dops, RenderGraph::RGDepthAccess::ReadWrite);
            // No PreventCulling: the world pass's AttachDepth(Load) derives
            // the read that keeps this alive. A frame where the world CLEARS
            // depth instead culls the prepass — correct dead-work elimination.
        },
        [this, params = std::move(params)](RenderGraph::RGContext& ctx) mutable
        { RecordDepthOnlyPass(ctx, std::move(params), MakeDepthDrawServices()); });

    if (isNonOccluding)
        return pass;
    ViewFrameRGFor(frame, viewId).DepthPrepassDepth = depth;
    // The formats the prepass draws with, for the forward producers that pin their heads' pipelines at
    // next frame's emit (PerViewResources::PrepassFormatKey). A formatless import keeps the last snapshot.
    if (!isRecover)
    {
        const RenderGraph::RGResourceDesc& depthDesc = frame.Graph().ResourceDesc(depth.Id);
        if (depthDesc.Format != 0u)
        {
            const RenderGraph::RGAttachmentKeyInput attachment{
                0u, /*IsDepth=*/true, static_cast<Rendering::TextureFormat>(depthDesc.Format), depthDesc.SampleCount};
            m_ViewRegistry.PerView(viewId).PrepassFormatKey =
                RenderGraph::DeriveFormatKey(&attachment, 1u, Rendering::TextureFormat{});
        }
    }
    return pass;
}

} // namespace Engine::Renderer
} // namespace GameEngine
