#pragma once

#include "Types/Types.h"

#include <atomic>
#include <mutex>
#include <vector>

namespace GameEngine::TerrainECS
{

/// Lets a bake report each distinct problem once between two Reset calls, however
/// many terrains, tiles and concurrent row bands meet it. First(key) is true for
/// the first call with `key` since the last Reset. Callable from any thread; a
/// thread asking again for the key it asked last returns at once, without the lock,
/// so a per-texel call costs nothing after its first answer.
class BakeWarningLatch
{
public:
    BakeWarningLatch();

    bool First(uint64 key);
    void Reset();

private:
    std::mutex m_Mutex;
    std::vector<uint64> m_Keys;
    // Unique across every latch and every Reset, so a thread's remembered answer
    // can never be mistaken for one from another latch or an earlier update.
    std::atomic<uint64> m_Generation;
};

} // namespace GameEngine::TerrainECS
