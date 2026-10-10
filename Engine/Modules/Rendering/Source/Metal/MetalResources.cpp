// Resource creation/destruction for MetalDevice: buffers, textures, samplers
// and concrete pipeline objects.

#include "MetalAccelerationStructures.h"
#include "Rendering/Core/TextureFormatSupportGate.h"
#include "MetalDevice.h"
#include "MetalMappings.h"
#include "MetalShaderTranslator.h"

#include "Rendering/Core/DeviceFormatting.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Core/SpecializationConstants.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <functional>
#include <thread>

namespace GameEngine
{
namespace Rendering
{

namespace
{
constexpr uint32_t kSpirvMagic = 0x07230203u;

bool IsSpirv(const std::vector<uint8_t>& bytes)
{
    if (bytes.size() < 4)
    {
        return false;
    }
    uint32_t magic = 0;
    std::memcpy(&magic, bytes.data(), sizeof(magic));
    return magic == kSpirvMagic;
}

// GE_METAL_DUMP_PIPELINE_TIMING=1: per-pipeline stage costs (translate /
// newLibrary / PSO) for attributing first-frame creation hitches.
bool DumpPipelineTiming()
{
    static const bool kEnabled = []() {
        const char* env = std::getenv("GE_METAL_DUMP_PIPELINE_TIMING");
        return env != nullptr && env[0] != '0';
    }();
    return kEnabled;
}

NS::String* MakeNSString(const std::string& s)
{
    return NS::String::string(s.c_str(), NS::UTF8StringEncoding);
}

// Shared MSL compile options. Fast math stays OFF: the depth prepass and the
// color pass compile the same vertex shader into separate pipelines, and the
// reverse-Z GreaterOrEqual contract needs both to produce bit-identical
// positions (fast-math reassociation breaks that between compilations).
MTL::CompileOptions* MakeCompileOptions()
{
    MTL::CompileOptions* options = MTL::CompileOptions::alloc()->init();
    // Fast math matches what the Vulkan path gets (MoltenVK compiles its MSL
    // with fast math on by default) and what GLSL-without-precise implies.
    // GE_METAL_PRECISE_MATH=1 flips it off when bisecting NaN/precision bugs.
    static const bool kPreciseMath = []() {
        const char* env = std::getenv("GE_METAL_PRECISE_MATH");
        return env != nullptr && env[0] != '0';
    }();
    options->setFastMathEnabled(!kPreciseMath);
    return options;
}

// Debug aids: failed MSL compiles always dump the generated source;
// GE_METAL_DUMP_MSL=1 dumps every translation for offline inspection.
void DumpMslTo(const char* prefix, const std::string& msl, const std::string& debugName)
{
    std::string safeName = debugName.empty() ? "unnamed" : debugName;
    for (char& c : safeName)
    {
        if (c == '/' || c == ' ' || c == ':')
        {
            c = '_';
        }
    }
    const std::string path = std::string("/tmp/") + prefix + safeName + ".metal";
    if (FILE* f = std::fopen(path.c_str(), "w"))
    {
        std::fwrite(msl.data(), 1, msl.size(), f);
        std::fclose(f);
    }
}

void DumpFailedMsl(const std::string& msl, const std::string& debugName)
{
    DumpMslTo("ge_msl_fail_", msl, debugName);
    Logger::Log::Error("MetalDevice: failing MSL written to /tmp/ge_msl_fail_*.metal");
}

bool ShouldDumpAllMsl()
{
    static const bool kDump = []() {
        const char* env = std::getenv("GE_METAL_DUMP_MSL");
        return env != nullptr && env[0] != '0';
    }();
    return kDump;
}

// Functions that declare [[function_constant(n)]] (SPIRV-Cross output for
// spec constants) can only build pipelines from a *specialized* MTLFunction;
// passing an empty constant-values object specializes with the defaults we
// baked at translation time. Plain functions take the same path harmlessly.
MTL::Function* NewSpecializedFunction(MTL::Library* library, const char* entry)
{
    MTL::FunctionConstantValues* constants = MTL::FunctionConstantValues::alloc()->init();
    NS::Error* error = nullptr;
    MTL::Function* fn = library->newFunction(NS::String::string(entry, NS::UTF8StringEncoding), constants, &error);
    constants->release();
    if (fn == nullptr && error != nullptr)
    {
        Logger::Log::Error("MetalDevice: function specialization failed for '{}': {}", entry,
                           error->localizedDescription()->utf8String());
    }
    return fn;
}
} // namespace

// ---------------------------------------------------------------------------
// Buffers
// ---------------------------------------------------------------------------

BufferHandle MetalDevice::CreateBuffer(const BufferDesc& desc)
{
    if (m_Device == nullptr || desc.size == 0)
    {
        return INVALID_BUFFER_HANDLE;
    }
    // Apple Silicon is UMA; shared storage covers upload, readback and
    // device-local uses for the bring-up milestone. desc.memoryUsage is
    // therefore not read at all, which is also the right answer for
    // UploadDeviceLocalPreferred: on unified memory shared storage already IS
    // device-local and host-writable, so the preference is satisfied rather
    // than ignored, and the caller keeps its mapping either way.
    MTL::Buffer* buffer = m_Device->newBuffer(desc.size, MTL::ResourceStorageModeShared);
    if (buffer == nullptr)
    {
        Logger::Log::Error("MetalDevice::CreateBuffer: allocation failed ({} bytes)", desc.size);
        return INVALID_BUFFER_HANDLE;
    }

    MetalBuffer wrapper{};
    wrapper.buffer = buffer;
    wrapper.size = desc.size;
    wrapper.usage = static_cast<BufferUsage>(desc.usage);
    if (desc.debugName != nullptr)
    {
        wrapper.debugName = desc.debugName;
        buffer->setLabel(MakeNSString(wrapper.debugName));
    }

    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    if ((wrapper.usage & BufferUsage::Indirect) != BufferUsage::None)
    {
        m_IndirectArgumentBuffers.push_back(buffer);
    }
    if ((wrapper.usage & BufferUsage::ShaderDeviceAddress) != BufferUsage::None)
    {
        m_DeviceAddressBuffers.push_back(buffer);
        if (m_BdaResidencySet != nullptr)
        {
            m_BdaResidencySet->addAllocation(static_cast<const MTL::Allocation*>(buffer));
            m_ResidencyDirty = true;
        }
    }
    return FromGeneric<BufferTag>(m_Buffers.Create(std::move(wrapper)));
}

void MetalDevice::DestroyBuffer(BufferHandle handle)
{
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    MetalBuffer* buf = m_Buffers.Get(ToGeneric(handle));
    if (buf == nullptr)
    {
        return;
    }
    if ((buf->usage & BufferUsage::Indirect) != BufferUsage::None)
    {
        m_IndirectArgumentBuffers.erase(
            std::remove(m_IndirectArgumentBuffers.begin(), m_IndirectArgumentBuffers.end(), buf->buffer),
            m_IndirectArgumentBuffers.end());
    }
    if ((buf->usage & BufferUsage::ShaderDeviceAddress) != BufferUsage::None)
    {
        m_DeviceAddressBuffers.erase(
            std::remove(m_DeviceAddressBuffers.begin(), m_DeviceAddressBuffers.end(), buf->buffer),
            m_DeviceAddressBuffers.end());
        if (m_BdaResidencySet != nullptr && buf->buffer != nullptr)
        {
            m_BdaResidencySet->removeAllocation(static_cast<const MTL::Allocation*>(buf->buffer));
            m_ResidencyDirty = true;
        }
    }
    ScrubResidency(buf->buffer);
    DeferRelease(buf->buffer);
    buf->buffer = nullptr;
    m_Buffers.Destroy(ToGeneric(handle));
}

void* MetalDevice::MapBuffer(BufferHandle handle)
{
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    MetalBuffer* buf = m_Buffers.Get(ToGeneric(handle));
    return (buf != nullptr && buf->buffer != nullptr) ? buf->buffer->contents() : nullptr;
}

void MetalDevice::UnmapBuffer(BufferHandle /*handle*/)
{
    // Shared storage buffers are persistently CPU-visible; nothing to do.
}

void MetalDevice::UpdateBuffer(BufferHandle handle, size_t offset, size_t size, const void* data)
{
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    MetalBuffer* buf = m_Buffers.Get(ToGeneric(handle));
    if (buf == nullptr || buf->buffer == nullptr || data == nullptr)
    {
        return;
    }
    // Reject writes that would run past the allocation — loudly, matching the
    // Vulkan/D3D12 backends, so a caller bug surfaces as a diagnostic instead
    // of a silently stale buffer. (Overflow-safe form: offset + size can wrap.)
    if (size > buf->size || offset > buf->size - size)
    {
        Logger::Log::Error(
            "MetalDevice::UpdateBuffer: write of {} bytes at offset {} exceeds buffer '{}' ({} bytes); rejected",
            size, offset, buf->debugName, buf->size);
        return;
    }
    std::memcpy(static_cast<uint8_t*>(buf->buffer->contents()) + offset, data, size);
}

void MetalDevice::UpdateBufferRanges(BufferHandle handle, std::span<const BufferUpdateRange> ranges)
{
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    MetalBuffer* buf = m_Buffers.Get(ToGeneric(handle));
    if (buf == nullptr || buf->buffer == nullptr)
    {
        return;
    }
    // The batch is all or nothing: every non-empty range is checked before any
    // is written, so one bad range cannot leave a valid prefix landed.
    for (const BufferUpdateRange& range : ranges)
    {
        if (range.size == 0)
        {
            continue;
        }
        if (range.data == nullptr || range.size > buf->size || range.offset > buf->size - range.size)
        {
            Logger::Log::Error(
                "MetalDevice::UpdateBufferRanges: invalid write of {} bytes at offset {} to '{}' ({} bytes); batch "
                "rejected",
                range.size, range.offset, buf->debugName, buf->size);
            return;
        }
    }
    // Shared storage is coherent: the copy is the whole upload.
    uint8_t* contents = static_cast<uint8_t*>(buf->buffer->contents());
    for (const BufferUpdateRange& range : ranges)
    {
        if (range.size != 0)
        {
            std::memcpy(contents + range.offset, range.data, range.size);
        }
    }
}

IDevice::BufferMemoryResidency MetalDevice::GetBufferMemoryResidency(BufferHandle handle) const
{
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    const MetalBuffer* buf = m_Buffers.Get(ToGeneric(handle));
    const std::optional<DeviceMemoryTopology>& topology = m_Capabilities.memoryTopology;
    // CreateBuffer makes every buffer shared storage, which is what this answer
    // describes; anything else is reported as unknown.
    if (buf == nullptr || buf->buffer == nullptr || buf->buffer->storageMode() != MTL::StorageModeShared ||
        !topology.has_value())
    {
        return {};
    }
    // Shared storage is CPU-visible and coherent. On unified memory it is the
    // GPU's own memory: heap 0, the device's working set, the same bytes the
    // topology declares device-local. On a discrete GPU it is host system
    // memory, which is heap 1.
    BufferMemoryResidency residency{};
    residency.reported = true;
    residency.hostVisible = true;
    residency.hostCoherent = true;
    residency.deviceLocal = topology->isUnifiedMemory;
    if (residency.deviceLocal)
    {
        residency.heapIndex = 0;
        residency.heapSizeBytes = topology->deviceLocalHeapBytesTotal;
    }
    else
    {
        residency.heapIndex = 1;
        residency.heapSizeBytes = NS::ProcessInfo::processInfo()->physicalMemory();
    }
    return residency;
}

// ---------------------------------------------------------------------------
// Textures
// ---------------------------------------------------------------------------

TextureHandle MetalDevice::CreateTexture(const TextureDesc& desc)
{
    if (m_Device == nullptr)
    {
        return INVALID_TEXTURE_HANDLE;
    }
    const TextureFormat format = static_cast<TextureFormat>(desc.format);
    const MTL::PixelFormat pixelFormat = MetalMappings::ToMTLPixelFormat(format);
    if (pixelFormat == MTL::PixelFormatInvalid)
    {
        Logger::Log::Error("MetalDevice::CreateTexture: format {} ({}) has no Metal pixel format",
                           ToString(format), desc.format);
        return INVALID_TEXTURE_HANDLE;
    }
    if (TextureFormatSupportGateRefuses(*this, desc))
    {
        return INVALID_TEXTURE_HANDLE;
    }

    MTL::TextureDescriptor* td = MTL::TextureDescriptor::alloc()->init();
    const bool isCube = (desc.flags & TextureCreateFlags::CubeCompatible) != TextureCreateFlags::None;
    if (desc.depth > 1)
    {
        td->setTextureType(MTL::TextureType3D);
    }
    else if (isCube)
    {
        td->setTextureType(desc.arrayLayers > 6 ? MTL::TextureTypeCubeArray : MTL::TextureTypeCube);
    }
    else if (desc.sampleCount > 1)
    {
        td->setTextureType(MTL::TextureType2DMultisample);
    }
    else if (desc.arrayLayers > 1 ||
             (desc.flags & TextureCreateFlags::ForceArrayView) != TextureCreateFlags::None)
    {
        // ForceArrayView keeps a one-layer array texture ARRAY-typed. Shaders
        // bind such a texture as texture2d_array/image2DArray whatever its
        // current depth, and Metal silently drops array-typed writes to a
        // plain TextureType2D — the dispatch encodes and stores nothing.
        // Vulkan applies the same flag to its view type (VulkanDevice.cpp).
        td->setTextureType(MTL::TextureType2DArray);
    }
    else
    {
        td->setTextureType(MTL::TextureType2D);
    }
    td->setPixelFormat(pixelFormat);
    td->setWidth(desc.width);
    td->setHeight(desc.height);
    td->setDepth(desc.depth);
    td->setMipmapLevelCount(desc.mipLevels);
    td->setSampleCount(desc.sampleCount);
    if (isCube)
    {
        td->setArrayLength(std::max(1u, desc.arrayLayers / 6u));
    }
    else
    {
        td->setArrayLength(desc.depth > 1 ? 1 : desc.arrayLayers);
    }

    const TextureUsage usage = static_cast<TextureUsage>(desc.usage);
    MTL::TextureUsage mtlUsage = 0;
    if ((usage & TextureUsage::ShaderResource) != TextureUsage::None)
    {
        mtlUsage |= MTL::TextureUsageShaderRead;
    }
    if ((usage & (TextureUsage::RenderTarget | TextureUsage::DepthStencil)) != TextureUsage::None)
    {
        mtlUsage |= MTL::TextureUsageRenderTarget;
    }
    if ((usage & TextureUsage::UnorderedAccess) != TextureUsage::None)
    {
        mtlUsage |= MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite;
    }
    td->setUsage(mtlUsage);

    // Private storage for everything: shared-storage textures forfeit lossless
    // bandwidth compression and GPU-optimal layouts on Apple Silicon, which
    // multiplies sampling bandwidth per pixel (measured ~6x frame cost at
    // native res). CPU transfers go through staging buffers + blits anyway.
    td->setStorageMode(MTL::StorageModePrivate);

    MTL::Texture* texture = m_Device->newTexture(td);
    td->release();
    if (texture == nullptr)
    {
        Logger::Log::Error("MetalDevice::CreateTexture: creation failed ({}x{})", desc.width, desc.height);
        return INVALID_TEXTURE_HANDLE;
    }

    MetalTexture wrapper{};
    wrapper.texture = texture;
    wrapper.format = format;
    wrapper.width = desc.width;
    wrapper.height = desc.height;
    wrapper.depth = desc.depth;
    wrapper.mipLevels = desc.mipLevels;
    wrapper.arrayLayers = desc.arrayLayers;
    wrapper.sampleCount = desc.sampleCount;
    wrapper.usage = usage;
    if (desc.debugName != nullptr)
    {
        wrapper.debugName = desc.debugName;
        texture->setLabel(MakeNSString(wrapper.debugName));
    }

    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    return FromGeneric<TextureTag>(m_Textures.Create(std::move(wrapper)));
}

void MetalDevice::DestroyTexture(TextureHandle handle)
{
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    MetalTexture* tex = m_Textures.Get(ToGeneric(handle));
    if (tex == nullptr)
    {
        return;
    }
    if (!tex->isSwapchainSlot && tex->texture != nullptr)
    {
        ScrubResidency(tex->texture);
        DeferRelease(tex->texture);
    }
    tex->texture = nullptr;
    m_Textures.Destroy(ToGeneric(handle));
}

size_t MetalDevice::DebugGetBufferRegistryCount() const
{
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    return m_Buffers.Size();
}

size_t MetalDevice::DebugGetImageRegistryCount() const
{
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    return m_Textures.Size();
}

size_t MetalDevice::DebugGetAllocationCount() const
{
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    return m_Buffers.Size() + m_Textures.Size();
}

// Like the Vulkan/VMA path (which sums vmaGetAllocationInfo().size), these report
// the REAL device footprint via MTL::Resource::allocatedSize() rather than the
// logical request size — so alignment/padding and the full mip chain are
// included. Swapchain slots alias the drawable (no owned allocation) and are
// excluded.
size_t MetalDevice::DebugGetBufferBytes() const
{
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    size_t total = 0;
    m_Buffers.ForEach([&](auto, const MetalBuffer& b) {
        if (b.buffer != nullptr)
            total += static_cast<size_t>(b.buffer->allocatedSize());
    });
    return total;
}

size_t MetalDevice::DebugGetTextureBytes() const
{
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    size_t total = 0;
    m_Textures.ForEach([&](auto, const MetalTexture& t) {
        if (!t.isSwapchainSlot && t.texture != nullptr)
            total += static_cast<size_t>(t.texture->allocatedSize());
    });
    return total;
}

size_t MetalDevice::DebugGetAllocatedBytes() const
{
    return DebugGetBufferBytes() + DebugGetTextureBytes();
}

void MetalDevice::DebugEnumerateResources(
    const std::function<void(const DebugResourceInfo&)>& fn) const
{
    if (!fn)
        return;
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    m_Buffers.ForEach([&](auto, const MetalBuffer& b) {
        if (b.buffer == nullptr)
            return;
        DebugResourceInfo info;
        info.Type = DebugResourceInfo::Kind::Buffer;
        info.Name = b.debugName.empty() ? "(unnamed buffer)" : b.debugName;
        info.Bytes = static_cast<uint64_t>(b.buffer->allocatedSize());
        fn(info);
    });
    m_Textures.ForEach([&](auto, const MetalTexture& t) {
        if (t.isSwapchainSlot || t.texture == nullptr)
            return;
        DebugResourceInfo info;
        info.Type = DebugResourceInfo::Kind::Texture;
        info.Name = t.debugName.empty() ? "(unnamed texture)" : t.debugName;
        info.Bytes = static_cast<uint64_t>(t.texture->allocatedSize());
        info.Width = t.width;
        info.Height = t.height;
        info.Format = t.format;
        fn(info);
    });
}

bool MetalDevice::IsTextureAlive(TextureHandle texture) const
{
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    return m_Textures.IsValid(ToGeneric(texture));
}

bool MetalDevice::IsTextureHandleLive(TextureHandle texture) const
{
    return IsTextureAlive(texture);
}

bool MetalDevice::IsPipelineAlive(PipelineHandle pipeline) const
{
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    return m_Pipelines.IsValid(ToGeneric(pipeline));
}

bool MetalDevice::IsTextureFormatSupported(TextureFormat format, uint32_t /*usageFlags*/) const
{
    // A format the engine can't map to an MTLPixelFormat can't be created at
    // all; that mapping is the dominant gate. (Per-usage caps — e.g. a format
    // usable as sampled but not as a render target — aren't probed here; texture
    // creation still validates against the actual descriptor.)
    return MetalMappings::ToMTLPixelFormat(format) != MTL::PixelFormatInvalid;
}

TextureViewHandle MetalDevice::CreateTextureView(TextureHandle texture, const TextureViewDesc& desc)
{
    MetalTexture* tex = GetMetalTexture(texture);
    if (tex == nullptr || tex->texture == nullptr)
    {
        return INVALID_TEXTURE_VIEW_HANDLE;
    }
    const TextureFormat format =
        desc.formatOverride != 0 ? static_cast<TextureFormat>(desc.formatOverride) : tex->format;
    MTL::PixelFormat pixelFormat = MetalMappings::ToMTLPixelFormat(format);
    if (pixelFormat == MTL::PixelFormatInvalid)
    {
        return INVALID_TEXTURE_VIEW_HANDLE;
    }
    // Depth/stencil aspect selection: Metal exposes the stencil plane of
    // combined formats through the X32_Stencil8 view formats.
    if (desc.aspect == TextureAspect::Stencil && tex->texture->pixelFormat() == MTL::PixelFormatDepth32Float_Stencil8)
    {
        pixelFormat = MTL::PixelFormatX32_Stencil8;
    }

    MTL::TextureType viewType = tex->texture->textureType();
    switch (desc.viewType)
    {
    case TextureViewType::View2D:        viewType = MTL::TextureType2D; break;
    case TextureViewType::View2DArray:   viewType = MTL::TextureType2DArray; break;
    case TextureViewType::ViewCube:      viewType = MTL::TextureTypeCube; break;
    case TextureViewType::ViewCubeArray: viewType = MTL::TextureTypeCubeArray; break;
    case TextureViewType::View3D:        viewType = MTL::TextureType3D; break;
    }
    if (tex->sampleCount > 1)
    {
        viewType = MTL::TextureType2DMultisample;
    }

    const uint32_t levelCount = desc.levelCount != 0 ? desc.levelCount : tex->mipLevels - desc.baseMip;
    const uint32_t layerCount = desc.layerCount != 0 ? desc.layerCount : tex->arrayLayers - desc.baseLayer;

    MTL::Texture* view = nullptr;
    const bool hasSwizzle = desc.r != TextureSwizzle::Identity || desc.g != TextureSwizzle::Identity ||
                            desc.b != TextureSwizzle::Identity || desc.a != TextureSwizzle::Identity;
    if (hasSwizzle)
    {
        auto toMtlSwizzle = [](TextureSwizzle s, MTL::TextureSwizzle identity) {
            switch (s)
            {
            case TextureSwizzle::Zero: return MTL::TextureSwizzleZero;
            case TextureSwizzle::One:  return MTL::TextureSwizzleOne;
            case TextureSwizzle::R:    return MTL::TextureSwizzleRed;
            case TextureSwizzle::G:    return MTL::TextureSwizzleGreen;
            case TextureSwizzle::B:    return MTL::TextureSwizzleBlue;
            case TextureSwizzle::A:    return MTL::TextureSwizzleAlpha;
            case TextureSwizzle::Identity:
            default:                   return identity;
            }
        };
        const MTL::TextureSwizzleChannels channels = MTL::TextureSwizzleChannels(
            toMtlSwizzle(desc.r, MTL::TextureSwizzleRed), toMtlSwizzle(desc.g, MTL::TextureSwizzleGreen),
            toMtlSwizzle(desc.b, MTL::TextureSwizzleBlue), toMtlSwizzle(desc.a, MTL::TextureSwizzleAlpha));
        view = tex->texture->newTextureView(pixelFormat, viewType, NS::Range(desc.baseMip, levelCount),
                                            NS::Range(desc.baseLayer, layerCount), channels);
    }
    else
    {
        view = tex->texture->newTextureView(pixelFormat, viewType, NS::Range(desc.baseMip, levelCount),
                                            NS::Range(desc.baseLayer, layerCount));
    }
    if (view == nullptr)
    {
        Logger::Log::Error("MetalDevice::CreateTextureView: view creation failed for '{}'", tex->debugName);
        return INVALID_TEXTURE_VIEW_HANDLE;
    }

    MetalTextureView wrapper{};
    wrapper.view = view;
    wrapper.format = format;
    if (desc.debugName != nullptr)
    {
        wrapper.debugName = desc.debugName;
        view->setLabel(MakeNSString(wrapper.debugName));
    }
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    return FromGeneric<TextureViewTag>(m_TextureViews.Create(std::move(wrapper)));
}

void MetalDevice::DestroyTextureView(TextureViewHandle handle)
{
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    MetalTextureView* view = m_TextureViews.Get(ToGeneric(handle));
    if (view == nullptr)
    {
        return;
    }
    ScrubResidency(view->view);
    DeferRelease(view->view);
    view->view = nullptr;
    m_TextureViews.Destroy(ToGeneric(handle));
}

TextureFormat MetalDevice::GetTextureFormat(TextureHandle texture) const
{
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    const MetalTexture* tex = m_Textures.Get(ToGeneric(texture));
    return tex != nullptr ? tex->format : TextureFormat::Unknown;
}

uint32_t MetalDevice::GetTextureSampleCount(TextureHandle texture) const
{
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    const MetalTexture* tex = m_Textures.Get(ToGeneric(texture));
    return tex != nullptr ? tex->sampleCount : 1u;
}

uint32_t MetalDevice::GetTextureArrayLayers(TextureHandle texture) const
{
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    const MetalTexture* tex = m_Textures.Get(ToGeneric(texture));
    return tex != nullptr ? tex->arrayLayers : 1u;
}

// Without this override the base IDevice stub returns 0x0, so the render graph
// imports the swapchain backbuffer with extent 0x0 — the editor then creates its
// FinalLinear composite at 1x1 (the >0?value:1u fallback), which both blacks the
// window and yields 1x1 screenshots. MetalTexture stores live dims (refreshed for
// swapchain slots at acquire), so a plain lookup covers normal + swapchain textures.
void MetalDevice::GetTextureSize(TextureHandle texture, uint32_t& outWidth, uint32_t& outHeight) const
{
    outWidth = 0;
    outHeight = 0;
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    const MetalTexture* tex = m_Textures.Get(ToGeneric(texture));
    if (tex != nullptr)
    {
        outWidth = tex->width;
        outHeight = tex->height;
    }
}

// ---------------------------------------------------------------------------
// Samplers
// ---------------------------------------------------------------------------

SamplerHandle MetalDevice::CreateSampler(const SamplerDesc& desc)
{
    if (m_Device == nullptr)
    {
        return INVALID_SAMPLER_HANDLE;
    }
    MTL::SamplerDescriptor* sd = MTL::SamplerDescriptor::alloc()->init();
    sd->setMinFilter(MetalMappings::ToMTLMinMagFilter(desc.minFilter));
    sd->setMagFilter(MetalMappings::ToMTLMinMagFilter(desc.magFilter));
    sd->setMipFilter(MetalMappings::ToMTLMipFilter(desc.mipFilter));
    sd->setSAddressMode(MetalMappings::ToMTLAddressMode(desc.addressModeU));
    sd->setTAddressMode(MetalMappings::ToMTLAddressMode(desc.addressModeV));
    sd->setRAddressMode(MetalMappings::ToMTLAddressMode(desc.addressModeW));
    sd->setLodMinClamp(desc.minLod);
    sd->setLodMaxClamp(desc.maxLod);
    sd->setMaxAnisotropy(static_cast<NS::UInteger>(std::max(1.0f, desc.maxAnisotropy)));
    // Required for writing the sampler's gpuResourceID into argument buffers.
    sd->setSupportArgumentBuffers(true);
    sd->setBorderColor(desc.borderColor == 1 ? MTL::SamplerBorderColorOpaqueWhite
                                             : MTL::SamplerBorderColorOpaqueBlack);
    if (desc.compareEnable)
    {
        sd->setCompareFunction(MetalMappings::ToMTLCompareFunction(desc.compareOp));
    }
    if (desc.debugName != nullptr)
    {
        sd->setLabel(MakeNSString(desc.debugName));
    }

    MTL::SamplerState* sampler = m_Device->newSamplerState(sd);
    sd->release();
    if (sampler == nullptr)
    {
        Logger::Log::Error("MetalDevice::CreateSampler: creation failed");
        return INVALID_SAMPLER_HANDLE;
    }

    MetalSampler wrapper{};
    wrapper.sampler = sampler;
    if (desc.debugName != nullptr)
    {
        wrapper.debugName = desc.debugName;
    }
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    return FromGeneric<SamplerTag>(m_Samplers.Create(std::move(wrapper)));
}

void MetalDevice::DestroySampler(SamplerHandle handle)
{
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    MetalSampler* sampler = m_Samplers.Get(ToGeneric(handle));
    if (sampler == nullptr)
    {
        return;
    }
    DeferRelease(sampler->sampler);
    sampler->sampler = nullptr;
    m_Samplers.Destroy(ToGeneric(handle));
}

// ---------------------------------------------------------------------------
// Pipelines
// ---------------------------------------------------------------------------

PipelineHandle MetalDevice::CreateConcreteGraphicsPipeline(const GraphicsPipelineDesc& gd, const PipelineFormatKey& fk)
{
    // Pipeline builds run on job workers, which have no autorelease pool of their
    // own: without this one the autoreleased MSL source string (MakeNSString) and
    // compile errors of every worker-side build stay alive until the thread exits.
    const NS::SharedPtr<NS::AutoreleasePool> autoreleasePool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());
    const bool isMesh = (gd.Kind == GraphicsPipelineKind::MeshFragment);
    if (m_Device == nullptr)
    {
        return INVALID_PIPELINE_HANDLE;
    }
    if (isMesh ? (!gd.MeshShader || gd.MeshShader->empty())
               : (!gd.VertexShader || gd.VertexShader->empty()))
    {
        return INVALID_PIPELINE_HANDLE;
    }

    // Snapshot layout descs by value under the cache mutex: pipeline creation
    // runs on prewarm job threads concurrently with interning, which can
    // reallocate the storage Lookup* pointers alias.
    std::vector<DescriptorSetLayoutDesc> layoutValues;
    layoutValues.reserve(gd.DescriptorSetLayouts.size());
    for (DescriptorSetLayoutId id : gd.DescriptorSetLayouts)
    {
        DescriptorSetLayoutDesc layout{};
        CopyDescriptorSetLayout(id, layout);
        layoutValues.push_back(std::move(layout));
    }
    std::vector<const DescriptorSetLayoutDesc*> setLayouts;
    setLayouts.reserve(layoutValues.size());
    for (const DescriptorSetLayoutDesc& layout : layoutValues)
    {
        setLayouts.push_back(&layout);
    }
    const SpecializationConstants* specialization =
        gd.Specialization.has_value() ? &gd.Specialization.value() : nullptr;

    const auto creationStart = std::chrono::steady_clock::now();
    double translateMs = 0.0;
    double libraryMs = 0.0;
    // Mesh stage's workgroup size (threads per mesh threadgroup), captured during
    // translation for DrawMeshTasks. Defaults to 1 for hand-written MSL.
    uint32_t meshThreadsX = 1, meshThreadsY = 1, meshThreadsZ = 1;
    auto msSince = [](std::chrono::steady_clock::time_point start) {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    };

    // Each stage is either SPIR-V (translated through SPIRV-Cross, entry
    // "main0") or raw MSL text (bring-up/test path, entry "<stage>_main").
    auto compileStage = [&](const std::vector<uint8_t>& source, MetalShaderStage stage,
                            const char* rawEntry) -> MTL::Function* {
        std::string msl;
        std::string entry = rawEntry;
        if (IsSpirv(source))
        {
            const auto translateStart = std::chrono::steady_clock::now();
            MetalShaderTranslation translation = TranslateSpirvToMsl(source, stage, setLayouts, specialization);
            translateMs += msSince(translateStart);
            if (!translation.Success)
            {
                Logger::Log::Error("MetalDevice: SPIR-V->MSL translation failed for '{}': {}", gd.DebugName,
                                   translation.Error);
                DumpMslTo("ge_msl_translate_fail_", translation.Error, gd.DebugName);
                return nullptr;
            }
            msl = std::move(translation.Msl);
            entry = translation.EntryPoint;
            if (stage == MetalShaderStage::Mesh)
            {
                meshThreadsX = std::max(1u, translation.LocalSizeX);
                meshThreadsY = std::max(1u, translation.LocalSizeY);
                meshThreadsZ = std::max(1u, translation.LocalSizeZ);
            }
            if (ShouldDumpAllMsl())
            {
                const char* dumpPrefix = "ge_msl_fs_";
                if (stage == MetalShaderStage::Vertex) dumpPrefix = "ge_msl_vs_";
                else if (stage == MetalShaderStage::Mesh) dumpPrefix = "ge_msl_ms_";
                else if (stage == MetalShaderStage::Object) dumpPrefix = "ge_msl_os_";
                DumpMslTo(dumpPrefix, msl, gd.DebugName);
            }
        }
        else
        {
            msl.assign(reinterpret_cast<const char*>(source.data()), source.size());
        }
        NS::Error* error = nullptr;
        MTL::CompileOptions* compileOptions = MakeCompileOptions();
        const auto libraryStart = std::chrono::steady_clock::now();
        MTL::Library* library = m_Device->newLibrary(MakeNSString(msl), compileOptions, &error);
        libraryMs += msSince(libraryStart);
        compileOptions->release();
        if (library == nullptr)
        {
            Logger::Log::Error("MetalDevice: MSL compile failed for '{}': {}", gd.DebugName,
                               error != nullptr ? error->localizedDescription()->utf8String() : "unknown error");
            DumpFailedMsl(msl, gd.DebugName);
            return nullptr;
        }
        MTL::Function* fn = NewSpecializedFunction(library, entry.c_str());
        library->release();
        if (fn == nullptr)
        {
            Logger::Log::Error("MetalDevice: entry point '{}' not found in MSL source for '{}'", entry, gd.DebugName);
        }
        return fn;
    };

    MTL::RenderPipelineState* pso = nullptr;
    if (isMesh)
    {
        // Mesh pipeline: object (optional amplification) + mesh + fragment, built
        // through MTL::MeshRenderPipelineDescriptor instead of the vertex path.
        MTL::Function* meshFn = compileStage(*gd.MeshShader, MetalShaderStage::Mesh, "mesh_main");
        if (meshFn == nullptr)
        {
            return INVALID_PIPELINE_HANDLE;
        }
        MTL::Function* objectFn = nullptr;
        if (gd.AmplificationShader && !gd.AmplificationShader->empty())
        {
            objectFn = compileStage(*gd.AmplificationShader, MetalShaderStage::Object, "object_main");
            if (objectFn == nullptr)
            {
                meshFn->release();
                return INVALID_PIPELINE_HANDLE;
            }
        }
        MTL::Function* meshFragmentFn = nullptr;
        if (gd.PixelShader && !gd.PixelShader->empty())
        {
            meshFragmentFn = compileStage(*gd.PixelShader, MetalShaderStage::Fragment, "fragment_main");
            if (meshFragmentFn == nullptr)
            {
                meshFn->release();
                if (objectFn != nullptr) objectFn->release();
                return INVALID_PIPELINE_HANDLE;
            }
        }

        MTL::MeshRenderPipelineDescriptor* md = MTL::MeshRenderPipelineDescriptor::alloc()->init();
        if (objectFn != nullptr) md->setObjectFunction(objectFn);
        md->setMeshFunction(meshFn);
        if (meshFragmentFn != nullptr) md->setFragmentFunction(meshFragmentFn);
        // Required for mesh pipelines: the max threads a mesh (and object)
        // threadgroup may launch. Derived from the shader's workgroup size; a
        // mesh-only pipeline keeps a single-thread object stage.
        md->setMaxTotalThreadsPerMeshThreadgroup(
            std::max(1u, meshThreadsX * meshThreadsY * meshThreadsZ));
        if (!gd.DebugName.empty()) md->setLabel(MakeNSString(gd.DebugName));
        md->setRasterSampleCount(std::max<uint8_t>(1, fk.RasterizationSamples));
        for (uint8_t i = 0; i < fk.ColorCount && i < PipelineFormatKey::kMaxColors; ++i)
        {
            MTL::RenderPipelineColorAttachmentDescriptor* att = md->colorAttachments()->object(i);
            att->setPixelFormat(MetalMappings::ToMTLPixelFormat(fk.ColorFormats[i]));
            if (i < gd.ColorBlend.attachments.size())
            {
                const ColorBlendAttachmentState& blend = gd.ColorBlend.attachments[i];
                att->setBlendingEnabled(blend.blendEnable);
                att->setSourceRGBBlendFactor(MetalMappings::ToMTLBlendFactor(blend.srcColorBlendFactor));
                att->setDestinationRGBBlendFactor(MetalMappings::ToMTLBlendFactor(blend.dstColorBlendFactor));
                att->setRgbBlendOperation(MetalMappings::ToMTLBlendOperation(blend.colorBlendOp));
                att->setSourceAlphaBlendFactor(MetalMappings::ToMTLBlendFactor(blend.srcAlphaBlendFactor));
                att->setDestinationAlphaBlendFactor(MetalMappings::ToMTLBlendFactor(blend.dstAlphaBlendFactor));
                att->setAlphaBlendOperation(MetalMappings::ToMTLBlendOperation(blend.alphaBlendOp));
                att->setWriteMask(MetalMappings::ToMTLColorWriteMask(blend.colorWriteMask));
            }
        }
        if (static_cast<uint32_t>(fk.DepthFormat) != 0)
            md->setDepthAttachmentPixelFormat(MetalMappings::ToMTLPixelFormat(fk.DepthFormat));
        if (static_cast<uint32_t>(fk.StencilFormat) != 0)
            md->setStencilAttachmentPixelFormat(MetalMappings::ToMTLPixelFormat(fk.StencilFormat));

        NS::Error* meshError = nullptr;
        pso = m_Device->newRenderPipelineState(md, MTL::PipelineOptionNone, nullptr, &meshError);
        md->release();
        meshFn->release();
        if (objectFn != nullptr) objectFn->release();
        if (meshFragmentFn != nullptr) meshFragmentFn->release();
        if (pso == nullptr)
        {
            Logger::Log::Error("MetalDevice: mesh pipeline creation failed for '{}': {}", gd.DebugName,
                               meshError != nullptr ? meshError->localizedDescription()->utf8String() : "unknown error");
            return INVALID_PIPELINE_HANDLE;
        }
    }
    else
    {
    MTL::Function* vertexFn = compileStage(*gd.VertexShader, MetalShaderStage::Vertex, "vertex_main");
    if (vertexFn == nullptr)
    {
        return INVALID_PIPELINE_HANDLE;
    }
    MTL::Function* fragmentFn = nullptr;
    if (gd.PixelShader && !gd.PixelShader->empty())
    {
        fragmentFn = compileStage(*gd.PixelShader, MetalShaderStage::Fragment, "fragment_main");
        if (fragmentFn == nullptr)
        {
            vertexFn->release();
            return INVALID_PIPELINE_HANDLE;
        }
    }
    NS::Error* error = nullptr;

    MTL::RenderPipelineDescriptor* pd = MTL::RenderPipelineDescriptor::alloc()->init();
    pd->setVertexFunction(vertexFn);
    if (fragmentFn != nullptr)
    {
        pd->setFragmentFunction(fragmentFn);
    }
    if (!gd.DebugName.empty())
    {
        pd->setLabel(MakeNSString(gd.DebugName));
    }
    pd->setRasterSampleCount(std::max<uint8_t>(1, fk.RasterizationSamples));
    pd->setAlphaToCoverageEnabled(gd.ColorBlend.alphaToCoverageEnable);
    // DrawIndexedIndirectCount runs indirect command buffer commands that
    // inherit this pipeline from the render encoder; Metal requires the
    // pipeline to opt in, and any graphics pipeline may issue such a draw.
    pd->setSupportIndirectCommandBuffers(true);

    for (uint8_t i = 0; i < fk.ColorCount && i < PipelineFormatKey::kMaxColors; ++i)
    {
        MTL::RenderPipelineColorAttachmentDescriptor* att = pd->colorAttachments()->object(i);
        att->setPixelFormat(MetalMappings::ToMTLPixelFormat(fk.ColorFormats[i]));
        if (i < gd.ColorBlend.attachments.size())
        {
            const ColorBlendAttachmentState& blend = gd.ColorBlend.attachments[i];
            att->setBlendingEnabled(blend.blendEnable);
            att->setSourceRGBBlendFactor(MetalMappings::ToMTLBlendFactor(blend.srcColorBlendFactor));
            att->setDestinationRGBBlendFactor(MetalMappings::ToMTLBlendFactor(blend.dstColorBlendFactor));
            att->setRgbBlendOperation(MetalMappings::ToMTLBlendOperation(blend.colorBlendOp));
            att->setSourceAlphaBlendFactor(MetalMappings::ToMTLBlendFactor(blend.srcAlphaBlendFactor));
            att->setDestinationAlphaBlendFactor(MetalMappings::ToMTLBlendFactor(blend.dstAlphaBlendFactor));
            att->setAlphaBlendOperation(MetalMappings::ToMTLBlendOperation(blend.alphaBlendOp));
            att->setWriteMask(MetalMappings::ToMTLColorWriteMask(blend.colorWriteMask));
        }
    }
    if (static_cast<uint32_t>(fk.DepthFormat) != 0)
    {
        pd->setDepthAttachmentPixelFormat(MetalMappings::ToMTLPixelFormat(fk.DepthFormat));
    }
    if (static_cast<uint32_t>(fk.StencilFormat) != 0)
    {
        pd->setStencilAttachmentPixelFormat(MetalMappings::ToMTLPixelFormat(fk.StencilFormat));
    }

    if (!gd.VertexAttributes.empty())
    {
        // Vertex buffers live at the top of Metal's buffer table (29 - slot)
        // so the low indices stay free for descriptor-set argument buffers.
        MTL::VertexDescriptor* vd = MTL::VertexDescriptor::alloc()->init();
        for (const VertexInputAttribute& attr : gd.VertexAttributes)
        {
            MTL::VertexAttributeDescriptor* a = vd->attributes()->object(attr.location);
            a->setFormat(MetalMappings::ToMTLVertexFormat(attr.format));
            a->setOffset(attr.offset);
            a->setBufferIndex(MetalVertexBufferIndex(attr.binding));
        }
        for (const VertexInputBinding& binding : gd.VertexBindings)
        {
            // A layout for a buffer no attribute reads aborts pipeline creation in Metal's
            // vertex descriptor; Vulkan accepts such a binding, and the shared depth variants
            // keep the bindings of streams their shaders do not read (Joints1 / Weights1).
            const bool read = std::any_of(gd.VertexAttributes.begin(), gd.VertexAttributes.end(),
                                          [&binding](const VertexInputAttribute& attr)
                                          { return attr.binding == binding.binding; });
            if (!read)
            {
                continue;
            }
            MTL::VertexBufferLayoutDescriptor* layout = vd->layouts()->object(MetalVertexBufferIndex(binding.binding));
            layout->setStride(binding.stride);
            layout->setStepFunction(binding.inputRate == 0 ? MTL::VertexStepFunctionPerVertex
                                                           : MTL::VertexStepFunctionPerInstance);
        }
        pd->setVertexDescriptor(vd);
        vd->release();
    }

    error = nullptr;
    const auto psoStart = std::chrono::steady_clock::now();
    pso = m_Device->newRenderPipelineState(pd, &error);
    if (DumpPipelineTiming())
    {
        Logger::Log::Warning("MetalPipelineTiming: '{}' total={:.1f}ms translate={:.1f}ms library={:.1f}ms pso={:.1f}ms",
                             gd.DebugName, msSince(creationStart), translateMs, libraryMs, msSince(psoStart));
    }
    pd->release();
    vertexFn->release();
    if (fragmentFn != nullptr)
    {
        fragmentFn->release();
    }
    if (pso == nullptr)
    {
        Logger::Log::Error("MetalDevice: render pipeline creation failed for '{}': {}", gd.DebugName,
                           error != nullptr ? error->localizedDescription()->utf8String() : "unknown error");
        return INVALID_PIPELINE_HANDLE;
    }
    } // end vertex-fragment path

    MetalPipeline wrapper{};
    wrapper.renderPipeline = pso;
    wrapper.type = PipelineType::Graphics;
    wrapper.isMeshPipeline = isMesh;
    if (isMesh)
    {
        wrapper.localSizeX = meshThreadsX;
        wrapper.localSizeY = meshThreadsY;
        wrapper.localSizeZ = meshThreadsZ;
    }
    wrapper.formatKey = fk;
    for (const DescriptorSetLayoutDesc& layout : layoutValues)
    {
        wrapper.setLayouts.push_back(ComputeMetalArgumentBufferLayout(layout));
    }
    wrapper.primitiveType = MetalMappings::ToMTLPrimitiveType(gd.Topology);
    wrapper.cullMode = MetalMappings::ToMTLCullMode(gd.Rasterization.cullMode);
    wrapper.winding = MetalMappings::ToMTLWinding(gd.Rasterization.frontFace);
    wrapper.fillMode = MetalMappings::ToMTLFillMode(gd.Rasterization.polygonMode);
    wrapper.depthBiasEnable = gd.Rasterization.depthBiasEnable;
    wrapper.depthBiasConstant = gd.Rasterization.depthBiasConstantFactor;
    wrapper.depthBiasSlope = gd.Rasterization.depthBiasSlopeFactor;
    wrapper.depthBiasClamp = gd.Rasterization.depthBiasClamp;
    wrapper.depthClipMode = gd.Rasterization.depthClampEnable ? MTL::DepthClipModeClamp
                                                              : MTL::DepthClipModeClip;
    wrapper.debugName = gd.DebugName;

    // Always build a depth-stencil state, even when both test and write are off.
    // A null state would leave the render encoder on whatever DSS the previous
    // draw in the pass bound (e.g. the depth-testing grid that shares the gizmo
    // overlay pass), so a depth-disabled draw like the transform gizmo would
    // inherit that test and get occluded by scene geometry. (Vulkan never hits
    // this because depthTestEnable lives in the PSO itself.) depthTestEnable=false
    // maps to CompareFunctionAlways — no effective test — and the read-only-depth
    // bind path also reads wrapper.depthCompare, so it must be Always too.
    wrapper.depthCompare = gd.DepthStencil.depthTestEnable
                               ? MetalMappings::ToMTLCompareFunction(gd.DepthStencil.depthCompareOp)
                               : MTL::CompareFunctionAlways;
    {
        MTL::DepthStencilDescriptor* dsd = MTL::DepthStencilDescriptor::alloc()->init();
        dsd->setDepthCompareFunction(wrapper.depthCompare);
        dsd->setDepthWriteEnabled(gd.DepthStencil.depthWriteEnable);
        wrapper.depthStencilState = m_Device->newDepthStencilState(dsd);
        dsd->release();
    }
    if (ShouldDumpAllMsl())
    {
        Logger::Log::Warning(
            "MetalPipelineDSS '{}': test={} write={} compare={} biasEnable={} biasConst={} biasSlope={}",
            gd.DebugName, gd.DepthStencil.depthTestEnable, gd.DepthStencil.depthWriteEnable,
            static_cast<int>(gd.DepthStencil.depthCompareOp), gd.Rasterization.depthBiasEnable,
            gd.Rasterization.depthBiasConstantFactor, gd.Rasterization.depthBiasSlopeFactor);
    }

    wrapper.pushConstantSize = gd.PushConstants.Size;
    wrapper.pushConstantStagesMask = gd.PushConstants.StageMask;
    for (const NamedPushConstantRange& range : gd.NamedPushConstantRanges)
    {
        wrapper.pushRanges.push_back({range.Name, range.Offset, range.Size, range.StageMask});
    }

    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    return FromGeneric<PipelineTag>(m_Pipelines.Create(std::move(wrapper)));
}

PipelineHandle MetalDevice::CreateConcreteComputePipeline(const ComputePipelineDesc& cd)
{
    // See CreateConcreteGraphicsPipeline.
    const NS::SharedPtr<NS::AutoreleasePool> autoreleasePool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());
    if (m_Device == nullptr || !cd.ComputeShader || cd.ComputeShader->empty())
    {
        return INVALID_PIPELINE_HANDLE;
    }

    const auto creationStart = std::chrono::steady_clock::now();
    std::string msl;
    std::string entry = "compute_main";
    uint32_t localSizeX = 0;
    uint32_t localSizeY = 0;
    uint32_t localSizeZ = 0;
    std::vector<DescriptorSetLayoutDesc> layoutValues;
    layoutValues.reserve(cd.DescriptorSetLayouts.size());
    for (DescriptorSetLayoutId id : cd.DescriptorSetLayouts)
    {
        DescriptorSetLayoutDesc layout{};
        CopyDescriptorSetLayout(id, layout);
        layoutValues.push_back(std::move(layout));
    }
    if (IsSpirv(*cd.ComputeShader))
    {
        std::vector<const DescriptorSetLayoutDesc*> setLayouts;
        setLayouts.reserve(layoutValues.size());
        for (const DescriptorSetLayoutDesc& layout : layoutValues)
        {
            setLayouts.push_back(&layout);
        }
        const SpecializationConstants* specialization =
            cd.Specialization.has_value() ? &cd.Specialization.value() : nullptr;
        MetalShaderTranslation translation =
            TranslateSpirvToMsl(*cd.ComputeShader, MetalShaderStage::Compute, setLayouts, specialization);
        if (!translation.Success)
        {
            Logger::Log::Error("MetalDevice: SPIR-V->MSL translation failed for compute '{}': {}", cd.DebugName,
                               translation.Error);
            DumpMslTo("ge_msl_translate_fail_", translation.Error, cd.DebugName);
            return INVALID_PIPELINE_HANDLE;
        }
        msl = std::move(translation.Msl);
        entry = translation.EntryPoint;
        localSizeX = translation.LocalSizeX;
        localSizeY = translation.LocalSizeY;
        localSizeZ = translation.LocalSizeZ;
    }
    else
    {
        msl.assign(reinterpret_cast<const char*>(cd.ComputeShader->data()), cd.ComputeShader->size());
    }

    NS::Error* error = nullptr;
    MTL::CompileOptions* compileOptions = MakeCompileOptions();
    MTL::Library* library = m_Device->newLibrary(MakeNSString(msl), compileOptions, &error);
    compileOptions->release();
    if (library == nullptr)
    {
        Logger::Log::Error("MetalDevice: MSL compile failed for compute '{}': {}", cd.DebugName,
                           error != nullptr ? error->localizedDescription()->utf8String() : "unknown error");
        DumpFailedMsl(msl, cd.DebugName);
        return INVALID_PIPELINE_HANDLE;
    }
    MTL::Function* fn = NewSpecializedFunction(library, entry.c_str());
    library->release();
    if (fn == nullptr)
    {
        Logger::Log::Error("MetalDevice: entry point '{}' not found for '{}'", entry, cd.DebugName);
        return INVALID_PIPELINE_HANDLE;
    }

    error = nullptr;
    const auto psoStart = std::chrono::steady_clock::now();
    MTL::ComputePipelineState* pso = m_Device->newComputePipelineState(fn, &error);
    fn->release();
    if (DumpPipelineTiming())
    {
        const auto ms = [](auto from, auto to) { return std::chrono::duration<double, std::milli>(to - from).count(); };
        const auto now = std::chrono::steady_clock::now();
        Logger::Log::Warning("MetalPipelineTiming: compute '{}' total={:.1f}ms pso={:.1f}ms thread={}", cd.DebugName,
                             ms(creationStart, now), ms(psoStart, now),
                             std::hash<std::thread::id>{}(std::this_thread::get_id()));
    }
    if (pso == nullptr)
    {
        Logger::Log::Error("MetalDevice: compute pipeline creation failed for '{}': {}", cd.DebugName,
                           error != nullptr ? error->localizedDescription()->utf8String() : "unknown error");
        return INVALID_PIPELINE_HANDLE;
    }

    MetalPipeline wrapper{};
    wrapper.computePipeline = pso;
    wrapper.type = PipelineType::Compute;
    for (const DescriptorSetLayoutDesc& layout : layoutValues)
    {
        wrapper.setLayouts.push_back(ComputeMetalArgumentBufferLayout(layout));
    }
    wrapper.localSizeX = localSizeX;
    wrapper.localSizeY = localSizeY;
    wrapper.localSizeZ = localSizeZ;
    wrapper.debugName = cd.DebugName;
    wrapper.pushConstantSize = cd.PushConstants.Size;
    wrapper.pushConstantStagesMask = cd.PushConstants.StageMask;
    for (const NamedPushConstantRange& range : cd.NamedPushConstantRanges)
    {
        wrapper.pushRanges.push_back({range.Name, range.Offset, range.Size, range.StageMask});
    }

    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    return FromGeneric<PipelineTag>(m_Pipelines.Create(std::move(wrapper)));
}

void MetalDevice::DestroyPipeline(PipelineHandle handle)
{
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    MetalPipeline* pipeline = m_Pipelines.Get(ToGeneric(handle));
    if (pipeline == nullptr)
    {
        return;
    }
    DeferRelease(pipeline->renderPipeline);
    DeferRelease(pipeline->computePipeline);
    DeferRelease(pipeline->depthStencilState);
    pipeline->renderPipeline = nullptr;
    pipeline->computePipeline = nullptr;
    pipeline->depthStencilState = nullptr;
    m_Pipelines.Destroy(ToGeneric(handle));
}

// ---------------------------------------------------------------------------
// Descriptor sets — Metal 3 argument buffers (Tier2 raw 8-byte slot encoding)
// ---------------------------------------------------------------------------

namespace
{
// Bytes of the entries of a set's argument table: one per resource slot plus
// the size-constants address at SizeConstantsSlot (MetalShaderTranslator
// reserves that id for SPIRV-Cross's kBufferSizeBufferBinding). The 4-byte
// sizes, one per resource slot, follow the entries.
size_t ArgumentTableEntryBytes(const MetalArgumentBufferLayout& layout)
{
    return (static_cast<size_t>(layout.SizeConstantsSlot) + 1) * sizeof(uint64_t);
}

size_t ArgumentTableBytes(const MetalArgumentBufferLayout& layout)
{
    return ArgumentTableEntryBytes(layout) + std::max<size_t>(1, layout.TotalSlotCount) * sizeof(uint32_t);
}

// Points `set` at its zeroed table at `data` (GPU address `gpuAddress`) and
// stores the sizes' address in the size-constants entry.
void AttachArgumentTable(MetalDescriptorSet& set, uint8_t* data, uint64_t gpuAddress)
{
    const size_t entryBytes = ArgumentTableEntryBytes(set.layout);
    set.entries = reinterpret_cast<uint64_t*>(data);
    set.sizeConstants = reinterpret_cast<uint32_t*>(data + entryBytes);
    set.entries[set.layout.SizeConstantsSlot] = gpuAddress + entryBytes;
}
} // namespace

DescriptorSetHandle MetalDevice::CreateDescriptorSet(const DescriptorSetDesc& desc)
{
    if (m_Device == nullptr)
    {
        return DescriptorSetHandle{};
    }
    MetalDescriptorSet set{};
    set.layout = ComputeMetalArgumentBufferLayout(desc.layout);
    set.transient = desc.transient;
    if (ShouldDumpAllMsl() && desc.debugName != nullptr)
    {
        std::string mapping;
        for (const MetalArgumentSlot& slot : set.layout.Slots)
        {
            mapping += " b" + std::to_string(slot.Binding) + "(t" + std::to_string(slot.Type == DescriptorType::UniformBuffer ? 0 : static_cast<int>(slot.Type)) +
                       ",buf=" + std::to_string(slot.BufferId) + ",tex=" + std::to_string(slot.TextureId) +
                       ",smp=" + std::to_string(slot.SamplerId) + ",n=" + std::to_string(slot.Count) + ")";
        }
        Logger::Log::Warning("MetalSetLayout '{}': total={}{}", desc.debugName, set.layout.TotalSlotCount, mapping);
    }
    const size_t tableBytes = ArgumentTableBytes(set.layout);
    set.residentResources.resize(set.layout.TotalSlotCount, nullptr);
    set.residentWritable.resize(set.layout.TotalSlotCount, 0);
    set.residentIndirectArguments.resize(set.layout.TotalSlotCount, 0);
    if (desc.debugName != nullptr)
    {
        set.debugName = desc.debugName;
    }

    if (!desc.transient)
    {
        set.argumentBuffer = m_Device->newBuffer(tableBytes, MTL::ResourceStorageModeShared);
        if (set.argumentBuffer == nullptr)
        {
            return DescriptorSetHandle{};
        }
        set.ownsArgumentBuffer = true;
        if (desc.debugName != nullptr)
        {
            set.argumentBuffer->setLabel(MakeNSString(set.debugName));
        }
        auto* data = static_cast<uint8_t*>(set.argumentBuffer->contents());
        std::memset(data, 0, tableBytes);
        AttachArgumentTable(set, data, set.argumentBuffer->gpuAddress());
        std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
        return FromGeneric<DescriptorSetTag>(m_DescriptorSets.Create(std::move(set)));
    }

    // The arena and the set list are indexed by the same frame slot, read once
    // under the lock that BeginFrame recycles both under.
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    const uint32_t frameSlot = m_FrameIndex.load(std::memory_order_relaxed);
    const MetalTransientDescriptorArena::Allocation table =
        m_TransientDescriptorArenas[frameSlot].Allocate(*m_Device, tableBytes);
    if (table.Buffer == nullptr)
    {
        return DescriptorSetHandle{};
    }
    set.argumentBuffer = table.Buffer;
    set.argumentOffset = table.Offset;
    AttachArgumentTable(set, table.Data, table.GpuAddress);
    const DescriptorSetHandle handle = FromGeneric<DescriptorSetTag>(m_DescriptorSets.Create(std::move(set)));
    m_TransientDescriptorSets[frameSlot].push_back(handle);
    return handle;
}

void MetalDevice::UpdateDescriptorSet(DescriptorSetHandle descriptorSet, const DescriptorSetUpdate& update)
{
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    MetalDescriptorSet* set = m_DescriptorSets.Get(ToGeneric(descriptorSet));
    if (set == nullptr || set->entries == nullptr)
    {
        return;
    }
    const MetalArgumentSlot* slot = set->layout.FindBinding(update.binding);
    if (slot == nullptr)
    {
        Logger::Log::Error("MetalDevice::UpdateDescriptorSet: binding {} not in layout", update.binding);
        return;
    }
    uint64_t* entries = set->entries;
    set->residentsDirty = true;

    auto setResident = [&](uint32_t slotId, MTL::Resource* resource, bool writable, bool indirectArguments) {
        if (slotId < set->residentResources.size())
        {
            set->residentResources[slotId] = resource;
            set->residentWritable[slotId] = writable ? 1 : 0;
            set->residentIndirectArguments[slotId] = indirectArguments ? 1 : 0;
        }
    };

    uint32_t* sizeConstants = set->sizeConstants;
    const size_t bufferCount = update.buffers.size();
    for (size_t i = 0; i < bufferCount && slot->BufferId != MetalArgumentSlot::kUnused; ++i)
    {
        MetalBuffer* buf = m_Buffers.Get(ToGeneric(update.buffers[i]));
        if (buf == nullptr || buf->buffer == nullptr)
        {
            continue;
        }
        const size_t offset = i < update.bufferOffsets.size() ? update.bufferOffsets[i] : 0;
        const size_t range = i < update.bufferRanges.size() && update.bufferRanges[i] > 0
                                 ? update.bufferRanges[i]
                                 : buf->size - offset;
        const uint32_t slotId = slot->BufferId + update.arrayElement + static_cast<uint32_t>(i);
        entries[slotId] = buf->buffer->gpuAddress() + offset;
        if (slotId < set->layout.TotalSlotCount)
        {
            sizeConstants[slotId] = static_cast<uint32_t>(range);
        }
        setResident(slotId, buf->buffer, update.type == DescriptorType::StorageBuffer,
                    (buf->usage & BufferUsage::Indirect) != BufferUsage::None);
    }

    const bool useViews = !update.textureViews.empty();
    const size_t textureCount = useViews ? update.textureViews.size() : update.textures.size();
    for (size_t i = 0; i < textureCount && slot->TextureId != MetalArgumentSlot::kUnused; ++i)
    {
        MTL::Texture* texture = nullptr;
        if (useViews)
        {
            MetalTextureView* view = m_TextureViews.Get(ToGeneric(update.textureViews[i]));
            texture = view != nullptr ? view->view : nullptr;
        }
        else
        {
            MetalTexture* tex = m_Textures.Get(ToGeneric(update.textures[i]));
            texture = tex != nullptr ? tex->texture : nullptr;
        }
        if (texture == nullptr)
        {
            continue;
        }
        const uint32_t slotId = slot->TextureId + update.arrayElement + static_cast<uint32_t>(i);
        entries[slotId] = texture->gpuResourceID()._impl;
        setResident(slotId, texture, update.type == DescriptorType::StorageImage, /*indirectArguments=*/false);
    }

    if (slot->AccelerationStructureId != MetalArgumentSlot::kUnused &&
        !update.accelerationStructures.empty())
    {
        if (m_AccelerationStructures == nullptr)
        {
            Logger::Log::Error(
                "MetalDevice::UpdateDescriptorSet: acceleration-structure write at binding {} on a device "
                "with no AS backend",
                update.binding);
            return;
        }
        for (size_t i = 0; i < update.accelerationStructures.size(); ++i)
        {
            // GetTlasDeviceAddress is the slot's MTLResourceID on this backend
            // (MetalAccelerationStructures.h documents the deliberate naming
            // mismatch) — exactly the 8 bytes an argument-buffer
            // raytracing::acceleration_structure member reads.
            const uint64_t resourceId =
                m_AccelerationStructures->GetTlasDeviceAddress(update.accelerationStructures[i]);
            if (resourceId == 0)
            {
                Logger::Log::Error(
                    "MetalDevice::UpdateDescriptorSet: TLAS slot {} has no acceleration structure yet "
                    "(binding {})",
                    update.accelerationStructures[i].id, update.binding);
                continue;
            }
            const uint32_t slotId =
                slot->AccelerationStructureId + update.arrayElement + static_cast<uint32_t>(i);
            entries[slotId] = resourceId;
            m_AccelerationStructures->CollectTlasResidentResources(update.accelerationStructures[i],
                                                                   set->extraResidentReads);
        }
    }

    for (size_t i = 0; i < update.samplers.size() && slot->SamplerId != MetalArgumentSlot::kUnused; ++i)
    {
        MetalSampler* sampler = m_Samplers.Get(ToGeneric(update.samplers[i]));
        if (sampler == nullptr || sampler->sampler == nullptr)
        {
            continue;
        }
        const uint32_t slotId = slot->SamplerId + update.arrayElement + static_cast<uint32_t>(i);
        entries[slotId] = sampler->sampler->gpuResourceID()._impl;
        // Samplers need no residency.
    }
}

void MetalDevice::DestroyDescriptorSet(DescriptorSetHandle descriptorSet)
{
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    MetalDescriptorSet* set = m_DescriptorSets.Get(ToGeneric(descriptorSet));
    if (set == nullptr)
    {
        return;
    }
    if (set->ownsArgumentBuffer)
    {
        DeferRelease(set->argumentBuffer);
    }
    set->argumentBuffer = nullptr;
    set->entries = nullptr;
    set->sizeConstants = nullptr;
    set->residentResources.clear();
    set->extraResidentReads.clear();
    m_DescriptorSets.Destroy(ToGeneric(descriptorSet));
}

} // namespace Rendering
} // namespace GameEngine
