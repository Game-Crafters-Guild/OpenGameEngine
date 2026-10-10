#include "UI/SlotAllocator.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <random>
#include <unordered_set>
#include <vector>

using GameEngine::UI::SlotAllocator;

// --- Size class boundary checks ----------------------------------------------

TEST(SlotAllocator, AllocateGrantsCorrectSizeClasses)
{
    struct Case
    {
        uint16_t count;
        uint16_t expectedCap;
    };
    const Case cases[] = {
        {1, 1},   {2, 2},   {3, 4},   {4, 4},   {5, 8},   {8, 8},
        {9, 16},  {16, 16}, {17, 32}, {32, 32}, {33, 64}, {64, 64},
        {65, 65}, {100, 100}, {200, 200},
    };

    for (const auto& c : cases)
    {
        SlotAllocator a;
        uint16_t cap = 0;
        const uint32_t start = a.Allocate(c.count, cap);
        EXPECT_EQ(cap, c.expectedCap) << "count=" << c.count;
        EXPECT_EQ(start, 0u);
        EXPECT_EQ(a.GetTotalSlots(), c.expectedCap);
        EXPECT_EQ(a.GetUsedSlots(), c.expectedCap);
        EXPECT_FLOAT_EQ(a.GetFragmentationRatio(), 0.0f);
    }
}

// --- Roundtrip allocate/free -------------------------------------------------

TEST(SlotAllocator, FreeReturnsSlotsToFreeList)
{
    SlotAllocator a;
    uint16_t cap = 0;
    const uint32_t s0 = a.Allocate(5, cap);  // cap=8
    EXPECT_EQ(cap, 8);
    EXPECT_EQ(a.GetUsedSlots(), 8u);

    a.Free(s0, cap);
    EXPECT_EQ(a.GetUsedSlots(), 0u);
    EXPECT_EQ(a.GetTotalSlots(), 8u);  // high-water unchanged
    EXPECT_FLOAT_EQ(a.GetFragmentationRatio(), 1.0f);  // all 8 are free, all are holes
}

TEST(SlotAllocator, AllocateAfterFreeReusesSlots)
{
    SlotAllocator a;
    uint16_t cap0 = 0;
    const uint32_t s0 = a.Allocate(5, cap0);
    a.Free(s0, cap0);

    uint16_t cap1 = 0;
    const uint32_t s1 = a.Allocate(5, cap1);
    EXPECT_EQ(s1, s0);
    EXPECT_EQ(cap1, cap0);
    EXPECT_EQ(a.GetTotalSlots(), 8u);  // no extension
}

TEST(SlotAllocator, AllocateWalksUpToLargerClassWhenMatchingEmpty)
{
    SlotAllocator a;
    uint16_t cap16 = 0;
    const uint32_t s16 = a.Allocate(10, cap16);  // class 16
    EXPECT_EQ(cap16, 16);
    a.Free(s16, cap16);

    // Request a smaller class (4); the size-4 list is empty but size-16
    // bucket has a freed entry. Walk-up should find it.
    uint16_t cap = 0;
    const uint32_t s = a.Allocate(3, cap);
    EXPECT_EQ(s, s16);
    EXPECT_EQ(cap, 16);  // got the larger bucket's cap
    EXPECT_EQ(a.GetTotalSlots(), 16u);
}

TEST(SlotAllocator, AllocateExtendsWhenAllSmallClassesEmpty)
{
    SlotAllocator a;
    uint16_t cap0 = 0, cap1 = 0;
    const uint32_t s0 = a.Allocate(1, cap0);
    const uint32_t s1 = a.Allocate(1, cap1);
    EXPECT_EQ(s0, 0u);
    EXPECT_EQ(s1, 1u);
    EXPECT_EQ(a.GetTotalSlots(), 2u);
}

// --- Large bucket ------------------------------------------------------------

TEST(SlotAllocator, LargeBucketUsesExactCap)
{
    SlotAllocator a;
    uint16_t cap = 0;
    a.Allocate(100, cap);
    EXPECT_EQ(cap, 100);
    EXPECT_EQ(a.GetTotalSlots(), 100u);
}

TEST(SlotAllocator, LargeBucketFirstFit)
{
    SlotAllocator a;
    uint16_t cap0 = 0, cap1 = 0, cap2 = 0;
    const uint32_t s0 = a.Allocate(100, cap0);  // start=0
    const uint32_t s1 = a.Allocate(150, cap1);  // start=100
    const uint32_t s2 = a.Allocate(80, cap2);   // start=250

    a.Free(s0, cap0);
    a.Free(s1, cap1);

    // Request 90: first-fit picks the cap=100 entry (it walks the list in order).
    uint16_t capA = 0;
    const uint32_t sA = a.Allocate(90, capA);
    EXPECT_TRUE(sA == s0 || sA == s1);
    EXPECT_GE(capA, 90);

    // Request 130: only the cap=150 entry can satisfy.
    uint16_t capB = 0;
    const uint32_t sB = a.Allocate(130, capB);
    EXPECT_GE(capB, 130);
    EXPECT_NE(sB, sA);

    (void)s2;
    (void)cap2;
}

TEST(SlotAllocator, LargeBucketExtendsWhenNoFitFound)
{
    SlotAllocator a;
    uint16_t cap0 = 0;
    const uint32_t s0 = a.Allocate(80, cap0);
    a.Free(s0, cap0);

    // Request 200: no entry in the free list is large enough → extend.
    uint16_t cap1 = 0;
    const uint32_t s1 = a.Allocate(200, cap1);
    EXPECT_EQ(cap1, 200);
    EXPECT_EQ(s1, 80u);  // appended after the 80-slot freed range
    EXPECT_EQ(a.GetTotalSlots(), 280u);
}

// --- TryGrowInPlace ---------------------------------------------------------

TEST(SlotAllocator, TryGrowInPlaceSucceedsAtTail)
{
    SlotAllocator a;
    uint16_t cap = 0;
    const uint32_t s = a.Allocate(3, cap);  // cap=4, total=4
    EXPECT_EQ(cap, 4);
    EXPECT_EQ(a.GetTotalSlots(), 4u);

    const uint16_t newCap = a.TryGrowInPlace(s, cap, 6);
    EXPECT_EQ(newCap, 8);  // size class for 6 is 8
    EXPECT_EQ(a.GetTotalSlots(), 8u);
    EXPECT_EQ(a.GetUsedSlots(), 8u);
}

TEST(SlotAllocator, TryGrowInPlaceFailsWhenNotAtTail)
{
    SlotAllocator a;
    uint16_t cap0 = 0, cap1 = 0;
    const uint32_t s0 = a.Allocate(3, cap0);
    a.Allocate(3, cap1);
    EXPECT_EQ(a.GetTotalSlots(), 8u);

    const uint16_t newCap = a.TryGrowInPlace(s0, cap0, 6);
    EXPECT_EQ(newCap, 0);
    EXPECT_EQ(a.GetTotalSlots(), 8u);
}

TEST(SlotAllocator, TryGrowInPlaceLargeAtTailGrowsExact)
{
    SlotAllocator a;
    uint16_t cap = 0;
    const uint32_t s = a.Allocate(100, cap);
    EXPECT_EQ(cap, 100);

    const uint16_t newCap = a.TryGrowInPlace(s, cap, 150);
    EXPECT_EQ(newCap, 150);
    EXPECT_EQ(a.GetTotalSlots(), 150u);
}

// --- Fragmentation ratio ----------------------------------------------------

TEST(SlotAllocator, FragmentationRatioReflectsHoles)
{
    SlotAllocator a;
    uint16_t cap = 0;
    a.Allocate(8, cap);   // s=0..7
    const uint32_t s1 = a.Allocate(8, cap);  // s=8..15
    a.Allocate(8, cap);   // s=16..23

    a.Free(s1, cap);
    EXPECT_EQ(a.GetTotalSlots(), 24u);
    EXPECT_EQ(a.GetUsedSlots(), 16u);
    // 8 free out of 24 total = 1/3
    EXPECT_NEAR(a.GetFragmentationRatio(), 8.0f / 24.0f, 1e-5f);
}

// --- Stress: random workload, no leaks --------------------------------------

TEST(SlotAllocator, StressRandomAllocFreeRoundtrip)
{
    SlotAllocator a;
    std::mt19937 rng(42);

    struct Live
    {
        uint32_t start;
        uint16_t cap;
    };
    std::vector<Live> live;
    live.reserve(1024);

    for (int iter = 0; iter < 10000; ++iter)
    {
        const bool doAlloc = live.size() < 512 && (rng() % 3 != 0);
        if (doAlloc)
        {
            // Match the editor distribution from the plan.
            uint16_t count;
            const uint32_t r = rng() % 100;
            if (r < 60)
                count = 1;
            else if (r < 85)
                count = 5 + (rng() % 26);
            else
                count = 30 + (rng() % 171);

            uint16_t cap = 0;
            const uint32_t start = a.Allocate(count, cap);
            EXPECT_GE(cap, count);
            live.push_back({start, cap});
        }
        else if (!live.empty())
        {
            const size_t idx = rng() % live.size();
            a.Free(live[idx].start, live[idx].cap);
            live[idx] = live.back();
            live.pop_back();
        }
    }

    // Final invariant: used == sum of live caps; total >= used + free.
    size_t expectedUsed = 0;
    for (const auto& l : live)
        expectedUsed += l.cap;
    EXPECT_EQ(a.GetUsedSlots(), expectedUsed);

    // Free everything, used should hit zero.
    for (const auto& l : live)
        a.Free(l.start, l.cap);
    EXPECT_EQ(a.GetUsedSlots(), 0u);
}

// --- Non-overlap invariant ---------------------------------------------------

TEST(SlotAllocator, AllocationsDoNotOverlap)
{
    SlotAllocator a;
    std::mt19937 rng(123);

    struct Live
    {
        uint32_t start;
        uint16_t cap;
    };
    std::vector<Live> live;

    // Sized walk: keep ~200 ranges live, churning with allocs and frees.
    for (int iter = 0; iter < 5000; ++iter)
    {
        const bool doAlloc = live.size() < 200 && (rng() % 4 != 0);
        if (doAlloc)
        {
            const uint16_t count = static_cast<uint16_t>(1 + (rng() % 80));
            uint16_t cap = 0;
            const uint32_t start = a.Allocate(count, cap);
            live.push_back({start, cap});

            // Verify no overlap with any other live range.
            for (size_t i = 0; i + 1 < live.size(); ++i)
            {
                const auto& other = live[i];
                const bool overlap = (start < other.start + other.cap) &&
                                      (other.start < start + cap);
                EXPECT_FALSE(overlap)
                    << "range [" << start << "," << start + cap << ") overlaps ["
                    << other.start << "," << other.start + other.cap << ")";
            }
        }
        else if (!live.empty())
        {
            const size_t idx = rng() % live.size();
            a.Free(live[idx].start, live[idx].cap);
            live[idx] = live.back();
            live.pop_back();
        }
    }
}
