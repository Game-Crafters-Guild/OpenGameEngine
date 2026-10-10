// PipelineVariantCache implementation — see PipelineVariantCache.h for the
// service contract.
#include "Engine/Rendering/PipelineVariantCache.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/MaterialSystem.h"
#include "Core/CpuProfiler.h"
#include "Engine/Rendering/IRenderFeature.h"

#include "Core/DebugMetrics.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MaterialBinder.h"
#include "Engine/Rendering/MaterialCompiler.h"
#include "Engine/Rendering/ShaderGraphMaterial.h"
#include "Engine/Rendering/ParallaxReliefDepth.h"
#include "Engine/Rendering/DepthDrawRecorder.h"
#include "Rendering/Geometry/VertexLayoutBuilder.h"
#include "Rendering/Materials/MaterialBuildService.h"
#include "Rendering/Materials/MaterialKeywordDerivation.h"
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
#include "Engine/Rendering/Pipeline/Nodes/ShadowMapNode.h"
#include "Engine/Rendering/Pipeline/Nodes/IBLGenNode.h"
#include "Engine/Rendering/Pipeline/Nodes/ComputeShaderNode.h"
#include "Engine/Rendering/Pipeline/Nodes/DepthPrepassNode.h"
#include "Engine/Rendering/Pipeline/Nodes/DepthResolveNode.h"
#include "Engine/Rendering/Pipeline/Nodes/AONode.h"
#include "Engine/Rendering/Pipeline/Nodes/AutoExposureNode.h"
#include "Engine/Rendering/Pipeline/Nodes/LightUploadNode.h"
#include "Engine/Rendering/Pipeline/Nodes/ViewParamsUploadNode.h"
#include "Engine/Rendering/Pipeline/Nodes/HeightFogParamsUploadNode.h"
#include "Engine/Rendering/Pipeline/Nodes/FullscreenShaderNode.h"
#include "Engine/Rendering/Pipeline/Nodes/SkyRenderNode.h"
#include "Engine/Rendering/Pipeline/Nodes/LensFlareRenderNode.h"
#include "Engine/Rendering/Pipeline/Nodes/VolumetricFogNode.h"
#include "Engine/Rendering/Pipeline/Nodes/TransmissivePassNode.h"
#include "Engine/Rendering/Pipeline/Nodes/WorldRenderNode.h"
#include "Engine/Rendering/Pipeline/RenderPipeline.h"
#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"
#include "Logger/Logger.h"
#include "Rendering/Common/Utils.h"
#include "Rendering/Core/BindlessResourceManager.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/DeviceFormatting.h"
#include "Rendering/Core/PipelineCache.h"
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
#include <shared_mutex>
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

uint64_t PipelineVariantCache::NextCacheEpoch() noexcept
{
    static std::atomic<uint64_t> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}

PipelineVariantCache::PipelineVariantCache(MaterialSystem& materials)
    : m_Materials(&materials)
    , m_CacheEpoch(NextCacheEpoch())
{
}

namespace
{
std::string FormatPipelineKey(const Rendering::PipelineFormatKey& fk)
{
    std::string colorFormats;
    for (uint8_t i = 0; i < fk.ColorCount && i < Rendering::PipelineFormatKey::kMaxColors; ++i)
    {
        if (!colorFormats.empty())
            colorFormats += ",";
        colorFormats += Rendering::ToString(fk.ColorFormats[i]);
    }
    if (colorFormats.empty())
        colorFormats = "none";

    return "colors=" + colorFormats +
           " depth=" + Rendering::ToString(fk.DepthFormat) +
           " stencil=" + Rendering::ToString(fk.StencilFormat) +
           " samples=" + std::to_string(static_cast<uint32_t>(fk.RasterizationSamples));
}

// Fibonacci-hash the cache-key hash down to a power-of-two L1 slot index,
// matching the device PipelineCache's L1IndexFor. kVariantL1Slots == 256, so
// 8 index bits.
inline size_t VariantL1Index(size_t keyHash) noexcept
{
    return static_cast<size_t>(
        (static_cast<uint64_t>(keyHash) * 0x9E3779B97F4A7C15ull) >> (64 - 8));
}

} // namespace

// Hardware depth bias for shadow map rendering. The constant factor offsets all
// fragments by a fixed amount in depth-buffer units, while the slope factor
// scales with the surface's depth gradient so steep triangles get more bias.
// Reverse-Z + D32_FLOAT: signs are negative so the bias pushes fragments toward
// 0.0 (away from light); the magnitudes are unchanged from the forward-Z
// values that were tuned for this engine's CSM scale.
static constexpr float kShadowDepthBiasConstant = -1.25f;

static constexpr float kShadowDepthBiasSlope = -1.75f;

void PipelineVariantCache::RequestConcreteWarm(const Material& material,
                                               Rendering::GraphicsPipelineId id,
                                               const Rendering::PipelineFormatKey& fk)
{
    if (!id.IsValid())
        return;
    const uint64_t warmKey = Rendering::PipelineCache::CombineGraphicsKey(id, fk);
    // Outside m_ConcreteWarmMutex: with no job system the device builds inline.
    const Rendering::PipelineBuildState state = m_Materials->GetDevice()->RequestGraphicsPipeline(id, fk);
    std::lock_guard lock(m_ConcreteWarmMutex);
    if (state != Rendering::PipelineBuildState::Pending)
    {
        m_ConcreteWarmWaits.erase(warmKey);
        return;
    }
    ConcreteWarmWait& wait = m_ConcreteWarmWaits[warmKey];
    wait.Id = id;
    wait.FormatKey = fk;
    ++wait.Generation;
    if (std::find(wait.Materials.begin(), wait.Materials.end(), &material) == wait.Materials.end())
        wait.Materials.push_back(&material);
}

void PipelineVariantCache::PruneSettledConcreteWarmWaits()
{
    struct AskedWait
    {
        uint64_t WarmKey = 0;
        uint64_t Generation = 0;
        Rendering::GraphicsPipelineId Id{};
        Rendering::PipelineFormatKey FormatKey{};
    };
    std::vector<AskedWait> asked;
    {
        std::lock_guard lock(m_ConcreteWarmMutex);
        if (m_ConcreteWarmWaits.empty())
            return;
        std::erase_if(m_ConcreteWarmWaits, [](const auto& entry) { return entry.second.Materials.empty(); });
        asked.reserve(m_ConcreteWarmWaits.size());
        for (const auto& [warmKey, wait] : m_ConcreteWarmWaits)
            asked.push_back({warmKey, wait.Generation, wait.Id, wait.FormatKey});
    }
    // Asked outside the lock, as HasPipelineBuildsInFlight asks: a build the
    // device dropped unrun is queued again, since its materials still wait.
    Rendering::IDevice* device = m_Materials->GetDevice();
    std::erase_if(asked,
                  [device](const AskedWait& wait)
                  {
                      return device->RequestGraphicsPipeline(wait.Id, wait.FormatKey) ==
                             Rendering::PipelineBuildState::Pending;
                  });
    std::lock_guard lock(m_ConcreteWarmMutex);
    for (const AskedWait& settled : asked)
        if (auto it = m_ConcreteWarmWaits.find(settled.WarmKey);
            it != m_ConcreteWarmWaits.end() && it->second.Generation == settled.Generation)
            m_ConcreteWarmWaits.erase(it);
}

void PipelineVariantCache::WarnColdServe(const Material& material,
                                         Rendering::GraphicsPipelineId pipelineId,
                                         const Rendering::PipelineFormatKey& fk)
{
    // Dual-cold window (current AND previous cold for this pass): the caller
    // skips the draw and a worker is already building, so this fires per draw
    // per frame until the build lands. First occurrence, then every power of
    // two — a normal build window logs a handful of lines and stops.
    const uint64_t n = m_ColdServeWarnCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if ((n & (n - 1)) != 0)
        return;
    Logger::Log::Warning(
        "GetOrCompileColorVariant '{}': cached graphics pipeline id={} failed for current pass ({}) [{}x]",
        material.GetName(), pipelineId.Value, FormatPipelineKey(fk), n);
}

template <class Ctx>
std::pair<Rendering::PipelineHandle, const PipelineVariantCache::VariantServeUnit*>
PipelineVariantCache::ResolveWarmPipeline(const Material& material,
                                          const InstancedVariantEntry& entry,
                                          uint32_t materialIndex, ServedPassClass passClass,
                                          const Ctx& ctx)
{
    const Rendering::PipelineFormatKey fk = ctx.BuildCurrentFormatKey();
    Rendering::IDevice* device = m_Materials->GetDevice();
    if (auto h = device->TryGetWarmGraphicsPipeline(entry.Current.PipelineId, fk); h.IsValid())
    {
        m_ServedGenerations.Record(materialIndex, passClass, entry.Current.MaterialVersion);
        return {h, &entry.Current};
    }

    // Cold for this pass. Building here would run the backend's shader
    // compilation on the record thread — hundreds of ms, the whole frame — so
    // hand it to a worker and serve the entry's previous unit meanwhile:
    // pipeline, meta and set layouts as one snapshot, so the descriptor set the
    // binder builds matches the pipeline layout it binds.
    RequestConcreteWarm(material, entry.Current.PipelineId, fk);
    if (entry.Previous.PipelineId.IsValid())
    {
        auto h = device->TryGetWarmGraphicsPipeline(entry.Previous.PipelineId, fk);
        if (h.IsValid())
            m_ServedGenerations.Record(materialIndex, passClass, entry.Previous.MaterialVersion);
        return {h, &entry.Previous};
    }
    return {{}, nullptr};
}

template <class Ctx>
std::pair<Rendering::PipelineHandle, const PipelineVariantCache::VariantServeUnit*>
PipelineVariantCache::GetOrCompileColorVariantImpl(
    const Material& material,
    Rendering::VertexAttributeFlags vertexFlags,
    Rendering::PrimitiveTopology topology,
    Rendering::MaterialKeyword passKeywords,
    Rendering::FrontFace frontFace,
    const Ctx& ctx,
    VariantCacheMap& cache)
{
    InstancedVariantKey key{
        &material,
        vertexFlags,
        ctx.GetCurrentSampleCount(),
        Rendering::NarrowColorPassKeywords(material.GetVariantKey().materialKeywords, passKeywords),
        topology,
        material.IsDoubleSided() ? Rendering::CullModeFlagBits::None : m_Materials->DebugCullMode(),
        frontFace};

    const uint32_t materialVersion = material.GetVersion();
    const uint32_t materialIndex = material.GetGpuSceneMaterialIndex();
    const uint64_t epoch = m_CacheEpoch.load(std::memory_order_acquire);

    // Hit path: a per-thread L1 (no lock) fronts the map, then a shared_lock
    // find. A2.4-D6: up to N record workers share the map concurrently, so the
    // find is a pure `find` (operator[] would mutate on read) under a
    // shared_lock, and the L1 lets steady-state hits skip the lock entirely.
    // The concrete PipelineHandle still resolves per-draw via ctx (keyed on the
    // active pass format) so one variant used through two passes gets the right
    // handle each time.
    thread_local std::array<InstancedL1Slot, kVariantL1Slots> l1{};
    auto& slot = l1[VariantL1Index(InstancedVariantKeyHash{}(key))];
    if (slot.Epoch == epoch && slot.Entry != nullptr
        && slot.Key == key
        && slot.Entry->Current.PipelineId.IsValid()
        && slot.Entry->Current.MaterialVersion == materialVersion)
    {
        const auto serve =
            ResolveWarmPipeline(material, *slot.Entry, materialIndex, ServedPassClass::Color, ctx);
        if (!serve.first.IsValid())
            WarnColdServe(material, slot.Entry->Current.PipelineId, ctx.BuildCurrentFormatKey());
        return serve;
    }
    // Stale-serve: an entry whose Current generation has moved on (a shader edit
    // recompiled the material) still holds a working pipeline. Keep drawing it
    // and rebuild in the background — evicting first would leave the object out
    // of the frame for the whole rebuild. `staleEntry` is set inside the lock and
    // dereferenced after it, which is safe because node writes (the serial
    // publish apply and the eviction drain) are phase-separated from the record
    // window; the enqueue below never nests the cache lock inside the pending one.
    const InstancedVariantEntry* staleEntry = nullptr;
    {
        std::shared_lock lock(m_CacheMutex);
        if (auto it = cache.find(key);
            it != cache.end() && it->second.Current.PipelineId.IsValid())
        {
            if (it->second.Current.MaterialVersion == materialVersion)
            {
                slot = {key, &it->second, epoch};
                const auto serve =
                    ResolveWarmPipeline(material, it->second, materialIndex, ServedPassClass::Color, ctx);
                if (!serve.first.IsValid())
                    WarnColdServe(material, it->second.Current.PipelineId,
                                  ctx.BuildCurrentFormatKey());
                return serve;
            }
            staleEntry = &it->second;
        }
    }

    // Publish-gate (record worker): a cold miss must NOT compile here — a shaderc
    // variant compile is 10+s cold and would stall the record thread / scene-build
    // pump. Enqueue an async compile (deduped) that MaterialSystem::BeginFrame's
    // serial drain submits to the prewarm workers. A first-ever miss skips this
    // draw; the caller already handles {invalid, nullptr} and the mesh self-heals
    // to visible once the variant lands (a few frames later). The build context is
    // populated serially in BeginFrame; if it is not ready yet, skip WITHOUT enqueue
    // and let a later frame retry (the compile would fail anyway).
    if (!m_Materials->IsMaterialBuildContextReady())
    {
        if (staleEntry == nullptr)
            Logger::Log::Warning(
                "GetOrCompileColorVariant '{}': material build context unavailable ({})",
                material.GetName(),
                FormatPipelineKey(ctx.BuildCurrentFormatKey()));
        return staleEntry != nullptr
            ? ResolveWarmPipeline(material, *staleEntry, materialIndex, ServedPassClass::Color, ctx)
            : std::pair<PipelineHandle, const VariantServeUnit*>{{}, nullptr};
    }
    EnqueueVariantCompile(key, /*isDepth=*/false);
    if (staleEntry != nullptr)
        return ResolveWarmPipeline(material, *staleEntry, materialIndex, ServedPassClass::Color, ctx);
    return {{}, nullptr};
}

std::pair<Rendering::PipelineHandle, const PipelineVariantCache::VariantServeUnit*>
PipelineVariantCache::GetOrCompileColorVariant(
    const Material& material,
    Rendering::VertexAttributeFlags vertexFlags,
    Rendering::PrimitiveTopology topology,
    Rendering::MaterialKeyword passKeywords,
    Rendering::FrontFace frontFace,
    const Rendering::RenderGraph::RGContext& ctx)
{
    return GetOrCompileColorVariantImpl(material, vertexFlags, topology, passKeywords, frontFace,
                                        ctx, m_InstancedVariantCache);
}

template <class Ctx>
std::pair<Rendering::PipelineHandle, const PipelineVariantCache::VariantServeUnit*>
PipelineVariantCache::GetOrCompileDepthInstancedVariantImpl(
    const Material& material,
    Rendering::VertexAttributeFlags vertexFlags,
    Rendering::PrimitiveTopology topology,
    Rendering::MaterialKeyword passKeywords,
    uint32_t samples,
    bool depthBiasEnable,
    bool depthClampEnable,
    Rendering::FrontFace frontFace,
    const Ctx& ctx)
{
    auto& cache = m_DepthInstancedVariantCache;
    const Rendering::CullModeFlags depthCullMode = material.IsDoubleSided()
        ? Rendering::CullModeFlagBits::None
        : m_Materials->DebugCullMode();
    InstancedVariantKey key{&material,     vertexFlags,      samples,
                            passKeywords,  topology,         depthCullMode,
                            frontFace,     depthBiasEnable,  depthClampEnable};

    const uint32_t materialVersion = material.GetVersion();
    const uint32_t materialIndex = material.GetGpuSceneMaterialIndex();
    const uint64_t epoch = m_CacheEpoch.load(std::memory_order_acquire);

    // A2.4-D6: L1-first, then shared_lock find (see the color path).
    thread_local std::array<InstancedL1Slot, kVariantL1Slots> l1{};
    auto& slot = l1[VariantL1Index(InstancedVariantKeyHash{}(key))];
    if (slot.Epoch == epoch && slot.Entry != nullptr
        && slot.Key == key
        && slot.Entry->Current.PipelineId.IsValid()
        && slot.Entry->Current.MaterialVersion == materialVersion)
    {
        return ResolveWarmPipeline(material, *slot.Entry, materialIndex, ServedPassClass::Depth, ctx);
    }
    // Stale-serve, exactly as the color path: keep the previous depth pipeline
    // on screen while the edited material's replacement builds.
    const InstancedVariantEntry* staleEntry = nullptr;
    {
        std::shared_lock lock(m_CacheMutex);
        if (auto it = cache.find(key);
            it != cache.end() && it->second.Current.PipelineId.IsValid())
        {
            if (it->second.Current.MaterialVersion == materialVersion)
            {
                slot = {key, &it->second, epoch};
                return ResolveWarmPipeline(material, it->second, materialIndex, ServedPassClass::Depth, ctx);
            }
            staleEntry = &it->second;
        }
    }

    // Publish-gate (record worker): skip the cold compile, enqueue an async depth
    // variant compile (deduped), and skip the draw — the caller falls back to the
    // shared-depth pipeline or skips, and the per-material depth variant self-heals
    // once it lands. Read-only readiness check — populate runs serially in
    // MaterialSystem::BeginFrame (A2.4-P0-R); see the color path.
    if (!m_Materials->IsMaterialBuildContextReady())
        return staleEntry != nullptr
            ? ResolveWarmPipeline(material, *staleEntry, materialIndex, ServedPassClass::Depth, ctx)
            : std::pair<PipelineHandle, const VariantServeUnit*>{{}, nullptr};
    EnqueueVariantCompile(key, /*isDepth=*/true);
    if (staleEntry != nullptr)
        return ResolveWarmPipeline(material, *staleEntry, materialIndex, ServedPassClass::Depth, ctx);
    return {{}, nullptr};
}

std::pair<Rendering::PipelineHandle, const PipelineVariantCache::VariantServeUnit*>
PipelineVariantCache::GetOrCompileDepthInstancedVariant(
    const Material& material,
    Rendering::VertexAttributeFlags vertexFlags,
    Rendering::PrimitiveTopology topology,
    Rendering::MaterialKeyword passKeywords,
    uint32_t samples,
    bool depthBiasEnable,
    bool depthClampEnable,
    Rendering::FrontFace frontFace,
    const Rendering::RenderGraph::RGContext& ctx)
{
    return GetOrCompileDepthInstancedVariantImpl(material, vertexFlags, topology, passKeywords,
                                                 samples, depthBiasEnable, depthClampEnable,
                                                 frontFace, ctx);
}

const PipelineVariantCache::VariantServeUnit* PipelineVariantCache::FindOrRequestPrepassVariant(
    const Material& material,
    Rendering::VertexAttributeFlags vertexFlags,
    Rendering::PrimitiveTopology topology,
    Rendering::MaterialKeyword passKeywords,
    Rendering::FrontFace frontFace)
{
    // The key the camera prepass's own heads use: samples 0 (inherit the pass), no bias, no clamp.
    const InstancedVariantKey key{&material,
                                  vertexFlags,
                                  0u,
                                  passKeywords,
                                  topology,
                                  material.IsDoubleSided() ? Rendering::CullModeFlagBits::None
                                                           : m_Materials->DebugCullMode(),
                                  frontFace};
    {
        std::shared_lock lock(m_CacheMutex);
        if (auto it = m_DepthInstancedVariantCache.find(key);
            it != m_DepthInstancedVariantCache.end() && it->second.Current.PipelineId.IsValid() &&
            it->second.Current.MaterialVersion == material.GetVersion())
            return &it->second.Current;
    }
    if (m_Materials->IsMaterialBuildContextReady())
        EnqueueVariantCompile(key, /*isDepth=*/true);
    return nullptr;
}

bool PipelineVariantCache::EnsureConcreteWarm(const Material& material,
                                              Rendering::GraphicsPipelineId id,
                                              const Rendering::PipelineFormatKey& fk)
{
    if (!id.IsValid() || fk.DepthFormat == Rendering::TextureFormat{})
        return false;
    const Rendering::IDevice* device = m_Materials->GetDevice();
    if (device->TryGetWarmGraphicsPipeline(id, fk).IsValid())
        return true;
    // Builds on a prewarm worker (inline with no job system), once: a build that produced nothing is
    // not requested again, so a pipeline the device cannot build stays cold.
    RequestConcreteWarm(material, id, fk);
    return device->TryGetWarmGraphicsPipeline(id, fk).IsValid();
}

void PipelineVariantCache::EnqueueEviction(const Material* mat)
{
    std::lock_guard lock(m_PendingVariantCacheEvictionsMutex);
    m_PendingVariantCacheEvictions.push_back(mat);
}

size_t PipelineVariantCache::PendingEvictionCount() const
{
    std::lock_guard lock(m_PendingVariantCacheEvictionsMutex);
    return m_PendingVariantCacheEvictions.size();
}

void PipelineVariantCache::DrainPendingEvictions()
{
    // Render-thread-only. Move the pending list under the mutex into a local
    // so the four cache walks happen without holding the cross-thread lock.
    std::vector<const Material*> evictions;
    {
        std::lock_guard lock(m_PendingVariantCacheEvictionsMutex);
        evictions.swap(m_PendingVariantCacheEvictions);
    }
    if (evictions.empty())
        return;

    // Walk each cache once and drop every row whose key.material is in the
    // pending set. Cache sizes are bounded by the interactive scene's
    // material count, so the O(cache * pending) scan is cheap relative to
    // typical eviction frequency (asset reload / GC, not per-draw).
    const auto eraseInstancedKey = [&evictions](auto& cache) {
        for (auto it = cache.begin(); it != cache.end(); )
        {
            const bool drop = std::find(evictions.begin(), evictions.end(), it->first.material)
                              != evictions.end();
            it = drop ? cache.erase(it) : std::next(it);
        }
    };
    // Erase under the exclusive lock (the maps are shared with A2.4 record
    // workers) and move to a fresh epoch so per-thread L1 slots that cached
    // pointers into the erased nodes are retired on their next probe. Runs
    // serially at frame start, before the record window opens.
    {
        std::unique_lock lock(m_CacheMutex);
        eraseInstancedKey(m_InstancedVariantCache);
        eraseInstancedKey(m_DepthInstancedVariantCache);
        m_CacheEpoch.store(NextCacheEpoch(), std::memory_order_release);
    }
}

void PipelineVariantCache::EnqueueVariantCompile(const InstancedVariantKey& key, bool isDepth)
{
    // Record-worker cold-miss handoff. Dedupe against the in-flight set: a key
    // stays in its set from here until its compile completes, so a variant that is
    // still compiling (or already queued this frame) is requested exactly once.
    std::lock_guard lock(m_PendingVariantMutex);
    auto& inFlight = isDepth ? m_DepthCompilesInFlight : m_ColorCompilesInFlight;
    if (!inFlight.insert(key).second)
        return;
    m_PendingVariantCompiles.push_back({key, isDepth});
}

size_t PipelineVariantCache::PendingVariantCompileCount() const
{
    std::lock_guard lock(m_PendingVariantMutex);
    return m_PendingVariantCompiles.size();
}

bool PipelineVariantCache::HasPipelineBuildsInFlight(const Material& material) const
{
    // A key stays in its in-flight set from the miss until the serial apply
    // has inserted its cache entry (see EnqueueVariantCompile).
    const auto ofMaterial = [&material](const InstancedVariantKey& key)
    { return key.material == &material; };
    {
        std::lock_guard lock(m_PendingVariantMutex);
        if (std::any_of(m_ColorCompilesInFlight.begin(), m_ColorCompilesInFlight.end(), ofMaterial) ||
            std::any_of(m_DepthCompilesInFlight.begin(), m_DepthCompilesInFlight.end(), ofMaterial))
            return true;
    }
    std::vector<std::pair<Rendering::GraphicsPipelineId, Rendering::PipelineFormatKey>> awaited;
    {
        std::lock_guard lock(m_ConcreteWarmMutex);
        for (const auto& [warmKey, wait] : m_ConcreteWarmWaits)
            if (std::find(wait.Materials.begin(), wait.Materials.end(), &material) != wait.Materials.end())
                awaited.emplace_back(wait.Id, wait.FormatKey);
    }
    // Asked outside the lock, as a request: a build the device dropped unrun is
    // queued again, since the material still waits on it.
    Rendering::IDevice* device = m_Materials->GetDevice();
    return std::any_of(awaited.begin(), awaited.end(),
                       [device](const auto& pipeline)
                       {
                           return device->RequestGraphicsPipeline(pipeline.first, pipeline.second) ==
                                  Rendering::PipelineBuildState::Pending;
                       });
}

void PipelineVariantCache::ReleaseVariantCompile(const InstancedVariantKey& key, bool isDepth)
{
    {
        std::lock_guard lock(m_PendingVariantMutex);
        (isDepth ? m_DepthCompilesInFlight : m_ColorCompilesInFlight).erase(key);
    }
    m_VariantCompilesRunning.fetch_sub(1, std::memory_order_acq_rel);
}

void PipelineVariantCache::PurgePendingVariantCompiles(const Material* mat)
{
    std::lock_guard lock(m_PendingVariantMutex);
    std::erase_if(m_PendingVariantCompiles,
                  [mat](const PendingVariantCompile& p) { return p.Key.material == mat; });
    std::erase_if(m_PendingVariantPublishes,
                  [mat](const PendingVariantPublish& p) { return p.Key.material == mat; });
    std::erase_if(m_ColorCompilesInFlight,
                  [mat](const InstancedVariantKey& k) { return k.material == mat; });
    std::erase_if(m_DepthCompilesInFlight,
                  [mat](const InstancedVariantKey& k) { return k.material == mat; });
    // The backend build itself continues: it is keyed by pipeline, and other
    // materials may wait on it.
    std::lock_guard warmLock(m_ConcreteWarmMutex);
    for (auto& [warmKey, wait] : m_ConcreteWarmWaits)
        std::erase(wait.Materials, mat);
}

void PipelineVariantCache::SubmitPendingVariantCompiles()
{
    std::vector<PendingVariantCompile> pending;
    {
        std::lock_guard lock(m_PendingVariantMutex);
        pending.swap(m_PendingVariantCompiles);
    }
    if (pending.empty())
        return;

    // Reconstruct compile inputs from the (process-lifetime) Material on this serial
    // thread. If the build context isn't ready the compile can't run: requeue (the
    // in-flight markers stay set, so a record re-miss won't duplicate) and retry
    // next frame once BeginFrame's EnsureMaterialBuildContextReady lands.
    if (!m_Materials->IsMaterialBuildContextReady())
    {
        std::lock_guard lock(m_PendingVariantMutex);
        m_PendingVariantCompiles.insert(m_PendingVariantCompiles.end(),
                                        pending.begin(), pending.end());
        return;
    }

    // Snapshot the build context ONCE on this serial thread. Passing the live
    // m_MaterialBuildContext into an off-thread GetOrCompile would race
    // EnsureMaterialBuildContextReady, which reassigns its vectors every BeginFrame
    // during source-set churn (a torn read / UAF exactly during cold load).
    const Rendering::MaterialBuildContext buildContext = m_Materials->m_MaterialBuildContext;

    // Color-before-depth: the visible color variant warms first; the depth (shadow/
    // prepass) variant can trail a frame. stable_partition keeps intra-class order.
    std::stable_partition(pending.begin(), pending.end(),
                          [](const PendingVariantCompile& p) { return !p.IsDepth; });

    // Off-thread when the engine job system is up (the compiles run on prewarm
    // workers on the Background lane, tracked by the JobCounter the editor pill
    // reads). Inline when it's down (headless/tests) so the material is never dark;
    // inline is serial, so no cap applies.
    const bool async = m_Materials->CanSubmitAsyncBaseCompile();
    const uint32_t cap = async
        ? static_cast<uint32_t>(std::max<size_t>(
              1, GameEngine::EngineCore::GetInstance().GetJobSystem().GetWorkerCount() / 2))
        : 0xFFFFFFFFu;

    const auto requeueRemainder = [&](size_t from) {
        if (from >= pending.size())
            return;
        std::lock_guard lock(m_PendingVariantMutex);
        m_PendingVariantCompiles.insert(m_PendingVariantCompiles.end(),
                                        pending.begin() + static_cast<std::ptrdiff_t>(from),
                                        pending.end());
    };

    size_t i = 0;
    try
    {
        for (; i < pending.size(); ++i)
        {
            const auto& req = pending[i];
            const Material* material = req.Key.material;
            // Process-lifetime dependency: production never frees a material mid-
            // session (Unregister is test-only + main-thread; the PreUnregisterCallback
            // purges this queue before any free via PurgePendingVariantCompiles). A
            // future off-main free would UAF this deref — the purge is the defense.
            assert(material != nullptr && "variant compile request for a null material");
            const Rendering::GraphicsPipelineId baseId = material->GetGraphicsPipelineId();
            // Color variants copy the base template; without a valid base (evicted /
            // recompiled since the miss) drop + clear the marker so a later miss
            // re-requests. Depth variants don't reference the base template.
            if (!req.IsDepth && !baseId.IsValid())
            {
                std::lock_guard lock(m_PendingVariantMutex);
                m_ColorCompilesInFlight.erase(req.Key);
                continue;
            }
            // Throttle: stop submitting once the cap is already in flight; the rest
            // resume next BeginFrame (markers stay set — no record re-enqueue churn).
            if (m_VariantCompilesRunning.load(std::memory_order_acquire) >= cap)
                break;

            VariantCompileRequest job{};
            job.Key = req.Key;
            job.IsDepth = req.IsDepth;
            job.MaterialVersion = material->GetVersion();
            job.Name = material->GetName();
            job.Spec = material->GetCompileSpec();
            job.BaseGraphicsPipelineId = baseId;
            job.BuildContext = buildContext;
            // Reconstruct the SPIR-V cache key exactly as ShaderCompilationCache::
            // GetOrCompileVariant would (color: entity keywords; depth: GE_INSTANCED),
            // ORed into the material keywords, so the worker's GetOrCompile hits/
            // produces the same entry the record path would have.
            Rendering::ShaderVariantKey mergedKey = material->GetVariantKey();
            mergedKey.materialKeywords |= req.Key.PassKeywords;
            if (req.Key.VertexFlags != Rendering::VertexAttributeFlags::None)
                mergedKey.vertexFlags = req.Key.VertexFlags;
            if (job.Spec.customVertexShader)
                Rendering::ApplyCustomVertexShaderClamp(mergedKey);
            job.CacheKey.VariantKey = mergedKey;
            job.CacheKey.Source.SurfaceShaderPath = job.Spec.surfaceShaderPath;
            job.CacheKey.Source.VertexModifierPath = job.Spec.vertexModifierPath;
            job.CacheKey.Source.MaterialAssetPath = material->GetMaterialAssetPath();

            // Count the compile as running BEFORE dispatch so the throttle sees it;
            // every Run* exit decrements via the RAII guard (ReleaseVariantCompile).
            m_VariantCompilesRunning.fetch_add(1, std::memory_order_acq_rel);
            if (async)
            {
                try
                {
                    m_Materials->SubmitTrackedPrewarm(
                        [this, job]() mutable
                        {
                            if (job.IsDepth)
                                RunDepthVariantCompile(job);
                            else
                                RunColorVariantCompile(job);
                        },
                        JobSystem::JobPriority::Background);
                }
                catch (...)
                {
                    // Submit failed: the worker never runs, so release here.
                    ReleaseVariantCompile(req.Key, req.IsDepth);
                    throw;
                }
            }
            else if (job.IsDepth)
            {
                RunDepthVariantCompile(job);
            }
            else
            {
                RunColorVariantCompile(job);
            }
        }
    }
    catch (...)
    {
        // An inline Run* (headless) threw: its own guard cleared its marker + running
        // count. Requeue the UNPROCESSED remainder so their markers aren't wedged.
        requeueRemainder(i + 1);
        throw;
    }

    // Throttle remainder (broke at i): requeue [i, end) with markers intact.
    requeueRemainder(i);
}

void PipelineVariantCache::InsertVariantEntry(
    VariantCacheMap& cache, const InstancedVariantKey& key,
    Rendering::GraphicsPipelineId pipelineId, uint32_t materialVersion,
    std::vector<Rendering::DescriptorSetLayoutId> setLayouts,
    std::shared_ptr<Rendering::ShaderMeta> meta)
{
    std::unique_lock lock(m_CacheMutex);
#if GE_DEBUG_INSTRUMENTATION
    m_InsertLockHeldDebug = true;
#endif
    auto it = cache.find(key);
    if (it != cache.end()
        && it->second.Current.PipelineId.IsValid()
        && it->second.Current.MaterialVersion == materialVersion)
    {
        // Another publish already landed this key (device dedup gave both the
        // same id) — keep the existing node.
    }
    else
    {
        AssertInsertLockHeld();
        auto [it2, inserted] = cache.try_emplace(key);
        auto& cached = it2->second;
        // Republish (hot reload): keep the replaced unit whole. Its concrete
        // pipelines are already built for the passes that drew it, so the record
        // thread serves {pipeline, meta, layouts} from it — coherently — while a
        // worker builds the new pipeline's backend state.
        const bool republish = cached.Current.PipelineId.IsValid()
            && cached.Current.PipelineId.Value != pipelineId.Value;
        if (republish)
            cached.Previous = std::move(cached.Current);
        cached.Current.PipelineId  = pipelineId;
        cached.Current.SetLayouts  = std::move(setLayouts);
        cached.Current.VariantMeta = std::move(meta);
        cached.Current.MaterialVersion = materialVersion;
        // Retire L1 slots stamped against the previous publication so their next
        // probe re-finds the node under the lock. This runs in the serial window
        // (phase-separated from record), so the bump is slot hygiene, not a
        // data-race guard. A NEW key needs no bump — nothing can hold a slot for
        // it — which keeps the L1 intact through a cold scene load.
        if (republish)
            m_CacheEpoch.store(NextCacheEpoch(), std::memory_order_release);
    }
#if GE_DEBUG_INSTRUMENTATION
    m_InsertLockHeldDebug = false;
#endif
}

void PipelineVariantCache::QueueVariantPublish(PendingVariantPublish&& publish)
{
    std::lock_guard lock(m_PendingVariantMutex);
    m_PendingVariantPublishes.push_back(std::move(publish));
}

void PipelineVariantCache::ApplyPendingVariantPublishes()
{
    std::vector<PendingVariantPublish> pending;
    {
        std::lock_guard lock(m_PendingVariantMutex);
        if (m_PendingVariantPublishes.empty())
            return;
        pending.swap(m_PendingVariantPublishes);
    }
    for (auto& rec : pending)
    {
        // Look up by GUID, never the captured Material* in the key. A compile
        // still running on a worker when its material unregisters queues this
        // publish AFTER the pre-unregister purge, and this apply runs AFTER the
        // eviction drain — inserting would resurrect a row keyed on the freed
        // address (unreachable leak; wrong-pipeline stale-serve if the address
        // is reused). Drop the record whole: the purge already cleared the
        // key's in-flight marker, so any marker present now belongs to a newer
        // compile at a reused address and must not be erased, and the material
        // is gone, so no VariantPublished fires.
        if (!m_Materials->Registry().Find(rec.MaterialGuid))
            continue;
        InsertVariantEntry(rec.IsDepth ? m_DepthInstancedVariantCache : m_InstancedVariantCache,
                           rec.Key, rec.PipelineId, rec.MaterialVersion,
                           std::move(rec.SetLayouts), std::move(rec.VariantMeta));
        // Marker erase strictly AFTER the insert: a record worker that misses the
        // cache must always find the marker set, or it would re-enqueue a variant
        // that is already compiled.
        {
            std::lock_guard lock(m_PendingVariantMutex);
            (rec.IsDepth ? m_DepthCompilesInFlight : m_ColorCompilesInFlight).erase(rec.Key);
        }
        FireVariantPublished(rec.MaterialGuid);
    }
}

void PipelineVariantCache::RunColorVariantCompile(const VariantCompileRequest& req)
{
    // RAII: decrements the running counter on EVERY exit, and clears the
    // in-flight marker on every FAILURE exit (early return, or a throw
    // mid-compile — bad_alloc under a 10-30s shaderc run). A wedged marker would
    // dedup-block the key from ever re-enqueueing — permanently dark draws. On
    // the success path the marker is handed to the serial apply instead, which
    // erases it AFTER inserting the cache entry, so a record worker never
    // observes a cache miss with no marker (which would re-enqueue a compiled
    // variant).
    struct ReleaseScope
    {
        PipelineVariantCache* Self;
        const VariantCompileRequest* Req;
        bool PublishQueued = false;
        ~ReleaseScope()
        {
            if (PublishQueued)
                Self->m_VariantCompilesRunning.fetch_sub(1, std::memory_order_acq_rel);
            else
                Self->ReleaseVariantCompile(Req->Key, Req->IsDepth);
        }
    } releaseScope{this, &req};

    const auto tCompile0 = std::chrono::high_resolution_clock::now();
    auto variant = m_Materials->m_ShaderCompilationCache.GetOrCompile(
        req.CacheKey, req.Spec, req.BuildContext, req.Name,
        m_Materials->GetDevice()->PreferredShaderSource());
    if (!variant || variant->vertexBytes.empty())
        return; // transient (context cold) or genuine failure — the scope re-opens the key

    Rendering::GraphicsPipelineDesc gd{};
    gd.Kind = Rendering::GraphicsPipelineKind::VertexFragment;
    gd.VertexShader = std::shared_ptr<const std::vector<uint8_t>>(variant, &variant->vertexBytes);
    gd.PixelShader  = std::shared_ptr<const std::vector<uint8_t>>(variant, &variant->fragmentBytes);

    Rendering::BuildVertexLayoutFromFlags(req.Key.VertexFlags, gd);

    // Copy the base template's raster/depth/blend by value off the device cache
    // (thread-safe). By value, NOT the Lookup pointer: a concurrent intern can
    // reallocate the device's entry vector and dangle it. The copied blend array
    // is sized to the BASE variant's outputs, which say nothing about this
    // variant's — ApplyShaderMetaToGraphicsDesc below grows it to match the
    // keyword-enabled MRT outputs this fragment shader actually writes.
    Rendering::GraphicsPipelineDesc base{};
    if (m_Materials->GetDevice()->CopyGraphicsPipeline(req.BaseGraphicsPipelineId, base))
    {
        gd.Rasterization = base.Rasterization;
        gd.DepthStencil  = base.DepthStencil;
        gd.ColorBlend    = base.ColorBlend;
    }
    gd.DepthStencil.depthWriteEnable =
        ColorVariantWritesDepth(gd.DepthStencil.depthWriteEnable, req.Key.PassKeywords);
    // Front face is keyed per variant (mirrored instances draw flipped).
    gd.Rasterization.frontFace = req.Key.Winding;
    gd.Topology = req.Key.Topology;
    gd.DebugName = req.Name;

    // Set 1 is the material texture set, in whichever form this device's
    // indexing mode takes.
    auto patchLayout = [this](uint32_t setIndex, Rendering::DescriptorSetLayoutDesc& dsl) {
        if (setIndex == 1)
            dsl = CreateMaterialTextureSetLayout(m_Materials->GetDevice());
    };

    std::string applyErr;
    if (!Rendering::MaterialHelper::ApplyShaderMetaToGraphicsDesc(
            *m_Materials->GetDevice(), *variant->meta, gd,
            Rendering::MaterialBuilder::MergeMode::Auto,
            {true, 128}, patchLayout, &applyErr))
    {
        Logger::Log::Warning("RunColorVariantCompile '{}': failed to apply shader meta: {}",
                             req.Name, applyErr);
        return;
    }

    // Runtime-contract PC: V|F @ 128 bytes regardless of meta reflection.
    gd.PushConstants.Size      = 128;
    gd.PushConstants.StageMask = Rendering::kShaderStageVertex | Rendering::kShaderStageFragment;

    // SSSR's adapter writes colour + octahedral-NR + albedo. A cooked lookup
    // that silently resolved a 1-output row interns a legal pipeline whose extra
    // pass targets are write-masked off — classify then rejects every pixel.
    constexpr uint32_t kSssrColorAttachmentCount = 3;
    if (Rendering::HasKeyword(req.Key.PassKeywords, Rendering::MaterialKeyword::SSSRNormalRoughness)
        && gd.ColorBlend.attachments.size() < kSssrColorAttachmentCount)
    {
        Logger::Log::Warning(
            "RunColorVariantCompile '{}': SSSR variant interned with {} colour attachments "
            "(kw={:#x}); expected {} (color + oNormalRoughness + oAlbedo)",
            req.Name, static_cast<uint32_t>(gd.ColorBlend.attachments.size()),
            static_cast<uint64_t>(req.Key.PassKeywords), kSssrColorAttachmentCount);
    }

    std::vector<Rendering::DescriptorSetLayoutId> setLayouts = gd.DescriptorSetLayouts;
    const auto pipelineId = m_Materials->GetDevice()->InternGraphicsPipeline(std::move(gd));
    if (!pipelineId.IsValid())
    {
        Logger::Log::Warning("RunColorVariantCompile '{}': failed to intern graphics pipeline",
                             req.Name);
        return;
    }

    // Hand the compiled unit to the serial frame-begin apply. The worker must
    // not write the live cache node: record threads read entry fields and hold
    // stale-serve pointers without the lock, which is safe only because every
    // node write happens in the serial window (ApplyPendingVariantPublishes),
    // phase-separated from record exactly like the eviction drain.
    QueueVariantPublish({req.Key, pipelineId, req.MaterialVersion,
                         std::move(setLayouts), variant->meta,
                         req.Key.material->GetGuid(), /*IsDepth=*/false});
    releaseScope.PublishQueued = true;

    m_AsyncVariantCompilesTotal.fetch_add(1, std::memory_order_relaxed);
    Logger::Log::Trace(
        "[VariantWarm] color '{}' vf={:#x} kw={:#x}: {:.1f}ms (background)",
        req.Name, static_cast<uint32_t>(req.Key.VertexFlags),
        static_cast<uint64_t>(req.Key.PassKeywords),
        std::chrono::duration<double, std::milli>(
            std::chrono::high_resolution_clock::now() - tCompile0).count());
}

void PipelineVariantCache::RunDepthVariantCompile(const VariantCompileRequest& req)
{
    // RAII counter release on every exit, marker release on failure exits only —
    // see RunColorVariantCompile.
    struct ReleaseScope
    {
        PipelineVariantCache* Self;
        const VariantCompileRequest* Req;
        bool PublishQueued = false;
        ~ReleaseScope()
        {
            if (PublishQueued)
                Self->m_VariantCompilesRunning.fetch_sub(1, std::memory_order_acq_rel);
            else
                Self->ReleaseVariantCompile(Req->Key, Req->IsDepth);
        }
    } releaseScope{this, &req};

    const auto tCompile0 = std::chrono::high_resolution_clock::now();
    auto variant = m_Materials->m_ShaderCompilationCache.GetOrCompile(
        req.CacheKey, req.Spec, req.BuildContext, req.Name,
        m_Materials->GetDevice()->PreferredShaderSource());
    if (!variant || variant->vertexBytes.empty())
        return; // transient or genuine failure — the scope re-opens the key

    const DepthFragmentMode fragmentMode =
        Rendering::HasKeyword(req.Key.PassKeywords, Rendering::MaterialKeyword::MotionVectors)
            ? DepthFragmentMode::MotionColor
        : Rendering::HasKeyword(req.Key.PassKeywords, Rendering::MaterialKeyword::DepthOnlyTransmissionColor)
            ? DepthFragmentMode::GlassTintColor
        : Rendering::HasKeyword(req.Key.PassKeywords, Rendering::MaterialKeyword::DepthOnlyFragment)
            ? DepthFragmentMode::CoverageDiscard
            : DepthFragmentMode::None;
    // Under the compatibility profile a fragment-less pipeline declares, and the binder walks, only
    // what its vertex stage reads (CompatVertexStageDepthMeta). Desktop keeps the variant's meta.
    std::shared_ptr<Rendering::ShaderMeta> compatMeta;
    if (fragmentMode == DepthFragmentMode::None && variant->meta && Rendering::IsCompatShaderProfile())
        compatMeta = std::make_shared<Rendering::ShaderMeta>(CompatVertexStageDepthMeta(*variant->meta));
    // Material params are always bindless (D1), so set 1 is always patched.
    const auto pipelineId = InternDepthPipelineId(
        variant, req.Key.VertexFlags, req.Key.Topology, req.Key.CullMode,
        req.Key.DepthBiasEnable, req.Key.DepthClampEnable, req.Key.Winding, fragmentMode,
        /*patchBindlessTextureSet=*/true, compatMeta.get());
    if (!pipelineId.IsValid())
        return;

    // Snapshot set-layout ids off the device cache by value (a concurrent intern can
    // reallocate + dangle the Lookup pointer).
    std::vector<Rendering::DescriptorSetLayoutId> setLayouts;
    Rendering::GraphicsPipelineDesc dsDesc{};
    if (m_Materials->GetDevice()->CopyGraphicsPipeline(pipelineId, dsDesc))
        setLayouts = std::move(dsDesc.DescriptorSetLayouts);

    // Hand the compiled unit to the serial apply (see RunColorVariantCompile).
    QueueVariantPublish({req.Key, pipelineId, req.MaterialVersion,
                         std::move(setLayouts), compatMeta ? std::move(compatMeta) : variant->meta,
                         req.Key.material->GetGuid(), /*IsDepth=*/true});
    releaseScope.PublishQueued = true;

    m_AsyncVariantCompilesTotal.fetch_add(1, std::memory_order_relaxed);
    Logger::Log::Trace(
        "[VariantWarm] depth '{}' vf={:#x} kw={:#x}: {:.1f}ms (background)",
        req.Name, static_cast<uint32_t>(req.Key.VertexFlags),
        static_cast<uint64_t>(req.Key.PassKeywords),
        std::chrono::duration<double, std::milli>(
            std::chrono::high_resolution_clock::now() - tCompile0).count());
}

PipelineVariantCache::InstancedVariantKey
PipelineVariantCache::EnqueueColorVariantCompileForTesting(
    const Material& material, Rendering::VertexAttributeFlags vertexFlags,
    Rendering::PrimitiveTopology topology, Rendering::MaterialKeyword passKeywords,
    uint32_t sampleCount, Rendering::FrontFace frontFace)
{
    InstancedVariantKey key{
        &material, vertexFlags, sampleCount, passKeywords, topology,
        material.IsDoubleSided() ? Rendering::CullModeFlagBits::None : m_Materials->DebugCullMode(),
        frontFace};
    EnqueueVariantCompile(key, /*isDepth=*/false);
    return key;
}

bool PipelineVariantCache::HasColorVariantForTesting(const InstancedVariantKey& key) const
{
    std::shared_lock lock(m_CacheMutex);
    auto it = m_InstancedVariantCache.find(key);
    return it != m_InstancedVariantCache.end() && it->second.Current.PipelineId.IsValid();
}

void PipelineVariantCache::EnqueueColorVariantPublishForTesting(
    const InstancedVariantKey& key, Rendering::GraphicsPipelineId pipelineId,
    uint32_t materialVersion, const GUID& materialGuid)
{
    PendingVariantPublish publish{};
    publish.Key = key;
    publish.PipelineId = pipelineId;
    publish.MaterialVersion = materialVersion;
    publish.MaterialGuid = materialGuid;
    publish.IsDepth = false;
    QueueVariantPublish(std::move(publish));
}

PipelineVariantCache::VariantEntrySnapshotForTesting
PipelineVariantCache::SnapshotColorVariantForTesting(const InstancedVariantKey& key) const
{
    std::shared_lock lock(m_CacheMutex);
    auto it = m_InstancedVariantCache.find(key);
    if (it == m_InstancedVariantCache.end())
        return {};
    return {it->second.Current.PipelineId, it->second.Previous.PipelineId,
            it->second.Current.MaterialVersion, it->second.Previous.MaterialVersion};
}

PipelineVariantCache::InstancedVariantKey
PipelineVariantCache::EnqueueDepthVariantCompileForTesting(
    const Material& material, Rendering::VertexAttributeFlags vertexFlags,
    Rendering::PrimitiveTopology topology, Rendering::MaterialKeyword passKeywords,
    uint32_t sampleCount, bool depthBiasEnable, bool depthClampEnable,
    Rendering::FrontFace frontFace)
{
    InstancedVariantKey key{
        &material, vertexFlags, sampleCount, passKeywords, topology,
        material.IsDoubleSided() ? Rendering::CullModeFlagBits::None : m_Materials->DebugCullMode(),
        frontFace, depthBiasEnable, depthClampEnable};
    EnqueueVariantCompile(key, /*isDepth=*/true);
    return key;
}

bool PipelineVariantCache::HasDepthVariantForTesting(const InstancedVariantKey& key) const
{
    std::shared_lock lock(m_CacheMutex);
    auto it = m_DepthInstancedVariantCache.find(key);
    return it != m_DepthInstancedVariantCache.end() && it->second.Current.PipelineId.IsValid();
}

void PipelineVariantCache::OnDeviceRebuilt()
{
    std::lock_guard lock(m_ConcreteWarmMutex);
    m_ConcreteWarmWaits.clear();
}

void PipelineVariantCache::AssertInsertLockHeld() const noexcept
{
#if GE_DEBUG_INSTRUMENTATION
    assert(m_InsertLockHeldDebug
           && "PipelineVariantCache cold insert without holding m_CacheMutex "
              "exclusively (A2.4-D6 requires inserts under the unique_lock).");
#endif
}

Rendering::GraphicsPipelineId PipelineVariantCache::InternDepthPipelineId(
    const std::shared_ptr<const SharedShaderVariant>& variant,
    Rendering::VertexAttributeFlags vertexFlags,
    Rendering::PrimitiveTopology topology,
    Rendering::CullModeFlags cullMode,
    bool depthBiasEnable,
    bool depthClampEnable,
    Rendering::FrontFace frontFace,
    DepthFragmentMode fragmentMode,
    bool patchBindlessTextureSet,
    const Rendering::ShaderMeta* declaredMeta)
{
    // Every fragment mode needs the composed fragment SPIR-V; the two colour
    // modes add an attachment and turn depth-write off.
    const bool includeFragment = fragmentMode != DepthFragmentMode::None;
    const bool colorTint       = fragmentMode == DepthFragmentMode::GlassTintColor;
    const bool motionColor     = fragmentMode == DepthFragmentMode::MotionColor;

    Rendering::GraphicsPipelineDesc gd{};
    gd.Kind = Rendering::GraphicsPipelineKind::VertexFragment;
    gd.VertexShader = std::shared_ptr<const std::vector<uint8_t>>(variant, &variant->vertexBytes);
    if (includeFragment && variant && !variant->fragmentBytes.empty())
        gd.PixelShader = std::shared_ptr<const std::vector<uint8_t>>(variant, &variant->fragmentBytes);

    Rendering::BuildVertexLayoutFromFlags(vertexFlags, gd);

    // Depth/shadow shaders consume only a subset of the mesh's attributes
    // (position; +joints/weights when skinned; +uv0 for alpha-tested depth).
    // BuildVertexLayoutFromFlags declares the full mesh layout, so the unread
    // attributes trip "vertex attribute at location N not consumed by vertex
    // shader" validation warnings. Drop attributes the depth shader doesn't read,
    // keyed off its reflected vertex inputs. Bindings/strides are left intact so
    // the consumed (interleaved) attributes still read at the correct offsets;
    // this is rendering-neutral (the GPU already ignored the unread attributes).
    if (variant && variant->meta)
    {
        if (auto vsIt = variant->meta->Stages.find("vs"); vsIt != variant->meta->Stages.end())
        {
            std::unordered_set<uint32_t> consumed;
            for (const auto& in : vsIt->second.Inputs)
                consumed.insert(in.Location);
            if (!consumed.empty())
            {
                auto& attrs = gd.VertexAttributes;
                attrs.erase(std::remove_if(attrs.begin(), attrs.end(),
                                [&consumed](const Rendering::VertexInputAttribute& a)
                                { return consumed.find(a.location) == consumed.end(); }),
                            attrs.end());
            }
        }
    }

    gd.Topology = topology;

    auto internDepthLayout = [this, patchBindlessTextureSet](
                                 uint32_t setIndex,
                                 const Rendering::DescriptorSetLayoutDesc& layout)
    {
        Rendering::DescriptorSetLayoutDesc patched = layout;
        if (patchBindlessTextureSet && setIndex == 1)
            patched = CreateMaterialTextureSetLayout(m_Materials->GetDevice());
        return m_Materials->GetDevice()->InternDescriptorSetLayout(patched);
    };

    // The layouts come from the meta the pipeline declares: the variant's own, or the narrower one the
    // caller publishes for the binder to walk (declaredMeta), so the sets the binder builds match them.
    const Rendering::ShaderMeta* meta = declaredMeta ? declaredMeta : variant->meta.get();
    std::vector<Rendering::DescriptorSetLayoutDesc> declaredLayouts;
    if (declaredMeta)
        declaredLayouts = Rendering::MaterialBuilder::BuildSetLayouts(*declaredMeta);
    const std::vector<Rendering::DescriptorSetLayoutDesc>& setLayouts =
        declaredMeta ? declaredLayouts : variant->setLayouts;

    // A fragment stage brings every reflected set; a vertex-only pipeline declares set 0, or the sets up to the
    // highest one its vertex stage reads.
    const uint32_t vertexSetCount = meta ? DepthOnlyPipelineSetCount(*meta) : 0u;
    if (!setLayouts.empty() && (includeFragment || vertexSetCount > 1u))
    {
        std::vector<uint32_t> setIndices;
        if (meta)
        {
            setIndices.reserve(meta->Sets.size());
            for (const auto& setMeta : meta->Sets)
                setIndices.push_back(setMeta.Set);
            std::sort(setIndices.begin(), setIndices.end());
        }

        for (size_t i = 0; i < setLayouts.size(); ++i)
        {
            const uint32_t setIndex = (i < setIndices.size())
                ? setIndices[i]
                : static_cast<uint32_t>(i);
            if (!includeFragment && setIndex >= vertexSetCount)
                break;
            gd.DescriptorSetLayouts.push_back(
                internDepthLayout(setIndex, setLayouts[i]));
        }
    }
    else if (!setLayouts.empty())
    {
        gd.DescriptorSetLayouts.push_back(
            m_Materials->GetDevice()->InternDescriptorSetLayout(setLayouts[0]));
    }

    gd.Rasterization.cullMode  = cullMode;
    // Keyed per pipeline (mirrored casters flip winding); no longer the global
    // DebugFrontFace() injection.
    gd.Rasterization.frontFace = frontFace;
    gd.Rasterization.depthBiasEnable = depthBiasEnable;
    if (depthBiasEnable)
    {
        gd.Rasterization.depthBiasConstantFactor = kShadowDepthBiasConstant;
        gd.Rasterization.depthBiasSlopeFactor    = kShadowDepthBiasSlope;
    }
    // Pancaking (directional cascades only). Casters nearer the light than the
    // shadow camera's near plane rasterize clamped to the near depth instead of
    // being clipped away, so the near plane stops deciding which occluders
    // exist. Needs the Vulkan `depthClamp` device feature, requested in
    // VulkanDevice's main and minimal paths.
    gd.Rasterization.depthClampEnable = depthClampEnable;
    gd.DepthStencil.depthTestEnable  = true;
    // Tint pass tests against the opaque cascade read-only; writing would let stacked
    // glass occlude itself in the depth array. The motion producer tests against the
    // camera depth the prepass already completed — writing there would move the
    // depth every other consumer of this frame reconstructs against.
    gd.DepthStencil.depthWriteEnable = !colorTint && !motionColor;
    gd.DepthStencil.depthCompareOp   = Rendering::CompareOp::GreaterOrEqual;

    if (colorTint)
    {
        // RGB = the Beer-Lambert transmittance product (dst' = dst*src, each glass layer
        // compounds, order-independent; cleared to white so empty texels stay 1.0). A = the
        // glass presence/focus mask for the caustic pass: the fragment writes the C1.5 refraction
        // convergence normalized to [0.5,1.0] (0.5 = flat/diverging baseline, 1.0 = max convergence;
        // cleared a=0 = no glass, which gates the receiver caustic). MAX-blend keeps the
        // strongest-converging layer where glass stacks, order-independently.
        Rendering::ColorBlendAttachmentState blend{};
        blend.blendEnable        = true;
        blend.colorBlendOp       = Rendering::BlendOp::Add;
        blend.srcColorBlendFactor = Rendering::BlendFactor::Zero;     // 0*src
        blend.dstColorBlendFactor = Rendering::BlendFactor::SrcColor; // + dst*src = dst*src
        blend.alphaBlendOp       = Rendering::BlendOp::Max;
        blend.srcAlphaBlendFactor = Rendering::BlendFactor::One;      // max(srcA*1, dstA*1) = true running max
        blend.dstAlphaBlendFactor = Rendering::BlendFactor::One;
        blend.colorWriteMask     = 0xF; // RGB tint + A presence/focus
        gd.ColorBlend.attachments.push_back(blend);
    }
    else if (motionColor)
    {
        // One unblended attachment. The payload is a value describing one
        // surface — rg the viewport-UV delta, b the previous depth, a the
        // validity flag — and blending two surfaces' payloads would produce a
        // vector describing neither.
        Rendering::ColorBlendAttachmentState blend{};
        blend.blendEnable    = false;
        blend.colorWriteMask = 0xF;
        gd.ColorBlend.attachments.push_back(blend);
    }

    gd.PushConstants.Size      = 128;
    gd.PushConstants.StageMask = Rendering::kShaderStageVertex
        | (includeFragment ? Rendering::kShaderStageFragment : 0u);

    return m_Materials->GetDevice()->InternGraphicsPipeline(std::move(gd));
}

bool PipelineVariantCache::EnsureSharedDepthShaderLoaded(bool isSkinned)
{
    auto& entry = m_SharedDepthVariants[isSkinned ? 1 : 0];
    if (entry.Variant)
        return true;
    if (entry.LoadAttempted)
        return false;
    entry.LoadAttempted = true;

    const char* spvName = isSkinned
        ? "shadow_depth_shared_skinned.vert.spv"
        : "shadow_depth_shared.vert.spv";
    // ResolveShaderPath routes through the AssetManager's "editor" mount, which
    // is rooted at the install assets root. The shader lives under Shaders/.
    const std::string assetPath = std::string("Shaders/") + spvName;

    std::vector<uint8_t> spv;
    std::string resolved = Rendering::Utils::ResolveShaderPath(assetPath.c_str());
    if (!resolved.empty())
        spv = Rendering::Utils::ReadFile(resolved);
    if (spv.empty())
        spv = Rendering::Utils::ReadFile((PathUtils::GetInstallAssetsRoot() / assetPath).string());
    if (spv.empty())
    {
        Logger::Log::Warning(
            "SharedDepth: {} not found — depth path falls back to per-material variants",
            assetPath);
        return false;
    }
    if ((spv.size() & 0x3u) != 0u)
    {
        Logger::Log::Warning("SharedDepth: {} not 4-byte aligned (size={})",
                             spvName, spv.size());
        return false;
    }

    // Reflect → meta + BuildSetLayouts: exactly mirrors what
    // ShaderCompilationCache::GetOrCompile does for per-material variants
    // (line 67-68 of ShaderCompilationCache.cpp). The resulting variant is
    // bit-compatible with what InternDepthPipelineId expects.
    Rendering::StageReflectionResult stage{};
    Rendering::ReflectionOptions opts{};
    std::string err;
    if (!Rendering::ReflectSpirv(Rendering::ShaderStageKind::Vertex,
                                 reinterpret_cast<const uint32_t*>(spv.data()),
                                 spv.size() / 4u, opts, stage, &err))
    {
        Logger::Log::Warning("SharedDepth: ReflectSpirv failed for {}: {}", spvName, err);
        return false;
    }

    auto variant = std::make_shared<SharedShaderVariant>();
    variant->vertexBytes = std::move(spv);
    // No fragmentBytes — depth-only.
    variant->meta = std::make_shared<Rendering::ShaderMeta>(
        Rendering::MergeStages({stage}));
    variant->setLayouts =
        Rendering::MaterialBuilder::BuildSetLayouts(*variant->meta);

    entry.Variant = std::move(variant);
    Logger::Log::Info(
        "SharedDepth: loaded {} ({} bytes vsSpv, {} reflected set(s))",
        spvName,
        entry.Variant->vertexBytes.size(),
        entry.Variant->meta ? entry.Variant->meta->Sets.size() : 0);
    return true;
}

Rendering::GraphicsPipelineId PipelineVariantCache::GetSharedDepthPipelineId(
    Rendering::VertexAttributeFlags vertexFlags,
    Rendering::PrimitiveTopology topology,
    Rendering::CullModeFlags cullMode,
    bool depthBiasEnable,
    bool depthClampEnable,
    Rendering::FrontFace frontFace)
{
    const SharedDepthPipelineKey key{vertexFlags,     topology,         cullMode,
                                     depthBiasEnable, depthClampEnable, frontFace};
    const uint64_t epoch = m_CacheEpoch.load(std::memory_order_acquire);

    // A2.4-D6: L1-first, then shared_lock find (see the color path). The value is
    // a small pipeline id cached by value.
    thread_local std::array<SharedDepthL1Slot, kVariantL1Slots> l1{};
    auto& slot = l1[VariantL1Index(SharedDepthPipelineKeyHash{}(key))];
    if (slot.Epoch == epoch && slot.Value.IsValid()
        && slot.Key == key)
        return slot.Value;
    {
        std::shared_lock lock(m_CacheMutex);
        if (auto it = m_SharedDepthPipelines.find(key); it != m_SharedDepthPipelines.end())
        {
            if (it->second.IsValid())
                slot = {key, it->second, epoch};
            return it->second;
        }
    }

    const bool isSkinned = Rendering::HasFlag(
        vertexFlags,
        Rendering::VertexAttributeFlags::HasJoints
        | Rendering::VertexAttributeFlags::HasWeights);

    // The shader load MUST be warm before the record window (PreloadSharedDepthShaders
    // — A2.4-D6). On the serial preload / editor-preview path it may still lazy-load
    // here; that path is single-threaded.
    if (!EnsureSharedDepthShaderLoaded(isSkinned))
        return {};
    auto& entry = m_SharedDepthVariants[isSkinned ? 1 : 0];

    // Use the same entry point per-material depth pipelines use. The synthetic
    // SharedShaderVariant carries pre-reflected meta + BuildSetLayouts-derived
    // setLayouts, so InternDepthPipelineId's layout interning is bit-identical
    // to what MaterialBuildService-produced variants get. Interning runs on the
    // thread-safe device cache WITHOUT m_CacheMutex.
    const auto pipelineId = InternDepthPipelineId(
        entry.Variant, vertexFlags, topology, cullMode, depthBiasEnable, depthClampEnable,
        frontFace, DepthFragmentMode::None, /*patchBindlessTextureSet=*/false);

    // Snapshot set-layout ids off the device cache before locking. Copy by value
    // (NOT the Lookup pointer): a concurrent InternGraphicsPipeline can reallocate
    // the entry vector and dangle the pointer (cross-worker UAF).
    std::vector<Rendering::DescriptorSetLayoutId> setLayouts;
    Rendering::GraphicsPipelineDesc dsDesc{};
    if (m_Materials->GetDevice()->CopyGraphicsPipeline(pipelineId, dsDesc))
        setLayouts = std::move(dsDesc.DescriptorSetLayouts);

    Rendering::GraphicsPipelineId result{};
    {
        std::unique_lock lock(m_CacheMutex);
#if GE_DEBUG_INSTRUMENTATION
        m_InsertLockHeldDebug = true;
#endif
        AssertInsertLockHeld();
        // try_emplace: if another worker interned + inserted the same key while we
        // compiled without the lock, keep theirs (same id via device dedup).
        auto [it, inserted] = m_SharedDepthPipelines.try_emplace(key, pipelineId);
        result = it->second;
        // Set-layout ids are combo-independent for a given skinned-ness — warm
        // once. PreloadSharedDepthShaders populates this before the window, so
        // this branch is skipped during parallel record.
        if (entry.InternedSetLayoutIds.empty() && !setLayouts.empty())
            entry.InternedSetLayoutIds = std::move(setLayouts);
        if (result.IsValid())
            slot = {key, result, epoch};
#if GE_DEBUG_INSTRUMENTATION
        m_InsertLockHeldDebug = false;
#endif
    }

    Logger::Log::Info(
        "SharedDepth: interned vertexFlags={:#x} skinned={} cullMode={:#x} depthBias={} "
        "depthClamp={} valid={}",
        static_cast<uint32_t>(vertexFlags), isSkinned, static_cast<uint32_t>(cullMode),
        depthBiasEnable, depthClampEnable, result.IsValid());
    return result;
}

void PipelineVariantCache::PreloadSharedDepthShaders()
{
    // A2.4-D6 prerequisite: warm BOTH shared-depth variants — the shader load
    // (file I/O + reflection) AND entry.InternedSetLayoutIds — serially, before
    // any parallel record window opens, so no worker triggers a lazy load or
    // mutates m_SharedDepthVariants mid-record. Idempotent: after the first call
    // EnsureSharedDepthShaderLoaded early-returns and the canonical pipeline is
    // L1/lock-cached.
    for (const bool isSkinned : {false, true})
    {
        if (!EnsureSharedDepthShaderLoaded(isSkinned))
            continue; // .spv absent — depth falls back to per-material variants
        // Intern a canonical shared-depth pipeline to populate InternedSetLayoutIds.
        // The concrete rasterizer combo is irrelevant: set layouts come from the
        // shader reflection, not the raster state, so any valid combo warms them.
        const auto vertexFlags = isSkinned
            ? (Rendering::VertexAttributeFlags::HasPosition
               | Rendering::VertexAttributeFlags::HasJoints
               | Rendering::VertexAttributeFlags::HasWeights)
            : Rendering::VertexAttributeFlags::HasPosition;
        GetSharedDepthPipelineId(vertexFlags, Rendering::PrimitiveTopology::TriangleList,
                                 Rendering::CullModeFlagBits::Back, /*depthBiasEnable=*/false,
                                 /*depthClampEnable=*/false,
                                 Rendering::FrontFace::CounterClockwise);
    }
}

const Rendering::ShaderMeta* PipelineVariantCache::GetSharedDepthShaderMeta(bool isSkinned) const
{
    const auto& entry = m_SharedDepthVariants[isSkinned ? 1 : 0];
    return entry.Variant ? entry.Variant->meta.get() : nullptr;
}

std::span<const Rendering::DescriptorSetLayoutId>
PipelineVariantCache::GetSharedDepthSetLayouts(bool isSkinned) const
{
    const auto& v = m_SharedDepthVariants[isSkinned ? 1 : 0].InternedSetLayoutIds;
    return std::span<const Rendering::DescriptorSetLayoutId>(v.data(), v.size());
}

} // namespace Engine::Renderer
} // namespace GameEngine
