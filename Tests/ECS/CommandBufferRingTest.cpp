// CommandBufferRingTest.cpp - SPSC command ring semantics: FIFO order across
// wrap, full/empty boundary conditions, reuse after drain. Pins the ring
// contract so index-arithmetic changes (e.g. modulo -> pow2 mask) are gated.
#include <gtest/gtest.h>
#include "ECS/Entity.h"

using namespace GameEngine::ECS;

namespace {

Command MakeDestroy(uint32_t seq) {
    Command cmd;
    cmd.type = Command::DESTROY_ENTITY;
    cmd.entity = EntityHandle(seq, 1);
    return cmd;
}

} // namespace

TEST(CommandBufferRing, FifoOrderAcrossWrap) {
    CommandBuffer ring(8);
    uint32_t nextPush = 0;
    uint32_t nextPop = 0;

    // Interleaved bursts of 5 on a ring of 8 cross the wrap boundary every
    // other cycle; 100 cycles sweep every write/read position many times.
    for (int cycle = 0; cycle < 100; ++cycle) {
        for (int i = 0; i < 5; ++i) {
            ASSERT_TRUE(ring.TryPush(MakeDestroy(nextPush++)));
        }
        Command out;
        for (int i = 0; i < 5; ++i) {
            ASSERT_TRUE(ring.TryPop(out));
            ASSERT_EQ(out.entity.index, nextPop++);
            ASSERT_EQ(out.type, Command::DESTROY_ENTITY);
        }
    }

    EXPECT_TRUE(ring.IsEmpty());
    EXPECT_EQ(ring.GetSize(), 0u);
}

TEST(CommandBufferRing, FullAndEmptyBounds) {
    CommandBuffer ring(8);

    // One slot is reserved to distinguish full from empty: usable capacity is
    // capacity - 1.
    uint32_t pushed = 0;
    while (ring.TryPush(MakeDestroy(pushed))) {
        ++pushed;
    }
    EXPECT_EQ(pushed, ring.GetCapacity() - 1);
    EXPECT_EQ(ring.GetSize(), ring.GetCapacity() - 1);

    Command out;
    for (uint32_t i = 0; i < pushed; ++i) {
        ASSERT_TRUE(ring.TryPop(out));
        ASSERT_EQ(out.entity.index, i);
    }
    EXPECT_FALSE(ring.TryPop(out));
    EXPECT_TRUE(ring.IsEmpty());
    EXPECT_EQ(ring.GetSize(), 0u);

    // Reusable after a full drain (positions wrap; they are never reset).
    EXPECT_TRUE(ring.TryPush(MakeDestroy(999)));
    ASSERT_TRUE(ring.TryPop(out));
    EXPECT_EQ(out.entity.index, 999u);
}

TEST(CommandBufferRing, NonPowerOfTwoCapacityRoundsUp) {
    CommandBuffer ring(100);
    EXPECT_EQ(ring.GetCapacity(), 128u);

    // Usable capacity follows the rounded size (one slot stays reserved).
    uint32_t pushed = 0;
    while (ring.TryPush(MakeDestroy(pushed))) {
        ++pushed;
    }
    EXPECT_EQ(pushed, 127u);
}
