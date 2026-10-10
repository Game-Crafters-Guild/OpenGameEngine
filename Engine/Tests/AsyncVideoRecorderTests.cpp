#include <gtest/gtest.h>

#include "Video/AsyncVideoRecorder.h"

#include <chrono>
#include <thread>

using namespace GameEngine::Video;

namespace
{

VideoWriterOptions MakeOptions()
{
    VideoWriterOptions opts;
    opts.path = "/tmp/ge_test_recording.mp4";
    opts.fps = 30.0;
    opts.recordAudio = true;
    opts.audioChannels = 2;
    opts.audioSampleRate = 48000;
    return opts;
}

OwnedAudioSamples MakeAudio(uint32_t channels = 2, uint32_t sampleRate = 48000)
{
    OwnedAudioSamples audio;
    audio.Samples = {0.0f, 0.0f};
    audio.FrameCount = 1;
    audio.Channels = channels;
    audio.SampleRate = sampleRate;
    return audio;
}

OwnedVideoFrame MakeVideoFrame(uint32_t width = 2, uint32_t height = 2)
{
    OwnedVideoFrame frame;
    frame.Width = width;
    frame.Height = height;
    frame.StrideBytes = width * 4u;
    frame.Format = VideoPixelFormat::BGRA8;
    frame.Pixels.assign(static_cast<size_t>(width) * height * 4u, 0u);
    return frame;
}

// A path whose directory cannot exist makes VideoWriter::Open fail on every
// platform, which is the only way to drive the encoder-failure transitions from a
// unit test — no codec, device or file system state required.
VideoWriterOptions MakeUnopenableOptions()
{
    VideoWriterOptions opts = MakeOptions();
    opts.path = "/ge_movie_recorder_test_missing_dir/ge_test_recording.mp4";
    return opts;
}

// The writer opens on the worker thread when it dequeues the first video frame,
// so the failure is observable only after a submission and only asynchronously.
bool WaitForRecorderFailure(const AsyncVideoRecorder& recorder)
{
    constexpr auto kTimeout = std::chrono::seconds(5);
    constexpr auto kPollInterval = std::chrono::milliseconds(1);
    const auto deadline = std::chrono::steady_clock::now() + kTimeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (recorder.GetStats().Failed)
            return true;
        std::this_thread::sleep_for(kPollInterval);
    }
    return false;
}

} // namespace

TEST(AsyncVideoRecorderTest, InitialStateIsFinalized)
{
    AsyncVideoRecorder recorder;
    EXPECT_TRUE(recorder.IsFinalized());

    const auto stats = recorder.GetStats();
    EXPECT_FALSE(stats.Running);
    EXPECT_TRUE(stats.Finalized);
    EXPECT_FALSE(stats.Failed);
}

TEST(AsyncVideoRecorderTest, StartSetsRunningState)
{
    AsyncVideoRecorder recorder;
    ASSERT_TRUE(recorder.Start(MakeOptions()));

    const auto stats = recorder.GetStats();
    EXPECT_TRUE(stats.Running);
    EXPECT_FALSE(stats.Finalized);

    recorder.CancelAndWait();
}

TEST(AsyncVideoRecorderTest, StartFailsWhenAlreadyRunning)
{
    AsyncVideoRecorder recorder;
    ASSERT_TRUE(recorder.Start(MakeOptions()));

    std::string error;
    EXPECT_FALSE(recorder.Start(MakeOptions(), {}, &error));
    EXPECT_FALSE(error.empty());

    recorder.CancelAndWait();
}

TEST(AsyncVideoRecorderTest, CancelAndWaitFinalizes)
{
    AsyncVideoRecorder recorder;
    ASSERT_TRUE(recorder.Start(MakeOptions()));
    recorder.CancelAndWait();

    EXPECT_TRUE(recorder.IsFinalized());

    const auto stats = recorder.GetStats();
    EXPECT_FALSE(stats.Running);
    EXPECT_TRUE(stats.Finalized);
}

TEST(AsyncVideoRecorderTest, SubmitAudioBeforeStartIsRejected)
{
    AsyncVideoRecorder recorder;
    OwnedAudioSamples audio = MakeAudio();
    EXPECT_EQ(recorder.SubmitAudioSamples(audio), SubmitResult::Rejected);
    EXPECT_FALSE(recorder.CanAcceptVideoFrame());
}

TEST(AsyncVideoRecorderTest, SubmitAudioAfterCancelIsRejected)
{
    AsyncVideoRecorder recorder;
    ASSERT_TRUE(recorder.Start(MakeOptions()));
    recorder.CancelAndWait();
    OwnedAudioSamples audio = MakeAudio();
    EXPECT_EQ(recorder.SubmitAudioSamples(audio), SubmitResult::Rejected);
}

TEST(AsyncVideoRecorderTest, AudioFormatMismatch_WrongChannelCount)
{
    AsyncVideoRecorder recorder;
    ASSERT_TRUE(recorder.Start(MakeOptions()));  // expects 2 channels

    OwnedAudioSamples audio = MakeAudio(1, 48000);  // 1 channel
    EXPECT_EQ(recorder.SubmitAudioSamples(audio), SubmitResult::Rejected);

    const auto stats = recorder.GetStats();
    EXPECT_EQ(stats.DroppedAudioChunks, 1u);

    recorder.CancelAndWait();
}

TEST(AsyncVideoRecorderTest, AudioFormatMismatch_WrongSampleRate)
{
    AsyncVideoRecorder recorder;
    ASSERT_TRUE(recorder.Start(MakeOptions()));  // expects 48000 Hz

    OwnedAudioSamples audio = MakeAudio(2, 44100);  // 44100 Hz
    EXPECT_EQ(recorder.SubmitAudioSamples(audio), SubmitResult::Rejected);

    const auto stats = recorder.GetStats();
    EXPECT_EQ(stats.DroppedAudioChunks, 1u);

    recorder.CancelAndWait();
}

TEST(AsyncVideoRecorderTest, AudioFormatMismatch_SetsLastError)
{
    AsyncVideoRecorder recorder;
    ASSERT_TRUE(recorder.Start(MakeOptions()));

    OwnedAudioSamples audio = MakeAudio(1, 44100);
    recorder.SubmitAudioSamples(audio);

    const auto stats = recorder.GetStats();
    EXPECT_FALSE(stats.LastError.empty());
    EXPECT_NE(stats.LastError.find("mismatch"), std::string::npos);

    recorder.CancelAndWait();
}

TEST(AsyncVideoRecorderTest, AudioFormatMismatch_FiresQueueExhaustedCallback)
{
    int callbackCount = 0;
    AsyncVideoRecorderOptions recorderOpts;
    recorderOpts.OnQueueExhausted = [&callbackCount]() { ++callbackCount; };

    AsyncVideoRecorder recorder;
    ASSERT_TRUE(recorder.Start(MakeOptions(), recorderOpts));

    OwnedAudioSamples audio = MakeAudio(1, 48000);  // wrong channels
    recorder.SubmitAudioSamples(audio);

    EXPECT_EQ(callbackCount, 1);

    recorder.CancelAndWait();
}

TEST(AsyncVideoRecorderTest, ValidAudioAccepted)
{
    AsyncVideoRecorder recorder;
    ASSERT_TRUE(recorder.Start(MakeOptions()));

    OwnedAudioSamples audio = MakeAudio(2, 48000);
    EXPECT_EQ(recorder.SubmitAudioSamples(audio), SubmitResult::Accepted);

    const auto stats = recorder.GetStats();
    EXPECT_EQ(stats.SubmittedAudioChunks, 1u);
    EXPECT_EQ(stats.DroppedAudioChunks, 0u);

    recorder.CancelAndWait();
}

TEST(AsyncVideoRecorderTest, QueueExhaustedCallback_NotFiredOnValidAudio)
{
    int callbackCount = 0;
    AsyncVideoRecorderOptions recorderOpts;
    recorderOpts.OnQueueExhausted = [&callbackCount]() { ++callbackCount; };

    AsyncVideoRecorder recorder;
    ASSERT_TRUE(recorder.Start(MakeOptions(), recorderOpts));

    OwnedAudioSamples audio = MakeAudio(2, 48000);
    recorder.SubmitAudioSamples(audio);

    EXPECT_EQ(callbackCount, 0);

    recorder.CancelAndWait();
}

TEST(AsyncVideoRecorderTest, CanAcceptVideoFrameFalseAfterCancel)
{
    AsyncVideoRecorder recorder;
    ASSERT_TRUE(recorder.Start(MakeOptions()));
    EXPECT_TRUE(recorder.CanAcceptVideoFrame());

    recorder.CancelAndWait();
    EXPECT_FALSE(recorder.CanAcceptVideoFrame());
}

TEST(AsyncVideoRecorderTest, StatsTrackMultipleDrops)
{
    AsyncVideoRecorder recorder;
    ASSERT_TRUE(recorder.Start(MakeOptions()));  // expects 2ch 48000Hz

    for (int i = 0; i < 5; ++i)
    {
        OwnedAudioSamples audio = MakeAudio(1, 48000);  // wrong channels
        recorder.SubmitAudioSamples(audio);
    }

    const auto stats = recorder.GetStats();
    EXPECT_EQ(stats.DroppedAudioChunks, 5u);
    EXPECT_EQ(stats.SubmittedAudioChunks, 0u);

    recorder.CancelAndWait();
}

TEST(AsyncVideoRecorderTest, StartAgainAfterCancelSucceeds)
{
    AsyncVideoRecorder recorder;
    ASSERT_TRUE(recorder.Start(MakeOptions()));
    recorder.CancelAndWait();

    ASSERT_TRUE(recorder.Start(MakeOptions()));

    const auto stats = recorder.GetStats();
    EXPECT_TRUE(stats.Running);

    recorder.CancelAndWait();
}

TEST(AsyncVideoRecorderTest, SubmitVideoFrameBeforeStartIsRejected)
{
    AsyncVideoRecorder recorder;
    OwnedVideoFrame frame = MakeVideoFrame();
    EXPECT_EQ(recorder.SubmitVideoFrame(frame), SubmitResult::Rejected);
    // Rejected leaves the frame intact for the caller.
    EXPECT_FALSE(frame.Pixels.empty());
}

TEST(AsyncVideoRecorderTest, SubmitVideoFrameAfterCancelIsRejected)
{
    AsyncVideoRecorder recorder;
    ASSERT_TRUE(recorder.Start(MakeOptions()));
    recorder.CancelAndWait();

    OwnedVideoFrame frame = MakeVideoFrame();
    EXPECT_EQ(recorder.SubmitVideoFrame(frame), SubmitResult::Rejected);
    EXPECT_FALSE(frame.Pixels.empty());
}

TEST(AsyncVideoRecorderTest, InvalidVideoFrameIsRejected)
{
    AsyncVideoRecorder recorder;
    ASSERT_TRUE(recorder.Start(MakeOptions()));

    OwnedVideoFrame frame{};  // empty pixels
    EXPECT_EQ(recorder.SubmitVideoFrame(frame), SubmitResult::Rejected);

    recorder.CancelAndWait();
}

TEST(AsyncVideoRecorderTest, ValidVideoFrameAcceptedAndConsumed)
{
    AsyncVideoRecorder recorder;
    ASSERT_TRUE(recorder.Start(MakeOptions()));

    OwnedVideoFrame frame = MakeVideoFrame();
    EXPECT_EQ(recorder.SubmitVideoFrame(frame), SubmitResult::Accepted);
    // Accepted moves the pixels into the queue.
    EXPECT_TRUE(frame.Pixels.empty());

    recorder.CancelAndWait();
}

// Regression for the recording-stops-early bug: a full encoder queue is transient
// backpressure, never a silent drop. Video frames must never be counted as dropped,
// and a QueueFull result must leave the frame intact so the caller can retry it.
TEST(AsyncVideoRecorderTest, FullQueueNeverDropsVideoFrames)
{
    AsyncVideoRecorder recorder(/*maxQueuedItems=*/2);
    ASSERT_TRUE(recorder.Start(MakeOptions()));

    for (int i = 0; i < 256; ++i)
    {
        OwnedVideoFrame frame = MakeVideoFrame();
        const SubmitResult result = recorder.SubmitVideoFrame(frame);
        if (result == SubmitResult::Rejected)
        {
            // The only legitimate rejection here is a terminal encoder failure,
            // which is what builds without a video writer hit on the first frame.
            // Backpressure must never present as a rejection.
            EXPECT_TRUE(recorder.GetStats().Failed);
            break;
        }
        if (result == SubmitResult::QueueFull)
        {
            // Backpressure must preserve the frame for a retry, not consume it.
            EXPECT_FALSE(frame.Pixels.empty());
        }
        else
        {
            EXPECT_EQ(result, SubmitResult::Accepted);
            EXPECT_TRUE(frame.Pixels.empty());
        }
    }

    const auto stats = recorder.GetStats();
    EXPECT_EQ(stats.DroppedVideoFrames, 0u);

    recorder.CancelAndWait();
}

// A full encoder queue must back-pressure audio (caller buffers and retries),
// never silently drop it — dropped audio tears holes in the recording. A QueueFull
// result must leave the samples intact; a format mismatch stays a counted drop.
TEST(AsyncVideoRecorderTest, FullQueueNeverDropsValidAudio)
{
    AsyncVideoRecorder recorder(/*maxQueuedItems=*/2);
    ASSERT_TRUE(recorder.Start(MakeOptions()));

    for (int i = 0; i < 256; ++i)
    {
        OwnedAudioSamples audio = MakeAudio(2, 48000);
        const SubmitResult result = recorder.SubmitAudioSamples(audio);
        EXPECT_NE(result, SubmitResult::Rejected);
        if (result == SubmitResult::QueueFull)
        {
            // Backpressure must preserve the samples for a retry, not consume them.
            EXPECT_FALSE(audio.Samples.empty());
        }
        else
        {
            EXPECT_EQ(result, SubmitResult::Accepted);
            EXPECT_TRUE(audio.Samples.empty());
        }
    }

    const auto stats = recorder.GetStats();
    EXPECT_EQ(stats.DroppedAudioChunks, 0u);

    recorder.CancelAndWait();
}

// Regression for the silent-discard bug: a writer that cannot open is terminal, but
// the recorder used to keep accepting frames forever. The host then paid full
// capture cost — render, encode, GPU readback — every frame for a file that could
// never exist, and its "encoder stopped" path never triggered because acceptance
// never stopped. A failed recorder must REJECT, so the host can end the capture.
TEST(AsyncVideoRecorderTest, WriterFailureStopsAcceptingVideoFrames)
{
    AsyncVideoRecorder recorder;
    ASSERT_TRUE(recorder.Start(MakeUnopenableOptions()));

    // The first frame is what makes the worker try to open the writer.
    OwnedVideoFrame first = MakeVideoFrame();
    ASSERT_EQ(recorder.SubmitVideoFrame(first), SubmitResult::Accepted);
    ASSERT_TRUE(WaitForRecorderFailure(recorder));

    OwnedVideoFrame afterFailure = MakeVideoFrame();
    EXPECT_EQ(recorder.SubmitVideoFrame(afterFailure), SubmitResult::Rejected);
    // Rejected must not consume the frame, so the caller can report it, not lose it.
    EXPECT_FALSE(afterFailure.Pixels.empty());
    EXPECT_FALSE(recorder.CanAcceptVideoFrame());

    recorder.CancelAndWait();
}

TEST(AsyncVideoRecorderTest, WriterFailureStopsAcceptingAudio)
{
    AsyncVideoRecorder recorder;
    ASSERT_TRUE(recorder.Start(MakeUnopenableOptions()));

    OwnedVideoFrame first = MakeVideoFrame();
    ASSERT_EQ(recorder.SubmitVideoFrame(first), SubmitResult::Accepted);
    ASSERT_TRUE(WaitForRecorderFailure(recorder));

    OwnedAudioSamples audio = MakeAudio();
    EXPECT_EQ(recorder.SubmitAudioSamples(audio), SubmitResult::Rejected);
    EXPECT_FALSE(audio.Samples.empty());

    recorder.CancelAndWait();
}

// Why acceptance stops on `failed` and not by clearing `running`: Finish() returns
// early when the recorder is not running, so it would never enqueue its item, the
// worker would never exit, and the failure text would never reach the host. This
// pins the reachability that choice preserves.
TEST(AsyncVideoRecorderTest, WriterFailureStillFinalizesWithErrorText)
{
    AsyncVideoRecorder recorder;
    ASSERT_TRUE(recorder.Start(MakeUnopenableOptions()));

    OwnedVideoFrame first = MakeVideoFrame();
    ASSERT_EQ(recorder.SubmitVideoFrame(first), SubmitResult::Accepted);
    ASSERT_TRUE(WaitForRecorderFailure(recorder));

    bool finishOk = true;
    std::string finishMessage;
    recorder.Finish({}, "unused success message",
                    [&](bool ok, const std::string& message)
                    {
                        finishOk = ok;
                        finishMessage = message;
                    },
                    /*wait=*/true);

    EXPECT_TRUE(recorder.IsFinalized());
    EXPECT_FALSE(finishOk);
    // The REAL failure text, not the success message the caller passed: a
    // failed recording that reports "Recording finished." is worse than one
    // that reports nothing.
    EXPECT_NE(finishMessage.find("Movie capture failed"), std::string::npos)
        << "callback message was: '" << finishMessage << "'";
    EXPECT_EQ(finishMessage.find("unused success message"), std::string::npos)
        << "a failed recording reported the caller's success message";
    EXPECT_TRUE(recorder.GetStats().Failed);
}
