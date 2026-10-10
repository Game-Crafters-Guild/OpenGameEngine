#pragma once

#include "AssetCore/Asset.h"
#include "Types/Types.h"

namespace GameEngine {

/**
 * @brief Audio format types
 */
enum class AudioFormat {
    Unknown,
    WAV,
    MP3,
    OGG,
    FLAC,
    AAC
};

/**
 * @brief Audio channel configuration
 */
enum class AudioChannels {
    Mono = 1,
    Stereo = 2,
    Surround5_1 = 6,
    Surround7_1 = 8
};

/**
 * @brief Runtime load policy for an audio asset.
 *
 * This is a runtime policy (not file format). It can be driven by per-asset metadata
 * in the authoritative AssetRegistry database.
 */
enum class AudioLoadPolicy : uint8
{
    Auto = 0,        ///< Engine decides based on heuristics and optional metadata.
    DecodeToPCM,     ///< Fully decode to PCM at load time.
    Stream           ///< Keep encoded data; decode on demand (per-voice).
};

/**
 * @brief PCM storage format for decoded audio data.
 */
enum class AudioPCMFormat : uint8
{
    Unknown = 0,
    F32
};

/**
 * @brief Audio asset implementation
 * 
 * Handles loading and management of audio files including WAV, MP3, OGG, FLAC, and AAC formats.
 * Provides basic audio data access and format information for audio systems.
 */
class AudioAsset : public Asset {
public:
    AudioAsset(const GUID& guid, const std::filesystem::path& path);
    virtual ~AudioAsset();

    struct RuntimeSettings
    {
        AudioLoadPolicy loadPolicy = AudioLoadPolicy::Auto;
        bool allowVirtualization = true;
    };

    /**
     * @brief Load audio from file
     */
    bool Load() override;

    /**
     * @brief Load audio from memory data (for async loading)
     */
    bool LoadFromData(const Vector<uint8>& data) override;

    // Extended load entry point used by the Asset pipeline which has access to per-asset metadata.
    bool LoadFromDataWithRuntimeSettings(const Vector<uint8>& data, const RuntimeSettings& settings, size_t fileSizeBytes = 0);

    /**
     * @brief Unload audio from memory
     */
    void Unload() override;

    /**
     * @brief Hot-reload through the async pipeline: file read + decode runs on
     * workers, the decoded payload is adopted in place on the main thread
     * (AdoptReloadedPayload below). Same contract as TextureAsset.
     */
    bool SupportsAsyncReload() const override { return true; }

    /**
     * @brief Get audio format
     */
    AudioFormat GetFormat() const { return m_Format; }

    /**
     * @brief Get sample rate in Hz
     */
    uint32 GetSampleRate() const { return m_SampleRate; }

    /**
     * @brief Get number of channels
     */
    AudioChannels GetChannels() const { return m_Channels; }

    /**
     * @brief Get bit depth
     */
    uint32 GetBitDepth() const { return m_BitDepth; }

    /**
     * @brief Get duration in seconds
     */
    float GetDuration() const { return m_Duration; }

    /**
     * @brief Get raw PCM audio data pointer (interleaved). Format is returned by GetPCMFormat().
     *
     * For legacy compatibility, this is exposed as a byte pointer.
     */
    const uint8* GetAudioData() const { return reinterpret_cast<const uint8*>(m_PcmData.data()); }

    AudioPCMFormat GetPCMFormat() const { return m_PcmFormat; }
    const float* GetPCMFloatData() const { return m_PcmData.empty() ? nullptr : m_PcmData.data(); }
    uint64 GetPCMFrameCount() const { return m_FrameCount; }

    /**
     * @brief Get audio data size in bytes
     */
    uint64 GetDataSize() const { return static_cast<uint64>(m_PcmData.size() * sizeof(float)); }

    const uint8* GetEncodedData() const { return m_EncodedData.empty() ? nullptr : m_EncodedData.data(); }
    uint64 GetEncodedDataSize() const { return static_cast<uint64>(m_EncodedData.size()); }
    bool HasEncodedData() const { return !m_EncodedData.empty(); }

    /**
     * @brief Check if audio is compressed
     */
    bool IsCompressed() const { return m_IsCompressed; }

    /**
     * @brief Get number of samples
     */
    uint64 GetSampleCount() const { return m_FrameCount; }

    size_t GetMemoryUsage() const override { return (m_PcmData.size() * sizeof(float)) + m_EncodedData.size(); }

protected:
    /**
     * @brief Swap the decoded audio payload with a freshly loaded instance
     * (main thread, member swaps only). See Asset::AdoptReloadedPayload.
     */
    bool AdoptReloadedPayload(Asset& staged) override;

private:
    /**
     * @brief Determine audio format from file extension
     */
    AudioFormat DetermineFormat(const String& extension) const;

    static bool ShouldDecodeToPCM(AudioFormat format,
                                  const RuntimeSettings& settings,
                                  size_t fileSizeBytes,
                                  float durationSeconds);

private:
    AudioFormat m_Format;
    uint32 m_SampleRate = 0;
    AudioChannels m_Channels = AudioChannels::Mono;
    uint32 m_BitDepth = 32; // decoded PCM is f32
    float m_Duration = 0.0f;

    AudioPCMFormat m_PcmFormat = AudioPCMFormat::Unknown;
    Vector<float> m_PcmData;     // interleaved f32 PCM
    uint64 m_FrameCount = 0;

    Vector<uint8> m_EncodedData; // present when loadPolicy==Stream (or for debugging)
    bool m_IsCompressed = false;

    DISALLOW_COPY_AND_ASSIGN(AudioAsset);
};

} // namespace GameEngine
