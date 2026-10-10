#include "Memory/AllocationCategory.h"

#include <array>
#include <atomic>

namespace GameEngine
{
namespace Memory
{

namespace
{
// One atomic counter per category. Function-local static to dodge
// static-initialization ordering (the Animation pool can register
// allocations before main() if it ever moves into a global ctor).
std::array<std::atomic<uint64_t>, static_cast<size_t>(AllocationCategory::Count)>&
GetCounters()
{
    static std::array<std::atomic<uint64_t>, static_cast<size_t>(AllocationCategory::Count)>
        counters{};
    return counters;
}
} // namespace

void TrackAllocation(AllocationCategory category, uint64_t bytes)
{
    const size_t idx = static_cast<size_t>(category);
    if (idx >= static_cast<size_t>(AllocationCategory::Count)) return;
    GetCounters()[idx].fetch_add(bytes, std::memory_order_relaxed);
}

void TrackDeallocation(AllocationCategory category, uint64_t bytes)
{
    const size_t idx = static_cast<size_t>(category);
    if (idx >= static_cast<size_t>(AllocationCategory::Count)) return;
    auto& counter = GetCounters()[idx];
    // Saturate at zero — a misreport must not underflow the unsigned counter
    // and turn into a huge fake value the perf gate would then alarm on.
    uint64_t cur = counter.load(std::memory_order_relaxed);
    while (true)
    {
        const uint64_t sub = (bytes > cur) ? cur : bytes;
        if (counter.compare_exchange_weak(cur, cur - sub, std::memory_order_relaxed))
            return;
    }
}

uint64_t GetCategoryBytes(AllocationCategory category)
{
    const size_t idx = static_cast<size_t>(category);
    if (idx >= static_cast<size_t>(AllocationCategory::Count)) return 0;
    return GetCounters()[idx].load(std::memory_order_relaxed);
}

void ResetCategoryBytes(AllocationCategory category)
{
    const size_t idx = static_cast<size_t>(category);
    if (idx >= static_cast<size_t>(AllocationCategory::Count)) return;
    GetCounters()[idx].store(0, std::memory_order_relaxed);
}

} // namespace Memory
} // namespace GameEngine
