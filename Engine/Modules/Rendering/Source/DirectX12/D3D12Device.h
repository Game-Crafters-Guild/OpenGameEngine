/**
 * @file D3D12Device.h
 * @brief DirectX 12 implementation of the IDevice interface
 */

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "Rendering/Core/Device.h"
#include "Rendering/Core/ResourceContainer.h"
#include "Rendering/Core/ResourceManager.h"
#include <d3d12.h>
#include <dxgi1_6.h>
#include <memory>
#include <unordered_map>
#include <vector>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace GameEngine
{
namespace Rendering
{

// Forward declarations
struct D3D12Pipeline
{
    ComPtr<ID3D12PipelineState> pipelineState;
    ComPtr<ID3D12RootSignature> rootSignature;
    std::string debugName;
};

struct D3D12Buffer
{
    ComPtr<ID3D12Resource> resource;
    D3D12_RESOURCE_STATES currentState = D3D12_RESOURCE_STATE_COMMON;
    BufferUsage usage = BufferUsage::None;
    size_t size = 0;
    // UPLOAD / READBACK resources live in CPU-accessible heaps and can be
    // mapped; DEFAULT-heap resources cannot on a discrete adapter, so a write
    // to one has to route through a copy rather than Map().
    bool cpuMappable = false;
    std::string debugName;
};

/**
 * @brief DirectX 12 implementation of the device interface
 */
class D3D12Device : public IDevice
{
  public:
    D3D12Device();
    ~D3D12Device() override;

    // IDevice interface
    bool Initialize(const DeviceDesc& desc) override;
    void Shutdown() override;

    GraphicsAPI GetAPI() const override { return GraphicsAPI::DirectX12; }

    const RenderingDeviceCapabilities& GetCapabilities() const override { return m_Capabilities; }

    // Resource creation (parameter-based)
    BufferHandle CreateBuffer(const BufferDesc& desc) override;
    TextureHandle CreateTexture(const TextureDesc& desc) override;
    TextureViewHandle CreateTextureView(TextureHandle texture, const TextureViewDesc& desc) override;
    SamplerHandle CreateSampler(const SamplerDesc& desc) override;
    PipelineHandle CreateConcreteGraphicsPipeline(
        const GraphicsPipelineDesc& gd, const PipelineFormatKey& fk) override;
    PipelineHandle CreateConcreteComputePipeline(
        const ComputePipelineDesc& cd) override;

    // Resource destruction
    void DestroyBuffer(BufferHandle handle) override;
    void DestroyTexture(TextureHandle handle) override;
    void DestroyTextureView(TextureViewHandle handle) override;
    void DestroySampler(SamplerHandle handle) override;
    void DestroyPipeline(PipelineHandle handle) override;

    // Resource access
    void* MapBuffer(BufferHandle handle) override;
    void UnmapBuffer(BufferHandle handle) override;
    void UpdateBuffer(BufferHandle handle, size_t offset, size_t size, const void* data) override;

    // Command recording
    void ExecuteCommandLists(const std::vector<CommandList*>& commandLists) override;

    // Synchronization
    void WaitForIdle() override;

    // Frame management
    bool BeginFrame() override;
    void Present() override;

    // Window presentation abstraction methods
    WindowTargetHandle CreateWindowTarget(void* windowHandle, uint32_t width, uint32_t height) override;
    bool DestroyWindowTarget(WindowTargetHandle target) override;
    bool SetActiveWindowTarget(WindowTargetHandle target) override;
    WindowTargetHandle GetActiveWindowTarget() const override;
    bool RecreateWindowTargetSwapchain(WindowTargetHandle target, uint32_t width, uint32_t height) override;
    bool GetWindowTargetSize(WindowTargetHandle target, uint32_t& outWidth, uint32_t& outHeight) const override;
    TextureHandle GetSwapchainImage(uint32_t index) const override;
    HdrOutputState GetHdrOutputState() const override { return m_HdrState; }
    HdrOutputMode GetActiveHdrOutputMode() const override { return m_HdrState.activeMode; }
    bool SetHdrOutputMode(HdrOutputMode mode, const HdrStaticMetadata* metadata = nullptr,
                          HdrSwapchainBitDepth bitDepth = HdrSwapchainBitDepth::Bit10) override;

    // Note: AcquireNextImage, PresentImage, and GetSwapchainImageCount are declared below
    // They implement the abstract interface with override keyword

    // Resource management
    ResourceManager* GetResourceManager() override;
    IQueryPool* GetQueryPool() override;

    // Swapchain integration
    TextureHandle GetCurrentSwapchainImageHandle() override;

    // Debug events
    void BeginEvent(const char* name) override;
    void EndEvent() override;
    void SetMarker(const char* name) override;

    // GPU timeline profiling removed - use external profiling tools

    // Indirect command support removed - moved to CommandList interface

    // Descriptor set management
    DescriptorSetHandle CreateDescriptorSet(const DescriptorSetDesc& desc) override;
    void UpdateDescriptorSet(DescriptorSetHandle descriptorSet, const DescriptorSetUpdate& update) override;
    void DestroyDescriptorSet(DescriptorSetHandle descriptorSet) override;

    // DirectX 12-specific accessors
    ID3D12Device* GetD3D12Device() const
    {
        return m_Device.Get();
    }
    ID3D12CommandQueue* GetCommandQueue() const
    {
        return m_CommandQueue.Get();
    }
    const std::vector<D3D12_CPU_DESCRIPTOR_HANDLE>& GetSwapchainRTVs() const
    {
        return m_SwapchainRTVs;
    }
    const std::vector<ComPtr<ID3D12Resource>>& GetSwapchainBuffers() const
    {
        return m_SwapchainBuffers;
    }
    const D3D12Pipeline* GetD3D12Pipeline(PipelineHandle handle) const;
    ID3D12Resource* GetD3D12Buffer(BufferHandle handle) const;
    ID3D12Resource* GetD3D12Resource(TextureHandle handle) const;
    size_t GetBufferSize(BufferHandle handle) const;
    IDXGIFactory4* GetDXGIFactory() const
    {
        return m_DxgiFactory.Get();
    }

    // Swapchain support
    bool CreateSwapchain(void* windowHandle, uint32_t width, uint32_t height);
    void DestroySwapchain();
    bool AcquireNextImage(uint32_t& imageIndex) override;
    bool PresentImage(uint32_t imageIndex) override;
    uint32_t GetSwapchainImageCount() const override
    {
        return m_SwapchainImageCount;
    }
    DXGI_FORMAT GetSwapchainFormat() const
    {
        return m_SwapchainFormat;
    }
    TextureFormat GetSwapchainTextureFormat() const override;
    bool GetSwapchainSize(uint32_t& outWidth, uint32_t& outHeight) const override;
    bool IsTextureFormatSupported(TextureFormat format, uint32_t usageFlags) const override;

  private:
    bool CreateDXGIFactory();
    bool SelectAdapter();
    bool CreateDevice();
    bool CreateCommandQueue();
    bool CreateDescriptorHeaps();
    bool CreateSynchronization();
    void QueryDeviceCapabilities();
    void CleanupD3D12();

    // Shader and pipeline helpers

    bool CreateD3D12PipelineFromShaders(const std::vector<uint8_t>& vertexShader,
                                        const std::vector<uint8_t>& pixelShader,
                                        const PipelineDesc& desc,
                                        D3D12Pipeline& outPipeline);

    // DirectX 12 objects
    ComPtr<IDXGIFactory4> m_DxgiFactory;
    ComPtr<IDXGIAdapter1> m_Adapter;
    ComPtr<ID3D12Device> m_Device;
    ComPtr<ID3D12CommandQueue> m_CommandQueue;
    ComPtr<ID3D12CommandAllocator> m_CommandAllocator;

    // Descriptor heaps
    ComPtr<ID3D12DescriptorHeap> m_RtvHeap;
    ComPtr<ID3D12DescriptorHeap> m_DsvHeap;
    ComPtr<ID3D12DescriptorHeap> m_SamplerHeap;
    uint32_t m_RtvDescriptorSize = 0;
    uint32_t m_DsvDescriptorSize = 0;
    uint32_t m_SamplerDescriptorSize = 0;

    // Swapchain objects
    ComPtr<IDXGISwapChain3> m_Swapchain;
    std::vector<ComPtr<ID3D12Resource>> m_SwapchainBuffers;
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> m_SwapchainRTVs;
    uint32_t m_SwapchainImageCount = 0;
    uint32_t m_CurrentBackBufferIndex = 0;
    DXGI_FORMAT m_SwapchainFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    DXGI_COLOR_SPACE_TYPE m_SwapchainColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    HdrOutputState m_HdrState{};
    uint32_t m_SwapchainWidth = 0;
    uint32_t m_SwapchainHeight = 0;
    void* m_WindowHandle = nullptr;
    WindowTargetHandle m_WindowTarget{};
    bool m_WindowTargetActive = false;

    // Synchronization
    ComPtr<ID3D12Fence> m_Fence;
    uint64_t m_FenceValue = 0;
    HANDLE m_FenceEvent = nullptr;

    // Debug layer
    ComPtr<ID3D12Debug> m_DebugController;

    // Device capabilities
    RenderingDeviceCapabilities m_Capabilities{};

    // Resource tracking using high-performance ResourceContainer
    struct D3D12ResourceContainers;
    std::unique_ptr<D3D12ResourceContainers> m_ResourceContainers;

    // Debug
    bool m_DebugLayerEnabled = false;
    std::string m_ApplicationName;

    // Resource management
    std::unique_ptr<ResourceManager> m_ResourceManager;

    // Config: allow gating bindless support even if the device supports it
    bool m_ForceDisableBindlessResources = false;
};

/**
 * @brief DirectX 12 texture resource
 */
struct D3D12Texture
{
    ComPtr<ID3D12Resource> resource;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t depth = 1;
    uint32_t arrayLayers = 1;
    TextureUsage usage = TextureUsage::None;
    std::string debugName;
};

// CPU-only SRV descriptor produced by CreateTextureView. The cpu handle is
// borrowed from a pooled descriptor heap owned by D3D12Device; the chunk/slot
// pair locates the slot for release on destroy. Not shader-visible: a future
// bindless binding step must copy this descriptor into the shader-visible heap.
struct D3D12TextureView
{
    D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle{};
    uint32_t ChunkIndex = 0;
    uint32_t SlotIndex = 0;
    TextureHandle Texture = INVALID_TEXTURE_HANDLE;
    DXGI_FORMAT Format = DXGI_FORMAT_UNKNOWN;
    D3D12_SRV_DIMENSION Dimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    std::string DebugName;
};

} // namespace Rendering
} // namespace GameEngine
