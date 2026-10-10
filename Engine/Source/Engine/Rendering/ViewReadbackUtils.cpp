// ViewReadbackUtils.cpp - Implementation of non-blocking GPU readback for views.

#include "Engine/Rendering/ViewReadbackUtils.h"
#include "Logger/Logger.h"
#include "Mathematics/HalfFloat.h"
#include "Rendering/Core/BarrierMapping.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "UI/UITextureSpace.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace GameEngine::Rendering
{
namespace
{
// IEEE-754 binary16 bits -> float for an 8-bit display artifact. Subnormals flush
// to zero and inf/nan clamp to +-1: both are invisible at 8 bits, and the clamp
// keeps NaN out of the float -> uint8 conversion.
float HalfBitsToDisplayFloat(uint16_t v)
{
    const auto exponent = static_cast<uint16_t>(v & Mathematics::kHalfExponentMask);
    if (exponent == 0u)
        return 0.0f;
    if (exponent == Mathematics::kHalfExponentMask)
        return (v & Mathematics::kHalfSignMask) != 0u ? -1.0f : 1.0f;
    return Mathematics::HalfToFloat(v);
}

float LinearToSrgb01(float c)
{
    c = std::clamp(c, 0.0f, 1.0f);
    return c <= 0.0031308f ? c * 12.92f
                           : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

// Every per-pixel conversion below is a table lookup so the readback path stays
// branch/transcendental-free: movie capture runs it on every captured frame. Each
// table is built on first use, so a build that never reads back a given (format,
// space) pair never pays for its LUT.

// Linear half bits -> sRGB-encoded 8-bit.
const std::array<uint8_t, 65536>& HalfLinearToSrgb8Lut()
{
    static const std::array<uint8_t, 65536> lut = []
    {
        std::array<uint8_t, 65536> t{};
        for (uint32_t v = 0; v < 65536u; ++v)
            t[v] = static_cast<uint8_t>(
                LinearToSrgb01(HalfBitsToDisplayFloat(static_cast<uint16_t>(v))) * 255.0f + 0.5f);
        return t;
    }();
    return lut;
}

// Already-encoded half bits -> 8-bit, curve untouched: only the quantize.
const std::array<uint8_t, 65536>& HalfToUnorm8Lut()
{
    static const std::array<uint8_t, 65536> lut = []
    {
        std::array<uint8_t, 65536> t{};
        for (uint32_t v = 0; v < 65536u; ++v)
            t[v] = static_cast<uint8_t>(
                std::clamp(HalfBitsToDisplayFloat(static_cast<uint16_t>(v)), 0.0f, 1.0f) * 255.0f + 0.5f);
        return t;
    }();
    return lut;
}

// Linear 8-bit -> sRGB-encoded 8-bit. Lossy by construction (the source has no
// sub-LSB information to recover), which is what an 8-bit linear colour target
// costs — the conversion is honest about the space, not about the precision.
const std::array<uint8_t, 256>& Unorm8LinearToSrgb8Lut()
{
    static const std::array<uint8_t, 256> lut = []
    {
        std::array<uint8_t, 256> t{};
        for (uint32_t v = 0; v < 256u; ++v)
            t[v] = static_cast<uint8_t>(LinearToSrgb01(static_cast<float>(v) / 255.0f) * 255.0f + 0.5f);
        return t;
    }();
    return lut;
}

// RGB through the linear->sRGB table, alpha untouched (alpha is never curve-mapped).
void EncodeRgbInPlace(std::vector<uint8_t>& rgba8, size_t pixelCount)
{
    const std::array<uint8_t, 256>& lut = Unorm8LinearToSrgb8Lut();
    for (size_t i = 0; i < pixelCount; ++i)
    {
        rgba8[i * 4 + 0] = lut[rgba8[i * 4 + 0]];
        rgba8[i * 4 + 1] = lut[rgba8[i * 4 + 1]];
        rgba8[i * 4 + 2] = lut[rgba8[i * 4 + 2]];
    }
}
} // namespace

std::vector<uint8_t> ReadbackToRgba8Srgb(const ViewReadbackResult& result,
                                         UI::UITextureSpace sourceSpace)
{
    const size_t pixelCount = static_cast<size_t>(result.width) * result.height;
    if (pixelCount == 0 || result.pixels.empty())
        return {};

    // HdrLinear takes the same SDR curve as DisplayLinearSdr and clips above paper
    // white: this function's output is an 8-bit SDR artifact either way.
    const bool encode = !UI::IsEncodedAtRest(sourceSpace);

    std::vector<uint8_t> rgba8;
    switch (result.format)
    {
    case TextureFormat::RGBA8_UNORM:
    case TextureFormat::RGBA8_SRGB:
        if (result.pixels.size() >= pixelCount * 4)
        {
            rgba8.assign(result.pixels.begin(), result.pixels.begin() + pixelCount * 4);
            if (encode)
                EncodeRgbInPlace(rgba8, pixelCount);
        }
        break;
    case TextureFormat::BGRA8_UNORM:
    case TextureFormat::BGRA8_SRGB:
        if (result.pixels.size() >= pixelCount * 4)
        {
            rgba8.resize(pixelCount * 4);
            const uint8_t* src = result.pixels.data();
            for (size_t i = 0; i < pixelCount; ++i)
            {
                rgba8[i * 4 + 0] = src[i * 4 + 2]; // B -> R
                rgba8[i * 4 + 1] = src[i * 4 + 1]; // G
                rgba8[i * 4 + 2] = src[i * 4 + 0]; // R -> B
                rgba8[i * 4 + 3] = src[i * 4 + 3]; // A
            }
            if (encode)
                EncodeRgbInPlace(rgba8, pixelCount);
        }
        break;
    case TextureFormat::R16G16B16A16_FLOAT:
        if (result.pixels.size() >= pixelCount * 8)
        {
            rgba8.resize(pixelCount * 4);
            const std::array<uint8_t, 65536>& lut =
                encode ? HalfLinearToSrgb8Lut() : HalfToUnorm8Lut();
            const uint16_t* src = reinterpret_cast<const uint16_t*>(result.pixels.data());
            for (size_t i = 0; i < pixelCount; ++i)
            {
                rgba8[i * 4 + 0] = lut[src[i * 4 + 0]];
                rgba8[i * 4 + 1] = lut[src[i * 4 + 1]];
                rgba8[i * 4 + 2] = lut[src[i * 4 + 2]];
                rgba8[i * 4 + 3] = 255; // opaque; alpha is not curve-mapped
            }
        }
        break;
    default:
        break; // unsupported format -> empty
    }
    return rgba8;
}

// ── RenderGraph arm (slice 8c-1) ─────────────────────────────────────────────────────

namespace
{
// Pending stamps, main-thread only (declared during declaration scope,
// stamped right after Execute — both on the frame driver's thread). weak_ptr
// so an abandoned ticket never leaks an entry.
struct PendingReadbackStamp
{
    std::weak_ptr<RGReadbackTicket> Ticket;
    const RenderGraph::RGFrame* Frame = nullptr;
    uint64_t FrameIndex = 0;
};
std::vector<PendingReadbackStamp> s_PendingReadbackStamps;

struct PendingBufferReadbackStamp
{
    std::weak_ptr<RGBufferReadbackTicket> Ticket;
    const RenderGraph::RGFrame* Frame = nullptr;
    uint64_t FrameIndex = 0;
};
std::vector<PendingBufferReadbackStamp> s_PendingBufferReadbackStamps;

// No ticket access here (the anon-ns helper is not a friend): declare the
// pass + create the buffer, hand the fields back for the friend functions
// to seat into the ticket.
struct DeclaredReadback
{
    BufferHandle Buffer{};
    uint32 Width = 0;
    uint32 Height = 0;
    size_t RowPitch = 0;
    TextureFormat Format = TextureFormat::RGBA8_UNORM;
    bool Ok = false;
};

// Region (0,0,0,0) means full texture extent. Regions are clamped to the
// source desc; a region clamped to nothing fails the declare.
DeclaredReadback DeclareReadbackPass(IDevice* device, RenderGraph::RGFrame& frame, RenderGraph::RGTexture src,
                                     const char* debugName, uint32_t srcX = 0, uint32_t srcY = 0,
                                     uint32_t regionW = 0, uint32_t regionH = 0)
{
    DeclaredReadback out{};
    const auto& desc = frame.Graph().ResourceDesc(src.Id);
    if (desc.SampleCount > 1)
    {
        Logger::Log::Warning("[RGReadback] '{}' is multisampled — readback skipped (resolve first)",
                             debugName ? debugName : "Anon");
        return out;
    }
    const TextureFormat fmt =
        desc.Format != 0 ? static_cast<TextureFormat>(desc.Format) : TextureFormat::RGBA8_UNORM;
    const uint32_t bpp = BytesPerPixel(fmt);
    if (desc.Width == 0 || desc.Height == 0 || bpp == 0)
        return out;
    if (srcX >= desc.Width || srcY >= desc.Height)
        return out;
    const uint32_t w = std::min(regionW != 0 ? regionW : desc.Width, desc.Width - srcX);
    const uint32_t h = std::min(regionH != 0 ? regionH : desc.Height, desc.Height - srcY);
    if (w == 0 || h == 0)
        return out;

    const size_t tightRowPitch = static_cast<size_t>(w) * bpp;
    const size_t alignment = std::max<size_t>(1, device->GetCapabilities().textureCopyRowPitchAlignment);
    const size_t rowPitch = (tightRowPitch + alignment - 1) / alignment * alignment;
    const size_t bufferSize = rowPitch * h;
    const std::string bufferLabel = std::string("RGReadback.") + (debugName ? debugName : "Anon");
    BufferHandle buffer = device->CreateReadbackBuffer(bufferSize, bufferLabel.c_str());
    if (!buffer.IsValid())
        return out;

    frame.AddPass(
        bufferLabel.c_str(), static_cast<int32_t>(PassPhase::kFinalize),
        [&](RenderGraph::RGPassBuilder& p)
        {
            p.Read(src, RenderGraph::RGTextureRead::CopySrc);
            // The copy's output is CPU-side — no in-graph consumer exists.
            p.PreventCulling();
        },
        [src, buffer, w, h, srcX, srcY, rowPitch](RenderGraph::RGContext& ctx)
        {
            const TextureHandle tex = ctx.GetTexture(src);
            if (tex.IsValid())
                ctx.Cmd->CopyTextureToBuffer(tex, buffer, w, h, srcX, srcY, 0, rowPitch);
        });

    out.Buffer = buffer;
    out.Width = w;
    out.Height = h;
    out.RowPitch = rowPitch;
    out.Format = fmt;
    out.Ok = true;
    return out;
}

DeclaredReadback DeclareSubresourceReadbackPass(IDevice* device, RenderGraph::RGFrame& frame,
                                                RenderGraph::RGTexture src, uint32_t mip,
                                                uint32_t layer, const char* debugName,
                                                uint32_t srcX, uint32_t srcY,
                                                uint32_t regionW, uint32_t regionH)
{
    DeclaredReadback out{};
    const auto& desc = frame.Graph().ResourceDesc(src.Id);
    if (desc.SampleCount > 1)
    {
        Logger::Log::Warning("[RGReadback] '{}' is multisampled — readback skipped (resolve first)",
                             debugName ? debugName : "Anon");
        return out;
    }
    const TextureFormat fmt =
        desc.Format != 0 ? static_cast<TextureFormat>(desc.Format) : TextureFormat::RGBA8_UNORM;
    const uint32_t bpp = BytesPerPixel(fmt);
    if (desc.Width == 0 || desc.Height == 0 || bpp == 0)
        return out;

    const uint32_t mipCount = desc.MipLevels > 0 ? desc.MipLevels : 1u;
    const uint32_t layerCount = desc.ArrayLayers > 0 ? desc.ArrayLayers : 1u;
    if (mip >= mipCount || layer >= layerCount)
        return out;

    const uint32_t mipW = std::max(1u, desc.Width >> mip);
    const uint32_t mipH = std::max(1u, desc.Height >> mip);
    if (srcX >= mipW || srcY >= mipH)
        return out;
    const uint32_t w = std::min(regionW != 0 ? regionW : mipW, mipW - srcX);
    const uint32_t h = std::min(regionH != 0 ? regionH : mipH, mipH - srcY);
    if (w == 0 || h == 0)
        return out;

    const size_t tightRowPitch = static_cast<size_t>(w) * bpp;
    const size_t alignment = std::max<size_t>(1, device->GetCapabilities().textureCopyRowPitchAlignment);
    const size_t rowPitch = (tightRowPitch + alignment - 1) / alignment * alignment;
    const size_t bufferSize = rowPitch * h;
    const std::string bufferLabel = std::string("RGReadback.") + (debugName ? debugName : "Anon");
    BufferHandle buffer = device->CreateReadbackBuffer(bufferSize, bufferLabel.c_str());
    if (!buffer.IsValid())
        return out;

    RenderGraph::RGRange range{};
    range.BaseMip = mip;
    range.MipCount = 1;
    range.BaseLayer = layer;
    range.LayerCount = 1;
    frame.AddPass(
        bufferLabel.c_str(), static_cast<int32_t>(PassPhase::kFinalize),
        [&](RenderGraph::RGPassBuilder& p)
        {
            p.Read(src, RenderGraph::RGTextureRead::CopySrc, range);
            p.PreventCulling();
        },
        [src, buffer, mip, layer, w, h, srcX, srcY, rowPitch](RenderGraph::RGContext& ctx)
        {
            const TextureHandle tex = ctx.GetTexture(src);
            if (tex.IsValid())
                ctx.Cmd->CopyTextureSubresourceToBuffer(tex, mip, layer, buffer, w, h,
                                                        srcX, srcY, 0, rowPitch);
        });

    out.Buffer = buffer;
    out.Width = w;
    out.Height = h;
    out.RowPitch = rowPitch;
    out.Format = fmt;
    out.Ok = true;
    return out;
}
} // namespace

RGReadbackTicket::~RGReadbackTicket()
{
    Cancel();
}

bool RGReadbackTicket::TryGet(ViewReadbackResult& out)
{
    if (!m_Device || m_Consumed || !m_Buffer.IsValid())
        return false;
    if (m_Device->GetDeviceRebuildGeneration() != m_DeviceRebuildGen)
    {
        // A device rebuild freed the buffer and the token's timeline: this
        // readback can never resolve. Fail it observably (IsConsumed) so
        // holders retry or error instead of polling forever, and forget the
        // dead handle without a Destroy (the teardown already freed it).
        m_Buffer = BufferHandle{};
        m_Consumed = true;
        return false;
    }
    // Deterministic completion only: stamped (the declaring frame really was
    // submitted) AND the GPU proved it finished that submission. No frame-count
    // fallback — an unstamped ticket, or one whose token carries no completion
    // information, can never resolve.
    if (!m_Stamped)
        return false;
    if (m_Device->QueryGpuSyncToken(m_Token) != IDevice::GpuSyncStatus::Complete)
        return false;

    void* mapped = m_Device->MapBuffer(m_Buffer);
    if (!mapped)
        return false;

    const uint32_t bpp = BytesPerPixel(m_Format);
    const size_t tightRowPitch = static_cast<size_t>(m_Width) * (bpp > 0 ? bpp : 4);
    const size_t bytes = tightRowPitch * m_Height;
    out.viewId = 0;
    out.width = m_Width;
    out.height = m_Height;
    out.format = m_Format;
    if (m_RowPitch == tightRowPitch)
        out.pixels.assign(static_cast<uint8_t*>(mapped), static_cast<uint8_t*>(mapped) + bytes);
    else
    {
        // GPU copies may pad each row; image consumers receive tightly packed pixels.
        out.pixels.resize(bytes);
        for (uint32 y = 0; y < m_Height; ++y)
            std::memcpy(out.pixels.data() + y * tightRowPitch,
                        static_cast<uint8_t*>(mapped) + y * m_RowPitch, tightRowPitch);
    }

    m_Device->UnmapBuffer(m_Buffer);
    m_Device->DestroyBuffer(m_Buffer); // timeline-deferred on the device side
    m_Buffer = BufferHandle{};
    m_Consumed = true;
    return true;
}

bool RGReadbackTicket::WaitUntilReady(uint64_t timeoutNs)
{
    // Same unresolvable cases TryGet rejects, minus its side effects: this is a
    // wait, not a consume, so a rebuild-killed ticket is left for TryGet to fail
    // observably rather than being retired here.
    if (!m_Device || m_Consumed || !m_Buffer.IsValid())
        return false;
    if (m_Device->GetDeviceRebuildGeneration() != m_DeviceRebuildGen)
        return false;
    if (!m_Stamped)
        return false;
    return m_Device->WaitGpuSyncToken(m_Token, timeoutNs);
}

void RGReadbackTicket::Cancel()
{
    // On a stale rebuild generation the teardown already freed the buffer:
    // forget the handle rather than Destroy a dead one.
    if (m_Device && m_Buffer.IsValid() &&
        m_Device->GetDeviceRebuildGeneration() == m_DeviceRebuildGen)
    {
        // Device buffer destroys are timeline-deferred — safe even when the
        // copy submission is still in flight.
        m_Device->DestroyBuffer(m_Buffer);
    }
    m_Buffer = BufferHandle{};
    m_Consumed = true;
}

RGBufferReadbackTicket::~RGBufferReadbackTicket()
{
    Cancel();
}

bool RGBufferReadbackTicket::TryGet(BufferReadbackResult& out)
{
    if (!m_Device || m_Consumed || !m_Buffer.IsValid())
        return false;
    if (m_Device->GetDeviceRebuildGeneration() != m_DeviceRebuildGen)
    {
        // Same contract as RGReadbackTicket::TryGet: a rebuild killed this
        // readback — fail observably, forget the dead handle without Destroy.
        m_Buffer = BufferHandle{};
        m_Consumed = true;
        return false;
    }
    if (!m_Stamped)
        return false;
    if (m_Device->QueryGpuSyncToken(m_Token) != IDevice::GpuSyncStatus::Complete)
        return false;

    void* mapped = m_Device->MapBuffer(m_Buffer);
    if (!mapped)
        return false;

    out.bytes.assign(static_cast<uint8_t*>(mapped),
                     static_cast<uint8_t*>(mapped) + m_ByteCount);

    m_Device->UnmapBuffer(m_Buffer);
    m_Device->DestroyBuffer(m_Buffer);
    m_Buffer = BufferHandle{};
    m_Consumed = true;
    return true;
}

void RGBufferReadbackTicket::Cancel()
{
    if (m_Device && m_Buffer.IsValid() &&
        m_Device->GetDeviceRebuildGeneration() == m_DeviceRebuildGen)
    {
        m_Device->DestroyBuffer(m_Buffer);
    }
    m_Buffer = BufferHandle{};
    m_Consumed = true;
}

std::shared_ptr<RGReadbackTicket> RequestTextureRegionReadbackRG(IDevice* device,
                                                                 RenderGraph::RGFrame& frame,
                                                                 RenderGraph::RGTexture src, uint32 srcX,
                                                                 uint32 srcY, uint32 width,
                                                                 uint32 height,
                                                                 const char* debugName)
{
    if (!device || !src.IsValid())
        return nullptr;
    const DeclaredReadback d =
        DeclareReadbackPass(device, frame, src, debugName, srcX, srcY, width, height);
    if (!d.Ok)
        return nullptr;
    auto ticket = std::make_shared<RGReadbackTicket>();
    ticket->m_Device = device;
    ticket->m_Buffer = d.Buffer;
    ticket->m_Width = d.Width;
    ticket->m_Height = d.Height;
    ticket->m_RowPitch = d.RowPitch;
    ticket->m_Format = d.Format;
    ticket->m_DeviceRebuildGen = device->GetDeviceRebuildGeneration();
    s_PendingReadbackStamps.push_back({ticket, &frame, frame.FrameIndex()});
    return ticket;
}

std::shared_ptr<RGReadbackTicket> RequestTextureReadbackRG(IDevice* device, RenderGraph::RGFrame& frame,
                                                           RenderGraph::RGTexture src,
                                                           const char* debugName)
{
    return RequestTextureRegionReadbackRG(device, frame, src, 0, 0, 0, 0, debugName);
}

std::shared_ptr<RGReadbackTicket> RequestTextureSubresourceReadbackRG(
    IDevice* device, RenderGraph::RGFrame& frame, RenderGraph::RGTexture src,
    uint32 mip, uint32 layer, uint32 srcX, uint32 srcY, uint32 width,
    uint32 height, const char* debugName)
{
    if (!device || !src.IsValid())
        return nullptr;
    const DeclaredReadback d = DeclareSubresourceReadbackPass(
        device, frame, src, mip, layer, debugName, srcX, srcY, width, height);
    if (!d.Ok)
        return nullptr;
    auto ticket = std::make_shared<RGReadbackTicket>();
    ticket->m_Device = device;
    ticket->m_Buffer = d.Buffer;
    ticket->m_Width = d.Width;
    ticket->m_Height = d.Height;
    ticket->m_RowPitch = d.RowPitch;
    ticket->m_Format = d.Format;
    ticket->m_DeviceRebuildGen = device->GetDeviceRebuildGeneration();
    s_PendingReadbackStamps.push_back({ticket, &frame, frame.FrameIndex()});
    return ticket;
}

std::shared_ptr<RGReadbackTicket> RequestDeviceTextureReadbackRG(IDevice* device,
                                                                 RenderGraph::RGFrame& frame,
                                                                 TextureHandle texture,
                                                                 ResourceState currentState,
                                                                 const char* debugName)
{
    if (!device || !texture.IsValid())
        return nullptr;
    const std::string importName = std::string("RGReadback.Import.") + (debugName ? debugName : "Anon");
    const RenderGraph::RGTexture src = frame.ImportExternalTexture(
        importName.c_str(), texture, currentState, device->GetTextureFormat(texture));
    if (!src.IsValid())
        return nullptr;
    // Round-trip the import claim: the copy leaves the image TRANSFER_SRC, but
    // the caller keeps treating it as `currentState` and the next capture
    // imports at that state again — under the graph-authoritative oldLayout
    // rule that stale claim records a transition whose oldLayout mismatches
    // the image's actual layout. A contractual export restores the declared
    // layout after the last access. Only ShaderReadOnly/General are export
    // contracts; a caller declaring CopySource needs no restore (the copy
    // leaves exactly that), and same-frame dedup shares one contract (both
    // capture-class importers declare the texture's resting sampled state).
    const RenderGraph::RGImageLayout declared = RenderGraph::ToImageLayout(currentState);
    if (declared == RenderGraph::RGImageLayout::ShaderReadOnly ||
        declared == RenderGraph::RGImageLayout::General)
        frame.MarkOutput(src, declared);
    return RequestTextureReadbackRG(device, frame, src, debugName);
}

std::shared_ptr<RGBufferReadbackTicket> RequestBufferReadbackRG(
    IDevice* device, RenderGraph::RGFrame& frame, RenderGraph::RGBuffer src,
    size_t srcOffset, size_t byteCount, const char* debugName)
{
    if (!device || !src.IsValid() || byteCount == 0)
        return nullptr;

    const std::string bufferLabel = std::string("RGReadback.") + (debugName ? debugName : "Buffer");
    BufferHandle buffer = device->CreateReadbackBuffer(byteCount, bufferLabel.c_str());
    if (!buffer.IsValid())
        return nullptr;

    frame.AddPass(
        bufferLabel.c_str(), static_cast<int32_t>(PassPhase::kFinalize),
        [&](RenderGraph::RGPassBuilder& p)
        {
            p.Read(src, RenderGraph::RGBufferRead::CopySrc);
            p.PreventCulling();
        },
        [src, buffer, srcOffset, byteCount](RenderGraph::RGContext& ctx)
        {
            const BufferHandle srcBuffer = ctx.GetBuffer(src);
            if (srcBuffer.IsValid())
            {
                ctx.Cmd->Barrier(ResourceBarrier::CreateMemoryBarrier(
                    static_cast<uint64_t>(PipelineStageMask::Transfer)
                        | static_cast<uint64_t>(PipelineStageMask::ComputeShader),
                    static_cast<uint64_t>(PipelineStageMask::Transfer),
                    static_cast<uint64_t>(ResourceAccessMask::TransferWrite)
                        | static_cast<uint64_t>(ResourceAccessMask::ShaderWrite),
                    static_cast<uint64_t>(ResourceAccessMask::TransferRead)));
                ctx.Cmd->CopyBuffer(srcBuffer, buffer, byteCount, srcOffset, 0);
            }
        });

    auto ticket = std::make_shared<RGBufferReadbackTicket>();
    ticket->m_Device = device;
    ticket->m_Buffer = buffer;
    ticket->m_ByteCount = byteCount;
    ticket->m_DeviceRebuildGen = device->GetDeviceRebuildGeneration();
    s_PendingBufferReadbackStamps.push_back({ticket, &frame, frame.FrameIndex()});
    return ticket;
}

void OnFrameSubmittedReadbacksRG(RenderGraph::RGFrame& frame, const IDevice::GpuSyncToken& token)
{
    const uint64_t frameIndex = frame.FrameIndex();
    for (auto it = s_PendingReadbackStamps.begin(); it != s_PendingReadbackStamps.end();)
    {
        auto ticket = it->Ticket.lock();
        if (!ticket)
        {
            it = s_PendingReadbackStamps.erase(it); // abandoned ticket
            continue;
        }
        if (it->Frame != &frame)
        {
            ++it; // another stream's pending — its own submit stamps it
            continue;
        }
        if (it->FrameIndex != frameIndex)
        {
            // The frame was re-begun before this incarnation's stamp: the
            // declaration died with the old graph — the copy never recorded.
            // Cancel so single-in-flight consumers (thumbnail disk cache) can
            // observe IsConsumed and retry instead of wedging forever.
            ticket->Cancel();
            it = s_PendingReadbackStamps.erase(it);
            continue;
        }
        ticket->m_Token = token;
        ticket->m_Stamped = true;
        it = s_PendingReadbackStamps.erase(it);
    }

    for (auto it = s_PendingBufferReadbackStamps.begin();
         it != s_PendingBufferReadbackStamps.end();)
    {
        auto ticket = it->Ticket.lock();
        if (!ticket)
        {
            it = s_PendingBufferReadbackStamps.erase(it);
            continue;
        }
        if (it->Frame != &frame)
        {
            ++it;
            continue;
        }
        if (it->FrameIndex != frameIndex)
        {
            ticket->Cancel();
            it = s_PendingBufferReadbackStamps.erase(it);
            continue;
        }
        ticket->m_Token = token;
        ticket->m_Stamped = true;
        it = s_PendingBufferReadbackStamps.erase(it);
    }
}

void CancelPendingReadbacksRG(const RenderGraph::RGFrame* frame)
{
    for (auto it = s_PendingReadbackStamps.begin(); it != s_PendingReadbackStamps.end();)
    {
        if (it->Frame != frame)
        {
            ++it;
            continue;
        }
        if (auto ticket = it->Ticket.lock())
            ticket->Cancel();
        it = s_PendingReadbackStamps.erase(it);
    }
    for (auto it = s_PendingBufferReadbackStamps.begin();
         it != s_PendingBufferReadbackStamps.end();)
    {
        if (it->Frame != frame)
        {
            ++it;
            continue;
        }
        if (auto ticket = it->Ticket.lock())
            ticket->Cancel();
        it = s_PendingBufferReadbackStamps.erase(it);
    }
}

} // namespace GameEngine::Rendering
