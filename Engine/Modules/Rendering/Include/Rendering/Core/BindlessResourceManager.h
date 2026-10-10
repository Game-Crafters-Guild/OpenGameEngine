/**
 * @file BindlessResourceManager.h
 * @brief Modern bindless resource management for scalable GPU-driven rendering
 *
 * Provides bindless descriptor management that allows binding thousands of textures
 * and buffers without individual bind calls. Essential for modern GPU-driven rendering
 * pipelines that need to access large numbers of resources efficiently.
 *
 * Key Features:
 * - Bindless texture arrays with automatic descriptor allocation
 * - Bindless buffer management with GPU addresses
 * - Descriptor heap management for Vulkan and DirectX 12
 * - Automatic resource binding and dependency tracking
 * - Integration with render graph system
 */

#pragma once

#include "Rendering/Core/Device.h"
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace GameEngine
{
namespace Rendering
{

// Forward declarations
class IDevice;


/**
 * @brief Bindless resource handle types (distinct types for overloading)
 */
enum class BindlessTextureHandle : uint32_t
{
};
enum class BindlessBufferHandle : uint32_t
{
};
// Note: DescriptorSetHandle is defined in Device.h

/**
 * @brief Invalid bindless handles
 */
constexpr BindlessTextureHandle kInvalidBindlessTexture = static_cast<BindlessTextureHandle>(0);
constexpr BindlessBufferHandle kInvalidBindlessBuffer = static_cast<BindlessBufferHandle>(0);
// Note: INVALID_DESCRIPTOR_SET is defined in Device.h

/**
 * @brief Bindless resource types
 */
enum class BindlessResourceType
{
    Texture2D,
    Texture3D,
    TextureCube,
    TextureArray,
    StructuredBuffer,
    ConstantBuffer,
    RWTexture2D,
    RWStructuredBuffer
};

/**
 * @brief Descriptor heap types for different resource categories
 */
enum class DescriptorHeapType
{
    CBV_SRV_UAV, // Constant Buffer, Shader Resource, Unordered Access Views
    Sampler,     // Sampler descriptors
    RTV,         // Render Target Views
    DSV          // Depth Stencil Views
};

/**
 * @brief Bindless texture descriptor
 */
struct BindlessTextureDesc
{
    TextureHandle textureHandle = INVALID_TEXTURE_HANDLE;
    BindlessResourceType type = BindlessResourceType::Texture2D;
    uint32_t mipLevel = 0;
    // Mip levels the view spans, starting at `mipLevel`. 1 is a single level —
    // what a material slot or a per-cascade raw-depth read needs. 0 means every
    // remaining level, which a shader doing texelFetch(tex, coord, lod) with
    // lod > 0 requires: a single-level view has no such level to read.
    uint32_t mipCount = 1;
    uint32_t arraySlice = 0;
    // Aspect to view. Color is the default for material textures; Depth is
    // required when sampling a depth-format texture as a non-comparison
    // sampler2D (e.g. PCSS blocker search reading raw shadow depth).
    TextureAspect aspect = TextureAspect::Color;
    bool isWritable = false; // For UAV textures
    const char* debugName = nullptr;
};

/**
 * @brief Bindless buffer descriptor
 */
struct BindlessBufferDesc
{
    BufferHandle bufferHandle = INVALID_BUFFER_HANDLE;
    BindlessResourceType type = BindlessResourceType::StructuredBuffer;
    size_t offset = 0;
    size_t size = 0;         // 0 = entire buffer
    uint32_t stride = 0;     // For structured buffers
    bool isWritable = false; // For UAV buffers
    const char* debugName = nullptr;
};

/**
 * @brief Bindless descriptor set layout description
 * Note: DescriptorSetLayoutDesc is defined in Device.h for general use
 */
struct BindlessDescriptorSetLayoutDesc
{
    std::vector<BindlessResourceType> resourceTypes;
    uint32_t maxBindlessTextures = 1000000;
    uint32_t maxBindlessBuffers = 1000000;
    uint32_t maxSamplers = 1000;
    const char* debugName = nullptr;
};

/**
 * @brief Bindless resource binding information
 */
struct BindlessResourceBinding
{
    BindlessTextureHandle textureHandle = kInvalidBindlessTexture;
    BindlessBufferHandle bufferHandle = kInvalidBindlessBuffer;
    uint32_t descriptorIndex = 0;
    BindlessResourceType type = BindlessResourceType::Texture2D;
    bool isValid = false;
};

/**
 * @brief Descriptor heap statistics
 */
struct DescriptorHeapStats
{
    uint32_t totalDescriptors = 0;
    uint32_t usedDescriptors = 0;
    uint32_t freeDescriptors = 0;
    uint32_t peakUsage = 0;
    size_t memoryUsage = 0; // In bytes
    uint32_t heapGrowthCount = 0;
};

/**
 * @brief Bindless resource manager statistics
 */
struct BindlessResourceStats
{
    uint32_t totalBindlessTextures = 0;
    uint32_t totalBindlessBuffers = 0;
    uint32_t activeDescriptorSets = 0;
    DescriptorHeapStats heapStats[4]; // One for each DescriptorHeapType
    size_t totalMemoryUsage = 0;
};

/**
 * @brief Modern Bindless Resource Manager
 *
 * Manages bindless descriptors for scalable GPU-driven rendering.
 * Allows binding thousands of textures and buffers without individual
 * bind calls, essential for modern rendering techniques.
 */
class BindlessResourceManager
{
  public:
    explicit BindlessResourceManager(IDevice* device);
    ~BindlessResourceManager();

    // Initialization and shutdown
    bool Initialize(uint32_t maxBindlessTextures = 1000000, uint32_t maxBindlessBuffers = 1000000);
    void Shutdown();

    // Descriptor set management
    DescriptorSetHandle CreateDescriptorSet(const BindlessDescriptorSetLayoutDesc& desc);
    void DestroyDescriptorSet(DescriptorSetHandle handle);

    // Bindless texture management
    BindlessTextureHandle CreateBindlessTexture(const BindlessTextureDesc& desc);
    void UpdateBindlessTexture(BindlessTextureHandle handle, const BindlessTextureDesc& desc);
    void DestroyBindlessTexture(BindlessTextureHandle handle);

    // Bindless buffer management
    BindlessBufferHandle CreateBindlessBuffer(const BindlessBufferDesc& desc);
    void UpdateBindlessBuffer(BindlessBufferHandle handle, const BindlessBufferDesc& desc);
    void DestroyBindlessBuffer(BindlessBufferHandle handle);

    // Resource binding and access
    uint32_t GetTextureDescriptorIndex(BindlessTextureHandle handle) const;
    uint32_t GetBufferDescriptorIndex(BindlessBufferHandle handle) const;
    BindlessResourceBinding GetResourceBinding(BindlessTextureHandle handle) const;
    BindlessResourceBinding GetResourceBinding(BindlessBufferHandle handle) const;

    // Statistics and debugging
    BindlessResourceStats GetStats() const;
    void PrintResourceReport() const;
    void SetResourceName(BindlessTextureHandle handle, const char* name);
    void SetResourceName(BindlessBufferHandle handle, const char* name);

    // Platform-specific access
    void* GetNativeDescriptorSet(DescriptorSetHandle handle) const;
    void* GetNativeDescriptorHeap(DescriptorHeapType type) const;

  private:
    class Impl;
    std::unique_ptr<Impl> m_Impl;
};

/**
 * @brief Bindless resource factory for creating bindless managers
 */
class BindlessResourceFactory
{
  public:
    static std::unique_ptr<BindlessResourceManager> Create(IDevice* device);
};

} // namespace Rendering
} // namespace GameEngine
