// ProcessCommandsBatchLimitTest.cpp - pins the ProcessCommands batching
// contract: a single call executes at most World::kMaxCommandsPerBatch
// deferred commands, the remainder stays queued for subsequent calls, and no
// command is ever lost when the cap is hit. Regression for the loop that
// evaluated TryPop before the cap check and silently discarded the popped
// command -- one lost per non-empty buffer once a batch was capped.
#include <gtest/gtest.h>
#include "ECS/World.h"
#include "ECS/Entity.h"
#include "JobSystem/WorkStealingThreadPool.h"

#include <thread>

using namespace GameEngine;
using namespace GameEngine::ECS;

namespace {

Command MakeCreate() {
    Command cmd;
    cmd.type = Command::CREATE_ENTITY;
    return cmd;
}

} // namespace

TEST(ProcessCommandsBatchLimit, NoCommandLostAcrossCappedBatches) {
    WorldConfig cfg;
    // The ring must hold more than one full batch so commands genuinely queue
    // past the cap instead of overflowing at push time (capacity rounds up to
    // the next power of two; one slot stays reserved).
    cfg.CommandBufferSize = World::kMaxCommandsPerBatch * 2;
    JobSystem::WorkStealingThreadPool jobSystem(2);
    World world(cfg, &jobSystem);

    constexpr size_t kMainCommands = World::kMaxCommandsPerBatch + 3;
    constexpr size_t kWorkerCommands = 5;

    // Register the main thread's buffer first so it drains first, then fill a
    // second per-thread buffer from a worker: the old bug also dropped one
    // command from every subsequent buffer visited after the cap was reached.
    ASSERT_TRUE(world.TryPushCommand(MakeCreate()));
    std::thread worker([&world] {
        for (size_t i = 0; i < kWorkerCommands; ++i) {
            EXPECT_TRUE(world.TryPushCommand(MakeCreate()));
        }
    });
    worker.join();
    for (size_t i = 1; i < kMainCommands; ++i) {
        ASSERT_TRUE(world.TryPushCommand(MakeCreate()));
    }

    // First drain executes exactly one full batch; the excess stays queued.
    world.ProcessCommands();
    EXPECT_EQ(world.GetEntityCount(), World::kMaxCommandsPerBatch);

    // Second drain picks up every remaining command: zero lost at the cap.
    world.ProcessCommands();
    EXPECT_EQ(world.GetEntityCount(), kMainCommands + kWorkerCommands);

    // Steady state: nothing left to execute.
    world.ProcessCommands();
    EXPECT_EQ(world.GetEntityCount(), kMainCommands + kWorkerCommands);
}
