#pragma once

// LOD selection for point-light shadow cube faces.
//
// A cube face is its OWN camera. What a shadow map must not out-resolve is its
// own texel grid, so the level a caster rasterizes at is set by the caster's
// extent seen from the LIGHT and by the face's tile resolution — not by where
// the viewer stands or how tall the window is.
//
// This is what makes the atlas render cache (PointShadowAtlasPlanner L1a)
// SOUND rather than merely keyed. A cached slot replays depth rasterized at the
// levels its last render picked. Were selection to consume a camera-derived
// term, every frame of camera motion would force a choice between two broken
// options: key the term and re-render all six faces of every admitted slot
// whenever the viewer moves, or omit it and silently retain depth at levels a
// fresh render would no longer choose. Selecting from the light removes the
// term instead of trying to key it.
//
// Camera importance still reaches the decision — through the resolution Tier.
// A lower tier means a smaller tile, which raises the SSE switch points below
// and so selects coarser levels. That channel is quantized, hysteresis-damped
// and cache-keyed, which is exactly what a per-frame camera position is not.

#include "Engine/Rendering/MeshLODThresholds.h"
#include "Engine/Rendering/PointShadowAtlasPlanner.h"
#include "Mathematics/Vector3.h"
#include "Rendering/Core/GPUDrawStreamBuilder.h"

#include <cstdint>

namespace GameEngine
{
namespace Engine::Renderer
{

// cot(fovY/2) for a cube face's fixed 90° vertical FOV: PopulatePointShadowGeometry
// builds every face with MakePerspectiveLH_ZO_ReverseZ(0.5 * kPi, 1.0, ...), whose
// proj[1][1] is 1.0 exactly. With projScaleY == 1 the scatter's coverage metric
// (worldRadius * projScaleY / dist) reduces to the caster's angular half-extent as
// seen from the light, in the face's NDC. PointFaceProjScaleYMatchesFaceProjection
// pins this constant against the real matrix so a FOV change cannot drift from it.
inline constexpr float kPointShadowFaceProjScaleY = 1.0f;

// LOD selection params for the six depth slices of one point-shadow atlas slot.
//
// The light is the camera and the face's tile is the viewport: distance is
// measured to lightPositionWS, and the SSE budget converts to coverage space
// through tileResolution, so the error budget is spent in SHADOW-MAP texels
// rather than screen pixels.
//
// Takes the cache key type itself rather than a mirror of its fields: a slot's
// retained depth is only replayable when what the planner keyed is exactly what
// the bucketer registered, and passing the one struct makes that identity
// structural instead of a comment two files apart.
//
// smallCullCoverage stays 0 — the rule every shadow bucket follows, since a
// culled caster's shadow can be far larger than the caster's own coverage.
inline Rendering::GPUDrawStreamBuilder::ViewLODParams MakePointFaceLodParams(
    const Mathematics::Vector3& lightPositionWS, uint32_t tileResolution,
    const PointShadowLodKey& knobs)
{
    Rendering::GPUDrawStreamBuilder::ViewLODParams p{};
    p.cameraPos[0] = lightPositionWS.x;
    p.cameraPos[1] = lightPositionWS.y;
    p.cameraPos[2] = lightPositionWS.z;
    p.projScaleY = kPointShadowFaceProjScaleY;
    p.lodBiasGlobal = knobs.Bias;
    p.forceLod = knobs.ForceLevel;
    p.smallCullCoverage = 0.0f;
    p.sseThresholdToCoverage =
        ::GameEngine::Rendering::LodSseThresholdToCoverage(tileResolution, knobs.ErrorBudgetPx);
    p.sseThresholdToCoverageTight = ::GameEngine::Rendering::LodSseThresholdToCoverage(
        tileResolution, knobs.ErrorBudgetPx * knobs.SkinnedBudgetScale);
    return p;
}

} // namespace Engine::Renderer
} // namespace GameEngine
