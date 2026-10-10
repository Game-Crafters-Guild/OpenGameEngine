#include "Placement/SplineSweepStations.h"

#include "Spline/SplineData.h"
#include "Spline/SplineEvaluator.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>

namespace GameEngine::Editor
{
namespace
{

using Mathematics::Vector3;
namespace SG = GameEngine::SplineGeometry;

// A uniform sample this close to an authored corner, in local metres, IS the
// corner: it is moved onto the point rather than joined by a second sample a
// hair away.
constexpr float32 kCornerSnapMetres = 1.0e-4f;

constexpr float32 kPi = 3.14159265358979323846f;

// Parametric distance either side of an authored point at which a smooth
// spline's incoming and outgoing tangents are read, per segment.
constexpr float32 kTangentProbeSegments = 1.0e-3f;

// One sample while the centreline is assembled.
struct Sample
{
    Spline::SplineFrame Frame;
    float32 LocalDistance = 0.0f;
    // Parametric position on the spline, which orders samples consistently with
    // their positions: arc distance goes through the arc-length table, whose
    // interpolation can place a sample a few millimetres off along the run near
    // an authored point.
    float32 Parameter = 0.0f;
    bool Probed = true;
    int32 CornerPoint = -1;
    // The authored point this sample stands on, corner or not.
    int32 AuthoredPoint = -1;
};

[[nodiscard]] float32 Length(const Vector3& v)
{
    return std::sqrt(Vector3::Dot(v, v));
}

// Whether a smooth spline's point `point` is a corner: its tangent turns in the
// level plane by more than kSweepCornerDeflectionDegrees across it. A welded
// loop's point 0 reads its incoming tangent at the loop's end.
[[nodiscard]] bool TurnsAtPoint(const Spline::SplineData& data, uint32 point,
                                const Mathematics::Matrix4x4& placerWorld)
{
    const uint32 segments = data.GetSegmentCount();
    const float32 parameter = static_cast<float32>(point) / static_cast<float32>(segments);
    const float32 probe = kTangentProbeSegments / static_cast<float32>(segments);
    const float32 before = point == 0u ? 1.0f - probe : parameter - probe;
    const Vector3 origin = placerWorld.TransformPoint(Vector3(0.0f, 0.0f, 0.0f));
    const auto levelTangent = [&](float32 t)
    {
        const Vector3 world = placerWorld.TransformPoint(Spline::Evaluate(data, t).Forward) - origin;
        return NormalizedOrFallback(Vector3(world.x, 0.0f, world.z), Vector3(0.0f, 0.0f, 1.0f));
    };
    const float32 cosine = std::clamp(Vector3::Dot(levelTangent(before), levelTangent(parameter + probe)),
                                      -1.0f, 1.0f);
    return std::acos(cosine) > kSweepCornerDeflectionDegrees * kPi / 180.0f;
}

// Put a sample on every interior authored point that is a corner — all of a
// Linear spline's, and a smooth spline's where its tangent turns (TurnsAtPoint)
// — or on every interior point when `everyPoint` asks.
void InsertPointSamples(const Spline::SplineData& data, const Mathematics::Matrix4x4& placerWorld,
                        bool corners, bool everyPoint, std::vector<Sample>& samples)
{
    const bool linear = data.Type == Spline::SplineType::Linear;
    const uint32 segments = data.GetSegmentCount();
    for (uint32 point = 1; point < segments; ++point)
    {
        const bool corner = corners && (linear || TurnsAtPoint(data, point, placerWorld));
        if (!corner && !everyPoint)
            continue;
        const float32 parameter = static_cast<float32>(point) / static_cast<float32>(segments);
        const Vector3 position = data.Points[point].Position;

        const auto after = std::find_if(samples.begin(), samples.end(), [&](const Sample& s)
                                        { return s.Parameter >= parameter; });
        const auto snaps = [&](std::vector<Sample>::iterator it)
        { return it != samples.end() && Length(it->Frame.Position - position) <= kCornerSnapMetres; };
        auto target = snaps(after) ? after
                      : (after != samples.begin() && snaps(after - 1)) ? after - 1
                                                                       : samples.end();
        if (target != samples.end())
        {
            target->Frame.Position = position;
            target->CornerPoint = corner ? static_cast<int32>(point) : -1;
            target->AuthoredPoint = static_cast<int32>(point);
            continue;
        }

        Sample onPoint;
        onPoint.Frame = Spline::Evaluate(data, parameter);
        onPoint.Frame.Position = position;
        onPoint.LocalDistance = Spline::ParametricToDistance(data, parameter);
        onPoint.Parameter = parameter;
        onPoint.Probed = false;
        onPoint.CornerPoint = corner ? static_cast<int32>(point) : -1;
        onPoint.AuthoredPoint = static_cast<int32>(point);
        samples.insert(after, onPoint);
    }
}

// Append the samples that bring the chord from `from` to `to` within the
// sagitta of the curve, halving the interval while its midpoint on the curve
// stands further than that from the chord's own midpoint. `halvingsLeft` bounds
// the depth, and so the samples one interval can mint.
void AppendSagittaSamples(const Spline::SplineData& data,
                          const Mathematics::Matrix4x4& placerWorld, const Sample& from,
                          const Sample& to, uint32 halvingsLeft, std::vector<Sample>& out)
{
    if (halvingsLeft == 0u)
        return;
    Sample middle;
    middle.LocalDistance = (from.LocalDistance + to.LocalDistance) * 0.5f;
    middle.Frame = Spline::EvaluateAtDistance(data, middle.LocalDistance);
    const Vector3 chordMiddle = (placerWorld.TransformPoint(from.Frame.Position) +
                                 placerWorld.TransformPoint(to.Frame.Position)) * 0.5f;
    const float32 deviation =
        Length(placerWorld.TransformPoint(middle.Frame.Position) - chordMiddle);
    if (!(deviation > kSweepSagittaMetres))
        return;
    middle.Parameter = Spline::DistanceToParametric(data, middle.LocalDistance);
    middle.Probed = false;
    AppendSagittaSamples(data, placerWorld, from, middle, halvingsLeft - 1u, out);
    out.push_back(middle);
    AppendSagittaSamples(data, placerWorld, middle, to, halvingsLeft - 1u, out);
}

// Split every drape interval whose chord strays from the curve by more than
// the sagitta.
void InsertSagittaSamples(const Spline::SplineData& data, const Mathematics::Matrix4x4& placerWorld,
                          std::vector<Sample>& samples)
{
    std::vector<Sample> dense;
    dense.reserve(samples.size());
    for (size_t i = 0; i + 1u < samples.size(); ++i)
    {
        dense.push_back(samples[i]);
        AppendSagittaSamples(data, placerWorld, samples[i], samples[i + 1u],
                             kMaxSagittaHalvings, dense);
    }
    dense.push_back(samples.back());
    samples = std::move(dense);
}

// The ground's rise per metre of level travel, as a vector in the level plane:
// over a level displacement d the ground climbs Dot(rise, d). Read from the
// ground normal n: -(n - up (n·up)) / (n·up), no steeper than
// kMaxGroundedCrossSlope.
//
// A normal facing down is the same ground seen from its other side (a
// double-sided or inverted face the down-ray struck), so it is turned up
// rather than read as no slope. A normal lying in the level plane is a cliff
// face: it reads as the steepest slope sunk for.
[[nodiscard]] Vector3 GroundRise(Vector3 normal, const Vector3& up)
{
    if (Vector3::Dot(normal, up) < 0.0f)
        normal = normal * -1.0f;
    const float32 normalUp = std::max(Vector3::Dot(normal, up), 0.0f);
    const Vector3 level = normal - up * normalUp;
    const float32 levelLength = Length(level);
    if (!(levelLength > 0.0f))
        return Vector3(0.0f, 0.0f, 0.0f);
    if (levelLength > kMaxGroundedCrossSlope * normalUp)
        return level * (-kMaxGroundedCrossSlope / levelLength);
    return level * (-1.0f / normalUp);
}

// How far a station's grounded points sink: the fall of the ground from the
// station out to the lower of the base's two edges, each placed where the
// builder places it (along MitreRight on a mitred side). Both edges sink by
// it, so the downhill edge meets the ground and the uphill one stands in it.
[[nodiscard]] float32 GroundedBaseSink(const SG::SplineStripStation& station, const Vector3& rise)
{
    const Vector3 leftAxis =
        (station.MitreSides & SG::kSplineMitreLeftSide) != 0u ? station.MitreRight : station.Right;
    const Vector3 rightAxis =
        (station.MitreSides & SG::kSplineMitreRightSide) != 0u ? station.MitreRight : station.Right;
    const float32 leftFall = Vector3::Dot(rise, leftAxis * station.HalfWidthLeft);
    const float32 rightFall = -Vector3::Dot(rise, rightAxis * station.HalfWidthRight);
    return std::max({0.0f, leftFall, rightFall});
}

// Report the curves a sweep's wall folds on, once per segment between two
// authored points (SG::FindInnerFolds), on the stations as sampled: before the
// corners reshape them, with each corner and its two neighbours left out.
void ReportInnerFolds(const Spline::SplineData& data, const SweepSamples& samples,
                      std::span<const SG::SplineStripStation> stations, const Vector3& up,
                      std::vector<SG::SplineCornerIssue>& issues)
{
    std::vector<uint8> skip(stations.size(), 0u);
    for (const SG::SplineCornerSite& site : samples.Corners)
    {
        for (uint32 near = site.StationIndex == 0u ? 0u : site.StationIndex - 1u;
             near <= site.StationIndex + 1u && near < skip.size(); ++near)
            skip[near] = 1u;
    }
    const uint32 segments = data.GetSegmentCount();
    const uint32 pointCount = static_cast<uint32>(data.Points.size());
    uint32 lastReported = std::numeric_limits<uint32>::max();
    for (const uint32 station : SG::FindInnerFolds(stations, skip, up))
    {
        const float32 parameter = Spline::DistanceToParametric(data, samples.LocalDistance[station]);
        const uint32 segment = std::min(static_cast<uint32>(parameter * static_cast<float32>(segments)),
                                        segments > 0u ? segments - 1u : 0u);
        if (segment == lastReported)
            continue;
        lastReported = segment;
        const uint32 next = pointCount > 0u ? (segment + 1u) % pointCount : segment + 1u;
        issues.push_back({SG::SplineCornerIssueKind::InnerFold, segment, next, 0.0f});
    }
}

} // namespace

std::vector<std::string> CornerValidation(std::string_view recipeName,
                                          std::span<const SG::SplineCornerIssue> issues)
{
    std::vector<std::string> validation;
    validation.reserve(issues.size());
    for (const SG::SplineCornerIssue& issue : issues)
    {
        switch (issue.Kind)
        {
        case SG::SplineCornerIssueKind::PastMitreLimit:
            validation.push_back(std::format(
                "{}: point {} turns past the {:.0f}-degree mitre limit, so the corner "
                "is built round. Widen the turn to keep a mitred corner.",
                recipeName, issue.PointIndex, SG::kMitreLimitTurnDegrees));
            break;
        case SG::SplineCornerIssueKind::LegShorterThanReach:
            validation.push_back(std::format(
                "{}: point {} is too close to the end of the wall for its corner, so the inside face "
                "folds there. Lengthen the end leg or thin the wall.",
                recipeName, issue.PointIndex));
            break;
        case SG::SplineCornerIssueKind::InnerFold:
            validation.push_back(std::format(
                "{}: the curve between points {} and {} bends tighter than half the wall's "
                "thickness, so the inside face folds there. Widen the curve or thin the wall.",
                recipeName, issue.PointIndex, issue.OtherPointIndex));
            break;
        case SG::SplineCornerIssueKind::CornersOverlap:
            validation.push_back(std::format(
                "{}: points {} and {} turn the same way too close together for their corners, so the "
                "inside face folds between them. Lengthen the leg between them or thin the wall.",
                recipeName, issue.PointIndex, issue.OtherPointIndex));
            break;
        }
    }
    return validation;
}

SweepShape SweepShapeForExtrude()
{
    return SweepShape{};
}

SweepSamples SampleSweepCenterline(const Spline::SplineData& data, uint32 sampleCount,
                                   const Mathematics::Matrix4x4& placerWorld,
                                   const SweepShape& shape)
{
    std::vector<Spline::SplineFrame> uniform;
    Spline::SampleUniform(data, sampleCount, uniform);

    SweepSamples out;
    const size_t count = uniform.size();
    const float32 arcLength = data.TotalArcLength;
    const bool corners = shape.Corners != SG::SplineCornerStyle::None;
    const bool linear = data.Type == Spline::SplineType::Linear;
    const bool followCurve = shape.FollowCurve && !linear;
    const bool pointSamples = (corners && linear) || shape.StationAtPoints ||
                              (corners && followCurve);
    if (count < 2u || (!pointSamples && !followCurve))
    {
        // Channels are keyed by LOCAL arc distance and the frames are uniform in it.
        out.LocalDistance.resize(count);
        for (size_t i = 0; i < count; ++i)
            out.LocalDistance[i] =
                arcLength * static_cast<float32>(i) / static_cast<float32>(count - 1u);
        out.Probed.assign(count, 1u);
        out.Frames = std::move(uniform);
        return out;
    }

    // The arc distance SampleUniform placed each sample at, and its parameter.
    const float32 spacing = arcLength / static_cast<float32>(count - 1u);
    std::vector<Sample> samples(count);
    for (size_t i = 0; i < count; ++i)
    {
        samples[i].Frame = uniform[i];
        samples[i].LocalDistance =
            arcLength * static_cast<float32>(i) / static_cast<float32>(count - 1u);
        samples[i].Parameter =
            Spline::DistanceToParametric(data, static_cast<float32>(i) * spacing);
    }
    if (pointSamples)
        InsertPointSamples(data, placerWorld, corners, shape.StationAtPoints, samples);
    if (followCurve)
        InsertSagittaSamples(data, placerWorld, samples);

    // A welded loop's seam is a corner like any other: its first sample and
    // its last both stand on point 0.
    if (corners && shape.CloseLoop && data.IsEffectivelyClosed() &&
        (linear || TurnsAtPoint(data, 0u, placerWorld)))
        samples.front().CornerPoint = 0;

    out.Frames.reserve(samples.size());
    out.LocalDistance.reserve(samples.size());
    out.Probed.reserve(samples.size());
    for (const Sample& sample : samples)
    {
        if (sample.CornerPoint >= 0)
            out.Corners.push_back({static_cast<uint32>(out.Frames.size()),
                                   static_cast<uint32>(sample.CornerPoint)});
        if (shape.StationAtPoints && sample.AuthoredPoint > 0)
            out.Points.push_back(static_cast<uint32>(out.Frames.size()));
        out.Frames.push_back(sample.Frame);
        out.LocalDistance.push_back(sample.LocalDistance);
        out.Probed.push_back(sample.Probed ? 1u : 0u);
    }
    return out;
}

void DrapeInsertedSamples(const SweepSamples& samples, const Mathematics::Matrix4x4& placerWorld,
                          std::span<CenterSample> center)
{
    const size_t count = center.size();
    const auto authoredHeight = [&](size_t i)
    { return placerWorld.TransformPoint(samples.Frames[i].Position).y; };

    size_t before = count;
    for (size_t i = 0; i < count; ++i)
    {
        if (samples.Probed[i])
        {
            before = i;
            continue;
        }
        size_t after = i + 1u;
        while (after < count && !samples.Probed[after])
            ++after;
        if (before == count || after == count || !center[before].SurfaceValid ||
            !center[after].SurfaceValid)
        {
            center[i].SurfaceValid = false;
            continue;
        }
        const float32 span = samples.LocalDistance[after] - samples.LocalDistance[before];
        const float32 fraction =
            span > 0.0f ? (samples.LocalDistance[i] - samples.LocalDistance[before]) / span : 0.0f;
        const float32 dropBefore = center[before].Pos.y - authoredHeight(before);
        const float32 dropAfter = center[after].Pos.y - authoredHeight(after);
        center[i].Pos.y = authoredHeight(i) + dropBefore + (dropAfter - dropBefore) * fraction;
        center[i].Normal = NormalizedOrFallback(
            center[before].Normal + (center[after].Normal - center[before].Normal) * fraction,
            center[before].Normal);
        center[i].SurfaceValid = true;
    }
}

SweepStationStream BuildSweepStations(const Spline::SplineData& data, const SweepSamples& samples,
                                      std::span<const CenterSample> center,
                                      const Mathematics::Matrix4x4& invPlacerWorld,
                                      float32 nominalHalfWidth,
                                      SG::SplineProfileScale widthScale, bool worldStations,
                                      const SweepShape& shape)
{
    const Vector3 worldUp(0.0f, 1.0f, 0.0f);
    const Vector3 invOrigin = invPlacerWorld.TransformPoint(Vector3(0.0f, 0.0f, 0.0f));
    const auto toLocalDirection = [&](const Vector3& worldDirection)
    {
        return NormalizedOrFallback(invPlacerWorld.TransformPoint(worldDirection) - invOrigin,
                                    Vector3(0.0f, 0.0f, 1.0f));
    };

    SweepStationStream stream;
    std::vector<SG::SplineStripStation>& stations = stream.Local;
    std::vector<float32>& worldDistance = stream.WorldDistance;
    stations.resize(center.size());
    worldDistance.assign(center.size(), 0.0f);
    if (worldStations)
        stream.World.resize(center.size());
    for (size_t i = 0; i < center.size(); ++i)
    {
        if (i > 0u)
        {
            const Vector3 step = center[i].Pos - center[i - 1u].Pos;
            worldDistance[i] = worldDistance[i - 1u] + std::sqrt(Vector3::Dot(step, step));
        }

        const Vector3 ahead = center[std::min(i + 1u, center.size() - 1u)].Pos;
        const Vector3 behind = center[i == 0u ? 0u : i - 1u].Pos;
        const Vector3 worldForward =
            NormalizedOrFallback(ahead - behind, Vector3(0.0f, 0.0f, 1.0f));
        const Vector3 worldRight = NormalizedOrFallback(
            Vector3::Cross(worldUp, Vector3(worldForward.x, 0.0f, worldForward.z)), Vector3(1.0f, 0.0f, 0.0f));

        SG::SplineStripStation& station = stations[i];
        station.Position = invPlacerWorld.TransformPoint(center[i].Pos);
        station.Forward = toLocalDirection(worldForward);
        station.Right = toLocalDirection(worldRight);
        station.Up = Vector3::Cross(station.Forward, station.Right);
        if (worldStations)
        {
            SG::SplineStripStation& worldStation = stream.World[i];
            worldStation.Position = center[i].Pos;
            worldStation.Forward = worldForward;
            worldStation.Right = worldRight;
            worldStation.Up = Vector3::Cross(worldForward, worldRight);
            worldStation.Distance = worldDistance[i];
        }
        else
        {
            // One authored channel drives both sides, which is what keeps the
            // swept cross-section symmetric. The width stays local because the
            // geometry is generated local and the placer's transform scales it.
            const float32 channelHalfWidth = std::max(
                Spline::SampleChannelAtDistance(data, Spline::SplineChannels::kWidth,
                                                samples.LocalDistance[i], nominalHalfWidth),
                0.0f);
            const float32 placedHalfWidth =
                widthScale == SG::SplineProfileScale::None ? nominalHalfWidth : channelHalfWidth;
            station.HalfWidthLeft = placedHalfWidth;
            station.HalfWidthRight = placedHalfWidth;
        }
        station.RollRadians = samples.Frames[i].Roll;
        // U is world metres so texel density is independent of entity scale.
        station.Distance = worldDistance[i];
    }

    if (worldStations)
        return stream;
    const Vector3 localUp = toLocalDirection(worldUp);

    // The ground's rise per metre of level travel under each sample, in local
    // space, keyed by the sample's run distance: corners reshape the stream, but
    // every station they leave or add carries a distance of the samples'.
    std::vector<Vector3> groundRise;
    if (shape.GroundBase)
    {
        // A normal maps to local space through the inverse transpose of the
        // local-to-world matrix (its transpose, as world-to-local is inverse),
        // so a non-uniformly scaled placer keeps the ground's slope right.
        const float32* inverse = invPlacerWorld.Data();
        const glm::mat3 worldToLocal(inverse[0], inverse[1], inverse[2], inverse[4], inverse[5],
                                     inverse[6], inverse[8], inverse[9], inverse[10]);
        const glm::mat3 normalToLocal = glm::transpose(glm::inverse(worldToLocal));
        groundRise.resize(center.size());
        for (size_t i = 0; i < center.size(); ++i)
        {
            const glm::vec3 world(center[i].Normal.x, center[i].Normal.y, center[i].Normal.z);
            const glm::vec3 local = normalToLocal * world;
            groundRise[i] = GroundRise(
                NormalizedOrFallback(Vector3(local.x, local.y, local.z), localUp), localUp);
        }
    }
    const std::vector<float32> sampleDistance = worldDistance;
    stream.PointDistances.reserve(samples.Points.size());
    for (const uint32 point : samples.Points)
        stream.PointDistances.push_back(sampleDistance[point]);

    const bool closeLoop = shape.CloseLoop && data.IsEffectivelyClosed();
    if (shape.Corners != SG::SplineCornerStyle::None)
        ReportInnerFolds(data, samples, stations, localUp, stream.CornerIssues);
    if (closeLoop)
        SG::CloseStationLoop(stations, localUp);
    if (!samples.Corners.empty())
    {
        std::vector<SG::SplineCornerIssue> cornerIssues =
            SG::ApplyCorners(stations, samples.Corners, shape.Corners, localUp, closeLoop);
        stream.CornerIssues.insert(stream.CornerIssues.begin(), cornerIssues.begin(), cornerIssues.end());
        worldDistance.resize(stations.size());
        for (size_t i = 0; i < stations.size(); ++i)
            worldDistance[i] = stations[i].Distance;
    }
    if (shape.FaceU)
        SG::AccumulateFaceTurn(stations, localUp);
    if (shape.GroundBase)
    {
        for (SG::SplineStripStation& station : stations)
        {
            const size_t sample = static_cast<size_t>(
                std::lower_bound(sampleDistance.begin(), sampleDistance.end(), station.Distance) -
                sampleDistance.begin());
            station.GroundOffset =
                -GroundedBaseSink(station, groundRise[std::min(sample, groundRise.size() - 1u)]);
        }
    }
    return stream;
}

} // namespace GameEngine::Editor
