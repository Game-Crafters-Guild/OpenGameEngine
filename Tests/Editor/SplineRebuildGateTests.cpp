// The observe, settle and defer state machine every spline recipe controller
// (placement, fence, extrude, wall) rebuilds through. The controllers themselves
// need live RenderServices and are compiled into no test target, so the gate's
// behaviour is pinned here over the real class.

#include "Placement/SplineRebuildGate.h"

#include <gtest/gtest.h>

#include <limits>

using GameEngine::float32;
using GameEngine::uint32;
using GameEngine::Editor::ConformSurfaceReadiness;
using GameEngine::Editor::kSplineRebuildSettleSeconds;
using GameEngine::Editor::SplineObservedInputs;
using GameEngine::Editor::SplineRebuildGate;

namespace
{
struct TestObservation
{
    uint32 Value = 0;

    bool operator==(const TestObservation&) const = default;
};

constexpr uint32 kEntityId = 7u;
constexpr float32 kFrameSeconds = 1.0f / 60.0f;

// A frame for a recipe that reads no ground, with its output alive.
bool Frame(SplineRebuildGate<TestObservation>& gate, uint32 value, float32 deltaSeconds = kFrameSeconds,
           bool outputsAlive = true)
{
    return gate.ShouldRebuild(TestObservation{value}, outputsAlive, ConformSurfaceReadiness::NoSurface,
                              /*readsSurface*/ false, deltaSeconds, kEntityId);
}

// Rebuilds when asked, as a controller does.
bool FrameAndBuild(SplineRebuildGate<TestObservation>& gate, uint32 value, float32 deltaSeconds = kFrameSeconds)
{
    const bool rebuilt = Frame(gate, value, deltaSeconds);
    if (rebuilt)
        gate.MarkBuilt(TestObservation{value});
    return rebuilt;
}

TEST(SplineRebuildGate, FirstBuildRunsWithoutWaitingForTheSettleWindow)
{
    SplineRebuildGate<TestObservation> gate;
    EXPECT_TRUE(FrameAndBuild(gate, 1u));
    EXPECT_TRUE(gate.AppliedOnce());
    EXPECT_EQ(gate.Applied().Value, 1u);
}

TEST(SplineRebuildGate, UnchangedInputsWithLiveOutputNeverRebuild)
{
    SplineRebuildGate<TestObservation> gate;
    ASSERT_TRUE(FrameAndBuild(gate, 1u));
    for (int frame = 0; frame < 120; ++frame)
        EXPECT_FALSE(FrameAndBuild(gate, 1u)) << "frame " << frame;
}

TEST(SplineRebuildGate, AnEditRebuildsOnlyAfterItHasBeenStableForTheSettleWindow)
{
    SplineRebuildGate<TestObservation> gate;
    ASSERT_TRUE(FrameAndBuild(gate, 1u));

    // A drag: the value changes every frame, so the settle clock keeps restarting.
    for (uint32 value = 2u; value < 40u; ++value)
        EXPECT_FALSE(FrameAndBuild(gate, value)) << "rebuilt mid-drag at " << value;

    // The drag stops; the rebuild waits out the settle window, then commits once.
    const float32 halfWindow = kSplineRebuildSettleSeconds * 0.5f;
    EXPECT_FALSE(FrameAndBuild(gate, 39u, halfWindow));
    EXPECT_TRUE(FrameAndBuild(gate, 39u, halfWindow));
    EXPECT_EQ(gate.Applied().Value, 39u);
    EXPECT_FALSE(FrameAndBuild(gate, 39u));
}

TEST(SplineRebuildGate, DeadOutputForcesARebuildEvenWhenInputsAreUnchanged)
{
    SplineRebuildGate<TestObservation> gate;
    ASSERT_TRUE(FrameAndBuild(gate, 1u));
    EXPECT_TRUE(Frame(gate, 1u, kFrameSeconds, /*outputsAlive*/ false));
}

TEST(SplineRebuildGate, AGroundReadingFirstBuildWaitsForProvisionedSettledGround)
{
    SplineRebuildGate<TestObservation> gate;
    const TestObservation observation{1u};

    EXPECT_FALSE(gate.ShouldRebuild(observation, true, ConformSurfaceReadiness::Provisioning,
                                    /*readsSurface*/ true, kFrameSeconds, kEntityId));
    // Ready, but not yet stable for the settle window.
    EXPECT_FALSE(gate.ShouldRebuild(observation, true, ConformSurfaceReadiness::Ready,
                                    /*readsSurface*/ true, kFrameSeconds, kEntityId));
    EXPECT_TRUE(gate.ShouldRebuild(observation, true, ConformSurfaceReadiness::Ready,
                                   /*readsSurface*/ true, kSplineRebuildSettleSeconds, kEntityId));
}

TEST(SplineRebuildGate, MarkUnbuiltReturnsTheRecipeToItsFirstBuild)
{
    SplineRebuildGate<TestObservation> gate;
    ASSERT_TRUE(FrameAndBuild(gate, 1u));
    gate.MarkUnbuilt();
    EXPECT_FALSE(gate.AppliedOnce());
    // Same inputs, but never-built again: the first build skips the settle window.
    EXPECT_TRUE(Frame(gate, 1u));
}

TEST(SplineObservedInputs, ANaNWorldMatrixStillEqualsItself)
{
    SplineObservedInputs inputs;
    inputs.WorldMatrix[5] = std::numeric_limits<float32>::quiet_NaN();
    const SplineObservedInputs copy = inputs;
    EXPECT_TRUE(inputs == copy) << "a NaN would re-arm the settle window every frame";
}
} // namespace
