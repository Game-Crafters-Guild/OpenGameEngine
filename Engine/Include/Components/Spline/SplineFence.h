#pragma once

#include "Components/AssetRef.h"
#include "Components/Spline/SplinePlacement.h"
#include "Types/Types.h"

#include <type_traits>

namespace GameEngine::Components
{

// Mesh slots per fence role. Sized from the measured kits: the largest role
// family found is 6 (castle battlements, all 5.000 m) as largest-family + 1.
// Fixed capacity keeps the component trivially copyable; the inspector reports
// a full pool as a validation message, never a silent drop.
inline constexpr uint32 kSplineFencePoolCapacity = 7;

// Span overrides a recipe can carry. Fixed capacity for the same reason the
// pools are: the component stays trivially copyable and chunk-resident.
inline constexpr uint32 kSplineFenceMaxSpanOverrides = 16;

// How a span meets the grade between the two stations it joins.
enum class SplineSpanGrade : int32
{
    // The span pitches to follow the line between station bases — rails,
    // hedges, most game fences.
    Racked = 0,
    // The span stays level at the LOWER of its two station altitudes, so the
    // height jumps at stations and no span floats off its downhill end —
    // brick walls, panel fences, castle walls on steep ground.
    Stepped = 1,
    // The span is SHEARED to the grade rather than rotated onto it: its
    // along-run axis follows the line between station bases while its up axis
    // stays with the stations, so a panel's moulded-in posts and pickets stand
    // plumb while its rails climb. Both ends meet their own station's altitude,
    // which is what Stepped cannot do — a level panel buries its uphill end and
    // steps against its neighbour.
    //
    // This is the joinery a real fence uses on a slope, and the trade builders
    // name "racking". The enumerator is not called that here because Racked is
    // already taken by the mode that rotates the whole piece, which leans the
    // posts with it.
    //
    // Right for a piece carrying vertical detail (panels, pickets, balusters);
    // wrong for one whose whole silhouette should follow the ground, which is
    // what Racked is for.
    Sheared = 2,
};

// What an override substitutes for the span it names.
enum class SplineSpanOverrideKind : uint8
{
    // An empty slot of the override table: it names no span.
    None = 0,
    // Draw from GatePool instead of SpanPool. An empty GatePool leaves the
    // opening empty, which is itself useful (a gap in a hedge).
    Gate = 1,
    // Pin this span to a chosen SpanPool slot, overriding the seeded pick.
    ExplicitPiece = 2,
};

// One authored "this span is a gate" / "this span is that specific piece".
// Addressed by (authored point, ordinal within that run's fill) rather than by
// distance, so it survives a knot drag. Overrides live in the RECIPE, not on
// the spline: the spline stays recipe-agnostic and no consumer of its layout
// grows a field.
//
// @ge-no-add
struct SplineSpanOverride
{
    uint32 PointIndex = 0;  // authored point opening the run
    uint16 SpanOrdinal = 0; // which span within the run's fill sequence
    SplineSpanOverrideKind Kind = SplineSpanOverrideKind::None;
    uint8 PoolSlot = 0; // ExplicitPiece: SpanPool slot; Gate: GatePool slot

    bool operator==(const SplineSpanOverride&) const = default;
};

static_assert(sizeof(SplineSpanOverride) == 8, "SplineSpanOverride must stay 8 bytes");
static_assert(std::is_trivially_copyable_v<SplineSpanOverride>);
static_assert(std::is_standard_layout_v<SplineSpanOverride>);

// Builds a post-and-span structure — fence, hedge, wall — along the sibling
// SplineComponent's curve. Sibling of SplinePlacement, not a mode inside it:
// the two families share the centerline machinery and almost no recipe fields.
//
// Pure authored data, trivially copyable, serialized by the scene; the
// editor-side SplineFenceController samples, conforms and spawns runtime-only
// piece entities. Authored points are mandatory STATIONS: a fence turns a
// corner by planting a post, never by bending a rail.
struct SplineFence
{
    // Piece pools — one mesh family per role. A pool's active entries are its
    // leading non-empty slots (SplinePoolSelection.h); everything after the
    // first empty slot is ignored and reported as validation.
    //
    // PostPool MAY BE EMPTY, and on the measured fence kits it usually is:
    // those kits ship no standalone post — the post is modelled into each
    // panel's ends. A station then emits no post mesh, the spans still
    // articulate there, and the panels' own end posts cover the corner.
    ModelRef PostPool[kSplineFencePoolCapacity];
    // Stretched between consecutive stations. Members need not share a length:
    // the run's fill computes how many instances span the distance.
    ModelRef SpanPool[kSplineFencePoolCapacity];
    // Replaces the span an override marks as a gate.
    ModelRef GatePool[kSplineFencePoolCapacity];
    // Laid along the tops of the spans at a fixed pitch — battlements, coping,
    // lamps. Whole on the pitch grid: the cell is sized to the pool's longest
    // piece and the piece is centred in it. Where the pitch is that length and
    // every wall of a run shares it, one piece stands on each wall and is
    // stretched with it. Empty means no crest row.
    ModelRef CrestPool[kSplineFencePoolCapacity];
    // End pieces offered to the gap a whole crest piece cannot fill, at a post,
    // a tower or a gate. The longest that fits closes it; empty leaves the gap,
    // which the post covers.
    ModelRef CapPool[kSplineFencePoolCapacity];
    // Target metres between stations within a run, along the draped
    // centerline. Runs bend it slightly so their stations land on both
    // authored endpoints — the trade FitToLength already makes for tiles.
    float32 PostPitch = 2.4f;
    // Metres between crest cells, measured from each run's opening authored
    // point so the row keeps its phase when the spline is edited elsewhere.
    // 0 takes the longest active crest piece's own length, which meets flush;
    // a smaller value is raised to it, because overlapping crest pieces are
    // never right.
    float32 CrestPitch = 0.0f;
    // Stations conform; spans derive their frame from the stations and cast no
    // rays of their own.
    SplinePlacementConform ConformMode = SplinePlacementConform::HeightAndSlope;
    // What the conform ray may land on. Scene (the default) is every pickable
    // surface; TerrainOnly makes scenery transparent so stations do not ride
    // the roofs of the props they pass under.
    //
    // A fence is more exposed to this than a tile path. A run's length is the
    // 3D length of its DRAPED polyline, so a prop overhanging the line does not
    // merely lift the stations beneath it: it lengthens the run, re-solves how
    // many spans fill it, and so moves every station in that run.
    SplineConformTarget ConformTarget = SplineConformTarget::Scene;
    // 0 = plumb posts (what a real fence does), 1 = posts lean into the slope.
    float32 SlopeBlend = 0.0f;
    // Cap on a span's length scale (and, reciprocally, its floor) before the
    // run re-walks with one more or one fewer station. Protects proportion and
    // silhouette: a 2x stretched plank reads instantly as wrong.
    float32 SpanMaxStretch = 1.25f;
    SplinePlantMode PlantMode = SplinePlantMode::PivotPlane;
    SplineSpanGrade SpanGrade = SplineSpanGrade::Racked;
    // Authored span exceptions: "this span is a gate", "this span is that
    // piece". Every slot whose Kind is not None is one override, in any order;
    // an untouched table writes nothing to the scene, and a filled slot writes
    // one line per member ("Overrides3.SpanOrdinal = 2").
    SplineSpanOverride Overrides[kSplineFenceMaxSpanOverrides];
    // Salts the deterministic per-station, per-span and per-crest-cell pool
    // picks; same Seed + same ordinal = same pick on every rebuild. Caps are
    // chosen by length, not by the seed.
    uint32 Seed = 0;
    // Replaces the material on every emitted post, span, crest piece and cap.
    // Null (the default) keeps each mesh's own embedded model material. Kit FBX
    // commonly embed a bare untextured Lambert, or name source textures the
    // project never imported, so the embedded material is often not the one
    // the scene wants; the override retextures a fence without touching the
    // shared meshes. A GUID the runtime material registry cannot resolve is
    // not bound: the binder logs once and keeps the embedded material rather
    // than letting render extraction drop the pieces.
    MaterialRef OverrideMaterial;

    // Memberwise (arrays element-wise): the rebuild scheduler compares whole
    // recipes, so a hand-written comparison that missed a field would silently
    // stop rebuilds for edits to that field. Defaulted == cannot miss one.
    //
    // A NaN float would make a recipe unequal to ITSELF and re-arm the settle
    // rebuild forever, so the controller compares sanitized recipes only —
    // SanitizeFenceRecipe (Placement/FenceLayout.h) is the single gate.
    bool operator==(const SplineFence&) const = default;
};

static_assert(std::is_trivially_copyable_v<SplineFence>,
              "SplineFence must be trivially copyable for ECS storage");
static_assert(std::is_standard_layout_v<SplineFence>,
              "SplineFence must be standard layout for ECS storage");

} // namespace GameEngine::Components
