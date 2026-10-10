#include <gtest/gtest.h>

#include "UI/UiDispatcher.h"

#include <vector>
#include <string>

using namespace GameEngine::UI;

TEST(UiDispatcherTests, ExecutesInFifoOrder)
{
    UiDispatcher disp;

    std::vector<int> seen;

    disp.Post([&]{ seen.push_back(1); });
    disp.Post([&]{ seen.push_back(2); });
    disp.Post([&]{ seen.push_back(3); });

    EXPECT_EQ(disp.PendingCount(), static_cast<size_t>(3));

    disp.Drain();

    ASSERT_EQ(seen.size(), static_cast<size_t>(3));
    EXPECT_EQ(seen[0], 1);
    EXPECT_EQ(seen[1], 2);
    EXPECT_EQ(seen[2], 3);
}

TEST(UiDispatcherTests, ReentrantPostsRequireSecondDrain)
{
    UiDispatcher disp;

    std::vector<std::string> seq;

    disp.Post([&]{
        seq.push_back("A");
        // Re-entrant post during Drain() should be queued for the next Drain()
        disp.Post([&]{ seq.push_back("B"); });
    });

    EXPECT_EQ(disp.PendingCount(), static_cast<size_t>(1));

    disp.Drain();
    ASSERT_EQ(seq.size(), static_cast<size_t>(1));
    EXPECT_EQ(seq[0], "A");

    // Re-entrant task should still be pending
    EXPECT_EQ(disp.PendingCount(), static_cast<size_t>(1));

    disp.Drain();
    ASSERT_EQ(seq.size(), static_cast<size_t>(2));
    EXPECT_EQ(seq[1], "B");
}

TEST(UiDispatcherTests, NestedDrainIsNoOpAndDoesNotCorruptOuterIteration)
{
    // Regression: Drain() used to swap into a shared member buffer with no re-entry
    // guard; a lambda that re-entered Drain() would clobber the outer iteration and
    // invoke a moved-from std::function (Mac second-launch crash).
    // Contract (from IUiDispatcher docs): nested Drain is a no-op; work posted inside
    // a handler is deferred to a subsequent Drain.
    UiDispatcher disp;

    std::vector<std::string> seq;

    disp.Post([&]{
        seq.push_back("A");
        disp.Post([&]{ seq.push_back("nested"); });
        disp.Drain(); // re-entry: must be a no-op, must not corrupt outer iteration
    });
    disp.Post([&]{ seq.push_back("B"); });
    disp.Post([&]{ seq.push_back("C"); });

    disp.Drain();

    // Outer pass runs A, B, C. The nested Drain was a no-op so "nested" stays queued.
    ASSERT_EQ(seq.size(), static_cast<size_t>(3));
    EXPECT_EQ(seq[0], "A");
    EXPECT_EQ(seq[1], "B");
    EXPECT_EQ(seq[2], "C");
    EXPECT_EQ(disp.PendingCount(), static_cast<size_t>(1));

    disp.Drain();
    ASSERT_EQ(seq.size(), static_cast<size_t>(4));
    EXPECT_EQ(seq[3], "nested");
}

TEST(UiDispatcherTests, PendingCountReflectsQueueSize)
{
    UiDispatcher disp;

    EXPECT_EQ(disp.PendingCount(), static_cast<size_t>(0));

    disp.Post([]{});
    disp.Post([]{});
    EXPECT_EQ(disp.PendingCount(), static_cast<size_t>(2));

    disp.Drain();
    EXPECT_EQ(disp.PendingCount(), static_cast<size_t>(0));
}

TEST(UiDispatcherTests, CloseRefusesLaterPostsAndStillRunsWhatItAccepted)
{
    UiDispatcher disp;

    std::vector<std::string> seq;

    EXPECT_TRUE(disp.Post([&]{ seq.push_back("accepted"); }));
    disp.Close();
    EXPECT_FALSE(disp.Post([&]{ seq.push_back("refused"); }));
    EXPECT_EQ(disp.PendingCount(), static_cast<size_t>(1));

    disp.Drain();
    ASSERT_EQ(seq.size(), static_cast<size_t>(1));
    EXPECT_EQ(seq[0], "accepted");
    EXPECT_EQ(disp.PendingCount(), static_cast<size_t>(0));
}
