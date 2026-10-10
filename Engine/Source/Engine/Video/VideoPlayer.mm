#include "Video/VideoPlayer.h"

#import <AVFoundation/AVFoundation.h>
#import <CoreVideo/CoreVideo.h>
#import <QuartzCore/QuartzCore.h>

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
using Clock = std::chrono::steady_clock;

constexpr double kDefaultFrameRate = 30.0;
// AVFoundation makes a frame available for one host-time window; pulling at
// exactly the frame interval can straddle it and see every other frame, so the
// pump runs at a multiple of the source rate. A pull that finds nothing costs
// one hasNewPixelBufferForItemTime call.
constexpr double kPullOversample = 2.0;
constexpr double kMinPullInterval = 0.001;
constexpr double kMaxPullInterval = 0.1;
// How close to the duration counts as reaching the end. AVPlayer's clock stops a
// frame short of the nominal duration on many sources.
constexpr float kEndThresholdSeconds = 0.05f;
// A pull asked for while paused (load, seek) retries: the frame for a new time
// is not available the instant the seek is issued.
constexpr int kPausedPullAttempts = 30;
constexpr double kPausedPullInterval = 0.02;
// Run-loop slice the load-time first-frame pull gives AVFoundation between tries.
constexpr CFTimeInterval kLoadPullRunLoopSlice = 0.02;
constexpr int kBytesPerPixel = 4;

std::chrono::nanoseconds SecondsToDuration(double seconds)
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::duration<double>(seconds));
}
} // namespace

// AVFoundation decodes on its own threads and paces itself off the host clock,
// but a decoded frame only becomes a published frame when someone pulls it. This
// player owns a pump thread that does that pull at the source's own rate, so
// playback advances whether or not the caller ticks — the same contract the
// FFmpeg backend's decode thread provides. The caller samples whatever frame is
// ready and never waits for a pull.
//
// Threading: Mutex guards the shared state AND serialises every AVFoundation
// call, since a caller changing rate or seeking must not overlap the pump's pull.
// It is never held across the frame copy — the pump takes the pixel buffer under
// the lock, converts outside it, and re-takes it to publish. AVFoundation objects
// are created and released by Load/Unload while no pump thread exists.
struct VideoPlayer::Impl
{
    // ── Immutable between Load and Unload ─────────────────────────────────
    AVPlayer* player = nil;
    AVPlayerItem* item = nil;
    AVPlayerItemVideoOutput* output = nil;
    float duration = 0.0f;
    double frameInterval = 1.0 / kDefaultFrameRate;
    bool loaded = false;   // written only by Load/Unload

    // ── Shared handoff ────────────────────────────────────────────────────
    mutable std::mutex Mutex;
    std::condition_variable Cv;
    std::vector<uint8_t> ReadyBuffer;
    bool HasReadyFrame = false;
    bool Running = false;
    bool Playing = false;
    bool Loop = true;
    bool Ended = false;
    float Speed = 1.0f;
    int Width = 0;
    int Height = 0;
    float Position = 0.0f;
    // Pulls still owed to a paused player so a seek or a first frame shows up
    // without the caller resuming playback.
    int PausedPullsLeft = 0;
    uint64_t PublishedFrames = 0;
    uint64_t AcquiredFrames = 0;

    // ── Pump-thread-owned ─────────────────────────────────────────────────
    std::vector<uint8_t> DecodeBuffer;

    // ── Caller-owned ──────────────────────────────────────────────────────
    std::vector<uint8_t> PresentBuffer;
    std::thread Thread;

    ~Impl() { Unload(); }

    void StartThread()
    {
        {
            std::lock_guard<std::mutex> guard(Mutex);
            Running = true;
        }
        Thread = std::thread([this] { PumpLoop(); });
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
        // Join BEFORE releasing the AVFoundation objects: the pump dereferences
        // them, so tearing them down under a live thread is a use-after-free.
        StopThread();

        if (player)
            [player pause];
        if (item && output)
            [item removeOutput:output];
        if (player)
        {
            [player replaceCurrentItemWithPlayerItem:nil];
            [player release];
            player = nil;
        }
        if (output)
            [output release];
        if (item)
            [item release];
        item = nil;
        output = nil;

        duration = 0.0f;
        frameInterval = 1.0 / kDefaultFrameRate;
        loaded = false;

        HasReadyFrame = false;
        Playing = false;
        Ended = false;
        Width = 0;
        Height = 0;
        Position = 0.0f;
        PausedPullsLeft = 0;
        PublishedFrames = 0;
        AcquiredFrames = 0;

        std::vector<uint8_t>().swap(DecodeBuffer);
        std::vector<uint8_t>().swap(ReadyBuffer);
        std::vector<uint8_t>().swap(PresentBuffer);
    }

    float CurrentTimeLocked() const
    {
        if (Ended)
            return Position;
        if (!player)
            return 0.0f;
        return static_cast<float>(CMTimeGetSeconds(player.currentTime));
    }

    void SeekLocked(float timeSeconds)
    {
        CMTime target = CMTimeMakeWithSeconds(static_cast<Float64>(timeSeconds), 600);
        [player seekToTime:target toleranceBefore:kCMTimeZero toleranceAfter:kCMTimeZero];
        Position = timeSeconds;
        Ended = false;
    }

    double PullIntervalLocked() const
    {
        const double speed = Speed > 0.0f ? static_cast<double>(Speed) : 1.0;
        return std::clamp(frameInterval / (speed * kPullOversample),
                          kMinPullInterval, kMaxPullInterval);
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

    // ── Frame pull ────────────────────────────────────────────────────────

    CVPixelBufferRef CopyPixelBufferAtTimeLocked(CMTime itemTime)
    {
        if (!output || !CMTIME_IS_VALID(itemTime))
            return nullptr;
        if (![output hasNewPixelBufferForItemTime:itemTime])
            return nullptr;
        return [output copyPixelBufferForItemTime:itemTime itemTimeForDisplay:nil];
    }

    // Pull the frame that corresponds to the current host display time. After a
    // seek or a clip reload AVFoundation can temporarily miss that host-time
    // mapping, so fall back to the item's own playback time.
    CVPixelBufferRef CopyCurrentPixelBufferLocked()
    {
        if (!output || !item)
            return nullptr;
        CVPixelBufferRef pixelBuffer =
            CopyPixelBufferAtTimeLocked([output itemTimeForHostTime:CACurrentMediaTime()]);
        if (!pixelBuffer)
            pixelBuffer = CopyPixelBufferAtTimeLocked([item currentTime]);
        return pixelBuffer;
    }

    // Called without the mutex: this is the one expensive step, and holding the
    // lock across it would stall every caller for a full frame copy.
    bool ConvertIntoDecodeBuffer(CVPixelBufferRef pixelBuffer, int& outWidth, int& outHeight)
    {
        CVPixelBufferLockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly);

        const int w = static_cast<int>(CVPixelBufferGetWidth(pixelBuffer));
        const int h = static_cast<int>(CVPixelBufferGetHeight(pixelBuffer));
        const size_t bytesPerRow = CVPixelBufferGetBytesPerRow(pixelBuffer);
        const uint8_t* src = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(pixelBuffer));
        if (!src || w <= 0 || h <= 0)
        {
            CVPixelBufferUnlockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly);
            return false;
        }

        const size_t dstStride = static_cast<size_t>(w) * kBytesPerPixel;
        DecodeBuffer.resize(dstStride * static_cast<size_t>(h));
        uint8_t* dst = DecodeBuffer.data();
        if (bytesPerRow == dstStride)
        {
            std::memcpy(dst, src, static_cast<size_t>(h) * dstStride);
        }
        else
        {
            for (int row = 0; row < h; ++row)
                std::memcpy(dst + row * dstStride, src + row * bytesPerRow, dstStride);
        }

        CVPixelBufferUnlockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly);
        outWidth = w;
        outHeight = h;
        return true;
    }

    void PublishLocked(int width, int height)
    {
        DecodeBuffer.swap(ReadyBuffer);
        HasReadyFrame = true;
        Width = width;
        Height = height;
        Position = CurrentTimeLocked();
        ++PublishedFrames;
        PausedPullsLeft = 0;
    }

    // Pull, convert and publish one frame if AVFoundation has one ready. Releases
    // the lock across the copy, so the caller must re-check any state it cached.
    // Publishing after a stop request is harmless: StopThread joins before Unload
    // touches the buffers.
    bool PullAndPublishLocked(std::unique_lock<std::mutex>& lock)
    {
        CVPixelBufferRef pixelBuffer = CopyCurrentPixelBufferLocked();
        if (!pixelBuffer)
            return false;

        lock.unlock();
        int width = 0;
        int height = 0;
        const bool converted = ConvertIntoDecodeBuffer(pixelBuffer, width, height);
        CVBufferRelease(pixelBuffer);
        lock.lock();

        if (!converted)
            return false;
        PublishLocked(width, height);
        return true;
    }

    // AVPlayer does not update this player's lightweight state at the end of the
    // source, so the pump keeps it in sync — and restarts a looping source itself
    // rather than waiting for a caller's tick to notice.
    void UpdateEndStateLocked()
    {
        if (!Playing || duration <= 0.0f)
            return;
        const float cur = CurrentTimeLocked();
        Position = cur;
        if (cur < duration - kEndThresholdSeconds)
            return;

        if (Loop)
        {
            SeekLocked(0.0f);
            return;
        }
        [player pause];
        Playing = false;
        Ended = true;
        Position = duration;
        Cv.notify_all();
    }

    // ── Pump thread ───────────────────────────────────────────────────────

    void PumpLoop()
    {
        std::unique_lock<std::mutex> lock(Mutex);
        while (Running)
        {
            @autoreleasepool
            {
                const bool owedPull = PausedPullsLeft > 0;
                // A paused player, and a non-positive speed holding the current
                // frame, cost nothing: this wait is what makes that true.
                if (!owedPull && (!Playing || Speed <= 0.0f))
                {
                    Cv.wait(lock);
                    continue;
                }

                UpdateEndStateLocked();

                if (PullAndPublishLocked(lock))
                    Cv.notify_all();
                else if (owedPull)
                    --PausedPullsLeft;

                if (!Running)
                    break;

                const double interval = owedPull ? kPausedPullInterval : PullIntervalLocked();
                Cv.wait_for(lock, SecondsToDuration(interval), [this] { return !Running; });
            }
        }
    }

    // Load-time first frame, so a player that is never played still has something
    // to show. Runs on the caller's thread before the pump exists; the run-loop
    // slice is what gives AVFoundation a chance to produce the frame between tries.
    void PullFirstFrameBlocking()
    {
        std::unique_lock<std::mutex> lock(Mutex);
        for (int attempt = 0; attempt < kPausedPullAttempts; ++attempt)
        {
            if (PullAndPublishLocked(lock))
                return;
            lock.unlock();
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, kLoadPullRunLoopSlice, true);
            lock.lock();
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

    @autoreleasepool
    {
        NSString* nsPath = [NSString stringWithUTF8String:path.c_str()];
        NSURL* url = [NSURL fileURLWithPath:nsPath];

        AVAsset* asset = [AVAsset assetWithURL:url];
        if (!asset)
            return false;

        // Synchronously get duration and video track dimensions.
        m_Impl->duration = static_cast<float>(CMTimeGetSeconds(asset.duration));
        if (m_Impl->duration <= 0.0f)
            m_Impl->duration = 0.0f;

        NSArray<AVAssetTrack*>* videoTracks = [asset tracksWithMediaType:AVMediaTypeVideo];
        if (videoTracks.count == 0)
            return false;

        AVAssetTrack* track = videoTracks.firstObject;
        CGSize naturalSize = track.naturalSize;
        CGAffineTransform t = track.preferredTransform;
        // Account for rotation (portrait video has swapped dimensions after transform).
        CGSize displaySize = CGSizeApplyAffineTransform(naturalSize, t);
        m_Impl->Width = static_cast<int>(std::abs(displaySize.width));
        m_Impl->Height = static_cast<int>(std::abs(displaySize.height));
        if (m_Impl->Width <= 0 || m_Impl->Height <= 0)
        {
            m_Impl->Width  = static_cast<int>(naturalSize.width);
            m_Impl->Height = static_cast<int>(naturalSize.height);
        }

        // The pump's pull rate. A source that declares no frame rate is pulled at
        // a sane default; over-pulling only costs an availability check.
        const double frameRate = static_cast<double>(track.nominalFrameRate);
        m_Impl->frameInterval = frameRate > 0.0 ? 1.0 / frameRate : 1.0 / kDefaultFrameRate;

        // Set up AVPlayerItemVideoOutput to pull decoded BGRA frames.
        NSDictionary* pixelAttribs = @{
            (NSString*)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA),
            (NSString*)kCVPixelBufferIOSurfacePropertiesKey: @{}
        };
        m_Impl->output = [[AVPlayerItemVideoOutput alloc] initWithPixelBufferAttributes:pixelAttribs];

        m_Impl->item = [[AVPlayerItem alloc] initWithAsset:asset];
        [m_Impl->item addOutput:m_Impl->output];

        m_Impl->player = [[AVPlayer alloc] initWithPlayerItem:m_Impl->item];
        m_Impl->player.muted = NO;
        if ([m_Impl->player respondsToSelector:@selector(setAutomaticallyWaitsToMinimizeStalling:)])
            m_Impl->player.automaticallyWaitsToMinimizeStalling = NO;
        [m_Impl->player setRate:0.0f];  // start paused

        // Fault the frame buffers in before playback starts. First touch of a 4K
        // BGRA frame costs over a hundred milliseconds, and paying it on the pump
        // thread stalls publication for as long as it takes.
        const size_t frameBytes =
            static_cast<size_t>(m_Impl->Width) * m_Impl->Height * kBytesPerPixel;
        m_Impl->DecodeBuffer.assign(frameBytes, 0);
        m_Impl->ReadyBuffer.assign(frameBytes, 0);
        m_Impl->PresentBuffer.assign(frameBytes, 0);

        m_Impl->Position = 0.0f;
        [m_Impl->player seekToTime:kCMTimeZero toleranceBefore:kCMTimeZero toleranceAfter:kCMTimeZero];
        m_Impl->PullFirstFrameBlocking();

        m_Impl->loaded = true;
        m_Impl->StartThread();
        return true;
    }
}

void VideoPlayer::Unload()
{
    m_Impl->Unload();
}

bool VideoPlayer::IsLoaded() const { return m_Impl->loaded; }

void VideoPlayer::Play()
{
    if (!m_Impl->loaded)
        return;
    {
        std::lock_guard<std::mutex> guard(m_Impl->Mutex);
        const float cur = m_Impl->CurrentTimeLocked();
        const bool spent = m_Impl->Ended ||
            (m_Impl->duration > 0.0f && cur >= m_Impl->duration - kEndThresholdSeconds);
        if (spent)
            m_Impl->SeekLocked(0.0f);
        m_Impl->Ended = false;
        [m_Impl->player setRate:m_Impl->Speed];
        m_Impl->Playing = true;
    }
    m_Impl->Cv.notify_all();
}

void VideoPlayer::Pause()
{
    if (!m_Impl->loaded)
        return;
    {
        std::lock_guard<std::mutex> guard(m_Impl->Mutex);
        [m_Impl->player pause];
        m_Impl->Playing = false;
    }
    m_Impl->Cv.notify_all();
}

void VideoPlayer::Stop()
{
    if (!m_Impl->loaded)
        return;
    {
        std::lock_guard<std::mutex> guard(m_Impl->Mutex);
        [m_Impl->player pause];
        m_Impl->Playing = false;
        m_Impl->SeekLocked(0.0f);
        m_Impl->PausedPullsLeft = kPausedPullAttempts;
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
        // Callers sync this from an inspector field every tick; re-issuing the
        // rate on an unchanged value would restart AVPlayer's pacing each time.
        if (speed == m_Impl->Speed)
            return;
        m_Impl->Speed = speed;
        if (m_Impl->Playing && m_Impl->player)
            [m_Impl->player setRate:speed];
    }
    m_Impl->Cv.notify_all();
}

float VideoPlayer::GetPlaybackSpeed() const
{
    std::lock_guard<std::mutex> guard(m_Impl->Mutex);
    return m_Impl->Speed;
}

float VideoPlayer::GetDuration() const { return m_Impl->duration; }

float VideoPlayer::GetCurrentTime() const
{
    std::lock_guard<std::mutex> guard(m_Impl->Mutex);
    return m_Impl->CurrentTimeLocked();
}

void VideoPlayer::Seek(float timeSeconds)
{
    if (!m_Impl->loaded)
        return;
    {
        std::lock_guard<std::mutex> guard(m_Impl->Mutex);
        m_Impl->SeekLocked(timeSeconds);
        // Scrubbing a paused player still has to show the frame it landed on.
        if (!m_Impl->Playing)
            m_Impl->PausedPullsLeft = kPausedPullAttempts;
    }
    m_Impl->Cv.notify_all();
}

int VideoPlayer::GetWidth() const
{
    std::lock_guard<std::mutex> guard(m_Impl->Mutex);
    return m_Impl->Width;
}

int VideoPlayer::GetHeight() const
{
    std::lock_guard<std::mutex> guard(m_Impl->Mutex);
    return m_Impl->Height;
}

VideoPixelFormat VideoPlayer::GetFramePixelFormat() const
{
    return VideoPixelFormat::BGRA8;
}

bool VideoPlayer::PollNewFrame()
{
    std::lock_guard<std::mutex> guard(m_Impl->Mutex);
    return m_Impl->HasReadyFrame;
}

bool VideoPlayer::WaitForFrame(float timeoutSeconds)
{
    if (!m_Impl->loaded)
        return false;
    std::unique_lock<std::mutex> lock(m_Impl->Mutex);
    // Offline extraction (thumbnails, first-frame previews) waits on a player that
    // was never played, where the pump is idle by design.
    if (!m_Impl->HasReadyFrame && !m_Impl->Playing)
    {
        m_Impl->PausedPullsLeft = kPausedPullAttempts;
        m_Impl->Cv.notify_all();
    }
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
    if (!outPixels)
        return;
    m_Impl->AcquireIntoPresentBuffer();
    if (m_Impl->PresentBuffer.empty())
        return;
    std::memcpy(outPixels, m_Impl->PresentBuffer.data(), m_Impl->PresentBuffer.size());
}

void VideoPlayer::GetFrameRGBA(uint8_t* outPixels)
{
    if (!outPixels)
        return;
    m_Impl->AcquireIntoPresentBuffer();
    if (m_Impl->PresentBuffer.empty())
        return;
    const uint8_t* src = m_Impl->PresentBuffer.data();
    const size_t pixelCount = m_Impl->PresentBuffer.size() / kBytesPerPixel;
    for (size_t i = 0; i < pixelCount; ++i)
    {
        outPixels[i * 4 + 0] = src[i * 4 + 2];
        outPixels[i * 4 + 1] = src[i * 4 + 1];
        outPixels[i * 4 + 2] = src[i * 4 + 0];
        outPixels[i * 4 + 3] = src[i * 4 + 3];
    }
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
