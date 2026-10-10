#include "Rendering/Core/RenderGraph/RGTransientPool.h"

#include "Rendering/Core/RenderGraph/RGRealize.h"

#include <algorithm>

namespace GameEngine::Rendering::RenderGraph
{

RGTransientPool::~RGTransientPool()
{
    for (const TexEntry& e : m_Textures)
        m_Device->DestroyTexture(e.Handle);
    for (const BufEntry& e : m_Buffers)
        m_Device->DestroyBuffer(e.Handle);
}

TextureHandle RGTransientPool::AcquireTexture(const TextureDesc& desc, uint64_t frameIndex)
{
    for (TexEntry& e : m_Textures)
    {
        if (!e.InUse && DescEqual(e.Desc, desc))
        {
            e.InUse = true;
            e.LastUsedFrame = frameIndex;
            ++m_Hits;
            return e.Handle;
        }
    }
    const TextureHandle h = m_Device->CreateTexture(desc);
    m_Textures.push_back(TexEntry{h, desc, EstimateBytes(desc), frameIndex, true});
    ++m_Allocs;
    return h;
}

BufferHandle RGTransientPool::AcquireBuffer(const BufferDesc& desc, uint64_t frameIndex)
{
    for (BufEntry& e : m_Buffers)
    {
        if (!e.InUse && DescEqual(e.Desc, desc))
        {
            e.InUse = true;
            e.LastUsedFrame = frameIndex;
            ++m_Hits;
            return e.Handle;
        }
    }
    const BufferHandle h = m_Device->CreateBuffer(desc);
    m_Buffers.push_back(BufEntry{h, desc, EstimateBytes(desc), frameIndex, true});
    ++m_Allocs;
    return h;
}

void RGTransientPool::ReleaseFrame(uint64_t /*frameIndex*/)
{
    // Under the device's NaN fill nothing is recycled: every acquire creates,
    // so a pass reading a transient before writing it reads the fill instead
    // of the previous frame's contents. Destroys are deferred past the frames
    // in flight by the device.
    if (m_Device->DebugFillsNewResourcesWithNaN())
    {
        for (const TexEntry& e : m_Textures)
            m_Device->DestroyTexture(e.Handle);
        for (const BufEntry& e : m_Buffers)
            m_Device->DestroyBuffer(e.Handle);
        m_Textures.clear();
        m_Buffers.clear();
        return;
    }
    for (TexEntry& e : m_Textures)
        e.InUse = false;
    for (BufEntry& e : m_Buffers)
        e.InUse = false;
}

void RGTransientPool::SetBudgetBytes(uint64_t textureBudget, uint64_t bufferBudget)
{
    m_TextureBudget = textureBudget;
    m_BufferBudget = bufferBudget;
}

uint64_t RGTransientPool::TextureBytesInPool() const
{
    uint64_t total = 0;
    for (const TexEntry& e : m_Textures)
        total += e.Bytes;
    return total;
}

uint64_t RGTransientPool::BufferBytesInPool() const
{
    uint64_t total = 0;
    for (const BufEntry& e : m_Buffers)
        total += e.Bytes;
    return total;
}

namespace
{
// Erase free entries idle beyond maxIdleFrames, then (if budget>0) evict the
// least-recently-used free entries until total bytes <= budget. In-use entries
// are never evicted. Destroy is routed through `destroy`.
template <class Entry, class DestroyFn>
void EvictVector(std::vector<Entry>& entries, uint64_t frameIndex, uint64_t maxIdleFrames,
                 uint64_t budget, DestroyFn destroy)
{
    auto idle = [&](const Entry& e)
    { return !e.InUse && (frameIndex - e.LastUsedFrame) > maxIdleFrames; };
    for (Entry& e : entries)
        if (idle(e))
            destroy(e.Handle);
    entries.erase(std::remove_if(entries.begin(), entries.end(), idle), entries.end());

    if (budget == 0)
        return;

    uint64_t total = 0;
    for (const Entry& e : entries)
        total += e.Bytes;
    if (total <= budget)
        return;

    std::vector<size_t> freeIdx;
    for (size_t i = 0; i < entries.size(); ++i)
        if (!entries[i].InUse)
            freeIdx.push_back(i);
    std::sort(freeIdx.begin(), freeIdx.end(),
              [&](size_t a, size_t b) { return entries[a].LastUsedFrame < entries[b].LastUsedFrame; });

    std::vector<uint8_t> drop(entries.size(), 0);
    for (size_t i : freeIdx)
    {
        if (total <= budget)
            break;
        drop[i] = 1;
        total -= entries[i].Bytes;
        destroy(entries[i].Handle);
    }
    size_t w = 0;
    for (size_t i = 0; i < entries.size(); ++i)
        if (!drop[i])
            entries[w++] = entries[i];
    entries.resize(w);
}
} // namespace

void RGTransientPool::EvictIdle(uint64_t frameIndex, uint64_t maxIdleFrames)
{
    EvictVector(m_Textures, frameIndex, maxIdleFrames, m_TextureBudget,
                [this](TextureHandle h) { m_Device->DestroyTexture(h); });
    EvictVector(m_Buffers, frameIndex, maxIdleFrames, m_BufferBudget,
                [this](BufferHandle h) { m_Device->DestroyBuffer(h); });
}

} // namespace GameEngine::Rendering::RenderGraph
