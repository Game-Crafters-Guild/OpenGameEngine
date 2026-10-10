#include "Video/VideoPlayer.h"

extern "C"
{
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace GameEngine::Video
{

namespace
{
constexpr int64_t kSeekTimestamp = 0;
constexpr int kNoStream = -1;
constexpr double kFallbackFrameRate = 30.0;
constexpr int kRgbaBytesPerPixel = 4;

// A frame due further out than this is waited for in chunks. Bounds how long the
// decode thread can sit in one wait when playback speed is pathologically small,
// and bounds join latency for a stop that races a notify.
constexpr std::chrono::milliseconds kMaxFrameWait{250};

// How close to the end Play() considers a source spent and restarts it.
constexpr double kMinEndThreshold = 0.05;

using Clock = std::chrono::steady_clock;

Clock::duration SecondsToDuration(double seconds)
{
    using DoubleSeconds = std::chrono::duration<double>;
    return std::chrono::duration_cast<Clock::duration>(DoubleSeconds{seconds});
}
} // namespace

// Playback runs on a dedicated decode thread, not on the caller's tick.
//
// THREAD CONTRACT
//   - The FFmpeg objects (FormatCtx/CodecCtx/SwsCtx/DecodeFrame/Packet) and
//     DecodeBuffer are touched ONLY by the decode thread, or by Load/Unload while
//     no decode thread exists. No public method reaches them.
//   - Everything under Mutex is the handoff: playback state the caller writes and
//     the decoder reads, plus the published frame the decoder writes and the
//     caller reads.
//   - PresentBuffer belongs to the caller: acquiring swaps it with ReadyBuffer, so
//     the pointer handed out is never a buffer the decoder can write.
//
// PACING
//   A frame's presentation time maps to a wall-clock instant through an anchor:
//   frame p is due at AnchorTime + (p - AnchorPts) / Speed. The decoder sleeps
//   until then. Play/Seek/speed changes re-anchor and bump AnchorEpoch, which
//   wakes an in-progress wait to recompute. The caller's tick rate does not enter
//   the calculation at any point, so no amount of tick cost feeds back into it.
struct VideoPlayer::Impl
{
    // ── Decode-thread-owned ───────────────────────────────────────────────
    AVFormatContext* FormatCtx = nullptr;
    AVCodecContext*  CodecCtx  = nullptr;
    SwsContext*      SwsCtx    = nullptr;
    AVFrame*         DecodeFrame = nullptr;
    AVPacket*        Packet      = nullptr;
    std::vector<uint8_t> DecodeBuffer;
    // Presentation time to assign when the container carries none, advanced by one
    // frame interval per decoded frame.
    double SyntheticPts = 0.0;
    bool   PendingValid = false;
    double PendingPts   = 0.0;

    // ── Immutable between Load and Unload ─────────────────────────────────
    int    VideoStreamIndex = kNoStream;
    int    Width  = 0;
    int    Height = 0;
    float  Duration = 0.0f;
    double FrameDuration = 0.0;   // seconds per frame (1 / fps)
    double StreamTimeBase = 0.0;
    size_t FrameBytes = 0;
    bool   Loaded = false;        // written only by Load/Unload

    // ── Shared handoff ────────────────────────────────────────────────────
    mutable std::mutex Mutex;
    std::condition_variable Cv;
    std::vector<uint8_t> ReadyBuffer;
    bool   HasReadyFrame = false;
    bool   Running  = false;
    bool   Playing  = false;
    bool   Loop     = true;
    bool   Ended    = false;
    float  Speed    = 1.0f;
    double PositionPts = 0.0;
    Clock::time_point AnchorTime{};
    double   AnchorPts   = 0.0;
    uint64_t AnchorEpoch = 0;
    bool     SeekRequested = false;
    double   SeekTarget    = 0.0;
    uint64_t PublishedFrames = 0;
    uint64_t AcquiredFrames  = 0;
    // PublishedFrames at the last loop restart. A source that reaches EOF again
    // without having published anything yields nothing however often it is
    // restarted, and looping on it would spin a core forever.
    uint64_t PublishedAtLoopStart = 0;

    // ── Caller-owned ──────────────────────────────────────────────────────
    std::vector<uint8_t> PresentBuffer;
    std::thread Thread;

    ~Impl() { Unload(); }

    void ReAnchorLocked(double pts)
    {
        AnchorTime = Clock::now();
        AnchorPts = pts;
        ++AnchorEpoch;
    }

    void RequestSeekLocked(double timeSeconds)
    {
        SeekRequested = true;
        SeekTarget = timeSeconds;
        PositionPts = timeSeconds;
        ReAnchorLocked(timeSeconds);
    }

    void StartThread()
    {
        {
            std::lock_guard<std::mutex> guard(Mutex);
            Running = true;
        }
        Thread = std::thread([this] { DecodeLoop(); });
    }

    void StopThread()
    {
        {
            std::lock_guard<std::mutex> guard(Mutex);
            Running = false;
        }
        Cv.notify_all();
        if (Thread.joinable())
            Thread.join();
    }

    void Unload()
    {
        // Join BEFORE any FFmpeg teardown: the decode thread dereferences these
        // contexts without the mutex (see the thread contract), so freeing them
        // while it runs is a use-after-free no handle generation would catch.
        StopThread();

        if (Packet)
            av_packet_free(&Packet);
        if (DecodeFrame)
            av_frame_free(&DecodeFrame);
        if (SwsCtx)
        {
            sws_freeContext(SwsCtx);
            SwsCtx = nullptr;
        }
        if (CodecCtx)
            avcodec_free_context(&CodecCtx);
        if (FormatCtx)
            avformat_close_input(&FormatCtx);

        VideoStreamIndex = kNoStream;
        Width  = 0;
        Height = 0;
        Duration = 0.0f;
        FrameDuration = 0.0;
        StreamTimeBase = 0.0;
        FrameBytes = 0;
        Loaded = false;
        SyntheticPts = 0.0;
        PendingValid = false;
        PendingPts = 0.0;

        HasReadyFrame = false;
        Playing = false;
        Ended = false;
        PositionPts = 0.0;
        AnchorPts = 0.0;
        AnchorEpoch = 0;
        PublishedAtLoopStart = 0;
        SeekRequested = false;
        SeekTarget = 0.0;
        PublishedFrames = 0;
        AcquiredFrames = 0;

        std::vector<uint8_t>().swap(DecodeBuffer);
        std::vector<uint8_t>().swap(ReadyBuffer);
        std::vector<uint8_t>().swap(PresentBuffer);
    }

    // Take ownership of the published frame, if there is one. PresentBuffer is
    // left holding the newest frame the caller has been shown either way, so a
    // caller that asks for pixels without a new frame still reads the last one.
    void AcquireIntoPresentBuffer()
    {
        std::lock_guard<std::mutex> guard(Mutex);
        if (!HasReadyFrame)
            return;
        PresentBuffer.swap(ReadyBuffer);
        HasReadyFrame = false;
        ++AcquiredFrames;
    }

    // ── Decode thread ─────────────────────────────────────────────────────

    enum class DecodeStatus
    {
        Frame,
        EndOfStream,
        Error
    };

    double ResolveFramePts()
    {
        int64_t timestamp = DecodeFrame->pts;
        if (timestamp == AV_NOPTS_VALUE)
            timestamp = DecodeFrame->best_effort_timestamp;
        if (timestamp == AV_NOPTS_VALUE)
        {
            const double pts = SyntheticPts;
            SyntheticPts += FrameDuration;
            return pts;
        }
        const double pts = static_cast<double>(timestamp) * StreamTimeBase;
        SyntheticPts = pts + FrameDuration;
        return pts;
    }

    // Pull the next decoded frame, reading packets as needed. Leaves the frame in
    // DecodeFrame. Decode thread only, called without the mutex.
    DecodeStatus PullFrame(double& outPts)
    {
        for (;;)
        {
            const int received = avcodec_receive_frame(CodecCtx, DecodeFrame);
            if (received == 0)
            {
                outPts = ResolveFramePts();
                return DecodeStatus::Frame;
            }
            if (received != AVERROR(EAGAIN) && received != AVERROR_EOF)
                return DecodeStatus::Error;

            const int read = av_read_frame(FormatCtx, Packet);
            if (read == AVERROR_EOF)
            {
                // Flush the codec so frames it still holds for reordering come out
                // before the stream is declared finished.
                avcodec_send_packet(CodecCtx, nullptr);
                if (avcodec_receive_frame(CodecCtx, DecodeFrame) == 0)
                {
                    outPts = ResolveFramePts();
                    return DecodeStatus::Frame;
                }
                return DecodeStatus::EndOfStream;
            }
            if (read < 0)
                return DecodeStatus::Error;

            if (Packet->stream_index != VideoStreamIndex)
            {
                av_packet_unref(Packet);
                continue;
            }
            const int sent = avcodec_send_packet(CodecCtx, Packet);
            av_packet_unref(Packet);
            if (sent < 0 && sent != AVERROR(EAGAIN))
                continue; // undecodable packet — keep reading rather than stall
        }
    }

    // The only seek in this file: one routine is what stops a loop restart and an
    // explicit Seek() from disagreeing about which stream's time base the target
    // is expressed in. Decode thread only, called without the mutex.
    void SeekToSeconds(double timeSeconds)
    {
        const int64_t target = StreamTimeBase > 0.0
            ? static_cast<int64_t>(timeSeconds / StreamTimeBase)
            : kSeekTimestamp;
        av_seek_frame(FormatCtx, VideoStreamIndex, target, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(CodecCtx);
        SyntheticPts = timeSeconds;
    }

    void ScaleIntoDecodeBuffer()
    {
        uint8_t* dstData[4] = {DecodeBuffer.data(), nullptr, nullptr, nullptr};
        int dstLinesize[4] = {Width * kRgbaBytesPerPixel, 0, 0, 0};
        sws_scale(SwsCtx, DecodeFrame->data, DecodeFrame->linesize, 0, Height,
                  dstData, dstLinesize);
    }

    void PublishLocked()
    {
        DecodeBuffer.swap(ReadyBuffer);
        HasReadyFrame = true;
        PositionPts = PendingPts;
        PendingValid = false;
        ++PublishedFrames;
    }

    void DecodeLoop()
    {
        std::unique_lock<std::mutex> lock(Mutex);
        while (Running)
        {
            if (SeekRequested)
            {
                const double target = SeekTarget;
                SeekRequested = false;
                PendingValid = false;
                lock.unlock();
                SeekToSeconds(target);
                lock.lock();
                continue;
            }

            // A non-positive speed holds the current frame rather than dividing by
            // it; this wait is what makes a paused or held player cost nothing.
            if (!Playing || Speed <= 0.0f)
            {
                Cv.wait(lock);
                continue;
            }

            if (!PendingValid)
            {
                lock.unlock();
                double pts = 0.0;
                const DecodeStatus status = PullFrame(pts);
                lock.lock();
                if (!Running)
                    break;
                if (SeekRequested)
                    continue;

                if (status == DecodeStatus::Error)
                {
                    Playing = false;
                    Ended = true;
                    Cv.notify_all();
                    continue;
                }
                if (status == DecodeStatus::EndOfStream)
                {
                    if (Loop && PublishedFrames > PublishedAtLoopStart)
                    {
                        PublishedAtLoopStart = PublishedFrames;
                        lock.unlock();
                        SeekToSeconds(0.0);
                        lock.lock();
                        PositionPts = 0.0;
                        Ended = false;
                        ReAnchorLocked(0.0);
                    }
                    else
                    {
                        Playing = false;
                        Ended = true;
                        if (Duration > 0.0f)
                            PositionPts = static_cast<double>(Duration);
                        Cv.notify_all();
                    }
                    continue;
                }

                // Pre-roll from a keyframe seek: these frames precede the point the
                // caller asked for and are never shown, so they never pay for a
                // colour conversion.
                if (pts < AnchorPts - FrameDuration)
                    continue;

                lock.unlock();
                ScaleIntoDecodeBuffer();
                lock.lock();
                if (!Running)
                    break;
                if (SeekRequested)
                    continue;
                PendingPts = pts;
                PendingValid = true;
            }

            const uint64_t epoch = AnchorEpoch;
            const double offsetSeconds = (PendingPts - AnchorPts) / static_cast<double>(Speed);
            const Clock::time_point now = Clock::now();
            Clock::time_point due = AnchorTime + SecondsToDuration(offsetSeconds);
            if (due > now)
            {
                due = (std::min)(due, now + kMaxFrameWait);
                Cv.wait_until(lock, due, [this, epoch] {
                    return !Running || !Playing || SeekRequested || AnchorEpoch != epoch;
                });
                continue; // re-evaluate: the frame may still not be due
            }

            PublishLocked();
            Cv.notify_all();
        }
    }
};

VideoPlayer::VideoPlayer()
    : m_Impl(std::make_unique<Impl>())
{
}

VideoPlayer::~VideoPlayer() = default;

bool VideoPlayer::Load(const std::string& path)
{
    m_Impl->Unload();

    if (avformat_open_input(&m_Impl->FormatCtx, path.c_str(), nullptr, nullptr) < 0)
        return false;

    if (avformat_find_stream_info(m_Impl->FormatCtx, nullptr) < 0)
    {
        m_Impl->Unload();
        return false;
    }

    // Find the first video stream.
    m_Impl->VideoStreamIndex = kNoStream;
    for (unsigned int i = 0; i < m_Impl->FormatCtx->nb_streams; ++i)
    {
        if (m_Impl->FormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            m_Impl->VideoStreamIndex = static_cast<int>(i);
            break;
        }
    }
    if (m_Impl->VideoStreamIndex == kNoStream)
    {
        m_Impl->Unload();
        return false;
    }

    AVStream* videoStream = m_Impl->FormatCtx->streams[m_Impl->VideoStreamIndex];
    AVCodecParameters* codecPar = videoStream->codecpar;

    const AVCodec* codec = avcodec_find_decoder(codecPar->codec_id);
    if (!codec)
    {
        m_Impl->Unload();
        return false;
    }

    m_Impl->CodecCtx = avcodec_alloc_context3(codec);
    if (!m_Impl->CodecCtx)
    {
        m_Impl->Unload();
        return false;
    }

    if (avcodec_parameters_to_context(m_Impl->CodecCtx, codecPar) < 0)
    {
        m_Impl->Unload();
        return false;
    }

    if (avcodec_open2(m_Impl->CodecCtx, codec, nullptr) < 0)
    {
        m_Impl->Unload();
        return false;
    }

    m_Impl->Width  = m_Impl->CodecCtx->width;
    m_Impl->Height = m_Impl->CodecCtx->height;
    m_Impl->StreamTimeBase = av_q2d(videoStream->time_base);

    // Duration in seconds.
    if (m_Impl->FormatCtx->duration != AV_NOPTS_VALUE)
        m_Impl->Duration = static_cast<float>(m_Impl->FormatCtx->duration) /
                           static_cast<float>(AV_TIME_BASE);

    // Frame interval from the stream's average frame rate — this is the rate
    // playback is paced at.
    const AVRational avgFrameRate = videoStream->avg_frame_rate;
    if (avgFrameRate.num > 0 && avgFrameRate.den > 0)
        m_Impl->FrameDuration = static_cast<double>(avgFrameRate.den) /
                                static_cast<double>(avgFrameRate.num);
    else
        m_Impl->FrameDuration = 1.0 / kFallbackFrameRate;

    m_Impl->DecodeFrame = av_frame_alloc();
    if (!m_Impl->DecodeFrame)
    {
        m_Impl->Unload();
        return false;
    }

    const int frameBytes = av_image_get_buffer_size(AV_PIX_FMT_RGBA,
        m_Impl->Width, m_Impl->Height, 1);
    if (frameBytes <= 0)
    {
        m_Impl->Unload();
        return false;
    }

    // Three buffers, one per handoff stage: the decoder writes one, one holds the
    // published frame, one is the caller's. Publishing and acquiring are vector
    // swaps, so a frame is never copied between stages.
    m_Impl->FrameBytes = static_cast<size_t>(frameBytes);
    m_Impl->DecodeBuffer.assign(m_Impl->FrameBytes, 0);
    m_Impl->ReadyBuffer.assign(m_Impl->FrameBytes, 0);
    m_Impl->PresentBuffer.assign(m_Impl->FrameBytes, 0);

    m_Impl->SwsCtx = sws_getContext(
        m_Impl->Width, m_Impl->Height, m_Impl->CodecCtx->pix_fmt,
        m_Impl->Width, m_Impl->Height, AV_PIX_FMT_RGBA,
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!m_Impl->SwsCtx)
    {
        m_Impl->Unload();
        return false;
    }

    m_Impl->Packet = av_packet_alloc();
    if (!m_Impl->Packet)
    {
        m_Impl->Unload();
        return false;
    }

    m_Impl->Loaded = true;
    m_Impl->AnchorTime = Clock::now();
    m_Impl->StartThread();
    return true;
}

void VideoPlayer::Unload()
{
    m_Impl->Unload();
}

bool VideoPlayer::IsLoaded() const { return m_Impl->Loaded; }

void VideoPlayer::Play()
{
    if (!m_Impl->Loaded)
        return;

    {
        std::lock_guard<std::mutex> guard(m_Impl->Mutex);
        const double endThreshold = (std::max)(m_Impl->FrameDuration * 2.0, kMinEndThreshold);
        const bool spent = m_Impl->Ended ||
            (m_Impl->Duration > 0.0f &&
             m_Impl->PositionPts >= static_cast<double>(m_Impl->Duration) - endThreshold);
        if (spent)
            m_Impl->RequestSeekLocked(0.0);
        else
            m_Impl->ReAnchorLocked(m_Impl->PositionPts);
        m_Impl->Ended = false;
        m_Impl->Playing = true;
    }
    m_Impl->Cv.notify_all();
}

void VideoPlayer::Pause()
{
    {
        std::lock_guard<std::mutex> guard(m_Impl->Mutex);
        m_Impl->Playing = false;
    }
    m_Impl->Cv.notify_all();
}

void VideoPlayer::Stop()
{
    if (!m_Impl->Loaded)
        return;
    {
        std::lock_guard<std::mutex> guard(m_Impl->Mutex);
        m_Impl->Playing = false;
        m_Impl->Ended = false;
        m_Impl->RequestSeekLocked(0.0);
    }
    m_Impl->Cv.notify_all();
}

bool VideoPlayer::IsPlaying() const
{
    std::lock_guard<std::mutex> guard(m_Impl->Mutex);
    return m_Impl->Playing;
}

void VideoPlayer::SetLoop(bool loop)
{
    std::lock_guard<std::mutex> guard(m_Impl->Mutex);
    m_Impl->Loop = loop;
}

bool VideoPlayer::IsLooping() const
{
    std::lock_guard<std::mutex> guard(m_Impl->Mutex);
    return m_Impl->Loop;
}

void VideoPlayer::SetPlaybackSpeed(float speed)
{
    {
        std::lock_guard<std::mutex> guard(m_Impl->Mutex);
        // Re-anchoring on an unchanged value would restart the pacing window every
        // tick, and callers sync this from an inspector field every tick.
        if (speed == m_Impl->Speed)
            return;
        m_Impl->Speed = speed;
        m_Impl->ReAnchorLocked(m_Impl->PositionPts);
    }
    m_Impl->Cv.notify_all();
}

float VideoPlayer::GetPlaybackSpeed() const
{
    std::lock_guard<std::mutex> guard(m_Impl->Mutex);
    return m_Impl->Speed;
}

float VideoPlayer::GetDuration() const { return m_Impl->Duration; }

float VideoPlayer::GetCurrentTime() const
{
    std::lock_guard<std::mutex> guard(m_Impl->Mutex);
    return static_cast<float>(m_Impl->PositionPts);
}

void VideoPlayer::Seek(float timeSeconds)
{
    if (!m_Impl->Loaded)
        return;
    {
        std::lock_guard<std::mutex> guard(m_Impl->Mutex);
        m_Impl->Ended = false;
        m_Impl->RequestSeekLocked(static_cast<double>(timeSeconds));
    }
    m_Impl->Cv.notify_all();
}

int VideoPlayer::GetWidth()  const { return m_Impl->Width; }
int VideoPlayer::GetHeight() const { return m_Impl->Height; }

VideoPixelFormat VideoPlayer::GetFramePixelFormat() const
{
    return VideoPixelFormat::RGBA8;
}

bool VideoPlayer::PollNewFrame()
{
    std::lock_guard<std::mutex> guard(m_Impl->Mutex);
    return m_Impl->HasReadyFrame;
}

bool VideoPlayer::WaitForFrame(float timeoutSeconds)
{
    if (!m_Impl->Loaded)
        return false;
    std::unique_lock<std::mutex> lock(m_Impl->Mutex);
    m_Impl->Cv.wait_for(lock, SecondsToDuration(static_cast<double>(timeoutSeconds)),
        [this] { return m_Impl->HasReadyFrame || !m_Impl->Running || m_Impl->Ended; });
    return m_Impl->HasReadyFrame;
}

const uint8_t* VideoPlayer::AcquireFramePointer(size_t* outSizeBytes)
{
    m_Impl->AcquireIntoPresentBuffer();
    if (m_Impl->PresentBuffer.empty())
    {
        if (outSizeBytes)
            *outSizeBytes = 0;
        return nullptr;
    }
    if (outSizeBytes)
        *outSizeBytes = m_Impl->PresentBuffer.size();
    return m_Impl->PresentBuffer.data();
}

void VideoPlayer::GetFramePixels(uint8_t* outPixels)
{
    GetFrameRGBA(outPixels);
}

void VideoPlayer::GetFrameRGBA(uint8_t* outPixels)
{
    if (!outPixels)
        return;
    m_Impl->AcquireIntoPresentBuffer();
    if (m_Impl->PresentBuffer.empty())
        return;
    std::memcpy(outPixels, m_Impl->PresentBuffer.data(), m_Impl->PresentBuffer.size());
}

uint64_t VideoPlayer::GetPublishedFrameCount() const
{
    std::lock_guard<std::mutex> guard(m_Impl->Mutex);
    return m_Impl->PublishedFrames;
}

uint64_t VideoPlayer::GetAcquiredFrameCount() const
{
    std::lock_guard<std::mutex> guard(m_Impl->Mutex);
    return m_Impl->AcquiredFrames;
}

} // namespace GameEngine::Video
