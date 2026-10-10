#pragma once

#include "Video/VideoWriter.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace GameEngine::Video
{

struct AsyncVideoRecorderOptions
{
    std::function<void()> OnQueueExhausted;  // Called when invalid data is rejected.
};

struct OwnedVideoFrame
{
    std::vector<uint8_t> Pixels;
    uint32_t Width = 0;
    uint32_t Height = 0;
    uint32_t StrideBytes = 0;
    VideoPixelFormat Format = VideoPixelFormat::RGBA8;
    // Optional presentation time. Negative uses the fixed frameIndex / fps timeline.
    double TimestampSeconds = -1.0;
};

struct OwnedAudioSamples
{
    std::vector<float> Samples;
    uint32_t FrameCount = 0;
    uint32_t Channels = 0;
    uint32_t SampleRate = 0;
};

// Result of a video/audio submission. QueueFull is transient backpressure (the
// encoder is behind): the item is left untouched so the caller can retry it,
// not a failure. Rejected means the recorder is stopped or the item is invalid.
enum class SubmitResult
{
    Accepted,
    QueueFull,
    Rejected,
};

struct AsyncVideoRecorderStats
{
    size_t QueuedItems = 0;
    size_t QueuedVideoFrames = 0;
    size_t MaxQueuedItems = 0;
    uint64_t SubmittedVideoFrames = 0;
    uint64_t EncodedVideoFrames = 0;
    uint64_t SubmittedAudioChunks = 0;
    uint64_t EncodedAudioChunks = 0;
    uint64_t DroppedVideoFrames = 0;
    uint64_t DroppedAudioChunks = 0;
    bool Running = false;
    bool Finishing = false;
    bool Finalized = false;
    bool Failed = false;
    std::string LastError;
};

class AsyncVideoRecorder
{
public:
    using FinishCallback = std::function<void(bool ok, const std::string& message)>;

    explicit AsyncVideoRecorder(size_t maxQueuedItems = 128);
    ~AsyncVideoRecorder();

    AsyncVideoRecorder(const AsyncVideoRecorder&) = delete;
    AsyncVideoRecorder& operator=(const AsyncVideoRecorder&) = delete;

    bool Start(VideoWriterOptions options, const AsyncVideoRecorderOptions& recorderOptions = {}, std::string* outError = nullptr);
    // On QueueFull the frame is left intact so the caller can hold it and retry.
    SubmitResult SubmitVideoFrame(OwnedVideoFrame& frame);
    // On QueueFull the samples are left intact so the caller can buffer and retry.
    // A format mismatch is Rejected (and counted as a dropped chunk).
    SubmitResult SubmitAudioSamples(OwnedAudioSamples& audio);
    bool CanAcceptVideoFrame() const;
    // Finish recording. endFadeFrame is used to generate the fade-out effect if fadeOutEnabled is true.
    // If fadeOutEnabled but endFadeFrame has no pixels, the recording stops abruptly without fade-out.
    // For smooth endings with fade-out, endFadeFrame should contain a valid frame.
    void Finish(OwnedVideoFrame endFadeFrame, std::string successMessage, FinishCallback callback, bool wait,
                bool trimPendingFrames = false);
    void WaitForFinalization();
    void CancelAndWait();

    bool IsFinalized() const;
    AsyncVideoRecorderStats GetStats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

} // namespace GameEngine::Video
