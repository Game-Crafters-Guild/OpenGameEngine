#pragma once

#include "Mathematics/Vector3.h"
#include "SplineGeometry/SplineStation.h"
#include "Types/Types.h"

#include <limits>
#include <span>
#include <vector>

namespace GameEngine::SplineGeometry
{

// A water surface is not a swept cross-section. Its extent is decided by the
// GROUND -- the curve only says where the water is and how high it stands -- so
// the shape is found by flooding a region rather than by measuring a width at
// each station and welding the results into a ribbon.
//
// That is what makes the three geometry defects of the ribbon impossible rather
// than mitigated: a region cannot self-intersect, it reaches into pockets that
// lie on no station's perpendicular, and two legs of one hairpin over one basin
// are ONE region visited once instead of two surfaces overlapping.
//
// This half solves the field and the connectivity; SplineFillMesher turns the
// result into triangles.

// Ground heights on the terrain's OWN sample lattice: corners ARE heightfield
// samples, so a corner height is an array read rather than an interpolation,
// and the shoreline is compared against the same data the author sees.
// Sampling finer than the source would put the waterline at a bilinear guess
// about a bed the renderer does not draw that way either.
struct SplineGroundGrid
{
    // World XZ of corner (0, 0), and the metres between adjacent corners.
    float32 OriginX = 0.0f;
    float32 OriginZ = 0.0f;
    float32 SpacingX = 0.0f;
    float32 SpacingZ = 0.0f;
    // Corner counts, NOT cell counts: a grid of N corners has N-1 cells.
    uint32 CountX = 0;
    uint32 CountZ = 0;
    // World-space ground altitude per corner, row-major with X fastest.
    // Size must be CountX * CountZ.
    std::span<const float32> Heights;

    [[nodiscard]] uint32 CornerCount() const { return CountX * CountZ; }
    [[nodiscard]] uint32 Index(uint32 cx, uint32 cz) const { return cz * CountX + cx; }
    [[nodiscard]] float32 WorldX(uint32 cx) const
    {
        return OriginX + static_cast<float32>(cx) * SpacingX;
    }
    [[nodiscard]] float32 WorldZ(uint32 cz) const
    {
        return OriginZ + static_cast<float32>(cz) * SpacingZ;
    }
    [[nodiscard]] bool IsValid() const
    {
        return CountX >= 2u && CountZ >= 2u && SpacingX > 0.0f && SpacingZ > 0.0f &&
               Heights.size() == static_cast<size_t>(CountX) * CountZ;
    }
};

// A waterline that rises going downstream by more than this is an authoring
// error worth naming rather than absorbing: above float noise on a draped
// centreline, below anything an author would intend.
inline constexpr float32 kWaterlineRiseToleranceMetres = 0.05f;

// "No floor". Also the arithmetic identity of the clamp that applies it, so the
// disabled state needs neither a branch nor an equality test -- which matters,
// because a sentinel compared for equality would have to survive a scene text
// round-trip to keep working.
inline constexpr float32 kNoSeaLevelFloor = std::numeric_limits<float32>::lowest();

// Metres the sea-level floor is lifted above the level it names. A floor set to
// the sea's own altitude makes the mouth EXACTLY coplanar with the sea surface,
// and two coincident planes with different tessellations resolve per pixel: the
// junction reads as lattice-aligned stripes rather than as water meeting water.
// Water standing a couple of centimetres proud of the sea at a river mouth is
// invisible; the stripes are not.
//
// Added unconditionally, with no branch and no equality test, because the
// disabled state is arithmetically immune: at kNoSeaLevelFloor's magnitude one
// ULP is ~2e31 metres, so this addition cannot move it.
inline constexpr float32 kSeaLevelFloorLiftMetres = 0.02f;

struct SplineFillParams
{
    // Radius of the corridor around the station stream, in world metres. Bounds
    // the region: this is the authored MaxHalfWidth, and it bounds the SEARCH
    // literally. It is not a wall -- water that reaches it over lower ground has
    // no evidence holding it up, so it is left out and counted as an
    // UnseenBankCorner rather than cut off there.
    float32 MaxHalfWidth = 12.0f;
    // Does two jobs, both of which it already did for the ribbon: the contour
    // runs at phi = -EdgeDrop rather than phi = 0, so the shoreline sits
    // laterally PAST the waterline crossing into ground above the water, and the
    // rim vertex is placed EdgeDrop below the surface. Together they bury the
    // edge against a bank near 45 degrees and over-bury a steeper one, which is
    // the harmless direction.
    float32 EdgeDrop = 0.1f;
    // The waterline never falls below this, so a river mouth meeting a sea at a
    // known level merges into the shoreline instead of interpenetrating it.
    // Defaults to no floor.
    float32 SeaLevelFloor = kNoSeaLevelFloor;
    // Hard ceiling on the region. A run that exceeds it BUILDS NOTHING and says
    // so rather than silently coarsening: a water surface whose resolution
    // changed with its size would be a worse bug than a refusal.
    uint32 MaxWetCorners = 262144u;
};

// Where a refusal was worst: the world XZ of the sample, and how far along the
// run it sits. A count says how many and a magnitude says how bad; neither says
// which stretch to carve or how far to widen, which is the thing an author acts
// on. Ties go to the first such sample in lattice order. Meaningless while the
// counter it belongs to is zero.
struct SplineFillWorstSample
{
    float32 X = 0.0f;
    float32 Z = 0.0f;
    float32 ArcDistance = 0.0f;
};

// Everything the run can say about itself in one number each. Today's ray fit
// can report none of these, which is why a river that quietly became a lake had
// to be diagnosed from a screenshot.
struct SplineFillDiagnostics
{
    uint32 Seeds = 0;
    // Stations whose own ground is not below their waterline. The single most
    // useful thing this feature can emit: it names carve-deeper-or-lift-the-
    // offset directly, where the ray fit silently narrowed to nominal width.
    uint32 DrySeeds = 0;
    Mathematics::Vector3 FirstDrySeed{};
    uint32 WetCorners = 0;
    // Corners the ground and the corridor would both have taken, refused because
    // the water there would have been standing over a bank it could flow across,
    // and the worst such overshoot in metres. Non-zero means the run asked for
    // more water than its channel holds.
    uint32 OverBankCorners = 0;
    float32 MaxOverBankMetres = 0.0f;
    // The sample MaxOverBankMetres was measured at.
    SplineFillWorstSample WorstOverBank{};
    // The subset of those whose escape route topped out at the CORRIDOR EDGE
    // rather than at ground the run can see. These two numbers are different
    // instructions and the split is the whole reason to keep both: the
    // difference is a channel that really is too shallow (carve deeper, or lower
    // VerticalOffset), and this count is a bank that lies outside MaxHalfWidth
    // and that the fill therefore has no evidence for (widen the reach).
    uint32 UnseenBankCorners = 0;
    // The worst of THOSE, ranked by the same overshoot. Rarely the same sample
    // as WorstOverBank: the two counts answer to different knobs, so each names
    // the stretch its own fix applies to.
    SplineFillWorstSample WorstUnseenBank{};
    // Corners the containment term refused that the region then turned out to
    // enclose on every side, so they were taken after all. Water cannot drain
    // through water: the outward escape such a corner was refused for is not a
    // route it can take. Non-zero means the channel's banks dip below the
    // waterline INSIDE the run — worth naming, because the same dip at the
    // region's edge does still bite there.
    uint32 SealedPocketCorners = 0;
    float32 MaxDepthMetres = 0.0f;
    // Largest waterline difference between two adjacent wet corners: the step
    // down the middle of a pool where one run's two legs share a basin at
    // different altitudes. Honest -- the upper leg IS pouring into the lower --
    // but worth naming when it gets large.
    float32 MedialStepMetres = 0.0f;
    // Metres the waterline RISES going downstream, if it does. An uphill wiggle
    // is an authoring error that should be visible, not clamped away.
    float32 WaterlineRiseMetres = 0.0f;
    float32 WaterlineRiseAtArc = 0.0f;
    // The region exceeded MaxWetCorners; the field carries no wet corners and
    // the caller must build nothing.
    bool ExceededBudget = false;
};

// The stamped and flooded field, one entry per grid corner.
struct SplineFillCorner
{
    // World Y of the water surface here, from the nearest station's arc
    // position. Height is an INPUT throughout: the fill reads the station
    // altitude the drape and VerticalOffset already produced and never writes
    // one back -- water it cannot hold is removed from the region rather than
    // lowered.
    float32 Waterline = 0.0f;
    // The lowest bank a drop here must clear to leave the corridor on a path
    // that runs AWAY from the centreline: the minimum over such paths of the
    // maximum ground along the path. Infinity where no outward path exists,
    // which is a pocket the river itself holds. Water above this would already
    // be over that bank, so the waterline is capped here.
    float32 Escape = std::numeric_limits<float32>::infinity();
    // XZ metres to the nearest station. Infinity where no station is near
    // enough to have stamped this corner.
    float32 Distance = std::numeric_limits<float32>::infinity();
    // Draped distance along the run and signed metres across it, for UV. U runs
    // along the flow in world metres exactly as the sweep's does.
    float32 ArcDistance = 0.0f;
    float32 LateralSigned = 0.0f;
    // How far INSIDE the region this corner is, in metres, before connectivity:
    //   min(phi + EdgeDrop, MaxHalfWidth - Distance, max(Escape, seaFloor) - Waterline)
    // the ground, the corridor and the containment constraints intersected. A sea
    // level contains water the way a bank does, so it JOINS the escape rather than
    // being overruled by it; seaFloor is SeaLevelFloor as the fill applies it,
    // lifted by kSeaLevelFloorLiftMetres, not the authored level.
    // Positive is inside. The mesher interpolates it to place shoreline
    // vertices, which is what makes the shoreline sub-cell rather than a
    // staircase of whole cells.
    float32 Inside = -std::numeric_limits<float32>::infinity();
    // Index of the governing station, or kNoStation.
    uint32 Station = 0xFFFFFFFFu;
    // In the region: reached by the 4-connected flood from a station seed.
    // A corner can have Inside > 0 and still be dry here -- that is exactly the
    // detached puddle over a bank crest, and refusing it is the whole point.
    uint8 Wet = 0;
    // Escape was decided at the corridor edge rather than at ground inside it,
    // so the rim it names is the limit of what the run looked at rather than a
    // bank. Separates "the channel is too shallow" from "MaxHalfWidth is too
    // small", which are opposite instructions to an author.
    uint8 EscapeUnseen = 0;
};

inline constexpr uint32 kNoStation = 0xFFFFFFFFu;

struct SplineFillResult
{
    std::vector<SplineFillCorner> Corners;
    SplineFillDiagnostics Diagnostics;

    [[nodiscard]] bool HasRegion() const { return Diagnostics.WetCorners > 0u; }
};

// Stamp the waterline field from the stations, then flood the region from the
// station seeds.
//
// The stamp is a discrete closest-point transform at STATION resolution (the
// caller's sampling step, finer than the lattice it labels), mirroring the rule
// the terrain carve itself uses to decide a spline volume's flatten target --
// the height at the closest point on the curve in XZ. Bed and water therefore
// agree by construction rather than by coincidence.
//
// Between the two runs the escape elevation, which is what stops the surface
// from standing above the land beside it. A river is not a pond: its water is
// sustained by inflow, so "would this drain?" answers yes everywhere (the
// channel itself is the drain) and cannot be the test. The test that works is
// directional -- water leaving ALONG the flow is the river working, water
// leaving ACROSS the bank is a flood -- so a drainage path must move away from
// the centreline, and the corridor is where it leaves rather than a wall it
// piles up against.
//
// The flood is 4-connected, never 8: an 8-connected flood leaks through a single
// diagonal gap between two dry corners, which is how a puddle escapes a basin
// that visually holds it.
[[nodiscard]] SplineFillResult BuildSplineFillField(std::span<const SplineStripStation> stations,
                                                    const SplineGroundGrid& ground,
                                                    const SplineFillParams& params);

} // namespace GameEngine::SplineGeometry
