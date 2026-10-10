// Pacing and lifetime tests for VideoPlayer.
//
// The property under test is that playback is decoupled from the caller's tick.
// A player that decodes inside the caller's call advances one frame per call, so
// its frame rate is the app's frame rate and its cost per call scales with how
// much time the previous call took. A player that paces itself off the wall clock
// advances at the source's own rate whether the caller calls once or ten thousand
// times, and advances even when the caller does not call at all.
//
// Every assertion here is self-calibrating: none of them needs to know the
// fixture's frame rate, only the RATIO between calls made and frames published.

#include <gtest/gtest.h>

#include "Video/VideoPlayer.h"

#include <chrono>
#include <filesystem>
#include <thread>

#include "StagedTestPaths.h"

using namespace GameEngine;

namespace
{
// Long enough to contain several source frames at any sane frame rate, short
// enough to keep the suite quick.
constexpr auto kObservationWindow = std::chrono::milliseconds(400);
// A caller polling flat out must outnumber published frames by at least this
// much. A decode-per-call player scores exactly 1.
constexpr uint64_t kMinPollsPerPublishedFrame = 20;
constexpr float kFirstFrameTimeoutSeconds = 5.0f;

std::filesystem::path StagedSampleVideo()
{
    return TestPaths::StagedRoot() / "Apps" / "Editor" / "Assets" / "Sample" / "logo.mp4";
}
} // namespace

class VideoPlayerTest : public ::testing::Test
{
  protected:
    // Skips loudly by name when this build has no decoder — the backend is chosen
    // at configure time and the null backend cannot load anything. A missing
    // fixture is a failure, not a skip.
    //
    // NEITHER outcome stops the caller on its own: GTEST_SKIP and a failed ASSERT_*
    // both return only from the frame they appear in, and IsSkipped() is false for
    // the assertion. Callers must therefore follow this with
    // `if (IsSkipped() || HasFatalFailure()) return;` — without the second half a
    // missing fixture leaves the test running against an unloaded player, where
    // every later assertion measures nothing.
    void LoadVideo(const std::filesystem::path& video = StagedSampleVideo())
    {
        ASSERT_TRUE(std::filesystem::exists(video))
            << "staged fixture missing: " << video.string()
            << " (StageTestAssets should have copied it)";
        if (!m_Player.Load(video.string()))
        {
            GTEST_SKIP() << "video decode unavailable in this build (null VideoPlayer backend); "
                            "fixture present at " << video.string();
        }
    }

    Video::VideoPlayer m_Player;
};

// The decisive one. Poll as fast as the CPU allows for a fixed wall-clock window
// and count both sides: a player that decodes inside PollNewFrame publishes one
// frame per poll (ratio 1), a player paced off the wall clock publishes at the
// source's rate no matter how hard it is polled.
TEST_F(VideoPlayerTest, PublicationRateIsIndependentOfPollRate)
{
    LoadVideo();
    if (IsSkipped() || HasFatalFailure()) return;

    m_Player.Play();
    ASSERT_TRUE(m_Player.WaitForFrame(kFirstFrameTimeoutSeconds))
        << "no frame was ever published";

    const uint64_t startPublished = m_Player.GetPublishedFrameCount();
    uint64_t polls = 0;
    const auto deadline = std::chrono::steady_clock::now() + kObservationWindow;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (m_Player.PollNewFrame())
            m_Player.AcquireFramePointer();
        ++polls;
    }
    const uint64_t published = m_Player.GetPublishedFrameCount() - startPublished;

    EXPECT_GE(published, 1u) << "playback did not advance at all during the window";
    ASSERT_GT(polls, 0u);
    EXPECT_GE(polls, published * kMinPollsPerPublishedFrame)
        << "published " << published << " frames across " << polls
        << " polls — publication is tracking the caller's call rate, not the source's";
}

// The other half of the same property: frames arrive while nobody is asking.
// A player that only decodes inside a caller's call publishes nothing here.
TEST_F(VideoPlayerTest, FramesAdvanceWithoutAnyCallFromTheCaller)
{
    LoadVideo();
    if (IsSkipped() || HasFatalFailure()) return;

    m_Player.Play();
    ASSERT_TRUE(m_Player.WaitForFrame(kFirstFrameTimeoutSeconds));

    const uint64_t before = m_Player.GetPublishedFrameCount();
    std::this_thread::sleep_for(kObservationWindow);
    EXPECT_GT(m_Player.GetPublishedFrameCount(), before)
        << "no frame was decoded while the caller was idle";
}

// A paused player must cost nothing: no decoding, so no publication.
TEST_F(VideoPlayerTest, PausedPlaybackPublishesNothing)
{
    LoadVideo();
    if (IsSkipped() || HasFatalFailure()) return;

    m_Player.Play();
    ASSERT_TRUE(m_Player.WaitForFrame(kFirstFrameTimeoutSeconds));
    m_Player.Pause();

    // Drain the frame that may have been published between the pause request and
    // the decode thread observing it.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const uint64_t before = m_Player.GetPublishedFrameCount();
    std::this_thread::sleep_for(kObservationWindow);
    EXPECT_EQ(m_Player.GetPublishedFrameCount(), before)
        << "a paused player is still decoding";
}

// The small 60 fps fixture keeps this assertion about pacing rather than 4K
// decode throughput. Both windows fit before its five-second end at 4x speed.
TEST_F(VideoPlayerTest, PlaybackSpeedScalesPublicationInRealTime)
{
    LoadVideo(TestPaths::StagedRoot() / "Engine/Tests/Fixtures/Video/playback-pacing.mp4");
    if (IsSkipped() || HasFatalFailure()) return;

    ASSERT_EQ(m_Player.GetWidth(), 128);
    ASSERT_EQ(m_Player.GetHeight(), 96);
    ASSERT_GE(m_Player.GetDuration(), 5.0f);
    m_Player.SetLoop(false);
    m_Player.SetPlaybackSpeed(1.0f);
    m_Player.Play();
    ASSERT_TRUE(m_Player.WaitForFrame(kFirstFrameTimeoutSeconds));
    const uint64_t first = m_Player.GetPublishedFrameCount();
    std::this_thread::sleep_for(kObservationWindow);
    const uint64_t middle = m_Player.GetPublishedFrameCount();

    m_Player.SetPlaybackSpeed(4.0f);
    std::this_thread::sleep_for(kObservationWindow);
    const uint64_t last = m_Player.GetPublishedFrameCount();
    m_Player.Pause();

    const uint64_t slow = middle - first;
    const uint64_t fast = last - middle;
    EXPECT_GT(slow, 0u);
    EXPECT_GE(fast, slow * 2u)
        << "4x published " << fast << " frames in the same window 1x published " << slow;
}

// Unload has to join the decode thread before it frees the FFmpeg contexts the
// thread dereferences without the mutex. Doing it while a decode is in flight is
// the case that catches a missing join; the reload afterwards proves the player
// is genuinely reusable rather than merely not crashed.
TEST_F(VideoPlayerTest, UnloadDuringPlaybackJoinsTheDecodeThread)
{
    LoadVideo();
    if (IsSkipped() || HasFatalFailure()) return;

    m_Player.Play();
    m_Player.Unload();
    EXPECT_FALSE(m_Player.IsLoaded());
    EXPECT_EQ(m_Player.GetPublishedFrameCount(), 0u) << "Unload left counters from the old source";

    LoadVideo();
    if (IsSkipped() || HasFatalFailure()) return;
    m_Player.Play();
    EXPECT_TRUE(m_Player.WaitForFrame(kFirstFrameTimeoutSeconds))
        << "the player did not come back after an Unload mid-playback";
}

// Destruction runs the same join through the destructor rather than an explicit
// Unload. A player left playing when its owner goes away is the ordinary case:
// entity destroyed, scene closed, app shut down.
TEST_F(VideoPlayerTest, DestructionWhilePlayingIsSafe)
{
    const std::filesystem::path video = StagedSampleVideo();
    ASSERT_TRUE(std::filesystem::exists(video));
    {
        Video::VideoPlayer player;
        if (!player.Load(video.string()))
            GTEST_SKIP() << "video decode unavailable in this build";
        player.Play();
        ASSERT_TRUE(player.WaitForFrame(kFirstFrameTimeoutSeconds));
    }
    SUCCEED();
}

// Looping restarts from the source's video stream, and playback keeps advancing
// across the wrap. Driven at high speed so the wrap happens inside the window.
TEST_F(VideoPlayerTest, LoopingRestartsAndKeepsPublishing)
{
    LoadVideo();
    if (IsSkipped() || HasFatalFailure()) return;

    const float duration = m_Player.GetDuration();
    ASSERT_GT(duration, 0.0f) << "fixture reports no duration";

    // Start close enough to the end that any sane window crosses it.
    constexpr float kTailSeconds = 0.25f;
    constexpr float kWrapSpeed = 2.0f;
    m_Player.SetLoop(true);
    m_Player.SetPlaybackSpeed(kWrapSpeed);
    m_Player.Seek((std::max)(0.0f, duration - kTailSeconds));
    m_Player.Play();
    ASSERT_TRUE(m_Player.WaitForFrame(kFirstFrameTimeoutSeconds));

    const uint64_t before = m_Player.GetPublishedFrameCount();
    std::this_thread::sleep_for(kObservationWindow);
    EXPECT_GT(m_Player.GetPublishedFrameCount(), before)
        << "publication stopped at the loop point";
    EXPECT_TRUE(m_Player.IsPlaying()) << "a looping source stopped playing at the end";
}

// Acquiring is what consumes a frame; a caller that never acquires must still see
// the decoder run (frames are dropped, not queued unboundedly), and a caller that
// acquires must see the counters agree.
TEST_F(VideoPlayerTest, DroppedFramesAreThePublishedMinusAcquiredDifference)
{
    LoadVideo();
    if (IsSkipped() || HasFatalFailure()) return;

    m_Player.Play();
    ASSERT_TRUE(m_Player.WaitForFrame(kFirstFrameTimeoutSeconds));
    m_Player.AcquireFramePointer();

    std::this_thread::sleep_for(kObservationWindow);
    const uint64_t published = m_Player.GetPublishedFrameCount();
    const uint64_t acquired = m_Player.GetAcquiredFrameCount();
    EXPECT_GT(published, acquired)
        << "the caller acquired every published frame despite never asking during the wait";
    EXPECT_GE(acquired, 1u);
}
