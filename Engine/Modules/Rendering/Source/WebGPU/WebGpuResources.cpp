// Buffers, textures, views, samplers and bind groups for the WebGPU backend.

#include "WebGpuConversions.h"
#include "WebGpuDevice.h"
#include "WebGpuUnsupported.h"

#include "Logger/Logger.h"

#if !defined(__EMSCRIPTEN__)
// wgpu-native extras (immediates, DevicePoll); Dawn/emdawnwebgpu has no wgpu.h.
#include <webgpu/wgpu.h>
#endif

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace GameEngine::Rendering
{

namespace
{
// WebGPU requires buffer sizes (and mapped ranges) to be 4-byte aligned.
constexpr uint64_t kBufferSizeAlignment = 4;

// Two dirty spans closer than this are uploaded as one: a second
// wgpuQueueWriteBuffer costs more than re-sending the bytes between them.
constexpr uint64_t kDirtySpanMergeGapBytes = 4096;

// Granularity the flush audit compares at. Small enough to name the producer
// that missed a range, large enough that the hash table stays negligible
// (8 bytes per page, so a 32 MiB ring costs 64 KiB).
constexpr uint64_t kAuditPageBytes = 4096;

uint64_t AlignBufferSize(uint64_t size)
{
    return (size + kBufferSizeAlignment - 1) & ~(kBufferSizeAlignment - 1);
}

uint64_t AlignDown(uint64_t value, uint64_t alignment)
{
    return value & ~(alignment - 1);
}

// FNV-1a over one audit page. Only ever compared against itself, so the
// hash needs speed and a low accidental-collision rate, not cryptography.
uint64_t HashPage(const uint8_t* data, size_t bytes)
{
    constexpr uint64_t kFnvOffsetBasis = 1469598103934665603ull;
    constexpr uint64_t kFnvPrime = 1099511628211ull;
    uint64_t hash = kFnvOffsetBasis;
    for (size_t i = 0; i < bytes; ++i)
    {
        hash = (hash ^ data[i]) * kFnvPrime;
    }
    return hash;
}

bool EnvFlagEnabled(const char* name)
{
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0' && value[0] != '0';
}

// Upload every mapped shadow in full, ignoring declared ranges. The
// differential oracle for the range migration: a frame that renders
// differently with this set has a producer writing bytes it never declared.
constexpr const char* kFlushWholeShadowsEnvVar = "GE_WEBGPU_FLUSH_ALL";
// Report shadow bytes that changed without a covering FlushMappedRange, per
// buffer and byte range, instead of leaving the difference to be seen in
// pixels.
constexpr const char* kFlushAuditEnvVar = "GE_WEBGPU_FLUSH_AUDIT";
} // namespace

void WebGpuDevice::ReadFlushDiagnosticSettings()
{
    m_FlushWholeShadows = EnvFlagEnabled(kFlushWholeShadowsEnvVar);
    m_AuditUndeclaredWrites = EnvFlagEnabled(kFlushAuditEnvVar);
    if (m_FlushWholeShadows)
    {
        Logger::Log::Warning("WebGpuDevice: {}=1 — every mapped shadow uploads in full. "
                             "Diagnostic only; this is the cost the declared-range path removes.",
                             kFlushWholeShadowsEnvVar);
    }
    if (m_AuditUndeclaredWrites)
    {
        Logger::Log::Warning("WebGpuDevice: {}=1 — shadow writes are checked against their "
                             "declared ranges every submit. Diagnostic only; it hashes every "
                             "mapped byte per frame.",
                             kFlushAuditEnvVar);
    }
}

// ---------------------------------------------------------------------------
// Buffers
// ---------------------------------------------------------------------------

BufferHandle WebGpuDevice::CreateBuffer(const BufferDesc& desc)
{
    if (m_Device == nullptr || desc.size == 0)
    {
        return INVALID_BUFFER_HANDLE;
    }

    WGPUBufferDescriptor bufferDesc{};
    bufferDesc.label = WebGpu::MakeStringView(desc.debugName);
    bufferDesc.size = AlignBufferSize(desc.size);
    bufferDesc.usage = WebGpu::ToWgpuBufferUsage(desc.usage, desc.memoryUsage);
    bufferDesc.mappedAtCreation = false;

    WGPUBuffer buffer = wgpuDeviceCreateBuffer(m_Device, &bufferDesc);
    if (buffer == nullptr)
    {
        Logger::Log::Error("WebGpuDevice::CreateBuffer: allocation failed ({} bytes)", desc.size);
        return INVALID_BUFFER_HANDLE;
    }

    WebGpuBuffer wrapper{};
    wrapper.buffer = buffer;
    wrapper.size = bufferDesc.size;
    wrapper.usage = desc.usage;
    wrapper.memoryUsage = desc.memoryUsage;
    if (desc.debugName != nullptr)
    {
        wrapper.debugName = desc.debugName;
    }

    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    return FromGeneric<BufferTag>(m_Buffers.Create(std::move(wrapper)));
}

void WebGpuDevice::DestroyBuffer(BufferHandle handle)
{
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    WebGpuBuffer* buffer = m_Buffers.Get(ToGeneric(handle));
    if (buffer == nullptr)
    {
        return;
    }
    if (buffer->buffer != nullptr)
    {
        if (buffer->mappedForRead)
        {
            wgpuBufferUnmap(buffer->buffer);
        }
        wgpuBufferDestroy(buffer->buffer);
        wgpuBufferRelease(buffer->buffer);
        buffer->buffer = nullptr;
    }
    ForgetMappedShadow(*buffer, handle);
    m_Buffers.Destroy(ToGeneric(handle));
}

bool WebGpuDevice::IsBufferMapBusy(BufferHandle handle) const
{
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    const WebGpuBuffer* buffer = m_Buffers.Get(ToGeneric(handle));
    return buffer != nullptr && (buffer->mappedForRead || buffer->pendingReadMap != 0u);
}

void* WebGpuDevice::MapBuffer(BufferHandle handle)
{
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    WebGpuBuffer* buffer = m_Buffers.Get(ToGeneric(handle));
    if (buffer == nullptr || buffer->buffer == nullptr)
    {
        return nullptr;
    }

    if (buffer->memoryUsage == BufferMemoryUsage::Readback)
    {
        // Deferred, never blocking: pumping here yields, and a yield ends the
        // browser task that owns the swapchain texture — every later submit
        // touching the surface is then a use-after-destroy. The request parks
        // on the buffer, its callback lands on a natural inter-frame yield,
        // and the caller's own not-ready path (readbacks already gate on the
        // sync token and retry) picks the data up on a later call.
        if (buffer->pendingReadMap == 2u)
        {
            buffer->pendingReadMap = 0u;
            buffer->mappedForRead = true;
            return const_cast<void*>(wgpuBufferGetConstMappedRange(
                buffer->buffer, 0, static_cast<size_t>(buffer->size)));
        }
        if (buffer->pendingReadMap == 3u)
        {
            buffer->pendingReadMap = 0u;
            return nullptr;
        }
        if (buffer->pendingReadMap == 0u)
        {
            WGPUBufferMapCallbackInfo callbackInfo{};
            callbackInfo.mode = WGPUCallbackMode_AllowProcessEvents;
            callbackInfo.userdata1 = buffer;
            callbackInfo.callback = [](WGPUMapAsyncStatus status, WGPUStringView message, void* userdata1, void*) {
                auto* b = static_cast<WebGpuBuffer*>(userdata1);
                if (status != WGPUMapAsyncStatus_Success)
                {
                    Logger::Log::Error("WebGpuDevice::MapBuffer: map failed: {}", WebGpu::ToString(message));
                    b->pendingReadMap = 3u;
                    return;
                }
                b->pendingReadMap = 2u;
            };
            buffer->pendingReadMap = 1u;
            (void)wgpuBufferMapAsync(buffer->buffer, WGPUMapMode_Read, 0,
                                     static_cast<size_t>(buffer->size), callbackInfo);
        }
        // One yield-free drain: a map whose promise already settled completes
        // right here and costs nothing.
        wgpuInstanceProcessEvents(m_Instance);
        if (buffer->pendingReadMap == 2u)
        {
            buffer->pendingReadMap = 0u;
            buffer->mappedForRead = true;
            return const_cast<void*>(wgpuBufferGetConstMappedRange(
                buffer->buffer, 0, static_cast<size_t>(buffer->size)));
        }
        return nullptr;
    }

    // Everything else is GPU-visible, which WebGPU never maps persistently: the
    // caller writes into a CPU shadow that is flushed to the queue by
    // UnmapBuffer, or — for a mapping the caller never ends — by the submit path.
    if (buffer->shadow.size() != buffer->size)
    {
        buffer->shadow.assign(static_cast<size_t>(buffer->size), 0);
    }
    if (!buffer->shadowMappedForWrite)
    {
        buffer->shadowMappedForWrite = true;
        // A fresh mapping's extent is unknown, so it starts wholly dirty: a
        // producer that writes once and never declares a range — every
        // initialize-and-forget upload buffer — is correct without migrating.
        // A ring re-maps while already mapped and does not land here, so its
        // per-frame cost stays the ranges it declares.
        buffer->dirtyWhole = true;
        m_MappedForWriteBuffers.push_back(handle);
    }
    return buffer->shadow.data();
}

void WebGpuDevice::FlushMappedRange(BufferHandle handle, size_t offset, size_t size)
{
    if (size == 0)
    {
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    WebGpuBuffer* buffer = m_Buffers.Get(ToGeneric(handle));
    if (buffer == nullptr || buffer->shadow.empty() || buffer->dirtyWhole)
    {
        return;
    }
    const uint64_t begin = std::min<uint64_t>(offset, buffer->size);
    const uint64_t end = std::min<uint64_t>(static_cast<uint64_t>(offset) + size, buffer->size);
    if (begin >= end)
    {
        return;
    }
    buffer->dirtySpans.push_back(WebGpuDirtySpan{begin, end});
}

void WebGpuDevice::UnmapBuffer(BufferHandle handle)
{
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    WebGpuBuffer* buffer = m_Buffers.Get(ToGeneric(handle));
    if (buffer == nullptr || buffer->buffer == nullptr)
    {
        return;
    }

    if (buffer->mappedForRead)
    {
        wgpuBufferUnmap(buffer->buffer);
        buffer->mappedForRead = false;
        return;
    }

    ForgetMappedShadow(*buffer, handle);
    // An Unmap ends the mapping, so it is the producer's last chance to publish
    // — whatever it declared, plus everything if it declared nothing.
    (void)FlushBufferShadow(*buffer);
}

void WebGpuDevice::ForgetMappedShadow(WebGpuBuffer& buffer, BufferHandle handle)
{
    buffer.shadowMappedForWrite = false;
    m_MappedForWriteBuffers.erase(
        std::remove(m_MappedForWriteBuffers.begin(), m_MappedForWriteBuffers.end(), handle),
        m_MappedForWriteBuffers.end());
}

uint64_t WebGpuDevice::FlushBufferShadow(WebGpuBuffer& buffer)
{
    if (buffer.buffer == nullptr || buffer.shadow.empty())
    {
        return 0;
    }
    if (m_AuditUndeclaredWrites)
    {
        AuditUndeclaredWrites(buffer);
    }

    if (buffer.dirtyWhole || m_FlushWholeShadows)
    {
        wgpuQueueWriteBuffer(m_Queue, buffer.buffer, 0, buffer.shadow.data(), buffer.shadow.size());
        const uint64_t bytes = buffer.shadow.size();
        buffer.dirtySpans.clear();
        buffer.dirtyWhole = false;
        if (m_AuditUndeclaredWrites)
        {
            RefreshAuditHashes(buffer, 0, buffer.size);
        }
        return bytes;
    }

    if (buffer.dirtySpans.empty())
    {
        return 0;
    }

    // Coalesce: the spans arrive per allocation and a ring's are typically
    // contiguous, so sorting and merging turns a frame's allocations into one
    // or two writes. Span bounds widen to the 4-byte granularity
    // wgpuQueueWriteBuffer addresses in.
    std::sort(buffer.dirtySpans.begin(), buffer.dirtySpans.end(),
              [](const WebGpuDirtySpan& a, const WebGpuDirtySpan& b) { return a.Begin < b.Begin; });
    uint64_t totalBytes = 0;
    WebGpuDirtySpan pending{AlignDown(buffer.dirtySpans.front().Begin, kBufferSizeAlignment),
                            std::min(AlignBufferSize(buffer.dirtySpans.front().End), buffer.size)};
    auto write = [&](const WebGpuDirtySpan& span)
    {
        const uint64_t bytes = span.End - span.Begin;
        wgpuQueueWriteBuffer(m_Queue, buffer.buffer, span.Begin, buffer.shadow.data() + span.Begin,
                             static_cast<size_t>(bytes));
        totalBytes += bytes;
        if (m_AuditUndeclaredWrites)
        {
            RefreshAuditHashes(buffer, span.Begin, span.End);
        }
    };
    for (size_t i = 1; i < buffer.dirtySpans.size(); ++i)
    {
        const uint64_t begin = AlignDown(buffer.dirtySpans[i].Begin, kBufferSizeAlignment);
        const uint64_t end = std::min(AlignBufferSize(buffer.dirtySpans[i].End), buffer.size);
        if (begin <= pending.End + kDirtySpanMergeGapBytes)
        {
            pending.End = std::max(pending.End, end);
            continue;
        }
        write(pending);
        pending = WebGpuDirtySpan{begin, end};
    }
    write(pending);

    buffer.dirtySpans.clear();
    return totalBytes;
}

void WebGpuDevice::FlushMappedShadows()
{
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    uint64_t totalBytes = 0;
    for (const BufferHandle handle : m_MappedForWriteBuffers)
    {
        WebGpuBuffer* buffer = m_Buffers.Get(ToGeneric(handle));
        if (buffer == nullptr)
        {
            continue;
        }
        totalBytes += FlushBufferShadow(*buffer);
    }
    ReportShadowFlushVolume(totalBytes);
}

void WebGpuDevice::AuditUndeclaredWrites(WebGpuBuffer& buffer)
{
    const size_t pageCount = static_cast<size_t>((buffer.size + kAuditPageBytes - 1) / kAuditPageBytes);
    if (buffer.auditPageHashes.size() != pageCount)
    {
        // First audited flush of this buffer: nothing to compare against, and
        // the flush below publishes it in full anyway (a fresh mapping is
        // wholly dirty).
        buffer.auditPageHashes.assign(pageCount, 0);
        RefreshAuditHashes(buffer, 0, buffer.size);
        return;
    }
    if (buffer.dirtyWhole || m_FlushWholeShadows)
    {
        return;
    }

    auto declared = [&](uint64_t begin, uint64_t end)
    {
        for (const WebGpuDirtySpan& span : buffer.dirtySpans)
        {
            if (span.Begin < end && begin < span.End)
            {
                return true;
            }
        }
        return false;
    };

    uint64_t missedBytes = 0;
    uint64_t firstMissed = 0;
    uint64_t lastMissed = 0;
    for (size_t page = 0; page < pageCount; ++page)
    {
        const uint64_t begin = page * kAuditPageBytes;
        const uint64_t end = std::min(begin + kAuditPageBytes, buffer.size);
        if (HashPage(buffer.shadow.data() + begin, static_cast<size_t>(end - begin)) ==
            buffer.auditPageHashes[page])
        {
            continue;
        }
        if (declared(begin, end))
        {
            continue;
        }
        if (missedBytes == 0)
        {
            firstMissed = begin;
        }
        lastMissed = end;
        missedBytes += end - begin;
        // Publish it so the audited run still renders what the producer meant;
        // the log line, not a wrong pixel, is what reports the miss.
        buffer.dirtySpans.push_back(WebGpuDirtySpan{begin, end});
    }
    if (missedBytes == 0)
    {
        return;
    }
    Logger::Log::Error("WebGpuDevice: '{}' changed {} bytes with no declared range "
                       "(bytes {}..{}). The producer must call FlushMappedRange for every "
                       "region it writes, or those bytes are stale on WebGPU only.",
                       buffer.debugName, missedBytes, firstMissed, lastMissed);
}

void WebGpuDevice::RefreshAuditHashes(WebGpuBuffer& buffer, uint64_t begin, uint64_t end)
{
    if (buffer.auditPageHashes.empty())
    {
        return;
    }
    const size_t firstPage = static_cast<size_t>(begin / kAuditPageBytes);
    const size_t lastPage =
        std::min(buffer.auditPageHashes.size(), static_cast<size_t>((end + kAuditPageBytes - 1) / kAuditPageBytes));
    for (size_t page = firstPage; page < lastPage; ++page)
    {
        const uint64_t pageBegin = page * kAuditPageBytes;
        const uint64_t pageEnd = std::min(pageBegin + kAuditPageBytes, buffer.size);
        buffer.auditPageHashes[page] =
            HashPage(buffer.shadow.data() + pageBegin, static_cast<size_t>(pageEnd - pageBegin));
    }
}

void WebGpuDevice::ReportShadowFlushVolume(uint64_t totalBytes) const
{
    // Subsystems map their rings as they initialize, so an early flush sees
    // only the part of the set that exists yet. Wait until the set has settled
    // — a per-frame cost is not a fact about startup, and a partial figure here
    // would be read as the whole one.
    constexpr uint32_t kShadowFlushSettledCount = 120;
    if (m_ReportedShadowFlushVolume)
    {
        return;
    }
    if (++m_ShadowFlushCount < kShadowFlushSettledCount)
    {
        return;
    }
    m_ReportedShadowFlushVolume = true;

    // The browser copies these bytes on the main thread inside writeBuffer, so
    // this number paces the frame once it is large: a mapped ring that uploads
    // its capacity instead of its declared ranges costs tens of MiB a frame.
    // One decimal, because the declared-range steady state is a few tens of
    // KiB and integer division would report the win as "0 KiB".
    constexpr double kBytesPerKibibyte = 1024.0;
    Logger::Log::Warning("WebGpuDevice: mapped buffers upload {:.1f} KiB every frame across {} "
                         "buffers ({}).",
                         static_cast<double>(totalBytes) / kBytesPerKibibyte,
                         m_MappedForWriteBuffers.size(),
                         m_FlushWholeShadows ? "whole capacity — GE_WEBGPU_FLUSH_ALL is set"
                                             : "declared ranges only");
    // Capacity, not the flushed volume: this line is what tells the next reader
    // which ring would dominate the frame if its ranges stopped being declared.
    constexpr uint64_t kBytesPerMebibyte = 1ull << 20;
    for (const BufferHandle handle : m_MappedForWriteBuffers)
    {
        const WebGpuBuffer* buffer = m_Buffers.Get(ToGeneric(handle));
        if (buffer == nullptr || buffer->shadow.size() < kBytesPerMebibyte)
        {
            continue;
        }
        Logger::Log::Warning("WebGpuDevice:   {} MiB mapped  '{}'", buffer->shadow.size() / kBytesPerMebibyte,
                             buffer->debugName);
    }
}

void WebGpuDevice::UpdateBuffer(BufferHandle handle, size_t offset, size_t size, const void* data)
{
    if (data == nullptr || size == 0)
    {
        return;
    }

    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    WebGpuBuffer* buffer = m_Buffers.Get(ToGeneric(handle));
    if (buffer == nullptr || buffer->buffer == nullptr || offset + size > buffer->size)
    {
        return;
    }

    // wgpuQueueWriteBuffer copies at 4-byte granularity; the tail of an odd-sized
    // update is padded from the shadow (zero-initialized when absent) so the
    // write stays in range.
    const size_t alignedSize = static_cast<size_t>(AlignBufferSize(size));
    if (alignedSize == size)
    {
        wgpuQueueWriteBuffer(m_Queue, buffer->buffer, offset, data, size);
    }
    else
    {
        std::vector<uint8_t> padded(alignedSize, 0);
        std::memcpy(padded.data(), data, size);
        wgpuQueueWriteBuffer(m_Queue, buffer->buffer, offset, padded.data(), alignedSize);
    }

    // Keep the CPU shadow coherent for every buffer whose MapBuffer hands the
    // shadow out (everything but Readback) — materialized here on first
    // write, not only in MapBuffer, or a map-after-update reads zeros.
    if (buffer->memoryUsage != BufferMemoryUsage::Readback)
    {
        if (buffer->shadow.size() != buffer->size)
        {
            buffer->shadow.assign(static_cast<size_t>(buffer->size), 0);
        }
        std::memcpy(buffer->shadow.data() + offset, data, size);
        // This path already published the bytes, so they are not a missed
        // range — re-baseline them or the audit would report every update.
        if (m_AuditUndeclaredWrites)
        {
            RefreshAuditHashes(*buffer, offset, offset + size);
        }
    }
}

WebGpuBuffer* WebGpuDevice::GetBuffer(BufferHandle handle)
{
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    return m_Buffers.Get(ToGeneric(handle));
}

// ---------------------------------------------------------------------------
// Textures, views and samplers
// ---------------------------------------------------------------------------

TextureHandle WebGpuDevice::CreateTexture(const TextureDesc& desc)
{
    if (m_Device == nullptr)
    {
        return INVALID_TEXTURE_HANDLE;
    }

    const WGPUTextureFormat format = WebGpu::ToWgpuTextureFormat(static_cast<TextureFormat>(desc.format));
    if (format == WGPUTextureFormat_Undefined)
    {
        Logger::Log::Error("WebGpuDevice::CreateTexture: format {} has no WebGPU equivalent", desc.format);
        return INVALID_TEXTURE_HANDLE;
    }

    WGPUTextureDescriptor textureDesc{};
    textureDesc.label = WebGpu::MakeStringView(desc.debugName);
    textureDesc.usage = WebGpu::ToWgpuTextureUsage(desc.usage);
    textureDesc.dimension = desc.depth > 1 ? WGPUTextureDimension_3D : WGPUTextureDimension_2D;
    textureDesc.size.width = desc.width;
    textureDesc.size.height = desc.height;
    textureDesc.size.depthOrArrayLayers = desc.depth > 1 ? desc.depth : desc.arrayLayers;
    textureDesc.format = format;
    textureDesc.mipLevelCount = std::max(1u, desc.mipLevels);
    textureDesc.sampleCount = std::max(1u, desc.sampleCount);

    WGPUTexture texture = wgpuDeviceCreateTexture(m_Device, &textureDesc);
    if (texture == nullptr)
    {
        Logger::Log::Error("WebGpuDevice::CreateTexture: creation failed ({}x{})", desc.width, desc.height);
        return INVALID_TEXTURE_HANDLE;
    }

    WGPUTextureViewDescriptor viewDesc{};
    viewDesc.label = WebGpu::MakeStringView(desc.debugName);
    viewDesc.format = format;
    if (desc.depth > 1)
    {
        viewDesc.dimension = WGPUTextureViewDimension_3D;
    }
    else if ((desc.flags & TextureCreateFlags::CubeCompatible) != TextureCreateFlags::None && desc.arrayLayers >= 6)
    {
        viewDesc.dimension = desc.arrayLayers > 6 ? WGPUTextureViewDimension_CubeArray : WGPUTextureViewDimension_Cube;
    }
    else
    {
        viewDesc.dimension = (desc.arrayLayers > 1 ||
                              (desc.flags & TextureCreateFlags::ForceArrayView) != TextureCreateFlags::None)
                                 ? WGPUTextureViewDimension_2DArray
                                 : WGPUTextureViewDimension_2D;
    }
    viewDesc.mipLevelCount = textureDesc.mipLevelCount;
    viewDesc.arrayLayerCount = desc.depth > 1 ? 1 : std::max(1u, desc.arrayLayers);
    viewDesc.aspect = WGPUTextureAspect_All;

    WebGpuTexture wrapper{};
    wrapper.texture = texture;
    wrapper.defaultView = wgpuTextureCreateView(texture, &viewDesc);
    wrapper.format = format;
    wrapper.width = desc.width;
    wrapper.height = desc.height;
    wrapper.depth = desc.depth;
    wrapper.mipLevels = textureDesc.mipLevelCount;
    wrapper.arrayLayers = std::max(1u, desc.arrayLayers);
    wrapper.sampleCount = textureDesc.sampleCount;
    wrapper.usage = desc.usage;
    if (desc.debugName != nullptr)
    {
        wrapper.debugName = desc.debugName;
    }

    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    return FromGeneric<TextureTag>(m_Textures.Create(std::move(wrapper)));
}

void WebGpuDevice::DestroyTexture(TextureHandle handle)
{
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    WebGpuTexture* texture = m_Textures.Get(ToGeneric(handle));
    if (texture == nullptr)
    {
        return;
    }
    if (!texture->isSurfaceSlot)
    {
        if (texture->defaultView != nullptr) wgpuTextureViewRelease(texture->defaultView);
        if (texture->texture != nullptr)
        {
            wgpuTextureDestroy(texture->texture);
            wgpuTextureRelease(texture->texture);
        }
    }
    texture->defaultView = nullptr;
    texture->texture = nullptr;
    m_Textures.Destroy(ToGeneric(handle));
}

TextureViewHandle WebGpuDevice::CreateTextureView(TextureHandle texture, const TextureViewDesc& desc)
{
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    WebGpuTexture* source = m_Textures.Get(ToGeneric(texture));
    if (source == nullptr || source->texture == nullptr)
    {
        return INVALID_TEXTURE_VIEW_HANDLE;
    }

    const WGPUTextureFormat format =
        desc.formatOverride != 0 ? WebGpu::ToWgpuTextureFormat(static_cast<TextureFormat>(desc.formatOverride))
                                 : source->format;
    if (format == WGPUTextureFormat_Undefined)
    {
        return INVALID_TEXTURE_VIEW_HANDLE;
    }

    WGPUTextureViewDescriptor viewDesc{};
    viewDesc.label = WebGpu::MakeStringView(desc.debugName);
    viewDesc.format = format;
    viewDesc.dimension = WebGpu::ToWgpuTextureViewDimension(desc.viewType);
    viewDesc.baseMipLevel = desc.baseMip;
    viewDesc.mipLevelCount = desc.levelCount != 0 ? desc.levelCount : (source->mipLevels - desc.baseMip);
    viewDesc.baseArrayLayer = desc.baseLayer;
    viewDesc.arrayLayerCount = desc.layerCount != 0 ? desc.layerCount : (source->arrayLayers - desc.baseLayer);
    viewDesc.aspect = WebGpu::ToWgpuTextureAspect(desc.aspect);

    WGPUTextureView view = wgpuTextureCreateView(source->texture, &viewDesc);
    if (view == nullptr)
    {
        return INVALID_TEXTURE_VIEW_HANDLE;
    }

    WebGpuTextureView wrapper{};
    wrapper.view = view;
    wrapper.format = format;
    if (desc.debugName != nullptr)
    {
        wrapper.debugName = desc.debugName;
    }
    return FromGeneric<TextureViewTag>(m_TextureViews.Create(std::move(wrapper)));
}

void WebGpuDevice::DestroyTextureView(TextureViewHandle handle)
{
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    WebGpuTextureView* view = m_TextureViews.Get(ToGeneric(handle));
    if (view == nullptr)
    {
        return;
    }
    if (view->view != nullptr)
    {
        wgpuTextureViewRelease(view->view);
        view->view = nullptr;
    }
    m_TextureViews.Destroy(ToGeneric(handle));
}

SamplerHandle WebGpuDevice::CreateSampler(const SamplerDesc& desc)
{
    if (m_Device == nullptr)
    {
        return INVALID_SAMPLER_HANDLE;
    }

    WGPUSamplerDescriptor samplerDesc{};
    samplerDesc.label = WebGpu::MakeStringView(desc.debugName);
    samplerDesc.addressModeU = WebGpu::ToWgpuAddressMode(desc.addressModeU);
    samplerDesc.addressModeV = WebGpu::ToWgpuAddressMode(desc.addressModeV);
    samplerDesc.addressModeW = WebGpu::ToWgpuAddressMode(desc.addressModeW);
    samplerDesc.magFilter = WebGpu::ToWgpuFilterMode(desc.magFilter);
    samplerDesc.minFilter = WebGpu::ToWgpuFilterMode(desc.minFilter);
    samplerDesc.mipmapFilter = WebGpu::ToWgpuMipmapFilterMode(desc.mipFilter);
    samplerDesc.lodMinClamp = desc.minLod;
    samplerDesc.lodMaxClamp = desc.maxLod;
    samplerDesc.compare = desc.compareEnable ? WebGpu::ToWgpuCompareFunction(desc.compareOp)
                                             : WGPUCompareFunction_Undefined;
    // Anisotropy above 1 is only legal with linear min/mag/mip in WebGPU.
    const bool trilinear = samplerDesc.magFilter == WGPUFilterMode_Linear &&
                           samplerDesc.minFilter == WGPUFilterMode_Linear &&
                           samplerDesc.mipmapFilter == WGPUMipmapFilterMode_Linear;
    samplerDesc.maxAnisotropy = trilinear ? static_cast<uint16_t>(std::max(1.0f, desc.maxAnisotropy)) : 1;

    WGPUSampler sampler = wgpuDeviceCreateSampler(m_Device, &samplerDesc);
    if (sampler == nullptr)
    {
        Logger::Log::Error("WebGpuDevice::CreateSampler: creation failed");
        return INVALID_SAMPLER_HANDLE;
    }

    WebGpuSampler wrapper{};
    wrapper.sampler = sampler;
    if (desc.debugName != nullptr)
    {
        wrapper.debugName = desc.debugName;
    }

    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    return FromGeneric<SamplerTag>(m_Samplers.Create(std::move(wrapper)));
}

void WebGpuDevice::DestroySampler(SamplerHandle handle)
{
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    WebGpuSampler* sampler = m_Samplers.Get(ToGeneric(handle));
    if (sampler == nullptr)
    {
        return;
    }
    if (sampler->sampler != nullptr)
    {
        wgpuSamplerRelease(sampler->sampler);
        sampler->sampler = nullptr;
    }
    m_Samplers.Destroy(ToGeneric(handle));
}

WebGpuTexture* WebGpuDevice::GetTexture(TextureHandle handle)
{
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    return m_Textures.Get(ToGeneric(handle));
}

WebGpuTextureView* WebGpuDevice::GetTextureView(TextureViewHandle handle)
{
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    return m_TextureViews.Get(ToGeneric(handle));
}

WebGpuSampler* WebGpuDevice::GetSampler(SamplerHandle handle)
{
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    return m_Samplers.Get(ToGeneric(handle));
}

TextureFormat WebGpuDevice::GetTextureFormat(TextureHandle texture) const
{
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    const WebGpuTexture* entry = m_Textures.Get(ToGeneric(texture));
    return entry != nullptr ? WebGpu::FromWgpuTextureFormat(entry->format) : TextureFormat::Unknown;
}

namespace
{
// Packed footprint of the whole mip chain; a block-compressed level rounds up
// to whole 4x4 blocks. The browser's real allocation may pad further.
uint64_t PackedTextureBytes(const WebGpuTexture& t)
{
    const TextureFormat format = WebGpu::FromWgpuTextureFormat(t.format);
    const bool blocks = IsBlockCompressedFormat(format);
    const uint64_t unit = blocks ? BytesPerBlock(format) : BytesPerPixel(format);
    uint64_t total = 0;
    for (uint32_t mip = 0; mip < t.mipLevels; ++mip)
    {
        uint64_t w = std::max<uint32_t>(1u, t.width >> mip);
        uint64_t h = std::max<uint32_t>(1u, t.height >> mip);
        const uint64_t d = std::max<uint32_t>(1u, t.depth >> mip);
        if (blocks)
        {
            w = (w + kBlockCompressedBlockDim - 1) / kBlockCompressedBlockDim;
            h = (h + kBlockCompressedBlockDim - 1) / kBlockCompressedBlockDim;
        }
        total += w * h * d * unit;
    }
    return total * t.arrayLayers * t.sampleCount;
}
} // namespace

size_t WebGpuDevice::DebugGetBufferRegistryCount() const
{
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    return m_Buffers.Size();
}

size_t WebGpuDevice::DebugGetImageRegistryCount() const
{
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    return m_Textures.Size();
}

size_t WebGpuDevice::DebugGetAllocationCount() const
{
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    return m_Buffers.Size() + m_Textures.Size();
}

size_t WebGpuDevice::DebugGetBufferBytes() const
{
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    size_t total = 0;
    m_Buffers.ForEach([&](auto, const WebGpuBuffer& b) {
        if (b.buffer != nullptr)
            total += static_cast<size_t>(b.size);
    });
    return total;
}

size_t WebGpuDevice::DebugGetTextureBytes() const
{
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    size_t total = 0;
    m_Textures.ForEach([&](auto, const WebGpuTexture& t) {
        if (!t.isSurfaceSlot && t.texture != nullptr)
            total += static_cast<size_t>(PackedTextureBytes(t));
    });
    return total;
}

size_t WebGpuDevice::DebugGetAllocatedBytes() const
{
    return DebugGetBufferBytes() + DebugGetTextureBytes();
}

void WebGpuDevice::DebugEnumerateResources(
    const std::function<void(const DebugResourceInfo&)>& fn) const
{
    if (!fn)
        return;
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    m_Buffers.ForEach([&](auto, const WebGpuBuffer& b) {
        if (b.buffer == nullptr)
            return;
        DebugResourceInfo info;
        info.Type = DebugResourceInfo::Kind::Buffer;
        info.Name = b.debugName.empty() ? "(unnamed buffer)" : b.debugName;
        info.Bytes = b.size;
        fn(info);
    });
    m_Textures.ForEach([&](auto, const WebGpuTexture& t) {
        if (t.isSurfaceSlot || t.texture == nullptr)
            return;
        DebugResourceInfo info;
        info.Type = DebugResourceInfo::Kind::Texture;
        info.Name = t.debugName.empty() ? "(unnamed texture)" : t.debugName;
        info.Bytes = PackedTextureBytes(t);
        info.Width = t.width;
        info.Height = t.height;
        info.Format = WebGpu::FromWgpuTextureFormat(t.format);
        fn(info);
    });
}

uint32_t WebGpuDevice::GetTextureSampleCount(TextureHandle texture) const
{
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    const WebGpuTexture* entry = m_Textures.Get(ToGeneric(texture));
    return entry != nullptr ? entry->sampleCount : 1u;
}

uint32_t WebGpuDevice::GetTextureArrayLayers(TextureHandle texture) const
{
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    const WebGpuTexture* entry = m_Textures.Get(ToGeneric(texture));
    return entry != nullptr ? entry->arrayLayers : 1u;
}

void WebGpuDevice::GetTextureSize(TextureHandle texture, uint32_t& outWidth, uint32_t& outHeight) const
{
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    const WebGpuTexture* entry = m_Textures.Get(ToGeneric(texture));
    outWidth = entry != nullptr ? entry->width : 0u;
    outHeight = entry != nullptr ? entry->height : 0u;
}

bool WebGpuDevice::IsTextureAlive(TextureHandle texture) const
{
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    return m_Textures.Get(ToGeneric(texture)) != nullptr;
}

bool WebGpuDevice::IsTextureHandleLive(TextureHandle texture) const
{
    return IsTextureAlive(texture);
}

bool WebGpuDevice::IsPipelineAlive(PipelineHandle pipeline) const
{
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    return m_Pipelines.Get(ToGeneric(pipeline)) != nullptr;
}

bool WebGpuDevice::IsTextureFormatSupported(TextureFormat format, uint32_t usageFlags) const
{
    const WGPUTextureFormat wgpuFormat = WebGpu::ToWgpuTextureFormat(format);
    if (wgpuFormat == WGPUTextureFormat_Undefined)
    {
        return false;
    }
    if (IsBlockCompressedFormat(format) && !m_Capabilities.supportsTextureCompressionBC)
    {
        return false;
    }
    const auto usage = static_cast<TextureUsage>(usageFlags);
    if ((usage & TextureUsage::DepthStencil) != TextureUsage::None &&
        !WebGpu::IsDepthStencilFormat(wgpuFormat))
    {
        return false;
    }
    if ((usage & TextureUsage::RenderTarget) != TextureUsage::None &&
        WebGpu::IsDepthStencilFormat(wgpuFormat))
    {
        return false;
    }
    // Storage is a fixed format list in core WebGPU: rg16float, r16float,
    // r8unorm and the packed formats are all refused, which is what the
    // supportsRG16FloatStorage / supportsNarrowStorageFormats capabilities
    // summarise for callers that pick a format before asking.
    if ((usage & TextureUsage::UnorderedAccess) != TextureUsage::None &&
        !WebGpu::IsCoreStorageCapableFormat(wgpuFormat) &&
        !(wgpuFormat == WGPUTextureFormat_BGRA8Unorm &&
          wgpuDeviceHasFeature(m_Device, WGPUFeatureName_BGRA8UnormStorage) != 0))
    {
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Descriptor sets (bind groups)
// ---------------------------------------------------------------------------

DescriptorSetHandle WebGpuDevice::CreateDescriptorSet(const DescriptorSetDesc& desc)
{
    if (m_Device == nullptr)
    {
        return DescriptorSetHandle{};
    }

    // Interning the layout is what makes a set created here bind-compatible
    // with a pipeline built from the same layout desc: both resolve to the same
    // cached WGPUBindGroupLayout.
    const DescriptorSetLayoutId layoutId = InternDescriptorSetLayout(desc.layout);
    WGPUBindGroupLayout layout = GetOrCreateBindGroupLayout(layoutId);
    if (layout == nullptr)
    {
        return DescriptorSetHandle{};
    }

    WebGpuDescriptorSet set{};
    set.layout = layout;
    set.declaredBindings.reserve(desc.layout.bindings.size());
    for (const auto& binding : desc.layout.bindings)
    {
        set.declaredBindings.push_back(binding.binding);
        // A combined image/sampler splits into two WebGPU entries: the texture
        // keeps the binding and the sampler moves by the cook's fixed offset.
        // A MULTISAMPLED image is the exception and declares no sampler half at
        // all — WGSL reads it with textureLoad — so this list must agree with
        // the layout WebGpuPipeline builds, or the sampler write below survives
        // the declared-binding filter and invalidates the whole bind group.
        if (binding.type == DescriptorType::CombinedImageSampler && !binding.imageMultisample)
            set.declaredBindings.push_back(binding.binding + kCombinedSamplerBindingOffset);
    }
    if (desc.debugName != nullptr)
    {
        set.debugName = desc.debugName;
    }

    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    const DescriptorSetHandle handle =
        FromGeneric<DescriptorSetTag>(m_DescriptorSets.Create(std::move(set)));
    // A transient set is created and used within one frame and never destroyed
    // by the caller (the render path re-creates it every frame). Track it per
    // frame slot so BeginFrame can release it once its GPU work has retired —
    // otherwise every per-frame set leaks its cached WGPUBindGroup, and Dawn's
    // D3D12 descriptor heaps grow until CreateDescriptorHeap fails (device lost).
    // Same recycle contract Metal and Vulkan already implement.
    if (desc.transient)
    {
        m_TransientDescriptorSets[m_FrameIndex].push_back(handle);
    }
    return handle;
}

void WebGpuDevice::UpdateDescriptorSet(DescriptorSetHandle descriptorSet, const DescriptorSetUpdate& update)
{
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    WebGpuDescriptorSet* set = m_DescriptorSets.Get(ToGeneric(descriptorSet));
    if (set == nullptr)
    {
        return;
    }

    // Bind groups are immutable, so updates land in the shadow entry list and
    // the group is rebuilt on the next bind.
    auto upsert = [set](const WGPUBindGroupEntry& entry) {
        // Passes share a superset layout and write only what they use; the
        // reverse also happens, a pass writing a binding its own layout does not
        // declare. Vulkan ignores the extra write, WebGPU rejects the entire
        // bind group for it, so it is dropped here where the layout is known.
        if (!set->declaredBindings.empty() &&
            std::find(set->declaredBindings.begin(), set->declaredBindings.end(), entry.binding) ==
                set->declaredBindings.end())
        {
            return;
        }
        auto existing = std::find_if(set->entries.begin(), set->entries.end(),
                                     [&entry](const WGPUBindGroupEntry& e) { return e.binding == entry.binding; });
        if (existing != set->entries.end())
        {
            *existing = entry;
        }
        else
        {
            set->entries.push_back(entry);
        }
        set->dirty = true;
    };

    switch (update.type)
    {
    case DescriptorType::UniformBuffer:
    case DescriptorType::StorageBuffer:
    {
        for (size_t i = 0; i < update.buffers.size(); ++i)
        {
            WebGpuBuffer* buffer = m_Buffers.Get(ToGeneric(update.buffers[i]));
            if (buffer == nullptr || buffer->buffer == nullptr)
            {
                continue;
            }
            WGPUBindGroupEntry entry{};
            entry.binding = update.binding + static_cast<uint32_t>(i);
            entry.buffer = buffer->buffer;
            entry.offset = i < update.bufferOffsets.size() ? update.bufferOffsets[i] : 0;
            entry.size = (i < update.bufferRanges.size() && update.bufferRanges[i] != 0)
                             ? update.bufferRanges[i]
                             : (buffer->size - entry.offset);
            upsert(entry);
        }
        break;
    }
    case DescriptorType::Texture:
    case DescriptorType::StorageImage:
    {
        const size_t count = update.textureViews.empty() ? update.textures.size() : update.textureViews.size();
        for (size_t i = 0; i < count; ++i)
        {
            WGPUTextureView view = nullptr;
            if (!update.textureViews.empty())
            {
                WebGpuTextureView* entry = m_TextureViews.Get(ToGeneric(update.textureViews[i]));
                view = entry != nullptr ? entry->view : nullptr;
            }
            else
            {
                WebGpuTexture* entry = m_Textures.Get(ToGeneric(update.textures[i]));
                view = entry != nullptr ? entry->defaultView : nullptr;
            }
            if (view == nullptr)
            {
                continue;
            }
            WGPUBindGroupEntry entry{};
            entry.binding = update.binding + update.arrayElement + static_cast<uint32_t>(i);
            entry.textureView = view;
            upsert(entry);
        }
        break;
    }
    case DescriptorType::Sampler:
    {
        for (size_t i = 0; i < update.samplers.size(); ++i)
        {
            WebGpuSampler* sampler = m_Samplers.Get(ToGeneric(update.samplers[i]));
            if (sampler == nullptr || sampler->sampler == nullptr)
            {
                continue;
            }
            WGPUBindGroupEntry entry{};
            entry.binding = update.binding + update.arrayElement + static_cast<uint32_t>(i);
            entry.sampler = sampler->sampler;
            upsert(entry);
        }
        break;
    }
    case DescriptorType::AccelerationStructure:
        // No WebGPU equivalent — see the same case in WebGpuPipeline.cpp. A descriptor of this
        // type can never have been declared on this backend, so there is nothing to write.
        break;
    case DescriptorType::CombinedImageSampler:
    {
        // Split convention (see kCombinedSamplerBindingOffset): texture view
        // stays on the declared binding, sampler lands at binding + offset.
        const size_t count = update.textureViews.empty() ? update.textures.size() : update.textureViews.size();
        for (size_t i = 0; i < count; ++i)
        {
            WGPUTextureView view = nullptr;
            if (!update.textureViews.empty())
            {
                WebGpuTextureView* entry = m_TextureViews.Get(ToGeneric(update.textureViews[i]));
                view = entry != nullptr ? entry->view : nullptr;
            }
            else
            {
                WebGpuTexture* entry = m_Textures.Get(ToGeneric(update.textures[i]));
                view = entry != nullptr ? entry->defaultView : nullptr;
            }
            if (view != nullptr)
            {
                WGPUBindGroupEntry entry{};
                entry.binding = update.binding + update.arrayElement + static_cast<uint32_t>(i);
                entry.textureView = view;
                upsert(entry);
            }
        }
        for (size_t i = 0; i < update.samplers.size(); ++i)
        {
            WebGpuSampler* sampler = m_Samplers.Get(ToGeneric(update.samplers[i]));
            if (sampler != nullptr && sampler->sampler != nullptr)
            {
                WGPUBindGroupEntry entry{};
                entry.binding = update.binding + update.arrayElement + static_cast<uint32_t>(i) +
                                kCombinedSamplerBindingOffset;
                entry.sampler = sampler->sampler;
                upsert(entry);
            }
        }
        break;
    }
    }
}

void WebGpuDevice::DestroyDescriptorSet(DescriptorSetHandle descriptorSet)
{
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    WebGpuDescriptorSet* set = m_DescriptorSets.Get(ToGeneric(descriptorSet));
    if (set == nullptr)
    {
        return;
    }
    if (set->bindGroup != nullptr)
    {
        wgpuBindGroupRelease(set->bindGroup);
        set->bindGroup = nullptr;
    }
    m_DescriptorSets.Destroy(ToGeneric(descriptorSet));
}

WebGpuDescriptorSet* WebGpuDevice::GetDescriptorSet(DescriptorSetHandle handle)
{
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    return m_DescriptorSets.Get(ToGeneric(handle));
}

WGPUBindGroup WebGpuDevice::ResolveBindGroup(DescriptorSetHandle handle)
{
    std::lock_guard<std::recursive_mutex> lock(m_ResourceMutex);
    WebGpuDescriptorSet* set = m_DescriptorSets.Get(ToGeneric(handle));
    if (set == nullptr || set->layout == nullptr)
    {
        return nullptr;
    }
    if (!set->dirty && set->bindGroup != nullptr)
    {
        return set->bindGroup;
    }

    WGPUBindGroupDescriptor desc{};
    desc.label = WebGpu::MakeStringView(set->debugName.empty() ? "BindGroup" : set->debugName.c_str());
    desc.layout = set->layout;
    desc.entryCount = set->entries.size();
    desc.entries = set->entries.empty() ? nullptr : set->entries.data();

    WGPUBindGroup rebuilt = wgpuDeviceCreateBindGroup(m_Device, &desc);
    if (rebuilt == nullptr)
    {
        return set->bindGroup;
    }
    if (set->bindGroup != nullptr)
    {
        wgpuBindGroupRelease(set->bindGroup);
    }
    set->bindGroup = rebuilt;
    set->dirty = false;
    return set->bindGroup;
}

} // namespace GameEngine::Rendering
