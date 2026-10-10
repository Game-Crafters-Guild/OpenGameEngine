#include "SplineLayout/FenceLayout.h"

#include "Components/Spline/SplinePoolSelection.h"
#include "SplineLayout/PieceBasis.h"
#include "SplineLayout/SeamShear.h"
#include "SplineLayout/SpanMitre.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace GameEngine::SplineLayout
{
namespace
{

using V3 = Mathematics::Vector3;

// Authored points closer than this are the same corner: the run between them
// is skipped and its shared station emitted once.
constexpr float32 kMinRunLengthMetres = 1.0e-3f;
// A pool piece with no measurable length would divide the fill by zero.
constexpr float32 kMinPieceLengthMetres = 1.0e-3f;
constexpr float32 kMinPitchMetres = 0.05f;
// Ceiling on a run's counted crest cells, so the count always converts to its
// uint32 ordinal. Exactly representable in float32, and unreachable: the
// pitch floor is one millimetre, which is four thousand kilometres of run.
constexpr float32 kMaxCrestCellsPerRun = 4.0e9f;
// A draped sample this far above what a span is planted for — its plant line
// plus the skirt its piece is drawn to bury — counts as burying it; under it
// the difference is chord-approximation noise on the polyline.
constexpr float32 kBuryToleranceMetres = 0.05f;
// Pitch past which a span has no roll of its own left to carry and must borrow
// one. A DEGENERACY threshold, not a style dial: it marks where the frame stops
// being derivable, not where a steeper span would look wrong. Racked spans are
// meant to follow the grade however steep it gets, so this sits just under
// vertical — deliberately far from any aesthetic lean limit, which is a
// separate concept an author would own.
constexpr float32 kSpanRollDegeneratePitchDegrees = 85.0f;
// Below this squared length a span's forward has no horizontal extent to
// measure a grade along, and its plant line is its own base plane rather than a
// slope through it.
constexpr float32 kMinAlongGroundLengthSquared = 1.0e-8f;

const V3 kWorldUp(0.0f, 1.0f, 0.0f);
// The engine's forward axis, and what a piece falls back to when the frame it
// was to be laid on carries no usable direction at all.
const V3 kWorldForward(0.0f, 0.0f, 1.0f);

float32 VectorLength(const V3& v) { return std::sqrt(V3::Dot(v, v)); }

std::string Metres(float32 v)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", static_cast<double>(v));
    return std::string(buf);
}

std::string Factor(float32 v)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.3f", static_cast<double>(v));
    return std::string(buf);
}

std::string WholeDegrees(float32 radians)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.0f", static_cast<double>(radians * 180.0f / Mathematics::Pi));
    return std::string(buf);
}

// Linear sample of the draped polyline at a fractional index. Run boundaries
// land on authored points, which almost never coincide with a dense sample.
CenterSample SampleAtIndex(const std::vector<CenterSample>& center, float32 u)
{
    const float32 maxIndex = static_cast<float32>(center.size() - 1u);
    const float32 clamped = std::clamp(u, 0.0f, maxIndex);
    const size_t lo = std::min(static_cast<size_t>(clamped), center.size() - 2u);
    const float32 t = clamped - static_cast<float32>(lo);
    CenterSample out;
    out.Pos = center[lo].Pos + (center[lo + 1u].Pos - center[lo].Pos) * t;
    out.Normal = NormalizedOrFallback(
        center[lo].Normal + (center[lo + 1u].Normal - center[lo].Normal) * t, kWorldUp);
    out.SurfaceValid = true;
    return out;
}

// The polyline between two authored points, with both authored points as exact
// endpoints so the run's stations land on them.
std::vector<CenterSample> SliceRun(const std::vector<CenterSample>& center, float32 u0, float32 u1)
{
    std::vector<CenterSample> sub;
    sub.push_back(SampleAtIndex(center, u0));
    for (size_t i = 0; i < center.size(); ++i)
    {
        const float32 index = static_cast<float32>(i);
        if (index <= u0)
            continue;
        if (index >= u1)
            break;
        sub.push_back(center[i]);
    }
    sub.push_back(SampleAtIndex(center, u1));
    return sub;
}

float32 PolylineLength(const std::vector<CenterSample>& samples)
{
    float32 total = 0.0f;
    for (size_t i = 1; i < samples.size(); ++i)
        total += VectorLength(samples[i].Pos - samples[i - 1u].Pos);
    return total;
}

// Where one span of a run draws its piece from. An override turns a span into
// a gate (GatePool) or pins its SpanPool slot; a gate with no gate piece to draw
// is an opening, which keeps the room of the wall it replaced and places
// nothing in it.
enum class SpanSource : uint8
{
    Wall,
    Gate,
    Opening,
};

// One authored override as a run's fill reads it: the span it names, what it
// puts there, and which pool slot it asks for.
struct RunOverride
{
    uint32 Ordinal = 0;
    Components::SplineSpanOverrideKind Kind = Components::SplineSpanOverrideKind::None;
    uint32 PoolSlot = 0;
};

// How a run's length is divided into span pieces. The pieces need not share a
// length, so the count and the sequence are solved together.
struct RunFill
{
    // Per span, in order: the active pool slot it draws — a GatePool slot for a
    // gate, the SpanPool slot of the wall it replaced for an opening — and the
    // pool that slot is in.
    std::vector<uint32> Slots;
    std::vector<SpanSource> Sources;
    float32 NaturalLength = 0.0f;
    float32 Stretch = 1.0f;
    bool WithinCap = true;
    // The count the search started from. Validation names it, because the
    // search only ever tries this and its two neighbours — a count further out
    // may well fit, and saying otherwise would be false.
    int32 NominalCount = 1;
};

// The override a run's fill carries for one span, or null. A run carries at
// most the recipe's override capacity, so a scan is the whole cost.
const RunOverride* FindRunOverride(std::span<const RunOverride> overrides, uint32 ordinal)
{
    for (const RunOverride& entry : overrides)
    {
        if (entry.Ordinal == ordinal)
            return &entry;
    }
    return nullptr;
}

// What one span draws once its override, if any, is applied. A gate draws the
// gate piece its override names, or leaves an opening where the gate pool has
// no such piece; a pinned span draws the SpanPool slot its override names, and
// keeps the seeded pick where the pool has no such slot. The misses are
// reported where the run is solved (ReportRunOverrides), never here: the fill
// tries three counts and would say it three times.
void ApplyRunOverride(const RunOverride& entry, size_t spanPieceCount, size_t gatePieceCount,
                      uint32& slot, SpanSource& source)
{
    if (entry.Kind == Components::SplineSpanOverrideKind::Gate)
    {
        if (entry.PoolSlot < gatePieceCount)
        {
            slot = entry.PoolSlot;
            source = SpanSource::Gate;
        }
        else
        {
            source = SpanSource::Opening;
        }
    }
    else if (entry.Kind == Components::SplineSpanOverrideKind::ExplicitPiece &&
             entry.PoolSlot < spanPieceCount)
    {
        slot = entry.PoolSlot;
    }
}

// One attempt at the nominal count and one in each direction, then stop: the
// cap can be genuinely unsatisfiable (cap 1.25, 2 m piece, 2.6 m run has no
// integer count near the nominal), so the search must not chase it. The best of
// the three is still built — a run that skipped its spans would break the
// structure the author asked for — and the caller reports the miss.
//
// A gate is a span of the fill at its own length: it is chosen, not stretched to
// the length of the wall it replaces, and takes the run's one shared stretch as
// every span does, which closes the run around it as it does around walls of
// mixed length. The seeded picks of the other spans are those of the same
// ordinals without the override, so a gate that leaves its run's span count
// unchanged reshuffles nothing. One that changes the count moves the picks of
// every later run, whose ordinals are counted on from the spans before them.
RunFill SolveRunFill(float32 runLength, uint32 seed, uint32 spanBaseOrdinal,
                     std::span<const FencePieceBounds> pieces,
                     std::span<const FencePieceBounds> gatePieces,
                     std::span<const RunOverride> overrides, float32 maxStretch, float32 minScale)
{
    RunFill best;
    const uint32 activeCount = static_cast<uint32>(pieces.size());
    if (activeCount == 0u)
        return best;

    // Seed the count from the pool's own pieces, never from PostPitch. When the
    // pool has span meshes their lengths ARE the pitch, and a PostPitch that
    // disagrees with them puts the search nowhere near the exact fill: a 20 m
    // wall run of 5 m segments at the 2.4 m default pitch searches 7, 8 and 9
    // pieces and never sees the 4 that land on 1.00x. PostPitch estimates the
    // stations only when the pool is empty and the stations are all there is.
    float32 meanPieceLength = 0.0f;
    for (const FencePieceBounds& piece : pieces)
        meanPieceLength += std::max(piece.Length(), kMinPieceLengthMetres);
    meanPieceLength = std::max(meanPieceLength / static_cast<float32>(activeCount),
                               kMinPieceLengthMetres);

    const int32 nominal =
        std::max(1, static_cast<int32>(std::lround(runLength / meanPieceLength)));
    best.NominalCount = nominal;
    const int32 candidates[3] = {nominal, nominal + 1, nominal - 1};

    float32 bestDeviation = std::numeric_limits<float32>::max();
    for (const int32 count : candidates)
    {
        if (count < 1)
            continue;

        RunFill trial;
        trial.NominalCount = nominal;
        trial.Slots.resize(static_cast<size_t>(count));
        trial.Sources.assign(static_cast<size_t>(count), SpanSource::Wall);
        float32 natural = 0.0f;
        for (int32 k = 0; k < count; ++k)
        {
            uint32 slot = Components::SplinePoolSelect(
                seed, Components::SplinePoolRole::Span,
                spanBaseOrdinal + static_cast<uint32>(k), activeCount);
            SpanSource source = SpanSource::Wall;
            if (const RunOverride* entry = FindRunOverride(overrides, static_cast<uint32>(k)))
                ApplyRunOverride(*entry, pieces.size(), gatePieces.size(), slot, source);
            trial.Slots[static_cast<size_t>(k)] = slot;
            trial.Sources[static_cast<size_t>(k)] = source;
            const FencePieceBounds& piece =
                source == SpanSource::Gate ? gatePieces[slot] : pieces[slot];
            natural += std::max(piece.Length(), kMinPieceLengthMetres);
        }
        trial.NaturalLength = natural;
        trial.Stretch = runLength / natural;
        trial.WithinCap = trial.Stretch <= maxStretch && trial.Stretch >= minScale;
        if (trial.WithinCap)
            return trial;

        const float32 deviation = std::abs(std::log(std::max(trial.Stretch, 1.0e-6f)));
        if (deviation < bestDeviation)
        {
            bestDeviation = deviation;
            best = std::move(trial);
        }
    }
    return best;
}

// Everything one run contributes, resolved before any of it is emitted: a
// shared station's yaw needs BOTH adjoining runs' end directions.
struct Run
{
    std::vector<CenterSample> Samples;
    float32 LengthMetres = 0.0f;
    uint32 StartPoint = 0;
    uint32 EndPoint = 0;
    RunFill Fill;
    std::vector<float32> Stations; // 0 .. LengthMetres, one more than the spans
    V3 StartForward{0.0f, 0.0f, 1.0f};
    V3 EndForward{0.0f, 0.0f, 1.0f};
    std::vector<V3> StationForwards;
    std::vector<TilePose> Poses;
    // Span frames, resolved before emit so the bury check reads the geometry
    // that is actually placed rather than re-deriving a post-to-post line the
    // borrowed-roll case does not follow. Parallel to Fill.Slots.
    std::vector<TilePose> SpanPoses;
    std::vector<float32> SpanLengthScales;
    uint32 RollBorrowedSpans = 0;
    // Per span, parallel to SpanPoses: whether it borrowed its roll, and how
    // each end meets the span beyond its station. The poses and scales above
    // stay the chord's; a mitred end's overhang is applied where the span is
    // emitted, so every rule that reads the bay between two stations — the
    // crest cells, the bury check — keeps reading the bay.
    std::vector<uint8> SpanRollBorrowed;
    std::vector<FenceEndPlane> SpanStarts;
    std::vector<FenceEndPlane> SpanEnds;
    std::vector<float32> SpanStartOverhangs;
    std::vector<float32> SpanEndOverhangs;
};

// The pool piece span `span` of a run is laid as: its gate piece for a gate,
// otherwise the SpanPool piece it drew — for an opening, the wall whose room it
// keeps.
const FencePieceBounds& SpanPieceOf(const Run& run, size_t span, const FenceLayoutParams& params)
{
    return run.Fill.Sources[span] == SpanSource::Gate ? params.GatePieces[run.Fill.Slots[span]]
                                                      : params.SpanPieces[run.Fill.Slots[span]];
}

bool IsOpening(const Run& run, size_t span)
{
    return run.Fill.Sources[span] == SpanSource::Opening;
}

// How high the ground may stand under a world-space point before this span
// counts as buried. The plant line is a PLANE through the span's base: level
// across travel, climbing along it at the span's own grade. For a racked span
// that reproduces the interpolated post-to-post line, for a sheared one the
// line its bottom rail follows, and for a stepped one the level altitude, so it
// is the one model that also describes a span whose roll was borrowed.
//
// The projection is taken on the forward's HORIZONTAL part and divided by that
// part's own length, which is what makes one formula serve all three. Rise per
// metre of ground covered is a property of the grade, not of how long the
// direction vector happens to be — and a sheared span's forward is deliberately
// not a unit vector, so projecting on the raw vector would both scale the
// answer by its length and fold the sample's own height back into the line it
// is being measured against.
//
// The allowance above that line is the piece's own skirt. Under PivotPlane the
// skirt is ALREADY spent at plant time — the pivot sits on the surface, so that
// much panel is underground before the ground rises at all — and granting
// another skirt's worth on top of it is deliberate, not an oversight: total
// burial reaches about twice the skirt before this fires. The skirt is the only
// scale a piece carries for how much ground wander its design absorbs, and a
// check tuned tighter than the kit's own tolerance fires on fences that read
// correctly. BoundsMin stands the piece's lowest vertex on the surface instead
// — nothing of it is meant to be under the line, so it gets no allowance. A
// piece whose bounds stop ABOVE its own pivot declares no skirt and likewise
// gets none; a fence floating over its ground is a defect, but it is not this
// check's defect to report.
float32 SpanGroundAllowanceAt(const TilePose& span, const FencePieceBounds& piece,
                              Components::SplinePlantMode plantMode, const V3& at)
{
    const V3 alongGround(span.Forward.x, 0.0f, span.Forward.z);
    const float32 alongGroundLengthSquared = V3::Dot(alongGround, alongGround);
    const float32 plantLine =
        alongGroundLengthSquared > kMinAlongGroundLengthSquared
            ? span.Base.y + span.Forward.y * V3::Dot(at - span.Base, alongGround) /
                                alongGroundLengthSquared
            : span.Base.y;
    const float32 skirt = plantMode == Components::SplinePlantMode::PivotPlane
                              ? std::max(piece.SkirtDepth(), 0.0f)
                              : 0.0f;
    return plantLine + skirt;
}

V3 GroundDirection(const V3& v, const V3& fallback)
{
    return NormalizedOrFallback(V3(v.x, 0.0f, v.z), fallback);
}

// The caller's shared facing for one authored point, when it offered a usable
// one. Ground-projected like every other yaw source here: a station stands on
// the ground and only its heading is in question.
void ApplyAuthoredForward(std::span<const V3> authored, uint32 point, V3& forward)
{
    if (point < authored.size())
        forward = GroundDirection(authored[point], forward);
}

// How far a piece's own pivot stands above the line it is planted on:
// PivotPlane puts the pivot itself on that line and leaves the modelled skirt
// underground, BoundsMin lifts the piece until its lowest geometry rests on it.
float32 PlantedPivotLift(const FencePieceBounds& piece,
                         Components::SplinePlantMode plantMode)
{
    return plantMode == Components::SplinePlantMode::PivotPlane ? 0.0f : piece.SkirtDepth();
}

// Where a span mesh's top stands above the line its stations were planted on,
// which is what anything laid along that top has to be lifted by. The piece's
// authored HEIGHT is not that number: a skirted mesh planted by its pivot
// buries the skirt, so only the geometry above the pivot shows.
float32 SpanTopAboveBase(const FencePieceBounds& piece,
                         Components::SplinePlantMode plantMode)
{
    return PlantedPivotLift(piece, plantMode) + piece.Center.y + piece.HalfExtents.y;
}

// The two points a span joins: its stations' bases, both levelled to the lower
// of the two for a stepped span so it cannot float off its downhill end. Read
// by the span's own frame and by everything placed along that span.
void SpanEndBases(const TilePose& a, const TilePose& b, Components::SplineSpanGrade grade,
                  V3& outFrom, V3& outTo)
{
    outFrom = a.Base;
    outTo = b.Base;
    if (grade == Components::SplineSpanGrade::Stepped)
    {
        const float32 level = std::min(outFrom.y, outTo.y);
        outFrom.y = level;
        outTo.y = level;
    }
}

// Where a span's pivot stands for its base and scale. Footprint-centering, as
// for a station — the measured kits are corner-pivoted, so placing a span by
// raw pivot at the midpoint hangs it off-side. The offsets are cancelled along
// the piece's OWN local axes, so the one that runs along travel scales with
// the span exactly as the mesh does and the one across it does not.
void PlaceSpanPivot(TilePose& pose, const FencePieceBounds& piece,
                    Components::SplinePlantMode plantMode, float32 lengthScale)
{
    const float32 lift = PlantedPivotLift(piece, plantMode);
    const PieceBasis basis = MakePieceBasis(pose, piece.Axis());
    const PieceAxisScale scale = MakePieceAxisScale(piece.Axis(), lengthScale);
    pose.Position = pose.Base + pose.Up * lift - basis.LocalX * (piece.Center.x * scale.X) -
                    basis.LocalZ * (piece.Center.z * scale.Z);
}

// The frame a span takes between two stations. Derived entirely from the
// station poses: the stations already measured the ground, so a span over a
// terrain gap stays with the posts instead of diving into it.
TilePose MakeSpanPose(const TilePose& a, const TilePose& b, Components::SplineSpanGrade grade,
                      Components::SplinePlantMode plantMode, const FencePieceBounds& piece,
                      float32& outLengthScale, bool& outRollBorrowed)
{
    V3 from;
    V3 to;
    SpanEndBases(a, b, grade, from, to);

    const V3 delta = to - from;
    const V3 forward = NormalizedOrFallback(delta, a.Forward);
    const V3 upAverage = NormalizedOrFallback(a.Up + b.Up, kWorldUp);
    // What the piece has to cover with its own length. A rotated span — racked,
    // stepped, or one that borrowed its roll — is laid along the chord and
    // covers all of it; a sheared one carries its rise in the shear instead, so
    // its length answers for the run ACROSS the station up and nothing else.
    float32 spanLength = VectorLength(delta);

    // A span standing on end has no roll of its own to keep: every up
    // perpendicular to a vertical forward is horizontal, so deriving the up
    // from the cross product lays the piece FLAT on the ground. The trap is
    // that the basis stays perfectly orthonormal and finite while doing it —
    // nothing downstream can tell. Rule: past the degeneracy pitch the span
    // borrows its opening station's frame (fence design section 4).
    //
    // Discriminate on the span's own PITCH. A cross-product length cannot serve
    // here: |Cross(up, forward)| is still 0.042 for a span one degree off
    // vertical, so any threshold small enough to read as degenerate is only
    // reached once the piece is already flat.
    outRollBorrowed = std::abs(V3::Dot(forward, kWorldUp)) >
                      std::sin(kSpanRollDegeneratePitchDegrees * Mathematics::Pi / 180.0f);

    TilePose pose;
    if (outRollBorrowed)
    {
        // Section 4: roll falls back to station A's UP. Taking A's whole
        // orientation is the only self-consistent reading — with forward
        // parallel to A's up there is no roll about this forward that leaves
        // the piece upright, so the span holds the frame its post stands in.
        pose.Up = a.Up;
        pose.Right = NormalizedOrFallback(V3::Cross(pose.Up, forward), a.Right);
        pose.Forward = NormalizedOrFallback(V3::Cross(pose.Right, pose.Up), a.Forward);
    }
    else if (grade == Components::SplineSpanGrade::Sheared)
    {
        // Shear, not rotation: the piece's up stays with its stations so its
        // moulded posts and pickets keep standing, while its along-run axis
        // leaves that plane by exactly the grade and lands both ends on their
        // own station. Only Forward moves — Right and Up stay unit and mutually
        // perpendicular, the basis determinant stays 1, and the emitted matrix
        // (PieceEntity::WriteParentLocalPose) carries the whole map, exactly as
        // it already does for the seam shear.
        const float32 rise = V3::Dot(delta, upAverage);
        const V3 along = delta - upAverage * rise;
        const float32 alongLength = VectorLength(along);
        const V3 alongDirection = NormalizedOrFallback(along, forward);
        pose.Up = upAverage;
        pose.Right = NormalizedOrFallback(V3::Cross(pose.Up, alongDirection), a.Right);
        // NOT normalized, and that is the mechanism: the vector's length is the
        // stretch the piece takes to reach the higher station. Normalizing it
        // would put the top rail back on a rotation and lean the posts again.
        //
        // The floor on the divisor is the same one that calls two authored
        // points the same corner: stations nearer than that across the up
        // carry no run for a grade to be measured over, and the degeneracy
        // escape above has already taken every span steep enough to matter.
        pose.Forward =
            alongDirection + upAverage * (rise / std::max(alongLength, kMinRunLengthMetres));
        spanLength = alongLength;
    }
    else
    {
        // Right = Cross(up, forward) is the RIGHT of travel in this LH +Y-up
        // engine.
        pose.Right = NormalizedOrFallback(V3::Cross(upAverage, forward), a.Right);
        pose.Up = NormalizedOrFallback(V3::Cross(forward, pose.Right), upAverage);
        pose.Forward = NormalizedOrFallback(V3::Cross(pose.Right, pose.Up), forward);
    }

    outLengthScale = spanLength / std::max(piece.Length(), kMinPieceLengthMetres);

    pose.Base = from + (to - from) * 0.5f;
    PlaceSpanPivot(pose, piece, plantMode, outLengthScale);
    return pose;
}

// ---- Span joins ------------------------------------------------------------

// A station two spans meet at: the closing end of one and the opening end of
// the next, inside one run or across an authored point.
struct SpanJoin
{
    Run* Before = nullptr;
    size_t BeforeSpan = 0;
    Run* After = nullptr;
    size_t AfterSpan = 0;
};

// Whether a run's spans reach both of its end stations. A run the station
// budget cut short ends at no station its neighbour shares.
bool RunReachesItsEnds(const Run& run)
{
    return run.Poses.size() >= 2u && run.Poses.size() == run.Stations.size() &&
           run.SpanPoses.size() + 1u == run.Poses.size();
}

// Every station two spans share, in run order: each interior station of a
// run, then the authored point it closes on where the next run opens there —
// the closed loop's shared station included, as pass C pairs them.
std::vector<SpanJoin> CollectSpanJoins(std::vector<Run>& runs, bool closed)
{
    std::vector<SpanJoin> joins;
    const size_t sharedJoints = closed ? runs.size() : runs.size() - 1u;
    for (size_t r = 0; r < runs.size(); ++r)
    {
        Run& run = runs[r];
        for (size_t k = 1; k < run.SpanPoses.size(); ++k)
            joins.push_back({&run, k - 1u, &run, k});
        if (r >= sharedJoints)
            continue;
        Run& next = runs[(r + 1u) % runs.size()];
        if (&next == &run || !RunReachesItsEnds(run) || !RunReachesItsEnds(next))
            continue;
        const V3 gap = run.Poses.back().Base - next.Poses.front().Base;
        if (V3::Dot(gap, gap) > kMinRunLengthMetres * kMinRunLengthMetres)
            continue;
        joins.push_back({&run, run.SpanPoses.size() - 1u, &next, 0u});
    }
    return joins;
}

// How a join is named to the author: by its authored point where it has one.
std::string JoinName(const SpanJoin& join)
{
    if (join.Before != join.After)
        return "point " + std::to_string(join.Before->EndPoint);
    return "the station " + Metres(join.Before->Stations[join.AfterSpan]) + " m along run " +
           std::to_string(join.Before->StartPoint) + "->" + std::to_string(join.Before->EndPoint);
}

void MitreBareJoins(std::vector<Run>& runs, const FenceLayoutParams& params,
                    std::vector<std::string>& validation);

// What a registered crest adds to a run's spans at a join: the widest crest
// piece's half-thickness and the heights the crest pieces cover above the span
// piece's top. A zero half-thickness is no registered crest.
struct CrestReach
{
    float32 HalfThickness = 0.0f;
    float32 Bottom = 0.0f;
    float32 Top = 0.0f;
};

// One span's side of a join, as the join is measured on it.
SpanJoinSide JoinSideOf(const Run& run, size_t span, bool atStart, const FenceLayoutParams& params,
                        const CrestReach& crest)
{
    SpanJoinSide side;
    side.Pose = run.SpanPoses[span];
    V3 from;
    V3 to;
    SpanEndBases(run.Poses[span], run.Poses[span + 1u], params.SpanGrade, from, to);
    side.BaseLineEnd = atStart ? from : to;
    const FencePieceBounds& piece = SpanPieceOf(run, span, params);
    side.HalfThickness = piece.HalfThickness();
    const float32 lift = PlantedPivotLift(piece, params.PlantMode);
    side.Bottom = lift + piece.Center.y - piece.HalfExtents.y;
    side.Top = lift + piece.Center.y + piece.HalfExtents.y;
    // A gate stands in a reserved cell, so no crest stands on it to reach
    // across the join.
    if (crest.HalfThickness > 0.0f && run.Fill.Sources[span] == SpanSource::Wall)
    {
        side.CrestHalfThickness = crest.HalfThickness;
        side.CrestBottom = side.Top + crest.Bottom;
        side.CrestTop = side.Top + crest.Top;
    }
    return side;
}

// Decides every span end's join (fence design section 4c). A join is mitred
// where all of these hold: two spans meet there — a gate is a span, an opening
// places nothing to meet; no post mesh stands on it,
// because a post covers the join and a variant inside it would be a mesh for
// nothing; neither span borrowed its roll, because such a span is laid on its
// opening station's frame rather than on the chord and its end is not where
// the plane is; the notch its square ends would leave exceeds a millimetre,
// below which the turn is the polyline's chord noise; the turn is within the
// mitre limit; and the reach that carries both ends across the plane leaves
// both spans within the stretch cap. Past the limit or the cap the ends stay
// square and the author is told.
void MitreJoinsOfRuns(std::vector<Run>& runs, const FenceLayoutParams& params,
                      const std::vector<CrestReach>& crests, std::vector<std::string>& validation)
{
    for (Run& run : runs)
    {
        const size_t spans = run.SpanPoses.size();
        run.SpanStarts.assign(spans, FenceEndPlane{});
        run.SpanEnds.assign(spans, FenceEndPlane{});
        run.SpanStartOverhangs.assign(spans, 0.0f);
        run.SpanEndOverhangs.assign(spans, 0.0f);
    }
    if (params.HasPostMesh)
        return;

    const float32 limitRadians = kMitreLimitDegrees * Mathematics::Pi / 180.0f;
    for (const SpanJoin& join : CollectSpanJoins(runs, params.Closed))
    {
        Run& before = *join.Before;
        Run& after = *join.After;
        if (before.SpanRollBorrowed[join.BeforeSpan] != 0u ||
            after.SpanRollBorrowed[join.AfterSpan] != 0u || IsOpening(before, join.BeforeSpan) ||
            IsOpening(after, join.AfterSpan))
            continue;
        const TilePose& beforePose = before.SpanPoses[join.BeforeSpan];
        const TilePose& afterPose = after.SpanPoses[join.AfterSpan];
        const float32 turn = SignedGroundYaw(beforePose.Forward, afterPose.Forward);
        if (std::abs(turn) > limitRadians)
        {
            validation.push_back("Fence: " + JoinName(join) + " turns " +
                                 WholeDegrees(std::abs(turn)) + " degrees, past the " +
                                 WholeDegrees(limitRadians) +
                                 " degree mitre limit — the spans meet with square ends there; "
                                 "plant a post at it");
            continue;
        }
        SpanJoinMeasure measure;
        const size_t beforeRun = static_cast<size_t>(&before - runs.data());
        const size_t afterRun = static_cast<size_t>(&after - runs.data());
        if (!MeasureSpanJoin(
                JoinSideOf(before, join.BeforeSpan, false, params, crests[beforeRun]),
                JoinSideOf(after, join.AfterSpan, true, params, crests[afterRun]),
                before.Poses[join.BeforeSpan + 1u].Base, measure) ||
            !(measure.NotchMetres > kMinRunLengthMetres))
            continue;
        before.SpanEnds[join.BeforeSpan] = measure.BeforeEnd;
        after.SpanStarts[join.AfterSpan] = measure.AfterStart;
        before.SpanEndOverhangs[join.BeforeSpan] = measure.BeforeOverhangMetres;
        after.SpanStartOverhangs[join.AfterSpan] = measure.AfterOverhangMetres;
    }

    // The reach is a stretch the piece takes, so the cap governs it: a join
    // whose reach would carry either span past the cap, where its fill alone
    // did not, keeps square ends. A span whose fill is already past the cap has
    // been reported and is built at that stretch, so its joins stay mitred. Undoing a join only shortens its spans, so
    // one pass along the fence settles every join.
    const float32 maxStretch = std::max(params.SpanMaxStretch, 1.0f);
    const auto mitredScale = [&params](const Run& run, size_t span)
    {
        const float32 length =
            std::max(SpanPieceOf(run, span, params).Length(), kMinPieceLengthMetres);
        return (run.SpanLengthScales[span] * length + run.SpanStartOverhangs[span] +
                run.SpanEndOverhangs[span]) /
               length;
    };
    const auto overCap = [&](const Run& run, size_t span)
    {
        return mitredScale(run, span) > maxStretch && run.SpanLengthScales[span] <= maxStretch;
    };
    for (const SpanJoin& join : CollectSpanJoins(runs, params.Closed))
    {
        Run& before = *join.Before;
        Run& after = *join.After;
        if (!before.SpanEnds[join.BeforeSpan].Mitred ||
            (!overCap(before, join.BeforeSpan) && !overCap(after, join.AfterSpan)))
            continue;
        const float32 needed =
            std::max(mitredScale(before, join.BeforeSpan), mitredScale(after, join.AfterSpan));
        before.SpanEnds[join.BeforeSpan] = FenceEndPlane{};
        after.SpanStarts[join.AfterSpan] = FenceEndPlane{};
        before.SpanEndOverhangs[join.BeforeSpan] = 0.0f;
        after.SpanStartOverhangs[join.AfterSpan] = 0.0f;
        validation.push_back("Fence: mitring " + JoinName(join) + " needs a span stretched " +
                             Factor(needed) + "x, past the " + Factor(maxStretch) +
                             "x cap — the spans meet with square ends there; plant a post at it "
                             "or raise Span Max Stretch");
    }
}

// Where a leaning plane crosses the piece's centre line at the piece's top:
// the plane moves along travel by tan(lean) per metre up, which takes it
// further out at a closing end and further in at an opening one.
void SetTopInset(FenceEndPlane& plane, bool atStart, const FencePieceBounds& piece,
                 float32 lengthScale)
{
    const float32 rise = piece.Center.y + piece.HalfExtents.y - plane.StationLocalY;
    plane.TopInset = plane.StationInset + (atStart ? 1.0f : -1.0f) *
                                              std::tan(plane.LeanRadians) * rise / lengthScale;
}

// A span as it is emitted: laid longer by each mitred end's overhang, its
// base moved along travel by half their difference so each end reaches its
// own plane, and its planes stated in its final scale. A span with square
// ends is exactly its chord pose.
void ShapeEmittedSpan(const Run& run, size_t span, const FenceLayoutParams& params,
                      TilePose& outPose, float32& outLengthScale, FenceEndPlane& outStart,
                      FenceEndPlane& outEnd)
{
    outPose = run.SpanPoses[span];
    outLengthScale = run.SpanLengthScales[span];
    outStart = run.SpanStarts[span];
    outEnd = run.SpanEnds[span];
    if (!outStart.Mitred && !outEnd.Mitred)
        return;

    const FencePieceBounds& piece = SpanPieceOf(run, span, params);
    const float32 startOverhang = run.SpanStartOverhangs[span];
    const float32 endOverhang = run.SpanEndOverhangs[span];
    if (startOverhang > 0.0f || endOverhang > 0.0f)
    {
        const float32 length = std::max(piece.Length(), kMinPieceLengthMetres);
        outLengthScale = (outLengthScale * length + startOverhang + endOverhang) / length;
        outPose.Base = outPose.Base + outPose.Forward * (0.5f * (endOverhang - startOverhang));
        PlaceSpanPivot(outPose, piece, params.PlantMode, outLengthScale);
    }
    // The measured inset is in the frame's metres; the variant reads it in
    // the piece's unscaled local metres.
    const float32 stationLocalY = -PlantedPivotLift(piece, params.PlantMode);
    if (outStart.Mitred)
    {
        outStart.StationInset /= outLengthScale;
        outStart.StationLocalY = stationLocalY;
        SetTopInset(outStart, true, piece, outLengthScale);
    }
    if (outEnd.Mitred)
    {
        outEnd.StationInset /= outLengthScale;
        outEnd.StationLocalY = stationLocalY;
        SetTopInset(outEnd, false, piece, outLengthScale);
    }
}

// Lays one end of an emitted span square at its station: the piece budget
// stopped emission before the span that end was mitred against, so a cut there
// would show its angled face to nothing.
void SquareUnpairedEnd(std::vector<Run>& runs, const FenceLayoutParams& params, bool atStart,
                       FenceSpan& span)
{
    for (Run& run : runs)
    {
        if (run.StartPoint != span.Run)
            continue;
        const size_t k = span.OrdinalInRun;
        (atStart ? run.SpanStarts : run.SpanEnds)[k] = FenceEndPlane{};
        (atStart ? run.SpanStartOverhangs : run.SpanEndOverhangs)[k] = 0.0f;
        ShapeEmittedSpan(run, k, params, span.Pose, span.LengthScale, span.Start, span.End);
        return;
    }
}

// ---- Crest row -------------------------------------------------------------

// A stretch of one run's crest axis, in metres from its opening station.
struct RunInterval
{
    float32 Lo = 0.0f;
    float32 Hi = 0.0f;
};

// The axis a run's crest row is counted, reserved and placed on: metres along
// the spans, because a crest piece stands on a span and a span is the chord
// between its two stations. Where the draped centerline climbs over something
// inside a bay the span is shorter than the run it covers, and a row counted
// along the drape crowds more pieces onto it than it holds.
//
// Each station sits at its draped station less the drape — draped length minus
// chord — of every span before it, so the phase stays anchored at the run's
// opening station. A span whose chord is within the touching millimetre of its
// draped length carries no drape; a run with none reads its own stations and
// length bit for bit.
struct CrestAxis
{
    std::vector<float32> Stations; // parallel to Run::Stations
    float32 LengthMetres = 0.0f;
};

void BuildCrestAxis(const Run& run, Components::SplineSpanGrade grade, CrestAxis& out)
{
    out.Stations = run.Stations;
    float32 drape = 0.0f;
    for (size_t k = 1; k < out.Stations.size(); ++k)
    {
        if (k <= run.SpanPoses.size())
        {
            V3 from;
            V3 to;
            SpanEndBases(run.Poses[k - 1u], run.Poses[k], grade, from, to);
            const float32 spanDrape =
                (run.Stations[k] - run.Stations[k - 1u]) - VectorLength(to - from);
            if (std::abs(spanDrape) > kMinRunLengthMetres)
                drape += spanDrape;
        }
        out.Stations[k] = run.Stations[k] - drape;
    }
    out.LengthMetres = run.LengthMetres - drape;
}

// The pitch the crest cells are laid at, with the piece length one cell has to
// hold.
struct CrestPitch
{
    float32 Metres = 0.0f;
    float32 LongestPiece = 0.0f;
    // The pitch the author typed, 0 for the piece's own length.
    float32 Asked = 0.0f;
    // The author asked for less than the longest piece needs. Pieces that close
    // would overlap, so the piece's length is used and the author is told once.
    bool RaisedToPieceLength = false;
};

CrestPitch ResolveCrestPitch(std::span<const FencePieceBounds> pieces, float32 authored)
{
    CrestPitch pitch;
    // The floor every pool piece answers to is also the answer for a pool that
    // offers no measurable piece at all.
    pitch.LongestPiece = kMinPieceLengthMetres;
    for (const FencePieceBounds& piece : pieces)
        pitch.LongestPiece = std::max(pitch.LongestPiece, piece.Length());
    // A non-finite pitch carries no intent to space anything, so it reads as 0
    // — the piece's own length — and no cell can be positioned from a NaN.
    const float32 asked = std::isfinite(authored) ? authored : 0.0f;
    pitch.Asked = std::max(asked, 0.0f);
    pitch.Metres = asked > 0.0f ? asked : pitch.LongestPiece;
    pitch.RaisedToPieceLength = pitch.Metres < pitch.LongestPiece;
    if (pitch.RaisedToPieceLength)
        pitch.Metres = pitch.LongestPiece;
    return pitch;
}

// The longest crest piece a clear stretch holds whole whatever its phase
// against the cells. A cell holds its piece centred in [kP, (k+1)P], so a
// stretch S is certain to hold a piece of length L only when S >= P + L. A
// piece longer than the pitch raises the pitch to its own length, so at most
// half the stretch is certain either way; at pitch 0 half the stretch is the
// bound. Floored to the centimetre the report prints, so the printed number
// fits too.
float32 CrestPieceThatAlwaysFits(float32 stretch, float32 askedPitch)
{
    const float32 half = stretch * 0.5f;
    const float32 fits = askedPitch > 0.0f ? std::min(stretch - askedPitch, half) : half;
    return std::floor(fits * 100.0f) / 100.0f;
}

// The report for a crest row that placed no crest piece while a pitch-grid run
// was long enough to hold one bare: the posts or the caller's pieces took the
// room. It names the widest stretch left and the piece length that is certain
// to fit it. Caps may still stand, so it speaks of crest pieces only.
std::string EmptyCrestRowReport(float32 stretch, float32 askedPitch, bool hasPostMesh)
{
    const std::string wayOut =
        hasPostMesh ? ", or clear the Post pool." : ", or move what reserves the run.";
    const std::string opening =
        "Fence: the crest row placed no crest pieces — the longest clear stretch is " +
        Metres(stretch) + " m. ";
    const float32 fits = CrestPieceThatAlwaysFits(stretch, askedPitch);
    if (askedPitch > 0.0f && fits < kMinPieceLengthMetres)
    {
        return opening + "Crest Pitch " + Metres(askedPitch) +
               " m leaves no room for a piece in it; lower the pitch" + wayOut;
    }
    // The pitch, not the half, is what binds once it exceeds half the stretch.
    if (askedPitch > 0.0f && stretch - askedPitch < stretch * 0.5f)
    {
        return opening + "At a Crest Pitch of " + Metres(askedPitch) +
               " m, a crest piece no longer than " + Metres(fits) +
               " m (the stretch less the pitch) always fits; use one" + wayOut;
    }
    return opening + "A crest piece no longer than half of it, " + Metres(fits) +
           " m, always fits; use one" + wayOut;
}

// Everything on one run the crest row keeps clear: the footprint of the post at
// every station (a zero half-length reserves none), the span every gate and
// opening stands in, and the stretches the caller reserved for pieces of its
// own, all on the crest axis. Sorted and merged once per rebuild, so a cell
// tests one list; a zero-length interval reserves nothing and never reaches it.
//
// A gate's footprint is its span: it is laid from station to station as every
// span is, so the stretch between those two stations is exactly what it
// covers. An opening reserves the same stretch, because there is no wall under
// it for a crest to stand on.
std::vector<RunInterval> MergedRunReservations(const Run& run, const CrestAxis& axis,
                                               float32 postHalfLength,
                                               std::span<const FenceReservation> reserved)
{
    std::vector<RunInterval> intervals;
    if (postHalfLength > 0.0f)
    {
        intervals.reserve(axis.Stations.size());
        for (const float32 station : axis.Stations)
            intervals.push_back({station - postHalfLength, station + postHalfLength});
    }
    const size_t spans = std::min(run.SpanPoses.size(), run.Fill.Sources.size());
    for (size_t k = 0; k < spans && k + 1u < axis.Stations.size(); ++k)
    {
        if (run.Fill.Sources[k] != SpanSource::Wall)
            intervals.push_back({axis.Stations[k], axis.Stations[k + 1u]});
    }
    for (const FenceReservation& entry : reserved)
    {
        // Ordered comparisons throughout, so a NaN bound drops the entry rather
        // than reserving a stretch no footprint can be tested against.
        if (!(entry.EndMetres > entry.StartMetres))
            continue;
        // The part reaching forward from the point this run opens at, and the
        // part reaching back from the point it closes on. A closed spline's
        // single run answers to both, which is what its shared station is.
        if (entry.Point == run.StartPoint && entry.EndMetres > 0.0f)
            intervals.push_back({std::max(entry.StartMetres, 0.0f), entry.EndMetres});
        if (entry.Point == run.EndPoint && entry.StartMetres < 0.0f)
        {
            intervals.push_back({axis.LengthMetres + entry.StartMetres,
                                 axis.LengthMetres + std::min(entry.EndMetres, 0.0f)});
        }
    }
    std::sort(intervals.begin(), intervals.end(),
              [](const RunInterval& a, const RunInterval& b) { return a.Lo < b.Lo; });

    std::vector<RunInterval> merged;
    merged.reserve(intervals.size());
    for (const RunInterval& interval : intervals)
    {
        if (!merged.empty() && interval.Lo <= merged.back().Hi)
            merged.back().Hi = std::max(merged.back().Hi, interval.Hi);
        else
            merged.push_back(interval);
    }
    return merged;
}

// The stretches of a run no reservation covers, in run order: where cells may
// stand, and where a cap may close what they leave.
std::vector<RunInterval> FreeRunIntervals(float32 runLength,
                                          const std::vector<RunInterval>& reserved)
{
    std::vector<RunInterval> free;
    free.reserve(reserved.size() + 1u);
    float32 cursor = 0.0f;
    for (const RunInterval& interval : reserved)
    {
        const float32 lo = std::clamp(interval.Lo, 0.0f, runLength);
        if (lo > cursor)
            free.push_back({cursor, lo});
        cursor = std::max(cursor, std::min(interval.Hi, runLength));
    }
    if (cursor < runLength)
        free.push_back({cursor, runLength});
    return free;
}

// Cells of one run: one opens at every k * P inside it. Counted arithmetically
// and positioned by multiplication — accumulating float32 over 4096 cells would
// move the last one by millimetres and break the touching test.
//
// Every cell is counted, including the ones a reservation removes and the ones
// the piece budget never reaches: this is the ordinal the NEXT run's cells are
// numbered from, and it may not depend on what was cleared or cut upstream.
uint32 CrestCellCount(float32 runLength, float32 pitch)
{
    const float32 exact = std::ceil(runLength / pitch);
    if (!(exact > 0.0f))
        return 0u;
    return static_cast<uint32>(std::min(exact, kMaxCrestCellsPerRun));
}

// The cell a cap stands in, whose ordinal it inherits.
uint32 CellAtDistance(float32 distance, float32 pitch, uint32 cellCount)
{
    if (cellCount == 0u)
        return 0u;
    const float32 cell = std::floor(distance / pitch);
    if (!(cell > 0.0f))
        return 0u;
    return static_cast<uint32>(std::min(cell, static_cast<float32>(cellCount - 1u)));
}

// The cells one run's crest row is counted on. By default the pitch grid, every
// P metres from the run's opening station. A run registered to its spans counts
// one cell per span instead, and the cell IS the span: its stretch of the crest
// axis, from the station opening it to the one closing it.
struct CrestCells
{
    float32 Pitch = 0.0f;
    uint32 Count = 0;
    // The run's crest-axis stations when the row is registered to its spans;
    // null on the pitch grid.
    const std::vector<float32>* SpanStations = nullptr;

    bool RegisteredToSpans() const { return SpanStations != nullptr; }

    // The cell a distance along the crest axis falls in.
    uint32 CellAt(float32 distance) const
    {
        if (!RegisteredToSpans())
            return CellAtDistance(distance, Pitch, Count);
        if (Count == 0u)
            return 0u;
        const auto upper = std::upper_bound(SpanStations->begin(), SpanStations->end(), distance);
        const size_t after = static_cast<size_t>(upper - SpanStations->begin());
        return static_cast<uint32>(std::min<size_t>(after == 0u ? 0u : after - 1u, Count - 1u));
    }
};

// Whether a run's crest row is registered to its spans: the crest cell and
// every span piece the run draws share one nominal length. A crest laid at its
// own pitch would then drift off the walls wherever they are stretched — on a
// curve every chord is — so each crest takes its span's frame and scale
// instead, whatever that scale is: the wall is what the author sees, and a run
// stretched past the fill's bounds is already reported. Any span of another
// length leaves the whole run on the pitch grid: a row never mixes the two
// rules.
//
// Only the walls are asked: a gate or an opening reserves its own span, so the
// cell over it is removed however long its piece is, and a castle gate that is
// longer than the walls beside it leaves them registered.
bool CrestRegistersToSpans(const Run& run, const FenceLayoutParams& params,
                           const CrestPitch& pitch)
{
    // An author who asked for air between the pieces asked for the pitch.
    if (run.SpanPoses.empty() || pitch.Metres > pitch.LongestPiece + kMinRunLengthMetres)
        return false;
    for (size_t k = 0; k < run.SpanPoses.size(); ++k)
    {
        if (run.Fill.Sources[k] != SpanSource::Wall)
            continue;
        const float32 spanPiece =
            std::max(params.SpanPieces[run.Fill.Slots[k]].Length(), kMinPieceLengthMetres);
        if (std::abs(spanPiece - pitch.LongestPiece) > kMinRunLengthMetres)
            return false;
    }
    return true;
}

// Decides every span end's join, with what a registered crest adds to each
// run's spans (MitreJoinsOfRuns).
void MitreBareJoins(std::vector<Run>& runs, const FenceLayoutParams& params,
                    std::vector<std::string>& validation)
{
    std::vector<CrestReach> crests(runs.size());
    if (!params.CrestPieces.empty())
    {
        const CrestPitch pitch = ResolveCrestPitch(params.CrestPieces, params.CrestPitch);
        CrestReach reach;
        reach.Bottom = std::numeric_limits<float32>::max();
        reach.Top = std::numeric_limits<float32>::lowest();
        for (const FencePieceBounds& crest : params.CrestPieces)
        {
            reach.HalfThickness = std::max(reach.HalfThickness, crest.HalfThickness());
            reach.Bottom = std::min(reach.Bottom, crest.Center.y - crest.HalfExtents.y);
            reach.Top = std::max(reach.Top, crest.Center.y + crest.HalfExtents.y);
        }
        for (size_t r = 0; r < runs.size(); ++r)
        {
            if (CrestRegistersToSpans(runs[r], params, pitch))
                crests[r] = reach;
        }
    }
    MitreJoinsOfRuns(runs, params, crests, validation);
}

// One piece of one run's crest row, solved on the crest axis before anything is
// posed.
struct CrestPlacement
{
    float32 Centre = 0.0f;
    uint32 Ordinal = 0;
    uint32 PoolSlot = 0;
    bool IsCap = false;
    // A cap closing the edge BELOW its stretch — a run start, or the far side
    // of a reservation — faces back down the run: a half-turn about the span's
    // up, never a mirror.
    bool FacesBack = false;
};

// The longest cap that fits a remainder. By length and not by the seed: a
// remainder is a fit, and a shorter pick would leave a gap the pool could have
// closed. Touching counts as fitting, by the millimetre everything here does.
bool ChooseCapPiece(std::span<const FencePieceBounds> caps, float32 remainder, uint32& outSlot,
                    float32& outLength)
{
    bool found = false;
    for (uint32 slot = 0; slot < static_cast<uint32>(caps.size()); ++slot)
    {
        const float32 length = std::max(caps[slot].Length(), kMinPieceLengthMetres);
        if (length > remainder + kMinRunLengthMetres || (found && length <= outLength))
            continue;
        outSlot = slot;
        outLength = length;
        found = true;
    }
    return found;
}

// A cap centred where the remainder leaves it, presenting its front to the edge
// it closes against: the post, tower or gate that stretch of run ends at. Where
// that edge lies below the cap the piece takes a half-turn about the span's up,
// never a mirror. Which way it reaches and which way it faces are two answers,
// and a cap alone in a stretch reaches up from its opening edge while still
// facing it.
CrestPlacement CapAt(float32 centre, bool facesBack, uint32 slot, const CrestCells& cells)
{
    CrestPlacement cap;
    cap.Centre = centre;
    cap.Ordinal = cells.CellAt(cap.Centre);
    cap.PoolSlot = slot;
    cap.IsCap = true;
    cap.FacesBack = facesBack;
    return cap;
}

// One run's crest row: the cells that clear its reservations, then the caps
// that close what each free stretch has left over, in run order.
//
// Cells are laid from the run's OPENING station and never from the spline
// start, which is what makes the row's phase survive an edit: adding, deleting
// or dragging a point elsewhere leaves this run's cells where they were. A cell
// that does not fit whole inside the run, or that stands under a reservation,
// is removed and keeps its ordinal, so raising a tower renumbers nothing.
std::vector<CrestPlacement> SolveRunCrest(const Run& run, const CrestAxis& axis,
                                          const FenceLayoutParams& params,
                                          const CrestCells& cells, uint32 cellBase,
                                          float32& outLongestFreeStretch)
{
    // A registered crest spans the bay between two posts exactly as its wall
    // does, and no station stands inside a span, so the posts at its own two
    // ends are its join, not a collision: they reserve against the grid only.
    const float32 postHalfLength = params.HasPostMesh && !cells.RegisteredToSpans()
                                       ? params.PostPiece.Length() * 0.5f
                                       : 0.0f;
    const std::vector<RunInterval> reserved =
        MergedRunReservations(run, axis, postHalfLength, params.Reservations);
    const uint32 activeCrest = static_cast<uint32>(params.CrestPieces.size());

    std::vector<CrestPlacement> row;
    std::vector<CrestPlacement> kept;
    std::vector<RunInterval> footprints; // parallel to kept
    bool keptAny = false;
    uint32 lastKept = 0;
    for (const RunInterval& free : FreeRunIntervals(axis.LengthMetres, reserved))
    {
        // The widest stretch any piece was offered. A row that placed nothing
        // reports it, because it is the number that says which piece could have
        // fitted.
        outLongestFreeStretch = std::max(outLongestFreeStretch, free.Hi - free.Lo);
        // Only the cells that can reach this stretch are built. A cell holds
        // its piece inside [kP, (k+1)P], or inside its span when the row is
        // registered, so a tower covering a kilometre of run costs the
        // arithmetic that steps over its cells, not their solving.
        const int64 firstCell =
            cells.RegisteredToSpans()
                ? static_cast<int64>(cells.CellAt(free.Lo))
                : std::max<int64>(static_cast<int64>(std::floor(free.Lo / cells.Pitch)) - 1, 0);
        const int64 lastCell =
            cells.RegisteredToSpans()
                ? static_cast<int64>(cells.CellAt(free.Hi))
                : std::min<int64>(static_cast<int64>(std::floor(free.Hi / cells.Pitch)),
                                  static_cast<int64>(cells.Count) - 1);
        kept.clear();
        footprints.clear();
        for (int64 index = firstCell; index <= lastCell; ++index)
        {
            const uint32 cell = static_cast<uint32>(index);
            // Defensive: two stretches could both reach one cell only if the
            // reservation between them were shorter than the slack on either
            // side of it, which needs a crest piece under two millimetres. One
            // compare keeps the row single-valued whatever a caller reserves.
            if (keptAny && cell <= lastKept)
                continue;

            CrestPlacement placement;
            placement.Ordinal = cell;
            placement.PoolSlot = Components::SplinePoolSelect(
                params.Seed, Components::SplinePoolRole::Crest, cellBase + cell, activeCrest);
            float32 half =
                std::max(params.CrestPieces[placement.PoolSlot].Length(), kMinPieceLengthMetres) *
                0.5f;
            if (cells.RegisteredToSpans())
            {
                // Centred on its span and scaled as that span is.
                placement.Centre = (axis.Stations[cell] + axis.Stations[cell + 1u]) * 0.5f;
                half *= run.SpanLengthScales[cell];
            }
            else
            {
                placement.Centre = static_cast<float32>(cell) * cells.Pitch + cells.Pitch * 0.5f;
            }
            const RunInterval footprint{placement.Centre - half, placement.Centre + half};
            // Whole or not at all. A piece that does not fit between the edges
            // of its stretch is dropped and keeps its ordinal, and the post or
            // tower at that edge covers the join; touching an edge is not
            // crossing it, by the millimetre that calls two authored points one
            // corner.
            if (footprint.Lo < free.Lo - kMinRunLengthMetres ||
                footprint.Hi > free.Hi + kMinRunLengthMetres)
                continue;

            kept.push_back(placement);
            footprints.push_back(footprint);
            keptAny = true;
            lastKept = cell;
        }

        if (params.CapPieces.empty())
        {
            row.insert(row.end(), kept.begin(), kept.end());
            continue;
        }

        // One cap per remainder, flush against the kept cells, so the residual
        // the pool cannot close lies against the post or gate that covers it.
        uint32 slot = 0;
        float32 length = 0.0f;
        if (kept.empty())
        {
            // Nothing stands here, so the whole stretch is ONE remainder and it
            // opens at the lower edge — a run shorter than a pitch takes a cap
            // there or nothing at all. The cap reaches up from that edge and
            // faces it, as the cap closing a lower edge beside cells does.
            if (ChooseCapPiece(params.CapPieces, free.Hi - free.Lo, slot, length))
                row.push_back(CapAt(free.Lo + length * 0.5f, true, slot, cells));
            continue;
        }
        const float32 keptLo = footprints.front().Lo;
        const float32 keptHi = footprints.back().Hi;
        if (ChooseCapPiece(params.CapPieces, keptLo - free.Lo, slot, length))
            row.push_back(CapAt(keptLo - length * 0.5f, true, slot, cells));
        row.insert(row.end(), kept.begin(), kept.end());
        if (ChooseCapPiece(params.CapPieces, free.Hi - keptHi, slot, length))
            row.push_back(CapAt(keptHi + length * 0.5f, false, slot, cells));
    }
    return row;
}

// Where one crest piece stands: on the top of the span under it.
//
// The span's own frame carries the answer for every grade — a stepped span is
// level, a sheared one's forward holds the stretch that lands it on its higher
// station — so the piece is laid along that span rather than along the two
// stations it joins, and a sheared forward is normalized before it becomes the
// piece's along axis. The piece is whole: its scale is 1, and only the pivot
// compensation every other piece takes moves it off the top line. Its centre is
// a crest-axis distance, so it lands in metres along the span under it.
bool CrestPoseOnRun(const Run& run, const CrestAxis& axis, const FenceLayoutParams& params,
                    const CrestPlacement& placement, const FencePieceBounds& piece,
                    TilePose& outPose)
{
    if (run.SpanPoses.empty())
        return false;
    const auto upper =
        std::upper_bound(axis.Stations.begin(), axis.Stations.end(), placement.Centre);
    const size_t after = static_cast<size_t>(upper - axis.Stations.begin());
    const size_t index = std::min(after == 0u ? size_t{0} : after - 1u, run.SpanPoses.size() - 1u);
    const TilePose& span = run.SpanPoses[index];

    V3 from;
    V3 to;
    SpanEndBases(run.Poses[index], run.Poses[index + 1u], params.SpanGrade, from, to);
    const float32 along = axis.Stations[index + 1u] - axis.Stations[index];
    const float32 u = std::clamp((placement.Centre - axis.Stations[index]) /
                                     std::max(along, kMinRunLengthMetres),
                                 0.0f, 1.0f);

    TilePose pose;
    pose.Forward = NormalizedOrFallback(span.Forward, kWorldForward);
    if (placement.FacesBack)
        pose.Forward = pose.Forward * -1.0f;
    pose.Right = NormalizedOrFallback(V3::Cross(span.Up, pose.Forward), span.Right);
    pose.Up = NormalizedOrFallback(V3::Cross(pose.Forward, pose.Right), span.Up);
    pose.Base = from + (to - from) * u +
                span.Up * SpanTopAboveBase(SpanPieceOf(run, index, params), params.PlantMode);
    const PieceBasis basis = MakePieceBasis(pose, piece.Axis());
    pose.Position = pose.Base - basis.LocalX * piece.Center.x - basis.LocalZ * piece.Center.z;
    outPose = pose;
    return true;
}

// Where a crest registered to its span stands: in that span's own frame — a
// sheared span's skewed forward included — lifted to the span mesh's top and
// at the span's LengthScale, so its two ends are the span's two ends and the
// row closes wherever the wall does.
//
// A mitred span's crest takes the span's two planes into its own frame: the
// station stands as far from the crest's centre as from the span's, and at the
// wall top's depth below the crest's own base.
void PlaceCrestOnSpan(const Run& run, const FenceLayoutParams& params, size_t spanIndex,
                      const FencePieceBounds& piece, FenceCrest& crest)
{
    TilePose span;
    FenceEndPlane spanStart;
    FenceEndPlane spanEnd;
    ShapeEmittedSpan(run, spanIndex, params, span, crest.LengthScale, spanStart, spanEnd);

    const FencePieceBounds& spanPiece = SpanPieceOf(run, spanIndex, params);
    const float32 top = SpanTopAboveBase(spanPiece, params.PlantMode);
    TilePose& pose = crest.Pose;
    pose.Right = span.Right;
    pose.Up = span.Up;
    pose.Forward = span.Forward;
    pose.Base = span.Base + span.Up * top;
    const PieceBasis basis = MakePieceBasis(pose, piece.Axis());
    const PieceAxisScale scale = MakePieceAxisScale(piece.Axis(), crest.LengthScale);
    pose.Position = pose.Base - basis.LocalX * (piece.Center.x * scale.X) -
                    basis.LocalZ * (piece.Center.z * scale.Z);

    const float32 endDifference = 0.5f * (piece.Length() - spanPiece.Length());
    crest.Start = spanStart;
    crest.End = spanEnd;
    crest.Start.StationInset += endDifference;
    crest.End.StationInset += endDifference;
    crest.Start.StationLocalY = -top;
    crest.End.StationLocalY = -top;
    if (crest.Start.Mitred)
        SetTopInset(crest.Start, true, piece, crest.LengthScale);
    else
        crest.Start = FenceEndPlane{};
    if (crest.End.Mitred)
        SetTopInset(crest.End, false, piece, crest.LengthScale);
    else
        crest.End = FenceEndPlane{};
}

// What one crest row build owes the author beyond the pieces themselves: how
// many cells actually stood, the widest stretch a pitch-grid cell was offered,
// and which kind of run could hold a cell with nothing reserved. The caller
// owns every message, because structure may have spent the budget first and
// one cut is one message.
struct CrestRowReport
{
    uint32 CellsPlaced = 0;
    // Pitch-grid runs only: it is the stretch a shorter piece is measured
    // against, and a registered run measured it without the posts.
    float32 LongestGridStretch = 0.0f;
    // A pitch-grid run somewhere is at least one pitch long on the crest axis,
    // so bare it holds its first cell. It separates "the reservations took the
    // room" — invisible to the author — from "the run is shorter than its
    // piece", which the wall itself shows.
    bool AGridRunHoldsACell = false;
    // A run registered to its spans holds a crest on every span bare, and only
    // a caller's reservation removes one: the posts reserve nothing against it.
    bool ARegisteredRunHoldsACell = false;
    bool WithinBudget = true;
};

// The crest row over every run.
CrestRowReport EmitCrestRow(const std::vector<Run>& runs, const FenceLayoutParams& params,
                            const CrestPitch& pitch, uint32& pieceBudget,
                            std::vector<FenceCrest>& out)
{
    CrestRowReport report;
    uint32 cellBase = 0;
    CrestAxis axis;
    for (const Run& run : runs)
    {
        BuildCrestAxis(run, params.SpanGrade, axis);
        CrestCells cells;
        cells.Pitch = pitch.Metres;
        if (CrestRegistersToSpans(run, params, pitch))
        {
            cells.SpanStations = &axis.Stations;
            cells.Count = static_cast<uint32>(run.SpanPoses.size());
        }
        else
        {
            cells.Count = CrestCellCount(axis.LengthMetres, pitch.Metres);
        }
        float32 runLongestFree = 0.0f;
        const std::vector<CrestPlacement> placements =
            SolveRunCrest(run, axis, params, cells, cellBase, runLongestFree);
        if (cells.RegisteredToSpans())
        {
            report.ARegisteredRunHoldsACell = report.ARegisteredRunHoldsACell || cells.Count > 0u;
        }
        else
        {
            report.LongestGridStretch = std::max(report.LongestGridStretch, runLongestFree);
            report.AGridRunHoldsACell = report.AGridRunHoldsACell ||
                                        axis.LengthMetres + kMinRunLengthMetres >= pitch.Metres;
        }
        for (const CrestPlacement& placement : placements)
        {
            const bool onSpan = cells.RegisteredToSpans() && !placement.IsCap;
            const FencePieceBounds& piece = placement.IsCap
                                                ? params.CapPieces[placement.PoolSlot]
                                                : params.CrestPieces[placement.PoolSlot];
            FenceCrest crest;
            if (onSpan)
                PlaceCrestOnSpan(run, params, placement.Ordinal, piece, crest);
            else if (!CrestPoseOnRun(run, axis, params, placement, piece, crest.Pose))
                continue;
            if (pieceBudget == 0u)
            {
                report.WithinBudget = false;
                return report;
            }
            --pieceBudget;
            if (!placement.IsCap)
                ++report.CellsPlaced;

            crest.PoolSlot = placement.PoolSlot;
            crest.IsCap = placement.IsCap;
            crest.Index = cellBase + placement.Ordinal;
            crest.Run = run.StartPoint;
            crest.OrdinalInRun = placement.Ordinal;
            out.push_back(crest);
        }
        // The base counts through a run that emitted nothing, so a pick answers
        // to the geometry and not to what a reservation removed.
        cellBase += cells.Count;
    }
    return report;
}

std::string RunName(uint32 startPoint, uint32 endPoint)
{
    return "run " + std::to_string(startPoint) + "->" + std::to_string(endPoint);
}

// The recipe's overrides, sorted to the runs they name: one list per declared
// run, indexed by the authored point opening it. An override on a point that
// opens no run, and a second override on a span that already has one, are
// reported here and read by nothing — the first override written on a span is
// the one the author sees applied.
std::vector<std::vector<RunOverride>> CollectRunOverrides(
    std::span<const Components::SplineSpanOverride> authored, size_t declaredRuns, bool closed,
    std::vector<std::string>& validation)
{
    std::vector<std::vector<RunOverride>> byRun(declaredRuns);
    for (const Components::SplineSpanOverride& entry : authored)
    {
        if (entry.Kind == Components::SplineSpanOverrideKind::None)
            continue;
        if (entry.PointIndex >= declaredRuns)
        {
            validation.push_back("Fence: a span override names point " +
                                 std::to_string(entry.PointIndex) +
                                 ", which opens no run — it places nothing; move it or remove it");
            continue;
        }
        std::vector<RunOverride>& run = byRun[entry.PointIndex];
        if (FindRunOverride(run, entry.SpanOrdinal))
        {
            const uint32 endPoint = (closed && entry.PointIndex + 1u == declaredRuns)
                                        ? 0u
                                        : entry.PointIndex + 1u;
            validation.push_back("Fence: two span overrides name span " +
                                 std::to_string(entry.SpanOrdinal) + " of " +
                                 RunName(entry.PointIndex, endPoint) + " — the first is used");
            continue;
        }
        run.push_back({entry.SpanOrdinal, entry.Kind, entry.PoolSlot});
    }
    return byRun;
}

// What a run's overrides could not do once its fill is solved: name a span the
// run does not have, or a pool slot the pool does not hold. Each is said once
// per rebuild; none is dropped in silence. A gate over an EMPTY gate pool is not
// a miss — it is an opening, which is what an empty pool means.
void ReportRunOverrides(const Run& run, std::span<const RunOverride> overrides,
                        const FenceLayoutParams& params, std::vector<std::string>& validation)
{
    const size_t spans = run.Fill.Slots.size();
    // Built only when there is something to say: this runs on every rebuild, a
    // knot drag included. Pool pieces are named as their pool rows are, from 1.
    const auto spanName = [&run](const RunOverride& entry)
    { return "span " + std::to_string(entry.Ordinal) + " of " + RunName(run.StartPoint, run.EndPoint); };
    for (const RunOverride& entry : overrides)
    {
        if (entry.Ordinal >= spans)
        {
            validation.push_back("Fence: a span override names " + spanName(entry) + ", which has " +
                                 std::to_string(spans) +
                                 " span(s) — it places nothing; move it or remove it");
            continue;
        }
        if (entry.Kind == Components::SplineSpanOverrideKind::Gate && !params.GatePieces.empty() &&
            entry.PoolSlot >= params.GatePieces.size())
        {
            validation.push_back("Fence: the gate on " + spanName(entry) + " names Gate " +
                                 std::to_string(entry.PoolSlot + 1u) + ", but the Gate pool holds " +
                                 std::to_string(params.GatePieces.size()) +
                                 " — the span is left open");
        }
        else if (entry.Kind == Components::SplineSpanOverrideKind::ExplicitPiece &&
                 entry.PoolSlot >= params.SpanPieces.size())
        {
            validation.push_back("Fence: the explicit piece on " + spanName(entry) + " names Span " +
                                 std::to_string(entry.PoolSlot + 1u) + ", but the Span pool holds " +
                                 std::to_string(params.SpanPieces.size()) +
                                 " — it keeps the fence's own pick");
        }
    }
}

float32 SanitizedFloat(float32 value, float32 fallback)
{
    return std::isfinite(value) ? value : fallback;
}

} // namespace

Components::SplineFence SanitizeFenceRecipe(const Components::SplineFence& authored)
{
    const Components::SplineFence defaults{};
    Components::SplineFence clean = authored;
    clean.PostPitch = std::max(SanitizedFloat(authored.PostPitch, defaults.PostPitch),
                               kMinPitchMetres);
    clean.SlopeBlend =
        std::clamp(SanitizedFloat(authored.SlopeBlend, defaults.SlopeBlend), 0.0f, 1.0f);
    clean.SpanMaxStretch = std::max(
        SanitizedFloat(authored.SpanMaxStretch, defaults.SpanMaxStretch), 1.0f);
    // A negative pitch spaces nothing, so it reads as the default 0 — the
    // longest crest piece's own length — exactly as a non-finite one does.
    clean.CrestPitch =
        std::max(SanitizedFloat(authored.CrestPitch, defaults.CrestPitch), 0.0f);
    return clean;
}

FenceLayoutResult BuildFenceLayout(const std::vector<CenterSample>& center,
                                   const FenceLayoutParams& params)
{
    FenceLayoutResult result;
    if (center.size() < 2u || params.RunBoundaries.size() < 2u)
        return result;

    // Hold-then-split. The gap patch runs on the WHOLE draped polyline before
    // it is cut into runs, then every sample counts as measured: a run that
    // starts inside a terrain hole must inherit the altitude its neighbour ends
    // on, and a per-run patch would re-anchor it to the first measured sample
    // that run happens to contain.
    std::vector<CenterSample> held(center);
    HoldSurfaceAcrossGaps(held);
    for (CenterSample& sample : held)
        sample.SurfaceValid = true;

    const float32 pitch = std::max(params.PostPitch, kMinPitchMetres);
    const float32 maxStretch = std::max(params.SpanMaxStretch, 1.0f);
    const bool explicitMinScale = std::isfinite(params.SpanMinScale) && params.SpanMinScale > 0.0f;
    const float32 minScale = explicitMinScale ? std::min(params.SpanMinScale, 1.0f)
                                            : 1.0f / maxStretch;
    const size_t declaredRuns = params.RunBoundaries.size() - 1u;

    // ---- Pass A: slice each run and solve its fill. -----------------------
    const std::vector<std::vector<RunOverride>> overridesByRun =
        CollectRunOverrides(params.SpanOverrides, declaredRuns, params.Closed, result.Validation);
    std::vector<Run> runs;
    runs.reserve(declaredRuns);
    result.RunSpanCounts.assign(declaredRuns, 0u);
    uint32 spanOrdinal = 0;
    for (size_t r = 0; r < declaredRuns; ++r)
    {
        const float32 u0 = params.RunBoundaries[r];
        const float32 u1 = params.RunBoundaries[r + 1u];

        Run run;
        run.StartPoint = static_cast<uint32>(r);
        run.EndPoint = (params.Closed && r + 1u == declaredRuns)
                           ? 0u
                           : static_cast<uint32>(r + 1u);
        if (u1 > u0)
        {
            run.Samples = SliceRun(held, u0, u1);
            run.LengthMetres = PolylineLength(run.Samples);
        }

        if (run.LengthMetres <= kMinRunLengthMetres)
        {
            result.Validation.push_back(
                "Fence: authored points " + std::to_string(run.StartPoint) + " and " +
                std::to_string(run.EndPoint) +
                " are coincident — the run between them is skipped and the station emitted once");
            // The run holds no span, so nothing its overrides name exists.
            if (!overridesByRun[r].empty())
            {
                result.Validation.push_back(
                    "Fence: " + std::to_string(overridesByRun[r].size()) + " span override(s) on " +
                    RunName(run.StartPoint, run.EndPoint) +
                    " place nothing — the run has no length; move point " +
                    std::to_string(run.EndPoint) + " or remove them");
            }
            continue;
        }

        run.Fill = SolveRunFill(run.LengthMetres, params.Seed, spanOrdinal, params.SpanPieces,
                                params.GatePieces, overridesByRun[r], maxStretch, minScale);
        // The fill answers for its own stretch. The reach a mitred end is laid
        // with is checked against the same cap where the joins are decided
        // (MitreJoinsOfRuns): the joins follow from the stations this fill
        // places, so the fill cannot count them without depending on its own
        // result.
        if (!run.Fill.Slots.empty() && !run.Fill.WithinCap)
        {
            // Which bound was missed decides the wording: a run that needs
            // 0.571x is under the floor, not "past the cap", and reporting the
            // cap for a compression sends the author to the wrong knob.
            const bool overCap = run.Fill.Stretch > maxStretch;
            const std::string bound =
                overCap ? "past the " + Factor(maxStretch) + "x cap"
                        : "under the " + Factor(minScale) + "x floor" +
                              (explicitMinScale ? " (explicit compression limit)"
                                                : " (the " + Factor(maxStretch) + "x cap's reciprocal)");
            result.Validation.push_back(
                "Fence: run " + std::to_string(run.StartPoint) + "->" +
                std::to_string(run.EndPoint) + " is " + Metres(run.LengthMetres) +
                " m and its closest fill of " + std::to_string(run.Fill.Slots.size()) +
                " span piece(s) is " + Metres(run.Fill.NaturalLength) + " m — needs " +
                Factor(run.Fill.Stretch) + "x, " + bound + "; no count within one of the nominal " +
                std::to_string(run.Fill.NominalCount) +
                " satisfies it, so the run is built at the required stretch");
        }

        // Stations: cumulative piece lengths scaled to close the run exactly on
        // both authored points, which are mandatory stations. With no span
        // pieces the walk falls back to the pitch.
        if (run.Fill.Slots.empty())
        {
            const int64 rounded = static_cast<int64>(std::lround(run.LengthMetres / pitch)) + 1;
            const uint32 count = static_cast<uint32>(std::max<int64>(int64{2}, rounded));
            run.Stations.reserve(count);
            for (uint32 k = 0; k < count; ++k)
                run.Stations.push_back(run.LengthMetres * static_cast<float32>(k) /
                                       static_cast<float32>(count - 1u));
        }
        else
        {
            run.Stations.reserve(run.Fill.Slots.size() + 1u);
            float32 cumulative = 0.0f;
            run.Stations.push_back(0.0f);
            for (size_t k = 0; k < run.Fill.Slots.size(); ++k)
            {
                cumulative += std::max(SpanPieceOf(run, k, params).Length(), kMinPieceLengthMetres);
                run.Stations.push_back(run.LengthMetres * cumulative / run.Fill.NaturalLength);
            }
            run.Stations.back() = run.LengthMetres;
            spanOrdinal += static_cast<uint32>(run.Fill.Slots.size());
        }
        ReportRunOverrides(run, overridesByRun[r], params, result.Validation);
        result.RunSpanCounts[r] = static_cast<uint32>(run.Fill.Slots.size());

        runs.push_back(std::move(run));
    }

    if (runs.empty())
        return result;

    // ---- Pass B: each run's end directions, with no probe. ----------------
    // The chord is a pure function of the polyline, so this costs no rays; it
    // exists only to feed the bisector, and running it through the same layout
    // keeps the chord rule in one place.
    TileLayoutParams stationTemplate;
    stationTemplate.Spacing = pitch;
    stationTemplate.AlignToSurfaceNormal = params.AlignToSurfaceNormal;
    stationTemplate.SlopeBlend = params.SlopeBlend;
    stationTemplate.MeshBoundsCenter = params.PostPiece.Center;
    stationTemplate.MeshBoundsHalfExtents = params.PostPiece.HalfExtents;
    stationTemplate.PlantMode = params.PlantMode;
    stationTemplate.MaxTiles = params.MaxPieces;
    stationTemplate.MaxTiltDegrees = params.MaxTiltDegrees;
    // OutTiltClamp is deliberately NOT on the template: pass B builds throwaway
    // end poses from the same params, and counting those would double-report
    // every run's two ends. Only the real station pass reports.

    for (Run& run : runs)
    {
        const float32 ends[2] = {0.0f, run.LengthMetres};
        TileLayoutParams endParams = stationTemplate;
        endParams.StationDistances = std::span<const float32>(ends, 2u);
        const std::vector<TilePose> endPoses = BuildTilePoses(run.Samples, endParams);
        if (endPoses.size() == 2u)
        {
            run.StartForward = GroundDirection(endPoses[0].Forward, run.StartForward);
            run.EndForward = GroundDirection(endPoses[1].Forward, run.EndForward);
        }
    }

    // ---- Pass C: bisector yaw at every shared station. --------------------
    // Each run's own clamped chord would give the same physical post two
    // different answers, which shows on any non-square post and on wall towers.
    for (Run& run : runs)
        run.StationForwards.assign(run.Stations.size(), V3(0.0f, 0.0f, 0.0f));

    // A closed spline joins its last run back to its first, so every one of its
    // stations is a two-run corner.
    const size_t sharedJoints = params.Closed ? runs.size() : runs.size() - 1u;
    for (size_t joint = 0; joint < sharedJoints; ++joint)
    {
        const size_t next = (joint + 1u) % runs.size();
        if (next == joint)
            continue;
        const V3 bisector = NormalizedOrFallback(
            runs[joint].EndForward + runs[next].StartForward, runs[joint].EndForward);
        runs[joint].StationForwards.back() = bisector;
        runs[next].StationForwards.front() = bisector;
    }

    // A node several splines meet at has to face one way in every one of their
    // layouts, and no single spline's bisector can know that. The caller that
    // owns the network says so; an entry it left at zero keeps the bisector.
    if (!params.AuthoredPointForwards.empty())
    {
        for (Run& run : runs)
        {
            ApplyAuthoredForward(params.AuthoredPointForwards, run.StartPoint,
                                 run.StationForwards.front());
            ApplyAuthoredForward(params.AuthoredPointForwards, run.EndPoint,
                                 run.StationForwards.back());
        }
    }

    // ---- Pass D: the real station poses, probed. --------------------------
    for (Run& run : runs)
    {
        TileLayoutParams runParams = stationTemplate;
        runParams.StationDistances = std::span<const float32>(run.Stations);
        runParams.StationForwards = std::span<const V3>(run.StationForwards);
        runParams.Probe = params.Probe;
        runParams.OutTiltClamp = params.OutTiltClamp;
        run.Poses = BuildTilePoses(run.Samples, runParams);
    }

    // ---- Pass D2: span frames. --------------------------------------------
    // Resolved before emit because the bury check needs the pose that is
    // actually placed, not the post-to-post line it used to assume.
    for (Run& run : runs)
    {
        const size_t spanCount =
            run.Poses.size() < 2u ? size_t{0}
                                  : std::min(run.Fill.Slots.size(), run.Poses.size() - 1u);
        run.SpanPoses.reserve(spanCount);
        run.SpanLengthScales.reserve(spanCount);
        for (size_t k = 0; k < spanCount; ++k)
        {
            float32 lengthScale = 1.0f;
            bool rollBorrowed = false;
            run.SpanPoses.push_back(MakeSpanPose(run.Poses[k], run.Poses[k + 1u], params.SpanGrade,
                                                 params.PlantMode, SpanPieceOf(run, k, params),
                                                 lengthScale, rollBorrowed));
            run.SpanLengthScales.push_back(lengthScale);
            run.SpanRollBorrowed.push_back(rollBorrowed ? 1u : 0u);
            if (rollBorrowed)
                ++run.RollBorrowedSpans;
        }
    }

    // ---- Pass D3: span joins. ---------------------------------------------
    // Needs the frames of the spans on both sides of every station, so it
    // follows the span pass; what it decides is applied where each span is
    // emitted.
    MitreBareJoins(runs, params, result.Validation);

    // ---- Pass E: emit. ----------------------------------------------------
    // A shared station belongs to both runs; it is emitted with the run that
    // opens it, so the run that closes on it contributes only its spans.
    uint32 stationIndex = 0;
    uint32 spanIndex = 0;
    uint32 pieceBudget = params.MaxPieces;
    // Two different cuts, two latches: sharing one let whichever fired first
    // silence the other, and they are not the same problem.
    bool budgetReported = false;
    bool truncationReported = false;
    bool shortRunReported = false;
    // True while another piece may be emitted; reports the cut exactly once.
    const auto takePieceBudget = [&]()
    {
        if (pieceBudget > 0u)
        {
            --pieceBudget;
            return true;
        }
        if (!budgetReported)
        {
            budgetReported = true;
            result.Validation.push_back(
                "Fence: hit the " + std::to_string(params.MaxPieces) +
                " piece budget; the rest of the spline is not built — raise the post pitch or "
                "shorten the spline");
        }
        return false;
    };

    for (const Run& run : runs)
    {
        if (run.Poses.size() < 2u)
        {
            if (!shortRunReported)
            {
                shortRunReported = true;
                result.Validation.push_back(
                    "Fence: run " + std::to_string(run.StartPoint) + "->" +
                    std::to_string(run.EndPoint) +
                    " produced no usable stations and contributes nothing — its centerline has no "
                    "measurable length after draping");
            }
            continue;
        }
        // The station walk has its own budget, and a run long enough to exhaust
        // it comes back short. Emit what it produced and say so — dropping the
        // run on the mismatch would lose it silently, which is the one outcome
        // every other rule here exists to prevent.
        if (run.Poses.size() != run.Stations.size() && !truncationReported)
        {
            truncationReported = true;
            result.Validation.push_back(
                "Fence: run " + std::to_string(run.StartPoint) + "->" +
                std::to_string(run.EndPoint) + " needed " +
                std::to_string(run.Stations.size()) + " stations but the layout budget allows " +
                std::to_string(run.Poses.size()) + " — raise the post pitch");
        }

        const size_t emitCount = run.Poses.size() - 1u;
        for (size_t k = 0; k < emitCount; ++k)
        {
            if (!takePieceBudget())
                break;
            FenceStation station;
            station.Pose = run.Poses[k];
            station.Index = stationIndex++;
            station.IsAuthoredPoint = (k == 0u);
            station.AuthoredPoint = run.StartPoint;
            result.Stations.push_back(station);
        }

        // Spans the emission reached, openings included: an opening places no
        // piece and spends no budget, but it keeps its ordinal.
        size_t spansEmitted = 0;
        for (size_t k = 0; k < run.SpanPoses.size(); ++k)
        {
            if (IsOpening(run, k))
            {
                ++spanIndex;
                ++spansEmitted;
                continue;
            }
            if (!takePieceBudget())
                break;
            FenceSpan span;
            span.PoolSlot = run.Fill.Slots[k];
            span.IsGate = run.Fill.Sources[k] == SpanSource::Gate;
            ShapeEmittedSpan(run, k, params, span.Pose, span.LengthScale, span.Start, span.End);
            span.Index = spanIndex++;
            span.Run = run.StartPoint;
            span.OrdinalInRun = static_cast<uint32>(k);
            result.Spans.push_back(span);
            ++spansEmitted;
        }
        result.RollBorrowedSpans += run.RollBorrowedSpans;

        // Stepped risers need a station tall enough to cover them; with no post
        // mesh the span's own height is the cover.
        if (params.SpanGrade == Components::SplineSpanGrade::Stepped && !run.Fill.Slots.empty())
        {
            float32 worstStep = 0.0f;
            for (size_t k = 0; k + 1u < run.Poses.size(); ++k)
                worstStep = std::max(worstStep,
                                     std::abs(run.Poses[k + 1u].Base.y - run.Poses[k].Base.y));
            float32 cover = params.HasPostMesh ? params.PostPiece.Height() : 0.0f;
            if (cover <= 0.0f)
                cover = SpanPieceOf(run, 0u, params).Height();
            if (cover > 0.0f && worstStep > cover)
            {
                result.Validation.push_back(
                    "Fence: run " + std::to_string(run.StartPoint) + "->" +
                    std::to_string(run.EndPoint) + " steps " + Metres(worstStep) +
                    " m between stations, more than the " + Metres(cover) +
                    " m piece height covers — the stepped grade leaves a gap at the riser");
            }
        }

        // Buried spans: the run's own draped samples are already dense, so
        // checking whether the ground rises past what a span is planted for is
        // nearly free. Reporting it is v1; forcing a station at the bump is not.
        //
        // The line comes from the SPAN'S OWN POSE and its own pool piece, not
        // from the line between its two posts. A span that borrowed its roll
        // does not lie on that line — it is level across a face the posts climb
        // — so the post-to-post model would call it clear while the panel is
        // buried in the slope for most of its length.
        //
        // The walk keeps the WORST sample rather than the first past tolerance:
        // what an author acts on is the one station that has to be added, and
        // the depth there is what says whether it is worth adding.
        float32 cumulative = 0.0f;
        uint32 buriedSamples = 0;
        float32 worstDepth = 0.0f;
        size_t worstSpanOrdinal = 0;
        float32 worstArcMetres = 0.0f;
        V3 worstPos(0.0f, 0.0f, 0.0f);
        for (size_t i = 1; i + 1u < run.Samples.size(); ++i)
        {
            cumulative += VectorLength(run.Samples[i].Pos - run.Samples[i - 1u].Pos);
            const auto upper = std::upper_bound(run.Stations.begin(), run.Stations.end(), cumulative);
            if (upper == run.Stations.begin() || upper == run.Stations.end())
                continue;
            const size_t hi = static_cast<size_t>(upper - run.Stations.begin());
            if (hi > spansEmitted || hi > run.SpanPoses.size() || IsOpening(run, hi - 1u))
                continue;
            // Slots are parallel to SpanPoses, and each names the pool piece
            // this span actually draws — which is where its skirt comes from.
            // A run mixes pool pieces freely, so the skirt is per span, never
            // per run.
            const FencePieceBounds& piece = SpanPieceOf(run, hi - 1u, params);
            const float32 allowed = SpanGroundAllowanceAt(run.SpanPoses[hi - 1u], piece,
                                                          params.PlantMode, run.Samples[i].Pos);
            const float32 depth = run.Samples[i].Pos.y - allowed;
            if (depth <= kBuryToleranceMetres)
                continue;
            ++buriedSamples;
            // Ties go to the first such sample, in walk order along the run.
            if (buriedSamples == 1u || depth > worstDepth)
            {
                worstDepth = depth;
                worstSpanOrdinal = hi - 1u;
                worstArcMetres = cumulative;
                worstPos = run.Samples[i].Pos;
            }
        }
        if (buriedSamples > 0u)
        {
            result.Validation.push_back(
                "Fence: the ground rises through the spans of run " +
                std::to_string(run.StartPoint) + "->" + std::to_string(run.EndPoint) + " at " +
                std::to_string(buriedSamples) + " sample(s) — worst at span " +
                std::to_string(worstSpanOrdinal) + ", " + Metres(worstDepth) +
                " m deeper than the piece is planted for, " + Metres(worstArcMetres) +
                " m along the run at (" + Metres(worstPos.x) + ", " + Metres(worstPos.z) +
                ") — add an authored point there so a station lands on the bump, or the fence is "
                "buried at that span");
        }
    }

    // Emission is in order, so where the budget cut it the one span whose
    // partner is missing is the last emitted, and on a closed spline the first,
    // whose partner is the loop's last span.
    if (budgetReported && !result.Spans.empty())
    {
        if (result.Spans.back().End.Mitred)
            SquareUnpairedEnd(runs, params, false, result.Spans.back());
        if (params.Closed && result.Spans.front().Start.Mitred)
            SquareUnpairedEnd(runs, params, true, result.Spans.front());
    }

    // An open spline's final station closes the last run and belongs to no
    // following one; a closed spline's would be its first station again.
    if (!params.Closed)
    {
        const Run& last = runs.back();
        if (last.Poses.size() >= 2u && takePieceBudget())
        {
            FenceStation station;
            station.Pose = last.Poses.back();
            station.Index = stationIndex++;
            station.IsAuthoredPoint = true;
            station.AuthoredPoint = last.EndPoint;
            result.Stations.push_back(station);
        }
    }

    // ---- Pass F: the crest row. -------------------------------------------
    // After every station and span of every run, not beside them: one piece
    // budget, and decoration is what it cuts before structure.
    if (!params.CrestPieces.empty())
    {
        const CrestPitch crest = ResolveCrestPitch(params.CrestPieces, params.CrestPitch);
        if (crest.RaisedToPieceLength)
        {
            result.Validation.push_back(
                "Fence: Crest Pitch " + Metres(params.CrestPitch) +
                " m is below the longest crest piece, " + Metres(crest.LongestPiece) +
                " m; the piece length is used");
        }
        if (params.SpanPieces.empty())
        {
            result.Validation.push_back(
                "Fence: the crest row is laid along the tops of the spans and the span pool is "
                "empty — no crest piece is placed");
        }
        else
        {
            const CrestRowReport row =
                EmitCrestRow(runs, params, crest, pieceBudget, result.Crests);
            if (!row.WithinBudget && !budgetReported)
            {
                budgetReported = true;
                result.Validation.push_back(
                    "Fence: hit the " + std::to_string(params.MaxPieces) +
                    " piece budget; the rest of the crest row is not built — raise the crest "
                    "pitch or shorten the spline");
            }
            // A pool was assigned, a run was long enough to hold a cell with
            // nothing reserved, and not one cell stood anywhere: the posts or
            // the caller's pieces took the room. That is invisible in the
            // result — the wall simply has no row — so it is reported once with
            // the numbers that decide it and the ways out. The shorter-piece
            // advice is pitch-grid advice; a row registered to its walls lost
            // them to the caller's pieces alone.
            //
            // A run SHORTER than a pitch is not this case and stays silent,
            // with or without posts, as the pitch ceiling has always been: there
            // the wall itself shows the author why, and a message per short run
            // is noise.
            else if (row.WithinBudget && row.CellsPlaced == 0u && row.AGridRunHoldsACell)
            {
                result.Validation.push_back(EmptyCrestRowReport(
                    row.LongestGridStretch, crest.Asked, params.HasPostMesh));
            }
            else if (row.WithinBudget && row.CellsPlaced == 0u && row.ARegisteredRunHoldsACell)
            {
                result.Validation.push_back(
                    "Fence: the crest row placed no crest pieces — a reserved stretch reaches "
                    "every wall they stand on; move what reserves the run.");
            }
        }
    }

    return result;
}

} // namespace GameEngine::SplineLayout
