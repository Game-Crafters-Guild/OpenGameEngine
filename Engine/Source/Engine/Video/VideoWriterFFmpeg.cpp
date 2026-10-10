#include "Video/VideoWriter.h"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace GameEngine::Video
{

namespace
{
// Same tick rate as the AVFoundation writer's CMTime scale: exact for the
// common frame rates (24/25/30/50/60 and their NTSC variants).
constexpr int kVideoTimeScale = 60000;

// Encoder-default bitrate when the caller passes 0. libavcodec's own default
// (200 kbit/s) is unusable at any real resolution; 0.12 bits per pixel per
// frame lands 1080p60 near 15 Mbit/s, in the range screen recorders pick.
constexpr double kDefaultH264BitsPerPixel = 0.12;
// HEVC reaches similar quality at a fraction of the H264 rate.
constexpr double kDefaultHevcBitrateScale = 0.6;
constexpr double kKeyframeIntervalSeconds = 2.0;

std::string AvErrorToString(int errorCode)
{
    char text[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(errorCode, text, sizeof(text));
    return text;
}

bool IsMediaFoundationEncoder(const AVCodec* codec)
{
    return std::string_view(codec->name).ends_with("_mf");
}

// Encoder implementations tried per codec, in preference order. Selection never
// crosses codecs: if none of the requested codec's candidates exist the open
// fails loudly rather than substituting a different codec.
const std::vector<const char*>& EncoderCandidates(VideoCodec codec)
{
    static const std::vector<const char*> kH264 = {"h264_mf", "libx264", "libopenh264"};
    static const std::vector<const char*> kHevc = {"hevc_mf", "libx265"};
    static const std::vector<const char*> kProRes422 = {"prores_ks", "prores_aw", "prores"};
    // Only prores_ks encodes the 4444 profile with alpha.
    static const std::vector<const char*> kProRes4444 = {"prores_ks"};

    switch (codec)
    {
    case VideoCodec::HEVC: return kHevc;
    case VideoCodec::ProRes422: return kProRes422;
    case VideoCodec::ProRes4444: return kProRes4444;
    case VideoCodec::H264:
    case VideoCodec::Auto:
    default:
        return kH264;
    }
}

std::string JoinCandidateNames(const std::vector<const char*>& names)
{
    std::string joined;
    for (const char* name : names)
    {
        if (!joined.empty())
            joined += ", ";
        joined += name;
    }
    return joined;
}

const AVCodec* FindEncoder(VideoCodec codec)
{
    for (const char* name : EncoderCandidates(codec))
    {
        if (const AVCodec* encoder = avcodec_find_encoder_by_name(name))
            return encoder;
    }
    return nullptr;
}

bool IsProResCodec(VideoCodec codec)
{
    return codec == VideoCodec::ProRes422 || codec == VideoCodec::ProRes4444;
}

bool IsHdr(const VideoWriterOptions& options)
{
    return options.hdrMode != VideoHdrMode::Off;
}

// mp4/m4v get the MP4 muxer, everything else the QuickTime muxer — the same
// container-by-extension rule as the AVFoundation writer.
const char* ContainerFormatName(const std::string& path)
{
    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (ext == ".mp4" || ext == ".m4v")
        return "mp4";
    return "mov";
}

AVPixelFormat SourcePixelFormat(VideoPixelFormat format)
{
    switch (format)
    {
    case VideoPixelFormat::BGRA8: return AV_PIX_FMT_BGRA;
    // A2B10G10R10 (R in the low 10 bits, A in the top 2) is X2BGR10LE.
    case VideoPixelFormat::RGB10A2: return AV_PIX_FMT_X2BGR10LE;
    case VideoPixelFormat::RGBA8:
    default:
        return AV_PIX_FMT_RGBA;
    }
}

bool EncoderSupportsPixelFormat(const AVCodec* codec, AVPixelFormat format)
{
    const AVPixelFormat* formats = nullptr;
    int count = 0;
    if (avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0,
                                     reinterpret_cast<const void**>(&formats), &count) < 0)
        return false;
    if (!formats)
        return true; // no restriction reported
    for (int i = 0; i < count; ++i)
    {
        if (formats[i] == format)
            return true;
    }
    return false;
}

// The encoder-side frame format. SDR prefers NV12 (the Media Foundation
// native layout) then YUV420P; HDR needs a 10-bit format (P010).
AVPixelFormat SelectEncoderPixelFormat(const AVCodec* codec, const VideoWriterOptions& options,
                                       std::string& error)
{
    if (IsProResCodec(options.codec))
    {
        const AVPixelFormat wanted = options.codec == VideoCodec::ProRes4444
                                         ? AV_PIX_FMT_YUVA444P10LE
                                         : AV_PIX_FMT_YUV422P10LE;
        if (EncoderSupportsPixelFormat(codec, wanted))
            return wanted;
        error = std::string("The ") + codec->name + " encoder does not accept " +
                av_get_pix_fmt_name(wanted) + " input in this ffmpeg build";
        return AV_PIX_FMT_NONE;
    }

    if (IsHdr(options))
    {
        if (EncoderSupportsPixelFormat(codec, AV_PIX_FMT_P010LE))
            return AV_PIX_FMT_P010LE;
        // Static capability of the linked build: the query above probes the
        // codec's declared formats, not the machine, and ffmpeg's Media
        // Foundation wrapper accepts no 10-bit software frames in any build.
        error = std::string("HDR recording needs a 10-bit (P010) capable HEVC encoder; the ") +
                codec->name + " encoder in this ffmpeg build does not accept one. "
                "Record SDR, or add a 10-bit encoder to the build (the libx265 vcpkg feature, "
                "or a future D3D11 hardware-frames path)";
        return AV_PIX_FMT_NONE;
    }

    for (const AVPixelFormat candidate : {AV_PIX_FMT_NV12, AV_PIX_FMT_YUV420P})
    {
        if (EncoderSupportsPixelFormat(codec, candidate))
            return candidate;
    }
    error = std::string("The ") + codec->name +
            " encoder accepts neither NV12 nor YUV420P input; no conversion path exists";
    return AV_PIX_FMT_NONE;
}

int64_t TicksFromSeconds(double seconds)
{
    return static_cast<int64_t>(std::llround(seconds * kVideoTimeScale));
}

int64_t VideoFrameTicks(int64_t frameIndex, double fps)
{
    return static_cast<int64_t>(
        std::llround(static_cast<double>(frameIndex) * kVideoTimeScale / fps));
}

int64_t VideoFrameDurationTicks(double fps)
{
    return std::max<int64_t>(1, VideoFrameTicks(1, fps));
}

bool HasExplicitTimestamp(double seconds)
{
    return std::isfinite(seconds) && seconds >= 0.0;
}

uint32_t FadeFrameCount(const VideoWriterOptions& options)
{
    if (options.fadeDurationSeconds <= 0.0 || options.fps <= 0.0)
        return 0;
    return static_cast<uint32_t>(std::max(1.0, std::round(options.fadeDurationSeconds * options.fps)));
}

uint8_t BlendToFadeColor(uint8_t source, FadeColor color, double sourceAmount)
{
    sourceAmount = std::clamp(sourceAmount, 0.0, 1.0);
    const double fadeValue = color == FadeColor::White ? 255.0 : 0.0;
    const double value = fadeValue * (1.0 - sourceAmount) + static_cast<double>(source) * sourceAmount;
    return static_cast<uint8_t>(std::clamp(std::lround(value), 0l, 255l));
}

void ApplySampleFadeIn(std::vector<float>& samples,
                       uint32_t channels,
                       uint32_t sampleRate,
                       int64_t startFrame,
                       double durationSeconds)
{
    if (samples.empty() || channels == 0 || sampleRate == 0 || durationSeconds <= 0.0)
        return;
    const int64_t fadeFrames = static_cast<int64_t>(std::max(1.0, std::round(durationSeconds * sampleRate)));
    const size_t frameCount = samples.size() / channels;
    for (size_t frame = 0; frame < frameCount; ++frame)
    {
        const int64_t absoluteFrame = startFrame + static_cast<int64_t>(frame);
        if (absoluteFrame >= fadeFrames)
            break;
        const float gain = fadeFrames <= 1 ? 1.0f : static_cast<float>(static_cast<double>(absoluteFrame) / static_cast<double>(fadeFrames - 1));
        for (uint32_t channel = 0; channel < channels; ++channel)
            samples[frame * channels + channel] *= gain;
    }
}
} // namespace

struct VideoWriter::Impl
{
    AVFormatContext* formatCtx = nullptr;
    AVCodecContext* videoCodecCtx = nullptr;
    AVCodecContext* audioCodecCtx = nullptr;
    AVStream* videoStream = nullptr;
    AVStream* audioStream = nullptr;
    SwsContext* swsCtx = nullptr;
    AVFrame* videoFrame = nullptr;
    AVFrame* audioFrame = nullptr;
    AVPacket* packet = nullptr;
    // One encoded video packet is always held back so Close() can extend the
    // final sample's duration when ExtendToTimestamp asked for a longer movie.
    AVPacket* heldVideoPacket = nullptr;
    bool holdingVideoPacket = false;

    VideoWriterOptions options{};
    std::string lastError;
    bool open = false;
    bool headerWritten = false;

    // Cached swscale source description; the context is rebuilt when it changes.
    int swsSrcWidth = 0;
    int swsSrcHeight = 0;
    AVPixelFormat swsSrcFormat = AV_PIX_FMT_NONE;

    int64_t frameIndex = 0;
    int64_t lastVideoPtsTicks = AV_NOPTS_VALUE;
    int64_t lastVideoEndTicks = 0;
    int64_t requestedEndTicks = AV_NOPTS_VALUE;
    bool usingExplicitVideoTimestamps = false;

    // Interleaved float samples not yet grouped into encoder frames, plus the
    // absolute submitted/encoded sample positions.
    std::vector<float> audioFifo;
    int64_t audioSubmittedFrames = 0;
    int64_t audioEncodedFrames = 0;

    std::vector<uint8_t> fadeScratch;

    ~Impl()
    {
        FreeAll();
    }

    void FreeAll()
    {
        if (packet)
            av_packet_free(&packet);
        if (heldVideoPacket)
            av_packet_free(&heldVideoPacket);
        holdingVideoPacket = false;
        if (videoFrame)
            av_frame_free(&videoFrame);
        if (audioFrame)
            av_frame_free(&audioFrame);
        if (swsCtx)
        {
            sws_freeContext(swsCtx);
            swsCtx = nullptr;
        }
        swsSrcWidth = 0;
        swsSrcHeight = 0;
        swsSrcFormat = AV_PIX_FMT_NONE;
        if (videoCodecCtx)
            avcodec_free_context(&videoCodecCtx);
        if (audioCodecCtx)
            avcodec_free_context(&audioCodecCtx);
        if (formatCtx)
        {
            if (formatCtx->pb)
                avio_closep(&formatCtx->pb);
            avformat_free_context(formatCtx);
            formatCtx = nullptr;
        }
        videoStream = nullptr;
        audioStream = nullptr;
        headerWritten = false;

        frameIndex = 0;
        lastVideoPtsTicks = AV_NOPTS_VALUE;
        lastVideoEndTicks = 0;
        requestedEndTicks = AV_NOPTS_VALUE;
        usingExplicitVideoTimestamps = false;
        audioFifo.clear();
        audioSubmittedFrames = 0;
        audioEncodedFrames = 0;
        std::vector<uint8_t>().swap(fadeScratch);
        open = false;
    }

    void SetColorTags(AVCodecContext* ctx) const
    {
        if (IsHdr(options))
        {
            ctx->color_primaries = AVCOL_PRI_BT2020;
            ctx->color_trc = options.hdrMode == VideoHdrMode::HLG ? AVCOL_TRC_ARIB_STD_B67
                                                                  : AVCOL_TRC_SMPTE2084;
            ctx->colorspace = AVCOL_SPC_BT2020_NCL;
        }
        else
        {
            ctx->color_primaries = AVCOL_PRI_BT709;
            ctx->color_trc = AVCOL_TRC_BT709;
            ctx->colorspace = AVCOL_SPC_BT709;
        }
        ctx->color_range = AVCOL_RANGE_MPEG;
        ctx->chroma_sample_location = AVCHROMA_LOC_LEFT;
    }

    // The swscale matrix and the stream's color tags must be the same decision:
    // this returns the coefficient table matching what SetColorTags declared.
    const int* ColorspaceCoefficients() const
    {
        return sws_getCoefficients(IsHdr(options) ? SWS_CS_BT2020 : SWS_CS_ITU709);
    }

    bool EnsureSwsContext(int srcWidth, int srcHeight, AVPixelFormat srcFormat)
    {
        if (swsCtx && srcWidth == swsSrcWidth && srcHeight == swsSrcHeight && srcFormat == swsSrcFormat)
            return true;

        if (swsCtx)
        {
            sws_freeContext(swsCtx);
            swsCtx = nullptr;
        }

        swsCtx = sws_getContext(srcWidth, srcHeight, srcFormat,
                                videoCodecCtx->width, videoCodecCtx->height, videoCodecCtx->pix_fmt,
                                SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!swsCtx)
        {
            lastError = std::string("swscale cannot convert ") + av_get_pix_fmt_name(srcFormat) +
                        " to " + av_get_pix_fmt_name(videoCodecCtx->pix_fmt) +
                        " at the requested size";
            return false;
        }

        // SWS_CS_DEFAULT is BT.601: leaving the matrix unset here encodes HD
        // frames with SD coefficients while the stream is tagged BT.709/BT.2020,
        // and the colors shift visibly on playback. The matrix, the range, and
        // the tags SetColorTags wrote must all come from the same decision.
        const int* coefficients = ColorspaceCoefficients();
        const int srcRange = 1; // RGB input is full range
        const int dstRange = 0; // H264/HEVC/ProRes YCbCr is limited (tv) range
        if (sws_setColorspaceDetails(swsCtx, coefficients, srcRange,
                                     coefficients, dstRange, 0, 1 << 16, 1 << 16) < 0)
        {
            lastError = "swscale rejected the colorspace coefficients for this conversion";
            sws_freeContext(swsCtx);
            swsCtx = nullptr;
            return false;
        }

        swsSrcWidth = srcWidth;
        swsSrcHeight = srcHeight;
        swsSrcFormat = srcFormat;
        return true;
    }

    bool AttachHdrMasteringMetadata()
    {
        AVMasteringDisplayMetadata* mastering = av_mastering_display_metadata_alloc();
        if (!mastering)
        {
            lastError = "Failed to allocate HDR mastering-display metadata";
            return false;
        }
        // Fixed BT.2020 primaries and D65 white point; luminance from the
        // requested mastering nits — the same values the AVFoundation writer
        // feeds VideoToolbox.
        mastering->display_primaries[0][0] = av_make_q(35400, 50000); // R x 0.708
        mastering->display_primaries[0][1] = av_make_q(14600, 50000); // R y 0.292
        mastering->display_primaries[1][0] = av_make_q(8500, 50000);  // G x 0.170
        mastering->display_primaries[1][1] = av_make_q(39850, 50000); // G y 0.797
        mastering->display_primaries[2][0] = av_make_q(6550, 50000);  // B x 0.131
        mastering->display_primaries[2][1] = av_make_q(2300, 50000);  // B y 0.046
        mastering->white_point[0] = av_make_q(15635, 50000);          // 0.3127
        mastering->white_point[1] = av_make_q(16450, 50000);          // 0.3290
        mastering->max_luminance =
            av_d2q(std::max(1.0, static_cast<double>(options.hdrMaxMasteringNits)), 10000);
        mastering->min_luminance = av_make_q(0, 10000);
        mastering->has_primaries = 1;
        mastering->has_luminance = 1;
        if (!av_packet_side_data_add(&videoStream->codecpar->coded_side_data,
                                     &videoStream->codecpar->nb_coded_side_data,
                                     AV_PKT_DATA_MASTERING_DISPLAY_METADATA,
                                     mastering, sizeof(*mastering), 0))
        {
            av_free(mastering);
            lastError = "Failed to attach HDR mastering-display metadata";
            return false;
        }

        size_t clSize = 0;
        AVContentLightMetadata* contentLight = av_content_light_metadata_alloc(&clSize);
        if (!contentLight)
        {
            lastError = "Failed to allocate HDR content-light metadata";
            return false;
        }
        auto clampNits = [](float nits) {
            return static_cast<unsigned>(std::clamp(std::lround(nits), 0l, 65535l));
        };
        contentLight->MaxCLL = clampNits(options.hdrMaxContentLightLevelNits);
        contentLight->MaxFALL = clampNits(options.hdrMaxFrameAverageLightLevelNits);
        if (!av_packet_side_data_add(&videoStream->codecpar->coded_side_data,
                                     &videoStream->codecpar->nb_coded_side_data,
                                     AV_PKT_DATA_CONTENT_LIGHT_LEVEL,
                                     contentLight, clSize, 0))
        {
            av_free(contentLight);
            lastError = "Failed to attach HDR content-light metadata";
            return false;
        }
        return true;
    }

    bool OpenVideoEncoder(const AVCodec* encoder)
    {
        videoCodecCtx = avcodec_alloc_context3(encoder);
        if (!videoCodecCtx)
        {
            lastError = "Failed to allocate video encoder context";
            return false;
        }

        // 4:2:0 encoding needs even dimensions; odd requests are floored (never
        // refused — swscale already resamples source frames to the encoder size).
        videoCodecCtx->width = static_cast<int>(std::max(2u, options.width & ~1u));
        videoCodecCtx->height = static_cast<int>(std::max(2u, options.height & ~1u));
        videoCodecCtx->time_base = AVRational{1, kVideoTimeScale};
        videoCodecCtx->framerate = av_d2q(options.fps, kVideoTimeScale);

        std::string formatError;
        videoCodecCtx->pix_fmt = SelectEncoderPixelFormat(encoder, options, formatError);
        if (videoCodecCtx->pix_fmt == AV_PIX_FMT_NONE)
        {
            lastError = formatError;
            return false;
        }

        SetColorTags(videoCodecCtx);

        if (options.codec == VideoCodec::H264 || options.codec == VideoCodec::HEVC)
        {
            const double defaultBits = static_cast<double>(options.width) * options.height *
                                       options.fps * kDefaultH264BitsPerPixel *
                                       (options.codec == VideoCodec::HEVC ? kDefaultHevcBitrateScale : 1.0);
            videoCodecCtx->bit_rate = options.bitrateKbps > 0
                                          ? static_cast<int64_t>(options.bitrateKbps) * 1000
                                          : static_cast<int64_t>(defaultBits);
            videoCodecCtx->gop_size =
                static_cast<int>(std::max(1.0, std::round(options.fps * kKeyframeIntervalSeconds)));
        }

        if (IsHdr(options))
            videoCodecCtx->profile = AV_PROFILE_HEVC_MAIN_10;

        if (formatCtx->oformat->flags & AVFMT_GLOBALHEADER)
            videoCodecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

        if (IsMediaFoundationEncoder(encoder))
        {
            // hw_encoding=1 restricts Media Foundation to hardware transforms:
            // with no hardware encoder present the open below fails instead of
            // silently using the OS software encoder — the same contract as the
            // AVFoundation writer's requireHardwareAcceleration.
            av_opt_set_int(videoCodecCtx->priv_data, "hw_encoding",
                           options.requireHardwareAcceleration ? 1 : 0, 0);
        }
        else if (IsProResCodec(options.codec) && videoCodecCtx->priv_data &&
                 av_opt_find(videoCodecCtx->priv_data, "profile", nullptr, 0, 0))
        {
            av_opt_set(videoCodecCtx->priv_data, "profile",
                       options.codec == VideoCodec::ProRes4444 ? "4444" : "standard", 0);
        }

        const int openResult = avcodec_open2(videoCodecCtx, encoder, nullptr);
        if (openResult < 0)
        {
            lastError = std::string("The ") + encoder->name + " encoder failed to open (" +
                        AvErrorToString(openResult) + ")";
            if (IsMediaFoundationEncoder(encoder) && options.requireHardwareAcceleration)
            {
                lastError += ". No hardware " + std::string(ToString(options.codec)) +
                             " encoder is available on this machine; pass "
                             "requireHardwareAcceleration=false to use the software encoder";
            }
            return false;
        }

        videoStream = avformat_new_stream(formatCtx, nullptr);
        if (!videoStream)
        {
            lastError = "Failed to create the video stream";
            return false;
        }
        if (avcodec_parameters_from_context(videoStream->codecpar, videoCodecCtx) < 0)
        {
            lastError = "Failed to copy video encoder parameters to the stream";
            return false;
        }
        videoStream->time_base = videoCodecCtx->time_base;
        videoStream->avg_frame_rate = videoCodecCtx->framerate;

        if (IsHdr(options) && options.hdrMode == VideoHdrMode::HDR10_PQ && !AttachHdrMasteringMetadata())
            return false;

        videoFrame = av_frame_alloc();
        if (!videoFrame)
        {
            lastError = "Failed to allocate video frame";
            return false;
        }
        videoFrame->format = videoCodecCtx->pix_fmt;
        videoFrame->width = videoCodecCtx->width;
        videoFrame->height = videoCodecCtx->height;
        if (av_frame_get_buffer(videoFrame, 0) < 0)
        {
            lastError = "Failed to allocate video frame buffer";
            return false;
        }
        videoFrame->color_primaries = videoCodecCtx->color_primaries;
        videoFrame->color_trc = videoCodecCtx->color_trc;
        videoFrame->colorspace = videoCodecCtx->colorspace;
        videoFrame->color_range = videoCodecCtx->color_range;
        return true;
    }

    bool OpenAudioEncoder()
    {
        const AVCodec* encoder = avcodec_find_encoder_by_name("aac");
        if (!encoder)
        {
            lastError = "This ffmpeg build has no AAC audio encoder; rebuild ffmpeg with the "
                        "native AAC encoder or record with audio disabled";
            return false;
        }

        audioCodecCtx = avcodec_alloc_context3(encoder);
        if (!audioCodecCtx)
        {
            lastError = "Failed to allocate audio encoder context";
            return false;
        }

        const uint32_t channels = std::max<uint32_t>(1u, options.audioChannels);
        const uint32_t sampleRate = std::max<uint32_t>(1u, options.audioSampleRate);
        audioCodecCtx->sample_fmt = AV_SAMPLE_FMT_FLTP;
        audioCodecCtx->sample_rate = static_cast<int>(sampleRate);
        av_channel_layout_default(&audioCodecCtx->ch_layout, static_cast<int>(channels));
        audioCodecCtx->bit_rate = static_cast<int64_t>(std::max<uint32_t>(1u, options.audioBitrateKbps)) * 1000;
        audioCodecCtx->time_base = AVRational{1, static_cast<int>(sampleRate)};
        if (formatCtx->oformat->flags & AVFMT_GLOBALHEADER)
            audioCodecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

        const int openResult = avcodec_open2(audioCodecCtx, encoder, nullptr);
        if (openResult < 0)
        {
            lastError = "The AAC audio encoder failed to open (" + AvErrorToString(openResult) +
                        "). AAC supports the standard sample rates (44100, 48000, ...) and up to "
                        "8 channels";
            return false;
        }
        if (audioCodecCtx->frame_size <= 0)
        {
            // EncodeBufferedAudio groups the FIFO by this; zero would spin forever.
            lastError = "The AAC encoder reported no frame size";
            return false;
        }

        audioStream = avformat_new_stream(formatCtx, nullptr);
        if (!audioStream)
        {
            lastError = "Failed to create the audio stream";
            return false;
        }
        if (avcodec_parameters_from_context(audioStream->codecpar, audioCodecCtx) < 0)
        {
            lastError = "Failed to copy audio encoder parameters to the stream";
            return false;
        }
        audioStream->time_base = audioCodecCtx->time_base;

        audioFrame = av_frame_alloc();
        if (!audioFrame)
        {
            lastError = "Failed to allocate audio frame";
            return false;
        }
        audioFrame->format = audioCodecCtx->sample_fmt;
        av_channel_layout_copy(&audioFrame->ch_layout, &audioCodecCtx->ch_layout);
        audioFrame->sample_rate = audioCodecCtx->sample_rate;
        audioFrame->nb_samples = audioCodecCtx->frame_size;
        if (av_frame_get_buffer(audioFrame, 0) < 0)
        {
            lastError = "Failed to allocate audio frame buffer";
            return false;
        }
        return true;
    }

    // Writes through the one-packet holdback: the previously held packet goes
    // to the muxer and the new one takes its place.
    bool WriteVideoPacketHeld(AVPacket* newPacket)
    {
        if (holdingVideoPacket)
        {
            const int result = av_interleaved_write_frame(formatCtx, heldVideoPacket);
            holdingVideoPacket = false;
            if (result < 0)
            {
                av_packet_unref(newPacket);
                lastError = "Failed to write video packet (" + AvErrorToString(result) + ")";
                return false;
            }
        }
        av_packet_move_ref(heldVideoPacket, newPacket);
        holdingVideoPacket = true;
        return true;
    }

    bool DrainVideoPackets()
    {
        for (;;)
        {
            const int received = avcodec_receive_packet(videoCodecCtx, packet);
            if (received == AVERROR(EAGAIN) || received == AVERROR_EOF)
                return true;
            if (received < 0)
            {
                lastError = "Video encode failed (" + AvErrorToString(received) + ")";
                return false;
            }
            av_packet_rescale_ts(packet, videoCodecCtx->time_base, videoStream->time_base);
            packet->duration = av_rescale_q(VideoFrameDurationTicks(options.fps),
                                            AVRational{1, kVideoTimeScale}, videoStream->time_base);
            packet->stream_index = videoStream->index;
            if (!WriteVideoPacketHeld(packet))
                return false;
        }
    }

    bool DrainAudioPackets()
    {
        for (;;)
        {
            const int received = avcodec_receive_packet(audioCodecCtx, packet);
            if (received == AVERROR(EAGAIN) || received == AVERROR_EOF)
                return true;
            if (received < 0)
            {
                lastError = "Audio encode failed (" + AvErrorToString(received) + ")";
                return false;
            }
            av_packet_rescale_ts(packet, audioCodecCtx->time_base, audioStream->time_base);
            packet->stream_index = audioStream->index;
            const int result = av_interleaved_write_frame(formatCtx, packet);
            if (result < 0)
            {
                lastError = "Failed to write audio packet (" + AvErrorToString(result) + ")";
                return false;
            }
        }
    }

    // Groups the FIFO into encoder-sized frames. The AAC encoder consumes exactly
    // frame_size samples per frame; only Close() may send a shorter final frame.
    bool EncodeBufferedAudio(bool flushPartialFinalFrame)
    {
        const uint32_t channels = static_cast<uint32_t>(audioCodecCtx->ch_layout.nb_channels);
        const int frameSize = audioCodecCtx->frame_size;
        for (;;)
        {
            const int64_t bufferedFrames = static_cast<int64_t>(audioFifo.size() / channels);
            int64_t sendFrames = 0;
            if (bufferedFrames >= frameSize)
                sendFrames = frameSize;
            else if (flushPartialFinalFrame && bufferedFrames > 0)
                sendFrames = bufferedFrames;
            else
                return true;

            if (av_frame_make_writable(audioFrame) < 0)
            {
                lastError = "Audio frame buffer is not writable";
                return false;
            }
            audioFrame->nb_samples = static_cast<int>(sendFrames);
            for (uint32_t channel = 0; channel < channels; ++channel)
            {
                float* plane = reinterpret_cast<float*>(audioFrame->data[channel]);
                for (int64_t i = 0; i < sendFrames; ++i)
                    plane[i] = audioFifo[static_cast<size_t>(i) * channels + channel];
            }
            audioFifo.erase(audioFifo.begin(),
                            audioFifo.begin() + static_cast<std::ptrdiff_t>(sendFrames * channels));

            audioFrame->pts = audioEncodedFrames;
            audioEncodedFrames += sendFrames;

            const int sent = avcodec_send_frame(audioCodecCtx, audioFrame);
            if (sent < 0)
            {
                lastError = "Audio encode failed (" + AvErrorToString(sent) + ")";
                return false;
            }
            if (!DrainAudioPackets())
                return false;
        }
    }
};

VideoWriter::VideoWriter()
    : m_Impl(std::make_unique<Impl>())
{
}

VideoWriter::~VideoWriter()
{
    try
    {
        Close();
    }
    catch (...)
    {
        // Destructors must not throw during editor shutdown. Callers that need
        // error details should use Close() while the writer is still owned.
    }
}

bool VideoWriter::Open(const VideoWriterOptions& options)
{
    Close();

    if (options.path.empty() || options.width == 0 || options.height == 0 ||
        !std::isfinite(options.fps) || options.fps <= 0.0)
    {
        m_Impl->lastError = "Invalid video writer options: path, width, height and a finite positive fps must all be set";
        return false;
    }

    m_Impl->options = options;
    if (m_Impl->options.codec == VideoCodec::Auto)
        m_Impl->options.codec = GuessCodecForMoviePath(options.path);

    const char* containerName = ContainerFormatName(options.path);
    if (IsProResCodec(m_Impl->options.codec) && std::string_view(containerName) != std::string_view("mov"))
    {
        m_Impl->lastError = "ProRes output requires a QuickTime .mov container";
        return false;
    }
    if (IsHdr(m_Impl->options) && m_Impl->options.codec != VideoCodec::HEVC)
    {
        m_Impl->lastError = "HDR recording requires the HEVC codec (10-bit Main10 profile)";
        return false;
    }

    const AVCodec* encoder = FindEncoder(m_Impl->options.codec);
    if (!encoder)
    {
        m_Impl->lastError = std::string("This ffmpeg build has no ") + ToString(m_Impl->options.codec) +
                            " encoder (checked " + JoinCandidateNames(EncoderCandidates(m_Impl->options.codec)) +
                            "). Rebuild ffmpeg with one of them enabled, or pick a supported codec";
        return false;
    }

    // FreeAll leaves lastError alone, so the failure text set before a bail-out
    // survives the teardown.
    auto fail = [this] {
        m_Impl->FreeAll();
        return false;
    };

    std::error_code removeError;
    std::filesystem::remove(options.path, removeError);

    int result = avformat_alloc_output_context2(&m_Impl->formatCtx, nullptr, containerName,
                                                options.path.c_str());
    if (result < 0 || !m_Impl->formatCtx)
    {
        m_Impl->lastError = std::string("Failed to create the ") + containerName + " container (" +
                            AvErrorToString(result) + ")";
        return fail();
    }

    if (!m_Impl->OpenVideoEncoder(encoder))
        return fail();

    if (options.recordAudio && !m_Impl->OpenAudioEncoder())
        return fail();

    m_Impl->packet = av_packet_alloc();
    m_Impl->heldVideoPacket = av_packet_alloc();
    if (!m_Impl->packet || !m_Impl->heldVideoPacket)
    {
        m_Impl->lastError = "Failed to allocate encoder packets";
        return fail();
    }

    if (!(m_Impl->formatCtx->oformat->flags & AVFMT_NOFILE))
    {
        result = avio_open(&m_Impl->formatCtx->pb, options.path.c_str(), AVIO_FLAG_WRITE);
        if (result < 0)
        {
            m_Impl->lastError = "Cannot create '" + options.path + "' (" + AvErrorToString(result) +
                                "). Check that the directory exists and is writable";
            return fail();
        }
    }

    AVDictionary* muxerOptions = nullptr;
    av_dict_set(&muxerOptions, "movflags", "+faststart", 0);
    result = avformat_write_header(m_Impl->formatCtx, &muxerOptions);
    av_dict_free(&muxerOptions);
    if (result < 0)
    {
        m_Impl->lastError = std::string("Failed to write the ") + containerName + " header (" +
                            AvErrorToString(result) + ")";
        return fail();
    }
    m_Impl->headerWritten = true;

    m_Impl->open = true;
    m_Impl->lastError.clear();
    return true;
}

bool VideoWriter::WriteFrame(const VideoFrameView& frame)
{
    if (!m_Impl->open)
        return false;
    if (!frame.Pixels || frame.Width == 0 || frame.Height == 0)
    {
        m_Impl->lastError = "Invalid video frame";
        return false;
    }

    const AVPixelFormat srcFormat = SourcePixelFormat(frame.Format);
    const uint32_t bytesPerPixel = 4u; // every VideoPixelFormat is 4 bytes
    const uint32_t srcStride = frame.StrideBytes ? frame.StrideBytes : frame.Width * bytesPerPixel;

    if (!m_Impl->EnsureSwsContext(static_cast<int>(frame.Width), static_cast<int>(frame.Height), srcFormat))
        return false;

    const uint8_t* srcPixels = frame.Pixels;
    const bool sdr8Bit = frame.Format == VideoPixelFormat::RGBA8 || frame.Format == VideoPixelFormat::BGRA8;
    if (m_Impl->options.fadeInEnabled && sdr8Bit)
    {
        const uint32_t fadeFrames = FadeFrameCount(m_Impl->options);
        if (fadeFrames > 1 && static_cast<uint64_t>(m_Impl->frameIndex) < fadeFrames)
        {
            const double sourceAmount =
                static_cast<double>(m_Impl->frameIndex) / static_cast<double>(fadeFrames - 1u);
            m_Impl->fadeScratch.resize(static_cast<size_t>(srcStride) * frame.Height);
            for (uint32_t y = 0; y < frame.Height; ++y)
            {
                const uint8_t* srcRow = frame.Pixels + static_cast<size_t>(y) * srcStride;
                uint8_t* dstRow = m_Impl->fadeScratch.data() + static_cast<size_t>(y) * srcStride;
                for (uint32_t x = 0; x < frame.Width; ++x)
                {
                    const uint8_t* src = srcRow + static_cast<size_t>(x) * bytesPerPixel;
                    uint8_t* dst = dstRow + static_cast<size_t>(x) * bytesPerPixel;
                    dst[0] = BlendToFadeColor(src[0], m_Impl->options.fadeColor, sourceAmount);
                    dst[1] = BlendToFadeColor(src[1], m_Impl->options.fadeColor, sourceAmount);
                    dst[2] = BlendToFadeColor(src[2], m_Impl->options.fadeColor, sourceAmount);
                    dst[3] = src[3];
                }
            }
            srcPixels = m_Impl->fadeScratch.data();
        }
    }

    if (av_frame_make_writable(m_Impl->videoFrame) < 0)
    {
        m_Impl->lastError = "Video frame buffer is not writable";
        return false;
    }

    const uint8_t* srcData[4] = {srcPixels, nullptr, nullptr, nullptr};
    const int srcLinesize[4] = {static_cast<int>(srcStride), 0, 0, 0};
    sws_scale(m_Impl->swsCtx, srcData, srcLinesize, 0, static_cast<int>(frame.Height),
              m_Impl->videoFrame->data, m_Impl->videoFrame->linesize);

    int64_t ptsTicks = 0;
    if (HasExplicitTimestamp(frame.TimestampSeconds))
    {
        ptsTicks = TicksFromSeconds(frame.TimestampSeconds);
        m_Impl->usingExplicitVideoTimestamps = true;
    }
    else if (m_Impl->usingExplicitVideoTimestamps && m_Impl->lastVideoPtsTicks != AV_NOPTS_VALUE)
    {
        ptsTicks = m_Impl->lastVideoPtsTicks + VideoFrameDurationTicks(m_Impl->options.fps);
    }
    else
    {
        ptsTicks = VideoFrameTicks(m_Impl->frameIndex, m_Impl->options.fps);
    }

    if (m_Impl->lastVideoPtsTicks != AV_NOPTS_VALUE && ptsTicks <= m_Impl->lastVideoPtsTicks)
        ptsTicks = m_Impl->lastVideoPtsTicks + VideoFrameDurationTicks(m_Impl->options.fps);

    m_Impl->videoFrame->pts = ptsTicks;

    const int sent = avcodec_send_frame(m_Impl->videoCodecCtx, m_Impl->videoFrame);
    if (sent < 0)
    {
        m_Impl->lastError = "Video encode failed (" + AvErrorToString(sent) + ")";
        return false;
    }
    if (!m_Impl->DrainVideoPackets())
        return false;

    ++m_Impl->frameIndex;
    m_Impl->lastVideoPtsTicks = ptsTicks;
    m_Impl->lastVideoEndTicks = ptsTicks + VideoFrameDurationTicks(m_Impl->options.fps);
    return true;
}

bool VideoWriter::WriteAudio(const AudioFrameView& audio)
{
    if (!m_Impl->open || !m_Impl->audioCodecCtx)
        return true;
    if (!audio.Samples || audio.FrameCount == 0)
        return true;
    if (audio.Channels != m_Impl->options.audioChannels || audio.SampleRate != m_Impl->options.audioSampleRate)
    {
        m_Impl->lastError = "Audio frame format does not match writer options";
        return false;
    }

    const size_t sampleCount = static_cast<size_t>(audio.FrameCount) * audio.Channels;
    if (m_Impl->options.fadeInEnabled)
    {
        std::vector<float> faded(audio.Samples, audio.Samples + sampleCount);
        ApplySampleFadeIn(faded, audio.Channels, audio.SampleRate, m_Impl->audioSubmittedFrames,
                          m_Impl->options.fadeDurationSeconds);
        m_Impl->audioFifo.insert(m_Impl->audioFifo.end(), faded.begin(), faded.end());
    }
    else
    {
        m_Impl->audioFifo.insert(m_Impl->audioFifo.end(), audio.Samples, audio.Samples + sampleCount);
    }
    m_Impl->audioSubmittedFrames += audio.FrameCount;

    return m_Impl->EncodeBufferedAudio(false);
}

bool VideoWriter::ExtendToTimestamp(double timestampSeconds)
{
    if (!m_Impl->open)
        return true;
    if (!HasExplicitTimestamp(timestampSeconds))
        return true;

    const int64_t ticks = TicksFromSeconds(timestampSeconds);
    if (m_Impl->requestedEndTicks == AV_NOPTS_VALUE || ticks > m_Impl->requestedEndTicks)
        m_Impl->requestedEndTicks = ticks;
    return true;
}

bool VideoWriter::WriteEndFade(const VideoFrameView& lastFrame)
{
    // The fade blend is 8-bit sRGB; skip it for the 10-bit HDR path (the source
    // frame isn't 4-byte sRGB and blending toward 0/255 is meaningless in PQ/HLG).
    if (!m_Impl->open || !m_Impl->options.fadeOutEnabled || IsHdr(m_Impl->options))
        return true;
    if (!lastFrame.Pixels || lastFrame.Width == 0 || lastFrame.Height == 0 || lastFrame.StrideBytes == 0)
        return true;

    const uint32_t fadeFrames = FadeFrameCount(m_Impl->options);
    if (fadeFrames == 0)
        return true;

    const size_t rowBytes = static_cast<size_t>(lastFrame.Width) * 4u;
    std::vector<uint8_t> pixels(rowBytes * lastFrame.Height);
    VideoFrameView frame = lastFrame;
    frame.Pixels = pixels.data();
    frame.StrideBytes = static_cast<uint32_t>(rowBytes);

    for (uint32_t fadeFrame = 1; fadeFrame <= fadeFrames; ++fadeFrame)
    {
        const double sourceAmount = 1.0 - (static_cast<double>(fadeFrame) / static_cast<double>(fadeFrames));
        for (uint32_t y = 0; y < lastFrame.Height; ++y)
        {
            const uint8_t* srcRow = lastFrame.Pixels + static_cast<size_t>(y) * lastFrame.StrideBytes;
            uint8_t* dstRow = pixels.data() + static_cast<size_t>(y) * rowBytes;
            for (uint32_t x = 0; x < lastFrame.Width; ++x)
            {
                const uint8_t* src = srcRow + static_cast<size_t>(x) * 4u;
                uint8_t* dst = dstRow + static_cast<size_t>(x) * 4u;
                dst[0] = BlendToFadeColor(src[0], m_Impl->options.fadeColor, sourceAmount);
                dst[1] = BlendToFadeColor(src[1], m_Impl->options.fadeColor, sourceAmount);
                dst[2] = BlendToFadeColor(src[2], m_Impl->options.fadeColor, sourceAmount);
                dst[3] = src[3];
            }
        }

        if (!WriteFrame(frame))
            return false;
        frame.TimestampSeconds = -1.0;
    }

    if (m_Impl->audioCodecCtx && m_Impl->options.audioChannels > 0 && m_Impl->options.audioSampleRate > 0)
    {
        const double fadeVideoSeconds = static_cast<double>(fadeFrames) / m_Impl->options.fps;
        const uint32_t audioFrames = static_cast<uint32_t>(
            std::max(1.0, std::round(fadeVideoSeconds * m_Impl->options.audioSampleRate)));
        std::vector<float> silence(static_cast<size_t>(audioFrames) * m_Impl->options.audioChannels, 0.0f);
        AudioFrameView audio{};
        audio.Samples = silence.data();
        audio.FrameCount = audioFrames;
        audio.Channels = m_Impl->options.audioChannels;
        audio.SampleRate = m_Impl->options.audioSampleRate;
        if (!WriteAudio(audio))
            return false;
    }

    return true;
}

bool VideoWriter::Close()
{
    if (!m_Impl->open)
        return true;

    bool ok = true;

    // Flush the encoders: partial final audio frame, then both codec delays.
    if (m_Impl->audioCodecCtx)
    {
        ok = m_Impl->EncodeBufferedAudio(true) && ok;
        if (ok)
        {
            const int sent = avcodec_send_frame(m_Impl->audioCodecCtx, nullptr);
            if (sent < 0 && sent != AVERROR_EOF)
            {
                m_Impl->lastError = "Audio encoder flush failed (" + AvErrorToString(sent) + ")";
                ok = false;
            }
            ok = m_Impl->DrainAudioPackets() && ok;
        }
    }

    if (ok)
    {
        const int sent = avcodec_send_frame(m_Impl->videoCodecCtx, nullptr);
        if (sent < 0 && sent != AVERROR_EOF)
        {
            m_Impl->lastError = "Video encoder flush failed (" + AvErrorToString(sent) + ")";
            ok = false;
        }
        ok = m_Impl->DrainVideoPackets() && ok;
    }

    if (ok && m_Impl->holdingVideoPacket)
    {
        // ExtendToTimestamp extends the movie by lengthening the final video
        // sample rather than duplicating frames.
        if (m_Impl->requestedEndTicks != AV_NOPTS_VALUE &&
            m_Impl->heldVideoPacket->pts != AV_NOPTS_VALUE)
        {
            const int64_t requestedEnd = av_rescale_q(m_Impl->requestedEndTicks,
                                                      AVRational{1, kVideoTimeScale},
                                                      m_Impl->videoStream->time_base);
            const int64_t extendedDuration = requestedEnd - m_Impl->heldVideoPacket->pts;
            if (extendedDuration > m_Impl->heldVideoPacket->duration)
                m_Impl->heldVideoPacket->duration = extendedDuration;
        }
        const int result = av_interleaved_write_frame(m_Impl->formatCtx, m_Impl->heldVideoPacket);
        m_Impl->holdingVideoPacket = false;
        if (result < 0)
        {
            m_Impl->lastError = "Failed to write the final video packet (" + AvErrorToString(result) + ")";
            ok = false;
        }
    }

    if (m_Impl->headerWritten)
    {
        const int result = av_write_trailer(m_Impl->formatCtx);
        if (result < 0)
        {
            if (ok)
                m_Impl->lastError = "Failed to finalize the movie (" + AvErrorToString(result) + ")";
            ok = false;
        }
    }

    const std::string error = m_Impl->lastError;
    m_Impl->FreeAll();
    m_Impl->lastError = error;
    return ok;
}

bool VideoWriter::IsOpen() const
{
    return m_Impl->open;
}

bool VideoWriter::IsReadyForVideoFrame() const
{
    // The ffmpeg pipeline is push-based with no input-side backpressure.
    return m_Impl->open;
}

const std::string& VideoWriter::GetLastError() const
{
    return m_Impl->lastError;
}

bool VideoWriter::IsCodecSupported(VideoCodec codec)
{
    // "An encoder implementation exists in this build" — a Media Foundation
    // encoder can still fail at Open() when the machine lacks the transform
    // (hevc_mf with no hardware HEVC encoder); Open() reports that distinctly.
    return FindEncoder(codec) != nullptr;
}

} // namespace GameEngine::Video
