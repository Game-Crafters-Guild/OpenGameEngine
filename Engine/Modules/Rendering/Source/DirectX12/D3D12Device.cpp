/**
 * @file D3D12Device.cpp
 * @brief DirectX 12 device implementation
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "D3D12Device.h"
#include "Rendering/Core/TextureFormatSupportGate.h"
#include "D3D12CommandList.h"
#include "Logger/Logger.h"
#include "Rendering/Common/Utils.h"
#include <algorithm>
#include <cassert>
#include <d3dcompiler.h>
#include <fstream>
#include <iostream>
#include <mutex>
#include <utility>

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace GameEngine
{
namespace Rendering
{

// Pooled CPU-only descriptor allocator. Grows in fixed-size chunks; tracks
// free slots per chunk with a simple stack so allocation/free are O(1).
// Not shader-visible by design — callers that need GPU binding must copy
// the descriptor into the shader-visible heap (typically via the bindless
// resource manager). Thread-safe: Allocate/Free/Reset can be called from
// any thread.
struct D3D12CpuDescriptorHeapPool
{
    static constexpr uint32_t kChunkCapacity = 256;

    struct Chunk
    {
        ComPtr<ID3D12DescriptorHeap> Heap;
        D3D12_CPU_DESCRIPTOR_HANDLE Start{};
        std::vector<uint32_t> FreeSlots; // stack of free indices (LIFO)
    };

    void Initialize(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Device = device;
        m_Type = type;
        m_DescriptorSize = device ? device->GetDescriptorHandleIncrementSize(type) : 0;
    }

    void Reset()
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Chunks.clear();
        m_Device = nullptr;
        m_DescriptorSize = 0;
    }

    bool Allocate(D3D12_CPU_DESCRIPTOR_HANDLE& outHandle, uint32_t& outChunkIndex, uint32_t& outSlotIndex)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (!m_Device)
            return false;

        for (uint32_t c = 0; c < m_Chunks.size(); ++c)
        {
            if (!m_Chunks[c].FreeSlots.empty())
            {
                const uint32_t slot = m_Chunks[c].FreeSlots.back();
                m_Chunks[c].FreeSlots.pop_back();
                outHandle = m_Chunks[c].Start;
                outHandle.ptr += static_cast<SIZE_T>(slot) * m_DescriptorSize;
                outChunkIndex = c;
                outSlotIndex = slot;
                return true;
            }
        }

        // All chunks full — grow.
        Chunk chunk;
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.Type = m_Type;
        desc.NumDescriptors = kChunkCapacity;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (FAILED(m_Device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&chunk.Heap))))
            return false;
        chunk.Start = chunk.Heap->GetCPUDescriptorHandleForHeapStart();
        chunk.FreeSlots.reserve(kChunkCapacity);
        // Push indices in reverse so slot 0 is popped first.
        for (uint32_t i = kChunkCapacity; i > 1; --i)
            chunk.FreeSlots.push_back(i - 1);

        m_Chunks.push_back(std::move(chunk));
        outHandle = m_Chunks.back().Start;
        outChunkIndex = static_cast<uint32_t>(m_Chunks.size() - 1);
        outSlotIndex = 0;
        return true;
    }

    void Free(uint32_t chunkIndex, uint32_t slotIndex)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (chunkIndex < m_Chunks.size() && slotIndex < kChunkCapacity)
            m_Chunks[chunkIndex].FreeSlots.push_back(slotIndex);
    }

  private:
    mutable std::mutex m_Mutex;
    ID3D12Device* m_Device = nullptr;
    D3D12_DESCRIPTOR_HEAP_TYPE m_Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    uint32_t m_DescriptorSize = 0;
    std::vector<Chunk> m_Chunks;
};

// D3D12 resource containers for high-performance O(1) access
struct D3D12Device::D3D12ResourceContainers
{
    ResourceContainer<D3D12Buffer> buffers;
    ResourceContainer<D3D12Texture> textures;
    ResourceContainer<D3D12TextureView> textureViews;
    ResourceContainer<D3D12_CPU_DESCRIPTOR_HANDLE> samplers;
    ResourceContainer<D3D12Pipeline> pipelines;
    ResourceContainer<uint32_t> descriptorSets; // D3D12 descriptor set handles

    D3D12CpuDescriptorHeapPool srvCpuPool;

    uint32_t nextDescriptorSetHandle = 1; // Legacy compatibility for descriptor sets
};

D3D12Device::D3D12Device() : m_ResourceContainers(std::make_unique<D3D12ResourceContainers>())
{
}

D3D12Device::~D3D12Device()
{

    Shutdown();
}

bool D3D12Device::Initialize(const DeviceDesc& desc)
{

    m_ApplicationName = desc.applicationName;
    m_DebugLayerEnabled = desc.enableDebugLayer;
    m_HdrState.enabled = desc.hdrEnabled;
    m_HdrState.requestedMode = desc.hdrEnabled ? desc.hdrMode : HdrOutputMode::Off;
    m_HdrState.activeMode = HdrOutputMode::Off;
    m_HdrState.swapchainBitDepth = desc.hdrSwapchainBitDepth;
    m_HdrState.targetDisplay = desc.hdrTargetDisplay;
    m_HdrState.staticMetadata = desc.hdrStaticMetadata;

    // Respect config toggle for bindless support gating
    m_ForceDisableBindlessResources = desc.forceDisableBindlessResources;

    // Temporarily disable debug layer to avoid device hung issues during development
    // Enable debug layer if requested
    if (false && m_DebugLayerEnabled)
    { // Temporarily disabled
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&m_DebugController))))
        {
            m_DebugController->EnableDebugLayer();
        }
    }
    else
    {
    }

    // Create DXGI factory
    if (!CreateDXGIFactory())
    {
        std::cerr << "D3D12Device: Failed to create DXGI factory" << std::endl;
        return false;
    }

    // Select adapter
    if (!SelectAdapter())
    {
        std::cerr << "D3D12Device: Failed to select adapter" << std::endl;
        return false;
    }

    // Create D3D12 device
    if (!CreateDevice())
    {
        std::cerr << "D3D12Device: Failed to create device" << std::endl;
        return false;
    }

    // Create command queue
    if (!CreateCommandQueue())
    {
        std::cerr << "D3D12Device: Failed to create command queue" << std::endl;
        return false;
    }

    // Create descriptor heaps
    if (!CreateDescriptorHeaps())
    {
        std::cerr << "D3D12Device: Failed to create descriptor heaps" << std::endl;
        return false;
    }

    // Create synchronization objects
    if (!CreateSynchronization())
    {
        std::cerr << "D3D12Device: Failed to create synchronization objects" << std::endl;
        return false;
    }

    // Query device capabilities
    QueryDeviceCapabilities();

    // Print device info
    DXGI_ADAPTER_DESC1 adapterDesc;
    m_Adapter->GetDesc1(&adapterDesc);

    // Convert wide string to narrow string for display
    char deviceName[256];
    wcstombs_s(nullptr, deviceName, sizeof(deviceName), adapterDesc.Description, _TRUNCATE);

    return true;
}

void D3D12Device::Shutdown()
{
    if (!m_Device)
        return;

    // Wait for GPU to finish
    WaitForIdle();

    // Let subsystems drop their per-device caches while the objects those handles
    // name are still alive, before the backend tears down D3D12 objects.
    InvokePerDeviceCacheCleanups();

    // Clean up resources
    CleanupD3D12();
}

bool D3D12Device::CreateDXGIFactory()
{
    UINT dxgiFactoryFlags = 0;

    if (m_DebugLayerEnabled)
    {
        dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
    }

    HRESULT hr = CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&m_DxgiFactory));
    return SUCCEEDED(hr);
}

bool D3D12Device::SelectAdapter()
{
    ComPtr<IDXGIAdapter1> adapter;

    // Try to find a hardware adapter
    for (UINT adapterIndex = 0;
         DXGI_ERROR_NOT_FOUND != m_DxgiFactory->EnumAdapters1(adapterIndex, &adapter);
         ++adapterIndex)
    {

        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);

        // Skip software adapters
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
        {
            continue;
        }

        // Try to create a device with this adapter
        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, _uuidof(ID3D12Device), nullptr)))
        {
            m_Adapter = adapter;
            return true;
        }
    }

    return false;
}

bool D3D12Device::CreateDevice()
{
    HRESULT hr = D3D12CreateDevice(
        m_Adapter.Get(),
        D3D_FEATURE_LEVEL_11_0,
        IID_PPV_ARGS(&m_Device));

    if (FAILED(hr))
    {
        return false;
    }

    // Set debug name
    if (m_DebugLayerEnabled)
    {
        std::wstring wideName(m_ApplicationName.begin(), m_ApplicationName.end());
        m_Device->SetName(wideName.c_str());
    }

    return true;
}

bool D3D12Device::CreateCommandQueue()
{
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

    HRESULT hr = m_Device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_CommandQueue));
    if (FAILED(hr))
    {
        return false;
    }

    // Create command allocator
    hr = m_Device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(&m_CommandAllocator));

    return SUCCEEDED(hr);
}

bool D3D12Device::CreateDescriptorHeaps()
{
    // Create RTV descriptor heap
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = 8; // Enough for swapchain + render targets
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

    HRESULT hr = m_Device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_RtvHeap));
    if (FAILED(hr))
    {
        std::cerr << "Failed to create RTV descriptor heap! HRESULT: " << std::hex << hr << std::endl;
        return false;
    }

    m_RtvDescriptorSize = m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    // Create DSV descriptor heap
    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
    dsvHeapDesc.NumDescriptors = 4; // Enough for depth targets
    dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

    hr = m_Device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&m_DsvHeap));
    if (FAILED(hr))
    {
        std::cerr << "Failed to create DSV descriptor heap! HRESULT: " << std::hex << hr << std::endl;
        return false;
    }

    m_DsvDescriptorSize = m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

    // CPU-only SRV/CBV/UAV descriptor pool that backs CreateTextureView.
    m_ResourceContainers->srvCpuPool.Initialize(m_Device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    return true;
}

bool D3D12Device::CreateSynchronization()
{
    // Create fence
    HRESULT hr = m_Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_Fence));
    if (FAILED(hr))
    {
        std::cerr << "Failed to create fence! HRESULT: " << std::hex << hr << std::endl;
        return false;
    }

    // Create fence event
    m_FenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (m_FenceEvent == nullptr)
    {
        std::cerr << "Failed to create fence event!" << std::endl;
        return false;
    }

    return true;
}

void D3D12Device::QueryDeviceCapabilities()
{
    // Get adapter description
    DXGI_ADAPTER_DESC1 adapterDesc;
    m_Adapter->GetDesc1(&adapterDesc);

    // Fill capabilities
    m_Capabilities.dedicatedVideoMemory = adapterDesc.DedicatedVideoMemory;
    m_Capabilities.supportsRayTracing = false;  // Check for DXR support
    m_Capabilities.supportsMeshShaders = false; // Check for mesh shader support
    // Bindless/resource binding capability gating
    m_Capabilities.supportsBindlessResources = false;
    m_Capabilities.maxBindlessTextures = 0;

    // memoryTopology is left DISENGAGED on purpose, and that is the honest
    // answer rather than a placeholder: filling it with zeroes would tell a
    // residency gate this adapter has no device-local memory, which is a
    // stronger and falser claim than "this backend has not been taught to
    // look". DedicatedVideoMemory above is an adapter-level number, not a heap
    // walk, so it cannot stand in for the fields.
    //
    // Populating it needs three D3D12 queries this backend does not yet make:
    // D3D12_FEATURE_DATA_ARCHITECTURE.UMA for isUnifiedMemory,
    // D3D12_FEATURE_DATA_D3D12_OPTIONS16.GPUUploadHeapSupported plus
    // IDXGIAdapter3::QueryVideoMemoryInfo(DXGI_MEMORY_SEGMENT_GROUP_LOCAL) for
    // largestHostVisibleDeviceLocalHeapBytes, and the same segment's Budget for
    // deviceLocalHeapBytesTotal. Whoever adds them must verify on real
    // hardware, not by symmetry with the Vulkan path.

    // Check for additional features
    D3D12_FEATURE_DATA_D3D12_OPTIONS options = {};
    if (SUCCEEDED(m_Device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options))))
    {
        // Consider bindless viable on ResourceBindingTier >= 2 (descriptor tables, large heaps)
        if (options.ResourceBindingTier >= D3D12_RESOURCE_BINDING_TIER_2)
        {
            m_Capabilities.supportsBindlessResources = true;
            m_Capabilities.maxBindlessTextures = 1000000; // Policy ceiling; actual chosen heap size may be lower
        }
    }
    if (m_ForceDisableBindlessResources)
    {
        m_Capabilities.supportsBindlessResources = false;
        m_Capabilities.maxBindlessTextures = 0;
        m_Capabilities.maxBindlessBuffers = 0;
    }

    // D3D12 does not currently expose a direct analogue of Vulkan's
    // minStorageBufferOffsetAlignment through this abstraction layer. Leave the
    // alignment at the default (0) to signal "no additional constraint" to
    // higher-level systems that compute element-based alignments.
}

void D3D12Device::CleanupD3D12()
{
    // Release swapchain resources first
    DestroySwapchain();

    // Clear resource containers
    m_ResourceContainers->buffers.Clear();
    m_ResourceContainers->textureViews.Clear();
    m_ResourceContainers->textures.Clear();
    m_ResourceContainers->samplers.Clear();
    m_ResourceContainers->pipelines.Clear();
    m_ResourceContainers->srvCpuPool.Reset();

    // Release COM objects (smart pointers will handle this automatically)
    m_CommandAllocator.Reset();
    m_CommandQueue.Reset();
    m_Device.Reset();
    m_Adapter.Reset();
    m_DxgiFactory.Reset();
    m_DebugController.Reset();
}

// Resource creation methods

D3D12_HEAP_PROPERTIES heapProps = {};
heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

D3D12_RESOURCE_DESC resourceDesc = {};
resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
resourceDesc.Alignment = 0;
resourceDesc.Width = 256;  // Default texture width
resourceDesc.Height = 256; // Default texture height
resourceDesc.DepthOrArraySize = 1;
resourceDesc.MipLevels = 1;
resourceDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; // Default format
resourceDesc.SampleDesc.Count = 1;
resourceDesc.SampleDesc.Quality = 0;
resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
resourceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

D3D12_CLEAR_VALUE clearValue = {};
clearValue.Format = resourceDesc.Format;
clearValue.Color[0] = 0.0f;
clearValue.Color[1] = 0.0f;
clearValue.Color[2] = 0.0f;
clearValue.Color[3] = 1.0f;

ComPtr<ID3D12Resource> resource;
HRESULT hr = m_Device->CreateCommittedResource(
    &heapProps,
    D3D12_HEAP_FLAG_NONE,
    &resourceDesc,
    D3D12_RESOURCE_STATE_RENDER_TARGET,
    &clearValue,
    IID_PPV_ARGS(&resource));

if (FAILED(hr))
{
    std::cerr << "D3D12Device: Failed to create texture" << std::endl;
    return INVALID_HANDLE;
}

D3D12Texture texture{};
texture.resource = resource;
texture.format = resourceDesc.Format;
texture.width = static_cast<uint32_t>(resourceDesc.Width);
texture.height = resourceDesc.Height;
texture.depth = 1;
texture.arrayLayers = 1;
texture.usage = TextureUsage::RenderTarget;
texture.debugName = "default_texture";
TextureHandle handle = m_ResourceContainers->textures.Create(std::move(texture));

return handle;
}

SamplerHandle D3D12Device::CreateSampler()
{

    // Create a default descriptor handle
    D3D12_CPU_DESCRIPTOR_HANDLE defaultHandle = {};
    SamplerHandle handle = m_ResourceContainers->samplers.Create(defaultHandle);

    // Create real D3D12 sampler descriptor
    D3D12_SAMPLER_DESC samplerDesc = {};
    samplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplerDesc.MipLODBias = 0.0f;
    samplerDesc.MaxAnisotropy = 16;
    samplerDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    samplerDesc.BorderColor[0] = 0.0f;
    samplerDesc.BorderColor[1] = 0.0f;
    samplerDesc.BorderColor[2] = 0.0f;
    samplerDesc.BorderColor[3] = 0.0f;
    samplerDesc.MinLOD = 0.0f;
    samplerDesc.MaxLOD = D3D12_FLOAT32_MAX;

    // Get CPU descriptor handle from sampler heap
    D3D12_CPU_DESCRIPTOR_HANDLE samplerHandle = m_SamplerHeap->GetCPUDescriptorHandleForHeapStart();
    samplerHandle.ptr += handle.Index() * m_SamplerDescriptorSize;

    // Create the sampler descriptor
    m_Device->CreateSampler(&samplerDesc, samplerHandle);

    // Store the descriptor handle in the container after creation
    m_ResourceContainers->samplers.Get(handle) = samplerHandle;

    return handle;
}

// Helper functions to convert our enums to DirectX 12 enums
D3D12_CULL_MODE ConvertCullModeD3D12(CullModeFlags cullMode)
{
    switch (cullMode)
    {
    case CullModeFlagBits::None:
        return D3D12_CULL_MODE_NONE;
    case CullModeFlagBits::Front:
        return D3D12_CULL_MODE_FRONT;
    case CullModeFlagBits::Back:
        return D3D12_CULL_MODE_BACK;
    case CullModeFlagBits::FrontAndBack:
        return D3D12_CULL_MODE_NONE; // D3D12 doesn't have front+back, use none
    default:
        return D3D12_CULL_MODE_NONE;
    }
}

BOOL ConvertFrontFaceD3D12(FrontFace frontFace)
{
    switch (frontFace)
    {
    case FrontFace::CounterClockwise:
        return TRUE;
    case FrontFace::Clockwise:
        return FALSE;
    default:
        return TRUE;
    }
}

D3D12_FILL_MODE ConvertPolygonModeD3D12(PolygonMode polygonMode)
{
    switch (polygonMode)
    {
    case PolygonMode::Fill:
        return D3D12_FILL_MODE_SOLID;
    case PolygonMode::Line:
        return D3D12_FILL_MODE_WIREFRAME;
    case PolygonMode::Point:
        return D3D12_FILL_MODE_SOLID; // D3D12 doesn't have point mode
    default:
        return D3D12_FILL_MODE_SOLID;
    }
}

D3D12_COMPARISON_FUNC ConvertCompareOpD3D12(CompareOp compareOp)
{
    switch (compareOp)
    {
    case CompareOp::Never:
        return D3D12_COMPARISON_FUNC_NEVER;
    case CompareOp::Less:
        return D3D12_COMPARISON_FUNC_LESS;
    case CompareOp::Equal:
        return D3D12_COMPARISON_FUNC_EQUAL;
    case CompareOp::LessOrEqual:
        return D3D12_COMPARISON_FUNC_LESS_EQUAL;
    case CompareOp::Greater:
        return D3D12_COMPARISON_FUNC_GREATER;
    case CompareOp::NotEqual:
        return D3D12_COMPARISON_FUNC_NOT_EQUAL;
    case CompareOp::GreaterOrEqual:
        return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    case CompareOp::Always:
        return D3D12_COMPARISON_FUNC_ALWAYS;
    default:
        return D3D12_COMPARISON_FUNC_GREATER;  // reverse-Z fallback
    }
}

PipelineHandle D3D12Device::CreateConcreteGraphicsPipeline(
    const GraphicsPipelineDesc& /*gd*/, const PipelineFormatKey& /*fk*/)
{
    // The D3D12 backend has no production users today and was never wired
    // through the new id-based path. Return invalid; callers handle nulls.
    return INVALID_HANDLE;
}

PipelineHandle D3D12Device::CreateConcreteComputePipeline(const ComputePipelineDesc& /*cd*/)
{
    return INVALID_HANDLE;
}

// Resource destruction methods
void D3D12Device::DestroyBuffer(BufferHandle handle)
{

    m_ResourceContainers->buffers.Destroy(handle);
}

void D3D12Device::DestroyTexture(TextureHandle handle)
{

    m_ResourceContainers->textures.Destroy(handle);
}

void D3D12Device::DestroyTextureView(TextureViewHandle handle)
{
    if (const D3D12TextureView* view = m_ResourceContainers->textureViews.TryGet(handle))
    {
        m_ResourceContainers->srvCpuPool.Free(view->ChunkIndex, view->SlotIndex);
    }
    m_ResourceContainers->textureViews.Destroy(handle);
}

void D3D12Device::DestroySampler(SamplerHandle handle)
{

    m_ResourceContainers->samplers.Destroy(handle);
}

void D3D12Device::DestroyPipeline(PipelineHandle handle)
{

    m_ResourceContainers->pipelines.Destroy(handle);
}

// Resource access methods
void* D3D12Device::MapBuffer(BufferHandle handle)
{

    D3D12Buffer* buffer = m_ResourceContainers->buffers.TryGet(handle);
    if (!buffer)
    {
        return nullptr;
    }

    void* mappedData = nullptr;
    HRESULT hr = buffer->resource->Map(0, nullptr, &mappedData);
    if (FAILED(hr))
    {
        std::cerr << "D3D12Device: Failed to map buffer" << std::endl;
        return nullptr;
    }

    return mappedData;
}

void D3D12Device::UnmapBuffer(BufferHandle handle)
{

    D3D12Buffer* buffer = m_ResourceContainers->buffers.TryGet(handle);
    if (buffer)
    {
        buffer->resource->Unmap(0, nullptr);
    }
}

void D3D12Device::UpdateBuffer(BufferHandle handle, size_t offset, size_t size, const void* data)
{
    if (!data)
    {
        Logger::Log::Error("D3D12Device::UpdateBuffer: data is null");
        return;
    }

    // A zero-byte update is a no-op request, not an error.
    if (size == 0)
    {
        return;
    }

    D3D12Buffer* buffer = m_ResourceContainers->buffers.TryGet(handle);
    if (!buffer)
    {
        Logger::Log::Error("D3D12Device::UpdateBuffer: invalid buffer handle {}", handle.id);
        return;
    }

    // Reject writes that would run past the allocation — Map() exposes the
    // whole resource and the memcpy below trusts offset/size blindly.
    // (Overflow-safe form: offset + size can wrap.)
    if (size > buffer->size || offset > buffer->size - size)
    {
        Logger::Log::Error(
            "D3D12Device::UpdateBuffer: write of {} bytes at offset {} exceeds buffer '{}' ({} bytes); rejected",
            size, offset, buffer->debugName, buffer->size);
        return;
    }

    // Only CPU-accessible heaps can be written through Map(). A DEFAULT-heap
    // (device-local) target needs a staging copy, which this backend does not
    // implement — dropping the upload silently would leave the buffer holding
    // whatever it held before.
    if (!buffer->cpuMappable)
    {
        Logger::Log::Error(
            "D3D12Device::UpdateBuffer: buffer '{}' lives in a device-local heap; the D3D12 "
            "backend has no staging upload path. Create it with BufferMemoryUsage::Upload.",
            buffer->debugName);
        return;
    }

    void* mappedData = nullptr;
    D3D12_RANGE readRange = {0, 0}; // Write-only access: no CPU read of prior contents.
    HRESULT hr = buffer->resource->Map(0, &readRange, &mappedData);
    if (FAILED(hr) || !mappedData)
    {
        Logger::Log::Error("D3D12Device::UpdateBuffer: failed to map buffer '{}' (HRESULT 0x{:08X})",
                           buffer->debugName, static_cast<uint32_t>(hr));
        return;
    }

    memcpy(static_cast<char*>(mappedData) + offset, data, size);
    buffer->resource->Unmap(0, nullptr);
}

void D3D12Device::ExecuteCommandLists(const std::vector<CommandList*>& commandLists)
{

    if (commandLists.empty())
    {
        return;
    }

    // Convert to DirectX 12 command lists
    std::vector<ID3D12CommandList*> d3d12CommandLists;
    d3d12CommandLists.reserve(commandLists.size());

    for (CommandList* cmdList : commandLists)
    {
        D3D12CommandList* d3d12CmdList = static_cast<D3D12CommandList*>(cmdList);
        if (d3d12CmdList && d3d12CmdList->GetD3D12CommandList())
        {
            // Close the command list before execution
            d3d12CmdList->GetD3D12CommandList()->Close();
            d3d12CommandLists.push_back(d3d12CmdList->GetD3D12CommandList());
        }
    }

    if (!d3d12CommandLists.empty())
    {
        // Submit command lists to the command queue
        m_CommandQueue->ExecuteCommandLists(
            static_cast<UINT>(d3d12CommandLists.size()),
            d3d12CommandLists.data());

        // Signal fence for synchronization
        m_FenceValue++;
        m_CommandQueue->Signal(m_Fence.Get(), m_FenceValue);

        // Wait for GPU to complete
        if (m_Fence->GetCompletedValue() < m_FenceValue)
        {
            m_Fence->SetEventOnCompletion(m_FenceValue, m_FenceEvent);
            WaitForSingleObject(m_FenceEvent, INFINITE);
        }

        // Note: Command allocators are managed individually by each D3D12CommandList
        // No need to reset the device's shared command allocator here
    }
}

// Synchronization
void D3D12Device::WaitForIdle()
{

    if (m_Fence && m_CommandQueue)
    {
        // Signal fence with incremented value
        m_FenceValue++;
        HRESULT hr = m_CommandQueue->Signal(m_Fence.Get(), m_FenceValue);
        if (FAILED(hr))
        {
            std::cerr << "D3D12Device: Failed to signal fence for WaitForIdle! HRESULT: " << std::hex << hr << std::endl;
            return;
        }

        // Wait for fence to reach the signaled value
        if (m_Fence->GetCompletedValue() < m_FenceValue)
        {
            hr = m_Fence->SetEventOnCompletion(m_FenceValue, m_FenceEvent);
            if (FAILED(hr))
            {
                std::cerr << "D3D12Device: Failed to set fence event! HRESULT: " << std::hex << hr << std::endl;
                return;
            }
            WaitForSingleObject(m_FenceEvent, INFINITE);
        }
    }
    else
    {
        std::cerr << "D3D12Device: Cannot wait for idle - fence or command queue not available" << std::endl;
    }
}

bool D3D12Device::BeginFrame()
{

    // D3D12 doesn't need explicit frame begin like Vulkan
    // Swapchain images are acquired during present
    return true;
}

void D3D12Device::Present()
{
    if (m_Swapchain)
    {
        // Present the current back buffer
        HRESULT hr = m_Swapchain->Present(1, 0); // VSync enabled
        if (SUCCEEDED(hr))
        {
        }
        else
        {
            std::cerr << "D3D12Device: Failed to present swapchain! HRESULT: " << std::hex << hr << std::endl;
        }
    }
    else
    {
    }
}

// Resource management
ResourceManager* D3D12Device::GetResourceManager()
{
    // Create ResourceManager instance if not already created
    if (!m_ResourceManager)
    {
        m_ResourceManager = std::make_unique<ResourceManager>(this);
    }
    return m_ResourceManager.get();
}

IQueryPool* D3D12Device::GetQueryPool()
{
    // DirectX 12 QueryPool implementation is deferred per the modernization roadmap
    // The IQueryPool interface is designed and ready for D3D12 implementation
    return nullptr;
}

TextureHandle D3D12Device::GetCurrentSwapchainImageHandle()
{
    if (!m_Swapchain || m_CurrentBackBufferIndex >= m_SwapchainBuffers.size())
    {
        std::cerr << "D3D12Device: Invalid swapchain or buffer index for GetCurrentSwapchainImageHandle" << std::endl;
        return INVALID_HANDLE;
    }

    // Use a special range for swapchain image handles to avoid conflicts
    // Swapchain handles start at 1000000 to avoid conflicts with regular textures
    TextureHandle swapchainHandle = TextureHandle(1000000 + m_CurrentBackBufferIndex, 1);

    // For swapchain textures, we use a special handle range that doesn't go through ResourceContainer
    // This is a simplified approach - in a full implementation, we'd want to integrate this better
    // For now, we'll just return the handle and let the command list handle the special case

    return swapchainHandle;
}

// Debug events
void D3D12Device::BeginEvent(const char* name)
{
}

void D3D12Device::EndEvent()
{
}

// Window presentation abstraction implementations for D3D12 backend
WindowTargetHandle D3D12Device::CreateWindowTarget(void* windowHandle, uint32_t width, uint32_t height)
{
    if (!CreateSwapchain(windowHandle, width, height))
    {
        return WindowTargetHandle{};
    }
    m_WindowHandle = windowHandle;
    m_WindowTarget = WindowTargetHandle(1, 1);
    m_WindowTargetActive = true;
    return m_WindowTarget;
}

bool D3D12Device::DestroyWindowTarget(WindowTargetHandle target)
{
    if (!target.IsValid() || !m_WindowTarget.IsValid() || target.id != m_WindowTarget.id)
    {
        return false;
    }
    DestroySwapchain();
    m_WindowTarget = {};
    m_WindowTargetActive = false;
    m_WindowHandle = nullptr;
    return true;
}

bool D3D12Device::SetActiveWindowTarget(WindowTargetHandle target)
{
    if (!target.IsValid() || !m_WindowTarget.IsValid() || target.id != m_WindowTarget.id)
    {
        return false;
    }
    m_WindowTargetActive = true;
    return true;
}

WindowTargetHandle D3D12Device::GetActiveWindowTarget() const
{
    if (!m_WindowTargetActive || !m_WindowTarget.IsValid())
    {
        return WindowTargetHandle{};
    }
    return m_WindowTarget;
}

bool D3D12Device::RecreateWindowTargetSwapchain(WindowTargetHandle target, uint32_t width, uint32_t height)
{
    if (!SetActiveWindowTarget(target))
    {
        return false;
    }
    if (!m_WindowHandle)
    {
        return false;
    }
    return CreateSwapchain(m_WindowHandle, width, height);
}

bool D3D12Device::SetHdrOutputMode(HdrOutputMode mode, const HdrStaticMetadata* metadata, HdrSwapchainBitDepth bitDepth)
{
    const bool enabled = mode != HdrOutputMode::Off;
    const bool changed = m_HdrState.enabled != enabled || m_HdrState.requestedMode != mode ||
                         m_HdrState.swapchainBitDepth != bitDepth ||
                         (metadata != nullptr);
    m_HdrState.enabled = enabled;
    m_HdrState.requestedMode = mode;
    m_HdrState.swapchainBitDepth = bitDepth;
    if (metadata)
        m_HdrState.staticMetadata = *metadata;
    if (!changed)
        return true;
    if (m_WindowTargetActive && m_WindowTarget.IsValid())
        return RecreateWindowTargetSwapchain(m_WindowTarget, m_SwapchainWidth, m_SwapchainHeight);
    return true;
}

bool D3D12Device::GetWindowTargetSize(WindowTargetHandle target, uint32_t& outWidth, uint32_t& outHeight) const
{
    if (!target.IsValid() || !m_WindowTarget.IsValid() || target.id != m_WindowTarget.id)
    {
        outWidth = 0;
        outHeight = 0;
        return false;
    }
    return GetSwapchainSize(outWidth, outHeight);
}

TextureHandle D3D12Device::GetSwapchainImage(uint32_t index) const
{
    if (index < m_SwapchainBuffers.size())
    {
        // Return handle to swapchain image
        return TextureHandle(1000000 + index, 1); // Special range for swapchain images
    }
    return TextureHandle{}; // Invalid handle
}

TextureFormat D3D12Device::GetSwapchainTextureFormat() const
{
    switch (m_SwapchainFormat)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
        return TextureFormat::RGBA8_UNORM;
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return TextureFormat::RGBA8_SRGB;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
        return TextureFormat::BGRA8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return TextureFormat::BGRA8_SRGB;
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return TextureFormat::RGB10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return TextureFormat::R16G16B16A16_FLOAT;
    default:
        return TextureFormat::Unknown;
    }
}

void D3D12Device::SetMarker(const char* name)
{
}

// GPU timeline profiling removed - use external profiling tools

// Indirect command support removed - moved to CommandList interface

// Swapchain implementation
bool D3D12Device::CreateSwapchain(void* windowHandle, uint32_t width, uint32_t height)
{

    if (!m_DxgiFactory || !m_CommandQueue)
    {
        std::cerr << "D3D12Device: Cannot create swapchain without DXGI factory and command queue" << std::endl;
        return false;
    }

    if (!windowHandle)
    {
        std::cerr << "D3D12Device: Invalid window handle provided" << std::endl;
        return false;
    }

    // Destroy existing swapchain if any
    DestroySwapchain();

    m_SwapchainFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    m_SwapchainColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    if (m_HdrState.enabled)
    {
        if (m_HdrState.swapchainBitDepth == HdrSwapchainBitDepth::Float16 ||
            m_HdrState.requestedMode == HdrOutputMode::ScRGB)
        {
            m_SwapchainFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
            m_SwapchainColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
        }
        else if (m_HdrState.requestedMode == HdrOutputMode::HDR10_PQ ||
                 m_HdrState.requestedMode == HdrOutputMode::HDR10Plus ||
                 m_HdrState.requestedMode == HdrOutputMode::Auto)
        {
            m_SwapchainFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
            m_SwapchainColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
        }
    }

    // Get HWND from GLFW window
    HWND hwnd = glfwGetWin32Window(static_cast<GLFWwindow*>(windowHandle));
    if (!hwnd)
    {
        std::cerr << "D3D12Device: Failed to get Win32 window handle from GLFW" << std::endl;
        return false;
    }

    // Describe the swapchain
    DXGI_SWAP_CHAIN_DESC1 swapchainDesc = {};
    swapchainDesc.Width = width;
    swapchainDesc.Height = height;
    swapchainDesc.Format = m_SwapchainFormat;
    swapchainDesc.Stereo = FALSE;
    swapchainDesc.SampleDesc.Count = 1;
    swapchainDesc.SampleDesc.Quality = 0;
    swapchainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapchainDesc.BufferCount = 3; // Triple buffering
    swapchainDesc.Scaling = DXGI_SCALING_STRETCH;
    swapchainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapchainDesc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    swapchainDesc.Flags = 0;

    // Create swapchain
    ComPtr<IDXGISwapChain1> swapchain1;
    HRESULT hr = m_DxgiFactory->CreateSwapChainForHwnd(
        m_CommandQueue.Get(),
        hwnd,
        &swapchainDesc,
        nullptr,
        nullptr,
        &swapchain1);

    if (FAILED(hr))
    {
        std::cerr << "D3D12Device: Failed to create swapchain! HRESULT: " << std::hex << hr << std::endl;
        return false;
    }

    // Query for IDXGISwapChain3 interface
    hr = swapchain1.As(&m_Swapchain);
    if (FAILED(hr))
    {
        std::cerr << "D3D12Device: Failed to query IDXGISwapChain3 interface!" << std::endl;
        return false;
    }

    // Disable Alt+Enter fullscreen toggle
    m_DxgiFactory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

    // Get swapchain buffer count and current back buffer index
    m_SwapchainImageCount = swapchainDesc.BufferCount;
    m_CurrentBackBufferIndex = m_Swapchain->GetCurrentBackBufferIndex();

    // Cache current swapchain dimensions for viewport/layout queries
    m_SwapchainWidth = width;
    m_SwapchainHeight = height;
    m_HdrState.activeMode = HdrOutputMode::Off;
    m_HdrState.display = {};
    m_HdrState.display.requestedMode = m_HdrState.requestedMode;
    m_HdrState.display.swapchainFormat = GetSwapchainTextureFormat();
    m_HdrState.display.swapchainBitDepth = m_HdrState.swapchainBitDepth;
    m_HdrState.display.width = width;
    m_HdrState.display.height = height;
    UINT colorSpaceSupport = 0;
    if (m_HdrState.enabled && SUCCEEDED(m_Swapchain->CheckColorSpaceSupport(m_SwapchainColorSpace, &colorSpaceSupport)) &&
        (colorSpaceSupport & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT))
    {
        if (SUCCEEDED(m_Swapchain->SetColorSpace1(m_SwapchainColorSpace)))
        {
            if (m_SwapchainColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020)
            {
                m_HdrState.activeMode = HdrOutputMode::HDR10_PQ;
                m_HdrState.display.supportsHDR10_PQ = true;
            }
            else if (m_SwapchainColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709)
            {
                m_HdrState.activeMode = HdrOutputMode::ScRGB;
                m_HdrState.display.supportsScRGB = true;
            }
            m_HdrState.display.hdrAvailable = IsHdrOutputModeActive(m_HdrState.activeMode);
            m_HdrState.display.hdrActive = m_HdrState.display.hdrAvailable;
            m_HdrState.display.resolvedMode = m_HdrState.activeMode;
            m_HdrState.display.outputMaxLinearValue = GetHdrOutputMaxLinearValue(m_HdrState);
        }
    }
    else if (m_HdrState.enabled)
    {
        m_HdrState.display.diagnosticHints.push_back("DXGI swapchain color space is not supported for this output.");
    }
    if (!m_HdrState.display.hdrActive)
        m_HdrState.display.outputMaxLinearValue = 1.0f;

    if (m_HdrState.activeMode == HdrOutputMode::HDR10_PQ)
    {
        ComPtr<IDXGISwapChain4> swapchain4;
        if (SUCCEEDED(m_Swapchain.As(&swapchain4)))
        {
            const HdrStaticMetadata& src = m_HdrState.staticMetadata;
            auto chroma = [](float value) -> UINT16 {
                return static_cast<UINT16>(std::clamp(value, 0.0f, 1.0f) * 50000.0f + 0.5f);
            };
            DXGI_HDR_METADATA_HDR10 metadata{};
            metadata.RedPrimary[0] = chroma(src.redPrimary[0]);
            metadata.RedPrimary[1] = chroma(src.redPrimary[1]);
            metadata.GreenPrimary[0] = chroma(src.greenPrimary[0]);
            metadata.GreenPrimary[1] = chroma(src.greenPrimary[1]);
            metadata.BluePrimary[0] = chroma(src.bluePrimary[0]);
            metadata.BluePrimary[1] = chroma(src.bluePrimary[1]);
            metadata.WhitePoint[0] = chroma(src.whitePoint[0]);
            metadata.WhitePoint[1] = chroma(src.whitePoint[1]);
            metadata.MaxMasteringLuminance = static_cast<UINT>(std::max(0.0f, src.maxMasteringLuminance));
            metadata.MinMasteringLuminance = static_cast<UINT>(std::max(0.0f, src.minMasteringLuminance) * 10000.0f);
            metadata.MaxContentLightLevel = static_cast<UINT16>(std::max(0.0f, src.maxContentLightLevel));
            metadata.MaxFrameAverageLightLevel = static_cast<UINT16>(std::max(0.0f, src.maxFrameAverageLightLevel));
            swapchain4->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_HDR10, sizeof(metadata), &metadata);
            m_HdrState.display.diagnosticHints.push_back("Static HDR10 metadata applied with IDXGISwapChain4::SetHDRMetaData.");
        }
    }

    // Get swapchain buffers and create RTVs
    m_SwapchainBuffers.resize(m_SwapchainImageCount);
    m_SwapchainRTVs.resize(m_SwapchainImageCount);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_RtvHeap->GetCPUDescriptorHandleForHeapStart();

    for (uint32_t i = 0; i < m_SwapchainImageCount; i++)
    {
        hr = m_Swapchain->GetBuffer(i, IID_PPV_ARGS(&m_SwapchainBuffers[i]));
        if (FAILED(hr))
        {
            std::cerr << "D3D12Device: Failed to get swapchain buffer " << i << "!" << std::endl;
            return false;
        }

        // Set debug name
        std::wstring name = L"SwapchainBuffer" + std::to_wstring(i);
        m_SwapchainBuffers[i]->SetName(name.c_str());

        // Create RTV for this swapchain buffer
        m_Device->CreateRenderTargetView(m_SwapchainBuffers[i].Get(), nullptr, rtvHandle);
        m_SwapchainRTVs[i] = rtvHandle;

        // Move to next RTV descriptor
        rtvHandle.ptr += m_RtvDescriptorSize;
    }

    return true;
}

void D3D12Device::DestroySwapchain()
{
    if (m_Swapchain)
    {

        // Release swapchain buffers and RTVs
        for (auto& buffer : m_SwapchainBuffers)
        {
            buffer.Reset();
        }
        m_SwapchainBuffers.clear();
        m_SwapchainRTVs.clear();

        // Release swapchain
        m_Swapchain.Reset();

        m_SwapchainImageCount = 0;
        m_CurrentBackBufferIndex = 0;
        m_SwapchainWidth = 0;
        m_SwapchainHeight = 0;
    }
}

bool D3D12Device::GetSwapchainSize(uint32_t& outWidth, uint32_t& outHeight) const
{
    if (!m_Swapchain || m_SwapchainWidth == 0 || m_SwapchainHeight == 0)
    {
        outWidth = 0;
        outHeight = 0;
        return false;
    }

    outWidth = m_SwapchainWidth;
    outHeight = m_SwapchainHeight;
    return true;
}

bool D3D12Device::AcquireNextImage(uint32_t& imageIndex)
{
    if (!m_Swapchain)
    {
        return false;
    }

    // Get current back buffer index
    m_CurrentBackBufferIndex = m_Swapchain->GetCurrentBackBufferIndex();
    imageIndex = m_CurrentBackBufferIndex;

    return true;
}

bool D3D12Device::PresentImage(uint32_t imageIndex)
{
    if (!m_Swapchain)
    {
        return false;
    }

    // Present the swapchain
    HRESULT hr = m_Swapchain->Present(1, 0); // VSync enabled

    if (SUCCEEDED(hr))
    {

        return true;
    }
    else if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
    {
        std::cerr << "⚠️  D3D12Device: Device lost during present - this may be due to driver issues or debugging tools" << std::endl;
        std::cerr << "   HRESULT: " << std::hex << hr << std::endl;

        // Get device removed reason for better diagnostics
        if (m_Device)
        {
            HRESULT deviceRemovedReason = m_Device->GetDeviceRemovedReason();
            std::cerr << "   Device removed reason: " << std::hex << deviceRemovedReason << std::endl;
        }

        std::cerr << "   Continuing execution (device lost is non-fatal for this example)" << std::endl;
        return false; // Return false to indicate present failed, but don't crash
    }
    else
    {
        std::cerr << "D3D12Device: Failed to present swapchain! HRESULT: " << std::hex << hr << std::endl;
        return false;
    }
}

std::vector<uint8_t> D3D12Device::LoadShaderFile(const std::string& filename)
{
    std::ifstream file(filename, std::ios::ate | std::ios::binary);

    if (!file.is_open())
    {
        std::cerr << "Failed to open DirectX 12 shader file: " << filename << std::endl;
        return {};
    }

    size_t fileSize = (size_t)file.tellg();
    std::vector<uint8_t> buffer(fileSize);

    file.seekg(0);
    file.read(reinterpret_cast<char*>(buffer.data()), fileSize);
    file.close();

    return buffer;
}

bool D3D12Device::CreateD3D12PipelineFromShaders(const std::vector<uint8_t>& vertexShader,
                                                 const std::vector<uint8_t>& pixelShader,
                                                 const PipelineDesc& desc,
                                                 D3D12Pipeline& outPipeline)
{

    // Create root signature (empty for now - no constants or textures)
    D3D12_ROOT_SIGNATURE_DESC rootSignatureDesc = {};
    rootSignatureDesc.NumParameters = 0;
    rootSignatureDesc.pParameters = nullptr;
    rootSignatureDesc.NumStaticSamplers = 0;
    rootSignatureDesc.pStaticSamplers = nullptr;
    rootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> error;
    HRESULT hr = D3D12SerializeRootSignature(&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);

    if (FAILED(hr))
    {
        if (error)
        {
            std::cerr << "Root signature serialization failed: " << (char*)error->GetBufferPointer() << std::endl;
        }
        return false;
    }

    hr = m_Device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
                                       IID_PPV_ARGS(&outPipeline.rootSignature));
    if (FAILED(hr))
    {
        std::cerr << "Failed to create root signature! HRESULT: " << std::hex << hr << std::endl;
        return false;
    }

    // Define input layout for vertex data (position + color)
    D3D12_INPUT_ELEMENT_DESC inputElementDescs[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};

    // Create graphics pipeline state
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = {inputElementDescs, _countof(inputElementDescs)};
    psoDesc.pRootSignature = outPipeline.rootSignature.Get();
    psoDesc.VS = {vertexShader.data(), vertexShader.size()};
    psoDesc.PS = {pixelShader.data(), pixelShader.size()};
    // Use structured rasterization state
    psoDesc.RasterizerState.FillMode = ConvertPolygonModeD3D12(desc.rasterizationState.polygonMode);
    psoDesc.RasterizerState.CullMode = ConvertCullModeD3D12(desc.rasterizationState.cullMode);
    psoDesc.RasterizerState.FrontCounterClockwise = ConvertFrontFaceD3D12(desc.rasterizationState.frontFace);
    psoDesc.RasterizerState.DepthBias = static_cast<INT>(desc.rasterizationState.depthBiasConstantFactor);
    psoDesc.RasterizerState.DepthBiasClamp = desc.rasterizationState.depthBiasClamp;
    psoDesc.RasterizerState.SlopeScaledDepthBias = desc.rasterizationState.depthBiasSlopeFactor;
    psoDesc.RasterizerState.DepthClipEnable = desc.rasterizationState.depthClampEnable ? FALSE : TRUE; // Inverted logic
    psoDesc.RasterizerState.MultisampleEnable = FALSE;
    psoDesc.RasterizerState.AntialiasedLineEnable = FALSE;
    psoDesc.RasterizerState.ForcedSampleCount = 0;
    psoDesc.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    psoDesc.BlendState.AlphaToCoverageEnable =
        desc.colorBlendState.alphaToCoverageEnable ? TRUE : FALSE;
    psoDesc.BlendState.IndependentBlendEnable = FALSE;
    const D3D12_RENDER_TARGET_BLEND_DESC defaultRenderTargetBlendDesc = {
        FALSE,
        FALSE,
        D3D12_BLEND_ONE,
        D3D12_BLEND_ZERO,
        D3D12_BLEND_OP_ADD,
        D3D12_BLEND_ONE,
        D3D12_BLEND_ZERO,
        D3D12_BLEND_OP_ADD,
        D3D12_LOGIC_OP_NOOP,
        D3D12_COLOR_WRITE_ENABLE_ALL,
    };
    for (UINT i = 0; i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
        psoDesc.BlendState.RenderTarget[i] = defaultRenderTargetBlendDesc;
    // Use structured depth stencil state
    psoDesc.DepthStencilState.DepthEnable = desc.depthStencilState.depthTestEnable ? TRUE : FALSE;
    psoDesc.DepthStencilState.DepthWriteMask = desc.depthStencilState.depthWriteEnable ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    psoDesc.DepthStencilState.DepthFunc = ConvertCompareOpD3D12(desc.depthStencilState.depthCompareOp);
    psoDesc.DepthStencilState.StencilEnable = desc.depthStencilState.stencilTestEnable ? TRUE : FALSE;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.SampleDesc.Count = 1;

    hr = m_Device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&outPipeline.pipelineState));
    if (FAILED(hr))
    {
        std::cerr << "Failed to create graphics pipeline state! HRESULT: " << std::hex << hr << std::endl;
        return false;
    }

    return true;
}

const D3D12Pipeline* D3D12Device::GetD3D12Pipeline(PipelineHandle handle) const
{
    return m_ResourceContainers->pipelines.TryGet(handle);
}

ID3D12Resource* D3D12Device::GetD3D12Buffer(BufferHandle handle) const
{
    const D3D12Buffer* buffer = m_ResourceContainers->buffers.TryGet(handle);
    return buffer ? buffer->resource.Get() : nullptr;
}

ID3D12Resource* D3D12Device::GetD3D12Resource(TextureHandle handle) const
{
    const D3D12Texture* texture = m_ResourceContainers->textures.TryGet(handle);
    return texture ? texture->resource.Get() : nullptr;
}

size_t D3D12Device::GetBufferSize(BufferHandle handle) const
{
    const D3D12Buffer* buffer = m_ResourceContainers->buffers.TryGet(handle);
    return buffer ? buffer->size : 0;
}

// New descriptor set management methods

// Parameter-based resource creation
BufferHandle D3D12Device::CreateBuffer(const BufferDesc& desc)
{
    if (desc.size == 0)
    {
        std::cerr << "D3D12Device: Cannot create buffer with zero size" << std::endl;
        return INVALID_BUFFER_HANDLE;
    }

    // Create D3D12 buffer resource with specified parameters
    D3D12_HEAP_PROPERTIES heapProps = {};
    switch (desc.memoryUsage)
    {
    case BufferMemoryUsage::Upload:
    // Deliberately identical to Upload, not approximated. Expressing the
    // preference needs D3D12_HEAP_TYPE_GPU_UPLOAD, which is gated on
    // D3D12_FEATURE_D3D12_OPTIONS16::GPUUploadHeapSupported and on this backend
    // reporting RenderingDeviceCapabilities::memoryTopology — neither of which
    // it does. Resolving to DEFAULT instead would hand back a heap the CPU
    // cannot write and fault the caller's mapping, which is the one outcome a
    // residency preference must never produce. UPLOAD keeps this backend
    // byte-identical to today.
    case BufferMemoryUsage::UploadDeviceLocalPreferred:
        heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
        break;
    case BufferMemoryUsage::Readback:
        heapProps.Type = D3D12_HEAP_TYPE_READBACK;
        break;
    case BufferMemoryUsage::DeviceLocal:
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
        break;
    case BufferMemoryUsage::Auto:
    default:
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
        break;
    }
    heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

    D3D12_RESOURCE_DESC resourceDesc = {};
    resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resourceDesc.Alignment = 0;
    resourceDesc.Width = desc.size;
    resourceDesc.Height = 1;
    resourceDesc.DepthOrArraySize = 1;
    resourceDesc.MipLevels = 1;
    resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
    resourceDesc.SampleDesc.Count = 1;
    resourceDesc.SampleDesc.Quality = 0;
    resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    resourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    // Set resource state based on memory usage
    D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COMMON;
    if (heapProps.Type == D3D12_HEAP_TYPE_UPLOAD)
    {
        initialState = D3D12_RESOURCE_STATE_GENERIC_READ;
    }
    else if (heapProps.Type == D3D12_HEAP_TYPE_READBACK)
    {
        initialState = D3D12_RESOURCE_STATE_COPY_DEST; // will be copied from GPU
    }

    ComPtr<ID3D12Resource> resource;
    HRESULT hr = m_Device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &resourceDesc,
        initialState,
        nullptr,
        IID_PPV_ARGS(&resource));

    if (FAILED(hr))
    {
        std::cerr << "D3D12Device: Failed to create buffer resource" << std::endl;
        return INVALID_BUFFER_HANDLE;
    }

    // Set debug name if provided
    if (desc.debugName)
    {
        std::wstring wideName(desc.debugName, desc.debugName + strlen(desc.debugName));
        resource->SetName(wideName.c_str());
    }

    // Create D3D12Buffer struct and store
    D3D12Buffer d3d12Buffer;
    d3d12Buffer.resource = resource;
    d3d12Buffer.size = desc.size;
    d3d12Buffer.usage = static_cast<BufferUsage>(desc.usage);
    d3d12Buffer.currentState = initialState;
    d3d12Buffer.cpuMappable = heapProps.Type == D3D12_HEAP_TYPE_UPLOAD
                              || heapProps.Type == D3D12_HEAP_TYPE_READBACK;
    d3d12Buffer.debugName = desc.debugName ? desc.debugName : "unnamed_buffer";

    return m_ResourceContainers->buffers.Create(d3d12Buffer);
}

TextureHandle D3D12Device::CreateTexture(const TextureDesc& desc)
{
    if (desc.width == 0 || desc.height == 0 || desc.depth == 0)
    {
        Logger::Log::Error("D3D12Device: Cannot create texture with zero dimensions");
        return INVALID_TEXTURE_HANDLE;
    }
    if (TextureFormatSupportGateRefuses(*this, desc))
    {
        return INVALID_TEXTURE_HANDLE;
    }
    const bool is3DTexture = desc.depth > 1;
    if (is3DTexture)
    {
        const char* name = desc.debugName ? desc.debugName : "<unnamed>";
        if (desc.arrayLayers > 1)
        {
            Logger::Log::Error("D3D12Device: 3D texture '{}' must use arrayLayers=1, got {}", name, desc.arrayLayers);
            return INVALID_TEXTURE_HANDLE;
        }
        if (desc.sampleCount > 1)
        {
            Logger::Log::Error("D3D12Device: 3D texture '{}' cannot be multisampled", name);
            return INVALID_TEXTURE_HANDLE;
        }
        if (desc.usage & static_cast<uint32_t>(TextureUsage::DepthStencil))
        {
            Logger::Log::Error("D3D12Device: 3D texture '{}' cannot use DepthStencil usage", name);
            return INVALID_TEXTURE_HANDLE;
        }
    }

    // Convert format (using static_cast like VulkanDevice)
    DXGI_FORMAT dxgiFormat = DXGI_FORMAT_R8G8B8A8_UNORM; // Default format
    switch (desc.format)
    {
    case static_cast<uint32_t>(TextureFormat::RGBA8_UNORM):
        dxgiFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        break;
    case static_cast<uint32_t>(TextureFormat::BGRA8_UNORM):
        dxgiFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
        break;
    case static_cast<uint32_t>(TextureFormat::D24_UNORM_S8_UINT):
        dxgiFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
        break;
    case static_cast<uint32_t>(TextureFormat::D32_SFLOAT_S8_UINT):
        dxgiFormat = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        break;
    case static_cast<uint32_t>(TextureFormat::D32_FLOAT):
        dxgiFormat = DXGI_FORMAT_D32_FLOAT;
        break;
    case static_cast<uint32_t>(TextureFormat::D16_UNORM):
        dxgiFormat = DXGI_FORMAT_D16_UNORM;
        break;
    case static_cast<uint32_t>(TextureFormat::R16_FLOAT):
        dxgiFormat = DXGI_FORMAT_R16_FLOAT;
        break;
    case static_cast<uint32_t>(TextureFormat::R32G32_FLOAT):
        dxgiFormat = DXGI_FORMAT_R32G32_FLOAT;
        break;
    case static_cast<uint32_t>(TextureFormat::R16G16B16A16_FLOAT):
        dxgiFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
        break;
    default:
        break;
    }

    // Create D3D12 texture resource
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
    heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

    D3D12_RESOURCE_DESC resourceDesc = {};
    resourceDesc.Dimension = is3DTexture ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resourceDesc.Alignment = 0;
    resourceDesc.Width = desc.width;
    resourceDesc.Height = desc.height;
    resourceDesc.DepthOrArraySize = static_cast<UINT16>(is3DTexture ? desc.depth : (desc.arrayLayers > 0 ? desc.arrayLayers : 1));
    resourceDesc.MipLevels = desc.mipLevels;
    resourceDesc.Format = dxgiFormat;
    resourceDesc.SampleDesc.Count = 1;
    resourceDesc.SampleDesc.Quality = 0;
    resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    resourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    // Set flags based on usage
    if (desc.usage & static_cast<uint32_t>(TextureUsage::RenderTarget))
    {
        resourceDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    }
    if (desc.usage & static_cast<uint32_t>(TextureUsage::DepthStencil))
    {
        resourceDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    }

    // Set up clear value for depth/stencil textures
    D3D12_CLEAR_VALUE clearValue = {};
    D3D12_CLEAR_VALUE* pClearValue = nullptr;

    if (desc.usage & static_cast<uint32_t>(TextureUsage::DepthStencil))
    {
        clearValue.Format = dxgiFormat;
        clearValue.DepthStencil.Depth = 1.0f;
        clearValue.DepthStencil.Stencil = 0;
        pClearValue = &clearValue;
    }
    else if (desc.usage & static_cast<uint32_t>(TextureUsage::RenderTarget))
    {
        clearValue.Format = dxgiFormat;
        clearValue.Color[0] = 0.0f;
        clearValue.Color[1] = 0.0f;
        clearValue.Color[2] = 0.0f;
        clearValue.Color[3] = 1.0f;
        pClearValue = &clearValue;
    }

    ComPtr<ID3D12Resource> resource;
    HRESULT hr = m_Device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &resourceDesc,
        D3D12_RESOURCE_STATE_COMMON,
        pClearValue,
        IID_PPV_ARGS(&resource));

    if (FAILED(hr))
    {
        Logger::Log::Error("D3D12Device: Failed to create texture resource.");
        Logger::Log::Error("  HRESULT: 0x%08X", static_cast<unsigned int>(hr));
        Logger::Log::Error("  Texture dimensions: %ux%ux%u", desc.width, desc.height, desc.depth);
        Logger::Log::Error("  Format enum value: %u", desc.format);
        Logger::Log::Error("  Usage flags: 0x%08X", desc.usage);
        return INVALID_TEXTURE_HANDLE;
    }

    // Set debug name if provided
    if (desc.debugName)
    {
        std::wstring wideName(desc.debugName, desc.debugName + strlen(desc.debugName));
        resource->SetName(wideName.c_str());
    }

    D3D12Texture texture{};
    texture.resource = resource;
    texture.format = dxgiFormat;
    texture.width = desc.width;
    texture.height = desc.height;
    texture.depth = desc.depth;
    texture.arrayLayers = is3DTexture ? 1 : (desc.arrayLayers > 0 ? desc.arrayLayers : 1);
    texture.usage = static_cast<TextureUsage>(desc.usage);
    texture.debugName = desc.debugName ? desc.debugName : "";
    return m_ResourceContainers->textures.Create(std::move(texture));
}

TextureViewHandle D3D12Device::CreateTextureView(TextureHandle texture, const TextureViewDesc& desc)
{
    const D3D12Texture* tex = m_ResourceContainers->textures.TryGet(texture);
    if (!tex || !tex->resource)
    {
        return INVALID_TEXTURE_VIEW_HANDLE;
    }

    D3D12TextureView view{};
    if (!m_ResourceContainers->srvCpuPool.Allocate(view.CpuHandle, view.ChunkIndex, view.SlotIndex))
    {
        Logger::Log::Error("D3D12Device: Failed to allocate CPU descriptor slot for texture view");
        return INVALID_TEXTURE_VIEW_HANDLE;
    }

    view.Texture = texture;
    view.Format = desc.formatOverride ? static_cast<DXGI_FORMAT>(desc.formatOverride) : tex->format;
    view.DebugName = desc.debugName ? desc.debugName : "";

    const D3D12_RESOURCE_DESC rd = tex->resource->GetDesc();
    const UINT mipLevels = desc.levelCount ? desc.levelCount : static_cast<UINT>(rd.MipLevels - desc.baseMip);

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = view.Format;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

    switch (desc.viewType)
    {
    case TextureViewType::View3D:
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        srv.Texture3D.MostDetailedMip = desc.baseMip;
        srv.Texture3D.MipLevels = mipLevels;
        srv.Texture3D.ResourceMinLODClamp = 0.0f;
        break;
    case TextureViewType::View2DArray:
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        srv.Texture2DArray.MostDetailedMip = desc.baseMip;
        srv.Texture2DArray.MipLevels = mipLevels;
        srv.Texture2DArray.FirstArraySlice = desc.baseLayer;
        srv.Texture2DArray.ArraySize = desc.layerCount ? desc.layerCount : tex->arrayLayers - desc.baseLayer;
        srv.Texture2DArray.PlaneSlice = 0;
        srv.Texture2DArray.ResourceMinLODClamp = 0.0f;
        break;
    case TextureViewType::ViewCube:
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        srv.TextureCube.MostDetailedMip = desc.baseMip;
        srv.TextureCube.MipLevels = mipLevels;
        srv.TextureCube.ResourceMinLODClamp = 0.0f;
        break;
    case TextureViewType::ViewCubeArray:
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
        srv.TextureCubeArray.MostDetailedMip = desc.baseMip;
        srv.TextureCubeArray.MipLevels = mipLevels;
        srv.TextureCubeArray.First2DArrayFace = desc.baseLayer;
        srv.TextureCubeArray.NumCubes = (desc.layerCount ? desc.layerCount : tex->arrayLayers - desc.baseLayer) / 6u;
        srv.TextureCubeArray.ResourceMinLODClamp = 0.0f;
        break;
    case TextureViewType::View2D:
    default:
        if (rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D)
        {
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
            srv.Texture3D.MostDetailedMip = desc.baseMip;
            srv.Texture3D.MipLevels = mipLevels;
            srv.Texture3D.ResourceMinLODClamp = 0.0f;
        }
        else
        {
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Texture2D.MostDetailedMip = desc.baseMip;
            srv.Texture2D.MipLevels = mipLevels;
            srv.Texture2D.PlaneSlice = 0;
            srv.Texture2D.ResourceMinLODClamp = 0.0f;
        }
        break;
    }

    view.Dimension = srv.ViewDimension;
    m_Device->CreateShaderResourceView(tex->resource.Get(), &srv, view.CpuHandle);
    return m_ResourceContainers->textureViews.Create(std::move(view));
}

bool D3D12Device::IsTextureFormatSupported(TextureFormat format, uint32_t usageFlags) const
{
    DXGI_FORMAT dxgiFormat = DXGI_FORMAT_UNKNOWN;
    switch (format)
    {
    case TextureFormat::RGBA8_UNORM:
        dxgiFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        break;
    case TextureFormat::BGRA8_UNORM:
        dxgiFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
        break;
    case TextureFormat::D24_UNORM_S8_UINT:
        dxgiFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
        break;
    case TextureFormat::D32_SFLOAT_S8_UINT:
        dxgiFormat = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        break;
    case TextureFormat::D32_FLOAT:
        dxgiFormat = DXGI_FORMAT_D32_FLOAT;
        break;
    case TextureFormat::D16_UNORM:
        dxgiFormat = DXGI_FORMAT_D16_UNORM;
        break;
    case TextureFormat::R16_FLOAT:
        dxgiFormat = DXGI_FORMAT_R16_FLOAT;
        break;
    case TextureFormat::R32G32_FLOAT:
        dxgiFormat = DXGI_FORMAT_R32G32_FLOAT;
        break;
    case TextureFormat::R16G16B16A16_FLOAT:
        dxgiFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
        break;
    default:
        break;
    }

    if (dxgiFormat == DXGI_FORMAT_UNKNOWN)
    {
        return false;
    }

    D3D12_FEATURE_DATA_FORMAT_SUPPORT support = {};
    support.Format = dxgiFormat;
    if (FAILED(m_Device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))))
    {
        return false;
    }

    if (usageFlags & static_cast<uint32_t>(TextureUsage::RenderTarget))
    {
        if (!(support.Support1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET))
            return false;
    }
    if (usageFlags & static_cast<uint32_t>(TextureUsage::DepthStencil))
    {
        if (!(support.Support1 & D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL))
            return false;
    }
    if (usageFlags & static_cast<uint32_t>(TextureUsage::ShaderResource))
    {
        if (!(support.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE))
            return false;
    }
    if (usageFlags & static_cast<uint32_t>(TextureUsage::UnorderedAccess))
    {
        if (!(support.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW))
            return false;
    }

    return true;
}

SamplerHandle D3D12Device::CreateSampler(const SamplerDesc& desc)
{
    // Create D3D12 sampler descriptor with specified parameters
    D3D12_SAMPLER_DESC samplerDesc = {};

    // Convert filter mode (using uint32_t values from SamplerDesc)
    if (desc.minFilter == 1 && desc.magFilter == 1)
    {
        samplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    }
    else
    {
        samplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    }

    // Convert address mode (using uint32_t values from SamplerDesc)
    D3D12_TEXTURE_ADDRESS_MODE addressModeU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    switch (desc.addressModeU)
    {
    case 0:
        addressModeU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        break;
    case 1:
        addressModeU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        break;
    case 2:
        addressModeU = D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
        break;
    default:
        break;
    }

    samplerDesc.AddressU = addressModeU;
    samplerDesc.AddressV = addressModeU; // Use same mode for V and W for simplicity
    samplerDesc.AddressW = addressModeU;
    samplerDesc.MipLODBias = 0.0f;
    samplerDesc.MaxAnisotropy = 16;
    samplerDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    samplerDesc.BorderColor[0] = 0.0f;
    samplerDesc.BorderColor[1] = 0.0f;
    samplerDesc.BorderColor[2] = 0.0f;
    samplerDesc.BorderColor[3] = 0.0f;
    samplerDesc.MinLOD = 0.0f;
    samplerDesc.MaxLOD = D3D12_FLOAT32_MAX;

    // Create sampler descriptor handle
    D3D12_CPU_DESCRIPTOR_HANDLE samplerHandle = {};
    // Note: In a full implementation, this would allocate from a sampler descriptor heap
    // For now, we'll create a placeholder handle

    return m_ResourceContainers->samplers.Create(samplerHandle);
}

// UpdateDescriptorSet method removed - complex descriptor updates moved to CommandList

void D3D12Device::DestroyDescriptorSet(DescriptorSetHandle descriptorSet)
{
    if (!descriptorSet.IsValid())
    {
        return;
    }

    // In a full D3D12 implementation, this would:
    // 1. Free the descriptor heap allocation
    // 2. Return the descriptor range to the free list
    // 3. Update reference counts

    // For now, we just remove from the resource container
    // The actual descriptor heap management would be more complex
    Logger::Log::Info("D3D12Device: Descriptor set destroyed");
}

} // namespace Rendering
} // namespace GameEngine
