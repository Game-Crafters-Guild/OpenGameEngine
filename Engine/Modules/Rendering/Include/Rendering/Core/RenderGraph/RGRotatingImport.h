#pragma once

// N-buffer rotation for the FEW genuine cross-frame rotators (TAA history, async
// readback, shadow double-buffer), over the engine's real IDevice. Current(frame)
// returns the slot to write this frame; Previous(frame) returns last frame's slot
// to read as history. This is the ONLY rotation primitive in RenderGraph — ordinary
// per-frame CPU data uses RGUploadRing.
//
// COUNT CONTRACT: for GPU-only rotators (history read by the next frame's GPU
// work on the same queue), count >= 2 suffices: Current(f) != Current(f-1).
// For CPU-READBACK rotators (the CPU maps/reads a slot the GPU wrote N frames
// ago), count >= framesInFlight + 1 is required — with fewer slots the CPU reads
// a slot a still-in-flight frame may be writing.

#include "Rendering/Core/Device.h"

#include <cassert>
#include <cstdint>
#include <vector>

namespace GameEngine::Rendering::RenderGraph
{

class RGRotatingTexture
{
  public:
    RGRotatingTexture() = default;
    ~RGRotatingTexture() { assert(m_Handles.empty() && "RGRotatingTexture: call Destroy() before destruction"); }
    RGRotatingTexture(const RGRotatingTexture&) = delete;
    RGRotatingTexture& operator=(const RGRotatingTexture&) = delete;

    void Init(IDevice* device, const TextureDesc& desc, uint32_t count)
    {
        assert(m_Handles.empty());
        m_Handles.reserve(count);
        for (uint32_t i = 0; i < count; ++i)
            m_Handles.push_back(device->CreateTexture(desc));
    }
    void Destroy(IDevice* device)
    {
        for (TextureHandle h : m_Handles)
            device->DestroyTexture(h);
        m_Handles.clear();
    }
    uint32_t Count() const { return static_cast<uint32_t>(m_Handles.size()); }
    TextureHandle Current(uint64_t frameIndex) const
    {
        assert(!m_Handles.empty() && "RGRotatingTexture: Current() before Init()"); // modulo-by-zero UB otherwise
        return m_Handles[frameIndex % m_Handles.size()];
    }
    TextureHandle Previous(uint64_t frameIndex) const
    {
        assert(!m_Handles.empty() && "RGRotatingTexture: Previous() before Init()");
        const uint64_t n = m_Handles.size();
        return m_Handles[(frameIndex + n - 1) % n];
    }

  private:
    std::vector<TextureHandle> m_Handles;
};

class RGRotatingBuffer
{
  public:
    RGRotatingBuffer() = default;
    ~RGRotatingBuffer() { assert(m_Handles.empty() && "RGRotatingBuffer: call Destroy() before destruction"); }
    RGRotatingBuffer(const RGRotatingBuffer&) = delete;
    RGRotatingBuffer& operator=(const RGRotatingBuffer&) = delete;

    void Init(IDevice* device, const BufferDesc& desc, uint32_t count)
    {
        assert(m_Handles.empty());
        m_Handles.reserve(count);
        for (uint32_t i = 0; i < count; ++i)
            m_Handles.push_back(device->CreateBuffer(desc));
    }
    void Destroy(IDevice* device)
    {
        for (BufferHandle h : m_Handles)
            device->DestroyBuffer(h);
        m_Handles.clear();
    }
    uint32_t Count() const { return static_cast<uint32_t>(m_Handles.size()); }
    BufferHandle Current(uint64_t frameIndex) const
    {
        assert(!m_Handles.empty() && "RGRotatingBuffer: Current() before Init()"); // modulo-by-zero UB otherwise
        return m_Handles[frameIndex % m_Handles.size()];
    }
    BufferHandle Previous(uint64_t frameIndex) const
    {
        assert(!m_Handles.empty() && "RGRotatingBuffer: Previous() before Init()");
        const uint64_t n = m_Handles.size();
        return m_Handles[(frameIndex + n - 1) % n];
    }

  private:
    std::vector<BufferHandle> m_Handles;
};

} // namespace GameEngine::Rendering::RenderGraph
