#pragma once

// Bounded pool of single-frame transient resources, over the engine's real
// IDevice. Acquired during compile (realization), released at frame end, reused
// next frame when a free entry of matching desc exists. Frame-age eviction + an
// optional byte budget keep VRAM bounded — a stable-shape graph reaches a
// steady-state pool size with zero new allocations after warmup (the anti-goal
// is the old transient pool's unbounded growth: 273MB -> 700MB).

#include "Rendering/Core/Device.h"

#include <cstdint>
#include <vector>

namespace GameEngine::Rendering::RenderGraph
{

class RGTransientPool
{
  public:
    explicit RGTransientPool(IDevice* device) : m_Device(device) {}
    ~RGTransientPool();
    RGTransientPool(const RGTransientPool&) = delete;
    RGTransientPool& operator=(const RGTransientPool&) = delete;

    // Acquire a transient for THIS frame: reuse a free matching entry, else create.
    TextureHandle AcquireTexture(const TextureDesc& desc, uint64_t frameIndex);
    BufferHandle AcquireBuffer(const BufferDesc& desc, uint64_t frameIndex);

    // Mark every entry acquired so far as free again (call at frame end). Callers
    // honoring frames-in-flight should only release once the frame's GPU work is
    // known complete; reuse safety is the caller's contract. While the device
    // fills new resources with NaN, every entry is destroyed here instead.
    void ReleaseFrame(uint64_t frameIndex);

    // Destroy free entries idle for more than maxIdleFrames, then (if a budget is
    // set) destroy least-recently-used free entries until under budget.
    void EvictIdle(uint64_t frameIndex, uint64_t maxIdleFrames);

    // Q6 slice 4 (§8-completion): an in-place device rebuild freed every pooled
    // transient texture/buffer, but the reuse cache still holds their handles — so on
    // a desc MATCH AcquireTexture reuses the DEAD handle instead of recreating (the
    // world pass's transient MSAA color target came back dead, so its queried sample
    // count no longer matched the recompiled pipeline -> the rasterizationSamples
    // assert on the first resumed frame). Forget every cached entry so the next
    // Acquire recreates it fresh. (Not calling DestroyTexture/Buffer is not to avoid a
    // double-free — a stale Destroy* is a generational no-op — the physical is already
    // gone; what matters is never handing back the stale handle.)
    void DropAllAfterDeviceRebuild()
    {
        m_Textures.clear();
        m_Buffers.clear();
    }

    // 0 == unlimited.
    void SetBudgetBytes(uint64_t textureBudget, uint64_t bufferBudget);

    size_t TexturePoolSize() const { return m_Textures.size(); }
    size_t BufferPoolSize() const { return m_Buffers.size(); }
    uint64_t TextureBytesInPool() const;
    uint64_t BufferBytesInPool() const;
    uint64_t Allocs() const { return m_Allocs; }
    uint64_t Hits() const { return m_Hits; }

    // Introspection (VRAM panel / MCP resource listing): one row per entry.
    struct EntryInfo
    {
        bool IsTexture;
        uint64_t Bytes;
        uint64_t LastUsedFrame;
        bool InUse;
    };
    template <class Fn>
    void ForEachEntry(Fn&& fn) const
    {
        for (const TexEntry& e : m_Textures)
            fn(EntryInfo{true, e.Bytes, e.LastUsedFrame, e.InUse});
        for (const BufEntry& e : m_Buffers)
            fn(EntryInfo{false, e.Bytes, e.LastUsedFrame, e.InUse});
    }

  private:
    struct TexEntry
    {
        TextureHandle Handle;
        TextureDesc Desc;
        uint64_t Bytes;
        uint64_t LastUsedFrame;
        bool InUse;
    };
    struct BufEntry
    {
        BufferHandle Handle;
        BufferDesc Desc;
        uint64_t Bytes;
        uint64_t LastUsedFrame;
        bool InUse;
    };

    IDevice* m_Device = nullptr;
    std::vector<TexEntry> m_Textures;
    std::vector<BufEntry> m_Buffers;
    uint64_t m_TextureBudget = 0;
    uint64_t m_BufferBudget = 0;
    uint64_t m_Allocs = 0;
    uint64_t m_Hits = 0;
};

} // namespace GameEngine::Rendering::RenderGraph
