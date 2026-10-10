#pragma once

#include "Engine/Rendering/IRenderFeature.h"
#include "Rendering/CameraTypes.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/RenderGraph/PerViewReadbackRings.h"

#include <cstdint>
#include <unordered_map>

namespace GameEngine::Engine::Renderer
{

// Every-frame GPU->CPU readback of AutoExposureNode's adaptation state, per
// view, on the RGReadbackRing completion contract (token-gated via the generic
// IRenderFeature::OnFrameSubmittedRG fan-out, like CSM's SDSM readback). Off by
// default: a consumer opts a view in (the editor Scene View, to seed Fixed
// EV100 from the metered exposure when auto exposure is toggled off) and
// AutoExposureNode declares the copy only for enabled views, so uninterested
// views pay nothing.
class ExposureReadbackFeature : public IRenderFeature
{
  public:
    // CPU mirror of auto_exposure_resolve.comp's ExposureState SSBO — the one
    // layout both the node's copy size and the ring slot size derive from.
    struct State
    {
        float Scale;    // resolved LINEAR scene-exposure multiplier
        uint32_t Valid; // != 1 -> the resolve shader has not written yet
        float PadA;
        float PadB;
    };
    static constexpr uint32_t kStateBytes = sizeof(State);
    static_assert(kStateBytes == 16u, "must match the shader's ExposureState layout");

    ExposureReadbackFeature();
    ~ExposureReadbackFeature() override;

    // Enabling registers the view; disabling releases its ring and latch
    // (device buffer destroys are timeline-deferred, so in-flight copies are
    // safe). Both are idempotent — callers may sync the flag every frame.
    void SetReadbackEnabled(Rendering::ViewId viewId, bool enabled);
    bool IsReadbackEnabled(Rendering::ViewId viewId) const;

    // Begins a ring write for this frame; AutoExposureNode copies the
    // adaptation state into the returned slot after its resolve dispatch.
    Rendering::BufferHandle AcquireSlotRG(Rendering::IDevice* device,
                                          const Rendering::RenderGraph::RGFrame& frame,
                                          Rendering::ViewId viewId);

    void OnFrameSubmittedRG(Rendering::RenderGraph::RGFrame& frame,
                            const Rendering::IDevice::GpuSyncToken& token) override;
    void OnFrameStreamRetiredRG(Rendering::RenderGraph::RGFrame& frame) override;

    // Q6 slice 4 (§8-completion): the readback ring slots are persistently MAPPED; an
    // in-place device rebuild frees them, so the next AcquireSlotRG/read through the
    // dangling mapped pointer is a use-after-free. Drop the rings so they lazily
    // re-create + re-map (design §4). The last-good latched scale (CPU-side) survives.
    void OnDeviceRebuilt(Rendering::IDevice* device) override;

    // Newest adapted LINEAR exposure scale for the view. False until the first
    // metered frame's copy lands or while the resolve shader has not yet
    // written a valid state; latches the last good value between arrivals.
    // When requested, the frame index belongs to that last-good GPU sample,
    // which may precede the camera's latest declared frame. False leaves outputs untouched.
    bool TryResolveAdaptedExposure(Rendering::ViewId viewId, float& outLinearScale,
                                   uint64_t* outFrameIndex = nullptr);

  private:
    struct SlotTag
    {
        uint64_t FrameIndex = 0;
    };

    // CPU-side per-view state; the rings themselves live in m_Rings so the
    // shared helper owns the whole readback lifecycle.
    struct ViewState
    {
        float LastScale = 0.0f;
        uint64_t LastFrameIndex = 0;
        bool HasScale = false;
        bool Enabled = false;
    };

    Rendering::IDevice* m_Device = nullptr;
    Rendering::RenderGraph::PerViewReadbackRings<SlotTag> m_Rings;
    std::unordered_map<Rendering::ViewId, ViewState> m_Views;
};

} // namespace GameEngine::Engine::Renderer
