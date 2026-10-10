#include "MetalDevice.h"

#include "MetalAccelerationStructures.h"
#include "MetalCommandList.h"
#include "MetalHdrOutput.h"
#include "MetalIndirectCountResources.h"
#include "MetalLayerBridge.h"
#include "MetalQueryPool.h"
#include "MetalSwapchain.h"

#include <chrono>

#include "Logger/Logger.h"
#include "Rendering/Core/RendererProfile.h"
#include "Rendering/Materials/ShaderCompileService.h"

#include <cstring>

namespace GameEngine
{
namespace Rendering
{

MetalDevice::MetalDevice() = default;

MetalDevice::~MetalDevice()
{
    Shutdown();
}

bool MetalDevice::Initialize(const DeviceDesc& desc)
{
    if (m_Initialized)
    {
        return true;
    }
    m_Desc = desc;
    // The EDR decision needs the window's screen, which exists only once a
    // window target does; SetActiveWindowTarget applies this request through
    // SetHdrOutputMode on the first activation.
    m_StartupHdrRequest = MetalStartupHdrRequest(desc);

    m_Device = MTL::CreateSystemDefaultDevice();
    if (m_Device == nullptr)
    {
        Logger::Log::Error("MetalDevice: MTLCreateSystemDefaultDevice returned null");
        return false;
    }

    m_GraphicsQueue = m_Device->newCommandQueue();
    m_ComputeQueue = m_Device->newCommandQueue();
    m_TransferQueue = m_Device->newCommandQueue();
    if (m_GraphicsQueue == nullptr || m_ComputeQueue == nullptr || m_TransferQueue == nullptr)
    {
        Logger::Log::Error("MetalDevice: failed to create command queues");
        Shutdown();
        return false;
    }
    m_GraphicsQueue->setLabel(NS::String::string("GE Graphics Queue", NS::UTF8StringEncoding));
    m_ComputeQueue->setLabel(NS::String::string("GE Compute Queue", NS::UTF8StringEncoding));
    m_TransferQueue->setLabel(NS::String::string("GE Transfer Queue", NS::UTF8StringEncoding));

    m_GraphicsSubmitQueue.Start(m_GraphicsQueue);
    m_ComputeSubmitQueue.Start(m_ComputeQueue);
    m_TransferSubmitQueue.Start(m_TransferQueue);

    // Residency-set + fence path for the BDA registry (macOS 15+): declare the
    // device-address buffers resident once on the queues instead of a blanket
    // useResources on every render/compute encoder, and order compute->draw
    // with an explicit fence (residency sets don't carry hazard tracking).
    // newResidencySet returns null on older OSes, so a successful set is the
    // capability gate; on failure we keep the proven per-encoder useResources
    // path. Enabled by default where supported; GE_METAL_RESIDENCY_SET=0 forces
    // the legacy useResources path.
    if (const char* env = std::getenv("GE_METAL_RESIDENCY_SET"); env == nullptr || env[0] != '0')
    {
        MTL::ResidencySetDescriptor* rsDesc = MTL::ResidencySetDescriptor::alloc()->init();
        NS::Error* rsError = nullptr;
        MTL::ResidencySet* set = m_Device->newResidencySet(rsDesc, &rsError);
        rsDesc->release();
        MTL::Fence* fence = m_Device->newFence();
        if (set != nullptr && fence != nullptr)
        {
            m_BdaResidencySet = set;
            m_GpuDrivenFence = fence;
            m_GpuDrivenFence->setLabel(NS::String::string("GE BDA Compute->Draw Fence", NS::UTF8StringEncoding));
            m_GraphicsQueue->addResidencySet(m_BdaResidencySet);
            m_ComputeQueue->addResidencySet(m_BdaResidencySet);
            m_TransferQueue->addResidencySet(m_BdaResidencySet);
            m_UseResidencySet = true;
            Logger::Log::Info("MetalDevice: residency-set + fence path enabled (GE_METAL_RESIDENCY_SET)");
        }
        else
        {
            if (set != nullptr) set->release();
            if (fence != nullptr) fence->release();
            Logger::Log::Warning("MetalDevice: GE_METAL_RESIDENCY_SET requested but residency sets are unavailable; using useResources path");
        }
    }

    m_IndirectCountResources = std::make_shared<MetalIndirectCountResources>(m_Device);
    if (!m_IndirectCountResources->Initialize({m_GraphicsQueue, m_ComputeQueue, m_TransferQueue}))
    {
        m_IndirectCountResources.reset();
    }

    m_FrameSemaphore = dispatch_semaphore_create(kFramesInFlight);

    m_QueryPool = std::make_unique<MetalQueryPool>(m_Device);
    if (!m_QueryPool->Initialize())
    {
        m_QueryPool.reset();
    }

    // Honest milestone-1 capabilities: GPU-driven features arrive in later
    // milestones; the engine gates on these flags.
    m_Capabilities = {};
    // Bindless: the engine's texture/sampler arrays map onto Tier2 argument
    // buffers (all Apple Silicon); descriptor writes are raw 8-byte slot
    // stores, so update-after-bind semantics hold trivially.
    m_Capabilities.supportsBindlessResources = true;
    m_Capabilities.maxBindlessTextures = 16384;
    m_Capabilities.maxBindlessBuffers = 16384;
    // MTLBuffer::gpuAddress + per-encoder useResource residency (see
    // DeclareDeviceAddressResidency) back the engine's BDA contract.
    m_Capabilities.supportsBufferDeviceAddress = true;
    // Mesh shaders need Metal 3 + an Apple-family-7 GPU (M1/A14 and later);
    // both Apple7 and Metal3 capability imply the drawMeshThreadgroups API.
    m_Capabilities.supportsMeshShaders =
        m_Device->supportsFamily(MTL::GPUFamilyApple7) || m_Device->supportsFamily(MTL::GPUFamilyMetal3);
    // Plain 64-bit integer math is available on Apple GPU family 3 and newer.
    // CBT terrain uses non-atomic ulong load/store and arithmetic for HeapID;
    // its atomics remain 32-bit and do not require Metal's 64-bit atomics.
    m_Capabilities.supportsShaderInt64 = m_Device->supportsFamily(MTL::GPUFamilyApple3);
    m_Capabilities.supportsDescriptorBuffer = false;
    // DrawIndexedIndirectCount draws exactly min(count, maxDrawCount) records
    // (an indirect command buffer run over a GPU-written execution range; see
    // MetalIndirectCountEncoder), so producers need not zero-fill the rest.
    m_Capabilities.supportsDrawIndirectCountNative = m_IndirectCountResources != nullptr;
    // Ray query: MTLAccelerationStructure plus MSL's intersection_query.
    // supportsRaytracing is Metal's own answer for exactly that pair, so it is
    // the gate rather than a GPU-family proxy. It is an API gate, not a
    // performance one — dedicated ray-tracing hardware only arrives with Apple
    // family 9 (M3); earlier families report true here and intersect on the
    // shader cores instead, which is correct but slower.
    m_Capabilities.supportsRayQuery = m_Device->supportsRaytracing();
    // BC1 to BC7 sampling, as Metal reports it for this device rather than
    // inferred from a GPU family. TextureService publishes it to the asset
    // layer, which picks a block-compressed cook artifact and the BC7 Basis
    // transcode target only when it is true.
    m_Capabilities.supportsTextureCompressionBC = m_Device->supportsBCTextureCompression();

    // The Metal backend consumes SPIR-V via SPIRV-Cross, which accepts up to
    // SPIR-V 1.6, so material shaders can use the full 1.6 target here too.
    ShaderCompileService::SetMaxSupportedSpirv(ShaderCompileService::SpirvTarget::Spirv_1_6);
    m_Capabilities.maxPerStageSamplers = 16;
    // Metal's per-stage buffer argument table.
    m_Capabilities.maxPerStageStorageBuffers = 31;

    // Per-stage sampled-image / resource limits. Callers (e.g. the UI texture
    // registry) size their bindless sampled-image arrays against these. The
    // engine realizes every descriptor-set image array as consecutive 8-byte
    // entries inside a Tier2 argument buffer bound with setFragmentBuffer, with
    // residency declared via useResources -- NOT as direct [[texture(n)]]
    // fragment arguments. So the relevant ceiling is the argument buffer's
    // bindless capacity, not the ~128 direct-argument-per-stage limit. On Tier2
    // that capacity matches maxBindlessTextures; reporting the conservative
    // direct-argument count here is what starved the UI array. Tier1 lacks the
    // unbounded argument-buffer addressing, so fall back to the safe direct
    // limit there.
    static constexpr uint32_t kDirectFragmentSampledImageLimit = 96;
    static constexpr uint32_t kNonImageFragmentBindingHeadroom = 16;
    const bool tier2ArgumentBuffers =
        m_Device->argumentBuffersSupport() >= MTL::ArgumentBuffersTier2;
    if (tier2ArgumentBuffers)
    {
        m_Capabilities.maxPerStageSampledImages = m_Capabilities.maxBindlessTextures;
        m_Capabilities.maxPerStageResources =
            m_Capabilities.maxBindlessTextures + kNonImageFragmentBindingHeadroom;
    }
    else
    {
        m_Capabilities.maxPerStageSampledImages = kDirectFragmentSampledImageLimit;
        m_Capabilities.maxPerStageResources = kDirectFragmentSampledImageLimit;
    }
    // Max threads in a threadgroup's primary dimension (1024 on Apple GPUs),
    // queried rather than hardcoded.
    m_Capabilities.maxComputeWorkGroupSize =
        static_cast<uint32_t>(m_Device->maxThreadsPerThreadgroup().width);
    m_Capabilities.maxMSAASamples = 1;
    for (uint32_t samples : {2u, 4u, 8u})
    {
        if (m_Device->supportsTextureSampleCount(samples))
        {
            m_Capabilities.maxMSAASamples = samples;
        }
    }
    // MTLSamplerDescriptor.maxAnisotropy accepts 1 to 16 on every Metal GPU.
    m_Capabilities.maxSamplerAnisotropy = 16.0f;
    m_Capabilities.dedicatedVideoMemory = m_Device->recommendedMaxWorkingSetSize();

    // Queried, not assumed true: Apple silicon is unified, but an Intel Mac
    // with a discrete AMD GPU running Metal is not.
    {
        DeviceMemoryTopology topology{};
        topology.isUnifiedMemory = m_Device->hasUnifiedMemory();
        topology.deviceLocalHeapBytesTotal = m_Device->recommendedMaxWorkingSetSize();
        // On unified memory every byte the GPU can address is also CPU-writable,
        // so the whole working set is the host-visible window. On a discrete Mac
        // the answer is a real zero rather than an unknown: Metal exposes no
        // CPU-writable VRAM aperture at all — shared storage is host memory and
        // managed storage is a mirrored pair the driver blits, so there are
        // genuinely no device-local host-visible bytes to budget.
        topology.largestHostVisibleDeviceLocalHeapBytes =
            topology.isUnifiedMemory ? topology.deviceLocalHeapBytesTotal : 0;
        m_Capabilities.memoryTopology = topology;
    }

    m_Capabilities.minStorageBufferOffsetAlignment = 16;
    m_Capabilities.supportedDepthResolveModes =
        ResolveModeToBit(RenderPassDesc::ResolveMode::SampleZero) |
        ResolveModeToBit(RenderPassDesc::ResolveMode::Average) |
        ResolveModeToBit(RenderPassDesc::ResolveMode::Min) |
        ResolveModeToBit(RenderPassDesc::ResolveMode::Max);
    m_Capabilities.supportedStencilResolveModes = ResolveModeToBit(RenderPassDesc::ResolveMode::SampleZero);
    m_Capabilities.preferredDepthAndStencilFormat = TextureFormat::D32_SFLOAT_S8_UINT;

    // GE_FORCE_COMPAT=1: clamp to the WebGPU-class subset here too. Metal is the
    // default macOS device, so without this hook the override would be a
    // Windows/Vulkan-only test path.
    ApplyForceCompatOverride(m_Capabilities);

    m_Initialized = true;
    Logger::Log::Info("MetalDevice: initialized on '{}'", m_Device->name()->utf8String());
    return true;
}

void MetalDevice::Shutdown()
{
    if (m_Device == nullptr)
    {
        return;
    }
    WaitForIdle();

    // Let subsystems drop their per-device caches while the objects those handles
    // name are still alive, before the backend tears down Metal objects.
    InvokePerDeviceCacheCleanups();

    m_GraphicsSubmitQueue.Stop();
    m_ComputeSubmitQueue.Stop();
    m_TransferSubmitQueue.Stop();

    m_QueryPool.reset();
    // Command lists and completion handlers may still hold the indirect-count
    // pool; detach its residency set from the queues while they exist.
    if (m_IndirectCountResources != nullptr)
    {
        m_IndirectCountResources->Shutdown();
        m_IndirectCountResources.reset();
    }
    // Ahead of the resource pools below: its destructor releases Metal
    // acceleration structures and returns scratch/staging buffers.
    m_AccelerationStructures.reset();

    if (m_FramePool != nullptr)
    {
        m_FramePool->release();
        m_FramePool = nullptr;
    }

    {
        std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);

        m_WindowTargets.ForEach([this](Handle, std::unique_ptr<MetalSwapchain>& swapchain) {
            if (swapchain)
            {
                swapchain->Shutdown(*this);
            }
        });
        m_WindowTargets.Clear();
        m_ActiveWindowTarget = WindowTargetHandle{};

        for (auto& slot : m_DeferredReleases)
        {
            for (NS::Object* object : slot)
            {
                object->release();
            }
            slot.clear();
        }

        m_Buffers.ForEach([](Handle, MetalBuffer& buffer) {
            if (buffer.buffer != nullptr)
            {
                buffer.buffer->release();
                buffer.buffer = nullptr;
            }
        });
        m_Buffers.Clear();

        m_Textures.ForEach([](Handle, MetalTexture& texture) {
            if (!texture.isSwapchainSlot && texture.texture != nullptr)
            {
                texture.texture->release();
            }
            texture.texture = nullptr;
        });
        m_Textures.Clear();

        m_Samplers.ForEach([](Handle, MetalSampler& sampler) {
            if (sampler.sampler != nullptr)
            {
                sampler.sampler->release();
                sampler.sampler = nullptr;
            }
        });
        m_Samplers.Clear();

        m_TextureViews.ForEach([](Handle, MetalTextureView& view) {
            if (view.view != nullptr)
            {
                view.view->release();
                view.view = nullptr;
            }
        });
        m_TextureViews.Clear();

        m_Semaphores.ForEach([](Handle, MetalSemaphore& sem) {
            if (sem.event != nullptr)
            {
                sem.event->release();
                sem.event = nullptr;
            }
        });
        m_Semaphores.Clear();
        m_UploadSemaphore = SemaphoreHandle{};

        m_DescriptorSets.ForEach([](Handle, MetalDescriptorSet& set) {
            if (set.ownsArgumentBuffer)
            {
                set.argumentBuffer->release();
            }
            set.argumentBuffer = nullptr;
        });
        m_DescriptorSets.Clear();
        for (auto& slot : m_TransientDescriptorSets)
        {
            slot.clear();
        }
        for (MetalTransientDescriptorArena& arena : m_TransientDescriptorArenas)
        {
            arena.Release();
        }

        if (m_FillBufferPipeline != nullptr)
        {
            m_FillBufferPipeline->release();
            m_FillBufferPipeline = nullptr;
        }
        for (auto& [format, pipeline] : m_MipBlitPipelines)
        {
            if (pipeline != nullptr)
            {
                pipeline->release();
            }
        }
        m_MipBlitPipelines.clear();
        if (m_MipBlitSampler != nullptr)
        {
            m_MipBlitSampler->release();
            m_MipBlitSampler = nullptr;
        }
        if (m_MipBlitLibrary != nullptr)
        {
            m_MipBlitLibrary->release();
            m_MipBlitLibrary = nullptr;
        }
        if (m_DebugAlwaysDepthState != nullptr)
        {
            m_DebugAlwaysDepthState->release();
            m_DebugAlwaysDepthState = nullptr;
        }
        for (MTL::DepthStencilState*& state : m_ReadOnlyDepthStates)
        {
            if (state != nullptr)
            {
                state->release();
                state = nullptr;
            }
        }

        m_Pipelines.ForEach([](Handle, MetalPipeline& pipeline) {
            if (pipeline.renderPipeline != nullptr)
            {
                pipeline.renderPipeline->release();
            }
            if (pipeline.computePipeline != nullptr)
            {
                pipeline.computePipeline->release();
            }
            if (pipeline.depthStencilState != nullptr)
            {
                pipeline.depthStencilState->release();
            }
            pipeline.renderPipeline = nullptr;
            pipeline.computePipeline = nullptr;
            pipeline.depthStencilState = nullptr;
        });
        m_Pipelines.Clear();
    }

    if (m_BdaResidencySet != nullptr)
    {
        if (m_GraphicsQueue != nullptr) m_GraphicsQueue->removeResidencySet(m_BdaResidencySet);
        if (m_ComputeQueue != nullptr) m_ComputeQueue->removeResidencySet(m_BdaResidencySet);
        if (m_TransferQueue != nullptr) m_TransferQueue->removeResidencySet(m_BdaResidencySet);
        m_BdaResidencySet->release();
        m_BdaResidencySet = nullptr;
    }
    if (m_GpuDrivenFence != nullptr)
    {
        m_GpuDrivenFence->release();
        m_GpuDrivenFence = nullptr;
    }

    if (m_GraphicsQueue != nullptr)
    {
        m_GraphicsQueue->release();
        m_GraphicsQueue = nullptr;
    }
    if (m_ComputeQueue != nullptr)
    {
        m_ComputeQueue->release();
        m_ComputeQueue = nullptr;
    }
    if (m_TransferQueue != nullptr)
    {
        m_TransferQueue->release();
        m_TransferQueue = nullptr;
    }
    m_Device->release();
    m_Device = nullptr;
    m_Initialized = false;
}

std::string MetalDevice::GetHardwareDescription() const
{
    if (m_Device == nullptr)
    {
        return {};
    }
    return std::string(m_Device->name()->utf8String()) + "  Metal 3";
}

void MetalDevice::PrintCapabilityReport() const
{
    const RenderingDeviceCapabilities& c = m_Capabilities;
    Logger::Log::Info("Metal capabilities | {}", GetHardwareDescription());
    Logger::Log::Info("  bindless={} maxBindlessTex={} maxBindlessBuf={} BDA={} residencySet={}",
                      c.supportsBindlessResources, c.maxBindlessTextures, c.maxBindlessBuffers,
                      c.supportsBufferDeviceAddress, m_UseResidencySet);
    Logger::Log::Info("  maxMSAA={} maxComputeWorkGroup={} maxPerStageSamplers={} timestamps={}",
                      c.maxMSAASamples, c.maxComputeWorkGroupSize, c.maxPerStageSamplers,
                      m_QueryPool != nullptr && m_QueryPool->IsValid());
    Logger::Log::Info("  meshShaders={} rayTracing={} descriptorBuffer={} occlusionQueries=available",
                      c.supportsMeshShaders, c.supportsRayTracing, c.supportsDescriptorBuffer);
}

// ---------------------------------------------------------------------------
// Internal accessors
// ---------------------------------------------------------------------------

MTL::CommandQueue* MetalDevice::GetQueue(QueueType queue) const
{
    switch (queue)
    {
    case QueueType::Compute:  return m_ComputeQueue;
    case QueueType::Transfer: return m_TransferQueue;
    case QueueType::Graphics:
    default:                  return m_GraphicsQueue;
    }
}

bool MetalDevice::SetHdrOutputMode(HdrOutputMode mode, const HdrStaticMetadata* metadata,
                                   HdrSwapchainBitDepth bitDepth)
{
    MetalSwapchain* swapchain = GetSwapchain(m_ActiveWindowTarget);

    MetalEdrHeadroom headroom{};
    if (swapchain != nullptr)
    {
        MetalLayerBridge::GetWindowEdrHeadroom(swapchain->GetWindow(), headroom.current, headroom.potential);
    }
    // GE_METAL_FORCE_EDR=1 lets the EDR path run on SDR displays for
    // verification (output simply clips at headroom 1.0).
    static const bool kForceEdr = []() {
        const char* env = std::getenv("GE_METAL_FORCE_EDR");
        return env != nullptr && env[0] != '0';
    }();
    headroom.forced = kForceEdr;

    m_HdrState = ResolveMetalHdrOutputState(m_HdrState, mode, metadata, bitDepth, headroom);
    const HdrOutputMode resolved = m_HdrState.activeMode;

    if (swapchain != nullptr)
    {
        // The slot texture format changes; drain in-flight work first.
        WaitForIdle();
        swapchain->SetScRGBOutput(*this, resolved == HdrOutputMode::ScRGB);
        m_HdrState.display.swapchainFormat = GetSwapchainTextureFormat();
        m_HdrState.display.swapchainBitDepth = m_HdrState.swapchainBitDepth;
    }
    Logger::Log::Info("MetalDevice: HDR output requested={} resolved={} headroom current={:.2f} potential={:.2f}",
                      HdrOutputModeToString(mode), HdrOutputModeToString(resolved), headroom.current,
                      headroom.potential);
    return true;
}

void MetalDevice::SetVsync(bool vsync)
{
    m_Desc.vsync = vsync;
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    m_WindowTargets.ForEach([vsync](Handle, std::unique_ptr<MetalSwapchain>& swapchain) {
        if (swapchain)
        {
            swapchain->SetDisplaySync(vsync);
        }
    });
}

void MetalDevice::CommitResidencyIfDirty()
{
    if (m_BdaResidencySet == nullptr)
    {
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    if (m_ResidencyDirty)
    {
        m_ResidencyDirty = false;
        m_BdaResidencySet->commit();
        m_BdaResidencySet->requestResidency();
    }
}

void MetalDevice::TrackFrameCommandBuffer(MTL::CommandBuffer* buffer)
{
    if (buffer == nullptr)
    {
        return;
    }
    // A buffer added to the residency set since the last commit is not resident
    // until commit()+requestResidency() runs. Every enqueue path funnels here,
    // so commit any pending membership change before this CB reaches the GPU.
    CommitResidencyIfDirty();
    const uint32_t slot = m_FrameIndex.load(std::memory_order_relaxed);
    const bool onGraphicsQueue = buffer->commandQueue() == m_GraphicsQueue;
    m_SlotInFlightCbs[slot].fetch_add(1, std::memory_order_acq_rel);
    if (onGraphicsQueue)
    {
        m_GraphicsCbsInFlight.fetch_add(1, std::memory_order_acq_rel);
    }
    buffer->addCompletedHandler([this, slot, onGraphicsQueue](MTL::CommandBuffer*) {
        RetireTrackedCommandBuffer(slot, onGraphicsQueue);
    });
}

void MetalDevice::RetireTrackedCommandBuffer(uint32_t slot, bool onGraphicsQueue)
{
    if (onGraphicsQueue)
    {
        m_GraphicsCbsInFlight.fetch_sub(1, std::memory_order_acq_rel);
    }
    if (m_SlotInFlightCbs[slot].fetch_sub(1, std::memory_order_acq_rel) == 1)
    {
        std::lock_guard<std::mutex> lock(m_SlotCbMutex);
        m_SlotCbCv.notify_all();
    }
}

bool MetalDevice::IsPreviousFrameGraphicsComplete() const
{
    // Non-blocking. Metal runs a command buffer's completion handler only after
    // the GPU has finished it, when its writes to shared storage are visible to
    // the CPU; the acquire pairs with the handler's decrement. Zero in flight
    // therefore means the newest graphics submission, and any readback copy it
    // carried, has completed. A buffer the submit worker still holds behind a
    // semaphore wait was counted at enqueue and reads as in flight. Nothing
    // submitted yet reads as complete: there is no stale mirror to guard.
    return m_GraphicsCbsInFlight.load(std::memory_order_acquire) == 0;
}

void MetalDevice::WaitForSlotCommandBuffers(uint32_t slot)
{
    if (m_SlotInFlightCbs[slot].load(std::memory_order_acquire) == 0)
    {
        return;
    }
    std::unique_lock<std::mutex> lock(m_SlotCbMutex);
    int waitedSeconds = 0;
    while (!m_SlotCbCv.wait_for(lock, std::chrono::seconds(1), [this, slot]() {
        return m_SlotInFlightCbs[slot].load(std::memory_order_acquire) == 0;
    }))
    {
        if (++waitedSeconds == 3)
        {
            Logger::Log::Warning("MetalDevice: still waiting on {} command buffer(s) from frame slot {}",
                                 m_SlotInFlightCbs[slot].load(), slot);
        }
    }
}

MetalSubmitQueue* MetalDevice::GetSubmitQueue(QueueType queue)
{
    switch (queue)
    {
    case QueueType::Compute:  return &m_ComputeSubmitQueue;
    case QueueType::Transfer: return &m_TransferSubmitQueue;
    case QueueType::Graphics:
    default:                  return &m_GraphicsSubmitQueue;
    }
}

MetalBuffer* MetalDevice::GetMetalBuffer(BufferHandle handle)
{
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    return m_Buffers.Get(ToGeneric(handle));
}

MetalTextureView* MetalDevice::GetMetalTextureView(TextureViewHandle handle)
{
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    return m_TextureViews.Get(ToGeneric(handle));
}

MetalDescriptorSet* MetalDevice::GetMetalDescriptorSet(DescriptorSetHandle handle)
{
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    return m_DescriptorSets.Get(ToGeneric(handle));
}

MetalTexture* MetalDevice::GetMetalTexture(TextureHandle handle)
{
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    return m_Textures.Get(ToGeneric(handle));
}

MetalPipeline* MetalDevice::GetMetalPipeline(PipelineHandle handle)
{
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    return m_Pipelines.Get(ToGeneric(handle));
}

const MetalPipeline* MetalDevice::GetMetalPipeline(PipelineHandle handle) const
{
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    return m_Pipelines.Get(ToGeneric(handle));
}

void MetalDevice::DeferRelease(NS::Object* object)
{
    if (object == nullptr)
    {
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    m_DeferredReleases[m_FrameIndex.load(std::memory_order_relaxed)].push_back(object);
}

void MetalDevice::ScrubResidency(MTL::Resource* resource)
{
    if (resource == nullptr)
    {
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    m_DescriptorSets.ForEach([resource](Handle, MetalDescriptorSet& set) {
        for (size_t i = 0; i < set.residentResources.size(); ++i)
        {
            if (set.residentResources[i] == resource)
            {
                set.residentResources[i] = nullptr;
                set.residentWritable[i] = 0;
                set.residentIndirectArguments[i] = 0;
                set.residentsDirty = true;
            }
        }
    });
}

void MetalDevice::DrainDeferredReleases(uint32_t slot)
{
    std::vector<NS::Object*> pending;
    {
        std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
        pending.swap(m_DeferredReleases[slot]);
    }
    for (NS::Object* object : pending)
    {
        object->release();
    }
}

// ---------------------------------------------------------------------------
// Synchronization
// ---------------------------------------------------------------------------

uint64_t MetalDevice::GetBufferDeviceAddress(BufferHandle handle)
{
    MetalBuffer* buf = GetMetalBuffer(handle);
    return (buf != nullptr && buf->buffer != nullptr) ? buf->buffer->gpuAddress() : 0;
}

SemaphoreHandle MetalDevice::CreateTimelineSemaphore(uint64_t initialValue)
{
    if (m_Device == nullptr)
    {
        return SemaphoreHandle{};
    }
    MTL::SharedEvent* event = m_Device->newSharedEvent();
    if (event == nullptr)
    {
        return SemaphoreHandle{};
    }
    event->setSignaledValue(initialValue);
    MetalSemaphore wrapper{};
    wrapper.event = event;
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    return FromGeneric<SemaphoreTag>(m_Semaphores.Create(std::move(wrapper)));
}

void MetalDevice::DestroySemaphore(SemaphoreHandle handle)
{
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    MetalSemaphore* sem = m_Semaphores.Get(ToGeneric(handle));
    if (sem == nullptr)
    {
        return;
    }
    DeferRelease(sem->event);
    sem->event = nullptr;
    m_Semaphores.Destroy(ToGeneric(handle));
}

bool MetalDevice::GetTimelineSemaphoreValue(SemaphoreHandle handle, uint64_t& outValue) const
{
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    const MetalSemaphore* sem = m_Semaphores.Get(ToGeneric(handle));
    if (sem == nullptr || sem->event == nullptr)
    {
        return false;
    }
    outValue = sem->event->signaledValue();
    return true;
}

bool MetalDevice::WaitTimelineSemaphoreValue(SemaphoreHandle handle, uint64_t value, uint64_t timeoutNs)
{
    MTL::SharedEvent* event = nullptr;
    {
        std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
        MetalSemaphore* sem = m_Semaphores.Get(ToGeneric(handle));
        if (sem == nullptr || sem->event == nullptr)
        {
            return false;
        }
        event = sem->event;
    }
    const uint64_t timeoutMs = timeoutNs == ~0ull ? ~0ull : timeoutNs / 1'000'000ull;
    return event->waitUntilSignaledValue(value, timeoutMs);
}

bool MetalDevice::QueueSubmit(QueueType queue,
                              const std::vector<CommandList*>& cmdLists,
                              const std::vector<std::pair<SemaphoreHandle, uint64_t>>& waitSemaphores,
                              const std::vector<std::pair<SemaphoreHandle, uint64_t>>& signalSemaphores)
{
    MetalSubmitQueue* worker = GetSubmitQueue(queue);
    if (worker == nullptr)
    {
        return false;
    }

    // Queue-level event waits stall every later command buffer on the queue
    // and deadlock on wait-before-signal submission order, so semaphore waits
    // are satisfied CPU-side on the submit worker before the buffers commit.
    MetalSubmitQueue::Submit submit;
    {
        std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
        for (const auto& [handle, value] : waitSemaphores)
        {
            MetalSemaphore* sem = m_Semaphores.Get(ToGeneric(handle));
            if (sem != nullptr && sem->event != nullptr)
            {
                sem->event->retain();
                submit.Waits.emplace_back(sem->event, value);
            }
        }
        for (const auto& [handle, value] : signalSemaphores)
        {
            MetalSemaphore* sem = m_Semaphores.Get(ToGeneric(handle));
            if (sem != nullptr && sem->event != nullptr)
            {
                sem->event->retain();
                submit.Signals.emplace_back(sem->event, value);
            }
        }
    }
    for (CommandList* list : cmdLists)
    {
        if (list == nullptr)
        {
            continue;
        }
        MTL::CommandBuffer* cb = static_cast<MetalCommandList*>(list)->Detach();
        if (cb != nullptr)
        {
            TrackFrameCommandBuffer(cb);
            submit.Buffers.push_back(cb);
        }
    }
    worker->Enqueue(std::move(submit));
    return true;
}

IDevice::GpuSyncToken MetalDevice::SubmitTextureUploads(const TextureUploadRequest* requests, uint32_t count)
{
    if (m_Device == nullptr || requests == nullptr || count == 0 || m_TransferQueue == nullptr)
    {
        return {};
    }
    if (!m_UploadSemaphore.IsValid())
    {
        m_UploadSemaphore = CreateTimelineSemaphore(0);
        if (!m_UploadSemaphore.IsValid())
        {
            return {};
        }
    }

    size_t totalBytes = 0;
    for (uint32_t i = 0; i < count; ++i)
    {
        totalBytes += requests[i].RowPitchBytes * requests[i].Height;
    }
    MTL::Buffer* staging = m_Device->newBuffer(totalBytes, MTL::ResourceStorageModeShared);
    if (staging == nullptr)
    {
        return {};
    }

    uint8_t* dst = static_cast<uint8_t*>(staging->contents());
    size_t offset = 0;
    MTL::CommandBuffer* cb = m_TransferQueue->commandBuffer();
    MTL::BlitCommandEncoder* blit = cb != nullptr ? cb->blitCommandEncoder() : nullptr;
    if (blit == nullptr)
    {
        staging->release();
        return {};
    }
    for (uint32_t i = 0; i < count; ++i)
    {
        const TextureUploadRequest& req = requests[i];
        const size_t bytes = req.RowPitchBytes * req.Height;
        std::memcpy(dst + offset, req.Pixels, bytes);

        MetalTexture* tex = GetMetalTexture(req.Texture);
        if (tex != nullptr && tex->texture != nullptr)
        {
            blit->copyFromBuffer(staging, offset, req.RowPitchBytes, 0,
                                 MTL::Size(req.Width, req.Height, 1), tex->texture, req.ArrayLayer,
                                 req.MipLevel, MTL::Origin(0, 0, 0));
        }
        offset += bytes;
    }
    blit->endEncoding();

    const uint64_t value = ++m_UploadCounter;
    {
        std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
        MetalSemaphore* sem = m_Semaphores.Get(ToGeneric(m_UploadSemaphore));
        if (sem != nullptr && sem->event != nullptr)
        {
            cb->encodeSignalEvent(sem->event, value);
        }
    }
    TrackFrameCommandBuffer(cb);
    cb->retain();
    {
        MetalSubmitQueue::Submit submit;
        submit.Buffers.push_back(cb);
        m_TransferSubmitQueue.Enqueue(std::move(submit));
    }
    staging->release(); // command buffer retains it until completion

    GpuSyncToken token{};
    token.sem = m_UploadSemaphore;
    token.value = value;
    return token;
}

// ---------------------------------------------------------------------------
// Internal compute helpers
// ---------------------------------------------------------------------------

MTL::ComputePipelineState* MetalDevice::GetFillBufferPipeline()
{
    if (m_FillBufferPipeline != nullptr)
    {
        return m_FillBufferPipeline;
    }
    static const char* kFillSource = R"(
#include <metal_stdlib>
using namespace metal;
struct FillParams { uint value; uint count; };
kernel void fill_main(device uint* dst [[buffer(0)]],
                      constant FillParams& params [[buffer(1)]],
                      uint tid [[thread_position_in_grid]])
{
    if (tid < params.count) { dst[tid] = params.value; }
}
)";
    NS::Error* error = nullptr;
    MTL::Library* library =
        m_Device->newLibrary(NS::String::string(kFillSource, NS::UTF8StringEncoding), nullptr, &error);
    if (library == nullptr)
    {
        Logger::Log::Error("MetalDevice: fill kernel compile failed: {}",
                           error != nullptr ? error->localizedDescription()->utf8String() : "unknown");
        return nullptr;
    }
    MTL::Function* fn = library->newFunction(NS::String::string("fill_main", NS::UTF8StringEncoding));
    library->release();
    if (fn == nullptr)
    {
        return nullptr;
    }
    error = nullptr;
    m_FillBufferPipeline = m_Device->newComputePipelineState(fn, &error);
    fn->release();
    if (m_FillBufferPipeline == nullptr)
    {
        Logger::Log::Error("MetalDevice: fill pipeline creation failed: {}",
                           error != nullptr ? error->localizedDescription()->utf8String() : "unknown");
    }
    return m_FillBufferPipeline;
}

MTL::RenderPipelineState* MetalDevice::GetMipBlitPipeline(MTL::PixelFormat format)
{
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    auto it = m_MipBlitPipelines.find(static_cast<uint32_t>(format));
    if (it != m_MipBlitPipelines.end())
    {
        return it->second;
    }

    if (m_MipBlitLibrary == nullptr)
    {
        static const char* kBlitSource = R"(
#include <metal_stdlib>
using namespace metal;
struct BlitVOut { float4 pos [[position]]; float2 uv; };
vertex BlitVOut ge_mip_blit_vs(uint vid [[vertex_id]])
{
    float2 uv = float2((vid << 1) & 2, vid & 2);
    BlitVOut out;
    out.pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
    out.uv = uv;
    return out;
}
fragment float4 ge_mip_blit_fs(BlitVOut in [[stage_in]],
                               texture2d<float> src [[texture(0)]],
                               sampler smp [[sampler(0)]])
{
    return src.sample(smp, in.uv, level(0.0));
}
)";
        NS::Error* error = nullptr;
        m_MipBlitLibrary =
            m_Device->newLibrary(NS::String::string(kBlitSource, NS::UTF8StringEncoding), nullptr, &error);
        if (m_MipBlitLibrary == nullptr)
        {
            Logger::Log::Error("MetalDevice: mip blit shader compile failed: {}",
                               error != nullptr ? error->localizedDescription()->utf8String() : "unknown");
            return nullptr;
        }
    }

    MTL::Function* vs = m_MipBlitLibrary->newFunction(NS::String::string("ge_mip_blit_vs", NS::UTF8StringEncoding));
    MTL::Function* fs = m_MipBlitLibrary->newFunction(NS::String::string("ge_mip_blit_fs", NS::UTF8StringEncoding));
    MTL::RenderPipelineState* pipeline = nullptr;
    if (vs != nullptr && fs != nullptr)
    {
        MTL::RenderPipelineDescriptor* desc = MTL::RenderPipelineDescriptor::alloc()->init();
        desc->setVertexFunction(vs);
        desc->setFragmentFunction(fs);
        desc->colorAttachments()->object(0)->setPixelFormat(format);
        desc->setLabel(NS::String::string("GE MipBlit", NS::UTF8StringEncoding));
        NS::Error* error = nullptr;
        pipeline = m_Device->newRenderPipelineState(desc, &error);
        desc->release();
        if (pipeline == nullptr)
        {
            Logger::Log::Error("MetalDevice: mip blit pipeline creation failed for format {}: {}",
                               static_cast<uint32_t>(format),
                               error != nullptr ? error->localizedDescription()->utf8String() : "unknown");
        }
    }
    if (vs != nullptr)
    {
        vs->release();
    }
    if (fs != nullptr)
    {
        fs->release();
    }
    // Cache only on success — a transient creation failure must not poison the
    // slot and make every later call for this format silently return null.
    if (pipeline != nullptr)
        m_MipBlitPipelines.emplace(static_cast<uint32_t>(format), pipeline);
    return pipeline;
}

MTL::SamplerState* MetalDevice::GetMipBlitSampler()
{
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    if (m_MipBlitSampler != nullptr)
    {
        return m_MipBlitSampler;
    }
    MTL::SamplerDescriptor* desc = MTL::SamplerDescriptor::alloc()->init();
    desc->setMinFilter(MTL::SamplerMinMagFilterLinear);
    desc->setMagFilter(MTL::SamplerMinMagFilterLinear);
    desc->setMipFilter(MTL::SamplerMipFilterNotMipmapped);
    desc->setSAddressMode(MTL::SamplerAddressModeClampToEdge);
    desc->setTAddressMode(MTL::SamplerAddressModeClampToEdge);
    desc->setLabel(NS::String::string("GE MipBlit Sampler", NS::UTF8StringEncoding));
    m_MipBlitSampler = m_Device->newSamplerState(desc);
    desc->release();
    return m_MipBlitSampler;
}

void MetalDevice::DeclareDeviceAddressResidency(MTL::RenderCommandEncoder* encoder,
                                                std::unordered_map<MTL::Resource*, MTL::ResourceUsage>& residentCache)
{
    // Residency-set path declares residency once per frame on the queue and
    // orders compute->draw with the fence; the per-encoder blanket is skipped.
    if (m_UseResidencySet)
    {
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    if (encoder == nullptr || m_DeviceAddressBuffers.empty())
    {
        return;
    }
    // Read-only: graphics stages never write through device-address pointers
    // in this engine (draw-stream/visibility writes happen in compute).
    // Declaring Write here makes the hazard tracker serialize every render
    // encoder against every other encoder touching these buffers.
    encoder->useResources(reinterpret_cast<const MTL::Resource* const*>(m_DeviceAddressBuffers.data()),
                          m_DeviceAddressBuffers.size(), MTL::ResourceUsageRead,
                          MTL::RenderStageVertex | MTL::RenderStageFragment);
    for (MTL::Buffer* buffer : m_DeviceAddressBuffers)
    {
        residentCache[buffer] = MTL::ResourceUsageRead;
    }
}

void MetalDevice::DeclareDeviceAddressResidency(MTL::ComputeCommandEncoder* encoder,
                                                std::unordered_map<MTL::Resource*, MTL::ResourceUsage>& residentCache)
{
    if (m_UseResidencySet)
    {
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    if (encoder == nullptr || m_DeviceAddressBuffers.empty())
    {
        return;
    }
    encoder->useResources(reinterpret_cast<const MTL::Resource* const*>(m_DeviceAddressBuffers.data()),
                          m_DeviceAddressBuffers.size(), MTL::ResourceUsageRead | MTL::ResourceUsageWrite);
    for (MTL::Buffer* buffer : m_DeviceAddressBuffers)
    {
        residentCache[buffer] = MTL::ResourceUsageRead | MTL::ResourceUsageWrite;
    }
}

void MetalDevice::DeclareIndirectArgumentReads(MTL::ComputeCommandEncoder* encoder)
{
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    if (encoder == nullptr || m_IndirectArgumentBuffers.empty())
    {
        return;
    }
    encoder->useResources(reinterpret_cast<const MTL::Resource* const*>(m_IndirectArgumentBuffers.data()),
                          m_IndirectArgumentBuffers.size(), MTL::ResourceUsageRead);
}

IAccelerationStructureBackend* MetalDevice::GetAccelerationStructureBackend()
{
    if (!m_Capabilities.supportsRayQuery)
    {
        return nullptr;
    }
    if (m_AccelerationStructures == nullptr)
    {
        m_AccelerationStructures = std::make_unique<MetalAccelerationStructures>(*this);
    }
    return m_AccelerationStructures.get();
}

MTL::Buffer* MetalDevice::FindBufferByGpuAddress(uint64_t address, uint64_t& outOffset)
{
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    MTL::Buffer* found = nullptr;
    m_Buffers.ForEach([&](Handle, MetalBuffer& buffer) {
        if (found != nullptr || buffer.buffer == nullptr)
        {
            return;
        }
        const uint64_t base = buffer.buffer->gpuAddress();
        if (address >= base && address < base + buffer.buffer->length())
        {
            found = buffer.buffer;
            outOffset = address - base;
        }
    });
    return found;
}

void MetalDevice::ArmGpuCaptureIfRequested()
{
    static bool s_Requested = []() {
        const char* env = std::getenv("GE_METAL_GPU_CAPTURE");
        return env != nullptr && env[0] != '0';
    }();
    static bool s_Done = false;
    // Capture the 120th frame: the scene view is fully up by then.
    static int s_FrameCountdown = 120;
    if (!s_Requested || s_Done || m_GpuCaptureActive)
    {
        return;
    }
    if (--s_FrameCountdown > 0)
    {
        return;
    }
    s_Done = true;

    MTL::CaptureManager* manager = MTL::CaptureManager::sharedCaptureManager();
    MTL::CaptureDescriptor* descriptor = MTL::CaptureDescriptor::alloc()->init();
    descriptor->setCaptureObject(m_Device);
    descriptor->setDestination(MTL::CaptureDestinationGPUTraceDocument);
    NS::String* path = NS::String::string("/tmp/ge_metal_frame.gputrace", NS::UTF8StringEncoding);
    descriptor->setOutputURL(NS::URL::fileURLWithPath(path));
    NS::Error* error = nullptr;
    if (manager->startCapture(descriptor, &error))
    {
        m_GpuCaptureActive = true;
        m_GpuCaptureFramesLeft = 1;
        Logger::Log::Warning("MetalDevice: GPU capture started -> /tmp/ge_metal_frame.gputrace");
    }
    else
    {
        Logger::Log::Error("MetalDevice: GPU capture failed to start: {} (set METAL_CAPTURE_ENABLED=1)",
                           error != nullptr ? error->localizedDescription()->utf8String() : "unknown");
    }
    descriptor->release();
}

void MetalDevice::EndGpuCaptureIfActive()
{
    if (!m_GpuCaptureActive)
    {
        return;
    }
    if (--m_GpuCaptureFramesLeft > 0)
    {
        return;
    }
    MTL::CaptureManager::sharedCaptureManager()->stopCapture();
    m_GpuCaptureActive = false;
    Logger::Log::Warning("MetalDevice: GPU capture finished -> /tmp/ge_metal_frame.gputrace");
}

MTL::DepthStencilState* MetalDevice::GetReadOnlyDepthState(MTL::CompareFunction compare)
{
    const size_t index = static_cast<size_t>(compare) & 7;
    if (m_ReadOnlyDepthStates[index] == nullptr && m_Device != nullptr)
    {
        MTL::DepthStencilDescriptor* dsd = MTL::DepthStencilDescriptor::alloc()->init();
        dsd->setDepthCompareFunction(compare);
        dsd->setDepthWriteEnabled(false);
        m_ReadOnlyDepthStates[index] = m_Device->newDepthStencilState(dsd);
        dsd->release();
    }
    return m_ReadOnlyDepthStates[index];
}

MTL::DepthStencilState* MetalDevice::GetDebugAlwaysDepthState()
{
    if (m_DebugAlwaysDepthState == nullptr && m_Device != nullptr)
    {
        MTL::DepthStencilDescriptor* dsd = MTL::DepthStencilDescriptor::alloc()->init();
        dsd->setDepthCompareFunction(MTL::CompareFunctionAlways);
        dsd->setDepthWriteEnabled(true);
        m_DebugAlwaysDepthState = m_Device->newDepthStencilState(dsd);
        dsd->release();
    }
    return m_DebugAlwaysDepthState;
}

// ---------------------------------------------------------------------------
// Push constant introspection
// ---------------------------------------------------------------------------

uint32_t MetalDevice::GetPipelinePushConstantRangeCount(PipelineHandle pipeline) const
{
    const MetalPipeline* mp = GetMetalPipeline(pipeline);
    return mp != nullptr ? static_cast<uint32_t>(mp->pushRanges.size()) : 0u;
}

bool MetalDevice::GetPipelinePushConstantRangeInfo(PipelineHandle pipeline, uint32_t id,
                                                   PushConstantRangeInfo& outInfo) const
{
    const MetalPipeline* mp = GetMetalPipeline(pipeline);
    if (mp == nullptr || id >= mp->pushRanges.size())
    {
        return false;
    }
    const MetalPipeline::PushRange& range = mp->pushRanges[id];
    outInfo.id = id;
    outInfo.name = range.name.c_str();
    outInfo.offset = range.offset;
    outInfo.size = range.size;
    outInfo.stagesMask = range.stagesMask;
    return true;
}

bool MetalDevice::FindPipelinePushConstantRangeId(PipelineHandle pipeline, const char* name, uint32_t& outId) const
{
    const MetalPipeline* mp = GetMetalPipeline(pipeline);
    if (mp == nullptr || name == nullptr)
    {
        return false;
    }
    for (uint32_t i = 0; i < mp->pushRanges.size(); ++i)
    {
        if (mp->pushRanges[i].name == name)
        {
            outId = i;
            return true;
        }
    }
    return false;
}

bool MetalDevice::GetPipelinePushConstantInfo(PipelineHandle pipeline, PipelinePushConstantInfo& outInfo) const
{
    const MetalPipeline* mp = GetMetalPipeline(pipeline);
    if (mp == nullptr)
    {
        return false;
    }
    outInfo.size = mp->pushConstantSize;
    outInfo.stagesMask = mp->pushConstantStagesMask;
    return true;
}

// ---------------------------------------------------------------------------
// Command lists & submission
// ---------------------------------------------------------------------------

std::unique_ptr<CommandList> MetalDevice::CreateCommandList(QueueType queue)
{
    return std::make_unique<MetalCommandList>(*this, queue);
}

void MetalDevice::ExecuteCommandLists(const std::vector<CommandList*>& commandLists)
{
    for (CommandList* list : commandLists)
    {
        if (list == nullptr)
        {
            continue;
        }
        auto* metalList = static_cast<MetalCommandList*>(list);
        MTL::CommandBuffer* cb = metalList->Detach();
        if (cb == nullptr)
        {
            continue;
        }
        TrackFrameCommandBuffer(cb);
        MetalSubmitQueue* worker = GetSubmitQueue(metalList->GetQueueType());
        if (worker == nullptr)
        {
            cb->commit();
            cb->release();
            continue;
        }
        MetalSubmitQueue::Submit submit;
        submit.Buffers.push_back(cb);
        worker->Enqueue(std::move(submit));
    }
}

IQueryPool* MetalDevice::GetQueryPool()
{
    return m_QueryPool.get();
}

void MetalDevice::WaitForIdle()
{
    // Drain the submit workers first so every pending command buffer reaches
    // its queue before the marker buffers measure "idle".
    m_GraphicsSubmitQueue.Flush();
    m_ComputeSubmitQueue.Flush();
    m_TransferSubmitQueue.Flush();
    for (MTL::CommandQueue* queue : {m_GraphicsQueue, m_ComputeQueue, m_TransferQueue})
    {
        if (queue == nullptr)
        {
            continue;
        }
        MTL::CommandBuffer* cb = queue->commandBuffer();
        if (cb != nullptr)
        {
            cb->commit();
            cb->waitUntilCompleted();
        }
    }
}

// ---------------------------------------------------------------------------
// Frame loop
// ---------------------------------------------------------------------------

bool MetalDevice::BeginFrame()
{
    if (!m_Initialized)
    {
        return false;
    }
    dispatch_semaphore_wait(m_FrameSemaphore, DISPATCH_TIME_FOREVER);
    m_FrameSyncIssued = false;
    m_FrameActive = true;

    // The pacing semaphore covers work enqueued before the present; command
    // buffers enqueued after it (late views, uploads) are tracked per slot
    // and must also drain before the slot's resources recycle.
    WaitForSlotCommandBuffers(m_FrameIndex);

    // The waits above guarantee this slot's prior GPU work completed, so the
    // slot's counter samples can be resolved without stalling.
    if (m_QueryPool != nullptr)
    {
        m_QueryPool->BeginFrame(m_FrameIndex);
    }

    ArmGpuCaptureIfRequested();

    if (m_FramePool != nullptr)
    {
        m_FramePool->release();
    }
    m_FramePool = NS::AutoreleasePool::alloc()->init();

    DrainDeferredReleases(m_FrameIndex);

    // Transient descriptor sets allocated kFramesInFlight frames ago are no
    // longer referenced by in-flight GPU work; recycle them and their arena now,
    // under one lock so a set created meanwhile cannot land in the arena after
    // its list was taken.
    {
        std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
        std::vector<DescriptorSetHandle>& transientSets = m_TransientDescriptorSets[m_FrameIndex];
        for (DescriptorSetHandle handle : transientSets)
        {
            DestroyDescriptorSet(handle);
        }
        transientSets.clear();
        m_TransientDescriptorArenas[m_FrameIndex].Reset();
    }

    MetalSwapchain* swapchain = GetSwapchain(m_ActiveWindowTarget);
    if (swapchain != nullptr && m_Desc.enableSwapchain)
    {
        uint32_t imageIndex = 0;
        if (!swapchain->Acquire(*this, imageIndex))
        {
            // Occluded window: the frame still proceeds (offscreen work is
            // legal); Present degrades to FinalizeFrame.
            Logger::Log::Warning("MetalDevice::BeginFrame: drawable acquire failed");
        }
    }
    return true;
}

void MetalDevice::SignalFrameCompletion()
{
    if (m_FrameSyncIssued)
    {
        return;
    }
    m_FrameSyncIssued = true;
    m_FrameActive = false;

    EndGpuCaptureIfActive();

    m_FrameIndex.store((m_FrameIndex.load(std::memory_order_relaxed) + 1) % kFramesInFlight,
                       std::memory_order_relaxed);
}

void MetalDevice::RecordFrameGpuTiming(MTL::CommandBuffer* buffer)
{
    if (buffer == nullptr)
        return;
    // GPUEndTime is the host-clock time (seconds) the GPU finished this buffer.
    // The delta between consecutive frame-terminal buffers is the GPU frame
    // period — analogous to the Vulkan backend's per-frame end-timestamp delta.
    const double endTime = buffer->GPUEndTime();
    if (endTime <= 0.0)
        return;
    std::lock_guard<std::mutex> lock(m_FrameTimingMutex);
    if (m_LastFrameGpuEndTime > 0.0 && endTime > m_LastFrameGpuEndTime)
        m_LastFrameSync.frameGpuPeriodMs = (endTime - m_LastFrameGpuEndTime) * 1000.0;
    m_LastFrameGpuEndTime = endTime;
}

bool MetalDevice::GetLastFrameSyncTimings(FrameSyncTimings& out) const
{
    std::lock_guard<std::mutex> lock(m_FrameTimingMutex);
    if (m_LastFrameSync.frameGpuPeriodMs <= 0.0)
        return false; // need at least two frames to compute a delta
    out = m_LastFrameSync;
    return true;
}

void MetalDevice::Present()
{
    if (!m_Initialized)
    {
        return;
    }
    MetalSwapchain* swapchain = GetSwapchain(m_ActiveWindowTarget);
    if (swapchain == nullptr || !swapchain->HasCurrentDrawable())
    {
        FinalizeFrame();
        return;
    }

    MTL::CommandBuffer* cb = m_GraphicsQueue->commandBuffer();
    if (cb == nullptr)
    {
        FinalizeFrame();
        return;
    }
    // Process-lifetime constant label — retained once instead of allocating a
    // fresh NS::String every frame.
    static NS::String* const kPresentLabel =
        NS::String::string("GE Present", NS::UTF8StringEncoding)->retain();
    cb->setLabel(kPresentLabel);
    swapchain->Present(cb);
    dispatch_semaphore_t semaphore = m_FrameSemaphore;
    cb->addCompletedHandler([this, semaphore](MTL::CommandBuffer* completed) {
        RecordFrameGpuTiming(completed);
        dispatch_semaphore_signal(semaphore);
    });
    TrackFrameCommandBuffer(cb);
    // Through the submit worker so the present cannot overtake frame work
    // still waiting on semaphores in the FIFO.
    cb->retain();
    MetalSubmitQueue::Submit submit;
    submit.Buffers.push_back(cb);
    m_GraphicsSubmitQueue.Enqueue(std::move(submit));

    SignalFrameCompletion();

    if (m_FramePool != nullptr)
    {
        m_FramePool->release();
        m_FramePool = nullptr;
    }
}

void MetalDevice::FinalizeFrame()
{
    if (!m_Initialized || m_FrameSyncIssued)
    {
        return;
    }
    MTL::CommandBuffer* cb = m_GraphicsQueue != nullptr ? m_GraphicsQueue->commandBuffer() : nullptr;
    if (cb != nullptr)
    {
        static NS::String* const kFinalizeLabel =
            NS::String::string("GE FinalizeFrame", NS::UTF8StringEncoding)->retain();
        cb->setLabel(kFinalizeLabel);
        dispatch_semaphore_t semaphore = m_FrameSemaphore;
        cb->addCompletedHandler([this, semaphore](MTL::CommandBuffer* completed) {
            RecordFrameGpuTiming(completed);
            dispatch_semaphore_signal(semaphore);
        });
        TrackFrameCommandBuffer(cb);
        cb->retain();
        MetalSubmitQueue::Submit submit;
        submit.Buffers.push_back(cb);
        m_GraphicsSubmitQueue.Enqueue(std::move(submit));
    }
    else
    {
        dispatch_semaphore_signal(m_FrameSemaphore);
    }

    SignalFrameCompletion();

    if (m_FramePool != nullptr)
    {
        m_FramePool->release();
        m_FramePool = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Window targets / swapchain
// ---------------------------------------------------------------------------

MetalSwapchain* MetalDevice::GetSwapchain(WindowTargetHandle target)
{
    if (!target.IsValid())
    {
        return nullptr;
    }
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    std::unique_ptr<MetalSwapchain>* swapchain = m_WindowTargets.Get(ToGeneric(target));
    return swapchain != nullptr ? swapchain->get() : nullptr;
}

const MetalSwapchain* MetalDevice::GetSwapchain(WindowTargetHandle target) const
{
    return const_cast<MetalDevice*>(this)->GetSwapchain(target);
}

WindowTargetHandle MetalDevice::CreateWindowTarget(void* windowHandle, uint32_t width, uint32_t height)
{
    if (!m_Initialized || windowHandle == nullptr)
    {
        return WindowTargetHandle{};
    }
    auto swapchain = std::make_unique<MetalSwapchain>();
    if (!swapchain->Initialize(*this, windowHandle, width, height, m_Desc.vsync))
    {
        return WindowTargetHandle{};
    }
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    return FromGeneric<WindowTargetTag>(m_WindowTargets.Create(std::move(swapchain)));
}

bool MetalDevice::DestroyWindowTarget(WindowTargetHandle target)
{
    WaitForIdle();
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    std::unique_ptr<MetalSwapchain>* swapchain = m_WindowTargets.Get(ToGeneric(target));
    if (swapchain == nullptr)
    {
        return false;
    }
    if (*swapchain)
    {
        (*swapchain)->Shutdown(*this);
    }
    m_WindowTargets.Destroy(ToGeneric(target));
    if (m_ActiveWindowTarget == target)
    {
        m_ActiveWindowTarget = WindowTargetHandle{};
    }
    return true;
}

bool MetalDevice::SetActiveWindowTarget(WindowTargetHandle target)
{
    if (target.IsValid() && GetSwapchain(target) == nullptr)
    {
        return false;
    }
    m_ActiveWindowTarget = target;
    if (target.IsValid() && m_StartupHdrRequest.has_value())
    {
        const MetalHdrOutputRequest request = *m_StartupHdrRequest;
        m_StartupHdrRequest.reset();
        SetHdrOutputMode(request.mode, &request.metadata, request.bitDepth);
    }
    return true;
}

bool MetalDevice::RecreateWindowTargetSwapchain(WindowTargetHandle target, uint32_t width, uint32_t height)
{
    MetalSwapchain* swapchain = GetSwapchain(target);
    return swapchain != nullptr && swapchain->Resize(*this, width, height);
}

bool MetalDevice::GetWindowTargetSize(WindowTargetHandle target, uint32_t& outWidth, uint32_t& outHeight) const
{
    const MetalSwapchain* swapchain = GetSwapchain(target);
    if (swapchain == nullptr)
    {
        outWidth = 0;
        outHeight = 0;
        return false;
    }
    outWidth = swapchain->GetWidth();
    outHeight = swapchain->GetHeight();
    return true;
}

bool MetalDevice::GetSwapchainSize(uint32_t& outWidth, uint32_t& outHeight) const
{
    return GetWindowTargetSize(m_ActiveWindowTarget, outWidth, outHeight);
}

bool MetalDevice::AcquireNextImage(uint32_t& imageIndex)
{
    MetalSwapchain* swapchain = GetSwapchain(m_ActiveWindowTarget);
    return swapchain != nullptr && swapchain->Acquire(*this, imageIndex);
}

bool MetalDevice::PresentImage(uint32_t /*imageIndex*/)
{
    Present();
    return true;
}

uint32_t MetalDevice::GetSwapchainImageCount() const
{
    return GetSwapchain(m_ActiveWindowTarget) != nullptr ? MetalSwapchain::kImageCount : 0u;
}

TextureHandle MetalDevice::GetSwapchainImage(uint32_t index) const
{
    const MetalSwapchain* swapchain = GetSwapchain(m_ActiveWindowTarget);
    return swapchain != nullptr ? swapchain->GetSlotTexture(index) : TextureHandle{};
}

TextureHandle MetalDevice::GetCurrentSwapchainImageHandle()
{
    MetalSwapchain* swapchain = GetSwapchain(m_ActiveWindowTarget);
    if (swapchain == nullptr)
    {
        return TextureHandle{};
    }
    if (!swapchain->HasCurrentDrawable())
    {
        uint32_t imageIndex = 0;
        if (!swapchain->Acquire(*this, imageIndex))
        {
            return TextureHandle{};
        }
    }
    return swapchain->GetSlotTexture(swapchain->GetCurrentSlot());
}

TextureFormat MetalDevice::GetSwapchainTextureFormat() const
{
    const MetalSwapchain* swapchain = GetSwapchain(m_ActiveWindowTarget);
    return swapchain != nullptr ? swapchain->GetFormat() : TextureFormat::BGRA8_UNORM;
}

} // namespace Rendering
} // namespace GameEngine
