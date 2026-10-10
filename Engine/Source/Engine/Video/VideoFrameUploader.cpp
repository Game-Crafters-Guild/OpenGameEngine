#include "Engine/Video/VideoFrameUploader.h"

#include "Rendering/Core/CommandList.h"

#include <algorithm>
#include <cstring>

namespace GameEngine::Video
{

namespace
{
size_t AlignUp(size_t value, size_t alignment)
{
    return (value + alignment - 1u) & ~(alignment - 1u);
}
} // namespace

VideoFrameUploader::VideoFrameUploader(Rendering::IDevice* device)
    : m_Device(device)
{
}

VideoFrameUploader::~VideoFrameUploader()
{
    m_Staging.Shutdown();
}

size_t VideoFrameUploader::UploadRowPitch(size_t tightRowPitch) const
{
    return m_Device->GetAPI() == Rendering::GraphicsAPI::DirectX12
        ? AlignUp(tightRowPitch, kD3D12RowPitchAlignment)
        : tightRowPitch;
}

bool VideoFrameUploader::EnsureStaging(size_t frameBytes)
{
    if (m_Staging.IsInitialized())
        return true;

    // One slot per frame in flight plus one: the copy is submitted during the
    // tick's update phase, before that tick's own fence wait, and that phase costs
    // exactly one extra slot (FrameBufferAllocator::BeginFrame states the rule). The
    // per-slot token below is the actual guarantee; the extra slot is what keeps that
    // guarantee from ever having to stall.
    const uint32_t slots = (std::min)(m_Device->GetFramesInFlight() + 1u,
                                      Rendering::FrameBufferAllocator::kMaxRingSlots);
    const size_t capacity = (std::max)(AlignUp(frameBytes, kCopyOffsetAlignment),
                                       kMinStagingSlotBytes);
    // Sized for the first frame that arrives. A second source (or a larger one)
    // overflows once — the allocator REFUSES rather than reissuing a live offset —
    // and the recorded demand grows the slot before the next cycle, so the cost of
    // guessing low is one frame not shown rather than a permanently undersized
    // ring. Concurrent demand past kMaxStagingSlotBytes is the exception: the slot
    // stops growing there and the sources that do not fit drop frames every tick.
    if (!m_Staging.Initialize(m_Device, capacity, Rendering::BufferUsage::TransferSrc,
                              slots, kCopyOffsetAlignment, "VideoFrameStaging",
                              kMaxStagingSlotBytes))
    {
        return false;
    }
    m_Staging.BeginFrame(m_FrameToken);
    return true;
}

void VideoFrameUploader::BeginFrame()
{
    m_Staged.clear();
    const uint32_t frameToken = m_Device->GetFrameIndex();
    // The device index is a CHANGE token, not a counter — it wraps at the device's
    // pacing. Count the advances so a slot's write can be compared against the
    // device frame it happened in without wrap aliasing two rotations together.
    if (m_HasBegunFrame && frameToken != m_FrameToken)
        ++m_DeviceFrameSeq;
    m_HasBegunFrame = true;
    m_FrameToken = frameToken;
    if (!m_Staging.IsInitialized())
    {
        // Nothing has been staged yet, so there is no slot to be in flight.
        m_SlotUsable = true;
        return;
    }

    m_Staging.BeginFrame(m_FrameToken);
    // The rewind above moved no bytes; refusing to WRITE is what keeps a slot the
    // GPU is still reading intact. Two ticks inside one device frame land on the
    // same slot (the frame token did not change), and this is what stops the
    // second from overwriting the first's in-flight staging.
    m_SlotUsable = SlotRetired(m_Staging.GetCurrentSlot());
}

bool VideoFrameUploader::SlotRetired(uint32_t slot) const
{
    switch (m_Device->QueryGpuSyncToken(m_SlotTokens[slot]))
    {
    case Rendering::IDevice::GpuSyncStatus::Complete:
        return true;
    case Rendering::IDevice::GpuSyncStatus::Pending:
        return false;
    case Rendering::IDevice::GpuSyncStatus::Unknown:
        break;
    }

    // Unknown, for one of two reasons the token cannot tell apart: nothing has ever
    // been submitted from this slot, or this backend publishes no graphics timeline
    // at all. Fall back to the guarantee that does not depend on a timeline: the
    // allocator advances one slot per device frame across framesInFlight+1 slots, so
    // a slot last written in an earlier device frame has been fenced by the device's
    // own pacing. One written in THIS device frame has not — which is the
    // repeated-tick case this gate exists for.
    return m_SlotWriteSeq[slot] != m_DeviceFrameSeq;
}

bool VideoFrameUploader::Stage(Rendering::TextureHandle texture, const uint8_t* pixels,
                               uint32_t width, uint32_t height, size_t rowPitchBytes)
{
    if (!m_SlotUsable || !texture.IsValid() || !pixels || width == 0 || height == 0)
        return false;

    const size_t uploadRowPitch = UploadRowPitch(rowPitchBytes);
    const size_t uploadBytes = uploadRowPitch * static_cast<size_t>(height);
    if (!EnsureStaging(uploadBytes))
        return false;

    const Rendering::FrameBufferAllocator::Allocation alloc =
        m_Staging.Allocate(uploadBytes, kCopyOffsetAlignment);
    if (!alloc.IsValid())
        return false; // ring full — the ring grows next cycle from the recorded demand

    auto* dst = static_cast<uint8_t*>(alloc.ptr);
    if (uploadRowPitch == rowPitchBytes)
    {
        std::memcpy(dst, pixels, uploadBytes);
    }
    else
    {
        for (uint32_t y = 0; y < height; ++y)
        {
            std::memcpy(dst + static_cast<size_t>(y) * uploadRowPitch,
                        pixels + static_cast<size_t>(y) * rowPitchBytes,
                        rowPitchBytes);
        }
    }

    m_Staged.push_back(StagedCopy{texture, alloc.buffer, alloc.offset, width, height,
                                  uploadRowPitch});
    return true;
}

void VideoFrameUploader::Submit()
{
    if (m_Staged.empty())
        return;

    auto commandList = m_Device->CreateCommandList(Rendering::IDevice::QueueType::Graphics);
    if (!commandList)
    {
        m_Staged.clear();
        return;
    }
    commandList->Begin();
    for (const StagedCopy& copy : m_Staged)
    {
        commandList->Barrier(Rendering::ResourceBarrier::CreateTextureBarrier(
            copy.Texture, Rendering::ResourceState::ShaderResource,
            Rendering::ResourceState::CopyDest));
        commandList->CopyBufferToTextureSubresource(
            copy.Buffer, copy.Texture, 0, 0, copy.Width, copy.Height,
            copy.Offset, copy.RowPitch);
        commandList->Barrier(Rendering::ResourceBarrier::CreateTextureBarrier(
            copy.Texture, Rendering::ResourceState::CopyDest,
            Rendering::ResourceState::ShaderResource));
    }
    commandList->End();

    Rendering::CommandList* raw = commandList.get();
    m_Device->ExecuteCommandLists({raw});

    // Read the graphics timeline AFTER the submit. A concurrent submit can only
    // move it forward, and a later value on the same timeline still covers this
    // copy — over-waiting on slot reuse is safe, under-waiting is not.
    const uint32_t slot = m_Staging.GetCurrentSlot();
    m_SlotTokens[slot] = m_Device->LastGraphicsSubmissionToken();
    // Recorded whether or not a token came back: on a backend with no graphics
    // timeline this is the only evidence the slot is in flight.
    m_SlotWriteSeq[slot] = m_DeviceFrameSeq;

    ++m_SubmitCount;
    m_UploadCount += m_Staged.size();
    m_Staged.clear();
}

void VideoFrameUploader::ReprovisionAfterDeviceRebuild()
{
    m_Staged.clear();
    m_SlotTokens.fill({});
    // The rebuild drained every queue, so no slot is in flight any more.
    m_SlotWriteSeq.fill(0);
    m_SlotUsable = false;
    if (m_Staging.IsInitialized())
        m_Staging.ReprovisionAfterDeviceRebuild();
}

} // namespace GameEngine::Video
