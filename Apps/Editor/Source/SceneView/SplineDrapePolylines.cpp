#include "SceneView/SplineDrapePolylines.h"

#include "Mathematics/Matrix4x4.h"
#include "Placement/CenterlineSampling.h"
#include "Placement/SplineSurfaceConform.h"
#include "Placement/TileLayout.h"
#include "Spline/SplineData.h"
#include "Spline/SplineEvaluator.h"

#include <algorithm>
#include <cmath>

namespace GameEngine::Editor::SceneTools
{
using Mathematics::Vector3;

void BuildSplineDrapedPolylines(ECS::World& world,
                                const Spline::SplineData& data,
                                const Mathematics::Matrix4x4& worldM,
                                std::span<const ECS::EntityHandle> ignore,
                                bool wantEdges,
                                Components::SplineConformTarget target,
                                SplineDrapedPolylines& out)
{
    out.Center.clear();
    out.Left.clear();
    out.Right.clear();
    const float localArcLength = data.TotalArcLength;
    if (localArcLength <= 0.0f)
        return;

    // Sampling density follows the WORLD arc length (SplineData arc lengths
    // are entity-local), at the drape's own step and budget — deliberately
    // coarser than placement's kCenterlineStepMetres, because these polylines
    // are a preview, not the measurement tile stations derive from.
    const float worldArcLength = WorldCenterlineLength(localArcLength, worldM);
    if (!std::isfinite(worldArcLength))
        return; // a hostile transform gets empty polylines, same as an invalid spline
    // Cap the length BEFORE the cast: float→uint32 past the integer range is UB,
    // the same hazard CenterlineSampleCount guards, and scale amplifies the operand.
    const float cappedLength = std::min(
        worldArcLength, static_cast<float>(kDrapeMaxSamples) * kDrapeSampleStepMetres);
    const uint32_t sampleCount = std::clamp<uint32_t>(
        static_cast<uint32_t>(cappedLength / kDrapeSampleStepMetres) + 1u, 2u, kDrapeMaxSamples);
    std::vector<Spline::SplineFrame> frames;
    Spline::SampleUniform(data, sampleCount, frames);
    if (frames.size() < 2u)
        return;

    const float axisScale = LargestAxisScale(worldM);
    const Vector3 worldOrigin = worldM.TransformPoint(Vector3(0.0f, 0.0f, 0.0f));
    const Vector3 worldUp(0.0f, 1.0f, 0.0f);

    // Polylines accumulate as CenterSamples so the shared conform-miss rule
    // can run on them below; the drape never reads CenterSample::Normal.
    std::vector<CenterSample> center;
    std::vector<CenterSample> left;
    std::vector<CenterSample> right;
    center.reserve(frames.size());
    if (wantEdges)
    {
        left.reserve(frames.size());
        right.reserve(frames.size());
    }

    // Measured samples carry the raw conform hit altitude; misses keep the
    // authored/offset altitude and are marked for the gap-hold below.
    const auto drapedSample = [&](Vector3 at) -> CenterSample
    {
        CenterSample sample;
        Vector3 hit;
        sample.SurfaceValid = ConformRayDown(world, at, ignore, hit, nullptr, nullptr, target);
        if (sample.SurfaceValid)
            at.y = hit.y;
        sample.Pos = at;
        sample.Normal = worldUp;
        return sample;
    };

    for (size_t i = 0; i < frames.size(); ++i)
    {
        const Vector3 pos = worldM.TransformPoint(frames[i].Position);
        center.push_back(drapedSample(pos));

        if (!wantEdges)
            continue;

        // Channels are keyed by LOCAL arc distance and the frames are uniform
        // in it, so the lookup distance stays local; only the sampled width
        // converts to world metres — by the transform's largest axis, the
        // same convention WorldCenterlineLength pins (exact under uniform
        // scale).
        const float s =
            localArcLength * static_cast<float>(i) / static_cast<float>(frames.size() - 1u);
        const float worldHalfWidth =
            std::max(Spline::SampleChannelAtDistance(data, Spline::SplineChannels::kWidth, s, 0.0f),
                     kDrapeMinHalfWidth) *
            axisScale;

        const Vector3 worldTangent = NormalizedOrFallback(
            worldM.TransformPoint(frames[i].Forward) - worldOrigin, Vector3(0.0f, 0.0f, 1.0f));
        // Ground-plane travel direction; vertical tangents fall back to +Z.
        const Vector3 forward = NormalizedOrFallback(Vector3(worldTangent.x, 0.0f, worldTangent.z),
                                                     Vector3(0.0f, 0.0f, 1.0f));
        // Lateral axis: Cross(worldUp, forward) is the RIGHT of travel in this
        // LH +Y-up engine (+X when travelling +Z) — the same convention as the
        // placement controller's LateralOffset and tile Right, so ribbon and
        // tiles agree about sides by construction, not by inspection.
        const Vector3 rightAxis =
            NormalizedOrFallback(Vector3::Cross(worldUp, forward), Vector3(1.0f, 0.0f, 0.0f));

        // Each edge offsets from the AUTHORED altitude, then drapes at its own
        // XZ — never from the draped center, so a center that found ground and
        // an edge that hangs over a cliff stay independent.
        right.push_back(drapedSample(pos + rightAxis * worldHalfWidth));
        left.push_back(drapedSample(pos - rightAxis * worldHalfWidth));
    }

    // The shared conform-miss rule, per polyline: misses hold the nearest
    // measured altitude. The lift then applies to every sample — patched
    // samples carry measured altitudes too. A polyline with no measured
    // sample keeps its authored altitudes and takes no lift: there is no
    // surface to ride above.
    const auto finalize = [](std::vector<CenterSample>& samples, std::vector<Vector3>& outPts)
    {
        const bool anyMeasured = std::any_of(samples.begin(), samples.end(),
                                             [](const CenterSample& s) { return s.SurfaceValid; });
        HoldSurfaceAcrossGaps(samples);
        outPts.reserve(samples.size());
        for (const CenterSample& s : samples)
            outPts.push_back(anyMeasured ? Vector3(s.Pos.x, s.Pos.y + kDrapeSurfaceLiftMetres, s.Pos.z)
                                         : s.Pos);
    };
    finalize(center, out.Center);
    if (wantEdges)
    {
        finalize(left, out.Left);
        finalize(right, out.Right);
    }
}

} // namespace GameEngine::Editor::SceneTools
