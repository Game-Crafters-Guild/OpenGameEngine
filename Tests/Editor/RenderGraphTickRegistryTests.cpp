// The registration surface panels reach the per-frame render graph through.
// What matters is the bookkeeping, not what a participant draws: every
// registered participant runs once per drain, an unregistered one never runs
// again, and mutating the set from inside a drain must not walk a vector that
// has moved under the iteration.

#include "Editor/RenderGraphTickRegistry.h"

#include "Rendering/Core/Device.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Core/RenderGraph/RGResourcePool.h"
#include "Rendering/Core/RenderGraph/RGTransientPool.h"
#include "Rendering/Core/RenderGraph/RGUploadRing.h"
#include "UIRgTestHarness.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

using GameEngine::Editor::RenderGraphTickRegistry;

namespace
{

/// A real RGFrame is the only thing that satisfies the tick signature, and it
/// needs a device. The participants here never touch the frame, so the frame is
/// declared and never executed — same contract as the UI harness.
class RenderGraphTickRegistryTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Device = MakeHeadlessDevice();
        if (!m_Device)
            GTEST_SKIP() << "no Vulkan device available";
        m_Harness = std::make_unique<UiRgHarness>(m_Device.get());
    }

    /// Runs one drain against a fresh frame and returns the window id used.
    void Drain(RenderGraphTickRegistry& registry, uint64_t windowId)
    {
        GameEngine::Rendering::RenderGraph::RGFrame frame(
            m_Harness->Dev, &m_Harness->Persistent, &m_Harness->Transient, &m_Harness->Ring);
        frame.BeginFrame(++m_Harness->FrameIndex);
        registry.TickAll(windowId, nullptr, frame);
    }

    std::unique_ptr<GameEngine::Rendering::IDevice> m_Device;
    std::unique_ptr<UiRgHarness> m_Harness;
};

} // namespace

TEST_F(RenderGraphTickRegistryTest, RegisteredParticipantsRunOncePerDrainWithTheWindowId)
{
    RenderGraphTickRegistry registry;
    std::vector<uint64_t> seenA;
    std::vector<uint64_t> seenB;

    registry.Register([&](uint64_t windowId, GameEngine::UIManager*,
                          GameEngine::Rendering::RenderGraph::RGFrame&) { seenA.push_back(windowId); },
                      nullptr);
    registry.Register([&](uint64_t windowId, GameEngine::UIManager*,
                          GameEngine::Rendering::RenderGraph::RGFrame&) { seenB.push_back(windowId); },
                      nullptr);

    Drain(registry, 7);
    Drain(registry, 9);

    EXPECT_EQ(seenA, (std::vector<uint64_t>{7, 9}));
    EXPECT_EQ(seenB, (std::vector<uint64_t>{7, 9}));
}

TEST_F(RenderGraphTickRegistryTest, HandlesAreDistinctAndNullTicksAreRefused)
{
    RenderGraphTickRegistry registry;
    const auto a = registry.Register([](uint64_t, GameEngine::UIManager*,
                                        GameEngine::Rendering::RenderGraph::RGFrame&) {},
                                     nullptr);
    const auto b = registry.Register([](uint64_t, GameEngine::UIManager*,
                                        GameEngine::Rendering::RenderGraph::RGFrame&) {},
                                     nullptr);
    EXPECT_NE(a, RenderGraphTickRegistry::kInvalidHandle);
    EXPECT_NE(b, RenderGraphTickRegistry::kInvalidHandle);
    EXPECT_NE(a, b);

    EXPECT_EQ(registry.Register(nullptr, nullptr), RenderGraphTickRegistry::kInvalidHandle);
}

TEST_F(RenderGraphTickRegistryTest, UnregisteredParticipantsStopRunning)
{
    RenderGraphTickRegistry registry;
    int aRuns = 0;
    int bRuns = 0;
    const auto a = registry.Register(
        [&](uint64_t, GameEngine::UIManager*,
            GameEngine::Rendering::RenderGraph::RGFrame&) { ++aRuns; },
        nullptr);
    registry.Register([&](uint64_t, GameEngine::UIManager*,
                          GameEngine::Rendering::RenderGraph::RGFrame&) { ++bRuns; },
                      nullptr);

    Drain(registry, 1);
    registry.Unregister(a);
    Drain(registry, 1);

    EXPECT_EQ(aRuns, 1);
    EXPECT_EQ(bRuns, 2);

    // A stale handle must not take a live participant's entry with it.
    registry.Unregister(a);
    registry.Unregister(RenderGraphTickRegistry::kInvalidHandle);
    Drain(registry, 1);
    EXPECT_EQ(bRuns, 3);
}

TEST_F(RenderGraphTickRegistryTest, AParticipantMayUnregisterItselfMidDrain)
{
    // The teardown shape that matters: a panel closing inside its own tick. The
    // entry is tombstoned rather than erased, so the drain keeps walking a
    // vector that has not moved.
    RenderGraphTickRegistry registry;
    int selfRuns = 0;
    int otherRuns = 0;
    RenderGraphTickRegistry::Handle self = RenderGraphTickRegistry::kInvalidHandle;
    self = registry.Register([&](uint64_t, GameEngine::UIManager*,
                                 GameEngine::Rendering::RenderGraph::RGFrame&) {
        ++selfRuns;
        registry.Unregister(self);
    },
                             nullptr);
    registry.Register([&](uint64_t, GameEngine::UIManager*,
                          GameEngine::Rendering::RenderGraph::RGFrame&) { ++otherRuns; },
                      nullptr);

    Drain(registry, 1);
    Drain(registry, 1);

    EXPECT_EQ(selfRuns, 1);
    EXPECT_EQ(otherRuns, 2);
}

TEST_F(RenderGraphTickRegistryTest, SelfUnregisterDoesNotDestroyTheRunningCallback)
{
    // AParticipantMayUnregisterItselfMidDrain only captures by reference, so it
    // cannot see the closure's own storage go away. This one gives the callable
    // OWNED state: a shared_ptr held by value inside the std::function. Clearing
    // that function from Unregister would run the closure's destructor while its
    // body is still on the stack, which the use count observes from the test
    // frame (never from the closure — that read would itself be the UAF).
    RenderGraphTickRegistry registry;
    auto owned = std::make_shared<int>(0);
    long countDuringTick = 0;
    bool completedAfterUnregister = false;
    int otherRuns = 0;
    RenderGraphTickRegistry::Handle self = RenderGraphTickRegistry::kInvalidHandle;

    self = registry.Register(
        [&, heldByTheClosure = owned](uint64_t, GameEngine::UIManager*,
                                      GameEngine::Rendering::RenderGraph::RGFrame&) {
            registry.Unregister(self);
            countDuringTick = owned.use_count();
            completedAfterUnregister = true;
        },
        nullptr);
    registry.Register([&](uint64_t, GameEngine::UIManager*,
                          GameEngine::Rendering::RenderGraph::RGFrame&) { ++otherRuns; },
                      nullptr);

    Drain(registry, 1);

    // 2 = the test's own handle plus the copy living in the executing closure.
    // 1 means the closure was destroyed underneath its own body.
    EXPECT_EQ(countDuringTick, 2);
    EXPECT_TRUE(completedAfterUnregister);
    // The rest of the drain still runs: the tombstone must not truncate the walk.
    EXPECT_EQ(otherRuns, 1);

    // Compact owns the destruction, and it happens once the drain has unwound.
    EXPECT_EQ(owned.use_count(), 1);

    Drain(registry, 1);
    EXPECT_EQ(otherRuns, 2);
}

TEST_F(RenderGraphTickRegistryTest, RegisteringMidDrainDefersToTheNextDrain)
{
    // Appending during the walk would reallocate the vector holding the
    // callback currently executing. The new participant must therefore start on
    // the following frame, and exactly once.
    RenderGraphTickRegistry registry;
    int addedRuns = 0;
    bool added = false;
    registry.Register([&](uint64_t, GameEngine::UIManager*,
                          GameEngine::Rendering::RenderGraph::RGFrame&) {
        if (added)
            return;
        added = true;
        registry.Register([&](uint64_t, GameEngine::UIManager*,
                              GameEngine::Rendering::RenderGraph::RGFrame&) { ++addedRuns; },
                          nullptr);
    },
                      nullptr);

    Drain(registry, 1);
    EXPECT_EQ(addedRuns, 0);

    Drain(registry, 1);
    EXPECT_EQ(addedRuns, 1);
}

TEST_F(RenderGraphTickRegistryTest, AnEmptyRegistryDrainsCleanly)
{
    RenderGraphTickRegistry registry;
    Drain(registry, 1);
    SUCCEED();
}

// ReleaseAll is the shutdown-ordering hook: participants own renderer handles
// but are destroyed after the renderer, so the application drops those handles
// through the registry while RenderServices is still alive.

TEST_F(RenderGraphTickRegistryTest, ReleaseAllRunsEveryReleaseAndStopsTicking)
{
    RenderGraphTickRegistry registry;
    int aReleases = 0;
    int bReleases = 0;
    int aRuns = 0;
    int bRuns = 0;
    registry.Register([&](uint64_t, GameEngine::UIManager*,
                          GameEngine::Rendering::RenderGraph::RGFrame&) { ++aRuns; },
                      [&] { ++aReleases; });
    registry.Register([&](uint64_t, GameEngine::UIManager*,
                          GameEngine::Rendering::RenderGraph::RGFrame&) { ++bRuns; },
                      [&] { ++bReleases; });

    Drain(registry, 1);
    registry.ReleaseAll();
    Drain(registry, 1);

    EXPECT_EQ(aReleases, 1);
    EXPECT_EQ(bReleases, 1);
    // A released participant has no renderer resources left, so it must never be
    // ticked again.
    EXPECT_EQ(aRuns, 1);
    EXPECT_EQ(bRuns, 1);

    // Nothing is left to release a second time.
    registry.ReleaseAll();
    EXPECT_EQ(aReleases, 1);
    EXPECT_EQ(bReleases, 1);
}

TEST_F(RenderGraphTickRegistryTest, ReleaseTolerantOfTheRegistrantUnregisteringItself)
{
    // The real shape: the release callback runs the panel's own teardown, which
    // unregisters the handle it is holding.
    RenderGraphTickRegistry registry;
    int releases = 0;
    RenderGraphTickRegistry::Handle self = RenderGraphTickRegistry::kInvalidHandle;
    self = registry.Register([](uint64_t, GameEngine::UIManager*,
                                GameEngine::Rendering::RenderGraph::RGFrame&) {},
                             [&] {
                                 ++releases;
                                 registry.Unregister(self);
                             });

    registry.ReleaseAll();
    EXPECT_EQ(releases, 1);

    // And the panel's destructor, running later, unregisters the stale handle
    // again without resurrecting anything.
    registry.Unregister(self);
    Drain(registry, 1);
    SUCCEED();
}

TEST_F(RenderGraphTickRegistryTest, ReleaseAllSkipsParticipantsThatOwnNothing)
{
    RenderGraphTickRegistry registry;
    int runs = 0;
    registry.Register([&](uint64_t, GameEngine::UIManager*,
                          GameEngine::Rendering::RenderGraph::RGFrame&) { ++runs; },
                      nullptr);

    registry.ReleaseAll();
    Drain(registry, 1);
    EXPECT_EQ(runs, 0);
}

TEST_F(RenderGraphTickRegistryTest, ReleaseAllCoversParticipantsRegisteredMidDrain)
{
    // A participant added from inside another's tick is parked until the drain
    // unwinds. It owns its resources from the moment it registers, so a shutdown
    // on the following frame must still release it.
    RenderGraphTickRegistry registry;
    int lateReleases = 0;
    bool added = false;
    registry.Register([&](uint64_t, GameEngine::UIManager*,
                          GameEngine::Rendering::RenderGraph::RGFrame&) {
        if (added)
            return;
        added = true;
        registry.Register([](uint64_t, GameEngine::UIManager*,
                             GameEngine::Rendering::RenderGraph::RGFrame&) {},
                          [&] { ++lateReleases; });
    },
                      nullptr);

    Drain(registry, 1);
    registry.ReleaseAll();
    EXPECT_EQ(lateReleases, 1);
}
