#include "Audio/AudioSystem.h"

#include "Assets/AssetManager.h"
#include "Assets/AudioAsset.h"
#include "AssetCore/AssetTypes.h"
#include "Logger/Logger.h"

#include "MiniaudioBackend.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <future>
#include <limits>
#include <optional>
#include <mutex>
#include <string>
#include <typeinfo>
#include <unordered_map>
#include <vector>

namespace GameEngine::Audio
{

namespace
{
static std::uint8_t NextGeneration(std::uint8_t g)
{
    g = static_cast<std::uint8_t>(g + 1u);
    if (g == 0)
        g = 1;
    return g;
}
} // namespace

struct AudioSystem::Impl
{
    enum class ClipState : std::uint8_t
    {
        Empty = 0,
        Loading,
        ReadyPcm,
        ReadyEncoded,
        Failed
    };

    struct ClipSlot
    {
        std::uint8_t generation = 1;
        bool alive = false;
        GUID guid{};
        ClipState state = ClipState::Empty;
        std::string error;

        // Async load (GUID path)
        std::optional<AssetFuture> future;

        // PCM path (owned storage; immutable while any voice plays)
        ma_format pcmFormat = ma_format_unknown;
        ma_uint32 channels = 0;
        ma_uint32 sampleRate = 0;
        ma_uint64 frameCount = 0;
        std::vector<float> pcmF32; // interleaved f32

        // Encoded path (owned storage; immutable while any voice plays)
        std::vector<std::uint8_t> encodedBytes;
    };

    static bool FillFromAudioAsset(const AudioAsset& a, ClipSlot& c)
    {
        c.error.clear();
        c.pcmF32.clear();
        c.encodedBytes.clear();

        c.channels = static_cast<ma_uint32>(static_cast<int>(a.GetChannels()));
        c.sampleRate = static_cast<ma_uint32>(a.GetSampleRate());

        if (a.GetPCMFormat() == AudioPCMFormat::F32 && a.GetPCMFloatData() != nullptr && a.GetPCMFrameCount() > 0 && c.channels != 0)
        {
            const ma_uint64 frames = static_cast<ma_uint64>(a.GetPCMFrameCount());
            const size_t samples = static_cast<size_t>(frames * c.channels);

            c.pcmF32.resize(samples);
            std::memcpy(c.pcmF32.data(), a.GetPCMFloatData(), samples * sizeof(float));

            c.pcmFormat = ma_format_f32;
            c.frameCount = frames;
            c.state = AudioSystem::Impl::ClipState::ReadyPcm;
            return true;
        }

        if (a.HasEncodedData() && a.GetEncodedData() != nullptr && a.GetEncodedDataSize() != 0)
        {
            c.encodedBytes.resize(static_cast<size_t>(a.GetEncodedDataSize()));
            std::memcpy(c.encodedBytes.data(), a.GetEncodedData(), c.encodedBytes.size());

            c.pcmFormat = ma_format_unknown;
            c.frameCount = 0;
            c.state = AudioSystem::Impl::ClipState::ReadyEncoded;
            return true;
        }

        c.pcmFormat = ma_format_unknown;
        c.frameCount = 0;
        c.state = AudioSystem::Impl::ClipState::Failed;
        c.error = "AudioAsset has no playable PCM or encoded data";
        return false;
    }

    enum class VoiceState : std::uint8_t
    {
        Empty = 0,
        Pending,
        Playing
    };

    enum class SourceType : std::uint8_t
    {
        None = 0,
        PcmBufferRef,
        Decoder
    };

    struct VoiceSlot
    {
        std::uint8_t generation = 1;
        bool alive = false;
        VoiceState state = VoiceState::Empty;
        std::uint64_t startSeq = 0; // monotonically increasing when a voice begins playing (for simple priority/stealing)

        AudioClipHandle clip = INVALID_AUDIO_CLIP_HANDLE;
        PlayOptions opts{};
        bool is3D = false;
        AudioWorldId world = 0;
        Mathematics::Vector3 pos{};
        Mathematics::Vector3 vel{};

        SourceType sourceType = SourceType::None;

        // Miniaudio objects
        ma_sound sound{};
        bool soundInit = false;

        ma_audio_buffer_ref bufferRef{};
        bool bufferRefInit = false;

        ma_decoder decoder{};
        bool decoderInit = false;
    };

    // Asset hot-reload integration (queue-only callback)
    std::mutex assetEventMutex;
    std::vector<GUID> pendingAudioReloads;
    std::vector<GUID> pendingAudioDestroys;
    uint32 assetCallbackHandle = 0;

    std::unordered_map<GUID, AudioClipHandle> guidToClip;
    std::vector<ClipSlot> clips;
    std::vector<std::uint64_t> freeClips;

    std::vector<VoiceSlot> voices;
    std::vector<std::uint64_t> freeVoices;
    std::uint64_t nextVoiceStartSeq = 1;
};

AudioSystem::AudioSystem(AssetManager& assets)
    : m_Assets(&assets)
{
    m_Impl = std::make_unique<Impl>();
}

AudioSystem::~AudioSystem()
{
    Shutdown();
}

bool AudioSystem::Initialize(const AudioSystemConfig& config)
{
    if (m_Initialized)
    {
        return true;
    }

    m_Config = config;

    m_Backend = std::make_unique<MiniaudioBackend>();
    if (!m_Backend->Initialize(config))
    {
        Logger::Log::Warning("AudioSystem: backend init failed; audio disabled");
        m_Backend.reset();
        m_Initialized = false;
        return false;
    }

    m_Initialized = true;
    if (m_Impl)
    {
        m_Impl->clips.reserve(256);
        m_Impl->voices.reserve(config.maxVoices);
        m_Impl->freeVoices.reserve(config.maxVoices);
    }

    // Install asset event callback (queue-only; may be called from non-main threads).
    if (m_Assets && m_Impl && m_Impl->assetCallbackHandle == 0)
    {
        m_Impl->assetCallbackHandle = m_Assets->GetEventDispatcher().AddCallback(
            [this](const AssetEvent& e)
            {
                if (!m_Impl)
                    return;
                if (e.Type != AssetType::Audio)
                    return;

                std::lock_guard<std::mutex> lk(m_Impl->assetEventMutex);
                if (e.EventType == AssetEventType::AssetReloaded)
                {
                    m_Impl->pendingAudioReloads.push_back(e.AssetGuid);
                }
                else if (e.EventType == AssetEventType::AssetDestroyed)
                {
                    m_Impl->pendingAudioDestroys.push_back(e.AssetGuid);
                }
            });
    }

    Logger::Log::Info("AudioSystem initialized");
    return true;
}

void AudioSystem::Shutdown()
{
    if (!m_Initialized)
    {
        return;
    }

    if (m_Backend)
    {
        // Stop and free all voices before tearing down engine.
        if (m_Impl)
        {
            for (auto& v : m_Impl->voices)
            {
                if (!v.alive)
                    continue;
                if (v.soundInit)
                {
                    ma_sound_stop(&v.sound);
                    ma_sound_uninit(&v.sound);
                    v.soundInit = false;
                }
                if (v.decoderInit)
                {
                    ma_decoder_uninit(&v.decoder);
                    v.decoderInit = false;
                }
                if (v.bufferRefInit)
                {
                    ma_audio_buffer_ref_uninit(&v.bufferRef);
                    v.bufferRefInit = false;
                }
                v.alive = false;
                v.state = Impl::VoiceState::Empty;
                v.startSeq = 0;
            }
        }

        m_Backend->Shutdown();
        m_Backend.reset();
    }

    if (m_Impl)
    {
        if (m_Assets && m_Impl->assetCallbackHandle != 0)
        {
            m_Assets->GetEventDispatcher().RemoveCallback(m_Impl->assetCallbackHandle);
            m_Impl->assetCallbackHandle = 0;
        }

        m_Impl->guidToClip.clear();
        m_Impl->clips.clear();
        m_Impl->freeClips.clear();
        m_Impl->voices.clear();
        m_Impl->freeVoices.clear();
    }

    m_Initialized = false;
    Logger::Log::Info("AudioSystem shutdown");
}

void AudioSystem::Update(float dt)
{
    if (!m_Initialized || !m_Backend)
    {
        return;
    }
    m_Backend->Update(dt);

    if (!m_Impl)
        return;

    // 0) Pump asset events (thread-safe queue). This is important for hot-reload:
    // we stop affected voices and refresh clip data from the reloaded asset.
    {
        std::vector<GUID> reloads;
        std::vector<GUID> destroys;
        {
            std::lock_guard<std::mutex> lk(m_Impl->assetEventMutex);
            reloads.swap(m_Impl->pendingAudioReloads);
            destroys.swap(m_Impl->pendingAudioDestroys);
        }

        auto dedupe = [](std::vector<GUID>& v)
        {
            if (v.size() <= 1)
                return;
            std::sort(v.begin(), v.end());
            v.erase(std::unique(v.begin(), v.end()), v.end());
        };

        dedupe(reloads);
        dedupe(destroys);

        auto stopVoicesForClip = [&](AudioClipHandle clip)
        {
            if (!clip.IsValid())
                return;
            for (std::uint64_t i = 0; i < m_Impl->voices.size(); ++i)
            {
                auto& v = m_Impl->voices[i];
                if (!v.alive)
                    continue;
                if (v.clip != clip)
                    continue;
                const AudioEmitterHandle h(i, v.generation);
                Stop(h);
            }
        };

        for (const GUID& g : destroys)
        {
            auto it = m_Impl->guidToClip.find(g);
            if (it == m_Impl->guidToClip.end())
                continue;

            const AudioClipHandle clip = it->second;
            const std::uint64_t idx = clip.Index();
            if (idx >= m_Impl->clips.size())
                continue;

            stopVoicesForClip(clip);

            auto& c = m_Impl->clips[idx];
            c.future.reset();
            c.pcmF32.clear();
            c.encodedBytes.clear();
            c.pcmFormat = ma_format_unknown;
            c.channels = 0;
            c.sampleRate = 0;
            c.frameCount = 0;
            c.state = Impl::ClipState::Failed;
            c.error = "Audio asset destroyed";
        }

        for (const GUID& g : reloads)
        {
            auto it = m_Impl->guidToClip.find(g);
            if (it == m_Impl->guidToClip.end())
                continue;

            const AudioClipHandle clip = it->second;
            const std::uint64_t idx = clip.Index();
            if (idx >= m_Impl->clips.size())
                continue;

            stopVoicesForClip(clip);

            auto& c = m_Impl->clips[idx];
            c.future.reset();

            SharedPtr<Asset> base = m_Assets ? m_Assets->GetAsset(g) : nullptr;
            auto* a = base ? dynamic_cast<AudioAsset*>(base.get()) : nullptr;
            if (!a || !a->IsLoaded())
            {
                c.pcmF32.clear();
                c.encodedBytes.clear();
                c.pcmFormat = ma_format_unknown;
                c.channels = 0;
                c.sampleRate = 0;
                c.frameCount = 0;
                c.state = Impl::ClipState::Failed;
                c.error = "Reloaded audio asset not available/loaded";
                continue;
            }

            (void)Impl::FillFromAudioAsset(*a, c);
        }
    }

    // 1) Promote any loaded clips.
    for (auto& c : m_Impl->clips)
    {
        if (!c.alive)
            continue;

        // Handle async loading clips
        if (c.state == Impl::ClipState::Loading && c.future.has_value())
        {
            // Non-blocking poll.
            if (c.future->wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
                continue;

            SharedPtr<Asset> base = c.future->get();
            c.future.reset();

            SharedPtr<AudioAsset> a;
            if (base)
            {
                a = std::dynamic_pointer_cast<AudioAsset>(base);
                if (!a)
                {
                    // Fallback: try dynamic_cast on raw pointer
                    AudioAsset* rawPtr = dynamic_cast<AudioAsset*>(base.get());
                    if (rawPtr)
                    {
                        a = std::shared_ptr<AudioAsset>(base, rawPtr);
                    }
                    else if (base->GetType() == AssetType::Audio)
                    {
                        // Last resort: if asset type is Audio, use static_cast
                        a = std::static_pointer_cast<AudioAsset>(base);
                        Logger::Log::Debug("Audio: Used static_cast workaround for async load (GUID={})", c.guid.ToString());
                    }
                }
            }
            
            if (!a || !a->IsLoaded())
            {
                c.state = Impl::ClipState::Failed;
                c.error = "Asset load failed or asset is not AudioAsset";
                Logger::Log::Warning("Audio: Async load failed for GUID={}, a={}, loaded={}", 
                    c.guid.ToString(), (a != nullptr), (a ? a->IsLoaded() : false));
                continue;
            }

            if (!Impl::FillFromAudioAsset(*a, c))
            {
                Logger::Log::Warning("Audio: FillFromAudioAsset failed for async-loaded asset GUID={}, error='{}'", 
                    c.guid.ToString(), c.error);
            }
            continue;
        }

        // Retry failed clips if asset is now available and loaded
        if (c.state == Impl::ClipState::Failed && m_Assets)
        {
            SharedPtr<Asset> existing = m_Assets->GetAsset(c.guid);
            if (existing)
            {
                SharedPtr<AudioAsset> a = std::dynamic_pointer_cast<AudioAsset>(existing);
                if (!a)
                {
                    // Fallback: try dynamic_cast on raw pointer (workaround for RTTI issues)
                    AudioAsset* rawPtr = dynamic_cast<AudioAsset*>(existing.get());
                    if (rawPtr)
                    {
                        a = std::shared_ptr<AudioAsset>(existing, rawPtr);
                    }
                    else if (existing->GetType() == AssetType::Audio)
                    {
                        // Last resort: if asset type is Audio, use static_cast
                        a = std::static_pointer_cast<AudioAsset>(existing);
                        Logger::Log::Debug("Audio: Used static_cast workaround for retry (GUID={})", c.guid.ToString());
                    }
                }
                
                if (a && a->IsLoaded())
                {
                    Logger::Log::Debug("Audio: Retrying failed clip - asset is now loaded (GUID={})", c.guid.ToString());
                    c.error.clear();
                    if (Impl::FillFromAudioAsset(*a, c))
                    {
                        Logger::Log::Info("Audio: Successfully recovered failed clip (GUID={})", c.guid.ToString());
                    }
                    else
                    {
                        Logger::Log::Warning("Audio: Retry FillFromAudioAsset still failed for GUID={}, error='{}'", 
                            c.guid.ToString(), c.error);
                    }
                }
            }
        }
    }

    // 2) Start any pending voices whose clips are ready.
    for (std::uint64_t i = 0; i < m_Impl->voices.size(); ++i)
    {
        auto& v = m_Impl->voices[i];
        if (!v.alive || v.state != Impl::VoiceState::Pending)
            continue;

        const AudioEmitterHandle vh(i, v.generation);

        if (!v.clip.IsValid())
        {
            Stop(vh);
            continue;
        }

        const std::uint64_t clipIdx = v.clip.Index();
        if (clipIdx >= m_Impl->clips.size())
        {
            Stop(vh);
            continue;
        }

        auto& c = m_Impl->clips[clipIdx];
        if (!c.alive || c.generation != v.clip.Generation())
        {
            Stop(vh);
            continue;
        }

        if (c.state == Impl::ClipState::Failed)
        {
            Logger::Log::Warning("Audio: Cannot play clip - state is Failed, error='{}'", c.error.empty() ? "unknown" : c.error);
            Stop(vh);
            continue;
        }

        if (c.state != Impl::ClipState::ReadyPcm && c.state != Impl::ClipState::ReadyEncoded)
        {
            // Clip is still loading or in an invalid state
            if (c.state == Impl::ClipState::Loading)
            {
                Logger::Log::Debug("Audio: Clip still loading, waiting for asset load to complete (GUID={})", c.guid.ToString());
            }
            else
            {
                Logger::Log::Debug("Audio: Clip not ready for playback (state={})", (int)c.state);
            }
            continue;
        }

        if (c.state == Impl::ClipState::ReadyPcm)
        {
            // Initialize per-voice data source (buffer ref with its own cursor).
            const ma_result br =
                ma_audio_buffer_ref_init(c.pcmFormat, c.channels, c.pcmF32.data(), c.frameCount, &v.bufferRef);
            if (br != MA_SUCCESS)
            {
                Logger::Log::Warning("Audio: ma_audio_buffer_ref_init failed (ma_result={})", (int)br);
                Stop(vh);
                continue;
            }
            v.bufferRef.sampleRate = c.sampleRate;
            v.bufferRefInit = true;
            v.sourceType = Impl::SourceType::PcmBufferRef;
        }
        else
        {
            // Initialize per-voice decoder from in-memory encoded data.
            ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 0, 0);
            const ma_result dr = ma_decoder_init_memory(c.encodedBytes.data(), c.encodedBytes.size(), &cfg, &v.decoder);
            if (dr != MA_SUCCESS)
            {
                Logger::Log::Warning("Audio: ma_decoder_init_memory failed (ma_result={})", (int)dr);
                Stop(vh);
                continue;
            }
            v.decoderInit = true;
            v.sourceType = Impl::SourceType::Decoder;
        }

        ma_sound_group* group = m_Backend->GetBusGroup(v.opts.bus);
        ma_uint32 flags = 0;
        if (!v.opts.spatialized && !v.is3D)
            flags |= MA_SOUND_FLAG_NO_SPATIALIZATION;
        if (v.opts.loop)
            flags |= MA_SOUND_FLAG_LOOPING;

        ma_engine* eng = m_Backend->GetEngine();
        ma_data_source* ds = (v.sourceType == Impl::SourceType::PcmBufferRef)
                                 ? reinterpret_cast<ma_data_source*>(&v.bufferRef)
                                 : reinterpret_cast<ma_data_source*>(&v.decoder);

        const ma_result sr = ma_sound_init_from_data_source(eng, ds, flags, group, &v.sound);
        if (sr != MA_SUCCESS)
        {
            Logger::Log::Warning("Audio: ma_sound_init_from_data_source failed (ma_result={} '{}')", (int)sr, ma_result_description(sr));
            Stop(vh);
            continue;
        }
        v.soundInit = true;
        MiniaudioBackend::ApplySoundHandedness(v.sound);

        ma_sound_set_volume(&v.sound, v.opts.volume);
        ma_sound_set_pitch(&v.sound, v.opts.pitch);

        if (v.is3D || v.opts.spatialized)
        {
            ma_sound_set_position(&v.sound, v.pos.x, v.pos.y, v.pos.z);
            ma_sound_set_velocity(&v.sound, v.vel.x, v.vel.y, v.vel.z);
            ma_sound_set_attenuation_model(&v.sound, ma_attenuation_model_inverse);
        }

        // Verify device is started before attempting to play sound
        ma_device* dev = ma_engine_get_device(eng);
        if (dev != nullptr)
        {
            ma_device_state deviceState = ma_device_get_state(dev);
            if (deviceState != ma_device_state_started)
            {
                Logger::Log::Warning("Audio: Device not started (state={}) when attempting to play sound. Attempting to start...", (int)deviceState);
                const ma_result startResult = ma_device_start(dev);
                if (startResult != MA_SUCCESS)
                {
                    Logger::Log::Error("Audio: Failed to start device for playback (ma_result={} '{}')", (int)startResult, ma_result_description(startResult));
                }
                else
                {
                    Logger::Log::Info("Audio: Device started successfully for playback");
                }
            }
        }

        const ma_result startResult = ma_sound_start(&v.sound);
        if (startResult != MA_SUCCESS)
        {
            Logger::Log::Warning("Audio: ma_sound_start failed (ma_result={} '{}')", (int)startResult, ma_result_description(startResult));
        }
        else
        {
            Logger::Log::Debug("Audio: Sound started successfully (clip={}, volume={:.2f}, loop={})", (uint64)v.clip.Index(), v.opts.volume, v.opts.loop);
            m_Backend->PeakBusMeter(v.opts.bus, v.opts.volume);
        }
        v.state = Impl::VoiceState::Playing;
        v.startSeq = m_Impl->nextVoiceStartSeq++;
    }

    // 3) Reap finished voices (non-looping).
    for (std::uint64_t i = 0; i < m_Impl->voices.size(); ++i)
    {
        auto& v = m_Impl->voices[i];
        if (!v.alive || v.state != Impl::VoiceState::Playing || !v.soundInit)
            continue;

        if (!v.opts.loop && ma_sound_at_end(&v.sound))
        {
            ma_sound_stop(&v.sound);
            ma_sound_uninit(&v.sound);
            v.soundInit = false;

            if (v.decoderInit)
            {
                ma_decoder_uninit(&v.decoder);
                v.decoderInit = false;
            }
            if (v.bufferRefInit)
            {
                ma_audio_buffer_ref_uninit(&v.bufferRef);
                v.bufferRefInit = false;
            }

            v.alive = false;
            v.state = Impl::VoiceState::Empty;
            v.startSeq = 0;
            v.sourceType = Impl::SourceType::None;
            v.clip = INVALID_AUDIO_CLIP_HANDLE;
            v.generation = NextGeneration(v.generation);
            m_Impl->freeVoices.push_back(i);
        }
    }

    // Per-bus VU: bus 0 from device callback; buses 1-5 from per-bus PCM meter nodes in the backend (no override here).
}

AudioClipHandle AudioSystem::ResolveClip(const GUID& guid)
{
    if (!m_Impl)
        return INVALID_AUDIO_CLIP_HANDLE;

    // Fast path: already known.
    auto it = m_Impl->guidToClip.find(guid);
    if (it != m_Impl->guidToClip.end())
    {
        return it->second;
    }

    // Allocate slot.
    std::uint64_t idx = 0;
    if (!m_Impl->freeClips.empty())
    {
        idx = m_Impl->freeClips.back();
        m_Impl->freeClips.pop_back();
    }
    else
    {
        idx = static_cast<std::uint64_t>(m_Impl->clips.size());
        m_Impl->clips.emplace_back();
    }

    auto& slot = m_Impl->clips[idx];
    slot.alive = true;
    slot.guid = guid;
    slot.state = Impl::ClipState::Loading;
    slot.error.clear();
    slot.future.reset();
    slot.pcmFormat = ma_format_unknown;
    slot.channels = 0;
    slot.sampleRate = 0;
    slot.frameCount = 0;
    slot.pcmF32.clear();
    slot.encodedBytes.clear();

    const AudioClipHandle handle(idx, slot.generation);
    m_Impl->guidToClip[guid] = handle;

    // If already loaded, populate immediately; otherwise kick async load.
    if (!m_Assets)
    {
        slot.state = Impl::ClipState::Failed;
        slot.error = "No AssetManager";
        return handle;
    }

    SharedPtr<Asset> existing = m_Assets->GetAsset(guid);
    if (existing)
    {
        Logger::Log::Debug("Audio: ResolveClip - Found existing asset for GUID={}, type={}, state={}", 
            guid.ToString(), 
            AssetTypeToString(existing->GetType()),
            AssetStateToString(existing->GetState()));
            
        // Try dynamic_pointer_cast first
        SharedPtr<AudioAsset> a = std::dynamic_pointer_cast<AudioAsset>(existing);
        if (!a)
        {
            // Fallback: try dynamic_cast on raw pointer and wrap if successful
            AudioAsset* rawPtr = dynamic_cast<AudioAsset*>(existing.get());
            if (rawPtr)
            {
                // If dynamic_cast works on raw pointer, the issue is with dynamic_pointer_cast
                // This can happen with some compiler/RTTI configurations
                // Create a new shared_ptr that shares ownership with the original
                a = std::shared_ptr<AudioAsset>(existing, rawPtr);
                Logger::Log::Debug("Audio: ResolveClip - Used raw pointer dynamic_cast workaround for GUID={}", guid.ToString());
            }
            else
            {
                // Last resort: if asset type is Audio, use static_cast
                // This is safe because we've verified the asset type is Audio
                // The dynamic_cast failure is likely due to RTTI issues across translation units
                if (existing->GetType() == AssetType::Audio)
                {
                    // Type matches, safe to use static_cast
                    a = std::static_pointer_cast<AudioAsset>(existing);
                    Logger::Log::Debug("Audio: ResolveClip - Used static_cast workaround for GUID={} (dynamic_cast failed but type is Audio)", guid.ToString());
                }
                else
                {
                    slot.state = Impl::ClipState::Failed;
                    slot.error = "Asset is not an AudioAsset";
                    Logger::Log::Warning("Audio: ResolveClip - Asset with GUID={} is not an AudioAsset (type={}, name={}, actual type={})", 
                        guid.ToString(), 
                        AssetTypeToString(existing->GetType()),
                        existing->GetName(),
                        typeid(*existing.get()).name());
                    return handle;
                }
            }
        }
        
        if (a->IsLoaded())
        {
            Logger::Log::Debug("Audio: ResolveClip - Asset is loaded, filling clip data (GUID={}, format={}, sampleRate={}, channels={})", 
                guid.ToString(),
                (int)a->GetFormat(),
                a->GetSampleRate(),
                (int)a->GetChannels());
                
            if (!Impl::FillFromAudioAsset(*a, slot))
            {
                Logger::Log::Warning("Audio: ResolveClip - FillFromAudioAsset failed for GUID={}, error='{}'", 
                    guid.ToString(), slot.error);
            }
            else
            {
                Logger::Log::Debug("Audio: ResolveClip - Successfully populated clip from loaded asset (GUID={}, state={})", 
                    guid.ToString(), (int)slot.state);
            }
            return handle;
        }
        else
        {
            // Asset exists but isn't loaded yet - kick async load and wait for it
            Logger::Log::Debug("Audio: ResolveClip - Asset exists but not loaded yet (state={}), requesting async load (GUID={})", 
                AssetStateToString(existing->GetState()), guid.ToString());
            slot.future.emplace(m_Assets->LoadAssetAsync(guid, AssetLoadPriority::High));
            return handle;
        }
    }

    // Not loaded yet. Request async load.
    slot.future.emplace(m_Assets->LoadAssetAsync(guid, AssetLoadPriority::Normal));
    return handle;
}

AudioClipHandle AudioSystem::ResolveClip(const GameEngine::AudioAsset& asset)
{
    const AudioClipHandle h = ResolveClip(asset.GetGUID());
    if (!m_Impl || !h.IsValid())
        return h;

    const std::uint64_t idx = h.Index();
    if (idx >= m_Impl->clips.size())
        return h;

    auto& slot = m_Impl->clips[idx];
    if (!slot.alive || slot.generation != h.Generation())
        return h;

    if (asset.IsLoaded())
    {
        slot.future.reset();
        (void)Impl::FillFromAudioAsset(asset, slot);
    }

    return h;
}

AudioEmitterHandle AudioSystem::Play2D(const GUID& guid, const PlayOptions& opts)
{
    if (!m_Initialized || !m_Backend || !m_Impl)
    {
        Logger::Log::Warning("Audio: Play2D failed - AudioSystem not initialized (initialized={}, backend={}, impl={})", 
            m_Initialized, (m_Backend != nullptr), (m_Impl != nullptr));
        return INVALID_AUDIO_EMITTER_HANDLE;
    }
    
    Logger::Log::Debug("Audio: Play2D requested for GUID={}", guid.ToString());
    AudioClipHandle clip = ResolveClip(guid);
    if (!clip.IsValid())
    {
        Logger::Log::Warning("Audio: Play2D failed - ResolveClip returned invalid handle for GUID={}", guid.ToString());
        return INVALID_AUDIO_EMITTER_HANDLE;
    }
    
    return Play2D(clip, opts);
}

AudioEmitterHandle AudioSystem::Play2D(const GameEngine::AudioAsset& asset, const PlayOptions& opts)
{
    return Play2D(asset.GetGUID(), opts);
}

AudioEmitterHandle AudioSystem::Play2D(AudioClipHandle clip, const PlayOptions& opts)
{
    if (!m_Initialized || !m_Backend || !m_Impl)
    {
        Logger::Log::Warning("Audio: Play2D failed - AudioSystem not initialized");
        return INVALID_AUDIO_EMITTER_HANDLE;
    }
    if (!clip.IsValid())
    {
        Logger::Log::Warning("Audio: Play2D failed - invalid clip handle");
        return INVALID_AUDIO_EMITTER_HANDLE;
    }
    
    // Check clip state
    const std::uint64_t clipIdx = clip.Index();
    if (clipIdx < m_Impl->clips.size())
    {
        const auto& c = m_Impl->clips[clipIdx];
        if (c.alive && c.generation == clip.Generation())
        {
            Logger::Log::Debug("Audio: Play2D - clip state={}, error='{}'", 
                (int)c.state, c.error.empty() ? "none" : c.error);
        }
    }

    // Allocate voice slot.
    std::uint64_t idx = 0;
    if (!m_Impl->freeVoices.empty())
    {
        idx = m_Impl->freeVoices.back();
        m_Impl->freeVoices.pop_back();
    }
    else
    {
        if (m_Impl->voices.size() >= m_Config.maxVoices)
        {
            // Minimal virtualization: steal the oldest voice.
            auto pickVictim = [&](bool allowLooping) -> std::optional<std::uint64_t>
            {
                std::uint64_t bestIdx = std::numeric_limits<std::uint64_t>::max();
                std::uint64_t bestSeq = std::numeric_limits<std::uint64_t>::max();

                for (std::uint64_t i = 0; i < m_Impl->voices.size(); ++i)
                {
                    const auto& v = m_Impl->voices[i];
                    if (!v.alive)
                        continue;
                    if (!allowLooping && v.opts.loop)
                        continue;

                    // startSeq==0 means "pending/not-started" -> prefer to steal first.
                    const std::uint64_t seq = v.startSeq;
                    if (seq < bestSeq)
                    {
                        bestSeq = seq;
                        bestIdx = i;
                    }
                }

                if (bestIdx == std::numeric_limits<std::uint64_t>::max())
                    return std::nullopt;
                return bestIdx;
            };

            auto victimIdx = pickVictim(false);
            if (!victimIdx.has_value())
            {
                victimIdx = pickVictim(true);
            }
            if (!victimIdx.has_value())
            {
                return INVALID_AUDIO_EMITTER_HANDLE;
            }

            const auto& victim = m_Impl->voices[*victimIdx];
            const AudioEmitterHandle victimHandle(*victimIdx, victim.generation);
            Stop(victimHandle);

            if (m_Impl->freeVoices.empty())
            {
                return INVALID_AUDIO_EMITTER_HANDLE;
            }
            idx = m_Impl->freeVoices.back();
            m_Impl->freeVoices.pop_back();
        }
        else
        {
            idx = static_cast<std::uint64_t>(m_Impl->voices.size());
            m_Impl->voices.emplace_back();
        }
    }

    auto& v = m_Impl->voices[idx];
    v.alive = true;
    v.state = Impl::VoiceState::Pending;
    v.startSeq = 0;
    v.clip = clip;
    v.opts = opts;
    v.is3D = false;
    v.world = 0;
    v.pos = Mathematics::Vector3{};
    v.vel = Mathematics::Vector3{};
    v.sourceType = Impl::SourceType::None;
    v.soundInit = false;
    v.bufferRefInit = false;
    v.decoderInit = false;

    const AudioEmitterHandle handle(idx, v.generation);

    // Start immediately if possible to avoid 1-frame latency for callers like the Inspector.
    Update(0.0f);

    return handle;
}

AudioEmitterHandle AudioSystem::Play3D(const GameEngine::AudioAsset& asset,
                                       AudioWorldId world,
                                       const Mathematics::Vector3& pos,
                                       const Mathematics::Vector3& vel,
                                       const PlayOptions& opts)
{
    return Play3D(asset.GetGUID(), world, pos, vel, opts);
}

AudioEmitterHandle AudioSystem::Play3D(const GUID& guid,
                                       AudioWorldId world,
                                       const Mathematics::Vector3& pos,
                                       const Mathematics::Vector3& vel,
                                       const PlayOptions& opts)
{
    if (!m_Initialized || !m_Backend || !m_Impl)
        return INVALID_AUDIO_EMITTER_HANDLE;
    return Play3D(ResolveClip(guid), world, pos, vel, opts);
}

AudioEmitterHandle AudioSystem::Play3D(AudioClipHandle clip,
                                       AudioWorldId world,
                                       const Mathematics::Vector3& pos,
                                       const Mathematics::Vector3& vel,
                                       const PlayOptions& opts)
{
    if (!m_Initialized || !m_Backend || !m_Impl)
        return INVALID_AUDIO_EMITTER_HANDLE;
    if (!clip.IsValid())
        return INVALID_AUDIO_EMITTER_HANDLE;

    std::uint64_t idx = 0;
    if (!m_Impl->freeVoices.empty())
    {
        idx = m_Impl->freeVoices.back();
        m_Impl->freeVoices.pop_back();
    }
    else
    {
        if (m_Impl->voices.size() >= m_Config.maxVoices)
        {
            // Minimal virtualization: steal the oldest voice.
            auto pickVictim = [&](bool allowLooping) -> std::optional<std::uint64_t>
            {
                std::uint64_t bestIdx = std::numeric_limits<std::uint64_t>::max();
                std::uint64_t bestSeq = std::numeric_limits<std::uint64_t>::max();

                for (std::uint64_t i = 0; i < m_Impl->voices.size(); ++i)
                {
                    const auto& v = m_Impl->voices[i];
                    if (!v.alive)
                        continue;
                    if (!allowLooping && v.opts.loop)
                        continue;

                    // startSeq==0 means "pending/not-started" -> prefer to steal first.
                    const std::uint64_t seq = v.startSeq;
                    if (seq < bestSeq)
                    {
                        bestSeq = seq;
                        bestIdx = i;
                    }
                }

                if (bestIdx == std::numeric_limits<std::uint64_t>::max())
                    return std::nullopt;
                return bestIdx;
            };

            auto victimIdx = pickVictim(false);
            if (!victimIdx.has_value())
            {
                victimIdx = pickVictim(true);
            }
            if (!victimIdx.has_value())
            {
                return INVALID_AUDIO_EMITTER_HANDLE;
            }

            const auto& victim = m_Impl->voices[*victimIdx];
            const AudioEmitterHandle victimHandle(*victimIdx, victim.generation);
            Stop(victimHandle);

            if (m_Impl->freeVoices.empty())
            {
                return INVALID_AUDIO_EMITTER_HANDLE;
            }
            idx = m_Impl->freeVoices.back();
            m_Impl->freeVoices.pop_back();
        }
        else
        {
            idx = static_cast<std::uint64_t>(m_Impl->voices.size());
            m_Impl->voices.emplace_back();
        }
    }

    auto& v = m_Impl->voices[idx];
    v.alive = true;
    v.state = Impl::VoiceState::Pending;
    v.startSeq = 0;
    v.clip = clip;
    v.opts = opts;
    v.is3D = true;
    v.world = world;
    v.pos = pos;
    v.vel = vel;
    v.sourceType = Impl::SourceType::None;
    v.soundInit = false;
    v.bufferRefInit = false;
    v.decoderInit = false;

    const AudioEmitterHandle handle(idx, v.generation);

    Update(0.0f);
    return handle;
}

void AudioSystem::SetListener(AudioWorldId world, std::uint32_t listenerIndex, const ListenerState& state)
{
    if (!m_Initialized || !m_Backend)
        return;
    m_Backend->SetListener(world, listenerIndex, state);
}

void AudioSystem::SetEmitter3D(AudioEmitterHandle emitter,
                               AudioWorldId world,
                               const Mathematics::Vector3& pos,
                               const Mathematics::Vector3& vel)
{
    if (!m_Initialized || !m_Impl)
        return;
    if (!emitter.IsValid())
        return;

    const std::uint64_t idx = emitter.Index();
    if (idx >= m_Impl->voices.size())
        return;

    auto& v = m_Impl->voices[idx];
    if (!v.alive || v.generation != emitter.Generation())
        return;

    v.is3D = true;
    v.world = world;
    v.pos = pos;
    v.vel = vel;

    if (v.soundInit)
    {
        ma_sound_set_position(&v.sound, v.pos.x, v.pos.y, v.pos.z);
        ma_sound_set_velocity(&v.sound, v.vel.x, v.vel.y, v.vel.z);
    }
}

void AudioSystem::Stop(AudioEmitterHandle emitter)
{
    if (!m_Impl)
        return;
    if (!emitter.IsValid())
        return;

    const std::uint64_t idx = emitter.Index();
    if (idx >= m_Impl->voices.size())
        return;

    auto& v = m_Impl->voices[idx];
    if (!v.alive || v.generation != emitter.Generation())
        return;

    if (v.soundInit)
    {
        ma_sound_stop(&v.sound);
        ma_sound_uninit(&v.sound);
        v.soundInit = false;
    }
    if (v.decoderInit)
    {
        ma_decoder_uninit(&v.decoder);
        v.decoderInit = false;
    }
    if (v.bufferRefInit)
    {
        ma_audio_buffer_ref_uninit(&v.bufferRef);
        v.bufferRefInit = false;
    }

    v.alive = false;
    v.state = Impl::VoiceState::Empty;
    v.startSeq = 0;
    v.sourceType = Impl::SourceType::None;
    v.clip = INVALID_AUDIO_CLIP_HANDLE;

    v.generation = NextGeneration(v.generation);
    m_Impl->freeVoices.push_back(idx);
}

void AudioSystem::StopAllVoices()
{
    if (!m_Impl)
        return;

    // Iterate directly to avoid free-list churn; Stop() will update generation/free list.
    for (std::uint64_t i = 0; i < m_Impl->voices.size(); ++i)
    {
        auto& v = m_Impl->voices[i];
        if (!v.alive)
            continue;
        AudioEmitterHandle h(i, v.generation);
        Stop(h);
    }
}

bool AudioSystem::IsAlive(AudioEmitterHandle emitter) const
{
    if (!m_Impl)
        return false;
    if (!emitter.IsValid())
        return false;

    const std::uint64_t idx = emitter.Index();
    if (idx >= m_Impl->voices.size())
        return false;

    const auto& v = m_Impl->voices[idx];
    return v.alive && v.generation == emitter.Generation();
}

bool AudioSystem::IsPlaying(AudioEmitterHandle emitter) const
{
    if (!IsAlive(emitter))
        return false;

    const std::uint64_t idx = emitter.Index();
    const auto& v = m_Impl->voices[idx];
    if (!v.soundInit)
        return false;

    return ma_sound_is_playing(&v.sound) != MA_FALSE;
}

std::size_t AudioSystem::GetAliveVoiceCount() const
{
    if (!m_Impl)
        return 0;

    std::size_t count = 0;
    for (const auto& v : m_Impl->voices)
    {
        if (v.alive)
            ++count;
    }
    return count;
}

void AudioSystem::SetBusVolume(AudioBusId bus, float linearVolume)
{
    if (!m_Backend || !m_Initialized)
        return;
    m_Backend->SetBusVolume(bus, linearVolume);
}

float AudioSystem::GetBusVolume(AudioBusId bus) const
{
    if (!m_Backend || !m_Initialized)
        return 1.0f;
    return m_Backend->GetBusVolume(bus);
}

float AudioSystem::GetBusMeterLevel(AudioBusId bus) const
{
    if (!m_Backend || !m_Initialized)
        return 0.0f;
    return m_Backend->GetBusMeterLevel(bus);
}

float AudioSystem::GetOutputLatencyMs() const
{
    if (!m_Backend || !m_Initialized)
        return 0.0f;
    return m_Backend->GetOutputLatencyMs();
}

void AudioSystem::SetOutputCaptureCallback(OutputCaptureCallback callback)
{
    if (!m_Backend || !m_Initialized)
        return;
    m_Backend->SetOutputCaptureCallback(std::move(callback));
}

} // namespace GameEngine::Audio

