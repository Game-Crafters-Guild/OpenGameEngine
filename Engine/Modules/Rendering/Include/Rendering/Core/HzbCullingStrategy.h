#pragma once

#include "Rendering/Core/CullingStrategy.h"

#include <memory>

namespace GameEngine
{
namespace Rendering
{

// Two-phase HZB occlusion strategy (R2.1). Phase A submits the same
// frustum input as FrustumCullingStrategy but additionally reserves the
// view's phase-B visibility slice (ViewCullingInput::reserveOcclusionSlice):
// EndFrameRG lays the slice out and publishes its slicePhase=1 range, and
// the mid-pipeline HZB nodes dispatch into it after the phase-A raster via
// GPUCullingPipeline::ScheduleOcclusionCullPass.
//
// Scaffold scope (P1): the reservation + deferred dispatch seam. The
// occlusion TEST itself (prevVisible ∧ HZB sample) arrives with the P2 cull
// shader variant; until then the phase-B dispatch repeats the frustum test.
//
// Cascades stay frustum-only by design — the main-view HZB does not apply
// to light frusta — so only the view's own slice (cascadeIndex none)
// reserves a phase-B generation.
//
// Unity anchor: GPU Resident Drawer occlusion mode; UE anchor: the
// two-pass HZB cull from the GPU-driven pipeline.
class HzbCullingStrategy final : public ICullingStrategy
{
  public:
    void ScheduleCulling(const ViewCullingContext& ctx) override;
};

// Default occlusion culling strategy for a perspective main view (editor game
// view, shipped player view): the shared two-phase HZB strategy, or nullptr —
// which the view registry resolves to the default frustum-only strategy — when
// GE_HZB_OCCLUSION=0 kills it, so one env var governs every main view. The env
// is read once. The strategy is stateless (each view's prevVisible history
// lives in GPUCullingPipeline keyed by viewId, and its HZB pyramid is a pooled
// persistent render-graph texture keyed by name = poolPrefix + viewId), so a
// single cached instance is reused across every view, frame, and window.
std::shared_ptr<ICullingStrategy> MakeDefaultOcclusionStrategyOrNull();

} // namespace Rendering
} // namespace GameEngine
