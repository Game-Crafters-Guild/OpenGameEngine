#include "MiniaudioBackend.h"

#include "Audio/SpatializerHandedness.h"
#include "Logger/Logger.h"

#include <cmath>
#include <cstring>
#include <utility>

namespace GameEngine::Audio
{

const ma_node_vtable AudioSystem::MiniaudioBackend::kBusMeterNodeVtable = {
    &MiniaudioBackend::BusMeterNodeProcess,
    nullptr,
    1,  // inputBusCount
    1,  // outputBusCount
    0   // flags
};

void AudioSystem::MiniaudioBackend::BusMeterNodeProcess(ma_node* pNode, const float** ppFramesIn, ma_uint32* pFrameCountIn, float** ppFramesOut, ma_uint32* pFrameCountOut)
{
    auto* node = reinterpret_cast<BusMeterNode*>(pNode);
    if (!node || !node->pPeak || !ppFramesIn || !ppFramesOut || !pFrameCountIn || !pFrameCountOut)
        return;
    const ma_uint32 frameCountIn = *pFrameCountIn;
    const ma_uint32 frameCountOutCap = *pFrameCountOut;
    if (frameCountIn == 0 || frameCountOutCap == 0)
        return;
    const float* in = ppFramesIn[0];
    float* out = ppFramesOut[0];
    if (!in || !out)
        return;

    ma_uint32 channels = ma_node_get_input_channels(pNode, 0);
    if (channels == 0)
        channels = 1;
    const ma_uint32 totalSamples = frameCountIn * channels;
    float peak = 0.0f;
    for (ma_uint32 i = 0; i < totalSamples; ++i)
    {
        const float a = std::fabs(in[i]);
        if (a > peak)
            peak = a;
    }
    node->pPeak->store(peak, std::memory_order_relaxed);

    const ma_uint32 framesToCopy = (frameCountIn < frameCountOutCap) ? frameCountIn : frameCountOutCap;
    std::memcpy(out, in, framesToCopy * channels * sizeof(float));
    *pFrameCountIn = framesToCopy;
    *pFrameCountOut = framesToCopy;
}

void AudioSystem::MiniaudioBackend::DataCallback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount)
{
    (void)pInput;
    auto* backend = static_cast<MiniaudioBackend*>(pDevice->pUserData);
    if (!backend)
        return;
    ma_engine_read_pcm_frames(backend->GetEngine(), pOutput, frameCount, nullptr);
    backend->OnDeviceDataCallback(pDevice, pOutput, frameCount);
}

void AudioSystem::MiniaudioBackend::NotificationCallback(const ma_device_notification* pNotification)
{
    if (!pNotification)
        return;
    switch (pNotification->type)
    {
    case ma_device_notification_type_started:
        Logger::Log::Info("Audio: device started");
        break;
    case ma_device_notification_type_stopped:
        Logger::Log::Info("Audio: device stopped");
        break;
    case ma_device_notification_type_rerouted:
        Logger::Log::Info("Audio: device rerouted");
        break;
    case ma_device_notification_type_interruption_began:
        Logger::Log::Info("Audio: device interruption began");
        break;
    case ma_device_notification_type_interruption_ended:
        Logger::Log::Info("Audio: device interruption ended");
        break;
    case ma_device_notification_type_unlocked:
        // Browsers create the output context suspended and only resume it on a
        // user gesture; miniaudio reports that resume here. Until it arrives the
        // device is "started" but renders nothing.
        Logger::Log::Info("Audio: device unlocked by user gesture; output is live");
        break;
    default:
        break;
    }
}

bool AudioSystem::MiniaudioBackend::Initialize(const AudioSystemConfig& config)
{
    if (m_EngineInitialized)
    {
        return true;
    }

    auto clampListeners = [](std::uint32_t requested) -> ma_uint32
    {
        ma_uint32 v = (requested == 0) ? 1u : static_cast<ma_uint32>(requested);
        if (v < 1u)
            v = 1u;
        if (v > static_cast<ma_uint32>(MA_ENGINE_MAX_LISTENERS))
            v = static_cast<ma_uint32>(MA_ENGINE_MAX_LISTENERS);
        return v;
    };

    {
        ma_resource_manager_config rmCfg = ma_resource_manager_config_init();
        rmCfg.decodedFormat = ma_format_f32;
        rmCfg.decodedSampleRate = static_cast<ma_uint32>(config.mixSampleRate);
        rmCfg.jobThreadCount = 0;
        rmCfg.flags = MA_RESOURCE_MANAGER_FLAG_NO_THREADING;
        const ma_result rmResult = ma_resource_manager_init(&rmCfg, &m_ResourceManager);
        if (rmResult != MA_SUCCESS)
        {
            Logger::Log::Warning("Audio: ma_resource_manager_init failed (ma_result={} '{}')", (int)rmResult, ma_result_description(rmResult));
            return false;
        }
        m_ResourceManagerInit = true;
    }

    // Try custom device first so we can read PCM in the callback for real-time master VU metering.
    ma_result ctxResult = ma_context_init(nullptr, 0, nullptr, &m_Context);
    if (ctxResult == MA_SUCCESS)
    {
        ma_device_config deviceCfg = ma_device_config_init(ma_device_type_playback);
        deviceCfg.playback.format = ma_format_f32;
        deviceCfg.playback.channels = 0;
        deviceCfg.sampleRate = static_cast<ma_uint32>(config.mixSampleRate);
        deviceCfg.periodSizeInFrames = static_cast<ma_uint32>(config.mixBufferSizeFrames);
        deviceCfg.dataCallback = &MiniaudioBackend::DataCallback;
        deviceCfg.notificationCallback = &MiniaudioBackend::NotificationCallback;
        deviceCfg.pUserData = this;

        ma_result devResult = ma_device_init(&m_Context, &deviceCfg, &m_Device);
        if (devResult == MA_SUCCESS)
        {
            ma_engine_config engineCfg = ma_engine_config_init();
            engineCfg.pDevice = &m_Device;
            engineCfg.pResourceManager = &m_ResourceManager;
            engineCfg.noAutoStart = MA_TRUE;
            engineCfg.listenerCount = clampListeners(config.maxListeners);

            ma_result engResult = ma_engine_init(&engineCfg, &m_Engine);
            if (engResult == MA_SUCCESS)
            {
                ma_result startResult = ma_engine_start(&m_Engine);
                if (startResult == MA_SUCCESS)
                {
                    m_EngineInitialized = true;
                    m_DeviceOwned = true;
                }
                else
                {
                    Logger::Log::Warning("Audio: ma_engine_start failed (ma_result={})", (int)startResult);
                    ma_engine_uninit(&m_Engine);
                    ma_device_uninit(&m_Device);
                    ma_context_uninit(&m_Context);
                }
            }
            else
            {
                Logger::Log::Warning("Audio: ma_engine_init (custom device) failed (ma_result={})", (int)engResult);
                ma_device_uninit(&m_Device);
                ma_context_uninit(&m_Context);
            }
        }
        else
        {
            Logger::Log::Warning("Audio: ma_device_init failed (backend={}, ma_result={} '{}')",
                                 ma_get_backend_name(m_Context.backend), (int)devResult, ma_result_description(devResult));
            ma_context_uninit(&m_Context);
        }
    }
    else
    {
        Logger::Log::Warning("Audio: ma_context_init failed (ma_result={} '{}')", (int)ctxResult, ma_result_description(ctxResult));
    }

    // Fallback: engine-owned device (no real-time metering; VU will peak on sound start and decay).
    if (!m_EngineInitialized)
    {
        auto tryEngineInit = [&](const ma_engine_config& engineCfg, const char* attemptName) -> ma_result
        {
            const ma_result r = ma_engine_init(&engineCfg, &m_Engine);
            if (r != MA_SUCCESS)
            {
                Logger::Log::Warning(
                    "Audio: ma_engine_init failed (attempt={}, ma_result={} '{}', listeners={}, channels={}, sampleRate={}, periodFrames={})",
                    (attemptName ? attemptName : "<null>"),
                    (int)r,
                    ma_result_description(r),
                    (uint32)engineCfg.listenerCount,
                    (uint32)engineCfg.channels,
                    (uint32)engineCfg.sampleRate,
                    (uint32)engineCfg.periodSizeInFrames);
            }
            return r;
        };

        ma_engine_config engineCfg = ma_engine_config_init();
        engineCfg.pResourceManager = &m_ResourceManager;
        engineCfg.notificationCallback = &MiniaudioBackend::NotificationCallback;
        engineCfg.listenerCount = clampListeners(config.maxListeners);
        engineCfg.channels = 0;
        engineCfg.sampleRate = static_cast<ma_uint32>(config.mixSampleRate);
        engineCfg.periodSizeInFrames = static_cast<ma_uint32>(config.mixBufferSizeFrames);

        ma_result r = tryEngineInit(engineCfg, "requested");
        if (r != MA_SUCCESS)
        {
            ma_engine_config fallback = engineCfg;
            fallback.sampleRate = 0;
            r = tryEngineInit(fallback, "fallback_nativeSampleRate");
            if (r != MA_SUCCESS)
            {
                ma_engine_config fallback2 = fallback;
                fallback2.periodSizeInFrames = 0;
                fallback2.periodSizeInMilliseconds = 0;
                r = tryEngineInit(fallback2, "fallback_nativeSampleRate_defaultPeriod");
            }
        }

        if (r == MA_SUCCESS)
        {
            m_EngineInitialized = true;
            m_DeviceOwned = false;
            ma_device* dev = ma_engine_get_device(&m_Engine);
            if (dev != nullptr && ma_device_get_state(dev) != ma_device_state_started)
            {
                if (ma_device_start(dev) != MA_SUCCESS)
                    Logger::Log::Warning("Audio: ma_device_start failed. Audio may not work on macOS.");
                else
                    Logger::Log::Info("Audio: Device explicitly started.");
            }
        }
    }

    if (!m_EngineInitialized)
    {
        ma_resource_manager_uninit(&m_ResourceManager);
        m_ResourceManagerInit = false;
        return false;
    }

    ApplyListenerHandedness();
    Logger::Log::Info(
        "Audio: miniaudio listeners are left-handed (engine +Z; world +X pans right)");

    // MVP bus graph: master -> (music/sfx/ui/vo)
    {
        ma_result br = ma_sound_group_init(&m_Engine, 0, nullptr, &m_BusMaster);
        if (br != MA_SUCCESS)
        {
            Logger::Log::Warning("Audio: ma_sound_group_init(Master) failed (ma_result={})", (int)br);
        }
        else
        {
            m_BusMasterInit = true;
        }
    }

    auto initChildBus = [&](ma_sound_group& child, bool& childInit, const char* name)
    {
        ma_sound_group* parent = m_BusMasterInit ? &m_BusMaster : nullptr;
        const ma_result br = ma_sound_group_init(&m_Engine, 0, parent, &child);
        if (br != MA_SUCCESS)
        {
            Logger::Log::Warning("Audio: ma_sound_group_init({}) failed (ma_result={})", name, (int)br);
            childInit = false;
        }
        else
        {
            childInit = true;
        }
    };

    initChildBus(m_BusMusic, m_BusMusicInit, "Music");
    initChildBus(m_BusSfx, m_BusSfxInit, "SFX");
    initChildBus(m_BusUi, m_BusUiInit, "UI");
    initChildBus(m_BusVo, m_BusVoInit, "VO");
    initChildBus(m_BusAux, m_BusAuxInit, "Aux");

    // Per-bus PCM metering: insert passthrough meter nodes between each child bus and master.
    ma_node_graph* pGraph = ma_engine_get_node_graph(&m_Engine);
    const ma_uint32 engineChannels = ma_engine_get_channels(&m_Engine);
    ma_uint32 meterChannels = engineChannels;
    if (meterChannels == 0)
        meterChannels = 1;

    for (unsigned i = 0; i < kNumChildBuses; ++i)
    {
        m_BusMeterPeaks[i].store(0.0f, std::memory_order_relaxed);
        m_BusMeterNodes[i].pPeak = &m_BusMeterPeaks[i];

        ma_node_config nodeConfig = ma_node_config_init();
        nodeConfig.vtable = &kBusMeterNodeVtable;
        nodeConfig.pInputChannels = &meterChannels;
        nodeConfig.pOutputChannels = &meterChannels;

        ma_result nr = ma_node_init(pGraph, &nodeConfig, nullptr, reinterpret_cast<ma_node*>(&m_BusMeterNodes[i]));
        if (nr != MA_SUCCESS)
        {
            Logger::Log::Warning("Audio: ma_node_init(bus meter {}) failed (ma_result={})", i, (int)nr);
            while (i > 0)
            {
                --i;
                ma_node_uninit(reinterpret_cast<ma_node*>(&m_BusMeterNodes[i]), nullptr);
            }
            break;
        }
        m_BusMeterNodesInit = (i == kNumChildBuses - 1);
    }

    if (m_BusMeterNodesInit && m_BusMasterInit)
    {
        ma_sound_group* childBuses[] = { &m_BusMusic, &m_BusSfx, &m_BusUi, &m_BusVo, &m_BusAux };
        for (unsigned i = 0; i < kNumChildBuses; ++i)
        {
            ma_node* childNode = reinterpret_cast<ma_node*>(childBuses[i]);
            ma_node* meterNode = reinterpret_cast<ma_node*>(&m_BusMeterNodes[i]);
            ma_node* masterNode = reinterpret_cast<ma_node*>(&m_BusMaster);

            ma_result detachResult = ma_node_detach_output_bus(childNode, 0);
            if (detachResult != MA_SUCCESS)
                Logger::Log::Warning("Audio: ma_node_detach_output_bus(bus {}) failed (ma_result={})", i, (int)detachResult);

            ma_result ar = ma_node_attach_output_bus(childNode, 0, meterNode, 0);
            if (ar != MA_SUCCESS)
                Logger::Log::Warning("Audio: ma_node_attach_output_bus(child->meter {}) failed (ma_result={})", i, (int)ar);
            ma_result br = ma_node_attach_output_bus(meterNode, 0, masterNode, 0);
            if (br != MA_SUCCESS)
                Logger::Log::Warning("Audio: ma_node_attach_output_bus(meter->master {}) failed (ma_result={})", i, (int)br);
        }
    }

    // Diagnostics: log the actual device settings after init (engineCfg values may be adjusted internally).
    {
        ma_device* dev = ma_engine_get_device(&m_Engine);
        ma_device_info info{};
        const ma_bool32 gotInfo = (dev && ma_device_get_info(dev, ma_device_type_playback, &info) == MA_SUCCESS) ? MA_TRUE : MA_FALSE;

        const char* deviceName = (gotInfo == MA_TRUE && info.name[0] != '\0') ? info.name : (dev ? dev->playback.name : "<no-device>");
        ma_device_state deviceState = dev ? ma_device_get_state(dev) : ma_device_state_uninitialized;

        const char* deviceStateStr = "unknown";
        switch (deviceState)
        {
            case ma_device_state_uninitialized: deviceStateStr = "uninitialized"; break;
            case ma_device_state_stopped: deviceStateStr = "stopped"; break;
            case ma_device_state_started: deviceStateStr = "started"; break;
            case ma_device_state_starting: deviceStateStr = "starting"; break;
            case ma_device_state_stopping: deviceStateStr = "stopping"; break;
            default: deviceStateStr = "unknown"; break;
        }

        Logger::Log::Info(
            "Audio: Miniaudio backend initialized (backend={}, device='{}', deviceState={} ({}), engineSR={}Hz, playbackFmt={}, playbackCh={}, callbackSR={}Hz, internalFmt={}, internalCh={}, internalSR={}Hz, internalPeriodFrames={})",
            dev ? ma_get_backend_name(dev->pContext->backend) : "<no-device>",
            deviceName ? deviceName : "<unknown>",
            deviceStateStr,
            (int)deviceState,
            (uint32)ma_engine_get_sample_rate(&m_Engine),
            dev ? ma_get_format_name(dev->playback.format) : "<no-device>",
            dev ? (uint32)dev->playback.channels : 0u,
            dev ? (uint32)dev->sampleRate : 0u,
            dev ? ma_get_format_name(dev->playback.internalFormat) : "<no-device>",
            dev ? (uint32)dev->playback.internalChannels : 0u,
            dev ? (uint32)dev->playback.internalSampleRate : 0u,
            dev ? (uint32)dev->playback.internalPeriodSizeInFrames : 0u);
    }

    return m_EngineInitialized;
}

void AudioSystem::MiniaudioBackend::Shutdown()
{
    if (!m_EngineInitialized)
        return;

    if (m_BusMeterNodesInit)
    {
        for (unsigned i = 0; i < kNumChildBuses; ++i)
            ma_node_uninit(reinterpret_cast<ma_node*>(&m_BusMeterNodes[i]), nullptr);
        m_BusMeterNodesInit = false;
    }

    if (m_BusAuxInit) { ma_sound_group_uninit(&m_BusAux); m_BusAuxInit = false; }
    if (m_BusVoInit) { ma_sound_group_uninit(&m_BusVo); m_BusVoInit = false; }
    if (m_BusUiInit) { ma_sound_group_uninit(&m_BusUi); m_BusUiInit = false; }
    if (m_BusSfxInit) { ma_sound_group_uninit(&m_BusSfx); m_BusSfxInit = false; }
    if (m_BusMusicInit) { ma_sound_group_uninit(&m_BusMusic); m_BusMusicInit = false; }
    if (m_BusMasterInit) { ma_sound_group_uninit(&m_BusMaster); m_BusMasterInit = false; }

    ma_engine_uninit(&m_Engine);
    m_EngineInitialized = false;

    if (m_DeviceOwned)
    {
        m_DeviceOwned = false;
        ma_device_uninit(&m_Device);
        ma_context_uninit(&m_Context);
    }

    if (m_ResourceManagerInit)
    {
        ma_resource_manager_uninit(&m_ResourceManager);
        m_ResourceManagerInit = false;
    }
}

void AudioSystem::MiniaudioBackend::Update(float dt)
{
    const float peak = m_MasterMeterPeak.load(std::memory_order_relaxed);
    const float decay = 1.0f - 3.0f * dt;
    const float decayClamp = (decay > 0.0f ? decay : 0.0f);

    // Master bus (0): feed from real-time PCM peak; peak-hold with decay
    float& masterM = m_BusMeterLevels[0];
    masterM = (peak > masterM) ? peak : (masterM * decayClamp);
    if (masterM < 0.001f)
        masterM = 0.0f;

    // Buses 1–5: decay only; AudioSystem sets each channel’s level from master when that bus has playing voices
    if (m_BusMeterNodesInit)
    {
        for (unsigned i = 0; i < kNumChildBuses; ++i)
        {
            const float busPeak = m_BusMeterPeaks[i].load(std::memory_order_relaxed);
            float& m = m_BusMeterLevels[i + 1];
            m = (busPeak > m) ? busPeak : (m * decayClamp);
            if (m < 0.001f)
                m = 0.0f;
        }
    }
    else
    {
        for (unsigned i = 1; i < kMaxBuses; ++i)
        {
            float& m = m_BusMeterLevels[i];
            m *= decayClamp;
            if (m < 0.001f)
                m = 0.0f;
        }
    }
}

void AudioSystem::MiniaudioBackend::SetBusMeterLevel(AudioBusId bus, float level)
{
    const unsigned idx = (bus < static_cast<AudioBusId>(kMaxBuses)) ? static_cast<unsigned>(bus) : 0u;
    if (idx == 0u)
        return;
    float v = (level < 0.0f) ? 0.0f : (level > 1.0f ? 1.0f : level);
    m_BusMeterLevels[idx] = v;  // so this channel tracks master in real time while it has playing voices
}

void AudioSystem::MiniaudioBackend::OnDeviceDataCallback(ma_device* pDevice, void* pOutput, ma_uint32 frameCount)
{
    if (!pDevice || !pOutput || pDevice->playback.format != ma_format_f32)
        return;
    const ma_uint32 channels = pDevice->playback.channels;
    if (channels == 0)
        return;
    const float* samples = static_cast<const float*>(pOutput);
    const ma_uint32 totalSamples = frameCount * channels;
    float peak = 0.0f;
    for (ma_uint32 i = 0; i < totalSamples; ++i)
    {
        const float a = std::fabs(samples[i]);
        if (a > peak)
            peak = a;
    }
    m_MasterMeterPeak.store(peak, std::memory_order_relaxed);

    OutputCaptureCallback capture;
    {
        std::lock_guard<std::mutex> lock(m_OutputCaptureMutex);
        capture = m_OutputCaptureCallback;
    }
    if (capture)
        capture(samples, static_cast<std::uint32_t>(frameCount), static_cast<std::uint32_t>(channels), static_cast<std::uint32_t>(pDevice->sampleRate));
}

void AudioSystem::MiniaudioBackend::SetOutputCaptureCallback(OutputCaptureCallback callback)
{
    std::lock_guard<std::mutex> lock(m_OutputCaptureMutex);
    m_OutputCaptureCallback = std::move(callback);
}

void AudioSystem::MiniaudioBackend::ApplyListenerHandedness()
{
    if (!m_EngineInitialized)
        return;
    const ma_handedness hand =
        kMiniaudioListenerLeftHanded ? ma_handedness_left : ma_handedness_right;
    // Init already ran with the RH default direction (0,0,-1). LH init would
    // have swapped that to +Z; patch both fields now that handedness is set
    // after ma_engine_init. SetListener overwrites direction every frame.
    const float defaultForwardZ = kMiniaudioListenerLeftHanded ? 1.0f : -1.0f;
    for (ma_uint32 i = 0; i < m_Engine.listenerCount; ++i)
    {
        m_Engine.listeners[i].config.handedness = hand;
        ma_engine_listener_set_direction(&m_Engine, i, 0.0f, 0.0f, defaultForwardZ);
    }
}

void AudioSystem::MiniaudioBackend::ApplySoundHandedness(ma_sound& sound)
{
    sound.engineNode.spatializer.handedness =
        kMiniaudioListenerLeftHanded ? ma_handedness_left : ma_handedness_right;
    // Same post-init swap as the listener: emitter cone default is +Z in LH.
    // Omnidirectional sources ignore this; panning uses listener handedness.
    const float defaultForwardZ = kMiniaudioListenerLeftHanded ? 1.0f : -1.0f;
    ma_sound_set_direction(&sound, 0.0f, 0.0f, defaultForwardZ);
}

void AudioSystem::MiniaudioBackend::SetListener([[maybe_unused]] AudioWorldId world,
                                                [[maybe_unused]] std::uint32_t listenerIndex,
                                                [[maybe_unused]] const ListenerState& state)
{
    if (!m_EngineInitialized)
        return;

    // MVP: world is ignored for now; listenerIndex is direct.
    // Future: map (world, listenerIndex) into a global listener slot.
    const ma_uint32 li = (ma_uint32)listenerIndex;

    ma_engine_listener_set_position(&m_Engine, li, state.position[0], state.position[1], state.position[2]);
    ma_engine_listener_set_direction(&m_Engine, li, state.forward[0], state.forward[1], state.forward[2]);
    ma_engine_listener_set_world_up(&m_Engine, li, state.up[0], state.up[1], state.up[2]);
    ma_engine_listener_set_velocity(&m_Engine, li, state.velocity[0], state.velocity[1], state.velocity[2]);

    (void)world;
}

ma_sound_group* AudioSystem::MiniaudioBackend::GetBusGroup(AudioBusId bus)
{
    if (!m_EngineInitialized)
        return nullptr;

    switch (bus)
    {
    case 0:
        return m_BusMasterInit ? &m_BusMaster : nullptr;
    case 1:
        return m_BusMusicInit ? &m_BusMusic : (m_BusMasterInit ? &m_BusMaster : nullptr);
    case 2:
        return m_BusSfxInit ? &m_BusSfx : (m_BusMasterInit ? &m_BusMaster : nullptr);
    case 3:
        return m_BusUiInit ? &m_BusUi : (m_BusMasterInit ? &m_BusMaster : nullptr);
    case 4:
        return m_BusVoInit ? &m_BusVo : (m_BusMasterInit ? &m_BusMaster : nullptr);
    case 5:
        return m_BusAuxInit ? &m_BusAux : (m_BusMasterInit ? &m_BusMaster : nullptr);
    default:
        return m_BusMasterInit ? &m_BusMaster : nullptr;
    }
}

void AudioSystem::MiniaudioBackend::SetBusVolume(AudioBusId bus, float linearVolume)
{
    ma_sound_group* group = GetBusGroup(bus);
    if (group == nullptr)
        return;
    float v = linearVolume;
    if (v < 0.0f)
        v = 0.0f;
    if (v > 1.0f)
        v = 1.0f;
    ma_sound_group_set_volume(group, v);
    // Placeholder metering: peak the meter so VU bar responds; real metering would come from a tap node
    const unsigned idx = (bus < static_cast<AudioBusId>(kMaxBuses)) ? static_cast<unsigned>(bus) : 0u;
    if (v > m_BusMeterLevels[idx])
        m_BusMeterLevels[idx] = v;
}

float AudioSystem::MiniaudioBackend::GetBusVolume(AudioBusId bus) const
{
    ma_sound_group* group = const_cast<MiniaudioBackend*>(this)->GetBusGroup(bus);
    if (group == nullptr)
        return 1.0f;
    return ma_sound_group_get_volume(group);
}

float AudioSystem::MiniaudioBackend::GetBusMeterLevel(AudioBusId bus) const
{
    const unsigned idx = (bus < static_cast<AudioBusId>(kMaxBuses)) ? static_cast<unsigned>(bus) : 0u;
    return m_BusMeterLevels[idx];
}

void AudioSystem::MiniaudioBackend::PeakBusMeter(AudioBusId bus, float level)
{
    const unsigned idx = (bus < static_cast<AudioBusId>(kMaxBuses)) ? static_cast<unsigned>(bus) : 0u;
    float v = (level < 0.0f) ? 0.0f : (level > 1.0f ? 1.0f : level);
    if (v > m_BusMeterLevels[idx])
        m_BusMeterLevels[idx] = v;
}

float AudioSystem::MiniaudioBackend::GetOutputLatencyMs() const
{
    ma_device* dev = const_cast<ma_device*>(&m_Device);
    if (m_DeviceOwned)
    {
        const ma_uint32 sr = dev->sampleRate;
        const ma_uint32 period = dev->playback.internalPeriodSizeInFrames;
        if (sr > 0 && period > 0)
            return (static_cast<float>(period) / static_cast<float>(sr)) * 1000.0f;
    }
    if (m_EngineInitialized)
    {
        ma_device* engDev = ma_engine_get_device(const_cast<ma_engine*>(&m_Engine));
        if (engDev)
        {
            const ma_uint32 sr = engDev->sampleRate;
            const ma_uint32 period = engDev->playback.internalPeriodSizeInFrames;
            if (sr > 0 && period > 0)
                return (static_cast<float>(period) / static_cast<float>(sr)) * 1000.0f;
        }
    }
    return 0.0f;
}

} // namespace GameEngine::Audio
