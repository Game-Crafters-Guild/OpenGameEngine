/**
 * @file BindlessResourceManager.cpp
 * @brief Implementation of modern bindless resource management
 */

#include "Rendering/Core/BindlessResourceManager.h"
#include "Rendering/Core/Device.h"
#include "Logger/Logger.h"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <format>
#include <string>

namespace GameEngine::Rendering
{

/**
 * @brief Internal bindless resource tracking
 */
struct BindlessResource
{
    uint32_t descriptorIndex = 0;
    BindlessResourceType type = BindlessResourceType::Texture2D;
    std::string debugName;
    bool isValid = false;

    struct TextureResourceInfo
    {
        TextureHandle handle;
        uint32_t mipLevel;
        uint32_t arraySlice;
        TextureAspect aspect = TextureAspect::Color;
        bool isWritable;
    };

    struct BufferResourceInfo
    {
        BufferHandle handle;
        size_t offset;
        size_t size;
        uint32_t stride;
        bool isWritable;
    };

    TextureResourceInfo texture{};
    BufferResourceInfo buffer{};

    BindlessResource() = default;
};

/**
 * @brief Descriptor heap implementation
 */
struct DescriptorHeap
{
    DescriptorHeapType type = DescriptorHeapType::CBV_SRV_UAV;
    uint32_t maxDescriptors = 0;
    uint32_t usedDescriptors = 0;
    uint32_t nextFreeIndex = 1; // Start at 1, 0 is invalid
    std::vector<bool> freeSlots;
    std::string debugName;

    // Platform-specific handles
    void* nativeHeap = nullptr;

    DescriptorHeap(DescriptorHeapType heapType, uint32_t maxDescs, const char* name = nullptr)
        : type(heapType), maxDescriptors(maxDescs), debugName(name ? name : "unnamed_heap")
    {
        freeSlots.resize(maxDescriptors, true);
        freeSlots[0] = false; // Reserve index 0 as invalid
    }

    uint32_t AllocateDescriptor()
    {
        if (usedDescriptors >= maxDescriptors - 1)
        {
            Logger::Log::Error("DescriptorHeap '{}' is full ({} descriptors)", debugName, maxDescriptors);
            return 0; // Invalid index
        }

        // Find next free slot
        while (nextFreeIndex < maxDescriptors && !freeSlots[nextFreeIndex])
        {
            nextFreeIndex++;
        }

        if (nextFreeIndex >= maxDescriptors)
        {
            // Wrap around and search from beginning
            for (uint32_t i = 1; i < maxDescriptors; ++i)
            {
                if (freeSlots[i])
                {
                    nextFreeIndex = i;
                    break;
                }
            }
        }

        if (nextFreeIndex >= maxDescriptors || !freeSlots[nextFreeIndex])
        {
            Logger::Log::Error("DescriptorHeap '{}': no free descriptor found", debugName);
            return 0;
        }

        uint32_t index = nextFreeIndex;
        freeSlots[index] = false;
        usedDescriptors++;
        nextFreeIndex++;

        return index;
    }

    void FreeDescriptor(uint32_t index)
    {
        if (index == 0 || index >= maxDescriptors)
            return;

        if (freeSlots[index])
        {
            Logger::Log::Error("DescriptorHeap '{}': double free of descriptor {}", debugName, index);
            return;
        }

        freeSlots[index] = true;
        usedDescriptors--;
        nextFreeIndex = std::min(nextFreeIndex, index);
    }
};

/**
 * @brief BindlessResourceManager implementation
 */
class BindlessResourceManager::Impl
{
  public:
    explicit Impl(IDevice* device) : m_Device(device) {}

    IDevice* m_Device;
    bool m_Initialized = false;

    // Resource tracking
    std::unordered_map<BindlessTextureHandle, BindlessResource> m_BindlessTextures;
    std::unordered_map<BindlessBufferHandle, BindlessResource> m_BindlessBuffers;
    std::unordered_map<DescriptorSetHandle, std::unique_ptr<DescriptorHeap>> m_DescriptorSets;

    // Descriptor heaps for different resource types
    std::unique_ptr<DescriptorHeap> m_MainHeap;
    std::unique_ptr<DescriptorHeap> m_SamplerHeap;

    // Handle generation
    uint32_t m_NextTextureHandle = 1;
    uint32_t m_NextBufferHandle = 1;
    uint32_t m_NextDescriptorSetHandle = 1;

    // Generational free: descriptor indices freed by DestroyBindlessTexture
    // are parked here keyed by a manager-internal monotonic generation. They
    // are not returned to the heap's free pool until framesInFlight
    // generations have elapsed, so any in-flight GPU work referencing the
    // slot has drained.
    //
    // Why a manager-internal generation and not IDevice::GetFrameIndex():
    // GetFrameIndex returns the frame slot modulo MAX_FRAMES_IN_FLIGHT, so
    // it wraps (0,1,0,1,...). Using it directly makes the retire predicate
    // `frameWhenFreed + framesInFlight <= current` unsatisfiable and parked
    // entries leak forever. The manager observes the wrapped value and
    // advances its own monotonic generation whenever the slot changes.
    struct PendingFreeIndex
    {
        uint32_t descriptorIndex;
        uint64_t generationWhenFreed;
    };
    std::vector<PendingFreeIndex> m_PendingFreeTextureIndices;
    std::vector<PendingFreeIndex> m_PendingFreeBufferIndices;
    uint64_t m_GenerationCounter = 0;
    uint32_t m_LastObservedFrameSlot = UINT32_MAX;

    // Thread safety
    mutable std::mutex m_Mutex;

    // Statistics
    mutable BindlessResourceStats m_Stats;
    mutable bool m_StatsValid = false;

    void UpdateStats() const
    {
        if (m_StatsValid)
            return;

        m_Stats = {};
        m_Stats.totalBindlessTextures = static_cast<uint32_t>(m_BindlessTextures.size());
        m_Stats.totalBindlessBuffers = static_cast<uint32_t>(m_BindlessBuffers.size());
        m_Stats.activeDescriptorSets = static_cast<uint32_t>(m_DescriptorSets.size());

        // Update heap stats
        if (m_MainHeap)
        {
            auto& heapStats = m_Stats.heapStats[static_cast<int>(DescriptorHeapType::CBV_SRV_UAV)];
            heapStats.totalDescriptors = m_MainHeap->maxDescriptors;
            heapStats.usedDescriptors = m_MainHeap->usedDescriptors;
            heapStats.freeDescriptors = heapStats.totalDescriptors - heapStats.usedDescriptors;
            heapStats.memoryUsage = heapStats.totalDescriptors * 32; // Rough estimate
        }

        if (m_SamplerHeap)
        {
            auto& heapStats = m_Stats.heapStats[static_cast<int>(DescriptorHeapType::Sampler)];
            heapStats.totalDescriptors = m_SamplerHeap->maxDescriptors;
            heapStats.usedDescriptors = m_SamplerHeap->usedDescriptors;
            heapStats.freeDescriptors = heapStats.totalDescriptors - heapStats.usedDescriptors;
            heapStats.memoryUsage = heapStats.totalDescriptors * 16; // Rough estimate
        }

        // Calculate total memory usage
        for (const auto& heapStats : m_Stats.heapStats)
        {
            m_Stats.totalMemoryUsage += heapStats.memoryUsage;
        }

        m_StatsValid = true;
    }

    void InvalidateStats()
    {
        m_StatsValid = false;
    }

    // Retires parked descriptor indices older than framesInFlight generations
    // back into the heap's free pool. Caller must hold m_Mutex. Also advances
    // the manager's monotonic generation counter when the observed device
    // frame slot changes, which is the trigger for "a new frame boundary has
    // crossed." Called lazily from the allocate / destroy paths.
    void RetireExpiredFreesLocked()
    {
        if (!m_Device)
            return;
        // Advance generation on observed frame-slot change. The device returns
        // the slot modulo framesInFlight, so any inequality means we've
        // crossed a frame boundary.
        const uint32_t currentFrameSlot = m_Device->GetFrameIndex();
        if (currentFrameSlot != m_LastObservedFrameSlot)
        {
            ++m_GenerationCounter;
            m_LastObservedFrameSlot = currentFrameSlot;
        }
        const uint64_t currentGeneration = m_GenerationCounter;
        const uint32_t framesInFlight = std::max<uint32_t>(m_Device->GetFramesInFlight(), 1);

        auto retire = [&](std::vector<PendingFreeIndex>& pending, DescriptorHeap* heap)
        {
            if (!heap)
                return;
            auto newEnd = std::remove_if(pending.begin(), pending.end(),
                [&](const PendingFreeIndex& p)
                {
                    if (p.generationWhenFreed + framesInFlight <= currentGeneration)
                    {
                        heap->FreeDescriptor(p.descriptorIndex);
                        return true;
                    }
                    return false;
                });
            pending.erase(newEnd, pending.end());
        };
        retire(m_PendingFreeTextureIndices, m_MainHeap.get());
        retire(m_PendingFreeBufferIndices, m_MainHeap.get());
    }
};

namespace
{
// The provisioned capacity is clamped to the same device capability for every
// device in a process, so only the first manager to report it where a reader can
// see it says anything new. A suite that creates a device per test constructs
// hundreds of managers.
std::atomic<bool> g_CapacityUnreported{true};
} // namespace

// BindlessResourceManager public interface
BindlessResourceManager::BindlessResourceManager(IDevice* device)
    : m_Impl(std::make_unique<Impl>(device))
{
}

BindlessResourceManager::~BindlessResourceManager()
{
    if (m_Impl->m_Initialized)
    {
        Shutdown();
    }
}

bool BindlessResourceManager::Initialize(uint32_t maxBindlessTextures, uint32_t maxBindlessBuffers)
{
    std::lock_guard<std::mutex> lock(m_Impl->m_Mutex);

    if (m_Impl->m_Initialized)
    {
        Logger::Log::Debug("BindlessResourceManager: already initialized");
        return true;
    }

    // Create main descriptor heap for textures and buffers
    m_Impl->m_MainHeap = std::make_unique<DescriptorHeap>(
        DescriptorHeapType::CBV_SRV_UAV,
        maxBindlessTextures + maxBindlessBuffers,
        "main_bindless_heap");

    // Create sampler heap
    m_Impl->m_SamplerHeap = std::make_unique<DescriptorHeap>(
        DescriptorHeapType::Sampler,
        1000,
        "sampler_heap");

    // TODO: Create platform-specific descriptor heaps
    // For now, just mark as initialized

    m_Impl->m_Initialized = true;
    m_Impl->InvalidateStats();

    const std::string capacity =
        std::format("BindlessResourceManager: ready with {} texture and {} buffer descriptors",
                    maxBindlessTextures,
                    maxBindlessBuffers);
    // Spend the one Info report only where it can be seen. A host whose logger is still
    // below Info -- every test executable until some test calls Initialize -- would
    // otherwise burn it on a record the logger drops, and the capacity would be carried
    // at no level at all.
    if (Logger::Log::GetLogLevel() <= Logger::LogLevel::Info &&
        g_CapacityUnreported.exchange(false, std::memory_order_relaxed))
        Logger::Log::Info(capacity);
    else
        Logger::Log::Debug(capacity);
    return true;
}

void BindlessResourceManager::Shutdown()
{
    std::lock_guard<std::mutex> lock(m_Impl->m_Mutex);

    if (!m_Impl->m_Initialized)
        return;

    // Report stats inline (PrintResourceReport() would deadlock on m_Mutex).
    m_Impl->UpdateStats();
    Logger::Log::Debug("BindlessResourceManager: shutting down with {} textures, {} buffers, {} descriptor sets",
                       m_Impl->m_Stats.totalBindlessTextures,
                       m_Impl->m_Stats.totalBindlessBuffers,
                       m_Impl->m_Stats.activeDescriptorSets);

    // Clean up resources
    m_Impl->m_BindlessTextures.clear();
    m_Impl->m_BindlessBuffers.clear();
    m_Impl->m_DescriptorSets.clear();
    m_Impl->m_MainHeap.reset();
    m_Impl->m_SamplerHeap.reset();

    m_Impl->m_Initialized = false;
}

// Descriptor set management
DescriptorSetHandle BindlessResourceManager::CreateDescriptorSet(const BindlessDescriptorSetLayoutDesc& desc)
{
    std::lock_guard<std::mutex> lock(m_Impl->m_Mutex);

    if (!m_Impl->m_Initialized)
    {
        Logger::Log::Error("BindlessResourceManager: CreateDescriptorSet before Initialize");
        return INVALID_HANDLE; // implicit conversion to DescriptorSetHandle
    }

    DescriptorSetHandle handle = static_cast<DescriptorSetHandle>(m_Impl->m_NextDescriptorSetHandle++);

    // For now, create a simple descriptor set with default size
    // In a real implementation, this would analyze the bindings to determine size
    uint32_t descriptorCount = 1000; // Default size for bindless resources

    auto descriptorSet = std::make_unique<DescriptorHeap>(
        DescriptorHeapType::CBV_SRV_UAV,
        descriptorCount,
        desc.debugName);

    m_Impl->m_DescriptorSets[handle] = std::move(descriptorSet);
    m_Impl->InvalidateStats();

    Logger::Log::Debug("BindlessResourceManager: created descriptor set '{}' handle={}",
                       desc.debugName ? desc.debugName : "unnamed",
                       static_cast<uint64_t>(handle));

    return handle;
}

void BindlessResourceManager::DestroyDescriptorSet(DescriptorSetHandle handle)
{
    std::lock_guard<std::mutex> lock(m_Impl->m_Mutex);

    auto it = m_Impl->m_DescriptorSets.find(handle);
    if (it != m_Impl->m_DescriptorSets.end())
    {
        Logger::Log::Debug("BindlessResourceManager: destroyed descriptor set {}", static_cast<uint64_t>(handle));
        m_Impl->m_DescriptorSets.erase(it);
        m_Impl->InvalidateStats();
    }
}

// Bindless texture management
BindlessTextureHandle BindlessResourceManager::CreateBindlessTexture(const BindlessTextureDesc& desc)
{
    std::lock_guard<std::mutex> lock(m_Impl->m_Mutex);

    if (!m_Impl->m_Initialized)
    {
        Logger::Log::Error("BindlessResourceManager: CreateBindlessTexture before Initialize");
        return kInvalidBindlessTexture;
    }

    if (!desc.textureHandle.IsValid())
    {
        Logger::Log::Error("BindlessResourceManager: CreateBindlessTexture got an invalid texture handle");
        return kInvalidBindlessTexture;
    }

    // Retire any parked freed indices whose frame-drain window has elapsed, so
    // we can hand them back out without racing a still-in-flight GPU read.
    m_Impl->RetireExpiredFreesLocked();

    // Allocate descriptor index
    uint32_t descriptorIndex = m_Impl->m_MainHeap->AllocateDescriptor();
    if (descriptorIndex == 0)
    {
        Logger::Log::Error("BindlessResourceManager: no descriptor slot free for a bindless texture");
        return kInvalidBindlessTexture;
    }

    BindlessTextureHandle handle = static_cast<BindlessTextureHandle>(m_Impl->m_NextTextureHandle++);

    BindlessResource resource;
    resource.descriptorIndex = descriptorIndex;
    resource.type = desc.type;
    resource.debugName = desc.debugName ? desc.debugName : "unnamed_bindless_texture";
    resource.isValid = true;
    resource.texture.handle = desc.textureHandle;
    resource.texture.mipLevel = desc.mipLevel;
    resource.texture.arraySlice = desc.arraySlice;
    resource.texture.aspect = desc.aspect;
    resource.texture.isWritable = desc.isWritable;

    m_Impl->m_BindlessTextures[handle] = resource;
    m_Impl->InvalidateStats();

    Logger::Log::Debug("BindlessResourceManager: created bindless texture '{}' handle={} descriptor={}",
                       resource.debugName,
                       static_cast<uint32_t>(handle),
                       descriptorIndex);

    return handle;
}

void BindlessResourceManager::UpdateBindlessTexture(BindlessTextureHandle handle, const BindlessTextureDesc& desc)
{
    std::lock_guard<std::mutex> lock(m_Impl->m_Mutex);

    auto it = m_Impl->m_BindlessTextures.find(handle);
    if (it == m_Impl->m_BindlessTextures.end())
    {
        Logger::Log::Error("BindlessResourceManager: UpdateBindlessTexture got unknown handle {}",
                           static_cast<uint32_t>(handle));
        return;
    }

    auto& resource = it->second;
    resource.texture.handle = desc.textureHandle;
    resource.texture.mipLevel = desc.mipLevel;
    resource.texture.arraySlice = desc.arraySlice;
    resource.texture.aspect = desc.aspect;
    resource.texture.isWritable = desc.isWritable;

    if (desc.debugName)
    {
        resource.debugName = desc.debugName;
    }

    Logger::Log::Debug("BindlessResourceManager: updated bindless texture '{}' handle={}",
                       resource.debugName,
                       static_cast<uint32_t>(handle));
}

void BindlessResourceManager::DestroyBindlessTexture(BindlessTextureHandle handle)
{
    std::lock_guard<std::mutex> lock(m_Impl->m_Mutex);

    auto it = m_Impl->m_BindlessTextures.find(handle);
    if (it == m_Impl->m_BindlessTextures.end())
    {
        Logger::Log::Error("BindlessResourceManager: DestroyBindlessTexture got unknown handle {}",
                           static_cast<uint32_t>(handle));
        return;
    }

    const auto& resource = it->second;

    // Park the descriptor index instead of freeing it immediately. It is
    // returned to the heap's free pool only after framesInFlight generations
    // have elapsed, which guarantees any in-flight GPU work referencing the
    // slot has drained before the index can be reissued. Fixes the DB-path
    // slot-recycle race.
    //
    // Advance generation first so we tag this park with the current, post-
    // advance value; that way entries freed within the same frame as the
    // last retirement still serialise correctly.
    m_Impl->RetireExpiredFreesLocked();
    m_Impl->m_PendingFreeTextureIndices.push_back(
        { resource.descriptorIndex, m_Impl->m_GenerationCounter });

    Logger::Log::Debug("BindlessResourceManager: destroyed bindless texture '{}' handle={}",
                       resource.debugName,
                       static_cast<uint32_t>(handle));

    m_Impl->m_BindlessTextures.erase(it);
    m_Impl->InvalidateStats();
}

// Bindless buffer management
BindlessBufferHandle BindlessResourceManager::CreateBindlessBuffer(const BindlessBufferDesc& desc)
{
    std::lock_guard<std::mutex> lock(m_Impl->m_Mutex);

    if (!m_Impl->m_Initialized)
    {
        Logger::Log::Error("BindlessResourceManager: CreateBindlessBuffer before Initialize");
        return kInvalidBindlessBuffer;
    }

    if (!desc.bufferHandle.IsValid())
    {
        Logger::Log::Error("BindlessResourceManager: CreateBindlessBuffer got an invalid buffer handle");
        return kInvalidBindlessBuffer;
    }

    m_Impl->RetireExpiredFreesLocked();

    // Allocate descriptor index
    uint32_t descriptorIndex = m_Impl->m_MainHeap->AllocateDescriptor();
    if (descriptorIndex == 0)
    {
        Logger::Log::Error("BindlessResourceManager: no descriptor slot free for a bindless buffer");
        return kInvalidBindlessBuffer;
    }

    BindlessBufferHandle handle = static_cast<BindlessBufferHandle>(m_Impl->m_NextBufferHandle++);

    BindlessResource resource;
    resource.descriptorIndex = descriptorIndex;
    resource.type = desc.type;
    resource.debugName = desc.debugName ? desc.debugName : "unnamed_bindless_buffer";
    resource.isValid = true;
    resource.buffer.handle = desc.bufferHandle;
    resource.buffer.offset = desc.offset;
    resource.buffer.size = desc.size;
    resource.buffer.stride = desc.stride;
    resource.buffer.isWritable = desc.isWritable;

    m_Impl->m_BindlessBuffers[handle] = resource;
    m_Impl->InvalidateStats();

    Logger::Log::Debug("BindlessResourceManager: created bindless buffer '{}' handle={} descriptor={}",
                       resource.debugName,
                       static_cast<uint32_t>(handle),
                       descriptorIndex);

    return handle;
}

void BindlessResourceManager::UpdateBindlessBuffer(BindlessBufferHandle handle, const BindlessBufferDesc& desc)
{
    std::lock_guard<std::mutex> lock(m_Impl->m_Mutex);

    auto it = m_Impl->m_BindlessBuffers.find(handle);
    if (it == m_Impl->m_BindlessBuffers.end())
    {
        Logger::Log::Error("BindlessResourceManager: UpdateBindlessBuffer got unknown handle {}",
                           static_cast<uint32_t>(handle));
        return;
    }

    auto& resource = it->second;
    resource.buffer.handle = desc.bufferHandle;
    resource.buffer.offset = desc.offset;
    resource.buffer.size = desc.size;
    resource.buffer.stride = desc.stride;
    resource.buffer.isWritable = desc.isWritable;

    if (desc.debugName)
    {
        resource.debugName = desc.debugName;
    }

    Logger::Log::Debug("BindlessResourceManager: updated bindless buffer '{}' handle={}",
                       resource.debugName,
                       static_cast<uint32_t>(handle));
}

void BindlessResourceManager::DestroyBindlessBuffer(BindlessBufferHandle handle)
{
    std::lock_guard<std::mutex> lock(m_Impl->m_Mutex);

    auto it = m_Impl->m_BindlessBuffers.find(handle);
    if (it == m_Impl->m_BindlessBuffers.end())
    {
        Logger::Log::Error("BindlessResourceManager: DestroyBindlessBuffer got unknown handle {}",
                           static_cast<uint32_t>(handle));
        return;
    }

    const auto& resource = it->second;

    // Park the descriptor index (see DestroyBindlessTexture for rationale).
    m_Impl->RetireExpiredFreesLocked();
    m_Impl->m_PendingFreeBufferIndices.push_back(
        { resource.descriptorIndex, m_Impl->m_GenerationCounter });

    Logger::Log::Debug("BindlessResourceManager: destroyed bindless buffer '{}' handle={}",
                       resource.debugName,
                       static_cast<uint32_t>(handle));

    m_Impl->m_BindlessBuffers.erase(it);
    m_Impl->InvalidateStats();
}

// Resource binding and access
uint32_t BindlessResourceManager::GetTextureDescriptorIndex(BindlessTextureHandle handle) const
{
    std::lock_guard<std::mutex> lock(m_Impl->m_Mutex);

    auto it = m_Impl->m_BindlessTextures.find(handle);
    if (it == m_Impl->m_BindlessTextures.end())
    {
        return 0; // Invalid index
    }

    return it->second.descriptorIndex;
}

uint32_t BindlessResourceManager::GetBufferDescriptorIndex(BindlessBufferHandle handle) const
{
    std::lock_guard<std::mutex> lock(m_Impl->m_Mutex);

    auto it = m_Impl->m_BindlessBuffers.find(handle);
    if (it == m_Impl->m_BindlessBuffers.end())
    {
        return 0; // Invalid index
    }

    return it->second.descriptorIndex;
}

BindlessResourceBinding BindlessResourceManager::GetResourceBinding(BindlessTextureHandle handle) const
{
    std::lock_guard<std::mutex> lock(m_Impl->m_Mutex);

    BindlessResourceBinding binding;
    auto it = m_Impl->m_BindlessTextures.find(handle);
    if (it != m_Impl->m_BindlessTextures.end())
    {
        const auto& resource = it->second;
        binding.textureHandle = handle;
        binding.descriptorIndex = resource.descriptorIndex;
        binding.type = resource.type;
        binding.isValid = resource.isValid;
    }

    return binding;
}

BindlessResourceBinding BindlessResourceManager::GetResourceBinding(BindlessBufferHandle handle) const
{
    std::lock_guard<std::mutex> lock(m_Impl->m_Mutex);

    BindlessResourceBinding binding;
    auto it = m_Impl->m_BindlessBuffers.find(handle);
    if (it != m_Impl->m_BindlessBuffers.end())
    {
        const auto& resource = it->second;
        binding.bufferHandle = handle;
        binding.descriptorIndex = resource.descriptorIndex;
        binding.type = resource.type;
        binding.isValid = resource.isValid;
    }

    return binding;
}

// Statistics and debugging
BindlessResourceStats BindlessResourceManager::GetStats() const
{
    std::lock_guard<std::mutex> lock(m_Impl->m_Mutex);
    m_Impl->UpdateStats();
    return m_Impl->m_Stats;
}

// Info, not Debug: nothing emits this on its own schedule, so every line of it is
// something a caller asked for, and a report that answers below the level its caller
// is reading at answers nowhere.
void BindlessResourceManager::PrintResourceReport() const
{
    std::lock_guard<std::mutex> lock(m_Impl->m_Mutex);
    m_Impl->UpdateStats();

    Logger::Log::Info("BindlessResourceManager: {} textures, {} buffers, {} descriptor sets, {} KiB",
                      m_Impl->m_Stats.totalBindlessTextures,
                      m_Impl->m_Stats.totalBindlessBuffers,
                      m_Impl->m_Stats.activeDescriptorSets,
                      m_Impl->m_Stats.totalMemoryUsage / 1024);

    const char* heapNames[] = {"CBV_SRV_UAV", "Sampler", "RTV", "DSV"};
    for (int i = 0; i < 4; ++i)
    {
        const auto& heapStats = m_Impl->m_Stats.heapStats[i];
        if (heapStats.totalDescriptors > 0)
        {
            Logger::Log::Info("BindlessResourceManager: {} heap {}/{} descriptors used",
                              heapNames[i],
                              heapStats.usedDescriptors,
                              heapStats.totalDescriptors);
        }
    }
}

void BindlessResourceManager::SetResourceName(BindlessTextureHandle handle, const char* name)
{
    std::lock_guard<std::mutex> lock(m_Impl->m_Mutex);

    auto it = m_Impl->m_BindlessTextures.find(handle);
    if (it != m_Impl->m_BindlessTextures.end())
    {
        it->second.debugName = name ? name : "unnamed";
    }
}

void BindlessResourceManager::SetResourceName(BindlessBufferHandle handle, const char* name)
{
    std::lock_guard<std::mutex> lock(m_Impl->m_Mutex);

    auto it = m_Impl->m_BindlessBuffers.find(handle);
    if (it != m_Impl->m_BindlessBuffers.end())
    {
        it->second.debugName = name ? name : "unnamed";
    }
}

// Platform-specific access
void* BindlessResourceManager::GetNativeDescriptorSet(DescriptorSetHandle handle) const
{
    std::lock_guard<std::mutex> lock(m_Impl->m_Mutex);

    auto it = m_Impl->m_DescriptorSets.find(handle);
    if (it != m_Impl->m_DescriptorSets.end())
    {
        return it->second->nativeHeap;
    }

    return nullptr;
}

void* BindlessResourceManager::GetNativeDescriptorHeap(DescriptorHeapType type) const
{
    std::lock_guard<std::mutex> lock(m_Impl->m_Mutex);

    switch (type)
    {
    case DescriptorHeapType::CBV_SRV_UAV:
        return m_Impl->m_MainHeap ? m_Impl->m_MainHeap->nativeHeap : nullptr;
    case DescriptorHeapType::Sampler:
        return m_Impl->m_SamplerHeap ? m_Impl->m_SamplerHeap->nativeHeap : nullptr;
    default:
        return nullptr;
    }
}

// Factory implementation
std::unique_ptr<BindlessResourceManager> BindlessResourceFactory::Create(IDevice* device)
{
    return std::make_unique<BindlessResourceManager>(device);
}

} // namespace GameEngine::Rendering
