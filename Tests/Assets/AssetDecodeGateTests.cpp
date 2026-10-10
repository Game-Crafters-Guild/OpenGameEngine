// AssetDecodeGate: how many pool workers asset work may occupy, and how many of
// those texture work may take, for every pool size and the diagnostic override.

#include <gtest/gtest.h>

#include "Assets/AssetDecodeGate.h"

using namespace GameEngine;

namespace
{

using Work = AssetDecodeGate::Work;

struct GateSize
{
    size_t PoolWorkers;
    size_t MaxWorkersOverride;
    size_t MaxWorkers;
    size_t MaxTextureWorkers;
};

// Two workers stay free for the frame, and two of the gate's slots stay free of
// texture work; a gate of two or three slots keeps one, a gate of one shares it.
TEST(AssetDecodeGate, LeavesTwoWorkersToTheFrameAndTwoSlotsToOtherDecodes)
{
    const GateSize sizes[] = {
        {16, 0, 14, 12}, {8, 0, 6, 4}, {5, 0, 3, 1}, {4, 0, 2, 1}, {3, 0, 1, 1}, {1, 0, 1, 1}, {0, 0, 1, 1},
        {16, 5, 5, 3},   {16, 3, 3, 1}, {16, 2, 2, 1}, {16, 1, 1, 1},
    };
    for (const GateSize& size : sizes)
    {
        const AssetDecodeGate gate(size.PoolWorkers, size.MaxWorkersOverride, nullptr);
        SCOPED_TRACE(testing::Message() << size.PoolWorkers << " workers, override " << size.MaxWorkersOverride);
        EXPECT_EQ(gate.MaxWorkers(), size.MaxWorkers);
        EXPECT_EQ(gate.MaxTextureWorkers(), size.MaxTextureWorkers);
    }
}

// Texture work stops at its share while other decodes still find slots, and
// nothing passes the gate's size.
TEST(AssetDecodeGate, TextureWorkStopsAtItsShare)
{
    AssetDecodeGate gate(8, 0, nullptr);
    ASSERT_EQ(gate.MaxWorkers(), 6u);
    ASSERT_EQ(gate.MaxTextureWorkers(), 4u);

    for (int i = 0; i < 4; ++i)
        EXPECT_TRUE(gate.TryAcquire(Work::Texture)) << "texture slot " << i;
    EXPECT_FALSE(gate.TryAcquire(Work::Texture));
    EXPECT_EQ(gate.FreeTextureSlots(), 0u);

    EXPECT_TRUE(gate.TryAcquire(Work::Other));
    EXPECT_TRUE(gate.TryAcquire(Work::Other));
    EXPECT_FALSE(gate.TryAcquire(Work::Other));

    // A released texture slot is open to either class; once another decode takes
    // it, texture work has no slot although its share is not full.
    gate.Release(Work::Texture);
    EXPECT_EQ(gate.FreeTextureSlots(), 1u);
    EXPECT_TRUE(gate.TryAcquire(Work::Other));
    EXPECT_EQ(gate.FreeTextureSlots(), 0u);
    EXPECT_FALSE(gate.TryAcquire(Work::Texture));
}

// Other decodes may take every slot; texture work then waits for one of them.
TEST(AssetDecodeGate, OtherDecodesMayFillTheGate)
{
    AssetDecodeGate gate(8, 0, nullptr);
    for (int i = 0; i < 6; ++i)
        EXPECT_TRUE(gate.TryAcquire(Work::Other)) << "slot " << i;
    EXPECT_FALSE(gate.TryAcquire(Work::Texture));
    EXPECT_EQ(gate.FreeTextureSlots(), 0u);
    gate.Release(Work::Other);
    EXPECT_EQ(gate.FreeTextureSlots(), 1u);
    EXPECT_TRUE(gate.TryAcquire(Work::Texture));
}

// Every release reaches the hook the decode readers wait on.
TEST(AssetDecodeGate, EveryReleaseCallsTheHook)
{
    int released = 0;
    AssetDecodeGate gate(8, 0, [&released] { ++released; });
    ASSERT_TRUE(gate.TryAcquire(Work::Texture));
    ASSERT_TRUE(gate.TryAcquire(Work::Other));
    EXPECT_EQ(released, 0);
    gate.Release(Work::Texture);
    gate.Release(Work::Other);
    EXPECT_EQ(released, 2);
}

} // namespace
