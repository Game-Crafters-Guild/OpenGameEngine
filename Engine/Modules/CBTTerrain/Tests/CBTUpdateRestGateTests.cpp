// CBTUpdateRestGate: when the CBT update may be skipped. CPU only; the GPU side (the readback that
// feeds the gate, the skipped frames' buffers) is CBTUpdateNodeTest.AStillViewSettlesAndStopsUpdating.

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

#include "CBTTerrain/CBTUpdateRestGate.h"

using namespace GameEngine::CBTTerrain;

namespace
{
constexpr std::array<uint8_t, 4> kInputsA{1, 2, 3, 4};
constexpr std::array<uint8_t, 4> kInputsB{1, 2, 3, 5};

CBTUpdateActivity Quiet()
{
    return CBTUpdateActivity{};
}

CBTUpdateActivity Splitting()
{
    CBTUpdateActivity activity{};
    activity.SplitServed = 12;
    return activity;
}
} // namespace

// The update is skipped once two readings of updates with the current inputs split and merged
// nothing with the pressure and keep steps unchanged; a reading that splits, or moves a step,
// restarts the count.
TEST(CBTUpdateRestGateTest, RestsAfterQuietReadingsOfTheCurrentInputs)
{
    CBTUpdateRestGate gate;
    EXPECT_FALSE(gate.CanSkip(kInputsA));
    for (uint64_t expected = 0; expected < 6u; ++expected)
        EXPECT_EQ(gate.OnUpdateRecorded(false), expected);

    gate.OnActivityRead(0u, Splitting());
    gate.OnActivityRead(1u, Quiet());
    EXPECT_FALSE(gate.CanSkip(kInputsA)) << "one quiet reading proves nothing about the steps";
    gate.OnActivityRead(2u, Quiet());
    EXPECT_TRUE(gate.CanSkip(kInputsA));

    CBTUpdateActivity pressureMoved{};
    pressureMoved.PressureStep = 1;
    gate.OnActivityRead(3u, pressureMoved);
    EXPECT_FALSE(gate.CanSkip(kInputsA)) << "a moved pressure step is not rest";
    gate.OnActivityRead(4u, pressureMoved);
    gate.OnActivityRead(5u, pressureMoved);
    EXPECT_TRUE(gate.CanSkip(kInputsA));
}

// New inputs, or a forced full re-evaluation, restart the count, and readings of updates recorded
// before the restart are ignored even when they land after it.
TEST(CBTUpdateRestGateTest, NewInputsAndForcedUpdatesRestartTheCount)
{
    CBTUpdateRestGate gate;
    EXPECT_FALSE(gate.CanSkip(kInputsA));
    for (uint64_t i = 0; i < 3u; ++i)
        gate.OnActivityRead(gate.OnUpdateRecorded(false), Quiet());
    ASSERT_TRUE(gate.CanSkip(kInputsA));

    const uint64_t beforeChange = gate.OnUpdateRecorded(false);
    EXPECT_FALSE(gate.CanSkip(kInputsB));
    gate.OnActivityRead(beforeChange, Quiet());
    gate.OnActivityRead(beforeChange, Quiet());
    EXPECT_FALSE(gate.CanSkip(kInputsB)) << "readings from before the input change count";
    for (uint64_t i = 0; i < 3u; ++i)
        gate.OnActivityRead(gate.OnUpdateRecorded(false), Quiet());
    ASSERT_TRUE(gate.CanSkip(kInputsB));

    const uint64_t forced = gate.OnUpdateRecorded(true);
    gate.OnActivityRead(forced, Quiet());
    gate.OnActivityRead(forced, Quiet());
    EXPECT_FALSE(gate.CanSkip(kInputsB)) << "a forced update's own reading counts";
    for (uint64_t i = 0; i < 3u; ++i)
        gate.OnActivityRead(gate.OnUpdateRecorded(false), Quiet());
    EXPECT_TRUE(gate.CanSkip(kInputsB));
}
