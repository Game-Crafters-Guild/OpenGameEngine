// Q6 device-lost recovery — unit tests for the health state machine.
//
// These exercise DeviceHealthState directly (no GPU): transitions, edge
// reporting, and the Lost terminal invariant. Device-gated tests that need a
// real fence live in the DeviceRecoveryDevice suite further down and skip when
// no device can be created.

#include "Source/Vulkan/DeviceHealthState.h"
#include "Source/Vulkan/PendingWorkerDeviceLoss.h"
#include "Source/Vulkan/VulkanDevice.h"
#include "Source/Vulkan/VulkanFaultInjection.h"
#include "ScopedEnvVar.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <istream>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineCache.h"
#include "Rendering/Core/PipelineTypes.h"

using namespace GameEngine::Rendering;

// Fixed clock inputs for the transition tests that do not exercise the
// recover-relose window themselves. Deterministic and re-runnable; the tests that
// do exercise the window pass explicit times instead.
static constexpr auto kNow = DeviceHealthState::Clock::time_point{};
static constexpr auto kStick = std::chrono::seconds(60);

TEST(DeviceHealthStateMachine, DefaultsToHealthy)
{
    DeviceHealthState health;
    EXPECT_EQ(health.Load(), DeviceHealth::Healthy);
    EXPECT_TRUE(health.IsHealthy());
    EXPECT_FALSE(health.IsHung());
    EXPECT_FALSE(health.IsLost());
}

TEST(DeviceHealthStateMachine, HealthyToLostFiresEdgeOnce)
{
    DeviceHealthState health;
    EXPECT_TRUE(health.TransitionToLost(kNow, kStick));  // first edge
    EXPECT_EQ(health.Load(), DeviceHealth::Lost);
    EXPECT_FALSE(health.TransitionToLost(kNow, kStick)); // idempotent — no second edge
    EXPECT_EQ(health.Load(), DeviceHealth::Lost);
}

TEST(DeviceHealthStateMachine, HealthyToHungAndBack)
{
    DeviceHealthState health;
    const auto t0 = DeviceHealthState::Clock::now();
    EXPECT_TRUE(health.TransitionToHung(t0));   // edge Healthy->Hung
    EXPECT_EQ(health.Load(), DeviceHealth::Hung);
    EXPECT_FALSE(health.TransitionToHung(t0));   // already Hung — no edge

    EXPECT_TRUE(health.NoteFenceSignaled());   // edge Hung->Healthy
    EXPECT_EQ(health.Load(), DeviceHealth::Healthy);
    EXPECT_FALSE(health.NoteFenceSignaled());   // already Healthy — no edge
}

TEST(DeviceHealthStateMachine, HungEscalatesToLost)
{
    DeviceHealthState health;
    ASSERT_TRUE(health.TransitionToHung(DeviceHealthState::Clock::now()));
    EXPECT_TRUE(health.TransitionToLost(kNow, kStick));    // Hung -> Lost edge
    EXPECT_EQ(health.Load(), DeviceHealth::Lost);
}

TEST(DeviceHealthStateMachine, LostIsTerminal)
{
    DeviceHealthState health;
    ASSERT_TRUE(health.TransitionToLost(kNow, kStick));
    // Neither a timeout nor a signaled fence can revive a lost device.
    EXPECT_FALSE(health.TransitionToHung(DeviceHealthState::Clock::now()));
    EXPECT_EQ(health.Load(), DeviceHealth::Lost);
    EXPECT_FALSE(health.NoteFenceSignaled());
    EXPECT_EQ(health.Load(), DeviceHealth::Lost);
}

TEST(DeviceHealthStateMachine, ResetReturnsToHealthy)
{
    DeviceHealthState health;
    ASSERT_TRUE(health.TransitionToLost(kNow, kStick));
    health.Reset();
    EXPECT_EQ(health.Load(), DeviceHealth::Healthy);
    EXPECT_TRUE(health.IsHealthy());

    ASSERT_TRUE(health.TransitionToHung(DeviceHealthState::Clock::now()));
    health.Reset();
    EXPECT_EQ(health.Load(), DeviceHealth::Healthy);
}

// --- Slice 1: escalation cap, Hung elapsed, kill-switch parse -----------------

TEST(DeviceHealthStateMachine, EscalationCapFiresAfterCap)
{
    DeviceHealthState health;
    using Clock = DeviceHealthState::Clock;
    const auto t0 = Clock::time_point{}; // deterministic epoch
    const auto cap = std::chrono::seconds(30);

    ASSERT_TRUE(health.TransitionToHung(t0));
    EXPECT_FALSE(health.ShouldEscalate(t0, cap));                                  // just entered
    EXPECT_FALSE(health.ShouldEscalate(t0 + std::chrono::seconds(29), cap));        // under cap
    EXPECT_TRUE(health.ShouldEscalate(t0 + std::chrono::seconds(30), cap));         // at cap
    EXPECT_TRUE(health.ShouldEscalate(t0 + std::chrono::seconds(45), cap));         // past cap
}

TEST(DeviceHealthStateMachine, EscalationOnlyWhenHung)
{
    DeviceHealthState health;
    using Clock = DeviceHealthState::Clock;
    const auto cap = std::chrono::seconds(30);
    // Healthy never escalates.
    EXPECT_FALSE(health.ShouldEscalate(Clock::now() + std::chrono::hours(1), cap));
    // Lost never escalates (it is already terminal, handled by the loss path).
    ASSERT_TRUE(health.TransitionToLost(kNow, kStick));
    EXPECT_FALSE(health.ShouldEscalate(Clock::now() + std::chrono::hours(1), cap));
}

TEST(DeviceHealthStateMachine, HungElapsedTracksTime)
{
    DeviceHealthState health;
    using Clock = DeviceHealthState::Clock;
    const auto t0 = Clock::time_point{};
    EXPECT_DOUBLE_EQ(health.HungElapsedMs(t0), 0.0); // Healthy -> 0
    ASSERT_TRUE(health.TransitionToHung(t0));
    EXPECT_NEAR(health.HungElapsedMs(t0 + std::chrono::milliseconds(250)), 250.0, 0.001);
    health.NoteFenceSignaled();
    EXPECT_DOUBLE_EQ(health.HungElapsedMs(t0 + std::chrono::seconds(1)), 0.0); // Healthy again -> 0
}

TEST(DeviceHealthStateMachine, KillSwitchParse)
{
    // Only a leading '0' disables (matches the GE_* toggle idiom). Unset -> on.
    EXPECT_TRUE(ParseDeviceRecoveryEnabled(nullptr));
    EXPECT_TRUE(ParseDeviceRecoveryEnabled("1"));
    EXPECT_TRUE(ParseDeviceRecoveryEnabled(""));
    EXPECT_FALSE(ParseDeviceRecoveryEnabled("0"));
    EXPECT_FALSE(ParseDeviceRecoveryEnabled("0-legacy"));
}

// --- Slice 2: Tier-2 rebuild state machine -----------------------------------

TEST(DeviceHealthStateMachine, LostToRebuildingToAwaitingToHealthy)
{
    DeviceHealthState health;
    ASSERT_TRUE(health.TransitionToLost(kNow, kStick));

    ASSERT_TRUE(health.BeginRebuild()); // Lost -> Rebuilding
    EXPECT_EQ(health.Load(), DeviceHealth::Rebuilding);
    EXPECT_TRUE(health.IsRebuilding());
    EXPECT_EQ(health.RebuildAttempts(), 1u);

    ASSERT_TRUE(health.NoteRebuildSucceeded()); // -> AwaitingReprovision
    EXPECT_EQ(health.Load(), DeviceHealth::AwaitingReprovision);
    EXPECT_TRUE(health.IsAwaitingReprovision());
    EXPECT_FALSE(health.IsHealthy());
    // Device is functional (direct GPU ops) even though rendering stays suppressed.
    EXPECT_TRUE(health.IsDeviceUsable());
    // Attempts persist until a full recovery, so a still-dying device trips the cap.
    EXPECT_EQ(health.RebuildAttempts(), 1u);

    ASSERT_TRUE(health.NoteReprovisioned(kNow)); // -> Healthy
    EXPECT_EQ(health.Load(), DeviceHealth::Healthy);
    EXPECT_TRUE(health.IsHealthy());
    EXPECT_EQ(health.RebuildAttempts(), 0u); // cleared on full recovery
}

// Q6 slice 4 acceptance (d): a SECOND device loss after a full recovery drives a
// fresh full cycle — the retry budget is reset by the first NoteReprovisioned, so
// the second rebuild starts at attempt 1, not accumulated toward the cap.
TEST(DeviceHealthStateMachine, RepeatedLossAfterRecoveryResetsRetryBudget)
{
    DeviceHealthState health;

    // First cycle: Lost -> Rebuilding -> AwaitingReprovision -> Healthy.
    ASSERT_TRUE(health.TransitionToLost(kNow, kStick));
    ASSERT_TRUE(health.BeginRebuild());
    EXPECT_EQ(health.RebuildAttempts(), 1u);
    ASSERT_TRUE(health.NoteRebuildSucceeded());
    ASSERT_TRUE(health.NoteReprovisioned(kNow));
    EXPECT_EQ(health.Load(), DeviceHealth::Healthy);
    EXPECT_EQ(health.RebuildAttempts(), 0u);

    // Second, independent loss: the counter starts fresh, so the second cycle is
    // not one attempt closer to Failed — repeated recoverable losses never exhaust
    // the budget as long as each fully recovers.
    ASSERT_TRUE(health.TransitionToLost(kNow, kStick));
    ASSERT_TRUE(health.BeginRebuild());
    EXPECT_EQ(health.RebuildAttempts(), 1u) << "the retry budget must reset per fully-recovered incident";
    ASSERT_TRUE(health.NoteRebuildSucceeded());
    ASSERT_TRUE(health.NoteReprovisioned(kNow));
    EXPECT_EQ(health.Load(), DeviceHealth::Healthy);
    EXPECT_EQ(health.RebuildAttempts(), 0u);
}

TEST(DeviceHealthStateMachine, RebuildIsNotReentrant)
{
    DeviceHealthState health;
    ASSERT_TRUE(health.TransitionToLost(kNow, kStick));
    ASSERT_TRUE(health.BeginRebuild());
    EXPECT_FALSE(health.BeginRebuild()); // already Rebuilding — no re-entry
    EXPECT_EQ(health.RebuildAttempts(), 1u);
    EXPECT_TRUE(health.IsRebuilding());

    ASSERT_TRUE(health.NoteRebuildSucceeded());
    EXPECT_FALSE(health.BeginRebuild()); // not Lost — cannot start a rebuild
    EXPECT_TRUE(health.IsAwaitingReprovision());
}

TEST(DeviceHealthStateMachine, RebuildFailureReturnsToLostForRetry)
{
    DeviceHealthState health;
    ASSERT_TRUE(health.TransitionToLost(kNow, kStick));
    ASSERT_TRUE(health.BeginRebuild());
    ASSERT_TRUE(health.NoteRebuildFailed()); // Rebuilding -> Lost (retry next frame)
    EXPECT_EQ(health.Load(), DeviceHealth::Lost);
    EXPECT_TRUE(health.IsLost());
    // Attempt counter is not rewound by a failure — it keeps climbing toward the cap.
    EXPECT_EQ(health.RebuildAttempts(), 1u);
}

TEST(DeviceHealthStateMachine, RebuildRetryCapTripsFailed)
{
    DeviceHealthState health;
    const uint32_t cap = 3;
    ASSERT_TRUE(health.TransitionToLost(kNow, kStick));

    // Simulate the RebuildDevice caller: attempt, fail, retry, up to the budget.
    for (uint32_t i = 1; i <= cap; ++i)
    {
        ASSERT_TRUE(health.BeginRebuild());
        EXPECT_EQ(health.RebuildAttempts(), i);
        ASSERT_TRUE(health.NoteRebuildFailed()); // bringup failed -> back to Lost
        EXPECT_TRUE(health.IsLost());
    }

    // Budget exhausted: the caller checks RebuildAttempts() >= cap and gives up.
    EXPECT_GE(health.RebuildAttempts(), cap);
    ASSERT_TRUE(health.TransitionToFailed());
    EXPECT_EQ(health.Load(), DeviceHealth::Failed);
    EXPECT_TRUE(health.IsFailed());

    // Failed is terminal: no further rebuild, and a stray loss cannot revive it.
    EXPECT_FALSE(health.BeginRebuild());
    EXPECT_FALSE(health.TransitionToLost(kNow, kStick));
    EXPECT_TRUE(health.IsFailed());
}

TEST(DeviceHealthStateMachine, ResetClearsRebuildBudget)
{
    DeviceHealthState health;
    ASSERT_TRUE(health.TransitionToLost(kNow, kStick));
    ASSERT_TRUE(health.BeginRebuild());
    ASSERT_TRUE(health.NoteRebuildFailed());
    EXPECT_EQ(health.RebuildAttempts(), 1u);

    health.Reset(); // a fresh Initialize
    EXPECT_EQ(health.Load(), DeviceHealth::Healthy);
    EXPECT_EQ(health.RebuildAttempts(), 0u);
}

// --- Recover-relose loop bound ------------------------------------------------
//
// RebuildAttempts() counts consecutive rebuild FAILURES and is zeroed by every
// successful re-provision, so it cannot see a device that rebuilds cleanly and is
// then killed again by the same fault. RecoveryCycles() is what bounds that loop.

// Drive one full loss -> rebuild -> re-provision cycle, losing at `lostAt` and
// completing the recovery at `recoveredAt`.
static void RunRecoveryCycle(DeviceHealthState& health,
                             DeviceHealthState::Clock::time_point lostAt,
                             DeviceHealthState::Clock::time_point recoveredAt,
                             DeviceHealthState::Clock::duration stickWindow)
{
    ASSERT_TRUE(health.TransitionToLost(lostAt, stickWindow));
    ASSERT_TRUE(health.BeginRebuild());
    ASSERT_TRUE(health.NoteRebuildSucceeded());
    ASSERT_TRUE(health.NoteReprovisioned(recoveredAt));
}

TEST(DeviceHealthStateMachine, FirstLossOfSessionCountsNoRecoveryCycle)
{
    DeviceHealthState health;
    EXPECT_EQ(health.RecoveryCycles(), 0u);
    // Nothing has recovered yet, so there is no recovery for this loss to judge.
    ASSERT_TRUE(health.TransitionToLost(kNow, kStick));
    EXPECT_EQ(health.RecoveryCycles(), 0u);
}

TEST(DeviceHealthStateMachine, RecoveryThatDoesNotHoldCountsACycle)
{
    DeviceHealthState health;
    using Clock = DeviceHealthState::Clock;
    const auto t0 = Clock::time_point{};
    const auto stick = std::chrono::seconds(60);

    // Three recoveries, each killed 3 s later — the reported loop's shape.
    RunRecoveryCycle(health, t0, t0 + std::chrono::seconds(1), stick);
    EXPECT_EQ(health.RecoveryCycles(), 0u); // first loss judged nothing

    RunRecoveryCycle(health, t0 + std::chrono::seconds(4), t0 + std::chrono::seconds(5), stick);
    EXPECT_EQ(health.RecoveryCycles(), 1u);

    RunRecoveryCycle(health, t0 + std::chrono::seconds(8), t0 + std::chrono::seconds(9), stick);
    EXPECT_EQ(health.RecoveryCycles(), 2u);

    // The retry budget stays clean throughout — which is exactly why it cannot
    // bound this loop and RecoveryCycles() must.
    EXPECT_EQ(health.RebuildAttempts(), 0u);

    ASSERT_TRUE(health.TransitionToLost(t0 + std::chrono::seconds(12), stick));
    EXPECT_EQ(health.RecoveryCycles(), 3u);
}

TEST(DeviceHealthStateMachine, RecoveryThatHoldsRestartsTheCycleCount)
{
    DeviceHealthState health;
    using Clock = DeviceHealthState::Clock;
    const auto t0 = Clock::time_point{};
    const auto stick = std::chrono::seconds(60);

    RunRecoveryCycle(health, t0, t0 + std::chrono::seconds(1), stick);
    RunRecoveryCycle(health, t0 + std::chrono::seconds(4), t0 + std::chrono::seconds(5), stick);
    ASSERT_EQ(health.RecoveryCycles(), 1u);

    // This recovery survives the window, so the next loss is a fresh episode
    // rather than another turn of the same loop.
    RunRecoveryCycle(health, t0 + std::chrono::seconds(8), t0 + std::chrono::seconds(9), stick);
    ASSERT_EQ(health.RecoveryCycles(), 2u);
    ASSERT_TRUE(health.TransitionToLost(t0 + std::chrono::seconds(9) + stick, stick));
    EXPECT_EQ(health.RecoveryCycles(), 0u);
}

TEST(DeviceHealthStateMachine, CycleBoundaryIsAtExactlyTheStickWindow)
{
    using Clock = DeviceHealthState::Clock;
    const auto t0 = Clock::time_point{};
    const auto stick = std::chrono::seconds(60);

    // One tick under the window still counts as a loop...
    {
        DeviceHealthState health;
        RunRecoveryCycle(health, t0, t0 + std::chrono::seconds(1), stick);
        ASSERT_TRUE(health.TransitionToLost(t0 + std::chrono::seconds(1) + stick - Clock::duration(1), stick));
        EXPECT_EQ(health.RecoveryCycles(), 1u);
    }
    // ...and exactly at the window the recovery is credited as having held.
    {
        DeviceHealthState health;
        RunRecoveryCycle(health, t0, t0 + std::chrono::seconds(1), stick);
        ASSERT_TRUE(health.TransitionToLost(t0 + std::chrono::seconds(1) + stick, stick));
        EXPECT_EQ(health.RecoveryCycles(), 0u);
    }
}

TEST(DeviceHealthStateMachine, ResetClearsTheRecoveryCycleCount)
{
    DeviceHealthState health;
    using Clock = DeviceHealthState::Clock;
    const auto t0 = Clock::time_point{};
    const auto stick = std::chrono::seconds(60);

    RunRecoveryCycle(health, t0, t0 + std::chrono::seconds(1), stick);
    RunRecoveryCycle(health, t0 + std::chrono::seconds(4), t0 + std::chrono::seconds(5), stick);
    ASSERT_EQ(health.RecoveryCycles(), 1u);

    health.Reset(); // a fresh Initialize starts a new session
    EXPECT_EQ(health.RecoveryCycles(), 0u);
    // And the cleared has-recovered flag means the next loss judges nothing.
    ASSERT_TRUE(health.TransitionToLost(t0 + std::chrono::seconds(6), stick));
    EXPECT_EQ(health.RecoveryCycles(), 0u);
}

// --- Slice 5: fault-injection parse ------------------------------------------

// --- Worker-thread device-loss hand-off ---------------------------------------

TEST(PendingWorkerLoss, NothingPendingByDefault)
{
    PendingWorkerDeviceLoss pending;
    EXPECT_FALSE(pending.HasPending());
    EXPECT_FALSE(pending.Claim(0));
}

TEST(PendingWorkerLoss, ClaimOnTheSameGenerationSucceedsExactlyOnce)
{
    PendingWorkerDeviceLoss pending;
    pending.Note(7);
    EXPECT_TRUE(pending.HasPending());
    EXPECT_TRUE(pending.Claim(7));
    // Claiming consumes it: a second BeginFrame must not re-report the same loss.
    EXPECT_FALSE(pending.HasPending());
    EXPECT_FALSE(pending.Claim(7));
}

// The guard that stops a worker's observation from latching Lost onto the device
// that replaced the one which actually died.
TEST(PendingWorkerLoss, ClaimAfterARebuildDropsTheStaleRecord)
{
    PendingWorkerDeviceLoss pending;
    pending.Note(3);
    EXPECT_FALSE(pending.Claim(4)) << "a loss seen on generation 3 says nothing about generation 4";
    // Dropped, not left behind: a stale record must not block the next real one.
    EXPECT_FALSE(pending.HasPending());
    pending.Note(4);
    EXPECT_TRUE(pending.Claim(4));
}

TEST(PendingWorkerLoss, FirstRecordWinsAndLaterOnesAreTheSameLoss)
{
    PendingWorkerDeviceLoss pending;
    pending.Note(2);
    pending.Note(9); // a second worker on the same dead device
    EXPECT_FALSE(pending.Claim(9)) << "the recorded generation must stay the one first observed";
    pending.Note(2);
    EXPECT_TRUE(pending.Claim(2));
}

// Every upload worker fails at once when the device dies; the render thread must
// see exactly one claimable observation, not one per worker.
TEST(PendingWorkerLoss, ConcurrentWorkersProduceExactlyOneClaim)
{
    constexpr uint32_t kGeneration = 11;
    constexpr int kWorkers = 16;
    constexpr int kRounds = 200;

    for (int round = 0; round < kRounds; ++round)
    {
        PendingWorkerDeviceLoss pending;
        std::atomic<bool> go{false};
        std::vector<std::thread> workers;
        workers.reserve(kWorkers);
        for (int w = 0; w < kWorkers; ++w)
        {
            workers.emplace_back([&]
            {
                while (!go.load(std::memory_order_acquire))
                {
                }
                pending.Note(kGeneration);
            });
        }
        go.store(true, std::memory_order_release);
        for (std::thread& w : workers)
        {
            w.join();
        }

        EXPECT_TRUE(pending.Claim(kGeneration)) << "round " << round;
        EXPECT_FALSE(pending.Claim(kGeneration)) << "round " << round;
    }
}

TEST(FaultInjectionParse, UnsetIsInert)
{
    const VulkanFaultInjection cfg = ParseFaultInjection(nullptr, nullptr);
    EXPECT_FALSE(cfg.Active());
    EXPECT_FALSE(cfg.ShouldForceLostAtFrame(1));
    EXPECT_FALSE(cfg.ShouldForceLostAtSubmit(1));
    EXPECT_FALSE(cfg.ShouldForceTimeoutAtFrame(1));
    // Empty strings are also inert.
    EXPECT_FALSE(ParseFaultInjection("", "").Active());
}

TEST(FaultInjectionParse, ForceLostAtFrame)
{
    const VulkanFaultInjection cfg = ParseFaultInjection("42", nullptr);
    EXPECT_TRUE(cfg.Active());
    EXPECT_EQ(cfg.lostMode, VulkanFaultInjection::LostMode::AtFrame);
    EXPECT_FALSE(cfg.ShouldForceLostAtFrame(41)); // before target
    EXPECT_TRUE(cfg.ShouldForceLostAtFrame(42));   // at target
    EXPECT_TRUE(cfg.ShouldForceLostAtFrame(43));   // at/after target (frame N may lack a submit)
    EXPECT_FALSE(cfg.ShouldForceLostAtSubmit(42)); // wrong granularity
}

TEST(FaultInjectionParse, ForceLostAtMultipleFramesReArmsForRepeatLoss)
{
    VulkanFaultInjection cfg = ParseFaultInjection("42,100,250", nullptr);
    EXPECT_TRUE(cfg.Active());
    EXPECT_EQ(cfg.lostMode, VulkanFaultInjection::LostMode::AtFrame);
    EXPECT_EQ(cfg.lostTarget, 42u);
    ASSERT_EQ(cfg.lostExtraTargets.size(), 2u);
    EXPECT_EQ(cfg.lostExtraTargets[0], 100u);
    EXPECT_EQ(cfg.lostExtraTargets[1], 250u);

    // Fire #1 at frame 42 -> arms frame 100.
    EXPECT_TRUE(cfg.ShouldForceLostAtFrame(42));
    cfg.ConsumeLostTarget();
    EXPECT_EQ(cfg.lostMode, VulkanFaultInjection::LostMode::AtFrame);
    EXPECT_FALSE(cfg.ShouldForceLostAtFrame(43)); // 43 < 100
    EXPECT_TRUE(cfg.ShouldForceLostAtFrame(100));

    // Fire #2 -> arms frame 250.
    cfg.ConsumeLostTarget();
    EXPECT_TRUE(cfg.ShouldForceLostAtFrame(250));

    // Fire #3 -> queue empty -> disarms.
    cfg.ConsumeLostTarget();
    EXPECT_FALSE(cfg.Active());
    EXPECT_FALSE(cfg.ShouldForceLostAtFrame(1000));
}

TEST(FaultInjectionParse, ForceLostAtSubmit)
{
    const VulkanFaultInjection cfg = ParseFaultInjection("submit:7", nullptr);
    EXPECT_TRUE(cfg.Active());
    EXPECT_EQ(cfg.lostMode, VulkanFaultInjection::LostMode::AtSubmit);
    EXPECT_TRUE(cfg.ShouldForceLostAtSubmit(7));
    EXPECT_FALSE(cfg.ShouldForceLostAtSubmit(6));
    EXPECT_FALSE(cfg.ShouldForceLostAtFrame(7)); // wrong granularity
}

TEST(FaultInjectionParse, ForceBeginFrameTimeout)
{
    const VulkanFaultInjection cfg = ParseFaultInjection(nullptr, "9");
    EXPECT_TRUE(cfg.Active());
    EXPECT_TRUE(cfg.timeoutEnabled);
    EXPECT_EQ(cfg.timeoutFrames, 1u); // single-frame Hung by default
    EXPECT_TRUE(cfg.ShouldForceTimeoutAtFrame(9));
    EXPECT_FALSE(cfg.ShouldForceTimeoutAtFrame(8));
    EXPECT_FALSE(cfg.ShouldForceTimeoutAtFrame(10)); // K==1: only frame 9
    EXPECT_FALSE(cfg.ShouldForceLostAtFrame(9));      // independent hook
}

TEST(FaultInjectionParse, ForceBeginFrameTimeoutSustainedRange)
{
    // "N:K" sustains the Hung state across K consecutive frames so the >250ms
    // recovery toast can be observed before resume.
    const VulkanFaultInjection cfg = ParseFaultInjection(nullptr, "100:5");
    EXPECT_TRUE(cfg.timeoutEnabled);
    EXPECT_EQ(cfg.timeoutFrame, 100u);
    EXPECT_EQ(cfg.timeoutFrames, 5u);
    EXPECT_FALSE(cfg.ShouldForceTimeoutAtFrame(99));
    EXPECT_TRUE(cfg.ShouldForceTimeoutAtFrame(100));  // range start
    EXPECT_TRUE(cfg.ShouldForceTimeoutAtFrame(104));  // last in [100,105)
    EXPECT_FALSE(cfg.ShouldForceTimeoutAtFrame(105)); // resumes here
}

TEST(FaultInjectionParse, ZeroAndMalformedOrdinalsAreInert)
{
    EXPECT_FALSE(ParseFaultInjection("0", nullptr).Active());        // frame 0 never fires
    EXPECT_FALSE(ParseFaultInjection("submit:0", nullptr).Active());  // submit 0 never fires
    EXPECT_FALSE(ParseFaultInjection(nullptr, "0").Active());         // timeout frame 0 never fires
    EXPECT_FALSE(ParseFaultInjection("submit:xyz", nullptr).Active()); // unparsable -> 0 -> inert
}

// --- Slice 5: device-gated injection (skips without a real Vulkan device) ----

namespace
{
using GameEngine::Rendering::Tests::ScopedEnvVar;

// Create + initialize a headless Vulkan device, or nullptr if none is available.
std::unique_ptr<IDevice> MakeHeadlessDevice()
{
    // Deliberately unscoped: every device in this process wants headless mode.
    GameEngine::Rendering::Tests::SetEnvVar("GE_HEADLESS_TEST", "1");
    DeviceDesc dd{};
    dd.preferredAPI = GraphicsAPI::Vulkan;
    dd.enableDynamicRendering = true;
    auto dev = DeviceFactory::CreateDevice(dd);
    if (!dev || !dev->Initialize(dd))
    {
        return nullptr;
    }
    return dev;
}

// As RunOneFrame, but records a diagnostic marker so the frame carries a GPU
// checkpoint when checkpoints are armed.
void RunOneFrameWithMarker(IDevice& dev, const char* marker)
{
    dev.TickDeviceRecovery();
    if (!dev.BeginFrame())
    {
        return;
    }
    auto cl = dev.CreateCommandList(IDevice::QueueType::Graphics);
    if (cl)
    {
        cl->Begin();
        cl->SetMarker(marker);
        cl->End();
        std::vector<CommandList*> lists{cl.get()};
        dev.ExecuteCommandLists(lists);
    }
    dev.Present();
}

void RunOneFrame(IDevice& dev)
{
    // Mirror the render loop: the rebuild retry is driven by TickDeviceRecovery on
    // the unconditional per-tick poll (moved off BeginFrame so a suppressed-rendering
    // failed rebuild does not starve the retry — the M3 fix). Call it every tick,
    // before BeginFrame, exactly as EditorApplication::Render does.
    dev.TickDeviceRecovery();
    if (!dev.BeginFrame())
    {
        return;
    }
    auto cl = dev.CreateCommandList(IDevice::QueueType::Graphics);
    if (cl)
    {
        cl->Begin();
        cl->End();
        std::vector<CommandList*> lists{cl.get()};
        dev.ExecuteCommandLists(lists);
    }
    dev.Present();
}
} // namespace

// Slice 2: an injected loss now drives an in-place rebuild (rather than latching
// Lost for the process lifetime). The rebuilt device must be functional, the
// shared VkInstance must be kept, and the disk pipeline cache reconstructed. The
// device is left AwaitingReprovision (the upper-layer re-provision is slice 3a);
// this test stands in for that consumer via NotifyReprovisionComplete().
TEST(DeviceRecoveryDevice, InjectedLossRebuildsToFunctionalDevice)
{
    ScopedEnvVar forcedLoss("GE_VK_FORCE_DEVICE_LOST", "2");
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "no Vulkan device available";
    auto* vk = static_cast<VulkanDevice*>(dev.get());
    const VkInstance instanceBefore = vk->GetVkInstance();
    const VkPhysicalDevice physicalBefore = vk->GetVkPhysicalDevice();
    EXPECT_EQ(dev->GetDeviceHealth(), DeviceHealth::Healthy);

    // Drive frames: the injected loss fires on a graphics submit, latches Lost,
    // and the next BeginFrame rebuilds in place -> AwaitingReprovision.
    bool reachedAwaiting = false;
    for (int i = 0; i < 12 && !reachedAwaiting; ++i)
    {
        RunOneFrame(*dev);
        reachedAwaiting = (dev->GetDeviceHealth() == DeviceHealth::AwaitingReprovision);
    }
    ASSERT_TRUE(reachedAwaiting) << "injected loss should rebuild to AwaitingReprovision";

    // Instance + physical device are instance-scoped and must survive the rebuild
    // (F8: no refcount churn, no destroy/recreate — the very same handles).
    EXPECT_EQ(vk->GetVkInstance(), instanceBefore);
    EXPECT_EQ(vk->GetVkPhysicalDevice(), physicalBefore);
    // Disk pipeline cache reconstructed on the new device (reloaded, or a clean
    // empty fallback — either way a valid cache object exists post-rebuild, M2).
    EXPECT_NE(vk->GetVkDiskPipelineCache(), nullptr);

    // HEALTHY device proof: create + upload a buffer on the fresh device and read
    // it back through its host-visible mapping (exercises the new VkDevice + VMA).
    {
        const uint32_t kPattern[4] = {0xC0FFEEu, 0xBADF00Du, 0x1234u, 0xABCDu};
        BufferHandle buf = dev->CreateUploadBuffer(sizeof(kPattern), "RebuildProofBuffer");
        ASSERT_TRUE(buf.IsValid());
        dev->UpdateBuffer(buf, 0, sizeof(kPattern), kPattern);
        void* mapped = dev->MapBuffer(buf);
        ASSERT_NE(mapped, nullptr);
        EXPECT_EQ(std::memcmp(mapped, kPattern, sizeof(kPattern)), 0);
        dev->UnmapBuffer(buf);
        dev->DestroyBuffer(buf);
    }

    // Stand in for the slice-3a re-provision consumer: once complete the device
    // returns to Healthy and rendering resumes. A trivial frame (submit + fence
    // wait + present) must go through on the rebuilt device without a crash.
    dev->NotifyReprovisionComplete();
    EXPECT_EQ(dev->GetDeviceHealth(), DeviceHealth::Healthy);
    RunOneFrame(*dev);
    RunOneFrame(*dev);
    // The one-shot injection was consumed at first fire, so no second loss.
    EXPECT_EQ(dev->GetDeviceHealth(), DeviceHealth::Healthy);

    dev->Shutdown();
}

// The allocator/rebuild exclusion, exercised with GENUINE concurrency. The frame
// loop in the hosts serializes the synchronous job waves against the recovery
// tick by structure; this test deliberately breaks that serialization — worker
// threads hammer the allocator-touching entries (CreateBuffer / UpdateBuffer /
// MapBuffer / DestroyBuffer) with no frame coupling while the test thread drives
// injected losses through full in-place rebuild cycles. Every worker entry must
// serialize against RebuildDevice's exclusive teardown+bringup window
// (m_DeviceRebuildMutex via DeviceRebuildSharedGuard); without that exclusion a
// worker resumes into vmaCreateBuffer/vmaMapMemory on an allocator ShutdownVMA
// is destroying, or routes a destroy into the immediate helpers mid-teardown.
TEST(DeviceRecoveryDevice, ConcurrentAllocatorTrafficSurvivesRebuildCycles)
{
    // Three injected losses -> three rebuild cycles. Three is the ceiling inside
    // the recover-relose stick window: the cycle counter reads 0, 1, 2 at the
    // three rebuilds and kMaxRecoveryCycles(3) would turn a fourth into Failed.
    ScopedEnvVar forcedLoss("GE_VK_FORCE_DEVICE_LOST", "3,9,15");
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "no Vulkan device available";
    auto* vk = static_cast<VulkanDevice*>(dev.get());

    constexpr uint32_t kWorkers = 8;
    constexpr size_t kPatternWords = 64;
    std::atomic<bool> start{false};
    std::atomic<bool> stop{false};
    std::atomic<uint32_t> readyCount{0};
    std::atomic<uint64_t> workerIterations{0};
    std::atomic<uint64_t> contentMismatches{0};

    std::vector<std::thread> workers;
    workers.reserve(kWorkers);
    for (uint32_t t = 0; t < kWorkers; ++t)
    {
        workers.emplace_back(
            [&, t]()
            {
                readyCount.fetch_add(1, std::memory_order_release);
                while (!start.load(std::memory_order_acquire))
                    std::this_thread::yield();

                uint32_t pattern[kPatternWords];
                for (uint32_t i = 0; i < kPatternWords; ++i)
                    pattern[i] = (t << 24) ^ (i * 2654435761u);

                while (!stop.load(std::memory_order_acquire))
                {
                    // A create that lands while the device is mid-rebuild may
                    // fail, and a handle that predates the rebuild goes stale
                    // (generation mismatch): both are safe outcomes. What must
                    // never happen is a crash or torn content on a mapping that
                    // did resolve.
                    BufferHandle buf = dev->CreateUploadBuffer(sizeof(pattern), "RebuildStressWorker");
                    if (buf.IsValid())
                    {
                        dev->UpdateBuffer(buf, 0, sizeof(pattern), pattern);
                        {
                            // The scoped map holds the rebuild guard across
                            // map..read, so a rebuild cannot free the allocation
                            // between the map and the memcmp — a resolved mapping
                            // is readable for the whole scope, and the mismatch
                            // counter can only report genuine torn content. (A
                            // bare MapBuffer would leave the read window
                            // unguarded; off-render-thread readers must use the
                            // scoped form — see IDevice::MapBuffer.)
                            VulkanDevice::ScopedBufferMap mapped(*vk, buf);
                            if (mapped)
                            {
                                if (std::memcmp(mapped.Get(), pattern, sizeof(pattern)) != 0)
                                    contentMismatches.fetch_add(1, std::memory_order_relaxed);
                            }
                        }
                        dev->DestroyBuffer(buf);
                    }
                    workerIterations.fetch_add(1, std::memory_order_relaxed);
                }
            });
    }
    while (readyCount.load(std::memory_order_acquire) < kWorkers)
        std::this_thread::yield();
    start.store(true, std::memory_order_release);

    // Drive frames with the workers live. Each AwaitingReprovision observation is
    // one completed in-place rebuild; re-provision immediately so the next
    // injected loss can fire.
    uint32_t completedRebuilds = 0;
    for (int frame = 0; frame < 400 && completedRebuilds < 3; ++frame)
    {
        RunOneFrame(*dev);
        if (dev->GetDeviceHealth() == DeviceHealth::AwaitingReprovision)
        {
            ++completedRebuilds;
            dev->NotifyReprovisionComplete();
        }
    }

    stop.store(true, std::memory_order_release);
    for (std::thread& worker : workers)
        worker.join();

    EXPECT_EQ(completedRebuilds, 3u)
        << "all three injected losses should complete an in-place rebuild under worker load";
    EXPECT_EQ(contentMismatches.load(std::memory_order_relaxed), 0u)
        << "a resolved mapping must never expose torn content";
    EXPECT_GT(workerIterations.load(std::memory_order_relaxed), 0u)
        << "workers must have actually exercised the allocator entries";

    // The surviving device is functional: create + upload + readback on the
    // thread that owns it, exactly as the rebuild sibling above proves.
    {
        const uint32_t kPattern[4] = {0xC0FFEEu, 0xBADF00Du, 0x1234u, 0xABCDu};
        BufferHandle buf = dev->CreateUploadBuffer(sizeof(kPattern), "RebuildStressProofBuffer");
        ASSERT_TRUE(buf.IsValid());
        dev->UpdateBuffer(buf, 0, sizeof(kPattern), kPattern);
        void* mapped = dev->MapBuffer(buf);
        ASSERT_NE(mapped, nullptr);
        EXPECT_EQ(std::memcmp(mapped, kPattern, sizeof(kPattern)), 0);
        dev->UnmapBuffer(buf);
        dev->DestroyBuffer(buf);
    }

    dev->Shutdown();
}

// NOTE: The GE_DEVICE_RECOVERY kill switch is verified at the parse level by
// DeviceHealthStateMachine.KillSwitchParse. A device-gated variant is deliberately
// omitted: DeviceRecoveryEnabledFromEnv() latches the env read in a process-static
// (once per process), so a per-test toggle cannot take effect after any earlier
// test in this binary has already created a device.

TEST(DeviceRecoveryDevice, ForcedBeginFrameTimeoutEntersHungThenResumes)
{
    ScopedEnvVar forcedTimeout("GE_VK_FORCE_BEGINFRAME_TIMEOUT", "6");
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "no Vulkan device available";

    bool sawHung = false;
    bool recovered = false;
    for (int i = 0; i < 16; ++i)
    {
        RunOneFrame(*dev);
        const DeviceHealth h = dev->GetDeviceHealth();
        if (h == DeviceHealth::Hung)
        {
            sawHung = true;
        }
        if (sawHung && h == DeviceHealth::Healthy)
        {
            recovered = true;
        }
        // A forced timeout must never latch Lost.
        EXPECT_NE(h, DeviceHealth::Lost);
    }
    EXPECT_TRUE(sawHung) << "forced BeginFrame timeout should enter Hung";
    EXPECT_TRUE(recovered) << "poll-resume should return to Healthy";
    EXPECT_EQ(dev->GetDeviceHealth(), DeviceHealth::Healthy);

    dev->Shutdown();
}

// GPU device-loss breadcrumbs: the loss edge must run checkpoint retrieval and
// survive it.
//
// This deliberately does NOT assert that checkpoint data comes back. An injected
// loss is a classified VkResult on a device that is still perfectly healthy, not
// a real GPU fault, so a driver returning zero checkpoints here is correct
// behaviour — asserting non-empty data would be asserting something the
// injection cannot produce, and would fail for the right reasons on some drivers
// and pass for the wrong ones on others. What is genuinely provable headlessly
// is that markers recorded checkpoints, that the retrieval path RAN on the loss
// edge, and that it did not crash. End-to-end localization of a real GPU fault
// needs a real TDR and is not covered by any test here.
TEST(DeviceRecoveryDevice, InjectedLossRunsGpuCheckpointRetrievalSafely)
{
    ScopedEnvVar checkpoints("GE_VK_GPU_CHECKPOINTS", "1");
    ScopedEnvVar forcedLoss("GE_VK_FORCE_DEVICE_LOST", "2");
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "no Vulkan device available";
    auto* vk = static_cast<VulkanDevice*>(dev.get());
    if (!vk->GpuCheckpointsAvailable())
    {
        dev->Shutdown();
        GTEST_SKIP() << "device does not support VK_NV_device_diagnostic_checkpoints";
    }
    EXPECT_TRUE(vk->GpuCheckpointsEnabled());
    EXPECT_TRUE(vk->LastGpuCheckpointReport().empty()) << "no loss reported yet";

    bool leftHealthy = false;
    for (int i = 0; i < 12 && !leftHealthy; ++i)
    {
        RunOneFrameWithMarker(*dev, "Recovery.Checkpoint");
        leftHealthy = (dev->GetDeviceHealth() != DeviceHealth::Healthy);
    }
    ASSERT_TRUE(leftHealthy) << "injected loss should have driven the device out of Healthy";

    EXPECT_GT(vk->DebugGpuCheckpointsRecorded(), 0u)
        << "markers on a recording command list should have recorded checkpoints";
    // Retrieval ran on the loss edge and produced a report. Its CONTENT is
    // deliberately unasserted (see the note above) — only that the path executed.
    EXPECT_FALSE(vk->LastGpuCheckpointReport().empty())
        << "the device-lost handler did not run GPU checkpoint retrieval";
    // Printed so the report the loss path actually produced is visible in the
    // run log; an injected loss may legitimately show no executed checkpoints.
    std::cout << "[ report   ] " << vk->LastGpuCheckpointReport() << std::endl;

    dev->Shutdown();
}

// Checkpoint retrieval is valid at any time, which is why the test above runs it
// on an injected loss. The fault query is not: VUID-vkGetDeviceFaultInfoEXT-
// device-07336 requires the device to be in the _lost_ state, and an injected
// loss is a classified VkResult on a device that is still perfectly healthy — the
// injection site's own comment says it runs "against a live device". So the loss
// edge must skip the query here, and must say that it skipped, because an empty
// fault report and a missing instrument must never read alike.
TEST(DeviceRecoveryDevice, AnInjectedLossSkipsTheDeviceFaultQuery)
{
    ScopedEnvVar forcedLoss("GE_VK_FORCE_DEVICE_LOST", "2");
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "no Vulkan device available";
    auto* vk = static_cast<VulkanDevice*>(dev.get());
    EXPECT_TRUE(vk->LastDeviceFaultReport().empty()) << "no loss reported yet";

    bool leftHealthy = false;
    for (int i = 0; i < 12 && !leftHealthy; ++i)
    {
        RunOneFrameWithMarker(*dev, "Recovery.DeviceFault");
        leftHealthy = (dev->GetDeviceHealth() != DeviceHealth::Healthy);
    }
    ASSERT_TRUE(leftHealthy) << "injected loss should have driven the device out of Healthy";

    // Asserted whether or not this device supports the extension, which is the
    // point: genuineness is decided before availability, so an armed instrument is
    // exactly as quiet here as an absent one — and says which it was.
    EXPECT_EQ(vk->LastDeviceFaultReport(),
              "<not queried: loss was synthesized, device is not in the lost state>")
        << "a synthesized loss must not query a live device for fault records";
    std::cout << "[ report   ] device-fault armed="
              << (vk->DeviceFaultReportingAvailable() ? "yes" : "no") << ", report=\""
              << vk->LastDeviceFaultReport() << "\"" << std::endl;

    dev->Shutdown();
}

// The recover-relose bound, end to end. Each injected loss rebuilds and re-provisions
// cleanly, so the retry budget is zeroed every time and cannot stop anything; only
// the recovery-cycle count can. Every recovery here dies well inside the stick
// window, so the fourth loss must refuse to rebuild and land in Failed — which is
// what surfaces save-and-restart instead of looping until the process dies.
//
// Failed is terminal, so this also pins that the engine stops rebuilding: a device
// that kept recovering would report Healthy or AwaitingReprovision at the end.
TEST(DeviceRecoveryDevice, RepeatedNonHoldingRecoveriesStopRebuildingAndFail)
{
    // Frame ordinals climb monotonically across rebuilds, so these are four
    // separate losses, each a few frames after the previous recovery.
    ScopedEnvVar forcedLoss("GE_VK_FORCE_DEVICE_LOST", "2,5,8,11");
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "no Vulkan device available";
    auto* vk = static_cast<VulkanDevice*>(dev.get());
    ASSERT_EQ(dev->GetDeviceHealth(), DeviceHealth::Healthy);

    int recoveries = 0;
    bool failed = false;
    for (int frame = 0; frame < 200 && !failed; ++frame)
    {
        RunOneFrame(*dev);
        if (dev->GetDeviceHealth() == DeviceHealth::AwaitingReprovision)
        {
            dev->NotifyReprovisionComplete();
            ++recoveries;
        }
        failed = (dev->GetDeviceHealth() == DeviceHealth::Failed);
    }

    ASSERT_TRUE(failed) << "four non-holding recoveries must end in Failed, not another rebuild; "
                        << "reached " << recoveries << " recoveries, health now "
                        << (int)dev->GetDeviceHealth();
    // The cap is on recoveries that did not hold, so the loop gives up after
    // exactly kMaxRecoveryCycles of them rather than on the first repeat.
    EXPECT_EQ(recoveries, 3) << "gave up after the wrong number of recovery cycles";
    EXPECT_EQ(vk->GetDeviceHealth(), DeviceHealth::Failed);

    // Terminal: further ticks must not resurrect the device into another rebuild.
    RunOneFrame(*dev);
    RunOneFrame(*dev);
    EXPECT_EQ(dev->GetDeviceHealth(), DeviceHealth::Failed) << "Failed must be terminal";

    dev->Shutdown();
}

// --- Slice 3a: concrete-pipeline-cache invalidation contract (device-free) ----
// The device-lost re-provision drops the concrete VkPipeline map (its handles are
// dead) but must KEEP the interned descs + SPIR-V so PSOs recompile warm. This is
// the whole reason ClearConcreteOnly exists apart from Clear (which tombstones the
// intern tables, forcing a full re-intern). Pure PipelineCache — no GPU.

namespace
{
ComputePipelineDesc MakeMinimalComputeDescForReprovision()
{
    ComputePipelineDesc d;
    d.ComputeShader =
        std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{0x1, 0x2, 0x3, 0x4});
    d.PushConstants.Size = 16;
    d.PushConstants.StageMask = 0x20; // compute stage
    return d;
}
} // namespace

TEST(PipelineCacheReprovision, ClearConcreteOnlyDropsConcreteButKeepsInternTables)
{
    PipelineCache cache;
    const ComputePipelineId id = cache.InternComputePipeline(MakeMinimalComputeDescForReprovision());
    ASSERT_TRUE(id.IsValid());
    ASSERT_NE(cache.LookupComputePipeline(id), nullptr);

    const uint64_t key = PipelineCache::CombineComputeKey(id);
    PipelineCache::ConcreteInsertInfo info{};
    info.PipelineIdValue = id.Value;
    info.IsCompute = true;
    cache.InsertConcrete(key, info);

    PipelineHandle out{};
    EXPECT_TRUE(cache.TryGetConcrete(key, out)); // concrete entry present

    cache.ClearConcreteOnly();

    // Concrete map dropped — a lookup now misses (and would recompile).
    EXPECT_FALSE(cache.TryGetConcrete(key, out));
    // Intern table SURVIVES — the desc (+ SPIR-V) is still resolvable, so the
    // recompile is warm and the id stays valid (no re-intern).
    EXPECT_NE(cache.LookupComputePipeline(id), nullptr);
}

TEST(PipelineCacheReprovision, ClearTombstonesInternTables_ContrastWithClearConcreteOnly)
{
    PipelineCache cache;
    const ComputePipelineId id = cache.InternComputePipeline(MakeMinimalComputeDescForReprovision());
    ASSERT_TRUE(id.IsValid());
    ASSERT_NE(cache.LookupComputePipeline(id), nullptr);

    // Full Clear() tombstones the interned desc — the opposite of what a device
    // rebuild wants. This pins the contract that separates the two entry points.
    cache.Clear();
    EXPECT_EQ(cache.LookupComputePipeline(id), nullptr);
}

// --- Per-device caches across an in-place rebuild ----------------------------
//
// The IDevice POINTER survives RebuildDevice — that is the whole design (cached
// IDevice* stay valid, retained SPIR-V stays warm). Helper passes key their
// per-device GPU handles on exactly that pointer, so a cache cleaned only at
// shutdown keeps serving handles from the destroyed VkDevice for the rest of the
// session. RegisterPerDeviceCacheCleanup is the single registration for dropping
// such a cache, so there is no second seam to subscribe to by mistake; these pin
// that it fires on every death, and — separately — that it fires EARLY ENOUGH to
// be useful.

TEST(PerDeviceCacheCleanup, RegistersOnBothDeathEvents)
{
    ScopedEnvVar forcedLoss("GE_VK_FORCE_DEVICE_LOST", "2");
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "no Vulkan device available";

    int cleanupCalls = 0;
    dev->RegisterPerDeviceCacheCleanup("Test.PerDeviceCache",
                                       [&cleanupCalls](IDevice*) { ++cleanupCalls; });
    ASSERT_EQ(cleanupCalls, 0) << "registration must not invoke the cleanup";

    // First death event: the in-place rebuild. This is the one the shutdown-only
    // registration missed, and the one that made a cached sampler go stale.
    bool reachedAwaiting = false;
    for (int i = 0; i < 12 && !reachedAwaiting; ++i)
    {
        RunOneFrame(*dev);
        reachedAwaiting = (dev->GetDeviceHealth() == DeviceHealth::AwaitingReprovision);
    }
    ASSERT_TRUE(reachedAwaiting) << "injected loss should rebuild to AwaitingReprovision";
    EXPECT_EQ(cleanupCalls, 1) << "a device rebuild must drop per-device caches";

    dev->NotifyReprovisionComplete();
    ASSERT_EQ(dev->GetDeviceHealth(), DeviceHealth::Healthy);

    // Second death event: shutdown. Rebuilt-callbacks are kept (a session can lose
    // the device more than once), so the same cleanup must fire again here.
    dev->Shutdown();
    EXPECT_EQ(cleanupCalls, 2) << "shutdown must drop per-device caches too";

}

// A sampler created before the loss is dead after it, while its handle still
// reports IsValid(). This is the premise the pass caches were violating, stated
// as a test so it cannot regress quietly: anything holding a SamplerHandle across
// a rebuild is holding a handle that resolves to nothing.
TEST(PerDeviceCacheCleanup, SamplerHandleFromBeforeTheRebuildStillReportsValidButIsDead)
{
    ScopedEnvVar forcedLoss("GE_VK_FORCE_DEVICE_LOST", "2");
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "no Vulkan device available";

    SamplerDesc sd{};
    sd.debugName = "PreRebuildSampler";
    const SamplerHandle before = dev->CreateSampler(sd);
    ASSERT_TRUE(before.IsValid());

    bool reachedAwaiting = false;
    for (int i = 0; i < 12 && !reachedAwaiting; ++i)
    {
        RunOneFrame(*dev);
        reachedAwaiting = (dev->GetDeviceHealth() == DeviceHealth::AwaitingReprovision);
    }
    ASSERT_TRUE(reachedAwaiting) << "injected loss should rebuild to AwaitingReprovision";
    dev->NotifyReprovisionComplete();

    // IsValid() is a plain id test — nothing re-stamps it when the owning
    // VkDevice dies, which is exactly why a cache guarded by IsValid() kept
    // handing out a corpse.
    EXPECT_TRUE(before.IsValid()) << "handle staleness is invisible to IsValid()";

    // A fresh sampler on the rebuilt device must be a different handle: if the
    // engine ever recycled the id, a stale-handle check by value would be unsound.
    const SamplerHandle after = dev->CreateSampler(sd);
    ASSERT_TRUE(after.IsValid());
    EXPECT_NE(after.id, before.id) << "post-rebuild sampler aliases the dead handle";

    dev->DestroySampler(after);
    dev->Shutdown();
}

// WHEN the cleanup runs, not merely THAT it runs. A cleanup exists to destroy the
// handles it cached, so it has to execute while those objects are still alive —
// i.e. inside the rebuild teardown, not after the replacement device is up. Routed
// through the post-rebuild re-provision seam instead, every assertion in
// RegistersOnBothDeathEvents still passes while each cleanup calls Destroy* on a
// generational slot the teardown already freed.
TEST(PerDeviceCacheCleanup, RunsWhileTheHandlesItOwnsAreStillAlive)
{
    ScopedEnvVar forcedLoss("GE_VK_FORCE_DEVICE_LOST", "2");
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "no Vulkan device available";

    TextureDesc td{};
    td.width = 4;
    td.height = 4;
    td.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
    td.usage = static_cast<uint32_t>(TextureUsage::ShaderResource);
    td.debugName = "CleanupOrderingProbe";
    const TextureHandle cached = dev->CreateTexture(td);
    ASSERT_TRUE(cached.IsValid());
    ASSERT_TRUE(dev->IsTextureAlive(cached)) << "probe must start alive";

    // Stand-in for a real per-device cache: what it observes about its own handle
    // at cleanup time is the whole point of the test.
    bool aliveAtCleanup = false;
    DeviceHealth healthAtCleanup = DeviceHealth::Healthy;
    std::vector<std::string> order;
    dev->RegisterPerDeviceCacheCleanup("Test.CleanupOrdering",
                                       [&](IDevice* d)
                                       {
                                           order.emplace_back("cleanup");
                                           aliveAtCleanup = d->IsTextureAlive(cached);
                                           healthAtCleanup = d->GetDeviceHealth();
                                       });
    // The other seam, for contrast: re-provision is meant to run after the new
    // device is up, and the two must not be interchangeable.
    dev->RegisterDeviceRebuiltCallback("Test.ReprovisionOrdering",
                                       [&](IDevice*) { order.emplace_back("reprovision"); });

    bool reachedAwaiting = false;
    for (int i = 0; i < 12 && !reachedAwaiting; ++i)
    {
        RunOneFrame(*dev);
        reachedAwaiting = (dev->GetDeviceHealth() == DeviceHealth::AwaitingReprovision);
    }
    ASSERT_TRUE(reachedAwaiting) << "injected loss should rebuild to AwaitingReprovision";

    // Guard, not a regression test: while both seams shared one callback list this
    // order held anyway, from registration order alone. It pins that they stay
    // distinct and correctly sequenced, and would not have caught the defect.
    ASSERT_EQ(order.size(), 2u) << "each seam must fire exactly once per rebuild";
    EXPECT_EQ(order[0], "cleanup") << "caches drop their handles before the device dies";
    EXPECT_EQ(order[1], "reprovision") << "re-provision runs after the new device is up";

    // These two are the discriminators — both fail if the cleanup is routed through
    // the post-rebuild seam.
    EXPECT_TRUE(aliveAtCleanup)
        << "the cleanup ran after its texture was already destroyed — any Destroy* it "
           "issues targets a freed generational slot";
    EXPECT_EQ(healthAtCleanup, DeviceHealth::Rebuilding)
        << "the cleanup must run inside the rebuild teardown; AwaitingReprovision here "
           "means it was routed through the post-rebuild re-provision seam";

    dev->NotifyReprovisionComplete();
    dev->Shutdown();
}

// Declared after every injecting test in this file. gtest runs a suite's tests in
// declaration order and suites in first-declaration order, so this runs last of
// the three. VulkanDevice re-reads these variables on each device creation
// (FaultInjectionFromEnv in Initialize), so one that outlives its test does not
// merely dirty the environment — it injects into every later device here.
TEST(DeviceRecoveryEnvHygiene, InjectedFaultsDoNotLeakToLaterTests)
{
    EXPECT_EQ(std::getenv("GE_VK_FORCE_DEVICE_LOST"), nullptr)
        << "GE_VK_FORCE_DEVICE_LOST outlived its test";
    EXPECT_EQ(std::getenv("GE_VK_FORCE_BEGINFRAME_TIMEOUT"), nullptr)
        << "GE_VK_FORCE_BEGINFRAME_TIMEOUT outlived its test";
    EXPECT_EQ(std::getenv("GE_VK_GPU_CHECKPOINTS"), nullptr)
        << "GE_VK_GPU_CHECKPOINTS outlived its test";
}

// --- Device-loss routing lint -------------------------------------------------
//
// OnDeviceLostObserved is the only caller of DeviceHealthState::TransitionToLost,
// and the only thing that logs the loss and dumps the GPU checkpoints. A site that
// observed VK_ERROR_DEVICE_LOST and handled it locally would therefore recover (or
// fail to) with no error log and no checkpoint dump at all — silent recovery, the
// failure mode this lint exists to stop.
//
// Source-scanning is deliberate: the invariant is "no unrouted site EXISTS", and no
// runtime test can establish that, because it can only reach sites it can provoke.
// The backend source directory is baked in by CMake; this is a developer lint, and
// nothing staged or shipped resolves a path this way.

#ifndef GE_VULKAN_BACKEND_SOURCE_DIR
#error "GE_VULKAN_BACKEND_SOURCE_DIR must be defined for the device-loss routing lint"
#endif

namespace
{

// A check must be answered by an observer call within this many lines. Every site
// in the backend today answers within five.
constexpr size_t kMaxLinesToObserver = 8;

// How far a result stays interesting after the line that bound it. Past the longest
// bind-to-routing distance in the backend, which is the present: 78 lines from the
// queue-locked lambda producing the result to its routed DEVICE_LOST check.
//
// The backend census is identical across the whole band 79..312 and changes at both
// ends, so what this number buys is margin, not the answer — but the margin is
// finite in both directions. Below 79 the present's own routing falls out of reach.
// Above 312 a comparison starts resolving to a binding of the same name in an
// unrelated function: DrainWindowTarget's waitRes sits exactly 313 lines after the
// waitRes of WaitForSpecificGraphicsFence, and reports a swallow that is not there.
constexpr size_t kResultLiveRange = 96;

// Bound on the lambda body a result may be produced in, so an unbalanced brace
// inside a string literal cannot run a body to the end of the file. The longest
// body producing a result today is the present lambda, at 41 lines.
constexpr size_t kMaxLambdaBodyLines = 200;

// A VkResult-returning method that hands a loss-capable result to its callers makes
// calling it loss-capable in turn, and that call may sit inside another such method.
// Rounds needed = the depth of that chain, which is one today.
constexpr size_t kMaxYieldRounds = 4;

struct DeviceLostScan
{
    std::vector<std::string> Unrouted; // "file:line  code" for each unanswered check
    std::set<std::string> Sites;       // the string literal each observer call names
    // Results of loss-capable calls tested only against VK_SUCCESS and never
    // routed, counted per enclosing function. These never spell the enum, so the
    // explicit matcher above cannot see them at all.
    std::map<std::string, size_t> SwallowedByFunction;
    std::vector<std::string> Swallowed; // "file:line  code", for the failure message
    // Loss-capable results handed onward whole (`return name;`). The loss is still
    // live in the value the caller receives, so these are answered where they land
    // and not here. Recorded, never silently dropped: an escape that stopped landing
    // anywhere would otherwise absorb a swallow without trace.
    std::map<std::string, size_t> EscapedByFunction;
    std::vector<std::string> Escaped; // "file:line  code", for the failure message
    // Every line that calls a Vulkan entry point which can return
    // VK_ERROR_DEVICE_LOST — the population the two censuses above partition. Held
    // to a pinned count so a new one cannot appear without being classified.
    std::vector<std::string> LossCapableCallSites;
    size_t FilesScanned = 0;
    size_t ChecksFound = 0;
};

bool IsIdentifierChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

// One scanned source file: comment-free text, and the brace arithmetic that text
// implies. The matchers read the text and the scope rules read the braces, so the
// two are produced together, from one pass that knows where the comments and the
// literals are.
struct ScannedFile
{
    std::string Name;
    std::vector<std::string> Code;
    // Net `{` minus `}` per line, counted outside string and character literals so a
    // format placeholder ("{}") cannot open a scope.
    std::vector<int> BraceDelta;
};

// Removes C++ comments, preserving line numbering.
//
// Every matcher below decides routing from text alone, so a checker that reads
// comments is a checker that can be told by a comment that a site is safe: a
// `/* ... OnDeviceLostObserved(...) ... */` next to a genuine swallow answers for
// it, and a site literal quoted in a comment keeps the observed-sites census whole
// while the real call is deleted. Prose is the one thing this lint must never
// accept as evidence, so it is removed before anything is matched.
//
// String and character literals are kept — the observed-site labels are read out of
// them — but they are tracked, so neither a `/*` nor a brace inside one is read as
// syntax. `//` inside a block comment, and `/*` inside a line comment, are likewise
// just text.
ScannedFile StripComments(std::string name, std::istream& in)
{
    enum class Lexeme
    {
        Code,
        BlockComment,
        StringLiteral,
        CharLiteral,
        RawString
    };

    ScannedFile file;
    file.Name = std::move(name);
    Lexeme state = Lexeme::Code;
    std::string rawTerminator;

    for (std::string line; std::getline(in, line);)
    {
        std::string kept;
        int delta = 0;
        for (size_t i = 0; i < line.size();)
        {
            const char c = line[i];
            const char next = i + 1 < line.size() ? line[i + 1] : '\0';

            if (state == Lexeme::BlockComment)
            {
                if (c == '*' && next == '/')
                {
                    state = Lexeme::Code;
                    i += 2;
                }
                else
                {
                    ++i;
                }
                continue;
            }
            if (state == Lexeme::RawString)
            {
                if (line.compare(i, rawTerminator.size(), rawTerminator) == 0)
                {
                    kept += rawTerminator;
                    i += rawTerminator.size();
                    state = Lexeme::Code;
                    continue;
                }
                kept += c;
                ++i;
                continue;
            }
            if (state == Lexeme::StringLiteral || state == Lexeme::CharLiteral)
            {
                kept += c;
                if (c == '\\' && next != '\0')
                {
                    kept += next;
                    i += 2;
                    continue;
                }
                if ((state == Lexeme::StringLiteral && c == '"') ||
                    (state == Lexeme::CharLiteral && c == '\''))
                    state = Lexeme::Code;
                ++i;
                continue;
            }

            if (c == '/' && next == '/')
                break;
            if (c == '/' && next == '*')
            {
                state = Lexeme::BlockComment;
                i += 2;
                continue;
            }
            if (c == '"')
            {
                // R"delim( ... )delim" ends only at its own delimiter, so nothing
                // inside one is syntax.
                const bool raw =
                    i > 0 && line[i - 1] == 'R' && (i == 1 || !IsIdentifierChar(line[i - 2]));
                const size_t body = raw ? line.find('(', i + 1) : std::string::npos;
                if (body != std::string::npos)
                {
                    rawTerminator = ")" + line.substr(i + 1, body - i - 1) + "\"";
                    state = Lexeme::RawString;
                }
                else
                {
                    state = Lexeme::StringLiteral;
                }
            }
            else if (c == '\'')
            {
                state = Lexeme::CharLiteral;
            }
            else if (c == '{')
            {
                ++delta;
            }
            else if (c == '}')
            {
                --delta;
            }
            kept += c;
            ++i;
        }

        file.Code.push_back(std::move(kept));
        file.BraceDelta.push_back(delta);
    }
    return file;
}

// A check tests a VkResult against the enum. An assignment (fault injection), a
// switch label (VkResultToString), or a bare argument (the escalation site passes
// the enum to the observer) is not a check and needs no answer.
bool IsDeviceLostCheck(const std::string& code)
{
    return code.find("== VK_ERROR_DEVICE_LOST") != std::string::npos ||
           code.find("VK_ERROR_DEVICE_LOST ==") != std::string::npos;
}

// What counts as answering a check. OnDeviceLostObserved is the observation itself;
// NoteDeviceLostOnWorkerThread is the job-thread hand-off to it, which the render
// thread performs at the next BeginFrame; classifyQueueSubmit is the render-graph
// submit path's local router, whose body carries its own routed device-lost check.
// All three reach the observer, so all three answer.
bool IsObserverHandoff(const std::string& code)
{
    return code.find("OnDeviceLostObserved(") != std::string::npos ||
           code.find("NoteDeviceLostOnWorkerThread(") != std::string::npos ||
           code.find("classifyQueueSubmit(") != std::string::npos;
}

// Calls that can return VK_ERROR_DEVICE_LOST. Testing one of these results against
// VK_SUCCESS is a device-loss check that never spells the enum — invisible to
// IsDeviceLostCheck, and precisely how three submit sites swallowed a loss.
// queueSubmit2 is the backend's local dispatch alias for vkQueueSubmit2.
constexpr std::string_view kLossCapableCalls[] = {
    "vkQueueSubmit(",         "vkQueueSubmit2(",  "queueSubmit2(",
    "vkQueuePresentKHR(",     "vkWaitForFences(", "vkGetFenceStatus(",
    "vkAcquireNextImageKHR(", "vkAcquireNextImage2KHR(", "vkQueueBindSparse(",
};

// A call to `name`: the whole identifier, immediately followed by its argument list.
bool CallsFunction(const std::string& code, const std::string& name)
{
    for (size_t at = code.find(name); at != std::string::npos; at = code.find(name, at + 1))
    {
        const size_t end = at + name.size();
        if ((at == 0 || !IsIdentifierChar(code[at - 1])) && end < code.size() && code[end] == '(')
            return true;
    }
    return false;
}

// Resolving or declaring the dispatch pointer is not a call through it. `yielding`
// names methods that hand a loss-capable result to their callers (see
// MethodsThatYieldALossCapableResult): calling one produces a live loss-capable
// result exactly as the Vulkan entry points above do.
bool IsLossCapableCall(const std::string& code, const std::set<std::string>& yielding)
{
    if (code.find("PFN_") != std::string::npos || code.find("GetDeviceProcAddr") != std::string::npos)
        return false;
    for (const std::string_view call : kLossCapableCalls)
    {
        if (code.find(call) != std::string::npos)
            return true;
    }
    for (const std::string& call : yielding)
    {
        if (CallsFunction(code, call))
            return true;
    }
    return false;
}

bool IsSuccessComparison(const std::string& code)
{
    return code.find("!= VK_SUCCESS") != std::string::npos ||
           code.find("== VK_SUCCESS") != std::string::npos;
}

// Whole-identifier match, so `res` does not match `resource`.
bool MentionsIdentifier(const std::string& code, const std::string& name)
{
    for (size_t at = code.find(name); at != std::string::npos; at = code.find(name, at + 1))
    {
        const bool leftOk = at == 0 || !IsIdentifierChar(code[at - 1]);
        const size_t end = at + name.size();
        const bool rightOk = end >= code.size() || !IsIdentifierChar(code[end]);
        if (leftOk && rightOk)
            return true;
    }
    return false;
}

// The name a loss-capable call binds its result to, or empty for an inline test
// (`if (vkQueueSubmit(...) == VK_SUCCESS)`) that never names the result at all.
std::string BoundResultName(const std::string& code)
{
    static const std::regex declaration{R"((?:const\s+)?VkResult\s+(\w+)\s*=)"};
    static const std::regex assignment{R"(^\s*(\w+)\s*=[^=])"};
    std::smatch m;
    if (std::regex_search(code, m, declaration))
        return m[1].str();
    if (std::regex_search(code, m, assignment))
        return m[1].str();
    return {};
}

// Names the VulkanDevice method a definition line opens, or empty for other lines.
std::string DefinedMethodName(const std::string& code)
{
    static const std::regex definition{R"(^\w[\w:<>,&*\s]*\bVulkanDevice::(\w+)\s*\()"};
    std::smatch m;
    return std::regex_search(code, m, definition) ? m[1].str() : std::string{};
}

// Names the VulkanDevice method a VkResult-returning definition line opens. Only
// such a method can hand a loss-capable result out to its callers.
std::string DefinedVkResultMethodName(const std::string& code)
{
    static const std::regex definition{R"(^VkResult\s+[\w:]*\bVulkanDevice::(\w+)\s*\()"};
    std::smatch m;
    return std::regex_search(code, m, definition) ? m[1].str() : std::string{};
}

// A line that opens a lambda body, as `... ([&] {` / `([this](int x) -> VkResult {`.
// The body must actually open here, which is what separates a capture list from an
// array subscript (`&m_PresentReadySemaphores[i]`).
bool OpensLambdaBody(const std::string& code)
{
    static const std::regex introducer{R"(\[(?:&|=|this)?\]\s*(?:\([^)]*\))?\s*(?:->[^{]*)?\{\s*$)"};
    return std::regex_search(code, introducer);
}

// Last line of the lambda body opened on `at`, by brace depth. Bounded, so a file
// whose braces do not balance cannot run the body away to the end of the file.
size_t LambdaBodyEnd(const ScannedFile& file, size_t at)
{
    const size_t last = std::min(file.Code.size(), at + kMaxLambdaBodyLines);
    int depth = 0;
    for (size_t i = at; i < last; ++i)
    {
        depth += file.BraceDelta[i];
        if (i > at && depth <= 0)
            return i;
    }
    return last == 0 ? 0 : last - 1;
}

// The line on which the block holding a binding closes. A result cannot be returned
// after its own scope has ended, so a return past this line answers for something
// else. Bounded like LambdaBodyEnd.
size_t BindingScopeEnd(const ScannedFile& file, size_t boundAt)
{
    const size_t last = std::min(file.Code.size(), boundAt + kMaxLambdaBodyLines);
    int depth = 0;
    for (size_t i = boundAt; i < last; ++i)
    {
        depth += file.BraceDelta[i];
        if (depth < 0)
            return i;
    }
    return last;
}

// `return name;` hands the result on whole, so the loss is still live in the value
// the caller receives. `return name == VK_SUCCESS;` is the opposite: it reduces the
// loss to a bool, which is the very thing this lint hunts.
bool ReturnsResultIntact(const std::string& code, const std::string& name)
{
    static const std::regex bare{R"(^\s*return\s+(\w+)\s*;\s*$)"};
    std::smatch m;
    return std::regex_search(code, m, bare) && m[1].str() == name;
}

// The lines over which a binding of `name` at `boundAt` can be answered. Re-binding
// the name ends the window: whatever answers after it answers the NEW call. Without
// this the window is only sound while it happens to stop short of the next binding's
// routing, which is a line-count coincidence, not an invariant.
size_t ResultWindowEnd(size_t lineCount, const std::vector<size_t>& bindings, size_t boundAt)
{
    size_t last = std::min(lineCount, boundAt + kResultLiveRange);
    if (const auto next = std::upper_bound(bindings.begin(), bindings.end(), boundAt);
        next != bindings.end())
    {
        last = std::min(last, *next);
    }
    return last;
}

// A loss-capable result is routed if an observer call naming it, or an explicit
// device-lost check on it, appears in its window. The success branch of a result
// whose failure branch routes is not an escape. Both matches require the name: over
// a range this long, an anonymous observer match would let one result borrow the
// routing of its neighbour (the present-complete marker submit sits inside the
// present result's range).
bool ResultIsRouted(const std::vector<std::string>& code, const std::string& name, size_t boundAt,
                    size_t windowEnd)
{
    for (size_t k = boundAt; k < windowEnd; ++k)
    {
        if ((IsObserverHandoff(code[k]) || IsDeviceLostCheck(code[k])) &&
            MentionsIdentifier(code[k], name))
            return true;
    }
    return false;
}

// An escape needs EVERY return that can carry the result to hand it on whole. This
// is the only rule that removes a site from the swallow census, so one `return
// name;` is not enough on its own: a sibling `return VK_SUCCESS;` on the failure
// path destroys the loss precisely where it exists, and the escape would license
// exactly the swallow this lint hunts.
//
// The result's own scope bounds the search — a return after the enclosing block
// closes answers for a different value. Returns inside a nested lambda exit that
// lambda rather than this scope, including the lambda a result is produced in, so
// those are skipped.
bool ResultEscapes(const ScannedFile& file, const std::string& name, size_t boundAt,
                   size_t windowEnd)
{
    const std::vector<std::string>& code = file.Code;
    const size_t end = std::min(windowEnd, BindingScopeEnd(file, boundAt));
    bool handedOn = false;
    for (size_t k = boundAt; k < end; ++k)
    {
        if (OpensLambdaBody(code[k]))
        {
            k = LambdaBodyEnd(file, k);
            continue;
        }
        if (!MentionsIdentifier(code[k], "return"))
            continue;
        if (!ReturnsResultIntact(code[k], name))
            return false;
        handedOn = true;
    }
    return handedOn;
}

// Every line each loss-capable result name was bound on, ascending. A check resolves
// to the NEAREST PRECEDING binding of its name: names like submitRes are re-bound
// function after function, and resolving to any other binding judges this check
// against some other call's routing.
std::map<std::string, std::vector<size_t>> CollectResultBindings(
    const ScannedFile& file, const std::set<std::string>& yielding)
{
    const std::vector<std::string>& code = file.Code;
    std::map<std::string, std::vector<size_t>> bindings;
    for (size_t i = 0; i < code.size(); ++i)
    {
        const std::string bound = BoundResultName(code[i]);
        if (bound.empty())
            continue;
        if (IsLossCapableCall(code[i], yielding))
        {
            bindings[bound].push_back(i);
            continue;
        }
        // `VkResult r = context.WithQueueLocked([&] { return vkQueueSubmit(...); });`
        // The lambda is an argument of the call being bound, so it runs here and its
        // value IS r. Without this the site is invisible in both directions: the line
        // that calls binds no name, and the line that binds does not call.
        if (!OpensLambdaBody(code[i]))
            continue;
        const size_t bodyEnd = LambdaBodyEnd(file, i);
        for (size_t k = i + 1; k <= bodyEnd && k < code.size(); ++k)
        {
            if (IsLossCapableCall(code[k], yielding))
            {
                bindings[bound].push_back(i);
                break;
            }
        }
    }
    return bindings;
}

// VulkanDevice methods that hand a loss-capable result to their callers. Calling one
// is itself a loss-capable call, which is what keeps a submit visible once it moves
// out of its callers and behind a VkResult-returning helper: the helper's own check
// is not the answer, the call sites' checks are.
std::set<std::string> MethodsThatYieldALossCapableResult(const std::vector<ScannedFile>& files)
{
    std::set<std::string> yielding;
    for (size_t round = 0; round < kMaxYieldRounds; ++round)
    {
        const size_t before = yielding.size();
        for (const ScannedFile& file : files)
        {
            const std::vector<std::string>& code = file.Code;
            std::vector<std::string> vkResultMethod(code.size());
            std::string current;
            for (size_t i = 0; i < code.size(); ++i)
            {
                if (std::string named = DefinedVkResultMethodName(code[i]); !named.empty())
                    current = std::move(named);
                else if (!DefinedMethodName(code[i]).empty())
                    current.clear(); // a method returning anything else cannot yield one
                vkResultMethod[i] = current;
            }

            for (const auto& [name, bindings] : CollectResultBindings(file, yielding))
            {
                for (const size_t boundAt : bindings)
                {
                    if (vkResultMethod[boundAt].empty())
                        continue;
                    const size_t end = ResultWindowEnd(code.size(), bindings, boundAt);
                    if (ResultEscapes(file, name, boundAt, end))
                        yielding.insert(vkResultMethod[boundAt]);
                }
            }
        }
        if (yielding.size() == before)
            break;
    }
    return yielding;
}

// A definition or a header declaration names a method immediately after its return
// type; a call never does. Without this a yielding method's own signature reads as a
// call to itself, in both the definition and the header.
bool DeclaresVkResultFunction(const std::string& code, const std::string& name)
{
    static constexpr std::string_view kReturnType = "VkResult";
    for (size_t at = code.find(name); at != std::string::npos; at = code.find(name, at + 1))
    {
        const size_t end = at + name.size();
        if ((at != 0 && IsIdentifierChar(code[at - 1])) || end >= code.size() || code[end] != '(')
            continue;
        size_t before = at;
        while (before > 0 && (IsIdentifierChar(code[before - 1]) || code[before - 1] == ':'))
            --before;
        while (before > 0 && std::isspace(static_cast<unsigned char>(code[before - 1])))
            --before;
        if (before < kReturnType.size())
            continue;
        const size_t typeAt = before - kReturnType.size();
        if (code.compare(typeAt, kReturnType.size(), kReturnType) == 0 &&
            (typeAt == 0 || !IsIdentifierChar(code[typeAt - 1])))
            return true;
    }
    return false;
}

// Where a loss-capable result is produced: a Vulkan entry point, or a call to a
// method that hands one back. This is the population the swallow and escape censuses
// partition, so it counts calls exactly as those two do. Counting only the entry
// points leaves a discarded result from a yielding helper in no census at all — not
// a swallow, because nothing compares it; not an escape, because nothing returns it;
// and not a call site either, which is the arithmetic the census exists to close.
bool IsLossCapableCallSite(const std::string& code, const std::set<std::string>& yielding)
{
    if (IsLossCapableCall(code, {}))
        return true;
    for (const std::string& name : yielding)
    {
        if (CallsFunction(code, name) && !DeclaresVkResultFunction(code, name))
            return true;
    }
    return false;
}

std::vector<ScannedFile> LoadStrippedSources(const std::filesystem::path& dir)
{
    namespace fs = std::filesystem;
    std::vector<ScannedFile> files;
    for (const auto& entry : fs::directory_iterator(dir))
    {
        const fs::path& p = entry.path();
        if (!entry.is_regular_file() || (p.extension() != ".cpp" && p.extension() != ".h"))
            continue;

        std::ifstream in(p);
        if (!in.is_open())
            continue;

        files.push_back(StripComments(p.filename().string(), in));
    }
    return files;
}

std::string ObservedSiteLabel(const std::string& code)
{
    const size_t call = code.find("OnDeviceLostObserved(\"");
    if (call == std::string::npos)
        return {};
    const size_t open = code.find('"', call);
    const size_t close = code.find('"', open + 1);
    return close == std::string::npos ? std::string{} : code.substr(open + 1, close - open - 1);
}

DeviceLostScan ScanForDeviceLostRouting(const std::filesystem::path& dir)
{
    DeviceLostScan scan;

    const std::vector<ScannedFile> files = LoadStrippedSources(dir);
    scan.FilesScanned = files.size();

    // Which methods hand a loss-capable result onward has to be settled across the
    // whole directory before any file is judged: the method that yields and the call
    // site that must answer for it need not sit in the same file.
    const std::set<std::string> yielding = MethodsThatYieldALossCapableResult(files);

    for (const ScannedFile& file : files)
    {
        const std::vector<std::string>& code = file.Code;
        const std::map<std::string, std::vector<size_t>> resultBindings =
            CollectResultBindings(file, yielding);

        std::vector<std::string> enclosing(code.size());
        std::string current = "<file scope>";
        for (size_t i = 0; i < code.size(); ++i)
        {
            if (std::string method = DefinedMethodName(code[i]); !method.empty())
                current = std::move(method);
            enclosing[i] = current;
        }

        // Bindings whose result leaves this scope intact. Judged once per binding
        // rather than per check, so two comparisons on one escaping result do not
        // report the escape twice.
        std::set<size_t> escaping;
        for (const auto& [name, bindings] : resultBindings)
        {
            for (const size_t boundAt : bindings)
            {
                const size_t end = ResultWindowEnd(code.size(), bindings, boundAt);
                if (!ResultIsRouted(code, name, boundAt, end) &&
                    ResultEscapes(file, name, boundAt, end))
                    escaping.insert(boundAt);
            }
        }
        for (const size_t at : escaping)
        {
            ++scan.EscapedByFunction[enclosing[at]];
            scan.Escaped.push_back(file.Name + ":" + std::to_string(at + 1) + "  " + code[at]);
        }

        for (size_t i = 0; i < code.size(); ++i)
        {
            if (IsLossCapableCallSite(code[i], yielding))
                scan.LossCapableCallSites.push_back(file.Name + ":" + std::to_string(i + 1) + "  " +
                                                    code[i]);

            if (IsSuccessComparison(code[i]))
            {
                bool swallowed = false;
                if (IsLossCapableCall(code[i], yielding) && BoundResultName(code[i]).empty())
                {
                    // Inline test of an unnamed result: nothing else can ever route
                    // it, so only an observer in the window answers.
                    swallowed = true;
                    for (size_t j = i; j < code.size() && j <= i + kMaxLinesToObserver; ++j)
                    {
                        if (IsObserverHandoff(code[j]))
                        {
                            swallowed = false;
                            break;
                        }
                    }
                }
                else
                {
                    for (const auto& [name, bindings] : resultBindings)
                    {
                        if (!MentionsIdentifier(code[i], name))
                            continue;
                        // Nearest preceding binding of this name, if it is close
                        // enough for the check to belong to it.
                        const auto after = std::upper_bound(bindings.begin(), bindings.end(), i);
                        if (after == bindings.begin())
                            continue;
                        const size_t boundAt = *(after - 1);
                        if (i - boundAt > kResultLiveRange)
                            continue;
                        // An escaping result is answered where it lands, not here.
                        if (escaping.count(boundAt) != 0)
                            break;
                        const size_t end = ResultWindowEnd(code.size(), bindings, boundAt);
                        swallowed = !ResultIsRouted(code, name, boundAt, end);
                        break;
                    }
                }
                if (swallowed)
                {
                    ++scan.SwallowedByFunction[enclosing[i]];
                    scan.Swallowed.push_back(file.Name + ":" + std::to_string(i + 1) + "  " + code[i]);
                }
            }

            if (std::string label = ObservedSiteLabel(code[i]); !label.empty())
                scan.Sites.insert(std::move(label));

            if (!IsDeviceLostCheck(code[i]))
                continue;
            ++scan.ChecksFound;

            bool routed = false;
            for (size_t j = i; j < code.size() && j <= i + kMaxLinesToObserver; ++j)
            {
                // A later check owns everything from its own line on, so this one
                // cannot be answered by that check's observer call. Without this an
                // unrouted site silently borrows the routing of the next site within
                // the window, and the lint reports green on a real escape. Tested
                // before the observer match so a single line that both checks and
                // observes answers only itself.
                if (j > i && IsDeviceLostCheck(code[j]))
                {
                    break;
                }
                if (IsObserverHandoff(code[j]))
                {
                    routed = true;
                    break;
                }
            }
            if (!routed)
                scan.Unrouted.push_back(file.Name + ":" + std::to_string(i + 1) + "  " + code[i]);
        }
    }
    return scan;
}

} // namespace

// Verify the instrument before trusting its reading: a lint that cannot fail is
// indistinguishable from one that passes. Feed the scanner a routed site, an
// unrouted one, and a comment, and require it to separate them.
TEST(DeviceLossRouting, ScannerFlagsUnroutedSitesAndOnlyThose)
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "ge_devloss_routing_lint_selftest";
    std::error_code ec;
    fs::remove_all(dir, ec);
    ASSERT_TRUE(fs::create_directories(dir, ec)) << ec.message();

    {
        std::ofstream routed(dir / "Routed.cpp");
        ASSERT_TRUE(routed.is_open());
        routed << "void A() {\n"
                  "    if (result == VK_ERROR_DEVICE_LOST)\n"
                  "    {\n"
                  "        OnDeviceLostObserved(\"a routed site\", result);\n"
                  "    }\n"
                  "}\n";
    }
    {
        std::ofstream unrouted(dir / "Unrouted.cpp");
        ASSERT_TRUE(unrouted.is_open());
        unrouted << "void B() {\n"
                    "    // VK_ERROR_DEVICE_LOST in prose must not count as a check.\n"
                    "    if (result == VK_ERROR_DEVICE_LOST)\n"
                    "    {\n"
                    "        RecreateEverythingQuietly();\n"
                    "    }\n"
                    "}\n";
    }

    const DeviceLostScan scan = ScanForDeviceLostRouting(dir);
    EXPECT_EQ(scan.FilesScanned, 2u);
    EXPECT_EQ(scan.ChecksFound, 2u) << "the comment line must not be counted as a check";
    EXPECT_EQ(scan.Sites, (std::set<std::string>{"a routed site"}));
    ASSERT_EQ(scan.Unrouted.size(), 1u) << "exactly the unrouted site must be flagged";
    EXPECT_NE(scan.Unrouted[0].find("Unrouted.cpp:3"), std::string::npos) << scan.Unrouted[0];

    fs::remove_all(dir, ec);
}

// The forward scan must not let an unrouted check be answered by the NEXT check's
// observer call. Two sites inside one kMaxLinesToObserver window is the shape that
// occurs naturally (a queue submit followed by its fence wait), so an unrouted
// first site would have been reported green.
TEST(DeviceLossRouting, ScannerDoesNotLetOneSiteBorrowTheNextSitesRouting)
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "ge_devloss_routing_lint_shadowtest";
    std::error_code ec;
    fs::remove_all(dir, ec);
    ASSERT_TRUE(fs::create_directories(dir, ec)) << ec.message();

    {
        std::ofstream shadowed(dir / "Shadowed.cpp");
        ASSERT_TRUE(shadowed.is_open());
        shadowed << "void A() {\n"
                    "    if (submitResult == VK_ERROR_DEVICE_LOST)\n" // line 2: unrouted
                    "    {\n"
                    "        RecoverQuietly();\n"
                    "    }\n"
                    "    if (waitResult == VK_ERROR_DEVICE_LOST)\n" // line 6: routed
                    "    {\n"
                    "        OnDeviceLostObserved(\"the neighbour\", waitResult);\n"
                    "    }\n"
                    "}\n";
    }

    const DeviceLostScan scan = ScanForDeviceLostRouting(dir);
    EXPECT_EQ(scan.ChecksFound, 2u);
    ASSERT_EQ(scan.Unrouted.size(), 1u)
        << "the first check must be flagged even though the second one's observer is "
           "within the scan window";
    EXPECT_NE(scan.Unrouted[0].find("Shadowed.cpp:2"), std::string::npos) << scan.Unrouted[0];

    fs::remove_all(dir, ec);
}

// A single line that both checks and observes answers itself — the shadow fix must
// not break the compact form.
TEST(DeviceLossRouting, ScannerAcceptsACheckAndObserverOnOneLine)
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "ge_devloss_routing_lint_onelinetest";
    std::error_code ec;
    fs::remove_all(dir, ec);
    ASSERT_TRUE(fs::create_directories(dir, ec)) << ec.message();

    {
        std::ofstream compact(dir / "Compact.cpp");
        ASSERT_TRUE(compact.is_open());
        compact << "void A() {\n"
                   "    if (r == VK_ERROR_DEVICE_LOST) OnDeviceLostObserved(\"compact\", r);\n"
                   "    if (s == VK_ERROR_DEVICE_LOST) OnDeviceLostObserved(\"compact two\", s);\n"
                   "}\n";
    }

    const DeviceLostScan scan = ScanForDeviceLostRouting(dir);
    EXPECT_EQ(scan.ChecksFound, 2u);
    EXPECT_TRUE(scan.Unrouted.empty()) << "a line that observes its own check is routed";
    EXPECT_EQ(scan.Sites, (std::set<std::string>{"compact", "compact two"}));

    fs::remove_all(dir, ec);
}

// The job-thread hand-off answers a check too: NoteDeviceLostOnWorkerThread records
// the loss and BeginFrame performs it on the render thread.
TEST(DeviceLossRouting, ScannerAcceptsTheWorkerThreadHandoffAsRouting)
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "ge_devloss_routing_lint_handofftest";
    std::error_code ec;
    fs::remove_all(dir, ec);
    ASSERT_TRUE(fs::create_directories(dir, ec)) << ec.message();

    {
        std::ofstream handoff(dir / "Handoff.cpp");
        ASSERT_TRUE(handoff.is_open());
        handoff << "void A() {\n"
                   "    if (result == VK_ERROR_DEVICE_LOST)\n"
                   "    {\n"
                   "        NoteDeviceLostOnWorkerThread();\n"
                   "    }\n"
                   "}\n";
    }

    const DeviceLostScan scan = ScanForDeviceLostRouting(dir);
    EXPECT_EQ(scan.ChecksFound, 1u);
    EXPECT_TRUE(scan.Unrouted.empty()) << "the deferred hand-off reaches the observer";
    // The hand-off names no site; the drain that performs it does.
    EXPECT_TRUE(scan.Sites.empty());

    fs::remove_all(dir, ec);
}

// The blind spot that let three real submit sites swallow a loss: a check written
// against VK_SUCCESS never spells VK_ERROR_DEVICE_LOST, so the explicit matcher
// reports green on it. Both forms below are unrouted and must be caught.
TEST(DeviceLossRouting, ScannerFlagsALossCapableResultTestedOnlyAgainstVkSuccess)
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "ge_devloss_routing_lint_successtest";
    std::error_code ec;
    fs::remove_all(dir, ec);
    ASSERT_TRUE(fs::create_directories(dir, ec)) << ec.message();

    {
        // The SubmitTextureUploads shape: named result, tested for failure, dropped.
        std::ofstream named(dir / "Named.cpp");
        ASSERT_TRUE(named.is_open());
        named << "void GameEngine::Rendering::VulkanDevice::SwallowsNamed()\n"
                 "{\n"
                 "    VkResult result = vkQueueSubmit(m_TransferQueue, 1, &si, VK_NULL_HANDLE);\n"
                 "    if (result != VK_SUCCESS)\n"
                 "    {\n"
                 "        Logger::Log::Error(\"submit failed\");\n"
                 "    }\n"
                 "}\n";
    }
    {
        // The ArmComputeTransferFences shape: inline test of an unnamed result.
        std::ofstream inlined(dir / "Inlined.cpp");
        ASSERT_TRUE(inlined.is_open());
        inlined << "void GameEngine::Rendering::VulkanDevice::SwallowsInline()\n"
                   "{\n"
                   "    if (vkQueueSubmit(cQueue, 1, &emptySubmit, fence) == VK_SUCCESS)\n"
                   "        armed = true;\n"
                   "}\n";
    }

    const DeviceLostScan scan = ScanForDeviceLostRouting(dir);
    EXPECT_TRUE(scan.Unrouted.empty()) << "neither site spells the enum, so the explicit matcher sees nothing";
    ASSERT_EQ(scan.Swallowed.size(), 2u) << "both VK_SUCCESS-only sites must be flagged";
    EXPECT_EQ(scan.SwallowedByFunction.at("SwallowsNamed"), 1u);
    EXPECT_EQ(scan.SwallowedByFunction.at("SwallowsInline"), 1u);

    fs::remove_all(dir, ec);
}

// The success branch of a result whose failure branch DOES route is not an escape,
// or every routed site in the backend would report twice.
TEST(DeviceLossRouting, ScannerAcceptsASuccessBranchWhoseResultIsRoutedElsewhere)
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "ge_devloss_routing_lint_bothbranches";
    std::error_code ec;
    fs::remove_all(dir, ec);
    ASSERT_TRUE(fs::create_directories(dir, ec)) << ec.message();

    {
        std::ofstream routed(dir / "BothBranches.cpp");
        ASSERT_TRUE(routed.is_open());
        routed << "void GameEngine::Rendering::VulkanDevice::RoutesItsFailure()\n"
                  "{\n"
                  "    const VkResult status = vkGetFenceStatus(m_Device, fence);\n"
                  "    if (status == VK_SUCCESS)\n"
                  "    {\n"
                  "        return true;\n"
                  "    }\n"
                  "    if (status == VK_ERROR_DEVICE_LOST)\n"
                  "    {\n"
                  "        OnDeviceLostObserved(\"a routed site\", status, true);\n"
                  "    }\n"
                  "}\n";
    }

    const DeviceLostScan scan = ScanForDeviceLostRouting(dir);
    EXPECT_TRUE(scan.Unrouted.empty());
    EXPECT_TRUE(scan.Swallowed.empty())
        << "the VK_SUCCESS branch is answered by the device-lost branch on the same result";

    fs::remove_all(dir, ec);
}

// Result names are re-bound function after function (submitRes appears four times
// in VulkanDevice.cpp). A check must be judged against the call it actually
// belongs to, so resolution takes the nearest PRECEDING binding. Keeping only the
// first binding instead leaves every later re-use out of range and therefore never
// judged at all — silently skipped, not passed.
TEST(DeviceLossRouting, ScannerJudgesAReusedResultNameAgainstItsNearestBinding)
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "ge_devloss_routing_lint_reusetest";
    std::error_code ec;
    fs::remove_all(dir, ec);
    ASSERT_TRUE(fs::create_directories(dir, ec)) << ec.message();

    {
        std::ofstream reused(dir / "Reused.cpp");
        ASSERT_TRUE(reused.is_open());
        reused << "void GameEngine::Rendering::VulkanDevice::RoutesTheFirst()\n"
                  "{\n"
                  "    const VkResult submitRes = vkQueueSubmit(q, 1, &si, f);\n"
                  "    if (submitRes == VK_ERROR_DEVICE_LOST)\n"
                  "    {\n"
                  "        OnDeviceLostObserved(\"the first submit\", submitRes, true);\n"
                  "    }\n"
                  "}\n";
        // Blank filler, so the second binding sits clear of the first one's live
        // range and only nearest-binding resolution can reach it.
        for (size_t i = 0; i < kResultLiveRange + 8; ++i)
            reused << "\n";
        reused << "void GameEngine::Rendering::VulkanDevice::SwallowsTheSecond()\n"
                  "{\n"
                  "    const VkResult submitRes = vkQueueSubmit(q, 1, &si, f);\n"
                  "    if (submitRes != VK_SUCCESS)\n"
                  "    {\n"
                  "        Logger::Log::Error(\"submit failed\");\n"
                  "    }\n"
                  "}\n";
    }

    const DeviceLostScan scan = ScanForDeviceLostRouting(dir);
    std::string report;
    for (const std::string& site : scan.Swallowed)
        report += "\n  " + site;
    ASSERT_EQ(scan.Swallowed.size(), 1u)
        << "the re-used name's second binding must be judged on its own routing:" << report;
    EXPECT_EQ(scan.SwallowedByFunction.at("SwallowsTheSecond"), 1u);
    EXPECT_EQ(scan.SwallowedByFunction.count("RoutesTheFirst"), 0u);

    fs::remove_all(dir, ec);
}

// Re-binding a name ends the previous binding's routing window. Otherwise an
// unrouted call borrows the routing of the NEXT call through the same variable,
// and whether it does is decided by how many lines happen to separate them.
TEST(DeviceLossRouting, ScannerDoesNotLetARebindingsRoutingAnswerTheEarlierCall)
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "ge_devloss_routing_lint_rebindtest";
    std::error_code ec;
    fs::remove_all(dir, ec);
    ASSERT_TRUE(fs::create_directories(dir, ec)) << ec.message();

    {
        std::ofstream rebound(dir / "Rebound.cpp");
        ASSERT_TRUE(rebound.is_open());
        rebound << "void GameEngine::Rendering::VulkanDevice::SwallowsThenRoutes()\n"
                   "{\n"
                   "    VkResult submitRes = vkQueueSubmit(qa, 1, &si, fa);\n" // line 3
                   "    if (submitRes != VK_SUCCESS)\n"                        // line 4: unrouted
                   "    {\n"
                   "        Logger::Log::Error(\"arm failed\");\n"
                   "    }\n"
                   "    submitRes = vkQueueSubmit(qb, 1, &si, fb);\n" // line 8: re-binding
                   "    if (submitRes == VK_ERROR_DEVICE_LOST)\n"
                   "    {\n"
                   "        OnDeviceLostObserved(\"the second submit\", submitRes, true);\n"
                   "    }\n"
                   "}\n";
    }

    const DeviceLostScan scan = ScanForDeviceLostRouting(dir);
    std::string report;
    for (const std::string& site : scan.Swallowed)
        report += "\n  " + site;
    ASSERT_EQ(scan.Swallowed.size(), 1u)
        << "the first submit is answered by nothing before its result is overwritten:" << report;
    EXPECT_NE(scan.Swallowed[0].find("Rebound.cpp:4"), std::string::npos) << scan.Swallowed[0];
    EXPECT_TRUE(scan.Unrouted.empty()) << "the second submit's device-lost check is routed";

    fs::remove_all(dir, ec);
}

// A submit can sit inside a lambda that runs where it is written, with the enclosing
// statement binding its value: the line that calls names nothing, and the line that
// names does not call. Left unhandled the whole site is invisible — not routed, not
// swallowed, simply absent — which is the one reading a ratchet must never produce,
// because it looks identical to a site that was fixed.
TEST(DeviceLossRouting, ScannerSeesASubmitProducedInsideABoundLambda)
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "ge_devloss_routing_lint_lambdabind";
    std::error_code ec;
    fs::remove_all(dir, ec);
    ASSERT_TRUE(fs::create_directories(dir, ec)) << ec.message();

    {
        std::ofstream bound(dir / "Bound.cpp");
        ASSERT_TRUE(bound.is_open());
        bound << "void GameEngine::Rendering::VulkanDevice::ArmsAndDiscards()\n"
                 "{\n"
                 "    const VkResult result = context.WithQueueLocked([&] {\n"
                 "        return vkQueueSubmit(context.Queue(), 1, &submit, fence);\n"
                 "    });\n"
                 "    if (result == VK_SUCCESS)\n"
                 "    {\n"
                 "        queueFrame.fenceArmed = true;\n"
                 "    }\n"
                 "}\n";
    }

    const DeviceLostScan scan = ScanForDeviceLostRouting(dir);
    std::string report;
    for (const std::string& site : scan.Swallowed)
        report += "\n  " + site;
    ASSERT_EQ(scan.Swallowed.size(), 1u)
        << "the submit inside the lambda binds the result the enclosing line names:" << report;
    EXPECT_EQ(scan.SwallowedByFunction.at("ArmsAndDiscards"), 1u);

    fs::remove_all(dir, ec);
}

// `return name;` hands the whole result to the caller, so the loss is still live in
// the value the caller receives and this scope is not where it is answered. The call
// sites are — which is why a VkResult-returning method that does this counts as a
// loss-capable call in its own right. Judging such a helper locally would report one
// swallow at the helper and none at the three call sites that actually discard it.
TEST(DeviceLossRouting, ScannerJudgesAYieldedResultAtItsCallSitesNotItsOwnCheck)
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "ge_devloss_routing_lint_yield";
    std::error_code ec;
    fs::remove_all(dir, ec);
    ASSERT_TRUE(fs::create_directories(dir, ec)) << ec.message();

    {
        std::ofstream yielded(dir / "Yielded.cpp");
        ASSERT_TRUE(yielded.is_open());
        yielded << "VkResult GameEngine::Rendering::VulkanDevice::ArmGraphicsFrameFence()\n"
                   "{\n"
                   "    const VkResult result = context.WithQueueLocked([&] {\n"
                   "        return vkQueueSubmit(context.Queue(), 1, &submit, fence);\n"
                   "    });\n"
                   "    if (result == VK_SUCCESS)\n"
                   "    {\n"
                   "        queueFrame.fenceArmed = true;\n"
                   "    }\n"
                   "    return result;\n"
                   "}\n"
                   "\n"
                   "void GameEngine::Rendering::VulkanDevice::DiscardsTheLoss()\n"
                   "{\n"
                   "    if (ArmGraphicsFrameFence() == VK_SUCCESS)\n"
                   "    {\n"
                   "        m_DeviceKnownIdle = false;\n"
                   "    }\n"
                   "}\n";
    }

    const DeviceLostScan scan = ScanForDeviceLostRouting(dir);
    std::string report;
    for (const std::string& site : scan.Swallowed)
        report += "\n  " + site;
    ASSERT_EQ(scan.Swallowed.size(), 1u) << "the call site discards the loss, the helper passes it on:"
                                         << report;
    EXPECT_EQ(scan.SwallowedByFunction.at("DiscardsTheLoss"), 1u);
    EXPECT_EQ(scan.SwallowedByFunction.count("ArmGraphicsFrameFence"), 0u);
    EXPECT_EQ(scan.EscapedByFunction.at("ArmGraphicsFrameFence"), 1u)
        << "an escape is recorded, never silently dropped";

    fs::remove_all(dir, ec);
}

// The counterpart: a yielded result whose call site routes it is not swallowed
// anywhere. Without this, teaching the scanner to see submits behind a helper would
// report the compute/transfer fence arms as swallowing when they route.
TEST(DeviceLossRouting, ScannerAcceptsRoutingAtTheCallSiteOfAYieldedResult)
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "ge_devloss_routing_lint_yieldrouted";
    std::error_code ec;
    fs::remove_all(dir, ec);
    ASSERT_TRUE(fs::create_directories(dir, ec)) << ec.message();

    {
        std::ofstream routed(dir / "YieldRouted.cpp");
        ASSERT_TRUE(routed.is_open());
        routed << "VkResult GameEngine::Rendering::VulkanDevice::ArmsAndYields()\n"
                  "{\n"
                  "    const VkResult armResult = context.WithQueueLocked([&] {\n"
                  "        return vkQueueSubmit(context.Queue(), 1, &emptySubmit, fence);\n"
                  "    });\n"
                  "    if (armResult == VK_SUCCESS)\n"
                  "    {\n"
                  "        queueFrame.fenceArmed = true;\n"
                  "    }\n"
                  "    return armResult;\n"
                  "}\n"
                  "\n"
                  "void GameEngine::Rendering::VulkanDevice::RoutesTheArm()\n"
                  "{\n"
                  "    const VkResult computeRes = ArmsAndYields();\n"
                  "    if (computeRes == VK_ERROR_DEVICE_LOST)\n"
                  "    {\n"
                  "        OnDeviceLostObserved(\"compute fence arm\", computeRes, true);\n"
                  "    }\n"
                  "}\n";
    }

    const DeviceLostScan scan = ScanForDeviceLostRouting(dir);
    std::string report;
    for (const std::string& site : scan.Swallowed)
        report += "\n  " + site;
    EXPECT_TRUE(scan.Swallowed.empty()) << "the arm is routed one frame up, at its call site:" << report;
    EXPECT_TRUE(scan.Unrouted.empty());

    fs::remove_all(dir, ec);
}

// The guard on the rule above. `return r == VK_SUCCESS;` reduces the loss to a bool
// and is exactly what this lint hunts, so it must NOT read as handing the result on.
// Confusing the two would silently retire every fence-wait entry in the backlog.
TEST(DeviceLossRouting, ScannerDoesNotTreatABoolReturnAsHandingTheResultOn)
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "ge_devloss_routing_lint_boolreturn";
    std::error_code ec;
    fs::remove_all(dir, ec);
    ASSERT_TRUE(fs::create_directories(dir, ec)) << ec.message();

    {
        std::ofstream reduced(dir / "Reduced.cpp");
        ASSERT_TRUE(reduced.is_open());
        reduced << "bool GameEngine::Rendering::VulkanDevice::WaitsAndDiscards()\n"
                   "{\n"
                   "    const VkResult r = vkWaitForFences(m_Device, 1, &fence, VK_TRUE, timeoutNs);\n"
                   "    return r == VK_SUCCESS;\n"
                   "}\n";
    }

    const DeviceLostScan scan = ScanForDeviceLostRouting(dir);
    EXPECT_EQ(scan.Swallowed.size(), 1u) << "a bool return discards the loss; it does not pass it on";
    EXPECT_TRUE(scan.Escaped.empty());

    fs::remove_all(dir, ec);
}

// The escape rule is the only one that removes a site from the swallow census, so
// it carries the whole burden: one `return name;` anywhere in the scope must not be
// read as proof the result leaves. Here it does leave — on the success path, which
// is the one path that cannot be carrying a loss — while the failure path flattens
// it to a different code. Accepting that escape licenses the swallow.
TEST(DeviceLossRouting, ScannerDoesNotAcceptAnEscapeThatHoldsOnlyOnTheSuccessPath)
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "ge_devloss_routing_lint_partialescape";
    std::error_code ec;
    fs::remove_all(dir, ec);
    ASSERT_TRUE(fs::create_directories(dir, ec)) << ec.message();

    {
        std::ofstream partial(dir / "PartialEscape.cpp");
        ASSERT_TRUE(partial.is_open());
        partial << "VkResult GameEngine::Rendering::VulkanDevice::FlattensTheLoss()\n"
                   "{\n"
                   "    const VkResult result = context.WithQueueLocked([&] {\n"
                   "        return vkQueueSubmit(context.Queue(), 1, &submit, fence);\n"
                   "    });\n"
                   "    if (result == VK_SUCCESS)\n"
                   "        queueFrame.fenceArmed = true;\n"
                   "    if (result != VK_SUCCESS)\n"
                   "        return VK_ERROR_INITIALIZATION_FAILED;\n"
                   "    return result;\n"
                   "}\n";
    }

    const DeviceLostScan scan = ScanForDeviceLostRouting(dir);
    std::string report;
    for (const std::string& site : scan.Escaped)
        report += "\n  " + site;
    EXPECT_TRUE(scan.Escaped.empty())
        << "the failure path returns a different code, so the loss does not reach the caller:"
        << report;
    // Both comparisons stand: with the escape denied, neither is answered anywhere.
    EXPECT_EQ(scan.SwallowedByFunction, (std::map<std::string, size_t>{{"FlattensTheLoss", 2}}))
        << "nothing here answers the loss, so both tests of the result are swallows";

    fs::remove_all(dir, ec);
}

// A result discarded outright at the call site of a yielding helper compares against
// nothing and returns nothing, so neither census can see it. The call-site census is
// the only one that can, and it only can if it counts calls the way the other two
// do — otherwise a loss-capable call leaves all three at once, which is the
// arithmetic that census exists to make impossible.
TEST(DeviceLossRouting, ScannerCountsADiscardedCallToAYieldingHelperAsACallSite)
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "ge_devloss_routing_lint_discardedyield";
    std::error_code ec;
    fs::remove_all(dir, ec);
    ASSERT_TRUE(fs::create_directories(dir, ec)) << ec.message();

    {
        std::ofstream discarded(dir / "Discarded.cpp");
        ASSERT_TRUE(discarded.is_open());
        discarded << "VkResult GameEngine::Rendering::VulkanDevice::ArmsAndYields()\n" // line 1
                     "{\n"
                     "    const VkResult armResult = context.WithQueueLocked([&] {\n"
                     "        return vkQueueSubmit(context.Queue(), 1, &submit, fence);\n" // line 4
                     "    });\n"
                     "    return armResult;\n"
                     "}\n"
                     "\n"
                     "void GameEngine::Rendering::VulkanDevice::DropsItEntirely()\n"
                     "{\n"
                     "    ArmsAndYields();\n" // line 11
                     "}\n";
    }

    const DeviceLostScan scan = ScanForDeviceLostRouting(dir);
    std::string report;
    for (const std::string& site : scan.LossCapableCallSites)
        report += "\n  " + site;
    EXPECT_TRUE(scan.Swallowed.empty()) << "nothing compares the discarded result";
    EXPECT_EQ(scan.EscapedByFunction, (std::map<std::string, size_t>{{"ArmsAndYields", 1}}));
    ASSERT_EQ(scan.LossCapableCallSites.size(), 2u)
        << "the submit, and the discarded call to the helper that relays it:" << report;
    EXPECT_NE(scan.LossCapableCallSites[0].find("Discarded.cpp:4"), std::string::npos)
        << scan.LossCapableCallSites[0];
    EXPECT_NE(scan.LossCapableCallSites[1].find("Discarded.cpp:11"), std::string::npos)
        << "the helper's own definition names it without calling it: " << scan.LossCapableCallSites[1];

    fs::remove_all(dir, ec);
}

// A comment is prose, not routing. Block comments reached the matchers as code, so a
// comment naming the observer answered a genuine swallow, and a site literal quoted
// in a comment kept the observed-sites census whole while the real call was gone —
// a checker that trusts prose is the failure it exists to prevent. The inline
// `/*param=*/` form every real observer call carries must still read as routing,
// and a `/*` inside a string literal must not open a comment at all.
TEST(DeviceLossRouting, ScannerDoesNotAcceptACommentAsRouting)
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "ge_devloss_routing_lint_blockcomment";
    std::error_code ec;
    fs::remove_all(dir, ec);
    ASSERT_TRUE(fs::create_directories(dir, ec)) << ec.message();

    {
        std::ofstream masked(dir / "Masked.cpp");
        ASSERT_TRUE(masked.is_open());
        masked << "void GameEngine::Rendering::VulkanDevice::SwallowsBehindAComment()\n"
                  "{\n"
                  "    const VkResult result = vkQueueSubmit(q, 1, &si, f);\n"
                  "    /* Answered upstream, as if by\n"
                  "       OnDeviceLostObserved(\"a quoted site\", result, true); */\n"
                  "    if (result != VK_SUCCESS)\n" // line 6
                  "    {\n"
                  "        Logger::Log::Error(\"submit failed\");\n"
                  "    }\n"
                  "}\n";
    }
    {
        std::ofstream deleted(dir / "Deleted.cpp");
        ASSERT_TRUE(deleted.is_open());
        deleted << "void GameEngine::Rendering::VulkanDevice::ChecksBehindAComment()\n"
                   "{\n"
                   "    if (waitRes == VK_ERROR_DEVICE_LOST)\n" // line 3
                   "    {\n"
                   "        /* was OnDeviceLostObserved(\"the wait\", waitRes, true); */\n"
                   "        RecoverQuietly();\n"
                   "    }\n"
                   "}\n";
    }
    {
        std::ofstream inlineComment(dir / "InlineComment.cpp");
        ASSERT_TRUE(inlineComment.is_open());
        inlineComment
            << "void GameEngine::Rendering::VulkanDevice::RoutesPastAnInlineComment()\n"
               "{\n"
               "    if (result == VK_ERROR_DEVICE_LOST)\n"
               "        OnDeviceLostObserved(\"an inline site\", result, /*actuallyLost=*/true);\n"
               "}\n";
    }
    {
        std::ofstream literal(dir / "Literal.cpp");
        ASSERT_TRUE(literal.is_open());
        literal << "void GameEngine::Rendering::VulkanDevice::LogsASlashStar()\n"
                   "{\n"
                   "    Logger::Log::Error(\"pattern /* is not a comment\");\n"
                   "    const VkResult result = vkQueueSubmit(q, 1, &si, f);\n"
                   "    if (result != VK_SUCCESS)\n" // line 5
                   "        Logger::Log::Error(\"submit failed\");\n"
                   "}\n";
    }

    const DeviceLostScan scan = ScanForDeviceLostRouting(dir);
    std::string report;
    for (const std::string& site : scan.Swallowed)
        report += "\n  " + site;
    EXPECT_EQ(scan.SwallowedByFunction,
              (std::map<std::string, size_t>{{"LogsASlashStar", 1}, {"SwallowsBehindAComment", 1}}))
        << "a comment naming the observer must not answer a check, and a `/*` inside a string "
           "literal must not comment out the rest of the file:"
        << report;
    ASSERT_EQ(scan.Unrouted.size(), 1u) << "exactly the site whose observer was commented out";
    EXPECT_NE(scan.Unrouted[0].find("Deleted.cpp:3"), std::string::npos) << scan.Unrouted[0];
    EXPECT_EQ(scan.Sites, (std::set<std::string>{"an inline site"}))
        << "a site literal quoted in a comment is not an observation, and the inline "
           "`/*param=*/` form of a real call still is";

    fs::remove_all(dir, ec);
}

TEST(DeviceLossRouting, EveryDeviceLostCheckInTheBackendReachesTheObserver)
{
    namespace fs = std::filesystem;
    const fs::path backendDir{GE_VULKAN_BACKEND_SOURCE_DIR};
    ASSERT_TRUE(fs::is_directory(backendDir))
        << "Vulkan backend source not found at " << backendDir.string()
        << " — the lint scanned nothing, which must fail rather than pass silently.";

    const DeviceLostScan scan = ScanForDeviceLostRouting(backendDir);
    ASSERT_GT(scan.FilesScanned, 0u) << "scanned no source files — the lint is not looking where it thinks";
    ASSERT_GT(scan.ChecksFound, 0u)
        << "found no VK_ERROR_DEVICE_LOST checks at all; the scan or the matcher is broken, and a "
           "lint that matches nothing must fail rather than report green";

    std::string report;
    for (const std::string& site : scan.Unrouted)
        report += "\n  " + site;
    EXPECT_TRUE(scan.Unrouted.empty())
        << "these sites observe VK_ERROR_DEVICE_LOST without reaching OnDeviceLostObserved within "
        << kMaxLinesToObserver
        << " lines, so a loss there would recover (or not) with NO error log and NO GPU checkpoint "
           "dump:" << report;
}

TEST(DeviceLossRouting, TheKnownObservationSitesAreAllStillRouted)
{
    namespace fs = std::filesystem;
    const fs::path backendDir{GE_VULKAN_BACKEND_SOURCE_DIR};
    ASSERT_TRUE(fs::is_directory(backendDir)) << "Vulkan backend source not found";

    // Every place the backend can learn the device died. Deleting a site's routing
    // fails this loudly and names it; adding a genuinely new site is a deliberate
    // edit here, which is the point.
    const std::set<std::string> expected{
        "BeginFrame fence poll",
        "BeginFrame fence wait",
        "compute fence arm",
        "graphics queue submit",
        "hung escalation cap",
        "present",
        "render-graph queue submit",
        "swapchain acquire",
        // Observed on a job thread (SubmitTextureUploads) and performed on the
        // render thread by DrainWorkerDeviceLossObservation, which is where the
        // literal lives.
        "texture upload submit",
        "transfer fence arm",
    };

    EXPECT_EQ(ScanForDeviceLostRouting(backendDir).Sites, expected);
}

// Results of loss-capable calls that reach only a VK_SUCCESS comparison. These
// swallow a device loss silently, and the explicit matcher cannot see them — the
// class that hid SubmitTextureUploads and both compute/transfer fence arms.
//
// The backlog below is what the widened matcher found already present; it is a
// ratchet, not an endorsement. Routing any of these changes device-loss behaviour
// and needs a runtime check, so they are recorded rather than silently fixed. A
// NEW swallowing site fails this test, which is the point. Fixing one means
// deleting its entry.
//
// One loss-capable submit is deliberately absent from this census: the arm submit in
// the armFence lambda of ArmComputeTransferFences. Its result is handed back to
// ArmComputeTransferFences, which routes it as computeRes/transferRes outside the
// queue lock — so it does not swallow, and TheKnownObservationSitesAreAllStillRouted
// is what holds that routing in place ("compute fence arm", "transfer fence arm").
TEST(DeviceLossRouting, NoNewSiteSwallowsALossBehindAVkSuccessCheck)
{
    namespace fs = std::filesystem;
    const fs::path backendDir{GE_VULKAN_BACKEND_SOURCE_DIR};
    ASSERT_TRUE(fs::is_directory(backendDir)) << "Vulkan backend source not found";

    const std::map<std::string, size_t> knownBacklog{
        // The batch lambda's single submit, which serves both the timeline and the
        // no-timeline case.
        {"ExecuteCommandLists", 1},
        // Offscreen fence-arm submit, reached through ArmGraphicsFrameFence.
        {"FinalizeFrame", 1},
        // vkGetFenceStatus: a lost device reads as "not signaled", so callers spin.
        {"IsFenceSignaled", 1},
        // Two graphics fence arms through ArmGraphicsFrameFence, plus the
        // present-transition submit, whose one result is tested on two lines (inside
        // the signal lambda and again after it). Same shape as the compute/transfer
        // arms that route, for the queue that matters most.
        {"Present", 4},
        // Present-complete marker submit.
        {"PresentImage", 1},
        // Fence waits that report a loss as a generic wait failure.
        {"WaitForArmedGraphicsFences", 1},
        {"WaitForFence", 1},
        {"WaitForSpecificGraphicsFence", 1},
    };

    const DeviceLostScan scan = ScanForDeviceLostRouting(backendDir);
    ASSERT_GT(scan.FilesScanned, 0u) << "scanned no source files";

    std::string report;
    for (const std::string& site : scan.Swallowed)
        report += "\n  " + site;
    EXPECT_EQ(scan.SwallowedByFunction, knownBacklog)
        << "a loss-capable result is tested only against VK_SUCCESS. Route it "
           "(OnDeviceLostObserved, or NoteDeviceLostOnWorkerThread off the render thread), "
           "or update the backlog above if this site was fixed. All swallowing sites:"
        << report;
}

// The population the two censuses partition. Both of them count *comparisons*, so
// either can stay unchanged while a call site is added, moved behind a helper, or
// deleted — which is exactly how three arm submits went missing from the backlog
// while its total stayed at eleven. Pinning the call sites themselves makes that
// arithmetic impossible: a new place the backend can learn the device died has to be
// classified here before anything else can go green.
//
// Of the sites below, one is in neither census by design: TriggerGpuHang's submit
// discards its result outright. That is deliberate — the helper exists to make the
// GPU hang, and the ensuing TDR is observed by the next fence wait, which routes.
// A discarded result is not "tested only against VK_SUCCESS", so it is not a
// swallow; it is recorded here so it is not merely unmentioned.
//
// Calls to a method that hands a loss-capable result back count as call sites too,
// and for the same reason: a result discarded at such a call compares against
// nothing and returns nothing, so this census is the only one that can see it. The
// three ArmGraphicsFrameFence call sites below are the present shape of that.
TEST(DeviceLossRouting, EveryLossCapableCallSiteIsAccountedFor)
{
    namespace fs = std::filesystem;
    const fs::path backendDir{GE_VULKAN_BACKEND_SOURCE_DIR};
    ASSERT_TRUE(fs::is_directory(backendDir)) << "Vulkan backend source not found";

    constexpr size_t kKnownLossCapableCallSites = 21;

    const DeviceLostScan scan = ScanForDeviceLostRouting(backendDir);
    ASSERT_GT(scan.FilesScanned, 0u) << "scanned no source files";

    std::string report;
    for (const std::string& site : scan.LossCapableCallSites)
        report += "\n  " + site;
    EXPECT_EQ(scan.LossCapableCallSites.size(), kKnownLossCapableCallSites)
        << "the set of calls that can return VK_ERROR_DEVICE_LOST changed. Classify the new or "
           "removed site — routed, swallowed (backlog), handed to a caller (escapes), or "
           "deliberately discarded — then update this count. All call sites:"
        << report;
}

// The escape hatch of the census above: these results are not judged where they are
// produced, because they are handed to the caller whole. That is the one way a site
// can leave the swallow census without being fixed, so it is pinned separately — a
// new escape has to be justified, not absorbed.
TEST(DeviceLossRouting, TheOnlyResultsHandedToACallerAreTheKnownThree)
{
    namespace fs = std::filesystem;
    const fs::path backendDir{GE_VULKAN_BACKEND_SOURCE_DIR};
    ASSERT_TRUE(fs::is_directory(backendDir)) << "Vulkan backend source not found";

    const std::map<std::string, size_t> knownEscapes{
        // armFence's arm submit, routed by its caller as computeRes/transferRes
        // outside the queue lock.
        {"ArmComputeTransferFences", 1},
        // The graphics arm submit, discarded by all three of its call sites — which
        // is where the backlog above counts it.
        {"ArmGraphicsFrameFence", 1},
        // The present itself, routed after the queue-locked lambda returns it.
        {"PresentImage", 1},
    };

    const DeviceLostScan scan = ScanForDeviceLostRouting(backendDir);
    ASSERT_GT(scan.FilesScanned, 0u) << "scanned no source files";

    std::string report;
    for (const std::string& site : scan.Escaped)
        report += "\n  " + site;
    EXPECT_EQ(scan.EscapedByFunction, knownEscapes)
        << "a loss-capable result is handed to a caller rather than answered here. Confirm the "
           "caller answers it, then record it above. All escaping results:"
        << report;
}

// Does the memory topology survive an in-place device rebuild, and does a
// mesh-pool-shaped buffer created AFTER the rebuild resolve the same way as one
// created before it?
//
// RebuildDevice re-runs QueryDeviceCapabilities, which zeroes m_Capabilities;
// CreateBuffer resolves every UploadDeviceLocalPreferred allocation against the
// topology and runs on job threads without the rebuild lock. A topology that
// did not survive intact would silently demote every mesh pool back to host
// memory, with no error anywhere and no visible symptom but throughput.
TEST(DeviceRecoveryDevice, MemoryTopologyAndMeshPoolResidencySurviveRebuild)
{
    ScopedEnvVar forcedLoss("GE_VK_FORCE_DEVICE_LOST", "2");
    auto dev = MakeHeadlessDevice();
    if (!dev)
        GTEST_SKIP() << "no Vulkan device available";

    const auto topoBefore = dev->GetCapabilities().memoryTopology;
    ASSERT_TRUE(topoBefore.has_value()) << "Vulkan backend must report a topology before rebuild";

    BufferDesc desc{};
    desc.size = 4u * 1024u * 1024u;
    desc.usage = static_cast<uint32_t>(BufferUsage::Vertex);
    desc.memoryUsage = BufferMemoryUsage::UploadDeviceLocalPreferred;
    desc.flags = BufferCreateFlags::PersistentlyMapped;
    desc.debugName = "RebuildProbe_PreRebuildMeshPool";
    const BufferHandle before = dev->CreateBuffer(desc);
    ASSERT_TRUE(before.IsValid());
    const IDevice::BufferMemoryResidency resBefore = dev->GetBufferMemoryResidency(before);
    std::cout << "[ probe ] pre-rebuild  eligible="
              << (topoBefore->largestHostVisibleDeviceLocalHeapBytes >=
                  topoBefore->deviceLocalHeapBytesTotal)
              << " deviceLocal=" << resBefore.deviceLocal
              << " hostVisible=" << resBefore.hostVisible
              << " hostCoherent=" << resBefore.hostCoherent
              << " heapIndex=" << resBefore.heapIndex << std::endl;

    bool reachedAwaiting = false;
    for (int i = 0; i < 12 && !reachedAwaiting; ++i)
    {
        RunOneFrame(*dev);
        reachedAwaiting = (dev->GetDeviceHealth() == DeviceHealth::AwaitingReprovision);
    }
    ASSERT_TRUE(reachedAwaiting) << "injected loss should rebuild to AwaitingReprovision";
    dev->NotifyReprovisionComplete();
    ASSERT_EQ(dev->GetDeviceHealth(), DeviceHealth::Healthy);

    const auto topoAfter = dev->GetCapabilities().memoryTopology;
    ASSERT_TRUE(topoAfter.has_value()) << "topology went DISENGAGED across the rebuild";
    EXPECT_EQ(topoAfter->isUnifiedMemory, topoBefore->isUnifiedMemory);
    EXPECT_EQ(topoAfter->largestHostVisibleDeviceLocalHeapBytes,
              topoBefore->largestHostVisibleDeviceLocalHeapBytes);
    EXPECT_EQ(topoAfter->deviceLocalHeapBytesTotal, topoBefore->deviceLocalHeapBytesTotal);

    desc.debugName = "RebuildProbe_PostRebuildMeshPool";
    const BufferHandle after = dev->CreateBuffer(desc);
    ASSERT_TRUE(after.IsValid()) << "mesh-pool-shaped buffer failed on the rebuilt device";
    const IDevice::BufferMemoryResidency resAfter = dev->GetBufferMemoryResidency(after);
    std::cout << "[ probe ] post-rebuild deviceLocal=" << resAfter.deviceLocal
              << " hostVisible=" << resAfter.hostVisible
              << " hostCoherent=" << resAfter.hostCoherent
              << " heapIndex=" << resAfter.heapIndex << std::endl;

    EXPECT_EQ(resAfter.reported, resBefore.reported);
    EXPECT_EQ(resAfter.deviceLocal, resBefore.deviceLocal)
        << "mesh pool changed device-locality across a rebuild";
    EXPECT_EQ(resAfter.hostVisible, resBefore.hostVisible);
    EXPECT_EQ(resAfter.hostCoherent, resBefore.hostCoherent);
    EXPECT_EQ(resAfter.heapIndex, resBefore.heapIndex);

    // MappedUploadBufferIsStillMappable, post-rebuild arm: PersistentlyMapped
    // must still mean mapped after the device is replaced under it.
    void* mapped = dev->MapBuffer(after);
    ASSERT_NE(mapped, nullptr) << "post-rebuild mesh pool is not mappable";
    const uint32_t pattern[4] = {0xFEEDu, 0xBEEFu, 0x900Du, 0xF00Du};
    dev->UpdateBuffer(after, 0, sizeof(pattern), pattern);
    EXPECT_EQ(std::memcmp(mapped, pattern, sizeof(pattern)), 0)
        << "write through the mapped pointer did not land";
    dev->UnmapBuffer(after);
    dev->DestroyBuffer(after);
    dev->Shutdown();
}
