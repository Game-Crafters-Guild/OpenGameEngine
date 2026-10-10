#pragma once

#include "Audio/AudioHandles.h"
#include "Audio/AudioTypes.h"
#include "AssetCore/GUID.h"
#include "Mathematics/Vector3.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace GameEngine
{
class AssetManager;
class AudioAsset;
}

namespace GameEngine::Audio
{

class AudioSystem
{
public:
    using OutputCaptureCallback = std::function<void(const float* samples,
                                                     std::uint32_t frameCount,
                                                     std::uint32_t channels,
                                                     std::uint32_t sampleRate)>;

    explicit AudioSystem(AssetManager& assets);
    ~AudioSystem();

    AudioSystem(const AudioSystem&) = delete;
    AudioSystem& operator=(const AudioSystem&) = delete;

    bool Initialize(const AudioSystemConfig& config);
    void Shutdown();
    void Update(float dt);

    bool IsInitialized() const { return m_Initialized; }

    // Resolve/cache API
    AudioClipHandle ResolveClip(const GUID& guid);
    AudioClipHandle ResolveClip(const GameEngine::AudioAsset& asset);

    // Play APIs (overloads)
    AudioEmitterHandle Play2D(const GUID& guid, const PlayOptions& opts = {});
    AudioEmitterHandle Play2D(const GameEngine::AudioAsset& asset, const PlayOptions& opts = {});
    AudioEmitterHandle Play2D(AudioClipHandle clip, const PlayOptions& opts = {});

    AudioEmitterHandle Play3D(const GUID& guid,
                              AudioWorldId world,
                              const Mathematics::Vector3& pos,
                              const Mathematics::Vector3& vel,
                              const PlayOptions& opts = {});
    AudioEmitterHandle Play3D(const GameEngine::AudioAsset& asset,
                              AudioWorldId world,
                              const Mathematics::Vector3& pos,
                              const Mathematics::Vector3& vel,
                              const PlayOptions& opts = {});
    AudioEmitterHandle Play3D(AudioClipHandle clip,
                              AudioWorldId world,
                              const Mathematics::Vector3& pos,
                              const Mathematics::Vector3& vel,
                              const PlayOptions& opts = {});

    void SetListener(AudioWorldId world, std::uint32_t listenerIndex, const ListenerState& state);

    // Emitter control (used by ECS + editor)
    void SetEmitter3D(AudioEmitterHandle emitter, AudioWorldId world, const Mathematics::Vector3& pos, const Mathematics::Vector3& vel);
    void Stop(AudioEmitterHandle emitter);
    // Stop all currently alive voices (best-effort). Intended for Editor PlayMode teardown.
    void StopAllVoices();
    bool IsAlive(AudioEmitterHandle emitter) const;
    bool IsPlaying(AudioEmitterHandle emitter) const;

    // Number of currently-alive voices (pending or playing). The voice-level
    // observable for tests and debug overlays; per-voice handles stay private
    // to their owners (e.g. AudioEmitterSystem's pimpl).
    std::size_t GetAliveVoiceCount() const;

    // Bus volume (linear 0..1). Clamped in SetBusVolume.
    void SetBusVolume(AudioBusId bus, float linearVolume);
    float GetBusVolume(AudioBusId bus) const;

    /** Per-bus meter level (0..1) for VU display. Real metering: backend updates from audio; placeholder: peaks at volume and decays. */
    float GetBusMeterLevel(AudioBusId bus) const;

    /** Approximate hardware output buffer latency in milliseconds (period frames / sample rate). 0 if unavailable. */
    float GetOutputLatencyMs() const;

    /** Capture final mixed output PCM from the audio device callback. Pass nullptr to disable. */
    void SetOutputCaptureCallback(OutputCaptureCallback callback);

private:
    class MiniaudioBackend;
    struct Impl;

    AssetManager* m_Assets = nullptr; // not owned
    std::unique_ptr<MiniaudioBackend> m_Backend;
    std::unique_ptr<Impl> m_Impl;
    AudioSystemConfig m_Config{};
    bool m_Initialized = false;
};

} // namespace GameEngine::Audio

