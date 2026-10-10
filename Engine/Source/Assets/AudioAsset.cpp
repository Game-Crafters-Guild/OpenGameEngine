#include "Assets/AudioAsset.h"
#include "AssetCore/SharedFileRead.h"
#include "Logger/Logger.h"
#include <algorithm>
#include <cstring>

#include <miniaudio.h>

namespace GameEngine {

AudioAsset::AudioAsset(const GUID& guid, const std::filesystem::path& path)
    : Asset(guid, AssetType::Audio, path)
    , m_Format(AudioFormat::Unknown)
{
}

AudioAsset::~AudioAsset() {
    Unload();
}

bool AudioAsset::Load() {
    if (GetState() == AssetState::Loaded) {
        return true;
    }

    SetState(AssetState::Loading);
    Logger::Log::Info("Loading audio: {}", GetPath().string());

    if (!Exists()) {
        Logger::Log::Error("Audio file does not exist: {}", GetPath().string());
        SetState(AssetState::Failed);
        return false;
    }

    // Read file data
    Vector<uint8> fileData;
    if (!ReadFileBytesShared(GetPath(), fileData)) {
        Logger::Log::Error("Failed to read audio file: {}", GetPath().string());
        SetState(AssetState::Failed);
        return false;
    }

    return LoadFromData(fileData);
}

bool AudioAsset::LoadFromData(const Vector<uint8>& data) {
    if (GetState() == AssetState::Loaded) {
        return true;
    }

    return LoadFromDataWithRuntimeSettings(data, RuntimeSettings{}, data.size());
}

bool AudioAsset::AdoptReloadedPayload(Asset& staged) {
    auto* other = dynamic_cast<AudioAsset*>(&staged);
    if (!other || !other->IsLoaded()) {
        return false;
    }

    // Swap exactly the members Unload() resets plus the decode outputs
    // (format, bit depth, compressed flag). The previous payload moves into
    // `staged`, which frees it when destroyed. Voices that resolved the old
    // PCM pointer keep the same lifetime contract they have under an inline
    // Reload().
    std::swap(m_Format, other->m_Format);
    std::swap(m_SampleRate, other->m_SampleRate);
    std::swap(m_Channels, other->m_Channels);
    std::swap(m_BitDepth, other->m_BitDepth);
    std::swap(m_Duration, other->m_Duration);
    std::swap(m_PcmFormat, other->m_PcmFormat);
    std::swap(m_PcmData, other->m_PcmData);
    std::swap(m_FrameCount, other->m_FrameCount);
    std::swap(m_EncodedData, other->m_EncodedData);
    std::swap(m_IsCompressed, other->m_IsCompressed);
    return true;
}

void AudioAsset::Unload() {
    m_PcmData.clear();
    m_EncodedData.clear();
    m_FrameCount = 0;
    m_Duration = 0.0f;
    m_SampleRate = 0;
    m_Channels = AudioChannels::Mono;
    m_PcmFormat = AudioPCMFormat::Unknown;
    SetState(AssetState::Unloaded);
    
    Logger::Log::Debug("Audio unloaded: {}", GetName());
}

AudioFormat AudioAsset::DetermineFormat(const String& extension) const {
    if (extension == ".wav") return AudioFormat::WAV;
    if (extension == ".mp3") return AudioFormat::MP3;
    if (extension == ".ogg") return AudioFormat::OGG;
    if (extension == ".flac") return AudioFormat::FLAC;
    if (extension == ".aac") return AudioFormat::AAC;
    return AudioFormat::Unknown;
}

bool AudioAsset::ShouldDecodeToPCM(AudioFormat format,
                                  const RuntimeSettings& settings,
                                  size_t fileSizeBytes,
                                  float durationSeconds)
{
    // Explicit policy overrides everything.
    if (settings.loadPolicy == AudioLoadPolicy::DecodeToPCM)
        return true;
    if (settings.loadPolicy == AudioLoadPolicy::Stream)
        return false;

    // WAV files should always be decoded to PCM since they're uncompressed anyway.
    // Streaming WAV doesn't provide benefits and can cause compatibility issues.
    if (format == AudioFormat::WAV)
    {
        Logger::Log::Debug("Audio: WAV file detected, forcing DecodeToPCM (size={} bytes)", fileSizeBytes);
        return true;
    }

    // Auto heuristics for compressed formats.
    // File size heuristic.
    if (fileSizeBytes > 0 && fileSizeBytes <= 256u * 1024u)
        return true;

    // Duration heuristics (only if we could determine it).
    if (durationSeconds > 0.0f)
    {
        if (durationSeconds <= 3.0f)
            return true;
        if (durationSeconds > 10.0f)
            return false;
    }

    // Default: decode (better UX for typical SFX).
    return true;
}

bool AudioAsset::LoadFromDataWithRuntimeSettings(const Vector<uint8>& data,
                                                const RuntimeSettings& settings,
                                                size_t fileSizeBytes)
{
    if (GetState() == AssetState::Loaded)
    {
        return true;
    }

    SetState(AssetState::Loading);
    Logger::Log::Info("Loading audio from memory data: {}", GetName());

    if (data.empty())
    {
        Logger::Log::Error("Empty audio data provided");
        SetState(AssetState::Failed);
        return false;
    }

    String extension = GetExtension();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](char c) { return static_cast<char>(std::tolower(c)); });
    m_Format = DetermineFormat(extension);

    // Probe via miniaudio decoder (cheap header parse; used for duration heuristics + metadata).
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, /*channels*/ 0, /*sampleRate*/ 0);
    ma_decoder decoder{};
    const ma_result ir = ma_decoder_init_memory(data.data(), data.size(), &cfg, &decoder);
    if (ir != MA_SUCCESS)
    {
        Logger::Log::Error("Audio decode init failed for '{}': ma_result={} '{}', format={}, size={} bytes", 
            GetName(), (int)ir, ma_result_description(ir), 
            extension, data.size());
        SetState(AssetState::Failed);
        return false;
    }

    ma_format outFmt = ma_format_unknown;
    ma_uint32 outCh = 0;
    ma_uint32 outSr = 0;
    (void)ma_data_source_get_data_format(reinterpret_cast<ma_data_source*>(&decoder), &outFmt, &outCh, &outSr, nullptr, 0);

    ma_uint64 lengthFrames = 0;
    const ma_result lr = ma_data_source_get_length_in_pcm_frames(reinterpret_cast<ma_data_source*>(&decoder), &lengthFrames);

    float durationSeconds = 0.0f;
    if (lr == MA_SUCCESS && outSr != 0 && lengthFrames != 0)
    {
        durationSeconds = static_cast<float>(lengthFrames) / static_cast<float>(outSr);
    }

    const size_t sizeHint = (fileSizeBytes != 0) ? fileSizeBytes : data.size();
    const bool decodeToPcm = ShouldDecodeToPCM(m_Format, settings, sizeHint, durationSeconds);

    // Populate metadata (even for stream).
    m_PcmFormat = AudioPCMFormat::Unknown;
    m_PcmData.clear();
    m_EncodedData.clear();
    m_FrameCount = 0;
    m_SampleRate = outSr;
    m_Channels = static_cast<AudioChannels>(outCh == 0 ? 1 : outCh);
    m_BitDepth = 32;
    m_Duration = durationSeconds;

    // We treat non-WAV as compressed for heuristics/debug.
    m_IsCompressed = (m_Format != AudioFormat::WAV);

    bool ok = true;

    if (decodeToPcm)
    {
        // Decode full stream to interleaved f32 PCM.
        m_PcmFormat = AudioPCMFormat::F32;

        // If length is known, pre-size. Otherwise read in chunks.
        if (lr == MA_SUCCESS && lengthFrames != 0 && outCh != 0)
        {
            m_PcmData.resize(static_cast<size_t>(lengthFrames * outCh));
            ma_uint64 framesRead = 0;
            const ma_result rr = ma_decoder_read_pcm_frames(&decoder, m_PcmData.data(), lengthFrames, &framesRead);
            if (rr != MA_SUCCESS)
            {
                Logger::Log::Error("Audio decode failed for '{}': ma_result={} '{}', expectedFrames={}, readFrames={}", 
                    GetName(), (int)rr, ma_result_description(rr), lengthFrames, framesRead);
                ok = false;
            }
            else
            {
                m_FrameCount = framesRead;
                // Trim in case fewer frames were read.
                m_PcmData.resize(static_cast<size_t>(framesRead * outCh));
            }
        }
        else
        {
            constexpr ma_uint64 kChunkFrames = 4096;
            std::vector<float> chunk;
            chunk.resize(static_cast<size_t>(kChunkFrames * (outCh == 0 ? 1 : outCh)));

            ma_uint64 totalFrames = 0;
            while (true)
            {
                ma_uint64 framesRead = 0;
                const ma_result rr = ma_decoder_read_pcm_frames(&decoder, chunk.data(), kChunkFrames, &framesRead);
                if (rr != MA_SUCCESS)
                {
                    Logger::Log::Error("Audio decode failed for '{}': ma_result={} '{}', chunk={}, totalFrames={}", 
                        GetName(), (int)rr, ma_result_description(rr), kChunkFrames, totalFrames);
                    ok = false;
                    break;
                }
                if (framesRead == 0)
                {
                    break;
                }

                const size_t samplesRead = static_cast<size_t>(framesRead * (outCh == 0 ? 1 : outCh));
                const size_t oldSize = m_PcmData.size();
                m_PcmData.resize(oldSize + samplesRead);
                std::memcpy(m_PcmData.data() + oldSize, chunk.data(), samplesRead * sizeof(float));
                totalFrames += framesRead;
            }

            m_FrameCount = totalFrames;
        }

        // If duration was unknown, compute it now.
        if (m_Duration <= 0.0f && m_SampleRate != 0 && m_FrameCount != 0)
        {
            m_Duration = static_cast<float>(m_FrameCount) / static_cast<float>(m_SampleRate);
        }
    }
    else
    {
        // Stream: keep encoded data; no full PCM decode.
        m_EncodedData = data;
        m_PcmData.clear();
        m_PcmFormat = AudioPCMFormat::Unknown;
        m_FrameCount = 0;
    }

    ma_decoder_uninit(&decoder);

    if (!ok)
    {
        Unload();
        SetState(AssetState::Failed);
        return false;
    }

    SetState(AssetState::Loaded);
    Logger::Log::Info("Audio loaded: {} (policy={}, {}Hz, {} ch, {:.2f}s)",
                      GetName(),
                      decodeToPcm ? "DecodeToPCM" : "Stream",
                      m_SampleRate,
                      static_cast<int>(m_Channels),
                      m_Duration);
    return true;
}

} // namespace GameEngine
