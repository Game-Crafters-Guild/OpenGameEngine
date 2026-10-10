#include "Video/VideoWriter.h"

#import <AudioToolbox/AudioToolbox.h>
#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <VideoToolbox/VideoToolbox.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <dispatch/dispatch.h>
#include <filesystem>
#include <system_error>
#include <thread>
#include <vector>

namespace GameEngine::Video
{

namespace
{
NSString* CodecToAVCodec(VideoCodec codec)
{
    switch (codec)
    {
    case VideoCodec::HEVC: return AVVideoCodecTypeHEVC;
    case VideoCodec::ProRes422: return AVVideoCodecTypeAppleProRes422;
    case VideoCodec::ProRes4444: return AVVideoCodecTypeAppleProRes4444;
    case VideoCodec::H264:
    case VideoCodec::Auto:
    default:
        return AVVideoCodecTypeH264;
    }
}

CMVideoCodecType CodecToCMCodec(VideoCodec codec)
{
    switch (codec)
    {
    case VideoCodec::HEVC: return kCMVideoCodecType_HEVC;
    case VideoCodec::ProRes422: return kCMVideoCodecType_AppleProRes422;
    case VideoCodec::ProRes4444: return kCMVideoCodecType_AppleProRes4444;
    case VideoCodec::H264:
    case VideoCodec::Auto:
    default:
        return kCMVideoCodecType_H264;
    }
}

NSDictionary* HardwareEncoderSpecification(bool requireHardwareAcceleration)
{
    if (!requireHardwareAcceleration)
        return nil;

    return @{
        (NSString*)kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder: @YES
    };
}

NSString* FileTypeForPath(const std::string& path)
{
    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (ext == ".mp4" || ext == ".m4v")
        return AVFileTypeMPEG4;
    return AVFileTypeQuickTimeMovie;
}

bool IsProResCodec(VideoCodec codec)
{
    return codec == VideoCodec::ProRes422 || codec == VideoCodec::ProRes4444;
}

bool IsHdr(const VideoWriterOptions& options)
{
    return options.hdrMode != VideoHdrMode::Off;
}

// SMPTE ST 2086 mastering-display metadata, big-endian, as VideoToolbox expects
// for kVTCompressionPropertyKey_MasteringDisplayColorVolume. Primaries are the
// fixed BT.2020 set; luminance comes from the requested mastering nits.
NSData* BuildMasteringDisplayColorVolume(const VideoWriterOptions& options)
{
    auto append16 = [](std::vector<uint8_t>& out, uint16_t v) {
        out.push_back(static_cast<uint8_t>(v >> 8));
        out.push_back(static_cast<uint8_t>(v & 0xFF));
    };
    auto append32 = [](std::vector<uint8_t>& out, uint32_t v) {
        out.push_back(static_cast<uint8_t>(v >> 24));
        out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
        out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>(v & 0xFF));
    };

    std::vector<uint8_t> bytes;
    bytes.reserve(24);
    // Display primaries in G, B, R order, chromaticity in units of 0.00002.
    append16(bytes, 8500);  append16(bytes, 39850); // Green (0.170, 0.797)
    append16(bytes, 6550);  append16(bytes, 2300);  // Blue  (0.131, 0.046)
    append16(bytes, 35400); append16(bytes, 14600); // Red   (0.708, 0.292)
    append16(bytes, 15635); append16(bytes, 16450); // White D65 (0.3127, 0.3290)
    // Luminance in units of 0.0001 cd/m².
    const uint32_t maxLum = static_cast<uint32_t>(std::lround(std::max(1.0f, options.hdrMaxMasteringNits) * 10000.0));
    append32(bytes, maxLum);
    append32(bytes, 0u); // min luminance 0.0
    return [NSData dataWithBytes:bytes.data() length:bytes.size()];
}

// MaxCLL / MaxFALL (cd/m²), big-endian, for kVTCompressionPropertyKey_ContentLightLevelInfo.
NSData* BuildContentLightLevelInfo(const VideoWriterOptions& options)
{
    auto clamp16 = [](float v) -> uint16_t {
        return static_cast<uint16_t>(std::clamp(std::lround(v), 0l, 65535l));
    };
    const uint16_t maxCll = clamp16(options.hdrMaxContentLightLevelNits);
    const uint16_t maxFall = clamp16(options.hdrMaxFrameAverageLightLevelNits);
    const uint8_t bytes[4] = {
        static_cast<uint8_t>(maxCll >> 8), static_cast<uint8_t>(maxCll & 0xFF),
        static_cast<uint8_t>(maxFall >> 8), static_cast<uint8_t>(maxFall & 0xFF)};
    return [NSData dataWithBytes:bytes length:sizeof(bytes)];
}

NSString* HdrTransferFunction(VideoHdrMode mode)
{
    return mode == VideoHdrMode::HLG ? (NSString*)kCVImageBufferTransferFunction_ITU_R_2100_HLG
                                     : (NSString*)kCVImageBufferTransferFunction_SMPTE_ST_2084_PQ;
}

// Tag a 10-bit RGB pixel buffer so VideoToolbox converts it to BT.2020 YCbCr
// with the right transfer characteristic. Without these the encoder assumes
// Rec.709 and the stream is mislabelled (players show it as washed-out SDR).
void AttachHdrColorInfo(CVPixelBufferRef pixelBuffer, VideoHdrMode mode)
{
    if (!pixelBuffer)
        return;
    CVBufferSetAttachment(pixelBuffer, kCVImageBufferColorPrimariesKey,
                          kCVImageBufferColorPrimaries_ITU_R_2020, kCVAttachmentMode_ShouldPropagate);
    CVBufferSetAttachment(pixelBuffer, kCVImageBufferTransferFunctionKey,
                          (__bridge CFStringRef)HdrTransferFunction(mode), kCVAttachmentMode_ShouldPropagate);
    CVBufferSetAttachment(pixelBuffer, kCVImageBufferYCbCrMatrixKey,
                          kCVImageBufferYCbCrMatrix_ITU_R_2020, kCVAttachmentMode_ShouldPropagate);
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

void ApplyPixelBufferFade(CVPixelBufferRef pixelBuffer, uint32_t width, uint32_t height, FadeColor color, double sourceAmount)
{
    if (!pixelBuffer || sourceAmount >= 0.999999)
        return;

    uint8_t* dst = static_cast<uint8_t*>(CVPixelBufferGetBaseAddress(pixelBuffer));
    const size_t dstStride = CVPixelBufferGetBytesPerRow(pixelBuffer);
    for (uint32_t y = 0; y < height; ++y)
    {
        uint8_t* row = dst + static_cast<size_t>(y) * dstStride;
        for (uint32_t x = 0; x < width; ++x)
        {
            uint8_t* px = row + static_cast<size_t>(x) * 4u;
            px[0] = BlendToFadeColor(px[0], color, sourceAmount);
            px[1] = BlendToFadeColor(px[1], color, sourceAmount);
            px[2] = BlendToFadeColor(px[2], color, sourceAmount);
        }
    }
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

bool ValidateHardwareEncoderSupport(const VideoWriterOptions& options, std::string& error)
{
    if (!options.requireHardwareAcceleration)
        return true;

    NSDictionary* encoderSpecification = HardwareEncoderSpecification(true);
    CFStringRef encoderID = nullptr;
    CFDictionaryRef supportedProperties = nullptr;
    const OSStatus status = VTCopySupportedPropertyDictionaryForEncoder(
        static_cast<int32_t>(options.width),
        static_cast<int32_t>(options.height),
        CodecToCMCodec(options.codec),
        (__bridge CFDictionaryRef)encoderSpecification,
        &encoderID,
        &supportedProperties);

    if (encoderID)
        CFRelease(encoderID);
    if (supportedProperties)
        CFRelease(supportedProperties);

    if (status == noErr)
        return true;

    error = std::string("Hardware-accelerated ") + ToString(options.codec) +
            " encoder is not available for the requested movie settings (VideoToolbox status " +
            std::to_string(status) + ")";
    return false;
}

std::string NSErrorToString(NSError* error)
{
    if (!error)
        return {};
    NSString* text = [error localizedDescription];
    return text ? std::string([text UTF8String]) : std::string("Unknown AVFoundation error");
}

std::string NSExceptionToString(NSException* exception)
{
    if (!exception)
        return "Unknown AVFoundation exception";
    NSString* reason = [exception reason];
    if (reason && [reason length] > 0)
        return std::string([reason UTF8String]);
    NSString* name = [exception name];
    return name ? std::string([name UTF8String]) : std::string("Unknown AVFoundation exception");
}

CMTime VideoFrameTime(int64_t frameIndex, double fps)
{
    constexpr int32_t kVideoTimeScale = 60000;
    const int64_t value = static_cast<int64_t>(
        std::llround(static_cast<double>(frameIndex) * static_cast<double>(kVideoTimeScale) / fps));
    return CMTimeMake(value, kVideoTimeScale);
}

CMTime VideoTimestampTime(double seconds)
{
    constexpr int32_t kVideoTimeScale = 60000;
    if (!std::isfinite(seconds) || seconds <= 0.0)
        return kCMTimeZero;
    return CMTimeMakeWithSeconds(seconds, kVideoTimeScale);
}

CMTime VideoFrameDuration(double fps)
{
    return VideoFrameTime(1, fps);
}

bool HasExplicitTimestamp(double seconds)
{
    return std::isfinite(seconds) && seconds >= 0.0;
}
}

struct VideoWriter::Impl
{
    AVAssetWriter* writer = nil;
    AVAssetWriterInput* input = nil;
    AVAssetWriterInput* audioInput = nil;
    AVAssetWriterInputPixelBufferAdaptor* adaptor = nil;
    CMAudioFormatDescriptionRef audioFormatDescription = nullptr;
    VideoWriterOptions options{};
    std::string lastError;
    int64_t frameIndex = 0;
    int64_t audioFrameIndex = 0;
    CMTime lastVideoPresentationTime = kCMTimeInvalid;
    CMTime lastVideoEndTime = kCMTimeZero;
    CMTime requestedEndTime = kCMTimeInvalid;
    bool usingExplicitVideoTimestamps = false;
    bool open = false;

    void Reset()
    {
        if (audioFormatDescription)
        {
            CFRelease(audioFormatDescription);
            audioFormatDescription = nullptr;
        }
        writer = nil;
        input = nil;
        audioInput = nil;
        adaptor = nil;
        frameIndex = 0;
        audioFrameIndex = 0;
        lastVideoPresentationTime = kCMTimeInvalid;
        lastVideoEndTime = kCMTimeZero;
        requestedEndTime = kCMTimeInvalid;
        usingExplicitVideoTimestamps = false;
        open = false;
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

    if (options.path.empty() || options.width == 0 || options.height == 0 || options.fps <= 0.0)
    {
        m_Impl->lastError = "Invalid video writer options";
        return false;
    }

    m_Impl->options = options;
    if (m_Impl->options.codec == VideoCodec::Auto)
        m_Impl->options.codec = GuessCodecForMoviePath(options.path);

    NSString* fileType = FileTypeForPath(options.path);
    if (IsProResCodec(m_Impl->options.codec) && ![fileType isEqualToString:AVFileTypeQuickTimeMovie])
    {
        m_Impl->lastError = "ProRes output requires a QuickTime .mov container";
        return false;
    }
    if (IsHdr(m_Impl->options) && m_Impl->options.codec != VideoCodec::HEVC)
    {
        m_Impl->lastError = "HDR recording requires the HEVC codec (10-bit Main10 profile)";
        return false;
    }

    if (!ValidateHardwareEncoderSupport(m_Impl->options, m_Impl->lastError))
        return false;

    std::error_code removeError;
    std::filesystem::remove(options.path, removeError);

    NSString* nsPath = [NSString stringWithUTF8String:options.path.c_str()];
    NSURL* url = [NSURL fileURLWithPath:nsPath];
    NSError* error = nil;
    AVAssetWriter* writer = [[AVAssetWriter alloc] initWithURL:url fileType:fileType error:&error];
    if (!writer)
    {
        m_Impl->lastError = NSErrorToString(error);
        return false;
    }
    if (options.movieFragmentIntervalSeconds > 0.0)
    {
        const double fragmentSeconds = std::max(0.1, options.movieFragmentIntervalSeconds);
        writer.movieFragmentInterval = CMTimeMakeWithSeconds(fragmentSeconds, 60000);
    }

    NSMutableDictionary* outputSettings = [NSMutableDictionary dictionaryWithDictionary:@{
        AVVideoCodecKey: CodecToAVCodec(m_Impl->options.codec),
        AVVideoWidthKey: @(options.width),
        AVVideoHeightKey: @(options.height)
    }];
    if (!IsProResCodec(m_Impl->options.codec))
    {
        NSMutableDictionary* compression = [NSMutableDictionary dictionaryWithDictionary:@{
            AVVideoExpectedSourceFrameRateKey: @(std::max(1, static_cast<int>(std::round(options.fps))))
        }];
        if (options.bitrateKbps > 0)
        {
            const uint32_t bitrate = options.bitrateKbps * 1000u;
            compression[AVVideoAverageBitRateKey] = @(bitrate);
        }
        if (IsHdr(m_Impl->options))
        {
            // 10-bit HEVC, and (for PQ) the static HDR metadata players key off.
            compression[AVVideoProfileLevelKey] = (__bridge NSString*)kVTProfileLevel_HEVC_Main10_AutoLevel;
            if (m_Impl->options.hdrMode == VideoHdrMode::HDR10_PQ)
            {
                compression[(__bridge NSString*)kVTCompressionPropertyKey_MasteringDisplayColorVolume] =
                    BuildMasteringDisplayColorVolume(m_Impl->options);
                compression[(__bridge NSString*)kVTCompressionPropertyKey_ContentLightLevelInfo] =
                    BuildContentLightLevelInfo(m_Impl->options);
            }
        }
        outputSettings[AVVideoCompressionPropertiesKey] = compression;
    }
    if (IsHdr(m_Impl->options))
    {
        // Declares the stream's container/SEI color tags (the 'colr' atom).
        outputSettings[AVVideoColorPropertiesKey] = @{
            AVVideoColorPrimariesKey: AVVideoColorPrimaries_ITU_R_2020,
            AVVideoTransferFunctionKey: (m_Impl->options.hdrMode == VideoHdrMode::HLG
                                             ? AVVideoTransferFunction_ITU_R_2100_HLG
                                             : AVVideoTransferFunction_SMPTE_ST_2084_PQ),
            AVVideoYCbCrMatrixKey: AVVideoYCbCrMatrix_ITU_R_2020
        };
    }
    if (options.requireHardwareAcceleration)
    {
        outputSettings[AVVideoEncoderSpecificationKey] = HardwareEncoderSpecification(true);
    }

    AVAssetWriterInput* input = nil;
    @try
    {
        input = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo
                                                   outputSettings:outputSettings];
    }
    @catch (NSException* exception)
    {
        m_Impl->lastError = NSExceptionToString(exception);
        return false;
    }
    input.expectsMediaDataInRealTime = NO;

    const OSType pixelFormatType =
        IsHdr(m_Impl->options) ? kCVPixelFormatType_ARGB2101010LEPacked : kCVPixelFormatType_32BGRA;
    NSDictionary* pixelAttributes = @{
        (NSString*)kCVPixelBufferPixelFormatTypeKey: @(pixelFormatType),
        (NSString*)kCVPixelBufferWidthKey: @(options.width),
        (NSString*)kCVPixelBufferHeightKey: @(options.height),
        (NSString*)kCVPixelBufferIOSurfacePropertiesKey: @{}
    };
    AVAssetWriterInputPixelBufferAdaptor* adaptor =
        [AVAssetWriterInputPixelBufferAdaptor assetWriterInputPixelBufferAdaptorWithAssetWriterInput:input
                                                                         sourcePixelBufferAttributes:pixelAttributes];

    if (![writer canAddInput:input])
    {
        m_Impl->lastError = "AVAssetWriter cannot add video input for requested codec";
        return false;
    }
    [writer addInput:input];

    AVAssetWriterInput* audioInput = nil;
    CMAudioFormatDescriptionRef audioFormatDescription = nullptr;
    if (options.recordAudio)
    {
        const uint32_t audioChannels = std::max<uint32_t>(1u, options.audioChannels);
        const uint32_t audioSampleRate = std::max<uint32_t>(1u, options.audioSampleRate);
        NSDictionary* audioSettings = @{
            AVFormatIDKey: @(kAudioFormatMPEG4AAC),
            AVNumberOfChannelsKey: @(audioChannels),
            AVSampleRateKey: @(audioSampleRate),
            AVEncoderBitRateKey: @(std::max<uint32_t>(1u, options.audioBitrateKbps) * 1000u)
        };
        @try
        {
            audioInput = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeAudio
                                                             outputSettings:audioSettings];
        }
        @catch (NSException* exception)
        {
            m_Impl->lastError = NSExceptionToString(exception);
            return false;
        }
        audioInput.expectsMediaDataInRealTime = YES;
        if (![writer canAddInput:audioInput])
        {
            m_Impl->lastError = "AVAssetWriter cannot add AAC audio input";
            return false;
        }
        [writer addInput:audioInput];

        AudioStreamBasicDescription asbd{};
        asbd.mSampleRate = static_cast<Float64>(audioSampleRate);
        asbd.mFormatID = kAudioFormatLinearPCM;
        asbd.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
        asbd.mBytesPerPacket = audioChannels * sizeof(float);
        asbd.mFramesPerPacket = 1;
        asbd.mBytesPerFrame = audioChannels * sizeof(float);
        asbd.mChannelsPerFrame = audioChannels;
        asbd.mBitsPerChannel = 32;
        const OSStatus fmtStatus = CMAudioFormatDescriptionCreate(
            kCFAllocatorDefault,
            &asbd,
            0,
            nullptr,
            0,
            nullptr,
            nullptr,
            &audioFormatDescription);
        if (fmtStatus != noErr || !audioFormatDescription)
        {
            m_Impl->lastError = "Failed to create audio format description";
            return false;
        }
    }

    if (![writer startWriting])
    {
        if (audioFormatDescription)
            CFRelease(audioFormatDescription);
        m_Impl->lastError = NSErrorToString(writer.error);
        return false;
    }
    [writer startSessionAtSourceTime:kCMTimeZero];

    m_Impl->writer = writer;
    m_Impl->input = input;
    m_Impl->audioInput = audioInput;
    m_Impl->audioFormatDescription = audioFormatDescription;
    m_Impl->adaptor = adaptor;
    m_Impl->frameIndex = 0;
    m_Impl->audioFrameIndex = 0;
    m_Impl->lastVideoPresentationTime = kCMTimeInvalid;
    m_Impl->lastVideoEndTime = kCMTimeZero;
    m_Impl->requestedEndTime = kCMTimeInvalid;
    m_Impl->usingExplicitVideoTimestamps = false;
    m_Impl->open = true;
    m_Impl->lastError.clear();
    return true;
}

bool VideoWriter::WriteFrame(const VideoFrameView& frame)
{
    if (!m_Impl->open || !m_Impl->writer || !m_Impl->input || !m_Impl->adaptor)
        return false;
    if (!frame.Pixels || frame.Width == 0 || frame.Height == 0)
    {
        m_Impl->lastError = "Invalid video frame";
        return false;
    }

    constexpr uint32_t kMaxWaitMs = 5000;
    for (uint32_t waitMs = 0; waitMs < kMaxWaitMs && !m_Impl->input.readyForMoreMediaData; ++waitMs)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    if (!m_Impl->input.readyForMoreMediaData)
    {
        m_Impl->lastError = "Video encoder did not become ready for input within 5 seconds";
        return false;
    }

    CVPixelBufferRef pixelBuffer = nullptr;
    CVReturn result = CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault,
                                                         m_Impl->adaptor.pixelBufferPool,
                                                         &pixelBuffer);
    if (result != kCVReturnSuccess || !pixelBuffer)
    {
        m_Impl->lastError = "Failed to allocate AVFoundation pixel buffer";
        return false;
    }

    const bool hdr = IsHdr(m_Impl->options);
    if (hdr)
        AttachHdrColorInfo(pixelBuffer, m_Impl->options.hdrMode);

    CVPixelBufferLockBaseAddress(pixelBuffer, 0);
    auto* dst = static_cast<uint8_t*>(CVPixelBufferGetBaseAddress(pixelBuffer));
    const size_t dstStride = CVPixelBufferGetBytesPerRow(pixelBuffer);
    const uint32_t srcStride = frame.StrideBytes ? frame.StrideBytes : frame.Width * 4u;
    const uint32_t dstWidth = m_Impl->options.width;
    const uint32_t dstHeight = m_Impl->options.height;
    const bool sameSize = frame.Width == dstWidth && frame.Height == dstHeight;
    for (uint32_t y = 0; y < dstHeight; ++y)
    {
        const uint32_t srcY = sameSize ? y : static_cast<uint32_t>((static_cast<uint64_t>(y) * frame.Height) / dstHeight);
        const uint8_t* srcRow = frame.Pixels + static_cast<size_t>(srcY) * srcStride;
        uint8_t* dstRow = dst + static_cast<size_t>(y) * dstStride;
        if (hdr)
        {
            // Source is A2B10G10R10 (R in the low 10 bits); the CV buffer is
            // ARGB2101010 little-endian (word = A<<30 | R<<20 | G<<10 | B).
            const uint32_t* src32 = reinterpret_cast<const uint32_t*>(srcRow);
            uint32_t* out32 = reinterpret_cast<uint32_t*>(dstRow);
            for (uint32_t x = 0; x < dstWidth; ++x)
            {
                const uint32_t srcX = sameSize ? x : static_cast<uint32_t>((static_cast<uint64_t>(x) * frame.Width) / dstWidth);
                const uint32_t p = src32[srcX];
                const uint32_t r = p & 0x3FFu;
                const uint32_t g = (p >> 10) & 0x3FFu;
                const uint32_t b = (p >> 20) & 0x3FFu;
                out32[x] = (3u << 30) | (r << 20) | (g << 10) | b; // opaque alpha
            }
            continue;
        }
        if (frame.Format == VideoPixelFormat::BGRA8)
        {
            if (sameSize)
            {
                std::copy(srcRow, srcRow + static_cast<size_t>(dstWidth) * 4u, dstRow);
            }
            else
            {
                for (uint32_t x = 0; x < dstWidth; ++x)
                {
                    const uint32_t srcX = static_cast<uint32_t>((static_cast<uint64_t>(x) * frame.Width) / dstWidth);
                    const uint8_t* src = srcRow + static_cast<size_t>(srcX) * 4u;
                    uint8_t* out = dstRow + static_cast<size_t>(x) * 4u;
                    out[0] = src[0];
                    out[1] = src[1];
                    out[2] = src[2];
                    out[3] = src[3];
                }
            }
        }
        else
        {
            for (uint32_t x = 0; x < dstWidth; ++x)
            {
                const uint32_t srcX = sameSize ? x : static_cast<uint32_t>((static_cast<uint64_t>(x) * frame.Width) / dstWidth);
                const uint8_t* src = srcRow + static_cast<size_t>(srcX) * 4u;
                const uint8_t r = src[0];
                const uint8_t g = src[1];
                const uint8_t b = src[2];
                const uint8_t a = src[3];
                dstRow[x * 4u + 0u] = b;
                dstRow[x * 4u + 1u] = g;
                dstRow[x * 4u + 2u] = r;
                dstRow[x * 4u + 3u] = a;
            }
        }
    }
    CVPixelBufferUnlockBaseAddress(pixelBuffer, 0);

    if (!hdr && m_Impl->options.fadeInEnabled)
    {
        const uint32_t fadeFrames = FadeFrameCount(m_Impl->options);
        if (fadeFrames > 1 && static_cast<uint64_t>(m_Impl->frameIndex) < fadeFrames)
        {
            const double sourceAmount = static_cast<double>(m_Impl->frameIndex) / static_cast<double>(fadeFrames - 1u);
            CVPixelBufferLockBaseAddress(pixelBuffer, 0);
            ApplyPixelBufferFade(pixelBuffer, dstWidth, dstHeight, m_Impl->options.fadeColor, sourceAmount);
            CVPixelBufferUnlockBaseAddress(pixelBuffer, 0);
        }
    }

    CMTime frameTime = kCMTimeZero;
    if (HasExplicitTimestamp(frame.TimestampSeconds))
    {
        frameTime = VideoTimestampTime(frame.TimestampSeconds);
        m_Impl->usingExplicitVideoTimestamps = true;
    }
    else if (m_Impl->usingExplicitVideoTimestamps && CMTIME_IS_VALID(m_Impl->lastVideoPresentationTime))
    {
        frameTime = CMTimeAdd(m_Impl->lastVideoPresentationTime, VideoFrameDuration(m_Impl->options.fps));
    }
    else
    {
        frameTime = VideoFrameTime(m_Impl->frameIndex, m_Impl->options.fps);
    }

    if (CMTIME_IS_VALID(m_Impl->lastVideoPresentationTime) &&
        CMTimeCompare(frameTime, m_Impl->lastVideoPresentationTime) <= 0)
    {
        frameTime = CMTimeAdd(m_Impl->lastVideoPresentationTime, VideoFrameDuration(m_Impl->options.fps));
    }

    const bool ok = [m_Impl->adaptor appendPixelBuffer:pixelBuffer withPresentationTime:frameTime];
    CVPixelBufferRelease(pixelBuffer);

    if (!ok)
    {
        m_Impl->lastError = NSErrorToString(m_Impl->writer.error);
        if (m_Impl->lastError.empty())
            m_Impl->lastError = "AVFoundation rejected video frame";
        return false;
    }

    ++m_Impl->frameIndex;
    m_Impl->lastVideoPresentationTime = frameTime;
    m_Impl->lastVideoEndTime = CMTimeAdd(frameTime, VideoFrameDuration(m_Impl->options.fps));
    return true;
}

bool VideoWriter::WriteAudio(const AudioFrameView& audio)
{
    if (!m_Impl->open || !m_Impl->audioInput || !m_Impl->audioFormatDescription)
        return true;
    if (!audio.Samples || audio.FrameCount == 0)
        return true;
    if (audio.Channels != m_Impl->options.audioChannels || audio.SampleRate != m_Impl->options.audioSampleRate)
    {
        m_Impl->lastError = "Audio frame format does not match writer options";
        return false;
    }

    constexpr uint32_t kMaxAudioWaitMs = 5000;
    for (uint32_t waitMs = 0; waitMs < kMaxAudioWaitMs && !m_Impl->audioInput.readyForMoreMediaData; ++waitMs)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (!m_Impl->audioInput.readyForMoreMediaData)
    {
        m_Impl->lastError = "Audio encoder did not become ready for input within 5 seconds";
        return false;
    }

    std::vector<float> fadedSamples;
    const float* samples = audio.Samples;
    if (m_Impl->options.fadeInEnabled)
    {
        fadedSamples.assign(audio.Samples, audio.Samples + static_cast<size_t>(audio.FrameCount) * audio.Channels);
        ApplySampleFadeIn(
            fadedSamples,
            audio.Channels,
            audio.SampleRate,
            m_Impl->audioFrameIndex,
            m_Impl->options.fadeDurationSeconds);
        samples = fadedSamples.data();
    }

    if (audio.SampleRate > static_cast<uint32_t>(INT32_MAX))
    {
        m_Impl->lastError = "Audio sample rate " + std::to_string(audio.SampleRate) + " exceeds maximum supported value";
        return false;
    }

    const size_t bytes = static_cast<size_t>(audio.FrameCount) * audio.Channels * sizeof(float);
    CMBlockBufferRef blockBuffer = nullptr;
    OSStatus status = CMBlockBufferCreateWithMemoryBlock(
        kCFAllocatorDefault,
        nullptr,
        bytes,
        kCFAllocatorDefault,
        nullptr,
        0,
        bytes,
        0,
        &blockBuffer);
    if (status != noErr || !blockBuffer)
    {
        m_Impl->lastError = "Failed to create audio block buffer";
        return false;
    }

    status = CMBlockBufferReplaceDataBytes(samples, blockBuffer, 0, bytes);
    if (status != noErr)
    {
        CFRelease(blockBuffer);
        m_Impl->lastError = "Failed to copy audio samples";
        return false;
    }

    const CMTime duration = CMTimeMake(1, static_cast<int32_t>(audio.SampleRate));
    const CMTime pts = CMTimeMake(m_Impl->audioFrameIndex, static_cast<int32_t>(audio.SampleRate));
    CMSampleTimingInfo timing{};
    timing.duration = duration;
    timing.presentationTimeStamp = pts;
    timing.decodeTimeStamp = kCMTimeInvalid;

    CMSampleBufferRef sampleBuffer = nullptr;
    status = CMSampleBufferCreate(
        kCFAllocatorDefault,
        blockBuffer,
        true,
        nullptr,
        nullptr,
        m_Impl->audioFormatDescription,
        static_cast<CMItemCount>(audio.FrameCount),
        1,
        &timing,
        0,
        nullptr,
        &sampleBuffer);
    CFRelease(blockBuffer);
    if (status != noErr || !sampleBuffer)
    {
        m_Impl->lastError = "Failed to create audio sample buffer";
        return false;
    }

    const bool ok = [m_Impl->audioInput appendSampleBuffer:sampleBuffer];
    CFRelease(sampleBuffer);
    if (!ok)
    {
        m_Impl->lastError = NSErrorToString(m_Impl->writer.error);
        if (m_Impl->lastError.empty())
            m_Impl->lastError = "AVFoundation rejected audio samples";
        return false;
    }

    m_Impl->audioFrameIndex += audio.FrameCount;
    return true;
}

bool VideoWriter::ExtendToTimestamp(double timestampSeconds)
{
    if (!m_Impl->open)
        return true;
    if (!HasExplicitTimestamp(timestampSeconds))
        return true;

    const CMTime timestamp = VideoTimestampTime(timestampSeconds);
    if (!CMTIME_IS_VALID(m_Impl->requestedEndTime) || CMTimeCompare(timestamp, m_Impl->requestedEndTime) > 0)
    {
        m_Impl->requestedEndTime = timestamp;
    }
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
                const uint32_t r = lastFrame.Format == VideoPixelFormat::BGRA8 ? 2u : 0u;
                const uint32_t g = 1u;
                const uint32_t b = lastFrame.Format == VideoPixelFormat::BGRA8 ? 0u : 2u;
                dst[r] = BlendToFadeColor(src[r], m_Impl->options.fadeColor, sourceAmount);
                dst[g] = BlendToFadeColor(src[g], m_Impl->options.fadeColor, sourceAmount);
                dst[b] = BlendToFadeColor(src[b], m_Impl->options.fadeColor, sourceAmount);
                dst[3] = src[3];
            }
        }

        if (!WriteFrame(frame))
            return false;
        frame.TimestampSeconds = -1.0;
    }

    if (m_Impl->options.recordAudio && m_Impl->audioInput && m_Impl->options.audioChannels > 0 && m_Impl->options.audioSampleRate > 0)
    {
        const double fadeVideoSeconds = static_cast<double>(fadeFrames) / m_Impl->options.fps;
        const uint32_t audioFrames = static_cast<uint32_t>(std::max(1.0, std::round(fadeVideoSeconds * m_Impl->options.audioSampleRate)));
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

    if (m_Impl->frameIndex > 0 || m_Impl->audioFrameIndex > 0)
    {
        CMTime endTime = m_Impl->frameIndex > 0 ? m_Impl->lastVideoEndTime : kCMTimeZero;
        if (CMTIME_IS_VALID(m_Impl->requestedEndTime) && CMTimeCompare(m_Impl->requestedEndTime, endTime) > 0)
        {
            endTime = m_Impl->requestedEndTime;
        }
        if (m_Impl->frameIndex == 0 &&
            m_Impl->audioInput && m_Impl->audioFrameIndex > 0 &&
            m_Impl->options.audioSampleRate > 0 && m_Impl->options.audioSampleRate <= static_cast<uint32_t>(INT32_MAX))
        {
            const CMTime audioEndTime = CMTimeMake(m_Impl->audioFrameIndex, static_cast<int32_t>(m_Impl->options.audioSampleRate));
            if (CMTimeCompare(audioEndTime, endTime) > 0)
            {
                endTime = audioEndTime;
            }
        }
        [m_Impl->writer endSessionAtSourceTime:endTime];
    }

    [m_Impl->input markAsFinished];
    if (m_Impl->audioInput)
        [m_Impl->audioInput markAsFinished];
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    [m_Impl->writer finishWritingWithCompletionHandler:^{
        dispatch_semaphore_signal(done);
    }];
    dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);

    const bool ok = (m_Impl->writer.status == AVAssetWriterStatusCompleted);
    if (!ok)
        m_Impl->lastError = NSErrorToString(m_Impl->writer.error);
    m_Impl->Reset();
    return ok;
}

bool VideoWriter::IsOpen() const
{
    return m_Impl->open;
}

bool VideoWriter::IsReadyForVideoFrame() const
{
    if (!m_Impl->open || !m_Impl->input)
        return false;
    return m_Impl->input.readyForMoreMediaData;
}

const std::string& VideoWriter::GetLastError() const
{
    return m_Impl->lastError;
}

bool VideoWriter::IsCodecSupported(VideoCodec codec)
{
    return codec == VideoCodec::Auto ||
           codec == VideoCodec::H264 ||
           codec == VideoCodec::HEVC ||
           codec == VideoCodec::ProRes422 ||
           codec == VideoCodec::ProRes4444;
}

} // namespace GameEngine::Video
