// Full-run coverage of the extrude sweep's ConformMode=None station path: the
// spline's arc, the drape-step sample stream, the chunk carving and the strip
// builder together must cover every metre of the authored curve — a head chunk
// that produces no geometry or a station stream that stops short of the last
// point both surface here as a coverage gap.
//
// The station generation mirrors SplineExtrudeController::Rebuild for an
// identity placer transform with conform off (no raycast, so the path is pure);
// the spline is the RiverVignette water run — nine points, varying radii,
// ~247.6 m — the shape the chunked sweep ships under.

#include "Placement/CenterlineSampling.h"
#include "Placement/SplineChunkSlots.h"
#include "Placement/TileLayout.h"
#include "Spline/SplineData.h"
#include "Spline/SplineEvaluator.h"
#include "SplineGeometry/SplineProfile.h"
#include "SplineGeometry/SplineStripBuilder.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace GameEngine;
using V3 = Mathematics::Vector3;
namespace SG = GameEngine::SplineGeometry;

namespace
{

// SplineExtrudeController.cpp pins these; the coverage contract is defined at
// this pitch, so the test states them rather than importing controller
// internals.
constexpr float32 kChunkLengthMetres = 50.0f;
constexpr uint32 kMaxChunks = 200;

Spline::SplineData RiverSpline()
{
    Spline::SplineData data;
    data.Type = Spline::SplineType::CatmullRom;
    data.Closed = false;
    const float32 px[9] = {-78.0f, -58.0f, -38.0f, -26.0f, -34.0f, -14.0f, 18.0f, 44.0f, 72.0f};
    const float32 py[9] = {8.82f, 7.56f, 6.44f, 5.18f, 3.92f, 2.8f, 1.82f, 1.12f, 0.63f};
    const float32 pz[9] = {-70.0f, -78.0f, -62.0f, -34.0f, -6.0f, 16.0f, 30.0f, 54.0f, 80.0f};
    const float32 radius[9] = {2.2f, 2.4f, 2.6f, 2.8f, 3.0f, 3.2f, 3.5f, 3.8f, 4.2f};
    for (int i = 0; i < 9; ++i)
        data.AddPoint(V3(px[i], py[i], pz[i]), radius[i]);
    Spline::RebuildSplineCache(data);
    return data;
}

struct StationStream
{
    std::vector<SG::SplineStripStation> Stations;
    std::vector<float32> Distance;
};

// The controller's station loop for an identity placer with conform off:
// positions straight from SampleUniform, frames from the shared polyline,
// half-width from the spline's width channel, distance from the walked length.
StationStream BuildStations(const Spline::SplineData& data, const SG::SplineProfile& profile)
{
    const float32 arcLength = data.TotalArcLength;
    const uint32 denseCount = Editor::CenterlineSampleCount(arcLength);

    std::vector<Spline::SplineFrame> frames;
    Spline::SampleUniform(data, denseCount, frames);

    StationStream stream;
    stream.Stations.resize(frames.size());
    stream.Distance.resize(frames.size(), 0.0f);

    const V3 worldUp(0.0f, 1.0f, 0.0f);
    for (size_t i = 0; i < frames.size(); ++i)
    {
        if (i > 0u)
        {
            const V3 step = frames[i].Position - frames[i - 1u].Position;
            stream.Distance[i] = stream.Distance[i - 1u] + std::sqrt(V3::Dot(step, step));
        }

        const V3 ahead = frames[std::min(i + 1u, frames.size() - 1u)].Position;
        const V3 behind = frames[i == 0u ? 0u : i - 1u].Position;
        const V3 forward =
            Editor::NormalizedOrFallback(ahead - behind, V3(0.0f, 0.0f, 1.0f));
        const V3 right = Editor::NormalizedOrFallback(
            V3::Cross(worldUp, V3(forward.x, 0.0f, forward.z)), V3(1.0f, 0.0f, 0.0f));

        const float32 localS =
            arcLength * static_cast<float32>(i) / static_cast<float32>(frames.size() - 1u);

        SG::SplineStripStation& station = stream.Stations[i];
        station.Position = frames[i].Position;
        station.Forward = forward;
        station.Right = right;
        station.Up = V3::Cross(station.Forward, station.Right);
        const float32 channelHalfWidth = std::max(
            Spline::SampleChannelAtDistance(data, Spline::SplineChannels::kWidth, localS,
                                            profile.NominalHalfWidth),
            0.0f);
        station.HalfWidthLeft = channelHalfWidth;
        station.HalfWidthRight = channelHalfWidth;
        station.RollRadians = frames[i].Roll;
        station.Distance = stream.Distance[i];
    }
    return stream;
}

SG::SplineProfile RiverProfile()
{
    SG::SplineProfileParams params;
    params.Shape = SG::SplineProfileShape::Bevel;
    params.Width = 4.5f;
    params.Height = 1.0f;
    params.EdgeDrop = 0.3f;
    params.EdgeInset = 0.8f;
    return SG::BuildProfile(params);
}

} // namespace

TEST(SplineExtrudeCoverage, TheRiverRunIsSweptEndToEndWithNoHeadHoleOrTailGap)
{
    const Spline::SplineData data = RiverSpline();
    ASSERT_GT(data.TotalArcLength, 200.0f);
    ASSERT_LT(data.TotalArcLength, 300.0f);

    const SG::SplineProfile profile = RiverProfile();
    ASSERT_TRUE(profile.IsValid());

    const StationStream stream = BuildStations(data, profile);
    ASSERT_GE(stream.Stations.size(), 2u);

    // The stream itself spans the whole curve: distance 0 at the first point
    // and the full walked arc at the last, within the drape step.
    EXPECT_EQ(stream.Distance.front(), 0.0f);
    EXPECT_NEAR(stream.Distance.back(), data.TotalArcLength, Editor::kCenterlineStepMetres);
    const V3 firstPos = stream.Stations.front().Position;
    const V3 lastPos = stream.Stations.back().Position;
    EXPECT_NEAR(firstPos.x, -78.0f, 0.01f);
    EXPECT_NEAR(firstPos.z, -70.0f, 0.01f);
    EXPECT_NEAR(lastPos.x, 72.0f, 0.01f) << "the stream must reach the spline terminus";
    EXPECT_NEAR(lastPos.z, 80.0f, 0.01f) << "the stream must reach the spline terminus";

    const auto ranges =
        Editor::CarveChunkRanges(stream.Distance, kChunkLengthMetres, kMaxChunks);
    ASSERT_EQ(ranges.size(), 5u) << "~247.6 m at a 50 m pitch is five chunks";
    EXPECT_EQ(ranges.front().first, 0u);
    EXPECT_EQ(ranges.back().second, static_cast<uint32>(stream.Stations.size() - 1u));

    SG::SplineStripParams params;
    params.WidthScale = SG::SplineProfileScale::LateralOnly;
    params.TilesPerMetreU = 1.0f;
    params.TilesPerMetreV = 1.0f;
    params.UOriginMetres = stream.Distance.front();

    float32 covered = 0.0f;
    for (size_t c = 0; c < ranges.size(); ++c)
    {
        const auto [first, last] = ranges[c];
        const std::span<const SG::SplineStripStation> span(stream.Stations.data() + first,
                                                           last - first + 1u);
        const SG::SplineStripMesh strip = SG::BuildSplineStrip(profile, span, params);

        EXPECT_TRUE(strip.IsValid()) << "chunk " << c << " produced no geometry";
        EXPECT_EQ(strip.RingCount, span.size())
            << "chunk " << c << " lost stations to the ascending-distance filter";
        EXPECT_EQ(strip.DroppedStations, 0u) << "chunk " << c;
        if (strip.IsValid())
            covered += stream.Distance[last] - stream.Distance[first];
    }
    EXPECT_NEAR(covered, stream.Distance.back(), 1.0e-3f)
        << "every metre of the run must be swept by a valid chunk";
}

// The head chunk in isolation: the run's first ~50 m — endpoint tangent
// fallbacks, distance starting at exactly 0 — must build a full strip. An
// invalid head chunk is a hole at the river's source.
TEST(SplineExtrudeCoverage, TheHeadChunkBuildsAFullStrip)
{
    const Spline::SplineData data = RiverSpline();
    const SG::SplineProfile profile = RiverProfile();
    const StationStream stream = BuildStations(data, profile);

    const auto ranges =
        Editor::CarveChunkRanges(stream.Distance, kChunkLengthMetres, kMaxChunks);
    ASSERT_FALSE(ranges.empty());
    const auto [first, last] = ranges.front();
    ASSERT_EQ(first, 0u);

    SG::SplineStripParams params;
    params.WidthScale = SG::SplineProfileScale::LateralOnly;
    params.UOriginMetres = 0.0f;
    const std::span<const SG::SplineStripStation> span(stream.Stations.data(), last + 1u);
    const SG::SplineStripMesh strip = SG::BuildSplineStrip(profile, span, params);

    ASSERT_TRUE(strip.IsValid());
    EXPECT_EQ(strip.RingCount, span.size());
    // The head chunk's geometry starts at the source point (plus the profile's
    // lateral extent), not partway down the run.
    EXPECT_LT(strip.MinBounds.x, -77.0f) << "the sweep must cover the river source";
    EXPECT_GT(strip.MinBounds.x, -85.0f);
    EXPECT_LT(strip.MinBounds.z, -70.0f) << "the sweep must cover the river source";
}
