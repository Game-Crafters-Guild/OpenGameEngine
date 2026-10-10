#pragma once

#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <new>
#include <vector>

namespace GameEngine
{
namespace Rendering
{

// Minimal, header-only helpers for uploading pixel data to GPU textures via
// transient staging buffers. Follows the same pattern as BufferHelpers.h.
//
// GPU safety contract: after ExecuteCommandLists returns, each staging buffer
// is destroyed via IDevice::DestroyBuffer. Backends are expected to defer
// actual destruction until the GPU has finished referencing the buffer. The
// Vulkan backend implements this via timeline-semaphore or per-frame-fence
// deferral (see VulkanDevice::DestroyBuffer). If a backend does NOT defer,
// it must execute command lists synchronously before DestroyBuffer returns.

struct TextureUploadEntry
{
    TextureHandle Texture;
    const void* Pixels;
    uint32_t Width;
    uint32_t Height;
    uint32_t Depth = 1;
    size_t RowPitchBytes;       // bytes per row (e.g. Width * 4 for RGBA8)
    size_t SlicePitchBytes = 0; // bytes per depth slice (0 = Height * RowPitchBytes)

    // State the texture is in when this copy begins. Undefined is right for a
    // texture that was just created and that nothing has sampled yet.
    //
    // TRAP: Undefined resolves to srcStage = TOP_OF_PIPE, whose first
    // synchronization scope is empty. Re-uploading into a texture the GPU is
    // already sampling therefore gets NO write-after-read dependency on the
    // in-flight frame's reads, and the copy may land mid-sample. Repeat uploads
    // must name ShaderResource — see ReuploadTexture2D.
    ResourceState SourceState = ResourceState::Undefined;
};

namespace Detail
{
constexpr size_t kUploadEntryStackCapacity = 4;
constexpr size_t kD3D12TextureDataPitchAlignment = 256; // matches D3D12_TEXTURE_DATA_PITCH_ALIGNMENT

inline size_t AlignUp(size_t value, size_t alignment)
{
    return (value + alignment - 1u) & ~(alignment - 1u);
}
} // namespace Detail

// Upload one or more 2D or 3D textures in a single command list submission.
// Each texture must already be created with TextureUsage::TransferDst.
// Source layout is each entry's SourceState; final state is ShaderResource.
//
// D3D12 callers can supply any RowPitch/SlicePitch; this helper repacks the
// source into a tightly-packed, pitch-aligned staging buffer transparently.
// False means preparation failed and no command list was submitted. True means
// the complete batch was handed to ExecuteCommandLists; that void device API
// does not report submission/completion success. Device health/reprovision owns
// recovery after dispatch, so callers must not treat true as GPU completion.
inline bool UploadTexturesBatched(IDevice* device,
                                  const TextureUploadEntry* entries,
                                  size_t count,
                                  const char* stagingDebugName = "TexUploadStaging")
{
    if (!device || !entries || count == 0)
        return false;

    for (size_t i = 0; i < count; ++i)
        if (!entries[i].Texture.IsValid() || !entries[i].Pixels ||
            !entries[i].Width || !entries[i].Height || !entries[i].RowPitchBytes)
            return false;

    const bool isD3D12 = device->GetAPI() == GraphicsAPI::DirectX12;
    // Row-pitch alignment for buffer->texture copies: D3D12 and WebGPU both
    // require 256 (D3D12_TEXTURE_DATA_PITCH_ALIGNMENT / COPY_BYTES_PER_ROW_ALIGNMENT);
    // Vulkan/Metal report 1.
    const size_t rowPitchAlignment = isD3D12
                                         ? Detail::kD3D12TextureDataPitchAlignment
                                         : std::max<size_t>(1, device->GetCapabilities().textureCopyRowPitchAlignment);

    // Per-entry scratch state. Common case (≤4 textures) stays on the stack.
    struct EntryScratch
    {
        BufferHandle Staging;
        size_t UploadRowPitch = 0;
        size_t UploadSlicePitch = 0;
    };
    std::array<EntryScratch, Detail::kUploadEntryStackCapacity> stackScratch{};
    std::vector<EntryScratch> heapScratch;
    EntryScratch* scratch = stackScratch.data();
    if (count > Detail::kUploadEntryStackCapacity)
    {
        try
        {
            heapScratch.resize(count);
        }
        catch (const std::bad_alloc&)
        {
            return false;
        }
        scratch = heapScratch.data();
    }

    struct StagingCleanup
    {
        IDevice* Device;
        EntryScratch* Entries;
        size_t Count;
        ~StagingCleanup()
        {
            for (size_t i = 0; i < Count; ++i)
                if (Entries[i].Staging.IsValid())
                    Device->DestroyBuffer(Entries[i].Staging);
        }
    } cleanup{device, scratch, count};

    // Reusable padding buffer when D3D12 row-pitch alignment forces a copy.
    // Kept outside the loop so allocations amortize across the batch.
    std::vector<unsigned char> padded;
    std::unique_ptr<CommandList> cl;
    std::vector<CommandList*> commandLists;

    try
    {
        for (size_t i = 0; i < count; ++i)
        {
            const auto& entry = entries[i];
            const uint32_t depth = entry.Depth > 0 ? entry.Depth : 1u;
            const size_t sourceSlicePitch =
                entry.SlicePitchBytes ? entry.SlicePitchBytes : static_cast<size_t>(entry.Height) * entry.RowPitchBytes;
            scratch[i].UploadRowPitch = entry.RowPitchBytes;
            scratch[i].UploadSlicePitch = sourceSlicePitch;

            const void* uploadPixels = entry.Pixels;
            if (rowPitchAlignment > 1)
            {
                scratch[i].UploadRowPitch = Detail::AlignUp(entry.RowPitchBytes, rowPitchAlignment);
                scratch[i].UploadSlicePitch = scratch[i].UploadRowPitch * entry.Height;

                const bool rowPitchPadded = scratch[i].UploadRowPitch != entry.RowPitchBytes;
                const bool slicePitchPadded = scratch[i].UploadSlicePitch != sourceSlicePitch;
                if (rowPitchPadded || slicePitchPadded)
                {
                    padded.assign(static_cast<size_t>(depth) * scratch[i].UploadSlicePitch, 0);
                    const auto* src = static_cast<const unsigned char*>(entry.Pixels);
                    for (uint32_t z = 0; z < depth; ++z)
                    {
                        for (uint32_t y = 0; y < entry.Height; ++y)
                        {
                            const size_t srcOffset =
                                static_cast<size_t>(z) * sourceSlicePitch + static_cast<size_t>(y) * entry.RowPitchBytes;
                            const size_t dstOffset =
                                static_cast<size_t>(z) * scratch[i].UploadSlicePitch + static_cast<size_t>(y) * scratch[i].UploadRowPitch;
                            std::memcpy(padded.data() + dstOffset, src + srcOffset, entry.RowPitchBytes);
                        }
                    }
                    uploadPixels = padded.data();
                }
            }

            const size_t uploadSize = static_cast<size_t>(depth) * scratch[i].UploadSlicePitch;
            scratch[i].Staging = device->CreateUploadBuffer(uploadSize, stagingDebugName);
            if (!scratch[i].Staging.IsValid())
                return false;
            device->UpdateBuffer(scratch[i].Staging, 0, uploadSize, uploadPixels);
        }

        cl = device->CreateCommandList(IDevice::QueueType::Graphics);
        if (!cl)
            return false;
        cl->Begin();
        for (size_t i = 0; i < count; ++i)
        {
            if (!scratch[i].Staging.IsValid())
                continue;
            const auto& entry = entries[i];
            const uint32_t depth = entry.Depth > 0 ? entry.Depth : 1u;
            cl->Barrier(ResourceBarrier::CreateTextureBarrier(
                entry.Texture, entry.SourceState, ResourceState::CopyDest));
            cl->CopyBufferToTextureSubresource(
                scratch[i].Staging, entry.Texture, 0, 0,
                entry.Width, entry.Height, 0, scratch[i].UploadRowPitch,
                depth, scratch[i].UploadSlicePitch);
            cl->Barrier(ResourceBarrier::CreateTextureBarrier(
                entry.Texture, ResourceState::CopyDest, ResourceState::ShaderResource));
        }
        cl->End();
        commandLists.push_back(cl.get());
    }
    catch (const std::bad_alloc&)
    {
        return false;
    }

    // Exceptions after entering the device's void dispatch cannot establish
    // whether any copy ran. Propagate them; never report a pre-submit failure.
    device->ExecuteCommandLists(commandLists);

    // Cleanup defers actual staging deallocation through the backend. No copy
    // is submitted until every staging allocation and the command list exist.
    return true;
}

// Upload pixel data to a single existing 2D texture via a transient staging buffer.
// Texture must already be created with TextureUsage::TransferDst.
// Initial layout is assumed Undefined; final state is ShaderResource.
inline void UploadTexture2D(IDevice* device,
                            TextureHandle tex,
                            const void* pixels,
                            uint32_t width, uint32_t height,
                            size_t rowPitchBytes,
                            const char* stagingDebugName = "TexUploadStaging")
{
    TextureUploadEntry entry{tex, pixels, width, height, 1, rowPitchBytes, 0};
    UploadTexturesBatched(device, &entry, 1, stagingDebugName);
}

// Re-upload into a 2D texture that is already resident in ShaderResource and
// may still be sampled by an in-flight frame (video frames, dynamic previews).
// The copy is ordered after prior fragment reads by naming ShaderResource as
// the source state; UploadTexture2D's Undefined source would not order it at
// all (see TextureUploadEntry::SourceState).
//
// The texture must be created with TransferDst and reach ShaderResource before
// the first call — TextureDesc::initialState does that at creation time.
inline void ReuploadTexture2D(IDevice* device,
                              TextureHandle tex,
                              const void* pixels,
                              uint32_t width, uint32_t height,
                              size_t rowPitchBytes,
                              const char* stagingDebugName)
{
    TextureUploadEntry entry{tex, pixels, width, height, 1, rowPitchBytes, 0,
                             ResourceState::ShaderResource};
    UploadTexturesBatched(device, &entry, 1, stagingDebugName);
}

// One mip level of a 2D texture, tightly packed. For block-compressed formats
// SizeBytes is the exact per-level payload (e.g. ktxTexture_GetImageSize);
// Width/Height are the level's texel dimensions (not block counts).
struct TextureMipUploadEntry
{
    const void* Pixels;
    size_t SizeBytes;
    uint32_t Width;
    uint32_t Height;
};

// Upload a full 2D mip chain (layer 0) in a single submission. The texture must
// be created with mipLevels >= mipCount and TextureUsage::TransferDst. `format`
// must match the texture's format: it drives packing (block-compressed levels
// upload tightly packed; uncompressed levels use Width * BytesPerPixel rows).
// Initial layout is assumed Undefined; final state is ShaderResource across all
// uploaded levels.
inline void UploadTextureMips(IDevice* device,
                              TextureHandle tex,
                              const TextureMipUploadEntry* mips,
                              uint32_t mipCount,
                              TextureFormat format,
                              const char* stagingDebugName = "TexMipUploadStaging")
{
    if (!device || !mips || mipCount == 0)
        return;

    const bool isBlock = IsBlockCompressedFormat(format);
    const uint32_t bpp = BytesPerPixel(format);
    const bool isD3D12 = device->GetAPI() == GraphicsAPI::DirectX12;
    const size_t mipRowPitchAlignment = isD3D12
                                            ? Detail::kD3D12TextureDataPitchAlignment
                                            : std::max<size_t>(1, device->GetCapabilities().textureCopyRowPitchAlignment);
    if (isBlock && isD3D12)
    {
        // No DXGI mapping exists for the BC TextureFormats yet, so a BC texture
        // handle cannot have been created on this backend.
        return;
    }
    if (!isBlock && bpp == 0)
        return;

    // Staging layout: one buffer, each level 16-byte aligned so Vulkan's
    // bufferOffset % texel-block-size rule holds for every supported format
    // (BC1 = 8, BC5/7 = 16, uncompressed <= 16). D3D12 additionally requires
    // D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT (512) for each subresource start,
    // and uncompressed rows are repacked to the 256-byte pitch alignment.
    constexpr size_t kVulkanMipOffsetAlignment = 16;
    constexpr size_t kD3D12TextureDataPlacementAlignment = 512;
    const size_t mipOffsetAlignment =
        isD3D12 ? kD3D12TextureDataPlacementAlignment : kVulkanMipOffsetAlignment;
    std::vector<size_t> offsets(mipCount);
    std::vector<size_t> rowPitches(mipCount);
    size_t totalBytes = 0;
    for (uint32_t m = 0; m < mipCount; ++m)
    {
        size_t rowPitch = 0;
        size_t levelBytes = mips[m].SizeBytes;
        if (!isBlock)
        {
            rowPitch = static_cast<size_t>(mips[m].Width) * bpp;
            if (mipRowPitchAlignment > 1)
            {
                rowPitch = Detail::AlignUp(rowPitch, mipRowPitchAlignment);
                levelBytes = rowPitch * mips[m].Height;
            }
        }
        offsets[m] = totalBytes;
        rowPitches[m] = rowPitch; // 0 for block-compressed = tightly packed
        totalBytes += Detail::AlignUp(levelBytes, mipOffsetAlignment);
    }

    BufferHandle staging = device->CreateUploadBuffer(totalBytes, stagingDebugName);
    if (!staging.IsValid())
        return;

    std::vector<unsigned char> padded;
    for (uint32_t m = 0; m < mipCount; ++m)
    {
        const void* src = mips[m].Pixels;
        size_t writeBytes = mips[m].SizeBytes;
        const size_t tightRowPitch = static_cast<size_t>(mips[m].Width) * bpp;
        if (!isBlock && rowPitches[m] != 0 && rowPitches[m] != tightRowPitch)
        {
            padded.assign(rowPitches[m] * mips[m].Height, 0);
            const auto* tight = static_cast<const unsigned char*>(mips[m].Pixels);
            for (uint32_t y = 0; y < mips[m].Height; ++y)
                std::memcpy(padded.data() + static_cast<size_t>(y) * rowPitches[m],
                            tight + static_cast<size_t>(y) * tightRowPitch, tightRowPitch);
            src = padded.data();
            writeBytes = padded.size();
        }
        device->UpdateBuffer(staging, offsets[m], writeBytes, src);
    }

    auto cl = device->CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    cl->Barrier(ResourceBarrier::CreateTextureBarrier(
        tex, ResourceState::Undefined, ResourceState::CopyDest, 0, mipCount));
    for (uint32_t m = 0; m < mipCount; ++m)
    {
        cl->CopyBufferToTextureSubresource(staging, tex, m, 0,
                                           mips[m].Width, mips[m].Height,
                                           offsets[m], rowPitches[m],
                                           1, 0, 0, 0, ResourceState::CopyDest);
    }
    cl->Barrier(ResourceBarrier::CreateTextureBarrier(
        tex, ResourceState::CopyDest, ResourceState::ShaderResource, 0, mipCount));
    cl->End();

    CommandList* raw = cl.get();
    device->ExecuteCommandLists({raw});

    // Safe: the backend defers deallocation until GPU work completes (see the
    // contract at the top of this header).
    device->DestroyBuffer(staging);
}

// Upload tightly or explicitly pitched voxel data to a single existing 3D texture.
// Texture must already be created with depth > 1 and TextureUsage::TransferDst.
// Initial layout is assumed Undefined; final state is ShaderResource.
inline void UploadTexture3D(IDevice* device,
                            TextureHandle tex,
                            const void* pixels,
                            uint32_t width, uint32_t height, uint32_t depth,
                            size_t rowPitchBytes,
                            size_t slicePitchBytes = 0,
                            const char* stagingDebugName = "Tex3DUploadStaging")
{
    TextureUploadEntry entry{tex, pixels, width, height, depth, rowPitchBytes, slicePitchBytes};
    UploadTexturesBatched(device, &entry, 1, stagingDebugName);
}

} // namespace Rendering
} // namespace GameEngine
