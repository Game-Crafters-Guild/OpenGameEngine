#pragma once

#include "Rendering/Core/Device.h" // IDevice::GpuSyncToken in OnFrameSubmittedRG

#include <cstdint>
#include <vector>

namespace GameEngine
{
namespace Rendering
{
class GPUCullingPipeline;
class GPUScene;
struct ViewDesc;
struct CameraInfo;
namespace RenderGraph
{
class RGFrame;
}
} // namespace Rendering

namespace Engine::Renderer
{

class RenderServices;
struct FeatureDeclareContext;

// Per-frame context passed to IRenderFeature::OnScheduleCulling. Carries the
// active RG, the GPU culling backend, and the data a feature needs to emit
// supplementary ViewCullingInput submissions (e.g. CSM's per-cascade fan-out)
// beyond the per-view ScheduleCulling driven by ICullingStrategy.
struct FeatureCullingContext
{
    // The RenderGraph frame the pipeline captured at BeginFrame. Features that only
    // Submit* (CSM) don't need it — they go through the culling backend.
    GameEngine::Rendering::RenderGraph::RGFrame* Frame = nullptr;
    GameEngine::Rendering::GPUCullingPipeline* CullingPipeline = nullptr;
    GameEngine::Rendering::GPUScene* Scene = nullptr;
    // The services the frame is scheduled on: what a feature reads to build its
    // submissions from this frame's state (lights, settings, cameras).
    RenderServices* Services = nullptr;
    const std::vector<GameEngine::Rendering::ViewDesc>* Views = nullptr;
    const std::vector<GameEngine::Rendering::CameraInfo>* Cameras = nullptr;
    uint32_t InstanceCount = 0;
    uint32_t FrameIndex = 0;
    float DeltaTime = 0.0f;
};

class IRenderFeature
{
  public:
    virtual ~IRenderFeature() = default;

    // Optional hook for features to emit GPU culling submissions beyond the
    // per-view default strategy. Called once per frame, after all per-view
    // ICullingStrategy::ScheduleCulling calls and before
    // GPUCullingPipeline::EndFrame. CSM uses this to fan a single view out
    // into N cascade-specific ViewCullingInput entries; other features (HZB
    // refinement, async sky probe culling) can layer in the same way.
    virtual void OnScheduleCulling(const FeatureCullingContext& /*ctx*/) {}

    // True while the feature rasterizes depth that can change every frame
    // outside the GPU-driven instance path and the draw-producer registry (an
    // animated ocean surface, adaptive terrain). While any feature reports true,
    // idle elision re-runs every depth-derived family (depth min/max, clustered
    // light culling, SDSM, HZB). Read once per frame on the render thread.
    virtual bool WritesDynamicDepth() const { return false; }

    // RenderGraph declaration hook. A feature that owns render targets declares
    // the passes that write them here — driven per (feature, view) by the
    // pipeline node that orchestrates the feature. Default no-op: features that
    // declare no passes (the common case) leave their frames untouched, exactly
    // like the readback hooks below. `rs` is the shared RenderServices whose
    // MakeDepthDrawServices() the pass exec lambdas call to record through the free
    // RecordDepthOnlyPass (captured by value — never the ctx, which is a
    // declaration-scope temporary); `ctx`
    // carries the frame-validated snapshots plus the ShadowDeclareSeam passkey a
    // feature needs to reach the shared RenderServices shadow plumbing.
    virtual void Declare(::GameEngine::Rendering::RenderGraph::RGFrame& /*frame*/,
                         RenderServices& /*rs*/, const FeatureDeclareContext& /*ctx*/)
    {
    }

    // Called after a frame's command lists are submitted, with the
    // submission's fence token. Features stamp pending GPU->CPU readbacks
    // (RGReadbackRing::OnFrameSubmitted) — completion is the token signaling,
    // not ring depth or cadence.
    virtual void OnFrameSubmittedRG(::GameEngine::Rendering::RenderGraph::RGFrame& /*frame*/,
                                    const ::GameEngine::Rendering::IDevice::GpuSyncToken& /*token*/)
    {
    }

    // Called when a frame stream is retired (window closed). Features purge
    // readback pendings keyed to the dying RGFrame so a heap-recycled frame
    // at the same address cannot falsely stamp them
    // (RGReadbackRing::OnFrameStreamRetired).
    virtual void OnFrameStreamRetiredRG(::GameEngine::Rendering::RenderGraph::RGFrame& /*frame*/)
    {
    }

    // Q6 slice 4 (§8-completion): after an in-place device rebuild, every persistent
    // GPU resource a feature holds outside the per-frame render graph (shadow
    // samplers, IBL cubemaps, sky LUTs, mapped ring buffers, …) is dead, yet its
    // cached handle still reads IsValid() — Handle::IsValid inspects only the handle's
    // own bits (index/generation sentinels) and never consults the manager, so it stays
    // true no matter what the teardown did. A feature that caches such resources
    // overrides this to FORGET the dead handles so the resource is recreated. Why the
    // reset is load-bearing (a stale Destroy* is itself a harmless no-op — the teardown
    // routes every live resource through Destroy*, which bumps the manager generation,
    // so a stale handle no longer resolves — and double-free is NOT the reason): the
    // danger is USING the stale-but-valid
    // handle — (a) an IsValid()-guarded lazy getter returns the dead handle instead of
    // recreating, (b) a capacity/desc-match early-return reuses it instead of
    // recreating, and (c) worst, a cached RAW MAPPED POINTER into freed VMA memory is
    // written through on the next upload — a true use-after-free that generational
    // handles do not guard. Also re-arm any content bake. Runs on the render thread
    // inside OnDeviceRebuilt, after mesh/GPUScene re-provision, before rendering
    // resumes. Default no-op: features holding only RG-transient resources self-heal.
    virtual void OnDeviceRebuilt(::GameEngine::Rendering::IDevice* /*device*/) {}
};

} // namespace Engine::Renderer
} // namespace GameEngine
