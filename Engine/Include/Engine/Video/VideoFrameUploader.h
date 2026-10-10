#pragma once

#include "Rendering/Core/BufferRing.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/FrameBufferAllocator.h"
#include "Rendering/Core/Handle.h"

#include <array>
#include <cstdint>
#include <vector>

namespace GameEngine::Video
{

// Per-frame CPU -> GPU streaming path for decoded video frames.
//
// This is a STREAMING path, not an asset-load one: the same textures are
// rewritten every video frame for the life of the scene. It therefore owns its
// staging memory (a persistently-mapped ring, one slot per frame in flight)
// instead of creating and destroying an upload buffer per frame, and it records
// every frame's copies into ONE command list so entity count costs no extra
// queue submissions.
//
// SYNCHRONISATION. The copy is submitted on the GRAPHICS queue, from the caller's
// thread, at the caller's point in the frame — the same queue, thread and phase a
// per-entity ReuploadTexture2D used, so the hazard analysis is unchanged:
//   - Each texture's copy is bracketed ShaderResource -> CopyDest -> ShaderResource.
//     Naming ShaderResource as the source state is what orders the copy after the
//     previous frame's fragment reads of that same image; an Undefined source
//     resolves to TOP_OF_PIPE and would not order it at all.
//   - The transfer queue is NOT an option here: clampSrcStageToQueue masks FRAGMENT
//     off a non-graphics queue and zeroes srcAccess, which would silently restore
//     exactly that write-after-read hazard with no diagnostic.
//
// STAGING REUSE. A slot is written again only once its previous submit is known
// retired, established two ways:
//   - Each submit records the graphics timeline value it will signal, and the slot
//     is refused until that value is reached. This is the precise answer.
//   - A backend that publishes no graphics timeline hands back an INVALID token,
//     which IDevice::QueryGpuSyncToken reports as Unknown — it cannot separate
//     "never armed" from "already retired". Only VulkanDevice overrides
//     LastGraphicsSubmissionToken(); D3D12Device and MetalDevice inherit IDevice's
//     `return {}`. On Unknown the slot falls back to what ring rotation alone
//     guarantees: the allocator advances one slot per device frame over
//     framesInFlight+1 slots, so a slot last written in an EARLIER device frame has
//     been retired by the device's own frame pacing, and one written in THIS device
//     frame has not.
//
// MEMORY. The ring is permanently resident and persistently mapped for the life of
// the scene: framesInFlight+1 slots, each grown to the largest single frame it has
// been asked to stage (4 slots x 33 MB = 133 MB for one 3840x2160 RGBA8 source),
// capped per slot at kMaxStagingSlotBytes. With the source's own decode/ready/
// present buffers (~99 MB at 4K) and the sampled GPU texture (~33 MB), one 4K
// source costs roughly 265 MB resident. Against the per-frame create/destroy path
// this replaces, the ring's incremental cost is modest — but it converts churned
// allocations into permanently held ones.
class VideoFrameUploader
{
  public:
    explicit VideoFrameUploader(Rendering::IDevice* device);
    ~VideoFrameUploader();

    VideoFrameUploader(const VideoFrameUploader&) = delete;
    VideoFrameUploader& operator=(const VideoFrameUploader&) = delete;

    // Rotate to this frame's staging slot. When that slot's previous copy is still
    // in flight, Stage() refuses everything this tick and the textures keep showing
    // the frame they already hold.
    void BeginFrame();

    // Copy one decoded frame into staging and record its copy for this tick's
    // submission. Returns false when the slot is unusable or the ring is full; the
    // frame is then simply not shown (the texture holds its previous contents).
    bool Stage(Rendering::TextureHandle texture, const uint8_t* pixels,
               uint32_t width, uint32_t height, size_t rowPitchBytes);

    // Submit every copy staged since BeginFrame in one command list. No-op when
    // nothing was staged — a tick in which no video frame came due costs nothing.
    void Submit();

    // An in-place device rebuild freed the ring's buffers and its persistent maps,
    // and the timeline the slot tokens name no longer exists. Re-create the ring
    // and drop the tokens.
    void ReprovisionAfterDeviceRebuild();

    // Queue submissions issued, and frames copied, since construction. Config-
    // independent counters: uploads track the video's frame rate, submits track
    // ticks in which any video frame came due.
    uint64_t GetSubmitCount() const { return m_SubmitCount; }
    uint64_t GetUploadCount() const { return m_UploadCount; }

  private:
    // Vulkan requires a copy's buffer offset to be a multiple of the texel block
    // size; D3D12 requires 512-byte subresource placement. 512 satisfies both.
    static constexpr size_t kCopyOffsetAlignment = 512;
    // Matches D3D12_TEXTURE_DATA_PITCH_ALIGNMENT.
    static constexpr size_t kD3D12RowPitchAlignment = 256;
    static constexpr size_t kMinStagingSlotBytes = 1u << 20;   // 1 MiB
    static constexpr size_t kMaxStagingSlotBytes = 256u << 20; // 256 MiB

    struct StagedCopy
    {
        Rendering::TextureHandle Texture;
        Rendering::BufferHandle Buffer;
        size_t Offset = 0;
        uint32_t Width = 0;
        uint32_t Height = 0;
        size_t RowPitch = 0;
    };

    // Lazily create the ring on the first frame that needs it, so a scene with no
    // video entity never allocates staging memory at all.
    bool EnsureStaging(size_t frameBytes);
    size_t UploadRowPitch(size_t tightRowPitch) const;
    // Whether this slot's previous submit is known to have retired on the GPU. See
    // the STAGING REUSE note above for why absence of a token is not evidence of it.
    bool SlotRetired(uint32_t slot) const;

    Rendering::IDevice* m_Device = nullptr; // not owned
    Rendering::FrameBufferAllocator m_Staging;
    std::array<Rendering::IDevice::GpuSyncToken,
               Rendering::FrameBufferAllocator::kMaxRingSlots> m_SlotTokens{};
    // Device frame each slot's last submit was recorded in, 0 until one is.
    // Distinguishes a slot no submit ever touched from one whose submit yielded no
    // token because the backend keeps no graphics timeline.
    //
    // Counted here rather than taken from GetFrameIndex(): that value WRAPS at the
    // device's pacing, so a slot revisited a full rotation later would compare
    // equal to the frame it was written in and be refused forever. Only the fact
    // that the device's index CHANGED is used, which is also how
    // FrameBufferAllocator reads it.
    std::array<uint64_t,
               Rendering::FrameBufferAllocator::kMaxRingSlots> m_SlotWriteSeq{};
    std::vector<StagedCopy> m_Staged;
    uint64_t m_DeviceFrameSeq = 1; // never 0, so 0 means "never written"
    uint32_t m_FrameToken = 0;
    bool m_HasBegunFrame = false;
    bool m_SlotUsable = false;
    uint64_t m_SubmitCount = 0;
    uint64_t m_UploadCount = 0;
};

} // namespace GameEngine::Video
