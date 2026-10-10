#include "SplineGeometry/SplineCorner.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace GameEngine::SplineGeometry
{
namespace
{

using V3 = Mathematics::Vector3;

constexpr float32 kPi = 3.14159265358979323846f;
constexpr float32 kDegreesPerRadian = 180.0f / kPi;

// Relative slack on kMitreLimit, so a corner authored at exactly the limit
// (whose float cos(60°) makes the scale 2.0000001) is built as the Mitre the
// limit promises; about 0.007° of turn.
constexpr float32 kMitreLimitTolerance = 1.0e-4f;

// Below this turn the two legs are one straight run and there is no corner to
// build: the crease would split the normals of two coplanar faces.
constexpr float32 kMinCornerTurnRadians = 1.0e-3f;

// Nearest a neighbouring station may stand to the corner and still name its
// leg's direction. A sample a millimetre from the corner carries its position's
// rounding in its direction; this far away it does not.
constexpr float32 kMinLegProbeMetres = 0.05f;

[[nodiscard]] V3 NormalizedOr(const V3& v, const V3& fallback)
{
    const float32 lengthSquared = V3::Dot(v, v);
    if (!std::isfinite(lengthSquared) || lengthSquared < 1.0e-12f)
        return fallback;
    return v * (1.0f / std::sqrt(lengthSquared));
}

[[nodiscard]] V3 InPlane(const V3& v, const V3& up)
{
    return v - up * V3::Dot(v, up);
}

// The sweep's frame convention: Right is the right of travel in the plane the
// turn is measured in (Cross(up, forward) in this left-handed, +Y-up engine),
// and Up completes the basis, so a forward that climbs tilts Up with it.
struct Frame
{
    V3 Forward;
    V3 Right;
    V3 Up;
};

[[nodiscard]] Frame FrameAlong(const V3& forward, const V3& up)
{
    Frame frame;
    frame.Forward = forward;
    frame.Right = NormalizedOr(V3::Cross(up, InPlane(forward, up)), V3{1.0f, 0.0f, 0.0f});
    frame.Up = V3::Cross(frame.Forward, frame.Right);
    return frame;
}

// Below this a turn between two stations' forwards is the rounding of forwards
// taken from sampled positions, not a bend: about 2e-7 radians on a straight run
// at the 0.5 m drape step. A real bend this gentle has a radius past 50 km.
constexpr float32 kMinFaceTurnRadians = 1.0e-5f;

// The turn from `from` to `to` in radians, measured in the plane `up` defines:
// positive toward the right of travel, and zero below kMinFaceTurnRadians, so a
// straight run gains no face turn at all.
[[nodiscard]] float32 PlanTurn(const V3& from, const V3& to, const V3& up)
{
    const V3 a = InPlane(from, up);
    const V3 b = InPlane(to, up);
    const float32 turn = std::atan2(V3::Dot(V3::Cross(a, b), up), V3::Dot(a, b));
    return std::abs(turn) < kMinFaceTurnRadians ? 0.0f : turn;
}

// Rise per metre of plan travel along a direction.
[[nodiscard]] float32 Grade(const V3& direction, const V3& up)
{
    const V3 plan = InPlane(direction, up);
    const float32 planLength = std::sqrt(V3::Dot(plan, plan));
    return planLength > 1.0e-6f ? V3::Dot(direction, up) / planLength : 0.0f;
}

// One corner, measured once from the stream as sampled.
struct CornerTurn
{
    V3 In;       // direction of the incoming leg
    V3 Out;      // direction of the outgoing leg
    V3 InPlan;   // the same, in the turn's plane, unit length
    V3 OutPlan;
    float32 Turn = 0.0f;   // radians, 0 = straight on, π = a hairpin
    bool RightTurn = true;
    bool BuildRound = false;
    bool PastMitreLimit = false;
    bool Fallback = false; // a Mitre past kMitreLimit
    // Reach shortened to what the legs hold (BoundReachPastTheLimit), so the
    // inside face narrows toward the corner instead of folding past a leg.
    bool Bounded = false;
    // Lateral axis placing the faces of both legs on one edge: the bisector's
    // right scaled by 1 / cos(θ/2), which puts the inside at the inner mitre
    // point. A bounded reach scales it shorter.
    V3 MitreRight{};
    // How far before and after the corner, along each leg, the inside faces
    // meet: the inside half-width times tan(θ/2).
    float32 Reach = 0.0f;
    // The same per metre of half-width: how much shorter the inside face runs
    // than the centreline over each leg's last stretch into the corner.
    float32 ReachPerMetre = 0.0f;
};

// The station a leg's direction is read from: the nearest one at least
// kMinLegProbeMetres of run away from the corner, walking `step` (-1 back, +1
// ahead) and wrapping round a closed loop.
[[nodiscard]] uint32 LegProbe(std::span<const SplineStripStation> stations, uint32 corner,
                              int32 step, bool closedLoop)
{
    const uint32 count = static_cast<uint32>(stations.size());
    // On a closed loop the stream's last station IS its first, so walking back
    // from station 0 starts at the end, whose Distance is the loop's length.
    const bool wrapsBack = closedLoop && corner == 0u && step < 0;
    const float32 cornerDistance = wrapsBack ? stations[count - 1u].Distance
                                             : stations[corner].Distance;
    int32 index = wrapsBack ? static_cast<int32>(count) - 1 : static_cast<int32>(corner);
    const int32 last = static_cast<int32>(count) - 1;
    for (;;)
    {
        const int32 next = index + step;
        if (next < 0 || next > last)
            return static_cast<uint32>(index);
        index = next;
        if (std::abs(stations[static_cast<uint32>(index)].Distance - cornerDistance) >=
            kMinLegProbeMetres)
            return static_cast<uint32>(index);
    }
}

[[nodiscard]] CornerTurn MeasureCorner(std::span<const SplineStripStation> stations,
                                       uint32 corner, SplineCornerStyle style, const V3& up,
                                       bool closedLoop)
{
    const SplineStripStation& here = stations[corner];
    const SplineStripStation& before = stations[LegProbe(stations, corner, -1, closedLoop)];
    const SplineStripStation& after = stations[LegProbe(stations, corner, +1, closedLoop)];

    CornerTurn turn;
    turn.In = NormalizedOr(here.Position - before.Position, here.Forward);
    turn.Out = NormalizedOr(after.Position - here.Position, here.Forward);
    turn.InPlan = NormalizedOr(InPlane(turn.In, up), V3{0.0f, 0.0f, 1.0f});
    turn.OutPlan = NormalizedOr(InPlane(turn.Out, up), turn.InPlan);
    turn.Turn = std::acos(std::clamp(V3::Dot(turn.InPlan, turn.OutPlan), -1.0f, 1.0f));
    // Cross(in, out) points along +up for a turn toward the right of travel.
    turn.RightTurn = V3::Dot(V3::Cross(turn.InPlan, turn.OutPlan), up) >= 0.0f;

    const float32 halfTurnCosine = std::cos(turn.Turn * 0.5f);
    const float32 mitreScale =
        halfTurnCosine > 1.0e-6f ? 1.0f / halfTurnCosine : std::numeric_limits<float32>::max();
    // Past the limit a Mitre is built Round, and the inner mitre point runs
    // away toward a hairpin: ApplyCorners bounds its reach by the legs.
    turn.PastMitreLimit = mitreScale > kMitreLimit * (1.0f + kMitreLimitTolerance);
    turn.Fallback = style == SplineCornerStyle::Mitre && turn.PastMitreLimit;
    turn.BuildRound = style == SplineCornerStyle::Round || turn.Fallback;

    const V3 bisector = NormalizedOr(turn.InPlan + turn.OutPlan, turn.InPlan);
    turn.MitreRight = NormalizedOr(V3::Cross(up, bisector), V3{1.0f, 0.0f, 0.0f}) * mitreScale;
    const float32 insideHalfWidth = turn.RightTurn ? here.HalfWidthRight : here.HalfWidthLeft;
    turn.ReachPerMetre = std::tan(turn.Turn * 0.5f);
    turn.Reach = std::max(0.0f, insideHalfWidth) * turn.ReachPerMetre;
    return turn;
}

// Stamp a corner ring with its face turn relative to the leg before the corner
// (AccumulateFaceTurn makes it cumulative): `outsideTurn` on the side the corner
// turns away from, and the inside face's shortfall on the side it turns toward.
void StampCornerFaceTurn(SplineStripStation& ring, const CornerTurn& turn, float32 outsideTurn)
{
    const float32 inside = -turn.ReachPerMetre;
    ring.CornerTurnLeft = turn.RightTurn ? outsideTurn : inside;
    ring.CornerTurnRight = turn.RightTurn ? inside : outsideTurn;
}

// The ring every corner ring starts from: the corner station itself, so width,
// roll and distance carry over, placed with the corner's shared vertical.
[[nodiscard]] SplineStripStation CornerRing(const SplineStripStation& corner, const Frame& frame,
                                            const V3& cornerUp)
{
    SplineStripStation ring = corner;
    ring.Forward = frame.Forward;
    ring.Right = frame.Right;
    // One vertical for every ring of the corner, so rings that share a position
    // share every vertex position: the legs' own Up differ when they climb at
    // different grades.
    ring.Up = cornerUp;
    return ring;
}

// A Mitre: the corner ring twice, once shaded as the incoming leg and once as
// the outgoing one, at the same positions.
void AppendMitre(std::vector<SplineStripStation>& out, const SplineStripStation& corner,
                 const CornerTurn& turn, const V3& up, bool incomingHalf, bool outgoingHalf)
{
    const Frame in = FrameAlong(turn.In, up);
    const Frame outgoing = FrameAlong(turn.Out, up);
    const V3 cornerUp = NormalizedOr(in.Up + outgoing.Up, up);

    SplineStripStation incoming = CornerRing(corner, in, cornerUp);
    incoming.MitreRight = turn.MitreRight;
    incoming.MitreSides = kSplineMitreBothSides;
    // The outer face reaches the mitre edge tan(θ/2) per metre of half-width
    // beyond the centreline, and the inner one falls as far short of it.
    StampCornerFaceTurn(incoming, turn, turn.ReachPerMetre);
    SplineStripStation leaving = CornerRing(corner, outgoing, cornerUp);
    leaving.MitreRight = turn.MitreRight;
    leaving.MitreSides = kSplineMitreBothSides;
    StampCornerFaceTurn(leaving, turn, turn.ReachPerMetre);

    if (incomingHalf)
        out.push_back(incoming);
    if (outgoingHalf)
    {
        // Straight after the incoming ring it is a crease; opening a stream (the
        // seam of a closed loop) there is nothing before it to crease against.
        leaving.Join = incomingHalf ? SplineStationJoin::Crease : corner.Join;
        out.push_back(leaving);
    }
}

// A turn measured from sampled positions lands a few ULPs off an authored
// whole number of steps (150° reads as 150.00002°); this much of a ring is not
// worth another one.
constexpr float32 kRoundCornerRingTolerance = 1.0e-3f;

// Intervals of a Round corner's fan: one fewer than its rings.
[[nodiscard]] uint32 RoundSegments(const CornerTurn& turn)
{
    const float32 steps = turn.Turn * kDegreesPerRadian / kRoundCornerStepDegrees;
    const uint32 rings =
        std::clamp(static_cast<uint32>(std::ceil(steps - kRoundCornerRingTolerance)),
                   kMinRoundCornerRings, kMaxRoundCornerRings);
    return rings - 1u;
}

// A Round: a fan of rings turning about the corner point. The outside of the
// turn sweeps an arc of the wall's own half-width; the inside stays on the inner
// mitre point, bounded by the legs past the mitre limit.
void AppendRound(std::vector<SplineStripStation>& out, const SplineStripStation& corner,
                 const CornerTurn& turn, const V3& up, uint32 firstRing)
{
    const Frame in = FrameAlong(turn.In, up);
    const Frame outgoing = FrameAlong(turn.Out, up);
    const V3 cornerUp = NormalizedOr(in.Up + outgoing.Up, up);

    const uint32 segments = RoundSegments(turn);
    // Toward the inside of the turn, perpendicular to the incoming leg.
    const V3 rightOfTravel = NormalizedOr(V3::Cross(up, turn.InPlan), V3{1.0f, 0.0f, 0.0f});
    const V3 towardTurn = turn.RightTurn ? rightOfTravel : rightOfTravel * -1.0f;
    const float32 gradeIn = Grade(turn.In, up);
    const float32 gradeOut = Grade(turn.Out, up);
    const uint8 insideSide = turn.RightTurn ? kSplineMitreRightSide : kSplineMitreLeftSide;

    for (uint32 i = firstRing; i <= segments; ++i)
    {
        V3 forward;
        if (i == 0u)
        {
            forward = turn.In;
        }
        else if (i == segments)
        {
            forward = turn.Out;
        }
        else
        {
            const float32 fraction = static_cast<float32>(i) / static_cast<float32>(segments);
            const float32 angle = turn.Turn * fraction;
            const V3 plan = turn.InPlan * std::cos(angle) + towardTurn * std::sin(angle);
            const float32 grade = gradeIn + (gradeOut - gradeIn) * fraction;
            forward = NormalizedOr(plan + up * grade, plan);
        }
        SplineStripStation ring = CornerRing(corner, FrameAlong(forward, up), cornerUp);
        ring.MitreRight = turn.MitreRight;
        ring.MitreSides = insideSide;
        // The outside sweeps an arc of the half-width: radians of turn so far.
        StampCornerFaceTurn(ring, turn,
                            turn.Turn * static_cast<float32>(i) / static_cast<float32>(segments));
        ring.Join = (i == firstRing) ? corner.Join : SplineStationJoin::Pivot;
        out.push_back(ring);
    }
}

// Shortest band the inside face keeps before (and after) the inner mitre
// point. A station at the reach puts its inside vertex on that point, and one a
// hair beyond it — float rounding of the reach, or the arc-length table placing
// a sample a centimetre off along the run — leaves a band too short to see that
// still carries the station's whole U span, a cut in the pattern. Such a
// station is dropped with the stations inside the reach.
constexpr float32 kMinInsideBandMetres = 0.05f;

// Mark the stations between a corner and the inner mitre point on each leg,
// and those within kMinInsideBandMetres beyond it. Scans stop at another corner and never
// take a stream end.
void MarkWithinReach(std::span<const SplineStripStation> stations, uint32 corner, float32 reach,
                     bool closedLoop, const std::vector<uint8>& isSite, std::vector<uint8>& drop)
{
    if (reach <= 0.0f)
        return;
    reach += kMinInsideBandMetres;
    const uint32 count = static_cast<uint32>(stations.size());
    const float32 ahead = stations[corner].Distance;
    for (uint32 i = corner + 1u; i + 1u < count; ++i)
    {
        if (isSite[i] || stations[i].Distance - ahead > reach)
            break;
        drop[i] = 1u;
    }
    const bool wrapsBack = closedLoop && corner == 0u;
    const float32 behind = wrapsBack ? stations[count - 1u].Distance : ahead;
    const uint32 start = wrapsBack ? count - 1u : corner;
    for (uint32 i = start; i-- > 1u;)
    {
        if (isSite[i] || behind - stations[i].Distance > reach)
            break;
        drop[i] = 1u;
    }
}

// Bound each corner past the mitre limit to the reach its legs hold. Its inner
// mitre point stands halfWidth · tan(θ/2) back along each leg, which runs away
// toward a hairpin; where a leg is shorter than that, the inside point moves in
// along the bisector until it stands only as far back as the shorter leg allows,
// and the inside face narrows toward the corner rather than folding back past
// the leg. A leg holds its length less the band the inside face keeps, and
// toward a corner that turns the same way, less that corner's reach too (one
// turning the other way keeps its inside on the leg's other face).
void BoundReachPastTheLimit(std::span<const SplineStripStation> stations,
                            std::span<CornerTurn> turns, std::span<const SplineCornerSite> sites,
                            bool closedLoop)
{
    const size_t count = turns.size();
    const float32 runStart = stations.front().Distance;
    const float32 runEnd = stations.back().Distance;
    const float32 loopLength = runEnd - runStart;
    const auto at = [&](size_t t) { return stations[sites[t].StationIndex].Distance; };
    // Room on the leg from corner t toward its neighbour `step` away (-1 back,
    // +1 ahead): the neighbouring corner, or the open end.
    const auto room = [&](size_t t, int32 step) -> float32
    {
        const bool hasNeighbour = closedLoop ? count > 1u : (step < 0 ? t > 0u : t + 1u < count);
        if (!hasNeighbour)
        {
            if (closedLoop)
                return 0.5f * loopLength - kMinInsideBandMetres;
            const float32 leg = step < 0 ? at(t) - runStart : runEnd - at(t);
            return leg - kMinInsideBandMetres;
        }
        const size_t other = step < 0 ? (t + count - 1u) % count : (t + 1u) % count;
        float32 leg = step < 0 ? at(t) - at(other) : at(other) - at(t);
        if (leg < 0.0f)
            leg += loopLength;
        const float32 shared =
            turns[other].RightTurn == turns[t].RightTurn ? turns[other].Reach : 0.0f;
        return leg - shared - kMinInsideBandMetres;
    };
    for (size_t t = 0; t < count; ++t)
    {
        CornerTurn& turn = turns[t];
        if (!turn.PastMitreLimit)
            continue;
        const float32 held = std::max(0.0f, std::min(room(t, -1), room(t, +1)));
        if (turn.Reach <= held)
            continue;
        const SplineStripStation& here = stations[sites[t].StationIndex];
        const float32 insideHalfWidth = turn.RightTurn ? here.HalfWidthRight : here.HalfWidthLeft;
        const float32 halfTurnSine = std::sin(turn.Turn * 0.5f);
        if (!(insideHalfWidth > 0.0f) || !(halfTurnSine > 0.0f))
            continue;
        turn.Reach = held;
        turn.ReachPerMetre = held / insideHalfWidth;
        // A point s · halfWidth out along the bisector's right stands
        // s · halfWidth · sin(θ/2) back along each leg.
        const float32 scale = held / (insideHalfWidth * halfTurnSine);
        turn.MitreRight = NormalizedOr(turn.MitreRight, V3{1.0f, 0.0f, 0.0f}) * scale;
        turn.Bounded = true;
    }
}

// Report the legs too short for their corners' reach: between a corner and an
// open end of the run, and between two corners that turn the same way (two
// corners turning opposite ways put their inside faces on opposite sides of
// the leg, and meet nothing). A leg is too short when it is shorter than the
// reaches it has to hold plus the band the inside face keeps (MarkWithinReach).
// A corner whose reach was bounded to its legs fits them by construction.
void ReportFoldingLegs(std::span<const SplineStripStation> stations,
                       std::span<const CornerTurn> turns, std::span<const SplineCornerSite> sites,
                       bool closedLoop, std::vector<SplineCornerIssue>& issues)
{
    const size_t count = turns.size();
    const float32 runStart = stations.front().Distance;
    const float32 runEnd = stations.back().Distance;
    const float32 loopLength = runEnd - runStart;
    for (size_t t = 0; t < count; ++t)
    {
        const float32 at = stations[sites[t].StationIndex].Distance;
        const float32 reach = turns[t].Reach;
        if (!closedLoop)
        {
            const bool first = t == 0u;
            const bool last = t + 1u == count;
            if (!turns[t].Bounded &&
                ((first && at - runStart < reach + kMinInsideBandMetres) ||
                 (last && runEnd - at < reach + kMinInsideBandMetres)))
            {
                issues.push_back({SplineCornerIssueKind::LegShorterThanReach, sites[t].PointIndex,
                                  0u, turns[t].Turn * kDegreesPerRadian});
            }
        }
        if (t + 1u == count && (!closedLoop || count < 2u))
            continue;
        const size_t next = (t + 1u) % count;
        if (turns[next].RightTurn != turns[t].RightTurn || turns[t].Bounded || turns[next].Bounded)
            continue;
        float32 leg = stations[sites[next].StationIndex].Distance - at;
        if (next == 0u)
            leg += loopLength;
        if (leg < reach + turns[next].Reach + kMinInsideBandMetres)
        {
            issues.push_back({SplineCornerIssueKind::CornersOverlap, sites[t].PointIndex,
                              sites[next].PointIndex, turns[t].Turn * kDegreesPerRadian});
        }
    }
}

} // namespace

std::vector<SplineCornerIssue> ApplyCorners(std::vector<SplineStripStation>& stations,
                                            std::span<const SplineCornerSite> sites,
                                            SplineCornerStyle style, const Mathematics::Vector3& up,
                                            bool closedLoop)
{
    std::vector<SplineCornerIssue> issues;
    const uint32 count = static_cast<uint32>(stations.size());
    if (style == SplineCornerStyle::None || sites.empty() || count < 3u)
        return issues;

    // Every corner is measured on the stream as sampled, before any of them
    // reshapes it.
    std::vector<uint8> isSite(count, 0u);
    std::vector<int32> turnOfStation(count, -1);
    std::vector<CornerTurn> turns;
    std::vector<SplineCornerSite> turnSites;
    turns.reserve(sites.size());
    turnSites.reserve(sites.size());
    for (const SplineCornerSite& site : sites)
    {
        const uint32 index = site.StationIndex;
        const bool interior = index > 0u && index + 1u < count;
        const bool seam = closedLoop && index == 0u;
        if (!interior && !seam)
            continue;
        const CornerTurn turn = MeasureCorner(stations, index, style, up, closedLoop);
        if (turn.Turn < kMinCornerTurnRadians)
            continue;
        if (turn.Fallback)
        {
            issues.push_back({SplineCornerIssueKind::PastMitreLimit, site.PointIndex, 0u,
                              turn.Turn * kDegreesPerRadian});
        }
        isSite[index] = 1u;
        turnOfStation[index] = static_cast<int32>(turns.size());
        turns.push_back(turn);
        turnSites.push_back(site);
    }
    if (turns.empty())
        return issues;
    BoundReachPastTheLimit(stations, turns, turnSites, closedLoop);
    ReportFoldingLegs(stations, turns, turnSites, closedLoop, issues);

    std::vector<uint8> drop(count, 0u);
    for (uint32 i = 0; i < count; ++i)
    {
        if (turnOfStation[i] >= 0)
            MarkWithinReach(stations, i, turns[static_cast<uint32>(turnOfStation[i])].Reach,
                            closedLoop, isSite, drop);
    }

    // The seam corner of a closed loop opens the stream with its outgoing half
    // and closes it with its incoming half.
    const int32 seamTurn = closedLoop ? turnOfStation[0] : -1;

    std::vector<SplineStripStation> reshaped;
    reshaped.reserve(count + turns.size() * (kMaxRoundCornerRings + 1u));
    for (uint32 i = 0; i < count; ++i)
    {
        if (drop[i])
            continue;
        const bool closingSeam = seamTurn >= 0 && i + 1u == count;
        const int32 turnIndex = closingSeam ? seamTurn : turnOfStation[i];
        if (turnIndex < 0)
        {
            reshaped.push_back(stations[i]);
            continue;
        }
        const CornerTurn& turn = turns[static_cast<uint32>(turnIndex)];
        const bool openingSeam = seamTurn >= 0 && i == 0u;
        if (turn.BuildRound)
        {
            AppendRound(reshaped, stations[i], turn, up, openingSeam ? RoundSegments(turn) : 0u);
        }
        else
        {
            AppendMitre(reshaped, stations[i], turn, up, /*incomingHalf=*/!openingSeam,
                        /*outgoingHalf=*/!closingSeam);
        }
    }
    stations = std::move(reshaped);
    return issues;
}

std::vector<uint32> FindInnerFolds(std::span<const SplineStripStation> stations,
                                   std::span<const uint8> skip, const Mathematics::Vector3& up)
{
    std::vector<uint32> folds;
    for (size_t i = 1; i + 1u < stations.size(); ++i)
    {
        if (i < skip.size() && skip[i] != 0u)
            continue;
        const V3 a = InPlane(stations[i - 1u].Position, up);
        const V3 b = InPlane(stations[i].Position, up);
        const V3 c = InPlane(stations[i + 1u].Position, up);
        const V3 ab = b - a;
        const V3 bc = c - b;
        const V3 ca = a - c;
        // Twice the triangle's signed area, positive for a bend to the right.
        const float32 twiceArea = V3::Dot(V3::Cross(ab, bc), up);
        if (std::abs(twiceArea) < 1.0e-12f)
            continue;
        const float32 radius = std::sqrt(V3::Dot(ab, ab) * V3::Dot(bc, bc) * V3::Dot(ca, ca)) /
                               (2.0f * std::abs(twiceArea));
        const float32 insideHalfWidth =
            twiceArea > 0.0f ? stations[i].HalfWidthRight : stations[i].HalfWidthLeft;
        if (radius < insideHalfWidth)
            folds.push_back(static_cast<uint32>(i));
    }
    return folds;
}

void AccumulateFaceTurn(std::span<SplineStripStation> stations, const Mathematics::Vector3& up)
{
    // Running face turn per side over the plain stations. A corner's rings are
    // measured from the base the leg brought them to; past the corner, a side
    // whose rings stand on the mitre point (MitreSides) gains its stamp once
    // more — the other leg's stretch to or from that point — while a side that
    // swept an arc ends where its last ring stands.
    float32 left = 0.0f;
    float32 right = 0.0f;
    float32 baseLeft = 0.0f;
    float32 baseRight = 0.0f;
    float32 exitLeft = 0.0f;
    float32 exitRight = 0.0f;
    // The top's arc allowance: running, as the corner opened, and as it leaves.
    float32 allowance = 0.0f;
    float32 baseAllowance = 0.0f;
    float32 exitAllowance = 0.0f;
    bool inCorner = false;
    V3 previousForward = stations.empty() ? V3{} : stations.front().Forward;
    for (size_t i = 0; i < stations.size(); ++i)
    {
        SplineStripStation& station = stations[i];
        const float32 turn = i == 0u ? 0.0f : PlanTurn(previousForward, station.Forward, up);
        previousForward = station.Forward;
        const bool cornerRing = station.MitreSides != 0u;
        const bool opensCorner =
            cornerRing && (!inCorner || station.Join == SplineStationJoin::Advance);
        if (inCorner && (!cornerRing || opensCorner))
        {
            left = baseLeft + exitLeft;
            right = baseRight + exitRight;
            allowance = baseAllowance + exitAllowance;
            inCorner = false;
        }
        if (!cornerRing)
        {
            left += turn;
            right -= turn;
            station.FaceTurnLeft = left;
            station.FaceTurnRight = right;
            station.TopArcAllowance = allowance;
            continue;
        }

        const float32 stampLeft = station.CornerTurnLeft;
        const float32 stampRight = station.CornerTurnRight;
        // A round fan's rings stand on the mitre point on the inside only; their
        // outside sweeps an arc of the outside half-width, `arc` so far.
        float32 arc = 0.0f;
        if (station.MitreSides == kSplineMitreRightSide)
            arc = station.HalfWidthLeft * stampLeft;
        else if (station.MitreSides == kSplineMitreLeftSide)
            arc = station.HalfWidthRight * stampRight;
        if (opensCorner)
        {
            // A stream that opens on a corner ring starts at zero there.
            baseLeft = i == 0u ? -stampLeft : left + turn;
            baseRight = i == 0u ? -stampRight : right - turn;
            baseAllowance = i == 0u ? -arc : allowance;
            inCorner = true;
        }
        station.TopArcAllowance = baseAllowance + arc;
        exitAllowance = arc;
        station.FaceTurnLeft = baseLeft + stampLeft;
        station.FaceTurnRight = baseRight + stampRight;
        exitLeft = (station.MitreSides & kSplineMitreLeftSide) != 0u ? 2.0f * stampLeft : stampLeft;
        exitRight =
            (station.MitreSides & kSplineMitreRightSide) != 0u ? 2.0f * stampRight : stampRight;
    }
}

void CloseStationLoop(std::span<SplineStripStation> stations, const Mathematics::Vector3& up)
{
    const size_t count = stations.size();
    if (count < 3u)
        return;
    // Station 0 and the last station are the same place, so the frame there
    // looks from the station before the end to the one after the start.
    const V3 across = NormalizedOr(stations[1].Position - stations[count - 2u].Position,
                                   stations[0].Forward);
    const Frame frame = FrameAlong(across, up);
    stations[0].Forward = frame.Forward;
    stations[0].Right = frame.Right;
    stations[0].Up = frame.Up;

    const float32 loopLength = stations[count - 1u].Distance;
    stations[count - 1u] = stations[0];
    stations[count - 1u].Distance = loopLength;
}

} // namespace GameEngine::SplineGeometry
