#include "Video/AsyncVideoRecorder.h"

#include "Logger/Logger.h"

#include <condition_variable>
#include <deque>
#include <algorithm>
#include <thread>
#include <utility>

namespace GameEngine::Video
{

namespace
{
enum class RecorderItemType
{
    Video,
    Audio,
    Finish,
    Cancel
};

struct RecorderItem
{
    RecorderItemType type = RecorderItemType::Video;
    OwnedVideoFrame video;
    OwnedAudioSamples audio;
    OwnedVideoFrame endFadeFrame;
    std::string message;
    AsyncVideoRecorder::FinishCallback callback;
};

bool IsValidVideoFrame(const OwnedVideoFrame& frame)
{
    return !frame.Pixels.empty() && frame.Width > 0 && frame.Height > 0 && frame.StrideBytes > 0;
}

bool IsValidAudioSamples(const OwnedAudioSamples& audio)
{
    return !audio.Samples.empty() && audio.FrameCount > 0 && audio.Channels > 0 && audio.SampleRate > 0;
}

std::string FormatAudioSpec(uint32_t channels, uint32_t sampleRate)
{
    return std::to_string(channels) + "ch " + std::to_string(sampleRate) + "Hz";
}
} // namespace

struct AsyncVideoRecorder::Impl
{
    explicit Impl(size_t maxQueuedItems)
        : maxQueuedItems(std::max<size_t>(1, maxQueuedItems))
    {
    }

    ~Impl()
    {
        CancelAndWait();
    }

    bool Start(VideoWriterOptions startOptions, const AsyncVideoRecorderOptions& recorderOptions, std::string* outError)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (running && !finalized)
        {
            if (outError)
            {
                *outError = "Recorder is already running.";
            }
            return false;
        }

        options = std::move(startOptions);
        onQueueExhausted = recorderOptions.OnQueueExhausted;
        queue.clear();
        pendingAudio.clear();
        queuedVideoFrames = 0;
        submittedVideoFrames = 0;
        encodedVideoFrames = 0;
        submittedAudioChunks = 0;
        encodedAudioChunks = 0;
        droppedVideoFrames = 0;
        droppedAudioChunks = 0;
        maxObservedQueuedItems = 0;
        lastError.clear();
        failed = false;
        running = true;
        finishing = false;
        finalized = false;
        cancelRequested = false;
        writerOpened = false;
        expectedAudioChannels = options.audioChannels;
        expectedAudioSampleRate = options.audioSampleRate;

        worker = std::thread([this]() { WorkerMain(); });
        return true;
    }

    SubmitResult SubmitVideoFrame(OwnedVideoFrame& frame)
    {
        if (!IsValidVideoFrame(frame))
        {
            return SubmitResult::Rejected;
        }

        std::lock_guard<std::mutex> lock(mutex);
        // A failed writer is terminal — nothing submitted after it can reach a
        // file — so acceptance stops here. Rejected (never QueueFull) is what
        // tells the host to end the capture instead of retrying forever; a host
        // that kept submitting would pay full capture cost for discarded frames.
        // `running` deliberately stays set: Finish() early-returns on !running
        // and would never enqueue its item, leaving the worker unjoined and the
        // failure text unreported.
        if (!running || failed || finishing || finalized || cancelRequested)
        {
            return SubmitResult::Rejected;
        }
        // A full queue is transient backpressure: leave the frame untouched so the
        // caller can retry it next poll. Dropping it here would lose footage; aborting
        // would end the recording on a momentary encoder stall.
        if (queue.size() >= maxQueuedItems)
        {
            return SubmitResult::QueueFull;
        }

        RecorderItem item{};
        item.type = RecorderItemType::Video;
        item.video = std::move(frame);
        queue.push_back(std::move(item));
        ++queuedVideoFrames;
        ++submittedVideoFrames;
        maxObservedQueuedItems = std::max(maxObservedQueuedItems, queue.size());
        cv.notify_one();
        return SubmitResult::Accepted;
    }

    SubmitResult SubmitAudioSamples(OwnedAudioSamples& audio)
    {
        if (!IsValidAudioSamples(audio))
        {
            return SubmitResult::Rejected;
        }

        std::lock_guard<std::mutex> lock(mutex);
        if (!running || failed || finishing || finalized || cancelRequested)
        {
            return SubmitResult::Rejected;
        }

        // A format mismatch is bad data, not backpressure: drop it and report.
        if (audio.Channels != expectedAudioChannels || audio.SampleRate != expectedAudioSampleRate)
        {
            lastError = "Audio format mismatch: expected " + FormatAudioSpec(expectedAudioChannels, expectedAudioSampleRate) +
                        ", got " + FormatAudioSpec(audio.Channels, audio.SampleRate);
            ++droppedAudioChunks;
            if (onQueueExhausted)
            {
                onQueueExhausted();
            }
            return SubmitResult::Rejected;
        }

        // A full queue is transient backpressure: leave the samples untouched so the
        // caller can buffer and retry. Dropping here would tear holes in the audio.
        if (queue.size() >= maxQueuedItems)
        {
            return SubmitResult::QueueFull;
        }

        RecorderItem item{};
        item.type = RecorderItemType::Audio;
        item.audio = std::move(audio);
        queue.push_back(std::move(item));
        ++submittedAudioChunks;
        maxObservedQueuedItems = std::max(maxObservedQueuedItems, queue.size());
        cv.notify_one();
        return SubmitResult::Accepted;
    }

    bool CanAcceptVideoFrame() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return running && !failed && !finishing && !finalized && !cancelRequested &&
               queue.size() < maxQueuedItems;
    }

    void Finish(OwnedVideoFrame endFadeFrame, std::string successMessage, FinishCallback callback, bool wait,
                bool trimPendingFrames)
    {
        std::thread joinThread;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!running || finalized)
            {
                if (callback)
                {
                    callback(!failed, failed ? lastError : successMessage);
                }
                return;
            }
            if (finishing)
            {
                // A finish request is already queued; callers may still wait for it.
                callback = nullptr;
            }
            else
            {
                if (trimPendingFrames)
                {
                    queue.clear();
                    queuedVideoFrames = 0;
                }

                finishing = true;
                RecorderItem item{};
                item.type = RecorderItemType::Finish;
                item.endFadeFrame = std::move(endFadeFrame);
                item.message = std::move(successMessage);
                item.callback = std::move(callback);
                queue.push_back(std::move(item));
                maxObservedQueuedItems = std::max(maxObservedQueuedItems, queue.size());
                cv.notify_one();
            }
        }

        if (wait)
        {
            JoinWorker();
        }
    }

    void CancelAndWait()
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!running && finalized)
            {
                // Nothing to do; the worker has already gone away.
            }
            else
            {
                cancelRequested = true;
                finishing = true;
                queue.clear();
                pendingAudio.clear();
                queuedVideoFrames = 0;
                RecorderItem item{};
                item.type = RecorderItemType::Cancel;
                queue.push_back(std::move(item));
                cv.notify_one();
            }
        }
        JoinWorker();
    }

    void WaitForFinalization()
    {
        JoinWorker();
    }

    bool IsFinalized() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return finalized;
    }

    AsyncVideoRecorderStats GetStats() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        AsyncVideoRecorderStats stats{};
        stats.QueuedItems = queue.size();
        stats.QueuedVideoFrames = queuedVideoFrames;
        stats.MaxQueuedItems = maxObservedQueuedItems;
        stats.SubmittedVideoFrames = submittedVideoFrames;
        stats.EncodedVideoFrames = encodedVideoFrames;
        stats.SubmittedAudioChunks = submittedAudioChunks;
        stats.EncodedAudioChunks = encodedAudioChunks;
        stats.DroppedVideoFrames = droppedVideoFrames;
        stats.DroppedAudioChunks = droppedAudioChunks;
        stats.Running = running;
        stats.Finishing = finishing;
        stats.Finalized = finalized;
        stats.Failed = failed;
        stats.LastError = lastError;
        return stats;
    }

    void JoinWorker()
    {
        if (worker.joinable())
        {
            worker.join();
        }
    }

    void WorkerMain()
    {
        VideoWriter writer;
        bool shouldExit = false;

        while (!shouldExit)
        {
            RecorderItem item{};
            {
                std::unique_lock<std::mutex> lock(mutex);
                cv.wait(lock, [this]() { return !queue.empty(); });
                item = std::move(queue.front());
                queue.pop_front();
                if (item.type == RecorderItemType::Video && queuedVideoFrames > 0)
                {
                    --queuedVideoFrames;
                }
            }

            switch (item.type)
            {
            case RecorderItemType::Video:
                HandleVideo(writer, std::move(item.video));
                break;
            case RecorderItemType::Audio:
                HandleAudio(writer, std::move(item.audio));
                break;
            case RecorderItemType::Finish:
                HandleFinish(writer, std::move(item.endFadeFrame), std::move(item.message), std::move(item.callback));
                shouldExit = true;
                break;
            case RecorderItemType::Cancel:
                if (writer.IsOpen())
                {
                    writer.Close();
                }
                MarkFinalized(false, "Recording canceled.");
                shouldExit = true;
                break;
            }
        }
    }

    void HandleVideo(VideoWriter& writer, OwnedVideoFrame frame)
    {
        if (!EnsureWriterOpen(writer, frame.Width, frame.Height))
        {
            return;
        }

        DrainPendingAudio(writer);

        if (!WriteVideoFrame(writer, frame))
            return;
    }

    void HandleAudio(VideoWriter& writer, OwnedAudioSamples audio)
    {
        if (!writerOpened)
        {
            constexpr size_t kMaxPendingAudioBytes = 10 * 1024 * 1024;
            size_t currentBytes = 0;
            for (const auto& pending : pendingAudio)
            {
                currentBytes += pending.Samples.size() * sizeof(float);
            }

            if (currentBytes + audio.Samples.size() * sizeof(float) > kMaxPendingAudioBytes)
            {
                MarkFailed("Too much audio submitted before first video frame (exceeds 10MB buffer)");
                return;
            }
            pendingAudio.push_back(std::move(audio));
            return;
        }
        WriteAudio(writer, audio);
    }

    void HandleFinish(VideoWriter& writer,
                      OwnedVideoFrame endFadeFrame,
                      std::string successMessage,
                      FinishCallback callback)
    {
        bool ok = true;
        std::string message = std::move(successMessage);
        {
            std::lock_guard<std::mutex> lock(mutex);
            ok = !failed;
            // A failed recording reports its failure text, never the success
            // message the caller optimistically passed in.
            if (!ok && !lastError.empty())
            {
                message = lastError;
            }
        }

        if (writer.IsOpen())
        {
            DrainPendingAudio(writer);
            if (ok && IsValidVideoFrame(endFadeFrame) && !writer.ExtendToTimestamp(endFadeFrame.TimestampSeconds))
            {
                ok = false;
                message = "Movie finalization failed: " + writer.GetLastError();
            }
            if (ok && IsValidVideoFrame(endFadeFrame))
            {
                VideoFrameView view{};
                view.Pixels = endFadeFrame.Pixels.data();
                view.Width = endFadeFrame.Width;
                view.Height = endFadeFrame.Height;
                view.StrideBytes = endFadeFrame.StrideBytes;
                view.Format = endFadeFrame.Format;
                view.TimestampSeconds = endFadeFrame.TimestampSeconds;
                if (!writer.WriteEndFade(view))
                {
                    ok = false;
                    message = "Movie finalization failed: " + writer.GetLastError();
                }
            }

            if (!writer.Close())
            {
                ok = false;
                message = "Movie finalization failed: " + writer.GetLastError();
            }
        }

        MarkFinalized(ok, message);
        if (callback)
        {
            callback(ok, message);
        }
    }

    bool EnsureWriterOpen(VideoWriter& writer, uint32_t width, uint32_t height)
    {
        if (writerOpened)
        {
            return true;
        }

        VideoWriterOptions openOptions = options;
        openOptions.width = width;
        openOptions.height = height;
        if (openOptions.codec == VideoCodec::Auto)
        {
            openOptions.codec = GuessCodecForMoviePath(openOptions.path);
        }

        if (!writer.Open(openOptions))
        {
            MarkFailed("Movie capture failed: " + writer.GetLastError());
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(mutex);
            options = openOptions;
            writerOpened = true;
        }
        return true;
    }

    bool WriteVideoFrame(VideoWriter& writer, const OwnedVideoFrame& frame)
    {
        VideoFrameView view{};
        view.Pixels = frame.Pixels.data();
        view.Width = frame.Width;
        view.Height = frame.Height;
        view.StrideBytes = frame.StrideBytes;
        view.Format = frame.Format;
        view.TimestampSeconds = frame.TimestampSeconds;
        if (!writer.WriteFrame(view))
        {
            MarkFailed("Movie capture failed: " + writer.GetLastError());
            return false;
        }

        std::lock_guard<std::mutex> lock(mutex);
        ++encodedVideoFrames;
        return true;
    }

    void DrainPendingAudio(VideoWriter& writer)
    {
        while (!pendingAudio.empty())
        {
            OwnedAudioSamples audio = std::move(pendingAudio.front());
            pendingAudio.pop_front();
            WriteAudio(writer, audio);
        }
    }

    void WriteAudio(VideoWriter& writer, const OwnedAudioSamples& audio)
    {
        if (!IsValidAudioSamples(audio))
        {
            return;
        }

        AudioFrameView view{};
        view.Samples = audio.Samples.data();
        view.FrameCount = audio.FrameCount;
        view.Channels = audio.Channels;
        view.SampleRate = audio.SampleRate;
        if (!writer.WriteAudio(view))
        {
            MarkFailed("Movie audio failed: " + writer.GetLastError());
            return;
        }

        std::lock_guard<std::mutex> lock(mutex);
        ++encodedAudioChunks;
    }

    void MarkFailed(std::string error)
    {
        std::lock_guard<std::mutex> lock(mutex);
        failed = true;
        lastError = std::move(error);
    }

    void MarkFinalized(bool ok, const std::string& message)
    {
        std::lock_guard<std::mutex> lock(mutex);
        failed = failed || !ok;
        if (!ok && lastError.empty())
        {
            lastError = message;
        }
        running = false;
        finishing = false;
        finalized = true;
        queue.clear();
        pendingAudio.clear();
        queuedVideoFrames = 0;
    }

    mutable std::mutex mutex;
    std::condition_variable cv;
    std::thread worker;
    std::deque<RecorderItem> queue;
    std::deque<OwnedAudioSamples> pendingAudio;
    VideoWriterOptions options{};
    std::function<void()> onQueueExhausted;
    const size_t maxQueuedItems = 128;
    size_t queuedVideoFrames = 0;
    size_t maxObservedQueuedItems = 0;
    uint64_t submittedVideoFrames = 0;
    uint64_t encodedVideoFrames = 0;
    uint64_t submittedAudioChunks = 0;
    uint64_t encodedAudioChunks = 0;
    uint64_t droppedVideoFrames = 0;
    uint64_t droppedAudioChunks = 0;
    uint32_t expectedAudioChannels = 0;
    uint32_t expectedAudioSampleRate = 0;
    bool running = false;
    bool finishing = false;
    bool finalized = true;
    bool failed = false;
    bool cancelRequested = false;
    bool writerOpened = false;
    std::string lastError;
};

AsyncVideoRecorder::AsyncVideoRecorder(size_t maxQueuedItems)
    : m_Impl(std::make_unique<Impl>(maxQueuedItems))
{
}

AsyncVideoRecorder::~AsyncVideoRecorder() = default;

bool AsyncVideoRecorder::Start(VideoWriterOptions options, const AsyncVideoRecorderOptions& recorderOptions, std::string* outError)
{
    return m_Impl->Start(std::move(options), recorderOptions, outError);
}

SubmitResult AsyncVideoRecorder::SubmitVideoFrame(OwnedVideoFrame& frame)
{
    return m_Impl->SubmitVideoFrame(frame);
}

SubmitResult AsyncVideoRecorder::SubmitAudioSamples(OwnedAudioSamples& audio)
{
    return m_Impl->SubmitAudioSamples(audio);
}

bool AsyncVideoRecorder::CanAcceptVideoFrame() const
{
    return m_Impl->CanAcceptVideoFrame();
}

void AsyncVideoRecorder::Finish(OwnedVideoFrame endFadeFrame, std::string successMessage, FinishCallback callback, bool wait,
                                bool trimPendingFrames)
{
    m_Impl->Finish(std::move(endFadeFrame), std::move(successMessage), std::move(callback), wait, trimPendingFrames);
}

void AsyncVideoRecorder::CancelAndWait()
{
    m_Impl->CancelAndWait();
}

void AsyncVideoRecorder::WaitForFinalization()
{
    m_Impl->WaitForFinalization();
}

bool AsyncVideoRecorder::IsFinalized() const
{
    return m_Impl->IsFinalized();
}

AsyncVideoRecorderStats AsyncVideoRecorder::GetStats() const
{
    return m_Impl->GetStats();
}

} // namespace GameEngine::Video
