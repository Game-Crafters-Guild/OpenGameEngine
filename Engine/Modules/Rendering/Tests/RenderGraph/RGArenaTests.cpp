// Stage 1.1 isolation tests for the RenderGraph per-frame bump allocator.
// Pure, headless, no device — validates the foundational allocation invariants
// the immediate-mode render graph relies on.

#include "Rendering/Core/RenderGraph/RGArena.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <set>

using GameEngine::Rendering::RenderGraph::RGArena;

namespace
{

bool IsAligned(const void* p, size_t a)
{
    return (reinterpret_cast<uintptr_t>(p) & (a - 1)) == 0;
}

struct Tracked
{
    static int s_Live;
    static int s_Destroyed;
    int Value = 0;
    explicit Tracked(int v) : Value(v) { ++s_Live; }
    ~Tracked()
    {
        --s_Live;
        ++s_Destroyed;
    }
};
int Tracked::s_Live = 0;
int Tracked::s_Destroyed = 0;

} // namespace

TEST(RGArena, SingleAllocStoresValueAndAligns)
{
    RGArena arena;
    int* a = arena.Alloc<int>(42);
    double* b = arena.Alloc<double>(3.5);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(*a, 42);
    EXPECT_DOUBLE_EQ(*b, 3.5);
    EXPECT_TRUE(IsAligned(a, alignof(int)));
    EXPECT_TRUE(IsAligned(b, alignof(double)));
    EXPECT_NE(static_cast<void*>(a), static_cast<void*>(b));
}

TEST(RGArena, OverAlignedAllocationRespectsAlignment)
{
    RGArena arena;
    arena.Alloc<char>('x'); // perturb offset so the next alloc must realign
    void* p64 = arena.AllocRaw(8, 64);
    void* p128 = arena.AllocRaw(8, 128);
    EXPECT_TRUE(IsAligned(p64, 64));
    EXPECT_TRUE(IsAligned(p128, 128));
}

TEST(RGArena, AllocArrayDefaultInitsAndIsContiguous)
{
    RGArena arena;
    auto span = arena.AllocArray<uint32_t>(16);
    ASSERT_EQ(span.size(), 16u);
    for (uint32_t v : span)
        EXPECT_EQ(v, 0u); // default-initialized
    for (size_t i = 0; i < span.size(); ++i)
        span[i] = static_cast<uint32_t>(i * 7);
    // Contiguous storage.
    EXPECT_EQ(&span[15] - &span[0], 15);
    EXPECT_EQ(span[15], 15u * 7u);
}

TEST(RGArena, ZeroCountArrayIsEmptyAndSafe)
{
    RGArena arena;
    auto span = arena.AllocArray<int>(0);
    EXPECT_TRUE(span.empty());
}

TEST(RGArena, CopyArrayPreservesValues)
{
    RGArena arena;
    const int src[] = {1, 2, 3, 4, 5};
    auto dst = arena.CopyArray<int>(src, 5);
    ASSERT_EQ(dst.size(), 5u);
    for (size_t i = 0; i < 5; ++i)
        EXPECT_EQ(dst[i], src[i]);
}

TEST(RGArena, GrowthChainsBlocksWithoutInvalidatingPointers)
{
    // Tiny first block forces multiple block allocations; previously-returned
    // pointers must remain valid and readable (block chaining, not realloc).
    RGArena arena(256);
    std::vector<int*> ptrs;
    for (int i = 0; i < 2000; ++i)
        ptrs.push_back(arena.Alloc<int>(i));
    EXPECT_GT(arena.BlockCount(), 1u);
    for (int i = 0; i < 2000; ++i)
        ASSERT_EQ(*ptrs[static_cast<size_t>(i)], i) << "pointer invalidated at " << i;
    // All allocations are distinct addresses.
    std::set<int*> unique(ptrs.begin(), ptrs.end());
    EXPECT_EQ(unique.size(), ptrs.size());
}

TEST(RGArena, ResetRewindsAndRetainsCapacity)
{
    RGArena arena(256);
    for (int i = 0; i < 2000; ++i)
        arena.Alloc<int>(i);
    const size_t blocksAfterFill = arena.BlockCount();
    const size_t reservedAfterFill = arena.BytesReserved();
    EXPECT_GT(arena.BytesUsed(), 0u);

    arena.Reset();
    EXPECT_EQ(arena.BytesUsed(), 0u);
    EXPECT_EQ(arena.BlockCount(), blocksAfterFill);      // blocks retained
    EXPECT_EQ(arena.BytesReserved(), reservedAfterFill); // capacity retained

    // Second fill of the same shape must not allocate new blocks (zero-alloc steady state).
    for (int i = 0; i < 2000; ++i)
        arena.Alloc<int>(i);
    EXPECT_EQ(arena.BlockCount(), blocksAfterFill);
}

TEST(RGArena, NewRunsDestructorsOnReset)
{
    Tracked::s_Live = 0;
    Tracked::s_Destroyed = 0;
    {
        RGArena arena;
        for (int i = 0; i < 10; ++i)
            arena.New<Tracked>(i);
        EXPECT_EQ(Tracked::s_Live, 10);
        EXPECT_EQ(arena.PendingDestructors(), 10u);

        arena.Reset();
        EXPECT_EQ(Tracked::s_Live, 0);
        EXPECT_EQ(Tracked::s_Destroyed, 10);
        EXPECT_EQ(arena.PendingDestructors(), 0u);
    }
    // Arena destruction after Reset must not double-destroy.
    EXPECT_EQ(Tracked::s_Destroyed, 10);
}

TEST(RGArena, DestructorsRunOnArenaDestruction)
{
    Tracked::s_Live = 0;
    Tracked::s_Destroyed = 0;
    {
        RGArena arena;
        arena.New<Tracked>(1);
        arena.New<Tracked>(2);
        EXPECT_EQ(Tracked::s_Live, 2);
    } // ~RGArena runs the recorded destructors
    EXPECT_EQ(Tracked::s_Live, 0);
    EXPECT_EQ(Tracked::s_Destroyed, 2);
}
