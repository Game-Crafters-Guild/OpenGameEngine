#pragma once

#include "Audio/AudioSystem.h"

#include <miniaudio.h>
#include <atomic>
#include <cstddef>
#include <mutex>

namespace GameEngine::Audio
{

/** Passthrough node that writes per-bus PCM peak to an atomic (for VU metering). Must have ma_node_base as first member. */
struct BusMeterNode
{
    ma_node_base base;
    std::atomic<float>* pPeak;
};

// Private implementation. Defined here so AudioSystem.cpp can call methods without exposing miniaudio headers.
class AudioSystem::MiniaudioBackend
{
public:
    bool Initialize(const AudioSystemConfig& config);
    void Shutdown();
    void Update(float dt);

    void SetListener(AudioWorldId world, std::uint32_t listenerIndex, const ListenerState& state);

    // Engine world is LH +Z. miniaudio defaults to RH; flip the derived right
    // axis so world +X pans to the right ear. Call after engine/sound init.
    void ApplyListenerHandedness();
    static void ApplySoundHandedness(ma_sound& sound);

    ma_engine* GetEngine() { return &m_Engine; }
    ma_sound_group* GetBusGroup(AudioBusId bus);

    void SetBusVolume(AudioBusId bus, float linearVolume);
    float GetBusVolume(AudioBusId bus) const;
    float GetBusMeterLevel(AudioBusId bus) const;
    /** Buffer latency in ms (period frames / sample rate). 0 when device not initialized. */
    float GetOutputLatencyMs() const;
    /** Set bus meter level (e.g. from AudioSystem when that bus has playing voices). */
    void SetBusMeterLevel(AudioBusId bus, float level);
    /** Peak the bus meter (e.g. when a sound starts) for VU display. */
    void PeakBusMeter(AudioBusId bus, float level);

    /** Called from device data callback (audio thread): update master meter peak from mixed PCM. */
    void OnDeviceDataCallback(ma_device* pDevice, void* pOutput, ma_uint32 frameCount);
    void SetOutputCaptureCallback(OutputCaptureCallback callback);

    /** Static device callback for miniaudio (needs to be callable with backend as pUserData). */
    static void DataCallback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount);

private:
    /** Device lifecycle events (start/stop/reroute/interruption/unlock) logged for diagnostics. */
    static void NotificationCallback(const ma_device_notification* pNotification);

    ma_engine m_Engine{};
    bool m_EngineInitialized = false;
    ma_context m_Context{};
    ma_device m_Device{};
    bool m_DeviceOwned = false;  // true if we created the device (custom callback for metering)

    // Every voice is fed from a per-voice buffer ref or in-memory decoder, never
    // through resource-manager file loading, so the manager runs with no job
    // thread: the only threads alive are the platform's own audio thread(s).
    ma_resource_manager m_ResourceManager{};
    bool m_ResourceManagerInit = false;

    // Simple fixed buses for MVP:
    // 0=Master, 1=Music, 2=SFX, 3=UI, 4=VO, 5=Aux (so mixer CH5 is independent from CH4)
    ma_sound_group m_BusMaster{};
    ma_sound_group m_BusMusic{};
    ma_sound_group m_BusSfx{};
    ma_sound_group m_BusUi{};
    ma_sound_group m_BusVo{};
    ma_sound_group m_BusAux{};
    bool m_BusMasterInit = false;
    bool m_BusMusicInit = false;
    bool m_BusSfxInit = false;
    bool m_BusUiInit = false;
    bool m_BusVoInit = false;
    bool m_BusAuxInit = false;

    // Per-bus meter level (0..1) for VU. Bus 0 from device callback; buses 1–5 from per-bus PCM meter nodes.
    static constexpr unsigned kMaxBuses = 6;
    float m_BusMeterLevels[kMaxBuses] = {};
    std::atomic<float> m_MasterMeterPeak{0.0f};  // written in device callback, read in Update()
    std::mutex m_OutputCaptureMutex;
    OutputCaptureCallback m_OutputCaptureCallback;

    // Per-bus PCM metering: one passthrough node per bus 1..5, each writes peak to an atomic.
    static constexpr unsigned kNumChildBuses = 5;
    BusMeterNode m_BusMeterNodes[kNumChildBuses] = {};
    std::atomic<float> m_BusMeterPeaks[kNumChildBuses] = {};
    bool m_BusMeterNodesInit = false;

    static void BusMeterNodeProcess(ma_node* pNode, const float** ppFramesIn, ma_uint32* pFrameCountIn, float** ppFramesOut, ma_uint32* pFrameCountOut);

    static const ma_node_vtable kBusMeterNodeVtable;
};

} // namespace GameEngine::Audio

