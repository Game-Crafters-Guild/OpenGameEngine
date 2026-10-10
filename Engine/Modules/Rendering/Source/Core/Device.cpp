#include "Rendering/Core/Device.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/PipelineCache.h"
#include "PipelineBuildTable.h"
#include "Rendering/Core/PipelineDescTranslator.h"
#include "Rendering/Core/DeviceFormatting.h"
#include "Rendering/Core/PipelineManager.h"
#include "Rendering/Core/RendererProfile.h"
#if defined(RENDERING_HAS_VULKAN)
#include "../Vulkan/VulkanDevice.h"
#endif
#if defined(RENDERING_HAS_DIRECTX12)
#include "../DirectX12/D3D12Device.h"
#endif
#if defined(RENDERING_HAS_WEBGPU)
#include "../WebGPU/WebGpuDevice.h"
#endif
#if defined(RENDERING_HAS_METAL)
#include "../Metal/MetalDevice.h"
#endif
#include "Logger/Logger.h"
#include <cassert>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>

namespace GameEngine {
namespace Rendering {

namespace
{
std::string FormatGraphicsPipelineFailureKey(GraphicsPipelineId id,
                                             const PipelineFormatKey& fk)
{
    std::string colors;
    for (uint8_t i = 0; i < fk.ColorCount && i < PipelineFormatKey::kMaxColors; ++i)
    {
        if (!colors.empty())
            colors += ",";
        colors += ToString(fk.ColorFormats[i]);
    }
    if (colors.empty())
        colors = "none";

    return "id=" + std::to_string(id.Value) +
           " colors=" + colors +
           " depth=" + ToString(fk.DepthFormat) +
           " stencil=" + ToString(fk.StencilFormat) +
           " samples=" + std::to_string(static_cast<uint32_t>(fk.RasterizationSamples));
}
}

// Default: backend does not support secondary command lists. (Relocated from the
// deleted RenderPassContext.cpp during the RenderGraph cutover.)
std::unique_ptr<CommandList> IDevice::CreateSecondaryCommandList(QueueType /*queue*/)
{
    return nullptr;
}

    // SamplerDesc preset factories (moved from IDevice to SamplerDesc to reduce API bloat)
    SamplerDesc SamplerDesc::ShadowClampNearest(const char* name) {
        SamplerDesc d{}; d.debugName = name;
        d.minFilter = 0; d.magFilter = 0; d.mipFilter = 0;          // nearest, no mip
        d.addressModeU = 2; d.addressModeV = 2; d.addressModeW = 2; // CLAMP_TO_EDGE
        d.compareEnable = false; d.borderColor = 0;                 // opaque black (unused with clamp edge)
        return d;
    }

    SamplerDesc SamplerDesc::ShadowComparePCF(const char* name) {
        SamplerDesc d{}; d.debugName = name;
        d.minFilter = 1; d.magFilter = 1; d.mipFilter = 0;          // linear (hardware 2x2 PCF), no mip
        d.addressModeU = 3; d.addressModeV = 3; d.addressModeW = 3; // CLAMP_TO_BORDER
        d.borderColor = 0;                                         // reverse-Z far depth -> OOB considered lit
        d.compareEnable = true; d.compareOp = CompareOp::GreaterOrEqual;  // reverse-Z
        return d;
    }

    SamplerDesc SamplerDesc::MaterialLinearRepeat(const char* name) {
        SamplerDesc d{}; d.debugName = name;
        d.minFilter = 1; d.magFilter = 1; d.mipFilter = 1;          // linear + linear mip
        d.addressModeU = 0; d.addressModeV = 0; d.addressModeW = 0; // REPEAT
        return d;
    }

    SamplerDesc SamplerDesc::MaterialLinearClamp(const char* name) {
        SamplerDesc d{}; d.debugName = name;
        d.minFilter = 1; d.magFilter = 1; d.mipFilter = 1;          // linear + linear mip
        d.addressModeU = 2; d.addressModeV = 2; d.addressModeW = 2; // CLAMP_TO_EDGE
        return d;
    }

    SamplerDesc SamplerDesc::MaterialBilinearRepeat(const char* name) {
        SamplerDesc d{}; d.debugName = name;
        d.minFilter = 1; d.magFilter = 1; d.mipFilter = 0;          // linear min/mag, nearest mip (bilinear)
        d.addressModeU = 0; d.addressModeV = 0; d.addressModeW = 0; // REPEAT
        return d;
    }

    SamplerDesc SamplerDesc::PointClamp(const char* name) {
        SamplerDesc d{}; d.debugName = name;
        d.minFilter = 0; d.magFilter = 0; d.mipFilter = 0;          // nearest, no mip
        d.addressModeU = 2; d.addressModeV = 2; d.addressModeW = 2; // CLAMP_TO_EDGE
        return d;
    }

    SamplerDesc SamplerDesc::PointRepeat(const char* name) {
        SamplerDesc d{}; d.debugName = name;
        d.minFilter = 0; d.magFilter = 0; d.mipFilter = 0;          // nearest, no mip
        d.addressModeU = 0; d.addressModeV = 0; d.addressModeW = 0; // REPEAT
        return d;
    }

    SamplerDesc ResolveSamplerPreset(SamplerPreset preset, const RendererProfile& profile) {
        switch (preset)
        {
        case SamplerPreset::LinearRepeat:
        {
            SamplerDesc d = SamplerDesc::MaterialLinearRepeat("SamplerPreset.LinearRepeat");
            d.maxAnisotropy = profile.MaterialSamplerAnisotropy;
            return d;
        }
        case SamplerPreset::LinearClamp:    return SamplerDesc::MaterialLinearClamp("SamplerPreset.LinearClamp");
        case SamplerPreset::BilinearRepeat: return SamplerDesc::MaterialBilinearRepeat("SamplerPreset.BilinearRepeat");
        case SamplerPreset::PointClamp:     return SamplerDesc::PointClamp("SamplerPreset.PointClamp");
        case SamplerPreset::PointRepeat:    return SamplerDesc::PointRepeat("SamplerPreset.PointRepeat");
        case SamplerPreset::LinearClampAnisotropic:
        {
            SamplerDesc d = SamplerDesc::MaterialLinearClamp("SamplerPreset.LinearClampAnisotropic");
            d.maxAnisotropy = profile.MaterialSamplerAnisotropy;
            return d;
        }
        case SamplerPreset::kCount:         break;
        }
        assert(false && "ResolveSamplerPreset: kCount is not a preset");
        return SamplerDesc::MaterialLinearRepeat("SamplerPreset.Invalid");
    }

    // StubDevice removed - using proper Vulkan and DirectX implementations

    // DeviceFactory implementation
    std::unique_ptr<IDevice> DeviceFactory::CreateDevice(const DeviceDesc& desc) {


        // Determine which API to use
        GraphicsAPI targetAPI = desc.preferredAPI;

        // Environment override (GE_GFX_API=vulkan|metal|dx12) so tests and
        // apps can switch backends without code changes. Wins over the
        // caller's preference; unknown/unsupported values are ignored.
        if (const char* envApi = std::getenv("GE_GFX_API")) {
            std::string requested(envApi);
            std::transform(requested.begin(), requested.end(), requested.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            GraphicsAPI overrideAPI = targetAPI;
            if (requested == "vulkan") {
                overrideAPI = GraphicsAPI::Vulkan;
            } else if (requested == "metal") {
                overrideAPI = GraphicsAPI::Metal;
            } else if (requested == "dx12" || requested == "d3d12" || requested == "directx12") {
                overrideAPI = GraphicsAPI::DirectX12;
            } else if (requested == "webgpu" || requested == "wgpu") {
                overrideAPI = GraphicsAPI::WebGPU;
            }
            if (overrideAPI != targetAPI) {
                if (IsAPISupported(overrideAPI)) {
                    Logger::Log::Info("DeviceFactory: GE_GFX_API override -> {}", requested);
                    targetAPI = overrideAPI;
                } else {
                    Logger::Log::Warning("DeviceFactory: GE_GFX_API requested '{}' but that backend is not built",
                                         requested);
                }
            }
        }

        if (targetAPI == GraphicsAPI::Auto) {
            // Auto-select the best native backend: Metal on Apple (IsAPISupported
            // is false elsewhere, so this naturally gates to macOS), then Vulkan,
            // then DirectX12 as the Windows fallback. MoltenVK stays available as
            // an explicit "Vulkan" pick.
            if (IsAPISupported(GraphicsAPI::Metal)) {
                targetAPI = GraphicsAPI::Metal;
            } else if (IsAPISupported(GraphicsAPI::Vulkan)) {
                targetAPI = GraphicsAPI::Vulkan;
            } else if (IsAPISupported(GraphicsAPI::DirectX12)) {
                targetAPI = GraphicsAPI::DirectX12;
            } else if (IsAPISupported(GraphicsAPI::WebGPU)) {
                // wasm builds compile only the WebGPU backend.
                targetAPI = GraphicsAPI::WebGPU;
            } else {
                Logger::Log::Error("DeviceFactory: No supported graphics API found");
                return nullptr;
            }
        }

        // Create device based on API
        switch (targetAPI) {
            case GraphicsAPI::Vulkan:
            #if defined(RENDERING_HAS_VULKAN)
                return std::make_unique<VulkanDevice>();
            #else
                Logger::Log::Error("DeviceFactory: Vulkan backend is disabled in this build");
                return nullptr;
            #endif

            case GraphicsAPI::DirectX12:
            #if defined(RENDERING_HAS_DIRECTX12)

                return std::make_unique<D3D12Device>();
            #else
                Logger::Log::Error("DeviceFactory: DirectX 12 backend is disabled in this build");
                return nullptr;
            #endif

            case GraphicsAPI::Metal:
            #if defined(RENDERING_HAS_METAL)
                return std::make_unique<MetalDevice>();
            #else
                Logger::Log::Error("DeviceFactory: Metal backend is disabled in this build");
                return nullptr;
            #endif

            case GraphicsAPI::WebGPU:
            #if defined(RENDERING_HAS_WEBGPU)
                return std::make_unique<WebGpuDevice>();
            #else
                Logger::Log::Error("DeviceFactory: WebGPU backend is disabled in this build");
                return nullptr;
            #endif

            default:
                Logger::Log::Error("DeviceFactory: Unsupported graphics API");
                return nullptr;
        }
    }

    std::vector<GraphicsAPI> DeviceFactory::GetAvailableAPIs() {
        std::vector<GraphicsAPI> apis;

        // Report available APIs based on build flags
#if defined(RENDERING_HAS_VULKAN)
        apis.push_back(GraphicsAPI::Vulkan);
#endif

#if defined(RENDERING_HAS_DIRECTX12)
        apis.push_back(GraphicsAPI::DirectX12);
#endif

#if defined(RENDERING_HAS_METAL)
        apis.push_back(GraphicsAPI::Metal);
#endif

#if defined(RENDERING_HAS_WEBGPU)
        apis.push_back(GraphicsAPI::WebGPU);
#endif

        return apis;
    }

    bool DeviceFactory::IsAPISupported(GraphicsAPI api) {
        auto availableAPIs = GetAvailableAPIs();
        return std::find(availableAPIs.begin(), availableAPIs.end(), api) != availableAPIs.end();
    }

    // =====================================================================
    // IDevice pipeline cache implementations.
    //
    // The cache + intern tables + (id, formatKey) → handle map live on the
    // base class so every backend uses the same code. Backends implement
    // `CreateConcrete{Graphics,Compute}Pipeline` taking new desc types
    // directly; the base calls those on cache miss.
    // =====================================================================

    IDevice::IDevice()
        : m_PipelineCache(std::make_unique<PipelineCache>()),
          m_PipelineBuilds(std::make_unique<PipelineBuildTable>())
    {
    }

    IDevice::~IDevice() = default;

    void IDevice::RegisterPerDeviceCacheCleanup(const char* id, std::function<void(IDevice*)> cleanup)
    {
        if (!id || !id[0] || !cleanup)
            return;
        std::lock_guard<std::mutex> lock(m_PerDeviceCacheCleanupMutex);
        for (const auto& [existingId, cb] : m_PerDeviceCacheCleanups)
            if (existingId == id)
                return;
        m_PerDeviceCacheCleanups.emplace_back(id, std::move(cleanup));
    }

    void IDevice::InvokePerDeviceCacheCleanups()
    {
        // Copy under the lock, invoke outside it: a cleanup drops a lazy cache, and
        // the next use of that cache re-registers on this same list from inside
        // whatever thread revives it. Kept rather than swapped away — the device
        // outlives a rebuild, and the same cleanup must fire on the next death too.
        // Running builds are inside backend create calls; they end before
        // anything they use is destroyed. Queued ones are cancelled: their jobs
        // find no request and do nothing.
        m_PipelineBuilds->Quiesce();

        std::vector<std::pair<std::string, std::function<void(IDevice*)>>> cleanups;
        {
            std::lock_guard<std::mutex> lock(m_PerDeviceCacheCleanupMutex);
            cleanups = m_PerDeviceCacheCleanups;
        }
        for (auto& [id, cb] : cleanups)
            if (cb)
                cb(this);
    }

    void IDevice::RegisterDeviceRebuiltCallback(const char* id, std::function<void(IDevice*)> callback)
    {
        if (!id || !id[0] || !callback)
            return;
        std::lock_guard<std::mutex> lock(m_DeviceRebuiltCallbackMutex);
        for (const auto& [existingId, cb] : m_DeviceRebuiltCallbacks)
            if (existingId == id)
                return;
        m_DeviceRebuiltCallbacks.emplace_back(id, std::move(callback));
    }

    void IDevice::InvokeDeviceRebuiltCallbacks()
    {
        // Copy under the lock, invoke outside it: the callbacks are kept (a device
        // can be rebuilt again), and a consumer must be free to re-register or
        // touch device state without deadlocking on the registration mutex.
        std::vector<std::pair<std::string, std::function<void(IDevice*)>>> callbacks;
        {
            std::lock_guard<std::mutex> lock(m_DeviceRebuiltCallbackMutex);
            callbacks = m_DeviceRebuiltCallbacks;
        }
        for (auto& [id, cb] : callbacks)
            if (cb)
                cb(this);
    }

    GraphicsPipelineId IDevice::InternGraphicsPipeline(const GraphicsPipelineDesc& desc)
    {
        return m_PipelineCache->InternGraphicsPipeline(desc);
    }

    ComputePipelineId IDevice::InternComputePipeline(const ComputePipelineDesc& desc)
    {
        return m_PipelineCache->InternComputePipeline(desc);
    }

    DescriptorSetLayoutId IDevice::InternDescriptorSetLayout(const DescriptorSetLayoutDesc& desc)
    {
        return m_PipelineCache->InternDescriptorSetLayout(desc);
    }

    const GraphicsPipelineDesc* IDevice::LookupGraphicsPipeline(GraphicsPipelineId id) const
    {
        return m_PipelineCache->LookupGraphicsPipeline(id);
    }

    const ComputePipelineDesc* IDevice::LookupComputePipeline(ComputePipelineId id) const
    {
        return m_PipelineCache->LookupComputePipeline(id);
    }

    const DescriptorSetLayoutDesc* IDevice::LookupDescriptorSetLayout(DescriptorSetLayoutId id) const
    {
        return m_PipelineCache->LookupDescriptorSetLayout(id);
    }

    bool IDevice::CopyDescriptorSetLayout(DescriptorSetLayoutId id, DescriptorSetLayoutDesc& outDesc) const
    {
        return m_PipelineCache->TryCopyDescriptorSetLayout(id, outDesc);
    }

    bool IDevice::CopyGraphicsPipeline(GraphicsPipelineId id, GraphicsPipelineDesc& outDesc) const
    {
        return m_PipelineCache->TryCopyGraphicsPipeline(id, outDesc);
    }

    std::string IDevice::GetLastGraphicsPipelineFailure() const
    {
        std::lock_guard lock(m_GraphicsPipelineFailureMutex);
        return m_LastGraphicsPipelineFailure;
    }

    void IDevice::SetLastGraphicsPipelineFailure(std::string message) const
    {
        std::lock_guard lock(m_GraphicsPipelineFailureMutex);
        m_LastGraphicsPipelineFailure = std::move(message);
    }

    PipelineHandle IDevice::GetOrCreateGraphicsPipeline(GraphicsPipelineId id, const PipelineFormatKey& fk)
    {
        if (!id.IsValid())
        {
            SetLastGraphicsPipelineFailure("invalid graphics pipeline id");
            return {};
        }
        // Self-heal: a cached row can outlive its pipeline (a pipeline-change
        // sweep destroys the handle but the concrete cache keeps the key).
        // Serving the corpse forever turns one stale row into a permanently dead
        // pass, so the probe treats it as a miss and the build below recreates it.
        if (PipelineHandle hit = TryGetWarmGraphicsPipeline(id, fk); hit.IsValid())
            return hit;
        PipelineBuildRequest request{};
        request.Key = PipelineCache::CombineGraphicsKey(id, fk);
        request.PipelineIdValue = id.Value;
        request.FormatKey = fk;
        return BuildPipelineOnCallingThread(request);
    }

    PipelineHandle IDevice::TryGetWarmGraphicsPipeline(GraphicsPipelineId id,
                                                       const PipelineFormatKey& fk) const
    {
        if (!id.IsValid())
            return {};
        PipelineHandle hit{};
        // A cached row can outlive its pipeline, and a dead handle must read as
        // cold so the caller requests a rebuild instead of binding a corpse. The
        // probe MUST be the virtual (module-correct) one, never IsValidPipeline.
        if (m_PipelineCache->TryGetConcrete(PipelineCache::CombineGraphicsKey(id, fk), hit)
            && IsPipelineAlive(hit))
            return hit;
        return {};
    }

    PipelineHandle IDevice::GetOrCreateComputePipeline(ComputePipelineId id)
    {
        if (!id.IsValid())
            return {};
        // Same self-heal as the graphics path: never serve a dead handle.
        if (PipelineHandle hit = TryGetWarmComputePipeline(id); hit.IsValid())
            return hit;
        PipelineBuildRequest request{};
        request.Key = PipelineCache::CombineComputeKey(id);
        request.IsCompute = true;
        request.PipelineIdValue = id.Value;
        return BuildPipelineOnCallingThread(request);
    }

    PipelineHandle IDevice::TryGetWarmComputePipeline(ComputePipelineId id) const
    {
        if (!id.IsValid())
            return {};
        PipelineHandle hit{};
        if (m_PipelineCache->TryGetConcrete(PipelineCache::CombineComputeKey(id), hit) && IsPipelineAlive(hit))
            return hit;
        return {};
    }

    PipelineBuildState IDevice::RequestGraphicsPipeline(GraphicsPipelineId id, const PipelineFormatKey& fk)
    {
        if (!id.IsValid())
            return PipelineBuildState::Failed;
        if (TryGetWarmGraphicsPipeline(id, fk).IsValid())
            return PipelineBuildState::Warm;
        PipelineBuildRequest request{};
        request.Key = PipelineCache::CombineGraphicsKey(id, fk);
        request.PipelineIdValue = id.Value;
        request.FormatKey = fk;
        return RequestPipelineBuild(request);
    }

    PipelineBuildState IDevice::RequestComputePipeline(ComputePipelineId id)
    {
        if (!id.IsValid())
            return PipelineBuildState::Failed;
        if (TryGetWarmComputePipeline(id).IsValid())
            return PipelineBuildState::Warm;
        PipelineBuildRequest request{};
        request.Key = PipelineCache::CombineComputeKey(id);
        request.IsCompute = true;
        request.PipelineIdValue = id.Value;
        return RequestPipelineBuild(request);
    }

    void IDevice::SetPipelineBuildDispatcher(PipelineBuildDispatcher dispatcher)
    {
        std::shared_ptr<const PipelineBuildDispatcher> next;
        if (dispatcher)
            next = std::make_shared<const PipelineBuildDispatcher>(std::move(dispatcher));
        std::lock_guard lock(m_PipelineBuildDispatcherMutex);
        m_PipelineBuildDispatcher = std::move(next);
    }

    PipelineBuildState IDevice::RequestPipelineBuild(const PipelineBuildRequest& request)
    {
        const uint64_t epoch = m_PipelineCache->GetInvalidationEpoch();
        std::shared_ptr<const PipelineBuildDispatcher> dispatcher;
        {
            std::lock_guard lock(m_PipelineBuildDispatcherMutex);
            dispatcher = m_PipelineBuildDispatcher;
        }
        if (!dispatcher || !GetCapabilities().supportsMultithreadedResourceCreation)
        {
            // Inline, as before asynchronous requests existed; a remembered
            // failure still answers without rebuilding.
            if (m_PipelineBuilds->Find(request.Key, epoch) == PipelineBuildState::Failed)
                return PipelineBuildState::Failed;
            return BuildPipelineOnCallingThread(request).IsValid() ? PipelineBuildState::Warm
                                                                   : PipelineBuildState::Failed;
        }

        const PipelineBuildTable::RequestResult queued = m_PipelineBuilds->Request(request.Key, epoch);
        if (queued.Ticket == 0)
            return queued.State;

        PipelineBuildRequest job = request;
        job.Ticket = queued.Ticket;
        job.InvalidationEpoch = epoch;
        // Fires when the last copy of the job is destroyed. After the job ran
        // its ticket no longer names a queued request and this does nothing;
        // a job the dispatcher destroyed unrun releases the request here.
        auto releaseIfUnrun = std::shared_ptr<void>(
            nullptr, [this, key = job.Key, ticket = job.Ticket](void*) { m_PipelineBuilds->ReleaseUnrun(key, ticket); });
        // Called outside every device lock: a dispatcher may run the job inline.
        (*dispatcher)([this, job, releaseIfUnrun]() { RunQueuedPipelineBuild(job); });

        if (TryGetWarmPipeline(request).IsValid())
            return PipelineBuildState::Warm;
        return m_PipelineBuilds->Find(request.Key, epoch).value_or(PipelineBuildState::Pending);
    }

    PipelineHandle IDevice::BuildPipelineOnCallingThread(const PipelineBuildRequest& request)
    {
        for (;;)
        {
            bool waitedBuildFailed = false;
            const std::optional<uint64_t> ticket =
                m_PipelineBuilds->ClaimForSynchronousBuild(request.Key, waitedBuildFailed);
            if (!ticket)
            {
                // Another thread's build of this key ended: serve what it
                // published. It can publish nothing without failing when an
                // invalidation overtook it; build again then.
                if (PipelineHandle hit = TryGetWarmPipeline(request); hit.IsValid())
                    return hit;
                if (waitedBuildFailed)
                    return {};
                continue;
            }
            PipelineBuildRequest claimed = request;
            claimed.Ticket = *ticket;
            claimed.InvalidationEpoch = m_PipelineCache->GetInvalidationEpoch();
            bool published = false;
            // An unpublished handle (the cache was invalidated during the build)
            // is still the pipeline this caller asked for; it is returned
            // uncached, as a lost insert race always left it.
            const PipelineHandle h = CreateAndPublishPipeline(claimed, published);
            m_PipelineBuilds->Finish(claimed.Key, claimed.Ticket, h.IsValid(), claimed.InvalidationEpoch);
            return h;
        }
    }

    void IDevice::RunQueuedPipelineBuild(const PipelineBuildRequest& request)
    {
        if (!m_PipelineBuilds->ClaimQueued(request.Key, request.Ticket))
            return; // cancelled, or a synchronous caller took the build over
        bool published = false;
        const PipelineHandle h = CreateAndPublishPipeline(request, published);
        if (h.IsValid() && !published)
        {
            // An invalidation overtook the request. Nobody has seen the handle,
            // so it can go now; the next request builds afresh.
            DestroyPipeline(h);
        }
        if (!h.IsValid())
        {
            const std::string detail = request.IsCompute ? std::string("the backend logged why")
                                                         : GetLastGraphicsPipelineFailure();
            Logger::Log::Warning("IDevice: asynchronous {} pipeline build failed (id={}): {}. It is not "
                                 "requested again until the pipeline cache is invalidated.",
                                 request.IsCompute ? "compute" : "graphics", request.PipelineIdValue, detail);
        }
        m_PipelineBuilds->Finish(request.Key, request.Ticket, h.IsValid(), request.InvalidationEpoch);
    }

    PipelineHandle IDevice::TryGetWarmPipeline(const PipelineBuildRequest& request) const
    {
        if (request.IsCompute)
            return TryGetWarmComputePipeline(ComputePipelineId{request.PipelineIdValue});
        return TryGetWarmGraphicsPipeline(GraphicsPipelineId{request.PipelineIdValue}, request.FormatKey);
    }

    PipelineHandle IDevice::CreateAndPublishPipeline(const PipelineBuildRequest& request, bool& published)
    {
        published = false;
        if (request.IsCompute)
            return CreateAndPublishComputePipeline(ComputePipelineId{request.PipelineIdValue},
                                                   request.InvalidationEpoch, published);
        return CreateAndPublishGraphicsPipeline(GraphicsPipelineId{request.PipelineIdValue}, request.FormatKey,
                                                request.InvalidationEpoch, published);
    }

    PipelineHandle IDevice::CreateAndPublishGraphicsPipeline(GraphicsPipelineId id, const PipelineFormatKey& fk,
                                                             uint64_t invalidationEpoch, bool& published)
    {
        // Copy under the cache mutex: CreateConcreteGraphicsPipeline can take
        // hundreds of ms (backend shader compilation, prewarm threads) while
        // concurrent Intern* calls reallocate the storage a Lookup pointer
        // aliases.
        GraphicsPipelineDesc gd{};
        if (!m_PipelineCache->TryCopyGraphicsPipeline(id, gd))
        {
            SetLastGraphicsPipelineFailure(
                "missing interned graphics pipeline desc (" +
                FormatGraphicsPipelineFailureKey(id, fk) + ")");
            return {};
        }

        const std::string previousFailure = GetLastGraphicsPipelineFailure();
        PipelineHandle h = CreateConcreteGraphicsPipeline(gd, fk);
        if (!h.IsValid())
        {
            std::string msg = "backend failed to create concrete graphics pipeline";
            PipelineHandle deadCached{};
            if (m_PipelineCache->TryGetConcrete(PipelineCache::CombineGraphicsKey(id, fk), deadCached) &&
                deadCached.IsValid())
                msg += " after dead cached handle";
            if (!gd.DebugName.empty())
                msg += " name='" + gd.DebugName + "'";
            msg += " (" + FormatGraphicsPipelineFailureKey(id, fk) + ")";
            const std::string backendFailure = GetLastGraphicsPipelineFailure();
            if (!backendFailure.empty() && backendFailure != previousFailure)
                msg += "; backend detail: " + backendFailure;
            SetLastGraphicsPipelineFailure(std::move(msg));
            return {};
        }

        PipelineCache::ConcreteInsertInfo info{};
        info.Handle = h;
        info.FormatKey = fk;
        info.PipelineIdValue = id.Value;
        info.IsCompute = false;
        info.InvalidationEpoch = invalidationEpoch;
        published = m_PipelineCache->InsertConcrete(PipelineCache::CombineGraphicsKey(id, fk), info);
        return h;
    }

    PipelineHandle IDevice::CreateAndPublishComputePipeline(ComputePipelineId id, uint64_t invalidationEpoch,
                                                            bool& published)
    {
        ComputePipelineDesc cd{};
        if (!m_PipelineCache->TryCopyComputePipeline(id, cd))
            return {};

        PipelineHandle h = CreateConcreteComputePipeline(cd);
        if (!h.IsValid())
            return {};

        PipelineCache::ConcreteInsertInfo info{};
        info.Handle = h;
        info.PipelineIdValue = id.Value;
        info.IsCompute = true;
        info.InvalidationEpoch = invalidationEpoch;
        published = m_PipelineCache->InsertConcrete(PipelineCache::CombineComputeKey(id), info);
        return h;
    }

    uint32_t IDevice::PrewarmGraphicsPipeline(GraphicsPipelineId id, std::span<const PipelineFormatKey> fks)
    {
        if (!id.IsValid())
            return 0;
        uint32_t newlyCreated = 0;
        for (const auto& fk : fks)
        {
            const uint64_t key = PipelineCache::CombineGraphicsKey(id, fk);
            PipelineHandle hit{};
            // A dead cached row must not count as "warm" — same self-heal as
            // GetOrCreateGraphicsPipeline, which recreates it below.
            if (m_PipelineCache->TryGetConcrete(key, hit) && IsPipelineAlive(hit))
                continue;
            if (GetOrCreateGraphicsPipeline(id, fk).IsValid())
                ++newlyCreated;
        }
        return newlyCreated;
    }

    PipelineHandle IDevice::CreateGraphicsPipelineDirect(GraphicsPipelineId id, const PipelineFormatKey& fk)
    {
        if (!id.IsValid())
            return {};
        const auto* gd = m_PipelineCache->LookupGraphicsPipeline(id);
        if (!gd)
            return {};
        return CreateConcreteGraphicsPipeline(*gd, fk);
    }

    PipelineCacheStats IDevice::GetPipelineCacheStats() const
    {
        return m_PipelineCache->GetStats();
    }

    PipelineHandle IDevice::CreatePipeline(const PipelineDesc& desc)
    {
        if (desc.type == PipelineType::Compute)
            return GetOrCreateComputePipeline(PipelineDescTranslator::InternCompute(*this, desc));

        const auto id = PipelineDescTranslator::InternGraphics(*this, desc);
        const auto fk = PipelineDescTranslator::BuildFormatKey(desc);
        return GetOrCreateGraphicsPipeline(id, fk);
    }

} // namespace Rendering
} // namespace GameEngine
