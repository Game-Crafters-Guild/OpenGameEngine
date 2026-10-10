// VCSStatusPoller runs every VCS provider's background status poll: one poll
// at start, one per interval, one right after each Wake, a burst of wakes
// collapsed by a cooldown, and a Stop that joins the thread and ignores later
// wakes. The interval is an hour here, so every poll after the first one comes
// from a Wake.

#include "VCSIntegration/VCSStatusPoller.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>

namespace GameEngine
{
namespace
{

constexpr int kNeverSeconds = 3600;
constexpr std::chrono::seconds kPollTimeout{5};
constexpr std::chrono::milliseconds kSettleTime{1500};
constexpr int kBurstWakes = 10;

// Waits for the poll count to reach target; false on timeout.
bool WaitForPolls(const std::atomic<int>& polls, int target)
{
    const auto deadline = std::chrono::steady_clock::now() + kPollTimeout;
    while (polls.load() < target)
    {
        if (std::chrono::steady_clock::now() > deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

} // namespace

TEST(VCSStatusPollerTests, PollsOnceAtStart)
{
    std::atomic<int> polls{0};
    VCSStatusPoller poller;
    poller.SetIntervalSeconds(kNeverSeconds);
    poller.Start("Test Status", [&polls] { ++polls; });

    EXPECT_TRUE(WaitForPolls(polls, 1));
    EXPECT_FALSE(poller.IsStopping());
    poller.Stop();
}

TEST(VCSStatusPollerTests, WakePollsBeforeTheInterval)
{
    std::atomic<int> polls{0};
    VCSStatusPoller poller;
    poller.SetIntervalSeconds(kNeverSeconds);
    poller.Start("Test Status", [&polls] { ++polls; });
    ASSERT_TRUE(WaitForPolls(polls, 1));

    poller.Wake();
    EXPECT_TRUE(WaitForPolls(polls, 2));
    poller.Stop();
}

TEST(VCSStatusPollerTests, BurstOfWakesCollapsesIntoAtMostTwoPolls)
{
    std::atomic<int> polls{0};
    VCSStatusPoller poller;
    poller.SetIntervalSeconds(kNeverSeconds);
    poller.Start("Test Status", [&polls] { ++polls; });
    ASSERT_TRUE(WaitForPolls(polls, 1));

    for (int i = 0; i < kBurstWakes; ++i)
        poller.Wake();
    std::this_thread::sleep_for(kSettleTime);

    // The immediate poll, then at most one more after the cooldown.
    EXPECT_GE(polls.load(), 2);
    EXPECT_LE(polls.load(), 3);
    poller.Stop();
}

TEST(VCSStatusPollerTests, StopJoinsAndIgnoresLaterWakes)
{
    std::atomic<int> polls{0};
    VCSStatusPoller poller;
    poller.SetIntervalSeconds(kNeverSeconds);
    poller.Start("Test Status", [&polls] { ++polls; });
    ASSERT_TRUE(WaitForPolls(polls, 1));

    poller.Stop();
    EXPECT_TRUE(poller.IsStopping());
    const int pollsAtStop = polls.load();

    poller.Wake();
    std::this_thread::sleep_for(kSettleTime);
    EXPECT_EQ(polls.load(), pollsAtStop);
}

TEST(VCSStatusPollerTests, ThrowingPollDoesNotStopPolling)
{
    std::atomic<int> polls{0};
    VCSStatusPoller poller;
    poller.SetIntervalSeconds(kNeverSeconds);
    poller.Start("Test Status", [&polls] {
        if (++polls == 1)
            throw std::runtime_error("status command failed");
    });
    ASSERT_TRUE(WaitForPolls(polls, 1));

    poller.Wake();
    EXPECT_TRUE(WaitForPolls(polls, 2));
    poller.Stop();
}

TEST(VCSStatusPollerTests, NonPositiveIntervalRestoresTheDefault)
{
    VCSStatusPoller poller;
    poller.SetIntervalSeconds(0);
    EXPECT_EQ(poller.GetIntervalSeconds(), VCSStatusPoller::kDefaultIntervalSeconds);
    poller.SetIntervalSeconds(30);
    EXPECT_EQ(poller.GetIntervalSeconds(), 30);
}

} // namespace GameEngine
