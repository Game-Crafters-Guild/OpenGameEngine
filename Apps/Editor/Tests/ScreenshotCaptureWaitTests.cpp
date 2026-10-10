#include "DebugServer/ScreenshotCaptureWait.h"

#include <gtest/gtest.h>

#include <string>

namespace
{

using GameEngine::Editor::ScreenshotCaptureWait;
using GameEngine::Editor::ScreenshotFailureMessage;
using Outcome = GameEngine::Editor::ScreenshotCaptureWait::Outcome;

constexpr double kDeadline = GameEngine::Editor::kScreenshotWaitDeadlineSeconds;
constexpr int kMaxRefusals = GameEngine::Editor::kScreenshotMaxRefusedAttempts;

constexpr const char* kPaneRefusal =
    "the scene view produced no render-graph output — its tab may be behind another";

constexpr const char* kAbandonedRefusal =
    "the frame that declared the readback was abandoned before submit";

bool Mentions(const std::string& text, const std::string& fragment)
{
    return text.find(fragment) != std::string::npos;
}

// A capture request polls once per main-loop tick, and a tick does not have to
// render. Polling alone must never end the wait, or a starved frame loop reads
// as a failed capture.
TEST(ScreenshotCaptureWaitTests, PollingAloneNeverEndsAWaitThatSawNoFrame)
{
    ScreenshotCaptureWait wait(/*allowWindowCapture=*/false);
    for (int poll = 0; poll < 500; ++poll)
        ASSERT_EQ(Outcome::KeepWaiting, wait.Step(0.001 * poll)) << "poll " << poll;
    EXPECT_TRUE(wait.GiveUpReason().empty());
}

// A scene view that is a frame behind on extraction declines for a frame or two
// and then declares. Those frames are retries, not a failure.
TEST(ScreenshotCaptureWaitTests, AFewRefusalsAreRetriedRatherThanReported)
{
    ScreenshotCaptureWait wait(/*allowWindowCapture=*/false);
    for (int attempt = 0; attempt < kMaxRefusals - 1; ++attempt)
    {
        wait.RecordRefusal(kPaneRefusal);
        ASSERT_EQ(Outcome::KeepWaiting, wait.Step(0.02 * attempt)) << "attempt " << attempt;
    }
    wait.RecordReadbackDeclared();
    EXPECT_EQ(Outcome::KeepWaiting, wait.Step(0.2));
}

// A pane with no on-screen rect does not heal on its own: report it as soon as
// the refusal has repeated, and report the render phase's own reason.
TEST(ScreenshotCaptureWaitTests, ARepeatedRefusalIsReportedWithTheRenderPhasesOwnReason)
{
    ScreenshotCaptureWait wait(/*allowWindowCapture=*/false);
    Outcome outcome = Outcome::KeepWaiting;
    for (int attempt = 0; attempt < kMaxRefusals; ++attempt)
    {
        wait.RecordRefusal(kPaneRefusal);
        outcome = wait.Step(0.02 * attempt);
    }
    EXPECT_EQ(Outcome::Fail, outcome);
    EXPECT_TRUE(Mentions(wait.GiveUpReason(), kPaneRefusal)) << wait.GiveUpReason();
    EXPECT_TRUE(Mentions(wait.GiveUpReason(), "rendered frames")) << wait.GiveUpReason();
}

// Nothing observed at all means the editor never reached its render phase.
TEST(ScreenshotCaptureWaitTests, AWaitThatSawNoFrameNamesTheStarvedRenderLoop)
{
    ScreenshotCaptureWait wait(/*allowWindowCapture=*/false);
    ASSERT_EQ(Outcome::KeepWaiting, wait.Step(kDeadline - 0.1));
    EXPECT_EQ(Outcome::Fail, wait.Step(kDeadline + 0.1));
    EXPECT_TRUE(Mentions(wait.GiveUpReason(), "rendered no frame")) << wait.GiveUpReason();
}

// A window with no framebuffer is not a slow frame, it is no frame at all.
TEST(ScreenshotCaptureWaitTests, ABlockedWindowEndsTheWaitWithoutServingTheDeadline)
{
    ScreenshotCaptureWait wait(/*allowWindowCapture=*/false);
    wait.RecordBlocked("the editor window has no framebuffer");
    EXPECT_EQ(Outcome::Fail, wait.Step(0.0));
    EXPECT_EQ("the editor window has no framebuffer", wait.GiveUpReason());
}

// The substitute is a picture of the window, not the frame: it is served only to
// a caller that asked for it, and the reason travels with it either way.
TEST(ScreenshotCaptureWaitTests, AWindowCaptureIsServedOnlyWhenTheRequestAllowedIt)
{
    ScreenshotCaptureWait refuses(/*allowWindowCapture=*/false);
    refuses.RecordBlocked("the editor window has no framebuffer");
    EXPECT_EQ(Outcome::Fail, refuses.Step(0.0));

    ScreenshotCaptureWait allows(/*allowWindowCapture=*/true);
    allows.RecordBlocked("the editor window has no framebuffer");
    EXPECT_EQ(Outcome::ServeWindowCapture, allows.Step(0.0));
    EXPECT_EQ(refuses.GiveUpReason(), allows.GiveUpReason());
}

// Once the readback is declared the wait is on the GPU, and says so.
TEST(ScreenshotCaptureWaitTests, ADeclaredReadbackWaitsOnTheGpuUntilTheDeadline)
{
    ScreenshotCaptureWait wait(/*allowWindowCapture=*/false);
    wait.RecordReadbackDeclared();
    ASSERT_EQ(Outcome::KeepWaiting, wait.Step(kDeadline - 0.1));
    EXPECT_EQ(Outcome::Fail, wait.Step(kDeadline + 0.1));
    EXPECT_TRUE(Mentions(wait.GiveUpReason(), "GPU readback")) << wait.GiveUpReason();
}

// A readback whose frame was abandoned leaves the request where it started, so
// the next frame gets a fresh attempt rather than an error.
TEST(ScreenshotCaptureWaitTests, AnAbandonedReadbackGoesBackToWaitingForAFrame)
{
    ScreenshotCaptureWait wait(/*allowWindowCapture=*/false);
    wait.RecordReadbackDeclared();
    wait.RecordRefusal("the frame that declared the readback was abandoned before submit");
    EXPECT_EQ(Outcome::KeepWaiting, wait.Step(0.1));

    for (int attempt = 1; attempt < kMaxRefusals; ++attempt)
        wait.RecordRefusal("the frame that declared the readback was abandoned before submit");
    EXPECT_EQ(Outcome::Fail, wait.Step(0.2));
    EXPECT_TRUE(Mentions(wait.GiveUpReason(), "abandoned before submit")) << wait.GiveUpReason();
}

// "unchanged over N rendered frames" is a claim about one refusal repeating, so
// refusals that keep changing must not accumulate into it.
TEST(ScreenshotCaptureWaitTests, ARefusalThatChangesRestartsTheRepeatCount)
{
    ScreenshotCaptureWait wait(/*allowWindowCapture=*/false);
    for (int attempt = 0; attempt < kMaxRefusals * 2; ++attempt)
    {
        wait.RecordRefusal(attempt % 2 == 0 ? kPaneRefusal : kAbandonedRefusal);
        ASSERT_EQ(Outcome::KeepWaiting, wait.Step(0.01 * attempt)) << "attempt " << attempt;
    }
    ASSERT_TRUE(wait.GiveUpReason().empty()) << wait.GiveUpReason();

    // Once one reason does repeat, the count reports that run rather than every
    // frame the request has seen.
    for (int attempt = 0; attempt < kMaxRefusals; ++attempt)
        wait.RecordRefusal(kPaneRefusal);
    EXPECT_EQ(Outcome::Fail, wait.Step(0.2));
    EXPECT_TRUE(Mentions(wait.GiveUpReason(),
                         "unchanged over " + std::to_string(kMaxRefusals) + " rendered frames"))
        << wait.GiveUpReason();
}

// One rendered frame inside the whole deadline is a starved frame loop, not a
// pane refusing over and over.
TEST(ScreenshotCaptureWaitTests, ASingleRefusalAtTheDeadlineSaysOnlyOneFrameRendered)
{
    ScreenshotCaptureWait wait(/*allowWindowCapture=*/false);
    wait.RecordRefusal(kPaneRefusal);
    ASSERT_EQ(Outcome::KeepWaiting, wait.Step(kDeadline - 0.1));
    EXPECT_EQ(Outcome::Fail, wait.Step(kDeadline + 0.1));
    EXPECT_TRUE(Mentions(wait.GiveUpReason(), kPaneRefusal)) << wait.GiveUpReason();
    EXPECT_TRUE(Mentions(wait.GiveUpReason(), "rendered one frame")) << wait.GiveUpReason();
    EXPECT_FALSE(Mentions(wait.GiveUpReason(), "1 rendered frames")) << wait.GiveUpReason();
}

// A caller that did not ask for the substitute is told the option exists.
TEST(ScreenshotCaptureWaitTests, AFailureAWindowCaptureCouldServeNamesTheOptInFlag)
{
    const std::string message = ScreenshotFailureMessage(kPaneRefusal, /*offerWindowCapture=*/true);
    EXPECT_TRUE(Mentions(message, kPaneRefusal)) << message;
    EXPECT_TRUE(Mentions(message, "allowWindowCapture=true")) << message;
}

// The failures that survive the opt-in — a minimized window whose OS capture
// also failed, and asset_preview, which has no substitute at all — must not
// answer with a flag the caller either already passed or cannot use.
TEST(ScreenshotCaptureWaitTests, AFailureNoWindowCaptureCanServeDoesNotNameTheOptInFlag)
{
    const std::string message = ScreenshotFailureMessage(
        "the editor window has no framebuffer (minimized or hidden), so it renders no frame to "
        "capture; the window capture also failed (non-Windows platform, hidden window, or capture "
        "error)",
        /*offerWindowCapture=*/false);
    EXPECT_TRUE(Mentions(message, "the window capture also failed")) << message;
    EXPECT_FALSE(Mentions(message, "allowWindowCapture")) << message;
}

} // namespace
