#include "SplineLayout/TileLayout.h"

#include "Components/Spline/SplineStationJitter.h"
#include "SplineLayout/PieceBasis.h"

#include <algorithm>
#include <cmath>

namespace GameEngine::SplineLayout
{
namespace
{

using V3 = Mathematics::Vector3;
using Components::SplineJitterChannel;

// The chord window never collapses below this fraction of the spacing, so
// near-degenerate mesh bounds still yaw from a finite chord.
constexpr float32 kChordHalfSpacingFraction = 0.25f;

// A station may slide at most this fraction of the spacing either way. At the
// limit two neighbours can land on the same point but cannot swap, so the
// stations stay ordered and the chord each one yaws from keeps its direction.
constexpr float32 kMaxSpacingJitterFraction = 0.5f;

// Authored floats reach the layout unsanitized, and NaN passes std::clamp
// straight through (both its comparisons are false). Every knob below is a
// magnitude, so a non-finite one means "no opinion", not "infinite wobble".
float32 SanitizedMagnitude(float32 value, float32 maxValue)
{
    if (!std::isfinite(value))
        return 0.0f;
    return std::clamp(value, 0.0f, maxValue);
}

} // namespace

void HoldSurfaceAcrossGaps(std::vector<CenterSample>& center)
{
    const auto measured = std::find_if(center.begin(), center.end(),
                                       [](const CenterSample& s) { return s.SurfaceValid; });
    if (measured == center.end())
        return;

    for (auto it = center.begin(); it != measured; ++it)
    {
        it->Pos.y = measured->Pos.y;
        it->Normal = measured->Normal;
    }
    for (auto it = measured + 1; it != center.end(); ++it)
    {
        if (it->SurfaceValid)
            continue;
        it->Pos.y = (it - 1)->Pos.y;
        it->Normal = (it - 1)->Normal;
    }
}

std::vector<TilePose> BuildTilePoses(const std::vector<CenterSample>& center,
                                     const TileLayoutParams& params)
{
    std::vector<TilePose> poses;
    if (center.size() < 2u)
        return poses;

    // Only a centerline that actually has a gap pays for the patched copy.
    const bool anyGap = std::any_of(center.begin(), center.end(),
                                    [](const CenterSample& s) { return !s.SurfaceValid; });
    std::vector<CenterSample> patched;
    if (anyGap)
    {
        patched = center;
        HoldSurfaceAcrossGaps(patched);
    }
    const std::vector<CenterSample>& samples = anyGap ? patched : center;

    std::vector<float32> cum(samples.size(), 0.0f);
    for (size_t i = 1; i < samples.size(); ++i)
    {
        const V3 d = samples[i].Pos - samples[i - 1].Pos;
        cum[i] = cum[i - 1] + std::sqrt(V3::Dot(d, d));
    }
    const float32 drapedLength = cum.back();
    if (drapedLength <= 0.0f)
        return poses;

    const V3 worldUp(0.0f, 1.0f, 0.0f);

    const auto sampleAt = [&](float32 s) -> CenterSample
    {
        s = std::clamp(s, 0.0f, drapedLength);
        const auto it = std::lower_bound(cum.begin(), cum.end(), s);
        const size_t hi = std::clamp<size_t>(static_cast<size_t>(it - cum.begin()),
                                             size_t{1}, cum.size() - 1u);
        const size_t lo = hi - 1u;
        const float32 span = cum[hi] - cum[lo];
        const float32 t = span > 0.0f ? (s - cum[lo]) / span : 0.0f;
        CenterSample out;
        out.Pos = samples[lo].Pos + (samples[hi].Pos - samples[lo].Pos) * t;
        out.Normal = NormalizedOrFallback(
            samples[lo].Normal + (samples[hi].Normal - samples[lo].Normal) * t, worldUp);
        return out;
    };

    // Tile stations along the draped length.
    std::vector<float32> stations;
    if (!params.StationDistances.empty())
    {
        // A caller that solved its own fill owns the stations outright; the
        // walk below cannot reproduce non-uniform spacing.
        const size_t count = std::min<size_t>(params.StationDistances.size(), params.MaxTiles);
        stations.assign(params.StationDistances.begin(), params.StationDistances.begin() + count);
    }
    else if (params.Fit == Components::SplinePlacementFit::FixedPitch)
    {
        const uint32 count = std::min<uint32>(
            static_cast<uint32>(drapedLength / params.Spacing) + 1u, params.MaxTiles);
        stations.reserve(count);
        for (uint32 i = 0; i < count; ++i)
            stations.push_back(static_cast<float32>(i) * params.Spacing);
    }
    else // FitToLength: rounded count, stations land on both ends
    {
        const uint32 count = std::clamp<uint32>(
            static_cast<uint32>(std::lround(drapedLength / params.Spacing)) + 1u, 2u,
            params.MaxTiles);
        stations.reserve(count);
        for (uint32 i = 0; i < count; ++i)
            stations.push_back(drapedLength * static_cast<float32>(i) /
                               static_cast<float32>(count - 1u));
    }

    // Which local axis of the mesh runs along the path. One answer, read by the
    // chord window, the footprint centering and the emitted transform alike.
    const PieceAxis pieceAxis = ChoosePieceAxis(params.MeshBoundsHalfExtents);

    // Yaw comes from the chord across the tile's own footprint, not the
    // instantaneous tangent: a rigid tile is then the chord of its own arc
    // window, which removes the mid-tile sagitta and keeps facing edges of
    // neighbours near their shared surface point on bends. The window is half
    // the tile's extent ALONG the path, which is the axis it is laid on.
    const float32 chordHalf =
        std::max(PieceLength(params.MeshBoundsHalfExtents, pieceAxis) * 0.5f,
                 kChordHalfSpacingFraction * params.Spacing);

    // BoundsMin: the mesh's local-space base sits at center.y - halfExtents.y;
    // lifting the origin by its negation lands the lowest vertex on the conform
    // hit. A base-pivoted mesh has min.y ~ 0 and the lift is a no-op; center- or
    // anchor-pivoted models (common in the Synty kits) no longer sink.
    // PivotPlane: the pivot IS the authored plant line, so there is no lift and
    // anything below it buries — the only correct answer for a skirted mesh,
    // which BoundsMin would float by the depth of its skirt.
    const float32 meshBaseLift =
        params.PlantMode == Components::SplinePlantMode::PivotPlane
            ? 0.0f
            : params.MeshBoundsHalfExtents.y - params.MeshBoundsCenter.y;

    // Per-station variation. Every knob is sanitized once here rather than per
    // station, and each `anyX` gate keeps the default path from even taking a
    // draw — the poses below are then bit-identical to a build without it.
    const float32 spacingJitter =
        SanitizedMagnitude(params.SpacingJitterMetres, kMaxSpacingJitterFraction * params.Spacing);
    const float32 yawJitterRadians =
        SanitizedMagnitude(params.YawJitterDegrees, 180.0f) * Mathematics::Pi / 180.0f;
    const float32 lateralJitter = SanitizedMagnitude(params.LateralJitterMetres, drapedLength);
    const float32 dropoutChance = SanitizedMagnitude(params.DropoutChance, 1.0f);
    const float32 taperMetres = SanitizedMagnitude(params.EndTaperMetres, drapedLength);
    const bool anyThinning = dropoutChance > 0.0f || taperMetres > 0.0f;

    // How likely a station at `nominal` metres along the run is to be placed.
    // Dropout thins the whole run evenly; the end taper multiplies that by a
    // ramp reaching zero AT each end, so a tapered run stops by fading rather
    // than by stopping.
    const auto keepProbability = [&](float32 nominal)
    {
        float32 keep = 1.0f - dropoutChance;
        if (taperMetres > 0.0f)
        {
            const float32 fromNearestEnd = std::min(nominal, drapedLength - nominal);
            keep *= std::clamp(fromNearestEnd / taperMetres, 0.0f, 1.0f);
        }
        return keep;
    };

    poses.reserve(stations.size());
    for (size_t stationOrdinal = 0; stationOrdinal < stations.size(); ++stationOrdinal)
    {
        // The ordinal, not the surviving-pose index: it keys the draws below and
        // the caller's pool pick, so a dropped station leaves its neighbours'
        // wobble and meshes exactly as they were.
        const uint32 stationIndex = static_cast<uint32>(stationOrdinal);
        const float32 nominal = stations[stationOrdinal];

        if (anyThinning &&
            Components::SplineJitterUnit(params.Seed, SplineJitterChannel::Dropout, stationIndex) >=
                keepProbability(nominal))
        {
            continue;
        }

        // Jitter along the run moves where the station stands, so it is applied
        // before anything is sampled — the tile then yaws from the chord around
        // where it actually is, not around where the grid would have put it.
        float32 s = nominal;
        if (spacingJitter > 0.0f)
        {
            s += spacingJitter * Components::SplineJitterSigned(
                                     params.Seed, SplineJitterChannel::Spacing, stationIndex);
        }

        CenterSample c = sampleAt(s);
        const CenterSample a = sampleAt(s - chordHalf);
        const CenterSample b = sampleAt(s + chordHalf);
        const V3 chord = b.Pos - a.Pos;
        // Ground-plane travel direction from the chord; degenerate chords
        // (vertical or zero) fall back to +Z. A caller-supplied direction (a
        // shared corner station's bisector) wins over the chord; a zero entry
        // means "no opinion, use the chord".
        V3 forward = NormalizedOrFallback(V3(chord.x, 0.0f, chord.z), V3(0.0f, 0.0f, 1.0f));
        if (stationOrdinal < params.StationForwards.size())
        {
            const V3& authored = params.StationForwards[stationOrdinal];
            forward = NormalizedOrFallback(V3(authored.x, 0.0f, authored.z), forward);
        }
        if (yawJitterRadians > 0.0f)
        {
            // Yaw about world up: positive turns +Z toward +X in this LH +Y-up
            // engine. Applied to the ground-plane direction before the surface
            // basis is built around it, so the piece stays square to its ground.
            const float32 angle =
                yawJitterRadians *
                Components::SplineJitterSigned(params.Seed, SplineJitterChannel::Yaw, stationIndex);
            const float32 cosA = std::cos(angle);
            const float32 sinA = std::sin(angle);
            forward = NormalizedOrFallback(
                V3(forward.x * cosA + forward.z * sinA, 0.0f, forward.z * cosA - forward.x * sinA),
                forward);
        }
        if (lateralJitter > 0.0f)
        {
            // Cross(worldUp, forward) is the RIGHT of travel here, the same
            // convention the recipe's own LateralOffset is applied with.
            const V3 right = NormalizedOrFallback(V3::Cross(worldUp, forward), V3(1.0f, 0.0f, 0.0f));
            c.Pos = c.Pos + right * (lateralJitter *
                                     Components::SplineJitterSigned(
                                         params.Seed, SplineJitterChannel::Lateral, stationIndex));
        }

        if (params.Probe)
        {
            // The polyline is a chord approximation, so interpolating it cuts
            // any feature that falls between two dense samples. Re-read the
            // surface under the station instead — after the slides above, so a
            // moved station conforms to the ground it actually stands on.
            // Stations are already fixed, so this only ever moves its own tile.
            float32 altitude = c.Pos.y;
            V3 normal = c.Normal;
            if (params.Probe(c.Pos, altitude, normal))
            {
                c.Pos.y = altitude;
                c.Normal = NormalizedOrFallback(normal, worldUp);
            }
        }

        V3 up = worldUp;
        if (params.AlignToSurfaceNormal)
        {
            const float32 blend = std::clamp(params.SlopeBlend, 0.0f, 1.0f);
            up = NormalizedOrFallback(worldUp + (c.Normal - worldUp) * blend, worldUp);
            // Ground steep enough to lay a tile on its side is held at the
            // ceiling. Right is Cross(up, forward), so an up leaning this far
            // points the tile's WIDTH axis skyward and the tile renders as a
            // standing sheet instead of as path. Height still conforms.
            const V3 held = ClampTiltFromWorldUp(up, params.MaxTiltDegrees);
            if (params.OutTiltClamp && V3::Dot(held, up) < 1.0f - 1.0e-6f)
            {
                TiltClampReport& report = *params.OutTiltClamp;
                ++report.ClampedStations;
                const float32 tiltDegrees =
                    std::acos(std::clamp(V3::Dot(up, worldUp), -1.0f, 1.0f)) * 180.0f /
                    Mathematics::Pi;
                if (tiltDegrees > report.SteepestDegrees)
                {
                    report.SteepestDegrees = tiltDegrees;
                    report.SteepestAt = c.Pos;
                }
            }
            up = held;
        }

        TilePose pose;
        pose.StationIndex = stationIndex;
        // Yaw from the chord, pitch and roll from the surface. Right =
        // Cross(up, forward) is the RIGHT of travel in this LH +Y-up engine
        // (+X when travelling +Z with up = +Y); forward is re-completed so the
        // basis stays orthonormal around the surface up.
        pose.Right = NormalizedOrFallback(V3::Cross(up, forward), V3(1.0f, 0.0f, 0.0f));
        pose.Forward = NormalizedOrFallback(V3::Cross(pose.Right, up), forward);
        pose.Up = NormalizedOrFallback(V3::Cross(pose.Forward, pose.Right), worldUp);
        // Center the mesh FOOTPRINT on the centerline point: the Synty tiles
        // are corner-pivoted (bounds center ~1.2 m off the origin in X and Z),
        // so placing the raw pivot on the curve hangs the slab off one side
        // and swings seams around the corner at bends. The offsets are
        // cancelled along the tile's OWN local axes, so they follow it whichever
        // way it is laid. Then plant along the tile's own up by the lift
        // PlantMode chose above — the mesh's base under BoundsMin, nothing at
        // all under PivotPlane.
        pose.Base = c.Pos;
        const PieceBasis basis = MakePieceBasis(pose, pieceAxis);
        pose.Position = c.Pos + pose.Up * meshBaseLift - basis.LocalX * params.MeshBoundsCenter.x -
                        basis.LocalZ * params.MeshBoundsCenter.z;
        poses.push_back(pose);
    }

    return poses;
}

} // namespace GameEngine::SplineLayout
