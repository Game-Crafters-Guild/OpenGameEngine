#pragma once

#include "Components/Spline/SplineFence.h"
#include "Mathematics/Vector3.h"
#include "SplineLayout/PieceAxis.h"
#include "SplineLayout/TileLayout.h"
#include "Types/Types.h"

#include <span>
#include <string>
#include <vector>

namespace GameEngine::SplineLayout
{

// Local bounds of one pool piece, as the fence layout needs them: the pivot
// compensation and the authored length along the path.
struct FencePieceBounds
{
    Mathematics::Vector3 Center{0.0f, 0.0f, 0.0f};
    Mathematics::Vector3 HalfExtents{0.0f, 0.0f, 0.0f};

    // Derived from the extents, never stored alongside them: an axis a caller
    // could set independently is an axis that can contradict the bounds it
    // describes, and the fill would then count one axis while the pose laid the
    // piece along the other.
    PieceAxis Axis() const { return ChoosePieceAxis(HalfExtents); }
    // Authored length along the direction of travel.
    float32 Length() const { return PieceLength(HalfExtents, Axis()); }
    // Half the piece's extent across travel: how far each side face stands
    // off the path it is laid on.
    float32 HalfThickness() const { return Axis() == PieceAxis::X ? HalfExtents.z : HalfExtents.x; }
    // Authored height, the cover a station has to offer a stepped grade.
    float32 Height() const { return HalfExtents.y * 2.0f; }
    // Depth of the piece's lowest geometry below its own pivot, as a POSITIVE
    // magnitude — the planting skirt a kit models to sit under the ground line
    // (measured fence panels ~0.52 m, castle towers 2.50 m). Negative for a
    // piece whose bounds stop above its pivot.
    //
    // One number, two readers: SplinePlantMode::BoundsMin lifts a pose by
    // exactly this to stand the mesh on the surface, and the bury check adds it
    // to the plant line as the ground rise a span is allowed before it counts
    // as buried. Deriving both from here is the point — a plant that lifted by
    // one number while the check allowed another would drift in silence. It is
    // read per piece because that is where the geometry is, not because pieces
    // disagree by much: one measured fence pool carries 0.5216 m on 65 pieces
    // and 0.5239 m on 42, a spread too small to change any verdict.
    float32 SkirtDepth() const { return HalfExtents.y - Center.y; }
};

// A station is a post position whether or not a post mesh covers it: the
// measured fence kits ship no standalone post, so an empty PostPool still
// articulates its spans here.
struct FenceStation
{
    TilePose Pose;
    // Global ordinal along the whole spline — the pool-selection index. Runs
    // and indices-within-runs are equally deterministic but reshuffle the same
    // way downstream of an edited knot, and the global ordinal is the simpler
    // contract to test.
    uint32 Index = 0;
    // Authored control point this station coincides with; kNoAuthoredPoint for
    // a station the pitch fill placed between two authored points.
    uint32 AuthoredPoint = 0;
    bool IsAuthoredPoint = false;
};

// Where one end of a span — or of a crest registered to it — is cut: on the
// vertical plane through its station's base, perpendicular to the ground
// bisector of the two spans that meet there (SpanMitre.h, MeasureSpanJoin).
// Two equally thick pieces laid on the chords of a turn meet on it on both
// faces; a thinner piece meets a thicker one with the step their thicknesses
// make (fence design section 4c). Described in the piece's own frame, because
// the cut variant a piece draws is a function of exactly these numbers and its
// mesh.
struct FenceEndPlane
{
    // False keeps the end square as modelled: no span beyond it, a post over
    // the join, a notch under a millimetre, a turn past the mitre limit, or a
    // span that borrowed its roll.
    bool Mitred = false;
    // The plane's yaw away from square in the piece's frame, in radians: on
    // level ground the signed turn at the station, positive where the fence
    // turns right (SignedGroundYaw).
    float32 TurnRadians = 0.0f;
    // The plane's tilt about the piece's Right, in radians: a Racked span's
    // own pitch; zero on level ground and for Stepped and Sheared spans, whose
    // frames stay upright.
    float32 LeanRadians = 0.0f;
    // How far inside the piece's modelled end the plane crosses the span's
    // base line, along the piece's own axis in its unscaled local metres: at
    // the station, or straight below or above it for a stepped span.
    float32 StationInset = 0.0f;
    // The same at the piece's top (its bounds' highest local Y). Differs from
    // StationInset by the lean over the piece's height; the cut variant is
    // keyed on these two insets rather than on the lean angle, so rounding
    // moves the cut by a fixed distance at any height.
    float32 TopInset = 0.0f;
    // The span's base line's height in the piece's local Y: minus the planted
    // lift for a span, minus the wall top for a crest standing on it. Only a
    // leaning plane reads it.
    float32 StationLocalY = 0.0f;
};

struct FenceSpan
{
    TilePose Pose;
    // Scale along the piece's own along-path axis — the one ChoosePieceAxis
    // picked and FencePieceBounds::Length() measured, which the emission lays
    // on the pose's Forward. 1 = the piece's authored length.
    float32 LengthScale = 1.0f;
    // Active pool slot this span draws: a SpanPool slot, or a GatePool slot
    // when IsGate.
    uint32 PoolSlot = 0;
    // An override made this span a gate, drawn from GatePieces. A gate is laid
    // between its two stations exactly as a wall is, reserves its span of the
    // crest row, and is mitred where it meets a span no post covers.
    bool IsGate = false;
    // Global span ordinal. An opening — a gate with no gate piece to draw —
    // places no span but keeps its ordinal, so the ordinals after it do not
    // move when the gate pool is filled or emptied.
    uint32 Index = 0;
    uint32 Run = 0;          // authored point opening the run
    uint32 OrdinalInRun = 0; // position within the run's fill sequence
    // The planes the span's two ends are cut on where it meets a neighbour no
    // post covers. A mitred end was laid longer by its overhang, which
    // LengthScale already counts; square ends leave the span exactly as it
    // was laid between its stations.
    FenceEndPlane Start;
    FenceEndPlane End;
};

// One piece of the crest row — the row laid along the top of the spans:
// battlements, coping, lamps — or the end piece that closes a remainder beside
// a post, a tower or a gate.
//
// Two rules lay the row, one per run. On the pitch grid a crest piece is whole:
// the cell is sized to the pool's longest piece and the piece is centred in it.
// Where the crest cell and every span piece of a run share one nominal length,
// the row is registered to the spans instead — one crest per span, in that
// span's frame and at that span's LengthScale — because walls laid on the
// chords of a curve are stretched to close the run, and a whole piece on a
// stretched wall stands off the wall beside it.
struct FenceCrest
{
    TilePose Pose;
    // Scale along the piece's own along-path axis, as FenceSpan::LengthScale.
    // Exactly 1 on the pitch grid and for every cap; a crest registered to its
    // span carries that span's scale, so its two ends are the span's two ends.
    float32 LengthScale = 1.0f;
    // Active CrestPieces slot, or the active CapPieces slot when IsCap.
    uint32 PoolSlot = 0;
    // Drawn from CapPieces to close a remainder rather than from CrestPieces to
    // fill a cell. Chosen by length and not by the seed: a remainder is a fit.
    bool IsCap = false;
    // Global cell ordinal along the whole spline, counted BEFORE reservations
    // removed any — the rule TilePose::StationIndex states for dropout. A tower
    // or a gate therefore removes cells without renumbering the survivors, and
    // a cap carries the ordinal of the cell it stands in.
    uint32 Index = 0;
    uint32 Run = 0;          // authored point opening the run
    uint32 OrdinalInRun = 0; // cell ordinal within the run, likewise pre-removal
    // A crest registered to its span takes that span's two planes, in its own
    // frame: its ends are the span's ends, so it closes where the wall does.
    // Square on the pitch grid and for every cap, which never reach a join.
    FenceEndPlane Start;
    FenceEndPlane End;
};

// A stretch of fence that a piece the CALLER planted already occupies: the
// crest cells whose footprint it covers are removed, never shifted. The posts
// and gates the recipe plants reserve themselves and need no entry here.
//
// Measured from an authored point rather than from the spline start, because
// that is the frame that survives an edit: an authored point stays where the
// author put it, while a distance from the start of the spline moves under
// every upstream knot. The interval is SIGNED about that point and runs along
// the fence through it, so one entry addresses both of the runs a node joins:
// {i, -h, +h} is a tower of half-length h planted at authored point i, and
// {i, -h, 0} covers only the run that closes on it. Its metres run along the
// spans, where the crest row stands, not along the draped ground under them.
struct FenceReservation
{
    // Authored point the interval is measured from. FenceSpan::Run names the
    // same points: a run is addressed by the one it opens at.
    uint32 Point = 0;
    // Negative reaches back into the run closing on the point, positive forward
    // into the run opening at it. Ascending, and a zero-length pair reserves
    // nothing.
    float32 StartMetres = 0.0f;
    float32 EndMetres = 0.0f;
};

struct FenceLayoutParams
{
    // Where the authored points fall in the draped centerline, as ascending
    // fractional sample indices: one per authored point, plus the closing
    // boundary. Two entries describe one run.
    std::span<const float32> RunBoundaries;
    // A closed spline's last boundary addresses the same physical station as
    // its first: the station is emitted once and yawed by the bisector of both
    // adjoining runs, exactly like every interior corner.
    bool Closed = false;

    float32 PostPitch = 2.4f;
    float32 SpanMaxStretch = 1.25f;
    uint32 Seed = 0;
    Components::SplineSpanGrade SpanGrade = Components::SplineSpanGrade::Racked;
    Components::SplinePlantMode PlantMode = Components::SplinePlantMode::PivotPlane;
    bool AlignToSurfaceNormal = false;
    float32 SlopeBlend = 0.0f;
    // Ceiling on how far a STATION may lean from world up — a post rests on
    // ground exactly as a tile does, so it answers to the tile recipe's
    // constant. It says nothing about spans: a span's pitch follows the grade
    // it climbs, which is what SplineSpanGrade::Racked means, and the rule that
    // catches a span standing on end is a degeneracy threshold near vertical,
    // not a lean ceiling (FenceLayout.cpp).
    float32 MaxTiltDegrees = Components::kDefaultSplineMaxTiltDegrees;
    // Optional: counts the stations the ceiling held, for the caller's report.
    TiltClampReport* OutTiltClamp = nullptr;

    // Station geometry. HasPostMesh false leaves PostPiece at zero, which puts
    // every station pose exactly on the draped centerline.
    FencePieceBounds PostPiece{};
    bool HasPostMesh = false;

    // One entry per ACTIVE SpanPool slot, in slot order. Empty = no spans; the
    // stations are then walked at the pitch and nothing is stretched.
    std::span<const FencePieceBounds> SpanPieces;
    // One entry per ACTIVE GatePool slot, in slot order: what a gate override
    // draws. Empty leaves every gate an opening — the room of the wall it
    // replaced, with nothing in it.
    std::span<const FencePieceBounds> GatePieces;
    // The recipe's span overrides (SplineFence::Overrides), each naming a span
    // by the authored point opening its run and its ordinal in that run's
    // fill. A gate draws its GatePieces slot at its own length, and the run's
    // one shared stretch closes around it; a pinned span draws its SpanPieces
    // slot in place of the seeded pick. Slots whose Kind is None are empty.
    // With no override the result is byte-identical to one built without this.
    std::span<const Components::SplineSpanOverride> SpanOverrides;

    // One entry per ACTIVE CrestPool slot, in slot order. Empty = no crest row,
    // and the result is then byte-identical to one built with none of it.
    std::span<const FencePieceBounds> CrestPieces;
    // One entry per ACTIVE CapPool slot. End pieces offered to the remainder
    // each run leaves beside a post, a tower or a gate; the longest that fits
    // closes it. Empty leaves the remainder bare, which the post covers.
    std::span<const FencePieceBounds> CapPieces;
    // Metres between crest cells. 0 takes the longest active crest piece's own
    // length, so the row meets flush; a smaller value is raised to it and
    // reported, because overlapping crest pieces are never right. At that
    // length a run whose span pieces all share it is registered to its spans
    // instead, one crest per span (FenceCrest); a larger value asks for air
    // between the pieces and keeps the pitch.
    float32 CrestPitch = 0.0f;
    // Stretches of run the caller's own pieces already occupy. Sorted and
    // merged once per rebuild; a zero-length one reserves nothing.
    std::span<const FenceReservation> Reservations;
    // Optional shared facing per authored point, indexed by authored point:
    // a node several splines meet at faces one way in every one of their
    // layouts. A zero or unusable entry keeps the recipe's own bisector.
    std::span<const Mathematics::Vector3> AuthoredPointForwards;

    uint32 MaxPieces = 4096;
    // Re-reads the true surface under each station. Empty leaves stations on
    // the interpolated centerline.
    StationSurfaceProbe Probe;
    // Optional compression floor, independent of SpanMaxStretch. A finite value
    // in (0, 1] allows runtime kits to admit short connectors without stretching
    // long panels. Values above 1 clamp to 1; non-positive/non-finite values keep
    // the legacy reciprocal floor (1 / SpanMaxStretch).
    float32 SpanMinScale = 0.0f;
};

struct FenceLayoutResult
{
    std::vector<FenceStation> Stations;
    std::vector<FenceSpan> Spans;
    // The crest row and its caps, in run order and then along each run. Emitted
    // after every station and span: one piece budget, and decoration is what it
    // cuts first.
    std::vector<FenceCrest> Crests;
    // How many spans each run's fill holds, openings included, indexed by the
    // authored point opening the run; 0 for a run that was skipped. An editor
    // that re-addresses span overrides after a point edit reads it to keep an
    // override on the same place along the wall.
    std::vector<uint32> RunSpanCounts;
    // Author-facing problems: unsatisfiable stretch, zero-length runs, buried
    // spans, stepped risers taller than their cover. Never a silent drop.
    std::vector<std::string> Validation;
    // Spans whose travel direction stood too near vertical to carry a roll and
    // so borrowed their opening station's frame. Not an error — it is the
    // defined rule for that geometry — but it means those spans no longer join
    // their two posts, which the author is owed.
    uint32 RollBorrowedSpans = 0;
};

// Walks one run per authored segment, plants a station at every authored point
// and at the pitch fill between them, derives each span's frame from the two
// stations it joins, and lays the crest row along the tops of those spans.
//
// Gap-hold runs on the WHOLE centerline before it is split into runs: per-run
// hold would let a run starting inside a terrain hole re-anchor to a different
// altitude than its neighbour ends on.
FenceLayoutResult BuildFenceLayout(const std::vector<CenterSample>& center,
                                   const FenceLayoutParams& params);

// A NaN in an authored float makes the recipe unequal to ITSELF under the
// defaulted operator==, which would re-arm the settle rebuild every frame for
// as long as the field stayed poisoned. Every consumer therefore reads the
// recipe through here, and the controller stores the sanitized value, so the
// value it compares can never be self-unequal.
//
// Non-finite floats fall back to the authored default (an infinity carries no
// intent to clamp); finite ones outside their domain are clamped into it.
Components::SplineFence SanitizeFenceRecipe(const Components::SplineFence& authored);

} // namespace GameEngine::SplineLayout
