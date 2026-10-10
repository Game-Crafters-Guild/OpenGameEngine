#include "Engine/Rendering/DDGISolveBudget.h"

#include <gtest/gtest.h>

namespace GameEngine::Engine::Renderer
{
namespace
{

constexpr float kFastFrameMs = 8.0f;   // well under the 17.2 grow threshold
constexpr float kMissFrameMs = 20.0f;  // over the 18.5 shrink threshold
constexpr float kStallFrameMs = 200.0f;  // overload band: [100, 1000)
constexpr float kPauseFrameMs = 5000.0f; // pause band: >= 1000

TEST(DDGISolveBudget, StartsAtCeilingAndHoldsUnderFastCadence)
{
    DDGISolveBudget budget;
    const uint32 ceiling = budget.RaysPerTick();
    for (int i = 0; i < 300; ++i)
        budget.Tick(kFastFrameMs);
    EXPECT_EQ(budget.RaysPerTick(), ceiling);
}

TEST(DDGISolveBudget, CadenceMissHalvesTowardFloorWithCooldown)
{
    DDGISolveBudget budget;
    const uint32 ceiling = budget.RaysPerTick();

    // The EMA needs a few miss frames to cross the threshold, then shrinks.
    for (int i = 0; i < 20 && budget.RaysPerTick() == ceiling; ++i)
        budget.Tick(kMissFrameMs);
    EXPECT_EQ(budget.RaysPerTick(), ceiling / 2);

    // Cooldown: fast frames immediately after a shrink must NOT grow it back
    // (a render-bound scene would otherwise saw-tooth floor <-> ceiling).
    const uint32 afterShrink = budget.RaysPerTick();
    for (int i = 0; i < 60; ++i)
        budget.Tick(kFastFrameMs);
    EXPECT_EQ(budget.RaysPerTick(), afterShrink);

    // After the cooldown expires, sustained headroom grows it again.
    for (int i = 0; i < 120; ++i)
        budget.Tick(kFastFrameMs);
    EXPECT_GT(budget.RaysPerTick(), afterShrink);
}

TEST(DDGISolveBudget, SustainedMissesConvergeOnFloorNeverBelow)
{
    DDGISolveBudget budget;
    for (int i = 0; i < 1000; ++i)
        budget.Tick(kMissFrameMs);
    const uint32 floor = budget.RaysPerTick();
    EXPECT_EQ(floor, 2048u);
    budget.Tick(kMissFrameMs);
    EXPECT_EQ(budget.RaysPerTick(), floor);
}

TEST(DDGISolveBudget, OneStallIsIgnoredTwoShrink)
{
    DDGISolveBudget budget;
    const uint32 ceiling = budget.RaysPerTick();

    // A single overload frame between healthy ones: no shrink.
    budget.Tick(kFastFrameMs);
    budget.Tick(kStallFrameMs);
    budget.Tick(kFastFrameMs);
    EXPECT_EQ(budget.RaysPerTick(), ceiling);

    // Two consecutive overload frames: back off.
    budget.Tick(kStallFrameMs);
    budget.Tick(kStallFrameMs);
    EXPECT_EQ(budget.RaysPerTick(), ceiling / 2);
}

TEST(DDGISolveBudget, PauseIsDiscardedNotPressure)
{
    DDGISolveBudget budget;
    const uint32 ceiling = budget.RaysPerTick();
    // A debugger break / host stall, however long and however repeated,
    // never shrinks the budget and never arms the overload streak.
    for (int i = 0; i < 10; ++i)
        budget.Tick(kPauseFrameMs);
    EXPECT_EQ(budget.RaysPerTick(), ceiling);
    // A pause between two stalls also breaks the streak.
    budget.Tick(kStallFrameMs);
    budget.Tick(kPauseFrameMs);
    budget.Tick(kStallFrameMs);
    EXPECT_EQ(budget.RaysPerTick(), ceiling);
}

TEST(DDGISolveBudget, RestResumeClampsStaleHighBudget)
{
    DDGISolveBudget budget;
    EXPECT_GT(budget.RaysPerTick(), 4096u);
    budget.OnRestResume();
    EXPECT_EQ(budget.RaysPerTick(), 4096u);

    // The clamp is a ceiling, not a floor: a budget already below the resume
    // reserve is left alone.
    for (int i = 0; i < 1000; ++i)
        budget.Tick(kMissFrameMs);
    const uint32 low = budget.RaysPerTick();
    ASSERT_LT(low, 4096u);
    budget.OnRestResume();
    EXPECT_EQ(budget.RaysPerTick(), low);
}

TEST(DDGISolveBudget, GrowsBackAfterRestResumeWhenCadenceAllows)
{
    DDGISolveBudget budget;
    budget.OnRestResume();
    const uint32 clamped = budget.RaysPerTick();
    // Its short cooldown holds first (the cooldown decrements before the
    // grow check, so growth starts on the tick the counter reaches zero),
    // then headroom grows the budget again.
    for (int i = 0; i < 29; ++i)
        budget.Tick(kFastFrameMs);
    EXPECT_EQ(budget.RaysPerTick(), clamped);
    for (int i = 0; i < 200; ++i)
        budget.Tick(kFastFrameMs);
    EXPECT_GT(budget.RaysPerTick(), clamped);
}

TEST(DDGISolveBudget, ConvergedThrottleCapsWithoutDisturbingTheController)
{
    DDGISolveBudget budget;
    const uint32 ceiling = budget.RaysPerTick();
    budget.SetConvergedThrottle(true);
    EXPECT_TRUE(budget.IsConvergedThrottled());
    EXPECT_EQ(budget.RaysPerTick(), 2048u);
    // The controller keeps measuring underneath the cap: releasing the
    // throttle returns the budget it had, not a reset one.
    for (int i = 0; i < 50; ++i)
        budget.Tick(kFastFrameMs);
    budget.SetConvergedThrottle(false);
    EXPECT_EQ(budget.RaysPerTick(), ceiling);

    // A cap, not a floor: the cadence controller's own floor is the same
    // 2048, so a budget driven all the way down reads 2048 with the throttle
    // off and stays exactly there with it on.
    for (int i = 0; i < 1000; ++i)
        budget.Tick(kMissFrameMs);
    const uint32 low = budget.RaysPerTick();
    ASSERT_LE(low, 2048u);
    budget.SetConvergedThrottle(true);
    EXPECT_EQ(budget.RaysPerTick(), low);
}

}  // namespace
}  // namespace GameEngine::Engine::Renderer
