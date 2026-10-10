#pragma once

#include <cstdint>

#include "Rendering/CameraTypes.h"
#include "Rendering/Common/Math.h"
#include "Rendering/Core/GPUCulling.h"

namespace GameEngine
{
namespace Rendering
{

class GPUScene;
class GPUCullingPipeline;

// Per-view inputs the orchestrator hands to a strategy each frame. Carries
// everything an `ICullingStrategy` implementation might need: the view's
// transform / frustum, the per-view candidate instance range, and references
// to the shared backends (GPUScene for instance data, GPUCullingPipeline for
// enqueuing per-view culling dispatches). Strategies that don't need a piece
// of the context simply ignore it.
struct ViewCullingContext
{
    ViewId Id = 0;
    uint8_t CascadeIndex = kCullingCascadeIndexNone;

    Matrix4x4 ViewMatrix;
    Matrix4x4 ProjMatrix;
    Matrix4x4 ViewProjMatrix;
    Vector4 FrustumPlanes[6];

    Vector3 CameraPosition;
    Vector3 CameraForward;
    float NearPlane = 0.1f;
    float FarPlane = 1000.0f;

    // Camera-relative culling origin (Earth-scale precision): the view's render
    // origin in world meters, or (0,0,0) when inactive. Copied verbatim into
    // ViewCullingInput::cameraRelativeOrigin. See that field for the rationale.
    Vector3 CameraRelativeOrigin{0.0f, 0.0f, 0.0f};

    // The candidate range this view operates on. Today both fields cover the
    // whole GPUScene (FirstInstance=0, InstanceCount=scene-&gt;GetInstanceCount());
    // they're carried explicitly so future strategies (e.g. spatial-partition
    // narrowing on the CPU side, streaming worlds) can scope a view to a
    // subrange without having to plumb a parallel API.
    uint32_t FirstInstance = 0;
    uint32_t InstanceCount = 0;
    uint32_t RenderLayerMask = 0xFFFFFFFFu;

    uint32_t FrameIndex = 0;
    float DeltaTime = 0.0f;

    // Backends. Non-owning. Required for strategies that enqueue per-view
    // dispatches into the GPU culling pipeline; left null only for tests
    // that construct contexts in isolation.
    GPUCullingPipeline* CullingPipeline = nullptr;
    GPUScene* Scene = nullptr;
};

// Strategy interface for per-view culling. `RenderServices::ScheduleViewCulling`
// (Phase 3d) iterates the engine's active views and calls `ScheduleCulling`
// once per view; the strategy decides what work to enqueue. Implementations
// today: `FrustumCullingStrategy` (default — the existing GPU frustum cull),
// `NoneCullingStrategy` (layer-only — thumbnails / asset previews),
// `HzbCullingStrategy` (two-phase HZB occlusion; reserves the view's
// phase-B visibility slice). Future strategies (quadtree / octree spatial
// partitioning, inclusive-then-refine for CSM cascades) plug in without
// touching the orchestrator.
class ICullingStrategy
{
public:
    virtual ~ICullingStrategy() = default;

    // Called once per view per frame from the per-view culling loop. The
    // strategy may submit a `ViewCullingInput` to `ctx.CullingPipeline` to
    // enqueue a GPU dispatch, schedule its own compute passes against `rg`,
    // or do nothing at all. Must not call `BeginFrame`/`EndFrame` on the
    // pipeline — those are owned by the orchestrator so per-frame state
    // (visibility-layout, pending views) is collected across all strategies
    // before a single batched dispatch.
    virtual void ScheduleCulling(const ViewCullingContext& ctx) = 0;
};

} // namespace Rendering
} // namespace GameEngine
