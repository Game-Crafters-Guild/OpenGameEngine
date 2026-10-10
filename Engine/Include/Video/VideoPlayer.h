#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "Video/VideoTypes.h"

namespace GameEngine::Video
{

// Video player backed by AVFoundation (macOS), FFmpeg (Windows/Linux), or a null stub.
//
// Playback is PULL-based and paced by the player itself, never by the caller's
// tick: decoding runs off the caller's thread at the source's own encoded frame
// interval (scaled by playback speed), and the caller samples whatever frame is
// ready. A tick arriving before the next frame is due does nothing; a tick
// arriving after several are due sees only the newest. The cost of a tick is
// therefore independent of how long the previous tick took.
//
// Per tick: PollNewFrame(), and if it returns true, AcquireFramePointer() for the
// native decoded format or GetFrameRGBA() when the caller requires RGBA8. Offline
// consumers that need a particular frame (thumbnails, first-frame previews) use
// WaitForFrame() instead.
class VideoPlayer
{
public:
    VideoPlayer();
    ~VideoPlayer();

    // Load a video file. Returns false if the path is invalid or format unsupported.
    // Synchronous: opens the container and starts the decode thread before returning.
    bool Load(const std::string& path);
    // Stop decoding (joining the decode thread) and release the source.
    void Unload();
    bool IsLoaded() const;

    // Playback control
    void Play();
    void Pause();
    void Stop();   // Stops and rewinds to start
    bool IsPlaying() const;

    void SetLoop(bool loop);
    bool IsLooping() const;

    // Multiplier on real time. Zero or negative holds the current frame.
    void SetPlaybackSpeed(float speed);
    float GetPlaybackSpeed() const;

    float GetDuration() const;   // seconds; 0 if not loaded
    // Presentation time of the most recently published frame, in seconds.
    float GetCurrentTime() const;
    void Seek(float timeSeconds);

    int GetWidth() const;
    int GetHeight() const;

    VideoPixelFormat GetFramePixelFormat() const;

    // True when a frame newer than the last acquire is ready. Never blocks and
    // never decodes on the calling thread; backends that pull from a platform
    // decoder do that pull here.
    bool PollNewFrame();

    // Block until a frame newer than the last acquire is ready, or until the
    // timeout elapses. For offline extraction (thumbnails, preview first frame),
    // never for per-tick playback.
    bool WaitForFrame(float timeoutSeconds);

    // Write the current native-format frame into outPixels (must be width*height*4
    // bytes). Consumes the frame: PollNewFrame() reads false until another arrives.
    void GetFramePixels(uint8_t* outPixels);

    // Read-only pointer to the current native-format frame plus its byte size.
    // Consumes the frame. The pointer stays valid until the next acquire
    // (AcquireFramePointer / GetFramePixels / GetFrameRGBA) on this player.
    const uint8_t* AcquireFramePointer(size_t* outSizeBytes = nullptr);

    // Write the current RGBA8 frame into outPixels (must be width*height*4 bytes).
    // Consumes the frame.
    void GetFrameRGBA(uint8_t* outPixels);

    // Frames the decoder published since Load(), and the subset a caller acquired.
    // Their difference is the frames the caller was too slow to sample — the
    // cost-free drop that keeps playback on the wall clock instead of in slow
    // motion. Both are monotonic for the life of one loaded source.
    uint64_t GetPublishedFrameCount() const;
    uint64_t GetAcquiredFrameCount() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

} // namespace GameEngine::Video
