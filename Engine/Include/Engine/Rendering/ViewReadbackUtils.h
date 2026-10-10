// ViewReadbackUtils.h - Non-blocking GPU readback utility for RenderGraph frames.
//
// This utility provides async readback of GPU textures without stalling the
// rendering pipeline. A readback pass is declared into an RenderGraph frame at the
// finalize phase, then the result is mapped on a later frame when polled.
//
// Usage:
//   auto ticket = RequestTextureReadbackRG(device, frame, srcTexture, "Name");
//   // On a later frame (after the declaring frame is submitted):
//   ViewReadbackResult result;
//   if (ticket->TryGet(result)) { /* use result.pixels */ }

#pragma once

#include "Rendering/CameraTypes.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/Handle.h"
#include "Types/Types.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace GameEngine
{
// The readback layer states colour spaces in the UI module's vocabulary so a
// consumer can hand over the producer's stamp unchanged
// (RenderServices::GetPipelineOutputSpaceRG) instead of re-deriving one. Kept
// forward-declared: modules below the UI layer include this header, and only
// the sites that actually name a space include UI/UITextureSpace.h.
namespace UI
{
class UITextureSpace;
}

namespace Rendering
{
namespace RenderGraph
{
class RGFrame;
struct RGBuffer;
struct RGTexture;
}

// Result of a completed readback
struct ViewReadbackResult
{
    ViewId viewId = 0;
    uint32 width = 0;
    uint32 height = 0;
    TextureFormat format = TextureFormat::RGBA8_UNORM;
    std::vector<uint8_t> pixels; // Tightly packed pixel data in `format` layout
};

struct BufferReadbackResult
{
    std::vector<uint8_t> bytes;
};

// Convert a view readback to tightly-packed, sRGB-encoded 8-bit RGBA (R,G,B,A order).
//
// `sourceSpace` states what the readback bytes hold and is the ONLY input to the
// transfer-curve decision; `result.format` decides how to unpack them and nothing
// else. A format never implies a space: an F16 target can legitimately hold encoded
// bytes, and inferring "F16 therefore linear" is how a readback double-encodes.
//
// - SrgbAuthored     -> the bytes are already encoded; no curve is applied.
// - DisplayLinearSdr -> linear->sRGB is applied.
// - HdrLinear        -> same curve, and values above paper white CLIP: this function's
//                       output is an 8-bit SDR artifact, so an HDR frame loses its
//                       highlights here.
//
// Unpacks RGBA8_UNORM/SRGB (copy), BGRA8_UNORM/SRGB (channel swizzle) and
// R16G16B16A16_FLOAT (half-decode). Float sources encode before the quantize; 8-bit
// sources have no sub-LSB information left, so a linear 8-bit source encodes in 8-bit
// and banding is expected (the format cannot hold linear content — see
// UI::CheckTextureSpaceAgainstFormat). Alpha is never curve-mapped; float sources emit
// opaque alpha. Every per-pixel path is a table lookup: movie capture runs this on
// every captured frame. Returns an empty vector for unsupported formats.
std::vector<uint8_t> ReadbackToRgba8Srgb(const ViewReadbackResult& result,
                                         ::GameEngine::UI::UITextureSpace sourceSpace);

// ── RenderGraph arm (slice 8c-1). Immediate-mode readbacks: the pass is declared
// fresh into the frame (no retained pass, no consumed-gate machinery), the
// readback buffer is created at declaration sized from the frame's resource
// desc, and completion is DETERMINISTIC — the ticket resolves only once its
// frame's submission token is stamped (post-Execute, via
// OnFrameSubmittedReadbacksRG) AND signaled. No frame-count fallback: an
// unstamped ticket (frame abandoned before Execute, or re-begun before the
// stamp) never resolves and frees its buffer on destruction (device buffer
// destroys are timeline-deferred). Episodic by design — one dedicated buffer
// per request; every-frame readbacks belong on a FiF+1 ring, not here.
//
// Device rebuild: an in-place rebuild between declare and resolve freed both the
// readback buffer and the token's timeline, so the ticket can never resolve.
// TryGet fails such a ticket observably (IsConsumed flips true, the dead handle
// is forgotten without a Destroy) so holders retry or error instead of polling
// forever. ──
class RGReadbackTicket
{
  public:
    ~RGReadbackTicket();
    RGReadbackTicket() = default;
    RGReadbackTicket(const RGReadbackTicket&) = delete;
    RGReadbackTicket& operator=(const RGReadbackTicket&) = delete;

    // True once the frame that declared this readback has been submitted
    // (token stamped). Unstamped tickets can never resolve.
    bool IsStamped() const { return m_Stamped; }
    bool IsConsumed() const { return m_Consumed; }
    // Poll: succeeds exactly once, when stamped AND the GPU copy completed.
    // Fills `out` (viewId = 0) and releases the buffer.
    bool TryGet(ViewReadbackResult& out);
    // Block until the declaring frame's submission has completed, so the next
    // TryGet resolves. Waits that submission's own timeline value — never a
    // device drain, so it is legal from the render-graph declare phase. False
    // (immediately) when the ticket can never resolve — unstamped, already
    // consumed, or its device was rebuilt — or when the wait timed out; the
    // caller's next TryGet is what reports an unresolvable ticket observably.
    bool WaitUntilReady(uint64_t timeoutNs = ~0ull);
    // Release without consuming (buffer destroy is timeline-deferred).
    void Cancel();

  private:
    friend std::shared_ptr<RGReadbackTicket> RequestTextureReadbackRG(
        IDevice* device, RenderGraph::RGFrame& frame, RenderGraph::RGTexture src, const char* debugName);
    friend std::shared_ptr<RGReadbackTicket> RequestTextureRegionReadbackRG(
        IDevice* device, RenderGraph::RGFrame& frame, RenderGraph::RGTexture src, uint32 srcX, uint32 srcY,
        uint32 width, uint32 height, const char* debugName);
    friend std::shared_ptr<RGReadbackTicket> RequestTextureSubresourceReadbackRG(
        IDevice* device, RenderGraph::RGFrame& frame, RenderGraph::RGTexture src, uint32 mip, uint32 layer,
        uint32 srcX, uint32 srcY, uint32 width, uint32 height, const char* debugName);
    friend std::shared_ptr<RGReadbackTicket> RequestDeviceTextureReadbackRG(
        IDevice* device, RenderGraph::RGFrame& frame, TextureHandle texture, ResourceState currentState,
        const char* debugName);
    friend void OnFrameSubmittedReadbacksRG(RenderGraph::RGFrame& frame,
                                            const IDevice::GpuSyncToken& token);

    IDevice* m_Device = nullptr;
    BufferHandle m_Buffer{};
    uint32 m_Width = 0;
    uint32 m_Height = 0;
    size_t m_RowPitch = 0;
    TextureFormat m_Format = TextureFormat::RGBA8_UNORM;
    IDevice::GpuSyncToken m_Token{};
    // Rebuild generation at declare: a mismatch means the buffer and the
    // token's timeline died with the old device (fail observably, forget the
    // handles without Destroy).
    uint64_t m_DeviceRebuildGen = 0;
    bool m_Stamped = false;
    bool m_Consumed = false;
};

class RGBufferReadbackTicket
{
  public:
    ~RGBufferReadbackTicket();
    RGBufferReadbackTicket() = default;
    RGBufferReadbackTicket(const RGBufferReadbackTicket&) = delete;
    RGBufferReadbackTicket& operator=(const RGBufferReadbackTicket&) = delete;

    bool IsStamped() const { return m_Stamped; }
    bool IsConsumed() const { return m_Consumed; }
    bool TryGet(BufferReadbackResult& out);
    void Cancel();

  private:
    friend std::shared_ptr<RGBufferReadbackTicket> RequestBufferReadbackRG(
        IDevice* device, RenderGraph::RGFrame& frame, RenderGraph::RGBuffer src,
        size_t srcOffset, size_t byteCount, const char* debugName);
    friend void OnFrameSubmittedReadbacksRG(RenderGraph::RGFrame& frame,
                                            const IDevice::GpuSyncToken& token);

    IDevice* m_Device = nullptr;
    BufferHandle m_Buffer{};
    size_t m_ByteCount = 0;
    IDevice::GpuSyncToken m_Token{};
    // Same rebuild-generation contract as RGReadbackTicket.
    uint64_t m_DeviceRebuildGen = 0;
    bool m_Stamped = false;
    bool m_Consumed = false;
};

// Declare a CopySrc read of `src` (must be single-sample) plus a copy into a
// freshly created readback buffer, at the finalize phase of `frame`. Returns
// null on invalid src/MSAA source/buffer-creation failure. Main thread,
// declaration scope (before Execute).
std::shared_ptr<RGReadbackTicket> RequestTextureReadbackRG(IDevice* device, RenderGraph::RGFrame& frame,
                                                           RenderGraph::RGTexture src,
                                                           const char* debugName);

// Region variant (8e-7): reads back a sub-rectangle of `src`. width/height 0
// mean "to the texture edge"; the region is clamped to the source extent and
// an empty clamped region fails. The buffer holds width*height*bpp tightly
// packed rows.
std::shared_ptr<RGReadbackTicket> RequestTextureRegionReadbackRG(
    IDevice* device, RenderGraph::RGFrame& frame, RenderGraph::RGTexture src, uint32 srcX, uint32 srcY,
    uint32 width, uint32 height, const char* debugName);

// Subresource variant for array/mip diagnostics. Reads one mip/layer into a
// tightly packed buffer; width/height 0 mean "to the subresource edge".
std::shared_ptr<RGReadbackTicket> RequestTextureSubresourceReadbackRG(
    IDevice* device, RenderGraph::RGFrame& frame, RenderGraph::RGTexture src, uint32 mip, uint32 layer,
    uint32 srcX, uint32 srcY, uint32 width, uint32 height, const char* debugName);

// Convenience: import an externally-owned device texture (video preview
// frame, handler-owned thumbnail slot) at its CURRENT state and read it back.
std::shared_ptr<RGReadbackTicket> RequestDeviceTextureReadbackRG(IDevice* device,
                                                                 RenderGraph::RGFrame& frame,
                                                                 TextureHandle texture,
                                                                 ResourceState currentState,
                                                                 const char* debugName);

// Buffer readback variant for diagnostics and GPU-generated stream inspection.
std::shared_ptr<RGBufferReadbackTicket> RequestBufferReadbackRG(
    IDevice* device, RenderGraph::RGFrame& frame, RenderGraph::RGBuffer src,
    size_t srcOffset, size_t byteCount, const char* debugName);

// Stamp every pending ticket declared against (&frame, frame.FrameIndex()).
// The frame driver calls this ONCE per frame-stream right after
// frame.Execute(), next to RenderServices::OnFrameSubmittedRG. Pendings of a
// re-begun or never-submitted incarnation are dropped, never mis-stamped.
void OnFrameSubmittedReadbacksRG(RenderGraph::RGFrame& frame, const IDevice::GpuSyncToken& token);

// Cancel + drop every pending stamp declared against this frame stream.
// MUST be called before destroying an RGFrame whose tickets may be held by
// app-global consumers (thumbnail disk cache, bookmarks, debug captures) —
// without it a dangling Frame* lingers in the stamp list and a heap-recycled
// RGFrame at the same address whose FrameIndex collides would MIS-STAMP a
// foreign token onto the ticket. (8e: windows other than [0] own frames and
// can close mid-session.)
void CancelPendingReadbacksRG(const RenderGraph::RGFrame* frame);

} // namespace Rendering
} // namespace GameEngine
