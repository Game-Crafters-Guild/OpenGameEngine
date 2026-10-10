// Closed-loop tests for the FFmpeg VideoWriter: open a writer, encode synthetic
// frames whose content identifies them, finalize, then reopen the file with the
// engine's own decoder and assert structure — frame count, dimensions, duration,
// color tags, and that decoded pixels correlate with the input pattern.
//
// Pattern design note: the engine-player correlation asserts use GRAY content,
// which is invariant under the BT.601/BT.709 matrix choice — the gray row-error
// assert is the RANGE detector (a range break shifts the row mean by ~7-8
// codes, measured; clean encodes stay within ~2). The MATRIX axis is covered
// separately: a saturated red patch decoded by this test's own BT.709-configured
// converter must land on pure red, which a 601-coefficient encode under 709
// tags misses by ~24 codes of green. Stream identity is also asserted on the
// container tags.

#include <gtest/gtest.h>

#include "Video/VideoPlayer.h"
#include "Video/VideoWriter.h"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Video;

namespace
{
constexpr uint32_t kWidth = 128;
constexpr uint32_t kHeight = 96;
constexpr double kFps = 30.0;
constexpr uint32_t kFrameCount = 48;

// Background gradient tops out below the block's white so block detection has a
// clean threshold; the block's x position encodes the frame index.
constexpr uint8_t kGradientMax = 200;
constexpr uint32_t kBlockSize = 16;
constexpr uint32_t kBlockY = 8;
constexpr uint32_t kBlockStepX = 2;
constexpr uint32_t kGradientProbeRow = 80;

// Mean absolute error budget for the gray gradient row after one lossy encode
// and decode. Clean H264 round-trips measure ~1-2; an encode with the wrong
// range flag shifts the row mean by ~7-8 (measured via the range red arm).
constexpr double kGradientRowMaxMeanError = 6.0;

// Saturated red bar (matrix probe): rows below the moving block, above the
// gradient probe row. A 601-coefficient encode read back through a
// 709-configured decode shifts this patch's green by ~24 codes.
constexpr uint32_t kRedBarY = 32;
constexpr uint32_t kRedBarHeight = 16;
constexpr int kRedPatchTolerance = 15;

constexpr float kFirstFrameTimeoutSeconds = 10.0f;

std::filesystem::path MakeOutputPath(const char* extension)
{
    const ::testing::TestInfo* info = ::testing::UnitTest::GetInstance()->current_test_info();
    const std::string name = std::string("ge_video_writer_test_") + info->name() + extension;
    return std::filesystem::temp_directory_path() / name;
}

VideoWriterOptions MakeVideoOnlyOptions(const std::filesystem::path& path)
{
    VideoWriterOptions options;
    options.path = path.string();
    options.width = kWidth;
    options.height = kHeight;
    options.fps = kFps;
    options.codec = VideoCodec::H264;
    options.recordAudio = false;
    // Deterministic across machines: the software Media Foundation encoder is
    // always present; a hardware transform is not.
    options.requireHardwareAcceleration = false;
    // Generous for 128x96 so block edges stay crisp enough for index recovery.
    options.bitrateKbps = 1000;
    return options;
}

uint8_t ExpectedGradientGray(uint32_t x)
{
    return static_cast<uint8_t>((x * kGradientMax) / (kWidth - 1));
}

// BGRA frame: horizontal gray gradient plus a white block at x = index * step.
std::vector<uint8_t> MakePatternFrameBGRA(uint32_t frameIndex)
{
    std::vector<uint8_t> pixels(static_cast<size_t>(kWidth) * kHeight * 4u);
    for (uint32_t y = 0; y < kHeight; ++y)
    {
        for (uint32_t x = 0; x < kWidth; ++x)
        {
            const uint8_t gray = ExpectedGradientGray(x);
            uint8_t* px = pixels.data() + (static_cast<size_t>(y) * kWidth + x) * 4u;
            px[0] = gray;
            px[1] = gray;
            px[2] = gray;
            px[3] = 255;
        }
    }
    for (uint32_t y = kRedBarY; y < kRedBarY + kRedBarHeight && y < kHeight; ++y)
    {
        for (uint32_t x = 0; x < kWidth; ++x)
        {
            uint8_t* px = pixels.data() + (static_cast<size_t>(y) * kWidth + x) * 4u;
            px[0] = 0;   // B
            px[1] = 0;   // G
            px[2] = 255; // R
        }
    }
    const uint32_t blockX = frameIndex * kBlockStepX;
    for (uint32_t y = kBlockY; y < kBlockY + kBlockSize && y < kHeight; ++y)
    {
        for (uint32_t x = blockX; x < blockX + kBlockSize && x < kWidth; ++x)
        {
            uint8_t* px = pixels.data() + (static_cast<size_t>(y) * kWidth + x) * 4u;
            px[0] = 255;
            px[1] = 255;
            px[2] = 255;
        }
    }
    return pixels;
}

VideoFrameView ViewOf(const std::vector<uint8_t>& pixels, VideoPixelFormat format = VideoPixelFormat::BGRA8)
{
    VideoFrameView view{};
    view.Pixels = pixels.data();
    view.Width = kWidth;
    view.Height = kHeight;
    view.StrideBytes = kWidth * 4u;
    view.Format = format;
    return view;
}

// Recovers the frame index from a decoded RGBA frame by locating the white
// block's left edge on a row inside the block. Returns -1 if no block is found.
int DecodeFrameIndexFromRGBA(const uint8_t* rgba)
{
    const uint32_t row = kBlockY + kBlockSize / 2;
    int firstWhite = -1;
    int lastWhite = -1;
    for (uint32_t x = 0; x < kWidth; ++x)
    {
        const uint8_t* px = rgba + (static_cast<size_t>(row) * kWidth + x) * 4u;
        const int luminance = (static_cast<int>(px[0]) + px[1] + px[2]) / 3;
        if (luminance > 228)
        {
            if (firstWhite < 0)
                firstWhite = static_cast<int>(x);
            lastWhite = static_cast<int>(x);
        }
    }
    if (firstWhite < 0 || lastWhite - firstWhite < static_cast<int>(kBlockSize) / 2)
        return -1;
    return static_cast<int>(std::lround(static_cast<double>(firstWhite) / kBlockStepX));
}

double GradientRowMeanError(const uint8_t* rgba)
{
    double totalError = 0.0;
    uint32_t samples = 0;
    for (uint32_t x = 4; x < kWidth - 4; ++x)
    {
        const uint8_t* px = rgba + (static_cast<size_t>(kGradientProbeRow) * kWidth + x) * 4u;
        const double decoded = (static_cast<double>(px[0]) + px[1] + px[2]) / 3.0;
        totalError += std::abs(decoded - static_cast<double>(ExpectedGradientGray(x)));
        ++samples;
    }
    return totalError / samples;
}

// Decodes the first video frame with THIS test's own converter, configured with
// the BT.709 matrix the stream is tagged with, and returns the RGB at (x, y).
// This is what separates the encode matrix from the tags: the engine player's
// decode uses swscale defaults, so it cannot see a 601-under-709-tags encode on
// gray content, and tags alone cannot see the coefficients actually used.
bool DecodeFirstFramePixel709(const std::filesystem::path& path, uint32_t x, uint32_t y, int outRGB[3])
{
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.string().c_str(), nullptr, nullptr) < 0)
        return false;
    AVCodecContext* ctx = nullptr;
    SwsContext* sws = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    bool decoded = false;
    do
    {
        if (avformat_find_stream_info(fmt, nullptr) < 0)
            break;
        const AVCodec* decoder = nullptr;
        const int videoIndex = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
        if (videoIndex < 0 || !decoder)
            break;
        ctx = avcodec_alloc_context3(decoder);
        if (!ctx || avcodec_parameters_to_context(ctx, fmt->streams[videoIndex]->codecpar) < 0 ||
            avcodec_open2(ctx, decoder, nullptr) < 0)
            break;
        packet = av_packet_alloc();
        frame = av_frame_alloc();
        while (!decoded && av_read_frame(fmt, packet) >= 0)
        {
            if (packet->stream_index == videoIndex && avcodec_send_packet(ctx, packet) == 0 &&
                avcodec_receive_frame(ctx, frame) == 0)
                decoded = true;
            av_packet_unref(packet);
        }
        if (!decoded)
            break;
        sws = sws_getContext(frame->width, frame->height, static_cast<AVPixelFormat>(frame->format),
                             frame->width, frame->height, AV_PIX_FMT_RGBA, SWS_BILINEAR,
                             nullptr, nullptr, nullptr);
        if (!sws)
        {
            decoded = false;
            break;
        }
        const int* coefficients = sws_getCoefficients(SWS_CS_ITU709);
        if (sws_setColorspaceDetails(sws, coefficients, 0 /*limited yuv*/, coefficients,
                                     1 /*full rgb*/, 0, 1 << 16, 1 << 16) < 0)
        {
            decoded = false;
            break;
        }
        std::vector<uint8_t> rgba(static_cast<size_t>(frame->width) * frame->height * 4u);
        uint8_t* dstData[4] = {rgba.data(), nullptr, nullptr, nullptr};
        const int dstLinesize[4] = {frame->width * 4, 0, 0, 0};
        sws_scale(sws, frame->data, frame->linesize, 0, frame->height, dstData, dstLinesize);
        const uint8_t* px = rgba.data() + (static_cast<size_t>(y) * frame->width + x) * 4u;
        outRGB[0] = px[0];
        outRGB[1] = px[1];
        outRGB[2] = px[2];
    } while (false);
    if (sws)
        sws_freeContext(sws);
    if (frame)
        av_frame_free(&frame);
    if (packet)
        av_packet_free(&packet);
    if (ctx)
        avcodec_free_context(&ctx);
    avformat_close_input(&fmt);
    return decoded;
}

// Container-level facts read straight from the file, so the asserts do not
// depend on the player's decode conventions.
struct ContainerInfo
{
    bool opened = false;
    int videoStreamCount = 0;
    int audioStreamCount = 0;
    int width = 0;
    int height = 0;
    int64_t videoFrameCount = 0;
    double durationSeconds = 0.0;
    AVCodecID videoCodecId = AV_CODEC_ID_NONE;
    AVCodecID audioCodecId = AV_CODEC_ID_NONE;
    int audioSampleRate = 0;
    int audioChannels = 0;
    AVColorSpace colorSpace = AVCOL_SPC_UNSPECIFIED;
    AVColorPrimaries colorPrimaries = AVCOL_PRI_UNSPECIFIED;
    AVColorTransferCharacteristic colorTrc = AVCOL_TRC_UNSPECIFIED;
    AVColorRange colorRange = AVCOL_RANGE_UNSPECIFIED;
    bool hasMasteringMetadata = false;
    bool hasContentLightMetadata = false;
};

ContainerInfo ReadContainerInfo(const std::filesystem::path& path)
{
    ContainerInfo info;
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.string().c_str(), nullptr, nullptr) < 0)
        return info;
    if (avformat_find_stream_info(fmt, nullptr) < 0)
    {
        avformat_close_input(&fmt);
        return info;
    }
    info.opened = true;
    if (fmt->duration != AV_NOPTS_VALUE)
        info.durationSeconds = static_cast<double>(fmt->duration) / AV_TIME_BASE;
    for (unsigned i = 0; i < fmt->nb_streams; ++i)
    {
        const AVCodecParameters* par = fmt->streams[i]->codecpar;
        if (par->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            ++info.videoStreamCount;
            info.width = par->width;
            info.height = par->height;
            info.videoFrameCount = fmt->streams[i]->nb_frames;
            info.videoCodecId = par->codec_id;
            info.colorSpace = par->color_space;
            info.colorPrimaries = par->color_primaries;
            info.colorTrc = par->color_trc;
            info.colorRange = par->color_range;
            info.hasMasteringMetadata =
                av_packet_side_data_get(par->coded_side_data, par->nb_coded_side_data,
                                        AV_PKT_DATA_MASTERING_DISPLAY_METADATA) != nullptr;
            info.hasContentLightMetadata =
                av_packet_side_data_get(par->coded_side_data, par->nb_coded_side_data,
                                        AV_PKT_DATA_CONTENT_LIGHT_LEVEL) != nullptr;
        }
        else if (par->codec_type == AVMEDIA_TYPE_AUDIO)
        {
            ++info.audioStreamCount;
            info.audioCodecId = par->codec_id;
            info.audioSampleRate = par->sample_rate;
            info.audioChannels = par->ch_layout.nb_channels;
        }
    }
    avformat_close_input(&fmt);
    return info;
}

// Decodes the audio stream and returns the RMS of the first channel, skipping
// the leading/trailing skirt where AAC priming and padding live. When
// outChannelSamples is set it receives the TOTAL decoded per-channel sample
// count (no skirt), which is what makes a dropped final partial frame visible.
double DecodedAudioRms(const std::filesystem::path& path, int64_t* outChannelSamples = nullptr)
{
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.string().c_str(), nullptr, nullptr) < 0)
        return -1.0;
    if (avformat_find_stream_info(fmt, nullptr) < 0)
    {
        avformat_close_input(&fmt);
        return -1.0;
    }
    int audioIndex = -1;
    for (unsigned i = 0; i < fmt->nb_streams; ++i)
    {
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
        {
            audioIndex = static_cast<int>(i);
            break;
        }
    }
    if (audioIndex < 0)
    {
        avformat_close_input(&fmt);
        return -1.0;
    }

    const AVCodec* decoder = avcodec_find_decoder(fmt->streams[audioIndex]->codecpar->codec_id);
    AVCodecContext* ctx = decoder ? avcodec_alloc_context3(decoder) : nullptr;
    if (!ctx || avcodec_parameters_to_context(ctx, fmt->streams[audioIndex]->codecpar) < 0 ||
        avcodec_open2(ctx, decoder, nullptr) < 0)
    {
        if (ctx)
            avcodec_free_context(&ctx);
        avformat_close_input(&fmt);
        return -1.0;
    }

    std::vector<float> channelSamples;
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    auto receiveAll = [&]() {
        while (avcodec_receive_frame(ctx, frame) == 0)
        {
            if (frame->format == AV_SAMPLE_FMT_FLTP && frame->data[0])
            {
                const float* plane = reinterpret_cast<const float*>(frame->data[0]);
                channelSamples.insert(channelSamples.end(), plane, plane + frame->nb_samples);
            }
        }
    };
    while (av_read_frame(fmt, packet) >= 0)
    {
        if (packet->stream_index == audioIndex)
        {
            if (avcodec_send_packet(ctx, packet) == 0)
                receiveAll();
        }
        av_packet_unref(packet);
    }
    avcodec_send_packet(ctx, nullptr);
    receiveAll();

    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&ctx);
    avformat_close_input(&fmt);

    if (outChannelSamples)
        *outChannelSamples = static_cast<int64_t>(channelSamples.size());

    constexpr size_t kSkirtSamples = 4800; // 100 ms at 48 kHz on each end
    if (channelSamples.size() <= kSkirtSamples * 2)
        return -1.0;
    double sumSquares = 0.0;
    size_t count = 0;
    for (size_t i = kSkirtSamples; i < channelSamples.size() - kSkirtSamples; ++i)
    {
        sumSquares += static_cast<double>(channelSamples[i]) * channelSamples[i];
        ++count;
    }
    return std::sqrt(sumSquares / static_cast<double>(count));
}

bool EncoderExists(const char* name)
{
    return avcodec_find_encoder_by_name(name) != nullptr;
}
} // namespace

class VideoWriterFFmpegTest : public ::testing::Test
{
  protected:
    void TearDown() override
    {
        std::error_code ec;
        if (!m_OutputPath.empty())
            std::filesystem::remove(m_OutputPath, ec);
    }

    std::filesystem::path m_OutputPath;
};

// The build-level premise the rest of the suite stands on, and the permanent
// form of the encoder-inventory probe: which encoder implementations this
// ffmpeg build carries, and that IsCodecSupported agrees with them.
TEST_F(VideoWriterFFmpegTest, EncoderInventoryMatchesCodecSupport)
{
    const bool h264 = EncoderExists("h264_mf") || EncoderExists("libx264") || EncoderExists("libopenh264");
    const bool hevc = EncoderExists("hevc_mf") || EncoderExists("libx265");
    const bool prores422 = EncoderExists("prores_ks") || EncoderExists("prores_aw") || EncoderExists("prores");
    const bool prores4444 = EncoderExists("prores_ks");

    RecordProperty("h264_mf", EncoderExists("h264_mf"));
    RecordProperty("hevc_mf", EncoderExists("hevc_mf"));
    RecordProperty("aac", EncoderExists("aac"));
    RecordProperty("prores_ks", EncoderExists("prores_ks"));

    EXPECT_EQ(VideoWriter::IsCodecSupported(VideoCodec::H264), h264);
    EXPECT_EQ(VideoWriter::IsCodecSupported(VideoCodec::HEVC), hevc);
    EXPECT_EQ(VideoWriter::IsCodecSupported(VideoCodec::ProRes422), prores422);
    EXPECT_EQ(VideoWriter::IsCodecSupported(VideoCodec::ProRes4444), prores4444);

    EXPECT_TRUE(h264) << "no H264 encoder in this ffmpeg build — the vcpkg ffmpeg port must "
                         "enable mediafoundation (Windows default) or libx264/libopenh264";
    EXPECT_TRUE(EncoderExists("aac"))
        << "no native AAC encoder in this ffmpeg build — movie audio cannot be recorded";
}

// The core closed loop: encode a movie, reopen it with the engine's own
// decoder, and verify count, dimensions, duration, tags, and content.
TEST_F(VideoWriterFFmpegTest, RecordedMovieRoundTripsThroughTheEnginePlayer)
{
    m_OutputPath = MakeOutputPath(".mp4");
    VideoWriter writer;
    ASSERT_TRUE(writer.Open(MakeVideoOnlyOptions(m_OutputPath))) << writer.GetLastError();
    ASSERT_TRUE(writer.IsOpen());

    for (uint32_t i = 0; i < kFrameCount; ++i)
    {
        const std::vector<uint8_t> pixels = MakePatternFrameBGRA(i);
        ASSERT_TRUE(writer.WriteFrame(ViewOf(pixels))) << "frame " << i << ": " << writer.GetLastError();
    }
    ASSERT_TRUE(writer.Close()) << writer.GetLastError();

    // Container-level structure and identity.
    const ContainerInfo info = ReadContainerInfo(m_OutputPath);
    ASSERT_TRUE(info.opened) << "the written file is not a readable container";
    EXPECT_EQ(info.videoStreamCount, 1);
    EXPECT_EQ(info.audioStreamCount, 0);
    EXPECT_EQ(info.videoCodecId, AV_CODEC_ID_H264);
    EXPECT_EQ(info.width, static_cast<int>(kWidth));
    EXPECT_EQ(info.height, static_cast<int>(kHeight));
    EXPECT_EQ(info.videoFrameCount, static_cast<int64_t>(kFrameCount));
    EXPECT_NEAR(info.durationSeconds, kFrameCount / kFps, 0.05);
    EXPECT_EQ(info.colorSpace, AVCOL_SPC_BT709);
    EXPECT_EQ(info.colorPrimaries, AVCOL_PRI_BT709);
    EXPECT_EQ(info.colorTrc, AVCOL_TRC_BT709);
    EXPECT_EQ(info.colorRange, AVCOL_RANGE_MPEG);

    // Matrix identity, not just matrix tags: the red patch decoded with the
    // BT.709 coefficients the stream declares must come back as pure red. A
    // 601-coefficient encode under these tags shifts green here by ~24 codes.
    {
        int rgb[3] = {0, 0, 0};
        ASSERT_TRUE(DecodeFirstFramePixel709(m_OutputPath, kWidth / 2, kRedBarY + kRedBarHeight / 2, rgb));
        EXPECT_NEAR(rgb[0], 255, kRedPatchTolerance) << "red channel off — wrong encode matrix";
        EXPECT_NEAR(rgb[1], 0, kRedPatchTolerance) << "green channel off — wrong encode matrix";
        EXPECT_NEAR(rgb[2], 0, kRedPatchTolerance) << "blue channel off — wrong encode matrix";
    }

    // Closed loop through the engine's decoder.
    VideoPlayer player;
    ASSERT_TRUE(player.Load(m_OutputPath.string())) << "the engine's decoder cannot open the movie";
    EXPECT_EQ(player.GetWidth(), static_cast<int>(kWidth));
    EXPECT_EQ(player.GetHeight(), static_cast<int>(kHeight));
    EXPECT_NEAR(player.GetDuration(), kFrameCount / kFps, 0.1);

    player.SetLoop(false);
    player.Play();
    ASSERT_TRUE(player.WaitForFrame(kFirstFrameTimeoutSeconds)) << "no frame was ever decoded";

    std::vector<uint8_t> rgba(static_cast<size_t>(kWidth) * kHeight * 4u);
    std::vector<int> seenIndices;
    double worstGradientError = 0.0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (player.PollNewFrame())
        {
            player.GetFrameRGBA(rgba.data());
            const int index = DecodeFrameIndexFromRGBA(rgba.data());
            EXPECT_GE(index, 0) << "decoded frame has no recognizable pattern block";
            if (index >= 0 && (seenIndices.empty() || index != seenIndices.back()))
                seenIndices.push_back(index);
            worstGradientError = std::max(worstGradientError, GradientRowMeanError(rgba.data()));
        }
        if (!player.IsPlaying())
            break;
        player.WaitForFrame(0.25f);
    }

    // Playback pacing publishes every source frame; the writer must have
    // produced exactly one movie frame per input frame.
    EXPECT_EQ(player.GetPublishedFrameCount(), static_cast<uint64_t>(kFrameCount));

    ASSERT_FALSE(seenIndices.empty());
    EXPECT_LE(seenIndices.front(), 2) << "playback did not start at the beginning";
    EXPECT_GE(seenIndices.back(), static_cast<int>(kFrameCount) - 3)
        << "playback did not reach the final frames";
    for (size_t i = 1; i < seenIndices.size(); ++i)
    {
        // ±1 slack: block-edge detection can wobble one pixel on a lossy encode.
        // A pts regression reorders frames by far more than one index.
        EXPECT_GE(seenIndices[i], seenIndices[i - 1] - 1)
            << "decoded frame order regressed — presentation timestamps are wrong";
    }
    EXPECT_LT(worstGradientError, kGradientRowMaxMeanError)
        << "decoded gradient deviates from the source — wrong color range or broken conversion";
}

TEST_F(VideoWriterFFmpegTest, AudioTrackRoundTrips)
{
    m_OutputPath = MakeOutputPath(".mp4");
    VideoWriterOptions options = MakeVideoOnlyOptions(m_OutputPath);
    options.recordAudio = true;
    options.audioChannels = 2;
    options.audioSampleRate = 48000;

    VideoWriter writer;
    ASSERT_TRUE(writer.Open(options)) << writer.GetLastError();

    for (uint32_t i = 0; i < 30; ++i)
    {
        const std::vector<uint8_t> pixels = MakePatternFrameBGRA(i);
        ASSERT_TRUE(writer.WriteFrame(ViewOf(pixels))) << writer.GetLastError();
    }

    // One second of a 440 Hz sine at amplitude 0.5 (RMS 0.354), submitted in
    // odd-sized chunks so the writer's frame-size regrouping is exercised.
    constexpr uint32_t kSampleRate = 48000;
    constexpr uint32_t kChunkFrames = 471;
    constexpr float kAmplitude = 0.5f;
    uint32_t submitted = 0;
    while (submitted < kSampleRate)
    {
        const uint32_t frames = std::min(kChunkFrames, kSampleRate - submitted);
        std::vector<float> samples(static_cast<size_t>(frames) * 2u);
        for (uint32_t i = 0; i < frames; ++i)
        {
            const double t = static_cast<double>(submitted + i) / kSampleRate;
            const float value = kAmplitude * static_cast<float>(std::sin(2.0 * 3.14159265358979 * 440.0 * t));
            samples[i * 2u] = value;
            samples[i * 2u + 1u] = value;
        }
        AudioFrameView audio{};
        audio.Samples = samples.data();
        audio.FrameCount = frames;
        audio.Channels = 2;
        audio.SampleRate = kSampleRate;
        ASSERT_TRUE(writer.WriteAudio(audio)) << writer.GetLastError();
        submitted += frames;
    }
    ASSERT_TRUE(writer.Close()) << writer.GetLastError();

    const ContainerInfo info = ReadContainerInfo(m_OutputPath);
    ASSERT_TRUE(info.opened);
    EXPECT_EQ(info.audioStreamCount, 1);
    EXPECT_EQ(info.audioCodecId, AV_CODEC_ID_AAC);
    EXPECT_EQ(info.audioSampleRate, 48000);
    EXPECT_EQ(info.audioChannels, 2);

    int64_t decodedSamples = 0;
    const double rms = DecodedAudioRms(m_OutputPath, &decodedSamples);
    ASSERT_GE(rms, 0.0) << "the audio track did not decode";
    EXPECT_GT(rms, 0.25) << "decoded audio is (near) silence — samples were lost on the way in";
    EXPECT_LT(rms, 0.45) << "decoded audio energy is far above the source sine";
    // Sample conservation: the RMS band cannot see a lost 896-sample final
    // partial frame (a ~1.9% tail). Decoded = submitted + encoder delay and
    // padding; the bounds are calibrated against measured runs (see the
    // red-arm note in the suite report) so a dropped partial lands outside.
    RecordProperty("decodedSamples", static_cast<int>(decodedSamples));
    EXPECT_GE(decodedSamples, 48000);
    EXPECT_LE(decodedSamples, 48000 + 4096);
}

TEST_F(VideoWriterFFmpegTest, ExtendToTimestampLengthensTheMovie)
{
    m_OutputPath = MakeOutputPath(".mp4");
    VideoWriter writer;
    ASSERT_TRUE(writer.Open(MakeVideoOnlyOptions(m_OutputPath))) << writer.GetLastError();

    constexpr uint32_t kShortFrameCount = 10;
    for (uint32_t i = 0; i < kShortFrameCount; ++i)
    {
        const std::vector<uint8_t> pixels = MakePatternFrameBGRA(i);
        VideoFrameView view = ViewOf(pixels);
        view.TimestampSeconds = static_cast<double>(i) / kFps;
        ASSERT_TRUE(writer.WriteFrame(view)) << writer.GetLastError();
    }
    constexpr double kExtendedEndSeconds = 2.0;
    ASSERT_TRUE(writer.ExtendToTimestamp(kExtendedEndSeconds));
    ASSERT_TRUE(writer.Close()) << writer.GetLastError();

    const ContainerInfo info = ReadContainerInfo(m_OutputPath);
    ASSERT_TRUE(info.opened);
    // Without the extension the ten frames span ~0.33 s.
    EXPECT_GT(info.durationSeconds, kExtendedEndSeconds - 0.1)
        << "ExtendToTimestamp did not lengthen the final sample";
}

TEST_F(VideoWriterFFmpegTest, ProResRecordsIntoMov)
{
    if (!VideoWriter::IsCodecSupported(VideoCodec::ProRes422))
        GTEST_SKIP() << "no ProRes encoder in this ffmpeg build";

    m_OutputPath = MakeOutputPath(".mov");
    VideoWriterOptions options = MakeVideoOnlyOptions(m_OutputPath);
    options.codec = VideoCodec::ProRes422;

    VideoWriter writer;
    ASSERT_TRUE(writer.Open(options)) << writer.GetLastError();
    for (uint32_t i = 0; i < 10; ++i)
    {
        const std::vector<uint8_t> pixels = MakePatternFrameBGRA(i);
        ASSERT_TRUE(writer.WriteFrame(ViewOf(pixels))) << writer.GetLastError();
    }
    ASSERT_TRUE(writer.Close()) << writer.GetLastError();

    const ContainerInfo info = ReadContainerInfo(m_OutputPath);
    ASSERT_TRUE(info.opened);
    EXPECT_EQ(info.videoCodecId, AV_CODEC_ID_PRORES);
    EXPECT_EQ(info.videoFrameCount, 10);

    VideoPlayer player;
    ASSERT_TRUE(player.Load(m_OutputPath.string())) << "the engine's decoder cannot open the ProRes movie";
    player.Play();
    EXPECT_TRUE(player.WaitForFrame(kFirstFrameTimeoutSeconds));
}

// Odd dimensions are floored to even for the 4:2:0 encoder, never refused —
// swscale resamples the source to the encoder size either way.
TEST_F(VideoWriterFFmpegTest, OddDimensionsAreFlooredNotRefused)
{
    m_OutputPath = MakeOutputPath(".mp4");
    VideoWriterOptions options = MakeVideoOnlyOptions(m_OutputPath);
    options.width = 127;
    options.height = 95;

    VideoWriter writer;
    ASSERT_TRUE(writer.Open(options)) << writer.GetLastError();
    for (uint32_t i = 0; i < 10; ++i)
    {
        const std::vector<uint8_t> pixels = MakePatternFrameBGRA(i);
        ASSERT_TRUE(writer.WriteFrame(ViewOf(pixels))) << writer.GetLastError();
    }
    ASSERT_TRUE(writer.Close()) << writer.GetLastError();

    const ContainerInfo info = ReadContainerInfo(m_OutputPath);
    ASSERT_TRUE(info.opened);
    EXPECT_EQ(info.width, 126);
    EXPECT_EQ(info.height, 94);
    EXPECT_EQ(info.videoFrameCount, 10);
}

TEST_F(VideoWriterFFmpegTest, RefusalsStateTheProblem)
{
    VideoWriter writer;

    VideoWriterOptions empty;
    EXPECT_FALSE(writer.Open(empty));
    EXPECT_FALSE(writer.GetLastError().empty());

    // WriteFrame before a successful Open must refuse, not crash.
    const std::vector<uint8_t> pixels = MakePatternFrameBGRA(0);
    EXPECT_FALSE(writer.WriteFrame(ViewOf(pixels)));

    // Close without Open is a clean no-op.
    EXPECT_TRUE(writer.Close());

    // ProRes demands a QuickTime container.
    m_OutputPath = MakeOutputPath(".mp4");
    VideoWriterOptions proResIntoMp4 = MakeVideoOnlyOptions(m_OutputPath);
    proResIntoMp4.codec = VideoCodec::ProRes422;
    EXPECT_FALSE(writer.Open(proResIntoMp4));
    EXPECT_NE(writer.GetLastError().find(".mov"), std::string::npos) << writer.GetLastError();

    // HDR demands HEVC.
    VideoWriterOptions hdrH264 = MakeVideoOnlyOptions(m_OutputPath);
    hdrH264.hdrMode = VideoHdrMode::HDR10_PQ;
    hdrH264.codec = VideoCodec::H264;
    EXPECT_FALSE(writer.Open(hdrH264));
    EXPECT_NE(writer.GetLastError().find("HEVC"), std::string::npos) << writer.GetLastError();

    // Mismatched audio must refuse with the mismatch named.
    VideoWriterOptions withAudio = MakeVideoOnlyOptions(m_OutputPath);
    withAudio.recordAudio = true;
    ASSERT_TRUE(writer.Open(withAudio)) << writer.GetLastError();
    std::vector<float> samples(512, 0.0f);
    AudioFrameView wrongRate{};
    wrongRate.Samples = samples.data();
    wrongRate.FrameCount = 256;
    wrongRate.Channels = 2;
    wrongRate.SampleRate = 44100; // writer expects 48000
    EXPECT_FALSE(writer.WriteAudio(wrongRate));
    EXPECT_NE(writer.GetLastError().find("does not match"), std::string::npos) << writer.GetLastError();
    EXPECT_TRUE(writer.Close()) << writer.GetLastError();
}

// HDR needs a 10-bit-capable HEVC encoder, which is a property of the BUILD's
// encoder inventory (the format query is static; ffmpeg's Media Foundation
// wrapper takes no 10-bit software frames in any build — a 10-bit path needs
// e.g. libx265). Either outcome must be loud and specific.
TEST_F(VideoWriterFFmpegTest, HdrEitherRecordsOrExplainsItself)
{
    if (!VideoWriter::IsCodecSupported(VideoCodec::HEVC))
        GTEST_SKIP() << "no HEVC encoder in this ffmpeg build";

    m_OutputPath = MakeOutputPath(".mp4");
    VideoWriterOptions options = MakeVideoOnlyOptions(m_OutputPath);
    options.codec = VideoCodec::HEVC;
    options.hdrMode = VideoHdrMode::HDR10_PQ;
    options.requireHardwareAcceleration = true;

    VideoWriter writer;
    if (!writer.Open(options))
    {
        const std::string& error = writer.GetLastError();
        EXPECT_FALSE(error.empty());
        GTEST_SKIP() << "HDR HEVC encoding unavailable in this build: " << error;
    }

    std::vector<uint8_t> pixels(static_cast<size_t>(kWidth) * kHeight * 4u);
    for (uint32_t i = 0; i < static_cast<size_t>(kWidth) * kHeight; ++i)
    {
        // Mid-gray in 10-bit: R=G=B=512, A=3 → A2B10G10R10 word.
        const uint32_t word = (3u << 30) | (512u << 20) | (512u << 10) | 512u;
        std::memcpy(pixels.data() + static_cast<size_t>(i) * 4u, &word, 4u);
    }
    for (uint32_t i = 0; i < 10; ++i)
        ASSERT_TRUE(writer.WriteFrame(ViewOf(pixels, VideoPixelFormat::RGB10A2))) << writer.GetLastError();
    ASSERT_TRUE(writer.Close()) << writer.GetLastError();

    const ContainerInfo info = ReadContainerInfo(m_OutputPath);
    ASSERT_TRUE(info.opened);
    EXPECT_EQ(info.videoCodecId, AV_CODEC_ID_HEVC);
    EXPECT_EQ(info.colorPrimaries, AVCOL_PRI_BT2020);
    EXPECT_EQ(info.colorTrc, AVCOL_TRC_SMPTE2084);
    EXPECT_TRUE(info.hasMasteringMetadata);
    EXPECT_TRUE(info.hasContentLightMetadata);
}
