#include <gtest/gtest.h>

#include "Engine/Rendering/BucketPoolAllocator.h"

using GameEngine::Rendering::BucketPoolAllocator;
using Allocation = BucketPoolAllocator::Allocation;

namespace
{

constexpr uint64_t kCapacity = 1024;
constexpr uint64_t kAlign    = 16;

BucketPoolAllocator MakePool()
{
    BucketPoolAllocator pool;
    pool.Initialize(kCapacity, kAlign);
    return pool;
}

} // namespace

TEST(BucketPoolAllocatorTests, InitialState)
{
    auto pool       = MakePool();
    const auto stats = pool.GetStats();
    EXPECT_EQ(stats.capacity, kCapacity);
    EXPECT_EQ(stats.used, 0u);
    EXPECT_EQ(stats.freeRangeCount, 1u);
    EXPECT_EQ(stats.largestFreeRange, kCapacity);
    EXPECT_EQ(stats.fragRatio, 0.0f);
    EXPECT_TRUE(pool.IsEmpty());
}

TEST(BucketPoolAllocatorTests, AllocateRespectsAlignment)
{
    auto pool = MakePool();
    auto a    = pool.Allocate(10);
    ASSERT_TRUE(a.IsValid());
    EXPECT_EQ(a.offset % kAlign, 0u);
    // Allocation size is rounded up to alignment.
    EXPECT_EQ(a.size, 16u);
    EXPECT_EQ(pool.Used(), 16u);
}

TEST(BucketPoolAllocatorTests, BestFitSelectsTighterRange)
{
    auto pool = MakePool();
    auto a    = pool.Allocate(64);
    auto b    = pool.Allocate(128);
    auto c    = pool.Allocate(64);
    auto d    = pool.Allocate(256);
    ASSERT_TRUE(a.IsValid());
    ASSERT_TRUE(b.IsValid());
    ASSERT_TRUE(c.IsValid());
    ASSERT_TRUE(d.IsValid());

    // Free a and c: two 64-byte free ranges separated by b (128).
    pool.Free(a);
    pool.Free(c);

    // Allocate 64. Best-fit must pick one of the tight 64-byte holes,
    // not chip a 64-byte slice off the larger tail block.
    auto e = pool.Allocate(64);
    ASSERT_TRUE(e.IsValid());
    EXPECT_TRUE(e.offset == a.offset || e.offset == c.offset);
}

TEST(BucketPoolAllocatorTests, FreeCoalescesAdjacent)
{
    auto pool = MakePool();
    auto a    = pool.Allocate(64);
    auto b    = pool.Allocate(64);
    auto c    = pool.Allocate(64);
    ASSERT_TRUE(a.IsValid());
    ASSERT_TRUE(b.IsValid());
    ASSERT_TRUE(c.IsValid());

    pool.Free(a);
    pool.Free(c);
    // Three free ranges: [0..64), [128..192), [192..1024).
    // Note the tail [192..1024) merges with c into [128..1024).
    EXPECT_EQ(pool.GetStats().freeRangeCount, 2u);

    pool.Free(b);
    // After freeing b the whole pool should be one contiguous range again.
    EXPECT_EQ(pool.GetStats().freeRangeCount, 1u);
    EXPECT_EQ(pool.GetStats().largestFreeRange, kCapacity);
    EXPECT_TRUE(pool.IsEmpty());
}

TEST(BucketPoolAllocatorTests, AllocateFailsWhenExhausted)
{
    auto pool = MakePool();
    auto big  = pool.Allocate(kCapacity);
    ASSERT_TRUE(big.IsValid());
    EXPECT_EQ(big.size, kCapacity);

    auto fail = pool.Allocate(16);
    EXPECT_FALSE(fail.IsValid());
}

TEST(BucketPoolAllocatorTests, AllocateFailsOnZero)
{
    auto pool = MakePool();
    auto z    = pool.Allocate(0);
    EXPECT_FALSE(z.IsValid());
    EXPECT_EQ(pool.Used(), 0u);
}

TEST(BucketPoolAllocatorTests, PeakTracking)
{
    auto pool = MakePool();
    auto a    = pool.Allocate(256);
    auto b    = pool.Allocate(256);
    pool.Free(a);
    auto c = pool.Allocate(128);

    const auto stats = pool.GetStats();
    EXPECT_EQ(stats.peak, 512u); // peak reached when both a and b were live
    EXPECT_EQ(stats.used, 256u + 128u);
    (void)b;
    (void)c;
}

TEST(BucketPoolAllocatorTests, DeferredFreeDoesNotImmediatelyReclaim)
{
    auto pool = MakePool();
    auto a    = pool.Allocate(256);
    ASSERT_TRUE(a.IsValid());

    pool.FreeDeferred(a, /*retireAfterFrame=*/5);
    // Memory is still considered "used" until reclaimed -- we have not
    // returned the range to the live free-list yet.
    EXPECT_EQ(pool.Used(), 256u);
    EXPECT_EQ(pool.GetStats().deferredCount, 1u);
}

TEST(BucketPoolAllocatorTests, ReclaimUpToFrameReturnsRangesAtOrBeforeFrame)
{
    auto pool = MakePool();
    auto a    = pool.Allocate(128);
    auto b    = pool.Allocate(128);
    auto c    = pool.Allocate(128);

    pool.FreeDeferred(a, 1);
    pool.FreeDeferred(b, 2);
    pool.FreeDeferred(c, 3);
    EXPECT_EQ(pool.Used(), 128u * 3);

    pool.ReclaimUpToFrame(1);
    EXPECT_EQ(pool.Used(), 128u * 2);
    EXPECT_EQ(pool.GetStats().deferredCount, 2u);

    pool.ReclaimUpToFrame(3);
    EXPECT_TRUE(pool.IsEmpty());
    EXPECT_EQ(pool.GetStats().deferredCount, 0u);
}

TEST(BucketPoolAllocatorTests, FragmentationStat)
{
    auto pool = MakePool();
    auto a    = pool.Allocate(64);
    auto b    = pool.Allocate(64);
    auto c    = pool.Allocate(64);
    auto d    = pool.Allocate(64);
    auto e    = pool.Allocate(64);
    ASSERT_TRUE(a.IsValid());
    ASSERT_TRUE(b.IsValid());
    ASSERT_TRUE(c.IsValid());
    ASSERT_TRUE(d.IsValid());
    ASSERT_TRUE(e.IsValid());

    // Free every other allocation to create a fragmented pattern.
    pool.Free(a);
    pool.Free(c);
    pool.Free(e);
    const auto stats = pool.GetStats();
    EXPECT_GT(stats.freeRangeCount, 1u);
    EXPECT_GT(stats.fragRatio, 0.0f);
    EXPECT_LT(stats.fragRatio, 1.0f);
}

TEST(BucketPoolAllocatorTests, PowerOfTwoAlignmentRounding)
{
    BucketPoolAllocator pool;
    pool.Initialize(1024, /*alignment=*/12); // not pow2; should round up to 16
    EXPECT_EQ(pool.Alignment(), 16u);
}

TEST(BucketPoolAllocatorTests, DeferredFreeThenReclaimDoesNotDoubleAccount)
{
    auto pool = MakePool();
    auto a    = pool.Allocate(256);
    pool.FreeDeferred(a, 1);
    pool.ReclaimUpToFrame(1);
    // Reclaiming should restore exactly the freed range -- pool is empty
    // and the full capacity is available again.
    EXPECT_TRUE(pool.IsEmpty());
    auto big = pool.Allocate(kCapacity);
    EXPECT_TRUE(big.IsValid());
}

TEST(BucketPoolAllocatorTests, AllocAfterReclaimReusesRange)
{
    auto pool = MakePool();
    auto a    = pool.Allocate(256);
    const uint64_t aOffset = a.offset;
    pool.FreeDeferred(a, 1);
    pool.ReclaimUpToFrame(1);
    auto b = pool.Allocate(256);
    ASSERT_TRUE(b.IsValid());
    EXPECT_EQ(b.offset, aOffset);
}
