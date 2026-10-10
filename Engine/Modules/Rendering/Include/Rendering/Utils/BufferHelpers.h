#pragma once

#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"

namespace GameEngine
{
namespace Rendering
{

// Minimal, header-only helpers to avoid API bloat. Compose IDevice + CommandList.

// Create a buffer and initialize its contents. Uses IDevice::UpdateBuffer under the hood.
// No state transitions are issued here; pair with StageBufferData if you need a final state.
inline BufferHandle CreateBufferInitialized(IDevice* device,
                                            const BufferDesc& desc,
                                            const void* data,
                                            size_t sizeBytes)
{
    if (!device)
        return INVALID_BUFFER_HANDLE;
    BufferHandle handle = device->CreateBuffer(desc);
    if (handle.IsValid() && data && sizeBytes > 0)
    {
        // Assume offset 0; callers can stage additional ranges as needed
        device->UpdateBuffer(handle, 0, sizeBytes, data);
    }
    return handle;
}

// Stage data into an existing buffer and insert a barrier to the desired final state.
// This keeps callers from having to remember CopyDst -> desiredState transitions.
// If used within RenderGraph, prefer the RG-declared states and skip this explicit barrier.
inline void StageBufferData(IDevice* device,
                            CommandList* cmd,
                            BufferHandle dst,
                            size_t dstOffset,
                            const void* data,
                            size_t sizeBytes,
                            ResourceState desiredState)
{
    if (!device || !cmd || !dst.IsValid() || !data || sizeBytes == 0)
        return;

    // Upload data via the device (implementation may internally use a staging path)
    device->UpdateBuffer(dst, dstOffset, sizeBytes, data);

    // Transition to the requested state for immediate use by subsequent commands
    if (desiredState != ResourceState::Undefined)
    {
        // Best-effort: assume the resource can be considered CopyDest/COMMON before use
        // (Implementations may refine state tracking later without changing this signature.)
        ResourceBarrier barrier = ResourceBarrier::CreateBufferBarrier(dst, ResourceState::CopyDest, desiredState);
        cmd->Barrier(barrier);
    }
}

// Thin sugars with sensible defaults for vertex/index buffers
inline BufferHandle CreateVertexBuffer(IDevice* device,
                                       const void* data,
                                       size_t sizeBytes,
                                       const char* name = nullptr)
{
    if (!device || sizeBytes == 0)
        return INVALID_BUFFER_HANDLE;
    BufferDesc desc{};
    desc.size = sizeBytes;
    desc.usage = static_cast<uint32_t>(BufferUsage::Vertex | BufferUsage::TransferDst);
    desc.memoryUsage = BufferMemoryUsage::DeviceLocal;
    desc.flags = BufferCreateFlags::None;
    desc.debugName = name;
    return CreateBufferInitialized(device, desc, data, sizeBytes);
}

inline BufferHandle CreateIndexBuffer(IDevice* device,
                                      const void* data,
                                      size_t sizeBytes,
                                      const char* name = nullptr)
{
    if (!device || sizeBytes == 0)
        return INVALID_BUFFER_HANDLE;
    BufferDesc desc{};
    desc.size = sizeBytes;
    desc.usage = static_cast<uint32_t>(BufferUsage::Index | BufferUsage::TransferDst);
    desc.memoryUsage = BufferMemoryUsage::DeviceLocal;
    desc.flags = BufferCreateFlags::None;
    desc.debugName = name;
    return CreateBufferInitialized(device, desc, data, sizeBytes);
}

// Convenience helpers for skinned mesh streams (JOINTS_0 and WEIGHTS_0)
// jointsFlat4: flattened array with 4 uint16 per vertex; vertexCount is number of vertices
inline BufferHandle CreateSkinnedJointsBuffer(IDevice* device,
                                              const uint16_t* jointsFlat4,
                                              size_t vertexCount,
                                              const char* name = "Skinned_Joints0")
{
    if (!device || !jointsFlat4 || vertexCount == 0)
        return INVALID_BUFFER_HANDLE;
    const size_t sizeBytes = vertexCount * 4 * sizeof(uint16_t);
    BufferDesc desc{};
    desc.size = sizeBytes;
    desc.usage = static_cast<uint32_t>(BufferUsage::Vertex | BufferUsage::TransferDst);
    desc.memoryUsage = BufferMemoryUsage::DeviceLocal;
    desc.flags = BufferCreateFlags::None;
    desc.debugName = name;
    return CreateBufferInitialized(device, desc, jointsFlat4, sizeBytes);
}

// weightsFlat4: flattened array with 4 float per vertex
inline BufferHandle CreateSkinnedWeightsBuffer(IDevice* device,
                                               const float* weightsFlat4,
                                               size_t vertexCount,
                                               const char* name = "Skinned_Weights0")
{
    if (!device || !weightsFlat4 || vertexCount == 0)
        return INVALID_BUFFER_HANDLE;
    const size_t sizeBytes = vertexCount * 4 * sizeof(float);
    BufferDesc desc{};
    desc.size = sizeBytes;
    desc.usage = static_cast<uint32_t>(BufferUsage::Vertex | BufferUsage::TransferDst);
    desc.memoryUsage = BufferMemoryUsage::DeviceLocal;
    desc.flags = BufferCreateFlags::None;
    desc.debugName = name;
    return CreateBufferInitialized(device, desc, weightsFlat4, sizeBytes);
}

} // namespace Rendering
} // namespace GameEngine
