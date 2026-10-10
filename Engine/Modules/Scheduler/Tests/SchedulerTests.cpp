#include <gtest/gtest.h>
#include "Scheduler/Scheduler.h"
#include <vector>

using namespace GameEngine::Scheduler;

TEST(SchedulerTests, NextTickExecutesInFifoOrder) {
    DefaultScheduler sch;
    std::vector<int> seen;
    sch.ScheduleNext([&]{ seen.push_back(1); });
    sch.ScheduleNext([&]{ seen.push_back(2); });
    sch.ScheduleNext([&]{ seen.push_back(3); });
    EXPECT_EQ(sch.PendingCount(), static_cast<size_t>(3));
    EXPECT_EQ(sch.ProcessDue(), static_cast<size_t>(3));
    ASSERT_EQ(seen.size(), static_cast<size_t>(3));
    EXPECT_EQ(seen[0], 1);
    EXPECT_EQ(seen[1], 2);
    EXPECT_EQ(seen[2], 3);
}

TEST(SchedulerTests, DelayedExecutesOnlyAfterDelay) {
    DefaultScheduler sch;
    std::vector<int> seen;
    auto t0 = Clock::now();
    sch.ScheduleAfter(Millis(100), [&]{ seen.push_back(1); });
    EXPECT_EQ(sch.ProcessDue(t0 + Millis(50)), 0u);
    EXPECT_TRUE(seen.empty());
    // Allow small scheduling jitter; use >delay by 1ms to account for capture skew
    EXPECT_EQ(sch.ProcessDue(t0 + Millis(101)), 1u);
    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen[0], 1);
}

TEST(SchedulerTests, AbsoluteTimeExecutesAtOrAfterTime) {
    DefaultScheduler sch;
    std::vector<int> seen;
    auto when = Clock::now() + Millis(75);
    sch.ScheduleAt(when, [&]{ seen.push_back(42); });
    EXPECT_EQ(sch.ProcessDue(when - Millis(1)), 0u);
    EXPECT_EQ(sch.ProcessDue(when), 1u);
    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen[0], 42);
}

TEST(SchedulerTests, CancelPreventsExecution) {
    DefaultScheduler sch;
    bool ran = false;
    auto tok = sch.ScheduleNext([&]{ ran = true; });
    ASSERT_TRUE(tok.IsValid());
    EXPECT_TRUE(sch.Cancel(tok));
    EXPECT_EQ(sch.ProcessDue(), 0u);
    EXPECT_FALSE(ran);
}

TEST(SchedulerTests, ChainThenAndAfter) {
    DefaultScheduler sch;
    std::vector<int> seen;

    ChainBuilder builder{sch};
    builder.Then([&]{ seen.push_back(1); })
           .After(Millis(10), [&]{ seen.push_back(2); })
           .Then([&]{ seen.push_back(3); });

    auto handle = builder.Commit();

    // First ProcessDue: runs step 1 only
    EXPECT_EQ(sch.ProcessDue(), 1u);
    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen[0], 1);

    // Second call with small delta shouldn't run step 2 yet
    auto t0 = Clock::now();
    EXPECT_EQ(sch.ProcessDue(t0 + Millis(5)), 0u);
    ASSERT_EQ(seen.size(), 1u);

    // After delay elapsed, step 2 should run; it will schedule step 3 as Next
    EXPECT_EQ(sch.ProcessDue(t0 + Millis(10)), 1u);
    ASSERT_EQ(seen.size(), 2u);
    EXPECT_EQ(seen[1], 2);

    // Next ProcessDue executes step 3
    EXPECT_EQ(sch.ProcessDue(), 1u);
    ASSERT_EQ(seen.size(), 3u);
    EXPECT_EQ(seen[2], 3);
}


TEST(SchedulerTests, ReentrantNextDefersToNextProcessDue) {
    DefaultScheduler sch;
    std::vector<int> seen;
    sch.ScheduleNext([&]{
        seen.push_back(1);
        sch.ScheduleNext([&]{ seen.push_back(2); });
    });
    EXPECT_EQ(sch.ProcessDue(), 1u);
    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen[0], 1);
    EXPECT_EQ(sch.ProcessDue(), 1u);
    ASSERT_EQ(seen.size(), 2u);
    EXPECT_EQ(seen[1], 2);
}

TEST(SchedulerTests, EqualTimeOrderingStableFifo) {
    DefaultScheduler sch;
    std::vector<int> seen;
    auto when = Clock::now() + Millis(50);
    sch.ScheduleAt(when, [&]{ seen.push_back(1); });
    sch.ScheduleAt(when, [&]{ seen.push_back(2); });
    sch.ScheduleAt(when, [&]{ seen.push_back(3); });
    EXPECT_EQ(sch.ProcessDue(when - Millis(1)), 0u);
    EXPECT_EQ(sch.ProcessDue(when), 3u);
    ASSERT_EQ(seen.size(), 3u);
    EXPECT_EQ(seen[0], 1);
    EXPECT_EQ(seen[1], 2);
    EXPECT_EQ(seen[2], 3);
}

TEST(SchedulerTests, ChainCancelMidwayPreventsFurtherSteps) {
    DefaultScheduler sch;
    std::vector<int> seen;
    ChainBuilder b{sch};
    b.Then([&]{ seen.push_back(1); })
     .After(Millis(10), [&]{ seen.push_back(2); })
     .Then([&]{ seen.push_back(3); });
    auto h = b.Commit();
    EXPECT_EQ(sch.ProcessDue(), 1u);
    ASSERT_EQ(seen.size(), 1u);
    auto t0 = Clock::now();
    ASSERT_TRUE(h.Cancel());
    EXPECT_EQ(sch.ProcessDue(t0 + Millis(10)), 0u);
    EXPECT_EQ(sch.ProcessDue(), 0u);
    ASSERT_EQ(seen.size(), 1u);
}

