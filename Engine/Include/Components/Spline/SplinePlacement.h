#pragma once

#include "Components/AssetRef.h"
#include "Types/Types.h"

#include <type_traits>

namespace GameEngine::Components
{

// Mesh slots per placement role. Sized from the one real kit shape (the Synty
// footpath family: 2 straights, 4 corners, 5 stones) as largest-family + 1.
// Fixed capacity keeps the component trivially copyable; the inspector reports
// a full pool as a validation message, never a silent drop.
inline constexpr uint32 kSplinePoolCapacity = 6;

// Ceiling, in degrees, on how far a placed piece's up may depart from world up.
// One rule for both spline recipes: a tile takes its up from the surface normal
// and a racked span takes its up from the grade it climbs, and past this angle
// neither reads as a piece resting on ground — a fully-aligned tile on a
// near-vertical face turns its width axis skyward and renders as a standing
// sheet. Kit stair and path pieces read as ground cover to roughly 35-40
// degrees, so that is where the default sits; it is a dial, not a constant of
// nature. Height still conforms past the ceiling — only the tilt is held.
inline constexpr float32 kDefaultSplineMaxTiltDegrees = 35.0f;

// How placement distributes tile origins along the spline's arc length.
enum class SplinePlacementFit : int32
{
    // Round the tile count to fit the spline exactly; samples land on both ends.
    FitToLength = 0,
    // Step exactly Spacing metres from the start; the last partial step is cut.
    FixedPitch = 1,
};

// How each placed tile conforms to the surface under the spline.
enum class SplinePlacementConform : int32
{
    None = 0,           // use the spline frame as-is
    Height = 1,         // drop to the surface, keep world-up orientation
    HeightAndSlope = 2, // drop to the surface and align to its normal (pitch + roll)
};

// What a conform ray is allowed to land on. It is the author's choice because
// only the author knows whether a mesh under the spline is scenery the pieces
// should ride over or ground they should sit on: a bridge deck is ground, a
// cart is not, and the ray cannot tell them apart.
enum class SplineConformTarget : int32
{
    // Every pickable surface: terrain, meshes, primitives. A station standing
    // under a prop takes THAT prop's surface for ground and rides on its roof.
    Scene = 0,
    // Terrain only. Props, dressing and other placed meshes are transparent to
    // the ray, so pieces follow the ground beneath them.
    TerrainOnly = 1,
};

// Which plane of a placed mesh lands on the conform hit. Kits disagree about
// where "the ground" is in their local space, and guessing wrong is a visible
// half-metre error either way, so it is authored per recipe rather than derived.
enum class SplinePlantMode : int32
{
    // Lift until the mesh's lowest vertex touches the surface. Right for kits
    // authored with bounds-min at ground level.
    BoundsMin = 0,
    // Land the mesh's local y=0 plane on the surface and let anything below it
    // bury. Right for kits whose pivot IS the authored plant line and whose
    // bounds dip below it — measured fence panels carry a 0.52 m planting
    // skirt, castle towers 2.50 m, which BoundsMin would float into the air.
    PivotPlane = 1,
};

// Places instances of a mesh family along the sibling SplineComponent's curve.
// Pure authored data: the editor-side SplinePlacementController samples the
// spline, conforms against the scene, and spawns runtime-only tile entities —
// the scene serializes this recipe, never the placed tiles.
struct SplinePlacement
{
    // Piece pools — one mesh family per role. A pool's active entries are its
    // leading non-empty slots; everything after the first empty slot is
    // ignored (the inspector compacts on edit, so a hole only arises from a
    // hand-edited scene and is surfaced as a validation message — see
    // SplinePoolSelection.h for the active-count/hole helpers). A pool is a
    // set of interchangeable cross-sections, not an arbitrary bag of meshes:
    // members must share footprint width and pivot convention, which the
    // layout derives from the first entry.
    //
    // The controller consumes StraightPool (deterministic per-station pick,
    // SplinePoolSelection.h). CurvePool and ScatterPool are authored family
    // data whose consumers land with the corner and scatter slices; the
    // fields serialize and validate today so recipes authored now carry the
    // full family contract.
    Components::ModelRef StraightPool[kSplinePoolCapacity];
    Components::ModelRef CurvePool[kSplinePoolCapacity];
    Components::ModelRef ScatterPool[kSplinePoolCapacity];
    // Metres between tile centers along the conformed (draped) centerline —
    // true 3D distance over the ground, not authored-curve arc length.
    float32 Spacing = 2.0f;
    SplinePlacementFit Fit = SplinePlacementFit::FitToLength;
    SplinePlacementConform ConformMode = SplinePlacementConform::HeightAndSlope;
    // What the conform ray may land on. Scene (the default) is every pickable
    // surface; TerrainOnly makes scenery transparent so pieces do not ride the
    // roofs of the props they pass under.
    SplineConformTarget ConformTarget = SplineConformTarget::Scene;
    // Which plane of the tile lands on the conform hit. BoundsMin lifts the
    // lowest vertex onto the surface; PivotPlane lands local y=0 and lets what
    // is below it bury. The default is BoundsMin because that is what placement
    // did before the field existed: a scene with no PlantMode line must bed its
    // tiles exactly where the previous build put them.
    // @ge-tooltip Which plane of the tile meets the ground. Bounds Min lifts until the mesh's lowest vertex touches, so a slab authored around its own mid-plane stands its full thickness above the ground; Pivot Plane lands the mesh's local y=0 on the surface and lets anything below it bury. Kits disagree about where their ground is and the wrong choice is a visible error either way: the measured Synty footpath slabs are 0.27 m thick with their pivot 0.19 m above their lowest vertex, so Bounds Min stands the whole slab proud where Pivot Plane leaves under 0.10 m.
    SplinePlantMode PlantMode = SplinePlantMode::BoundsMin;
    // Metres to the right of travel; negative = left.
    float32 LateralOffset = 0.0f;
    // 0 = tiles stay world-up, 1 = tiles align fully to the surface normal.
    float32 SlopeBlend = 1.0f;
    // Ceiling on the blend result: a tile whose surface normal leans further
    // than this from world up is held at the ceiling and still conforms in
    // height. Without it a path crossing a near-vertical crest folds into
    // standing sheets, because the tile's width axis is derived from its up.
    float32 MaxTiltDegrees = kDefaultSplineMaxTiltDegrees;
    // Salts the deterministic per-station pool pick (SplinePoolSelection.h) and
    // every jitter draw below (SplineStationJitter.h); same Seed + same station
    // index = same pick, same wobble, same survivors on every rebuild.
    uint32 Seed = 0;
    // Per-station variation. A kit laid at an exact pitch reads as railway
    // sleepers — the eye finds the period immediately — and these break that up
    // without moving the path itself. All four are deterministic in Seed, and
    // all four are keyed on a station's ORDINAL, so raising Dropout does not
    // reshuffle the wobble or the mesh picks of the stations that survive.
    //
    // Metres a station may slide ALONG the centerline, either way. The layout
    // clamps it to half the spacing, so stations never reorder.
    float32 SpacingJitterMetres = 0.0f;
    // Degrees a station may turn about its own up, either way.
    float32 YawJitterDegrees = 0.0f;
    // Metres a station may slide ACROSS travel, either way.
    float32 LateralJitterMetres = 0.0f;
    // Fraction of stations left empty: 0 places every station, 1 places none.
    // Gaps are what stop a run reading as a manufactured strip.
    float32 DropoutChance = 0.0f;
    // Metres at EACH end of the spline over which stations thin to nothing, so
    // a run peters out instead of stopping square. Mirrors the name
    // SplineExtrude::EndTaperMetres carries for the same idea; that one narrows
    // a swept ribbon, and a kit piece cannot narrow, so the placement analogue
    // is density. 0 disables.
    float32 EndTaperMetres = 0.0f;
    // Cap, in degrees, on the per-tile skew used to soften rigid seams: each
    // joint shears both neighbouring tiles by tan(yaw/2) toward its bisector,
    // and a tile's summed shear is clamped to this angle — past roughly 8 the
    // skewed silhouette reads as distortion. 0 disables.
    float32 SeamShearMaxDegrees = 8.0f;
    // Replaces the material on every placed tile, across every pool. Null (the
    // default) keeps each mesh's own embedded model material. Kit FBX commonly
    // embed a bare untextured Lambert, or name source textures the project
    // never imported, so the embedded material is often not the one the scene
    // wants; the override retextures a placement without touching the shared
    // meshes. A GUID the runtime material registry cannot resolve is not bound:
    // the binder logs once and keeps the embedded material rather than letting
    // render extraction drop the tiles.
    MaterialRef OverrideMaterial;

    // Memberwise (arrays element-wise): the rebuild scheduler compares whole
    // recipes, so a hand-written comparison that missed a field would silently
    // stop rebuilds for edits to that field. Defaulted == cannot miss one.
    bool operator==(const SplinePlacement&) const = default;
};

static_assert(std::is_trivially_copyable_v<SplinePlacement>,
              "SplinePlacement must be trivially copyable for ECS storage");
static_assert(std::is_standard_layout_v<SplinePlacement>,
              "SplinePlacement must be standard layout for ECS storage");

} // namespace GameEngine::Components
