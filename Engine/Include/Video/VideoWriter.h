#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "Video/VideoTypes.h"

namespace GameEngine::Video
{

enum class VideoCodec
{
    Auto,
    H264,
    HEVC,
    ProRes422,
    ProRes4444
};

enum class FadeColor
{
    Black,
    White
};

struct VideoWriterOptions
{
    std::string path;
    uint32_t width = 0;
    uint32_t height = 0;
    double fps = 60.0;
    VideoCodec codec = VideoCodec::Auto;
    // 0 lets the platform encoder choose its default video bitrate.
    uint32_t bitrateKbps = 0;
    bool recordAudio = true;
    uint32_t audioSampleRate = 48000;
    uint32_t audioChannels = 2;
    uint32_t audioBitrateKbps = 192;
    bool fadeInEnabled = false;
    bool fadeOutEnabled = false;
    FadeColor fadeColor = FadeColor::Black;
    double fadeDurationSeconds = 1.0;
    // macOS: disabled by default. AVAssetWriter fragmented MP4 output can leave
    // the finalized movie truncated to an early fragment in editor captures.
    double movieFragmentIntervalSeconds = 0.0;
    // Fail instead of falling back to a software encoder when the platform's
    // hardware encoder is unavailable (VideoToolbox on macOS, Media Foundation
    // hardware transforms on Windows).
    bool requireHardwareAcceleration = true;
    // HDR delivery. Off = SDR sRGB (default). PQ/HLG require a 10-bit-capable
    // codec (HEVC) and a 10-bit source frame (VideoPixelFormat::RGB10A2). The
    // nits below feed the stream's mastering-display / content-light metadata.
    VideoHdrMode hdrMode = VideoHdrMode::Off;
    float hdrPaperWhiteNits = 203.0f;
    float hdrMaxMasteringNits = 1000.0f;
    float hdrMaxContentLightLevelNits = 1000.0f;
    float hdrMaxFrameAverageLightLevelNits = 400.0f;
};

struct VideoFrameView
{
    const uint8_t* Pixels = nullptr;
    uint32_t Width = 0;
    uint32_t Height = 0;
    uint32_t StrideBytes = 0;
    VideoPixelFormat Format = VideoPixelFormat::RGBA8;
    // Optional presentation time. Negative uses the fixed frameIndex / fps timeline.
    double TimestampSeconds = -1.0;
};

struct AudioFrameView
{
    const float* Samples = nullptr;
    uint32_t FrameCount = 0;
    uint32_t Channels = 0;
    uint32_t SampleRate = 0;
};

class VideoWriter
{
public:
    VideoWriter();
    ~VideoWriter();

    VideoWriter(const VideoWriter&) = delete;
    VideoWriter& operator=(const VideoWriter&) = delete;

    bool Open(const VideoWriterOptions& options);
    bool WriteFrame(const VideoFrameView& frame);
    bool WriteAudio(const AudioFrameView& audio);
    bool ExtendToTimestamp(double timestampSeconds);
    bool WriteEndFade(const VideoFrameView& lastFrame);
    bool Close();
    bool IsOpen() const;
    bool IsReadyForVideoFrame() const;
    const std::string& GetLastError() const;

    static bool IsCodecSupported(VideoCodec codec);

private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

VideoCodec GuessCodecForMoviePath(const std::string& path);
VideoCodec ParseVideoCodecName(const std::string& name, VideoCodec fallback = VideoCodec::Auto);
const char* ToString(VideoCodec codec);

} // namespace GameEngine::Video
