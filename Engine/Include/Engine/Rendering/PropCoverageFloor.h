#pragma once

// Distant small-prop screen-coverage floor MATH: the candidate design is an
// instance whose projected screen size falls below a small pixel threshold
// being culled in the GPU draw stream (draw_command_scatter.comp), with a
// hysteresis band so a prop drifting across the boundary never pops on/off
// frame to frame — cull below the floor, re-admit only above
// floor*(1+hysteresis). Analogous to UE's r.MinScreenRadiusForDepthPrepass
// with a GDistanceFadeMaxTravel-style band and Godot's visibility_range
// begin/end margins. A v1 revival floors MAIN-view slices only (color + depth
// prepass, the cascade-none scatter slices); shadow slices are never floored:
// a sub-2px prop's shadow is usually also sub-2px, but a NEAR shadow cast by a
// far prop is not — keeping shadows unfloored sidesteps that class entirely.
//
// The floor is NOT wired into the scatter. The 2026-07-20 census on
// Demo_unity (ElvenRealm) measured the sub-floor share of frustum-visible
// instances at 0.01% (street) / 0.20% (overview) — far under the 1% bar that
// would justify the cull machinery (per-instance hysteresis state SSBO,
// opt-out plumbing, scatter block). Post draw-consolidation there is no
// per-draw CPU cost left to save, so the floor's whole win is those
// instances' vertex/raster work: negligible here. The only consumer today is
// the GE_PROP_CENSUS diagnostic (RenderServicesWorldPass), which keeps this
// math honest as the sizing tool for any future scene that tips the calculus;
// CoverageFloorCulls is the unit-tested decision rule a future GLSL block
// must mirror.

#include <algorithm>
#include <cstdint>

namespace GameEngine
{
namespace Rendering
{

// Candidate projected-DIAMETER floor in pixels, denominated against
// kCoverageFloorReferenceHeightPx. Deliberately sub-2px so a cull at this
// floor would be visually imperceptible. The GE_PROP_CENSUS histogram reports
// the share of visible instances below this floor (and below its admit band).
inline constexpr float kMinPropCoveragePx = 1.5f;

// Reference viewport height the pixel floor is denominated against. The floor is
// applied as a resolution-INDEPENDENT fraction of screen height
// (floorPx / kCoverageFloorReferenceHeightPx), matching how the LOD coverage
// thresholds are already resolution-independent. At this native height the
// floor equals floorPx device pixels of projected diameter; at higher
// resolutions it culls the same screen FRACTION (a few more device pixels,
// still sub-perceptual).
inline constexpr float kCoverageFloorReferenceHeightPx = 1080.0f;

// Hysteresis band as a fraction of the base floor. A prop culled by the floor
// must grow past floor*(1+kCoverageFloorHysteresis) to be re-admitted, so a
// camera drifting across the boundary sees no per-frame pop.
inline constexpr float kCoverageFloorHysteresis = 0.6f;

// NDC coverage: the projected bounding-sphere RADIUS in NDC-Y units (NDC-Y spans
// [-1,1]). Matches draw_command_scatter.comp::ge_SelectLOD's `coverage` BEFORE
// its LOD-bias scaling — the floor is on true screen size, so lodBias (which
// only biases LOD selection) must not move it. projScaleY is proj[1][1].
inline float CoverageNdc(float worldRadius, float projScaleY, float dist)
{
    return worldRadius * projScaleY / std::max(dist, 1e-4f);
}

// Convert a projected-DIAMETER pixel floor into the NDC coverage (radius) floor
// the scatter compares against. A sphere of NDC radius r projects to a diameter
// of r * viewportHeightPx pixels, so the diameter floor in NDC-radius units is
// floorPx / viewportHeightPx. Callers pass kCoverageFloorReferenceHeightPx for
// the resolution-independent floor. Returns 0 (disabled) for a non-positive
// floor or height.
inline float CoverageFloorNdc(float floorPx, float viewportHeightPx)
{
    if (floorPx <= 0.0f || viewportHeightPx <= 1.0f)
        return 0.0f;
    return floorPx / viewportHeightPx;
}

// The re-admit threshold: a floor-culled instance stays culled until its
// coverage exceeds this. CPU-derived alongside CoverageFloorNdc so the shader
// receives both as push constants and never re-encodes the hysteresis rule.
inline float CoverageFloorAdmitNdc(float floorNdc, float hysteresis)
{
    return floorNdc * (1.0f + std::max(hysteresis, 0.0f));
}

// Hysteresis decision for one instance. `coverageNdc` is CoverageNdc above,
// `floorNdc` is CoverageFloorNdc, `wasCulled` is this instance's floor state
// from the previous frame. Returns whether the instance is culled THIS frame:
// while drawn, cull once coverage drops below floorNdc; while culled, stay
// culled until coverage exceeds floorNdc*(1+hysteresis). floorNdc <= 0 disables
// the floor (never culls). A future scatter implementation must mirror this
// rule exactly (and change both in the same commit).
inline bool CoverageFloorCulls(float coverageNdc, float floorNdc, float hysteresis,
                               bool wasCulled)
{
    if (floorNdc <= 0.0f)
        return false;
    const float effectiveFloor =
        wasCulled ? CoverageFloorAdmitNdc(floorNdc, hysteresis) : floorNdc;
    return coverageNdc < effectiveFloor;
}

} // namespace Rendering
} // namespace GameEngine
