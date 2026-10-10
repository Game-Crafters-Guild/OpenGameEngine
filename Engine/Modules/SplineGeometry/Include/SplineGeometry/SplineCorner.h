#pragma once

#include "Mathematics/Vector3.h"
#include "SplineGeometry/SplineStation.h"
#include "Types/Types.h"

#include <span>
#include <vector>

namespace GameEngine::SplineGeometry
{

// What a sweep builds where its run turns at an authored point.
enum class SplineCornerStyle : uint8
{
    // No corner: the stations stay where they were sampled and the sweep rounds
    // the turn over whatever spacing they have. The generic sweep's paths and
    // roads take this.
    None = 0,
    // One ring in the bisector plane, its lateral axis scaled by 1 / cos(θ/2) so
    // the faces of both legs meet on one edge, emitted twice (a crease) so each
    // leg keeps its own normals up to that edge.
    Mitre,
    // A fan of rings pivoting about the corner point from the incoming frame to
    // the outgoing one, ceil(θ / kRoundCornerStepDegrees) rings counting both
    // ends: the outside of the turn becomes an arc, the inside meets at the
    // inner mitre point. Past kMitreLimit that point runs away toward a
    // hairpin, so its reach back along each leg is bounded by what the legs
    // hold, and the inside face narrows toward the corner where they are short.
    Round,
};

// A mitre ring's lateral scale, 1 / cos(θ/2), at which a Mitre corner is built
// Round instead: 2.0 is a 120° turn. Past it the mitre point runs away toward
// infinity as the turn approaches a hairpin.
inline constexpr float32 kMitreLimit = 2.0f;
// The same limit as a turn: 2 · acos(1 / kMitreLimit).
inline constexpr float32 kMitreLimitTurnDegrees = 120.0f;

// A Round corner's ring count is ceil(θ / kRoundCornerStepDegrees), both ends
// included, so a 90° corner is 9 rings: on a 0.6 m wall that leaves about a
// millimetre of facet sagitta, and a finer fan costs rings nobody sees.
inline constexpr float32 kRoundCornerStepDegrees = 10.0f;
inline constexpr uint32 kMinRoundCornerRings = 2;
inline constexpr uint32 kMaxRoundCornerRings = 18;

// A station of the stream that stands on an authored corner point.
struct SplineCornerSite
{
    uint32 StationIndex = 0;
    // The authored point, so a report can name what the author sees.
    uint32 PointIndex = 0;
};

// What an author is told about a corner the sweep could not build as asked.
enum class SplineCornerIssueKind : uint8
{
    // Asked to be a Mitre but turning past kMitreLimit: built Round.
    PastMitreLimit = 0,
    // The leg between the corner and an open end of the run is shorter than
    // the corner's reach, so the inside face folds back past the end.
    LegShorterThanReach,
    // This corner and the next (OtherPointIndex) turn the same way closer
    // together than the sum of their reaches, so their inside faces fold
    // through each other on the leg between them.
    CornersOverlap,
    // Between points PointIndex and OtherPointIndex the curve bends tighter than
    // the wall's inside half-width, so its inside face folds through itself.
    InnerFold,
};

struct SplineCornerIssue
{
    SplineCornerIssueKind Kind = SplineCornerIssueKind::PastMitreLimit;
    uint32 PointIndex = 0;
    uint32 OtherPointIndex = 0;
    float32 TurnDegrees = 0.0f;
};

// Give the stream's corners the shape `style` asks for, in place.
//
// Each site's station is replaced by the rings of its corner (a crease pair for a
// Mitre, a fan for a Round), and the stations within the corner's reach on
// either leg are removed: the inside faces of the two legs meet at the inner
// mitre point, halfWidth · tan(θ/2) before the corner along each leg, and a ring
// sampled between that point and the corner would push the inside face past it
// and fold it back through itself. A ring sampled on the point itself, or
// within a few centimetres beyond it, is removed too: its band to the corner
// would have next to no length on the inside face and still carry its U.
//
// Directions are read from the neighbouring stations, so the stream must already
// carry the draped positions; `up` is the frame's vertical, in the stations'
// space, and defines the plane the turn is measured in.
//
// A closed loop (`closedLoop`, its stream closed by CloseStationLoop) may name
// station 0 as a site: its corner is then split across the seam, the outgoing
// half opening the stream and the incoming half closing it, so the first and
// last rings still coincide.
//
// Sites must be in ascending station order. Returns what the author should be
// told: corners built Round because they turned past the mitre limit, and
// corners whose inside face still folds because a leg is shorter than their
// reach (the stream ends, or the next corner turns the same way, inside it).
[[nodiscard]] std::vector<SplineCornerIssue> ApplyCorners(
    std::vector<SplineStripStation>& stations, std::span<const SplineCornerSite> sites,
    SplineCornerStyle style, const Mathematics::Vector3& up, bool closedLoop);

// Give every station the length its side faces have run beyond the centreline
// (SplineStripStation::FaceTurnLeft/Right), so each face takes U along its own
// length: the turn between consecutive stations' forwards in the plane `up`
// defines, and at each corner ApplyCorners built, the extra length of its outer
// face and the shortfall of its inner one. It also gives every station the top's
// arc allowance (SplineStripStation::TopArcAllowance): the arc each round
// corner's outer top edge sweeps, carried down the run so the legs after it keep
// square joints. A stream that opens on a corner ring (a welded loop's seam)
// starts both at zero there.
//
// Run it after ApplyCorners, whose rings carry their turn relative to the leg
// before them (SplineStripStation::CornerTurnLeft/Right). It reads only those and
// the forwards, so running it again gives the same result, and a stream it never
// ran on keeps the centreline U on every face.
void AccumulateFaceTurn(std::span<SplineStripStation> stations, const Mathematics::Vector3& up);

// The stations where a curve bends tighter than the wall is wide: the plan
// radius of the circle through the station and its two neighbours is smaller
// than the half-width on the inside of the bend, so the inside face folds back
// through itself there. Stations flagged in `skip` (a corner and the stations
// either side of it, which a corner reshapes) are not measured. `up` defines the
// plane the bend is measured in, as for ApplyCorners. Returns station indices,
// ascending.
[[nodiscard]] std::vector<uint32> FindInnerFolds(std::span<const SplineStripStation> stations,
                                                 std::span<const uint8> skip,
                                                 const Mathematics::Vector3& up);

// Close a sampled loop on itself: the stream's last station is the loop's start
// reached again (the caller samples it there), and this makes the two the same
// station in every field but Distance — the first ring's frame looks along the
// loop across the seam rather than one-sidedly ahead, and the last ring repeats
// it exactly — so the sweep's two end rings coincide and a mesh split into
// chunks meets itself the way its chunks meet each other.
//
// `up` is the frame's vertical, as for ApplyCorners. A stream of fewer than three
// stations encloses nothing and is left untouched.
void CloseStationLoop(std::span<SplineStripStation> stations, const Mathematics::Vector3& up);

} // namespace GameEngine::SplineGeometry
