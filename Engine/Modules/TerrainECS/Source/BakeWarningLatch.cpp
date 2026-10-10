#include "TerrainECS/BakeWarningLatch.h"

#include <algorithm>
#include <atomic>

namespace GameEngine::TerrainECS
{
namespace
{

std::atomic<uint64> g_NextGeneration{1};

struct LastAnswer
{
    uint64 Generation = 0;
    uint64 Key = 0;
};

thread_local LastAnswer t_LastAnswer;

} // namespace

BakeWarningLatch::BakeWarningLatch()
    : m_Generation(g_NextGeneration.fetch_add(1, std::memory_order_relaxed))
{
}

bool BakeWarningLatch::First(uint64 key)
{
    if (t_LastAnswer.Generation == m_Generation.load(std::memory_order_acquire) && t_LastAnswer.Key == key)
        return false;
    std::lock_guard<std::mutex> lock(m_Mutex);
    t_LastAnswer = {m_Generation.load(std::memory_order_relaxed), key};
    if (std::find(m_Keys.begin(), m_Keys.end(), key) != m_Keys.end())
        return false;
    m_Keys.push_back(key);
    return true;
}

void BakeWarningLatch::Reset()
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Keys.clear();
    m_Generation.store(g_NextGeneration.fetch_add(1, std::memory_order_relaxed), std::memory_order_release);
}

} // namespace GameEngine::TerrainECS
