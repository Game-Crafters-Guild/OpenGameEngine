// Site attribution: an allocation belongs to the calling thread's innermost tag only.

#include "Memory/AllocationSiteTag.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstring>
#include <new>
#include <thread>

namespace
{
using GameEngine::Memory::AllocationSiteTag;

constexpr std::size_t kProbeBytes = 64;

void AllocateOnce()
{
    ::operator delete(::operator new(kProbeBytes));
}
} // namespace

TEST(AllocationSiteTagTests, AnAllocationCountsForTheInnermostTagOnly)
{
    AllocationSiteTag outer("Outer");
    AllocateOnce();
    {
        AllocationSiteTag inner("Inner");
        AllocateOnce();
        AllocateOnce();
        EXPECT_EQ(inner.Count(), 2u);
        EXPECT_STREQ(inner.Name(), "Inner");
    }
    AllocateOnce();

    EXPECT_EQ(outer.Count(), 2u) << "the outer tag counted allocations made while an inner tag was open";
    EXPECT_STREQ(outer.Name(), "Outer");
}

// The other thread is started before the tag opens, because starting a thread allocates on the
// starting thread.
TEST(AllocationSiteTagTests, AnotherThreadsAllocationsAreNotAttributed)
{
    std::atomic<bool> go{false};
    std::thread other(
        [&go]
        {
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();
            AllocateOnce();
        });
    AllocationSiteTag site("ThisThreadOnly");
    go.store(true, std::memory_order_release);
    other.join();

    EXPECT_EQ(site.Count(), 0u);
}
