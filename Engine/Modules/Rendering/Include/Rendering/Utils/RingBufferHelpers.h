#pragma once
// RingBufferHelpers.h - per-frame ring buffer with overflow detection.
//
// This utility provides a small, explicit abstraction for data that is written once
// per logical frame and may be read by the GPU while other frames are in flight.
// Each RingBuffer<T> is split into "framesInFlight" equally-sized slices, with a
// separate head/overflow flag per slice. Call ResetRingBufferFrame(rb, frameIndex)
// at the beginning of a frame, then write only into that frame's slice.
//
// As long as the chosen frameIndex matches the device's frame-in-flight index and
// the device enforces per-frame fences, this avoids CPU/GPU hazards when streaming
// data such as UI vertices, dynamic instance data, etc.
//
// First-class GPU-only path uses a persistently mapped staging buffer; CPU-mapped
// upload is optional via RingBufferMode::Upload.

#include "Rendering/Core/CommandList.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "Rendering/Core/Device.h"

namespace GameEngine
{
namespace Rendering
{

// Header-only by design to avoid ABI complications; relies on existing Device API

enum class RingBufferMode
{
    GPUOnly,
    Upload
};

template <typename T>
struct RingBuffer
{
    // For Upload mode, 'buffer' is the upload buffer bound directly.
    // For GPUOnly mode, 'gpuBuffer' is bound, and 'buffer' is the staging upload buffer.
    BufferHandle buffer{INVALID_HANDLE};    // staging or upload buffer (CPU-visible)
    BufferHandle gpuBuffer{INVALID_HANDLE}; // GPU-only buffer (GPUOnly mode)
    uint32_t framesInFlight{2};
    size_t capacityPerFrame{0};
    size_t heads[8]{}; // bytes written per frame
    bool overflowed[8]{};
    bool persistentlyMapped{true}; // for Upload mode only
    RingBufferMode mode{RingBufferMode::GPUOnly};
    void* stagingMapped{nullptr}; // mapped base for staging (GPUOnly), persistent
};

struct RingBufferAlloc
{
    void* ptr{nullptr};
    size_t byteOffset{0};
    size_t sizeBytes{0};
};

// Create a ring buffer with countPerFrame entries per frame
// If PersistentlyMapped is available, prefer it for speed

template <typename T>
inline RingBuffer<T> CreateRingBuffer(IDevice* device,
                                      uint32_t framesInFlight,
                                      size_t countPerFrame,
                                      const char* name = nullptr,
                                      RingBufferMode mode = RingBufferMode::GPUOnly,
                                      BufferCreateFlags flags = BufferCreateFlags::PersistentlyMapped)
{
    RingBuffer<T> rb{};
    if (!device || framesInFlight == 0 || countPerFrame == 0)
        return rb;

    rb.framesInFlight = framesInFlight;
    rb.capacityPerFrame = countPerFrame * sizeof(T);
    rb.mode = mode;

    const size_t totalSize = rb.capacityPerFrame * framesInFlight;

    if (mode == RingBufferMode::GPUOnly)
    {
        BufferDesc gpu{};
        gpu.size = totalSize;
        gpu.usage = static_cast<uint32_t>(BufferUsage::TransferDst | BufferUsage::Uniform | BufferUsage::Storage | BufferUsage::Vertex | BufferUsage::Index);
        gpu.memoryUsage = BufferMemoryUsage::DeviceLocal;
        gpu.flags = BufferCreateFlags::None;
        gpu.debugName = name;
        rb.gpuBuffer = device->CreateBuffer(gpu);

        BufferDesc staging{};
        staging.size = totalSize;
        staging.usage = static_cast<uint32_t>(BufferUsage::TransferSrc | BufferUsage::Uniform | BufferUsage::Storage);
        staging.memoryUsage = BufferMemoryUsage::Upload;
        staging.flags = BufferCreateFlags::PersistentlyMapped;
        staging.debugName = name;
        rb.buffer = device->CreateBuffer(staging);
        rb.stagingMapped = device->MapBuffer(rb.buffer);
    }
    else
    {
        BufferDesc up{};
        up.size = totalSize;
        up.usage = static_cast<uint32_t>(BufferUsage::TransferDst | BufferUsage::Uniform | BufferUsage::Storage | BufferUsage::Vertex | BufferUsage::Index);
        up.memoryUsage = BufferMemoryUsage::Upload;
        up.flags = flags;
        up.debugName = name;
        rb.buffer = device->CreateBuffer(up);
        rb.persistentlyMapped = (flags & BufferCreateFlags::PersistentlyMapped) == BufferCreateFlags::PersistentlyMapped;
    }

    std::fill(std::begin(rb.heads), std::end(rb.heads), 0);
    return rb;
}

// Reset head and overflow flag for a frame; call at frame begin

template <typename T>
inline void ResetRingBufferFrame(RingBuffer<T>& rb, uint32_t frameIndex)
{
    if (frameIndex >= rb.framesInFlight)
        return;
    rb.heads[frameIndex] = 0;
    rb.overflowed[frameIndex] = false;
}

// Map N instances for the given frame; returns pointer and byte offset into buffer
// The caller should bind buffer + [byteOffset, sizeBytes]

template <typename T>
inline RingBufferAlloc MapRingBuffer(IDevice* device, RingBuffer<T>& rb, uint32_t frameIndex, size_t count)
{
    RingBufferAlloc out{};
    if (!device || count == 0 || frameIndex >= rb.framesInFlight)
        return out;

    // Validate buffers depending on mode
    if (rb.mode == RingBufferMode::GPUOnly)
    {
        if (!rb.buffer.IsValid() || !rb.gpuBuffer.IsValid())
            return out;
    }
    else
    {
        if (!rb.buffer.IsValid())
            return out;
    }

    size_t bytes = count * sizeof(T);
    if (rb.heads[frameIndex] + bytes > rb.capacityPerFrame)
    {
        rb.overflowed[frameIndex] = true;
        return out;
    }

    size_t baseFrameOffset = size_t(frameIndex) * rb.capacityPerFrame;
    out.byteOffset = baseFrameOffset + rb.heads[frameIndex];
    out.sizeBytes = bytes;

    if (rb.mode == RingBufferMode::GPUOnly)
    {
        if (!rb.stagingMapped)
        {
            rb.stagingMapped = device->MapBuffer(rb.buffer);
            if (!rb.stagingMapped)
                return out;
        }
        out.ptr = reinterpret_cast<uint8_t*>(rb.stagingMapped) + out.byteOffset;
    }
    else
    {
        void* base = device->MapBuffer(rb.buffer);
        if (!base)
            return out;
        out.ptr = reinterpret_cast<uint8_t*>(base) + out.byteOffset;
    }
    // The region is handed out to be written, so declare it here: the mapping is
    // held open for the ring's lifetime and nothing else would publish it.
    device->FlushMappedRange(rb.buffer, out.byteOffset, out.sizeBytes);
    return out;
}

// Issue copy from staging to GPU buffer for current frame in GPUOnly mode
inline void FlushRingBufferWrites(IDevice* device, CommandList* cmd, uint32_t frameIndex, size_t bytesToCopy, BufferHandle staging, BufferHandle gpu, size_t frameSize)
{
    if (!device || !cmd)
        return;
    if (!staging.IsValid() || !gpu.IsValid())
        return;
    const size_t srcOff = size_t(frameIndex) * frameSize;
    const size_t dstOff = srcOff;
    const size_t sz = bytesToCopy;
    cmd->CopyBuffer(staging, gpu, sz, srcOff, dstOff);
}

template <typename T>
inline void FlushRingBufferWrites(IDevice* device, CommandList* cmd, RingBuffer<T>& rb, uint32_t frameIndex)
{
    if (rb.mode != RingBufferMode::GPUOnly)
        return;
    if (frameIndex >= rb.framesInFlight)
        return;
    FlushRingBufferWrites(device, cmd, frameIndex, rb.heads[frameIndex], rb.buffer, rb.gpuBuffer, rb.capacityPerFrame);
}

template <typename T>
inline BufferHandle GetBufferForBinding(const RingBuffer<T>& rb)
{
    return (rb.mode == RingBufferMode::GPUOnly) ? rb.gpuBuffer : rb.buffer;
}

// Advance head after writing; call once you finished writing count elements

template <typename T>
inline void AdvanceRingBuffer(RingBuffer<T>& rb, uint32_t frameIndex, size_t count)
{
    if (frameIndex >= rb.framesInFlight)
        return;
    rb.heads[frameIndex] += count * sizeof(T);
}

// Optional unmap for non-persistent buffers

template <typename T>
inline void UnmapRingBuffer(IDevice* device, RingBuffer<T>& rb)
{
    if (device && rb.buffer.IsValid() && !rb.persistentlyMapped)
    {
        device->UnmapBuffer(rb.buffer);
    }
}

// Destroy ring buffer and release its underlying buffer
template <typename T>
inline void DestroyRingBuffer(IDevice* device, RingBuffer<T>& rb)
{
    if (!device)
        return;
    if (rb.mode == RingBufferMode::GPUOnly)
    {
        if (rb.buffer.IsValid())
        {
            device->UnmapBuffer(rb.buffer);
            device->DestroyBuffer(rb.buffer);
            rb.buffer = INVALID_HANDLE;
        }
        if (rb.gpuBuffer.IsValid())
        {
            device->DestroyBuffer(rb.gpuBuffer);
            rb.gpuBuffer = INVALID_HANDLE;
        }
        rb.stagingMapped = nullptr;
    }
    else
    {
        if (rb.buffer.IsValid())
        {
            if (!rb.persistentlyMapped)
                device->UnmapBuffer(rb.buffer);
            device->DestroyBuffer(rb.buffer);
            rb.buffer = INVALID_HANDLE;
        }
    }
}

} // namespace Rendering
} // namespace GameEngine
