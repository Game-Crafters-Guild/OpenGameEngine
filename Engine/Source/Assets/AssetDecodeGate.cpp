#include "Assets/AssetDecodeGate.h"

#include <algorithm>
#include <cassert>

namespace GameEngine {

namespace
{
// Pool workers no asset work takes: the frame's ECS waves run there.
constexpr size_t kWorkersKeptForFrame = 2;
// Gate slots texture work never takes: the decodes the frame waits on run there.
constexpr size_t kSlotsKeptFromTextures = 2;

size_t MaxWorkersFor(size_t poolWorkers, size_t maxWorkersOverride)
{
    if (maxWorkersOverride != 0)
        return maxWorkersOverride;
    return poolWorkers > kWorkersKeptForFrame ? poolWorkers - kWorkersKeptForFrame : 1;
}

// All but two of the gate's slots; a gate of two or three keeps one; a gate of one shares it.
size_t MaxTextureWorkersFor(size_t maxWorkers)
{
    if (maxWorkers > kSlotsKeptFromTextures)
        return maxWorkers - kSlotsKeptFromTextures;
    return std::max<size_t>(maxWorkers - 1, 1);
}
} // namespace

AssetDecodeGate::AssetDecodeGate(size_t poolWorkers, size_t maxWorkersOverride, std::function<void()> slotReleased)
    : m_MaxWorkers(MaxWorkersFor(poolWorkers, maxWorkersOverride))
    , m_MaxTextureWorkers(MaxTextureWorkersFor(m_MaxWorkers))
    , m_SlotReleased(std::move(slotReleased))
{
}

bool AssetDecodeGate::TryAcquire(Work work)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (m_Taken >= m_MaxWorkers)
        return false;
    if (work == Work::Texture)
    {
        if (m_TextureTaken >= m_MaxTextureWorkers)
            return false;
        ++m_TextureTaken;
    }
    ++m_Taken;
    return true;
}

void AssetDecodeGate::Release(Work work)
{
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        assert(m_Taken > 0 && "Release without a slot TryAcquire took");
        assert((work != Work::Texture || m_TextureTaken > 0) && "Release of a texture slot TryAcquire never took");
        --m_Taken;
        if (work == Work::Texture)
            --m_TextureTaken;
    }
    if (m_SlotReleased)
        m_SlotReleased();
}

size_t AssetDecodeGate::FreeTextureSlots() const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    return std::min(m_MaxWorkers - m_Taken, m_MaxTextureWorkers - m_TextureTaken);
}

} // namespace GameEngine
