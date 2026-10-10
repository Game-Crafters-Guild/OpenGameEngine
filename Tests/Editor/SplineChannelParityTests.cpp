// Byte-identity oracle for WidthMode = Channel.
//
// The region fill replaces the swept ribbon for FitToBanks only; Channel keeps
// the sweep, and "keeps" has to mean the same bytes rather than the same idea.
// This digests the whole Channel pipeline the controller runs — station stream,
// end taper, chunk carving, strip sweep — over the RiverVignette water run, and
// pins the result to a literal captured from the builder BEFORE the fill landed.
//
// A change to the sweep, the profile, the taper or the chunking moves the digest.
// That is the point: Channel output is frozen, so any move is either a bug or a
// deliberate change that must re-baseline this literal ON PURPOSE.
//
// The station generation mirrors SplineExtrudeController::Rebuild for an identity
// placer with conform off (no raycast, so the path is pure and device-free).

#include "Placement/CenterlineSampling.h"
#include "Placement/SplineChunkSlots.h"
#include "Placement/TileLayout.h"
#include "Spline/SplineData.h"
#include "Spline/SplineEvaluator.h"
#include "SplineGeometry/SplineEndTaper.h"
#include "SplineGeometry/SplineProfile.h"
#include "SplineGeometry/SplineStripBuilder.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

using namespace GameEngine;
using V3 = Mathematics::Vector3;
namespace SG = GameEngine::SplineGeometry;

namespace
{

// SplineExtrudeController.cpp pins these.
constexpr float32 kChunkLengthMetres = 50.0f;
constexpr uint32 kMaxChunks = 200;

// FNV-1a over raw bytes: a bit-identity check, not a numeric comparison. Float
// bytes are hashed as they lie, so a one-ULP move in any vertex changes it.
struct ByteDigest
{
    uint64 Value = 1469598103934665603ull;

    void Bytes(const void* data, size_t size)
    {
        const auto* p = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < size; ++i)
        {
            Value ^= static_cast<uint64>(p[i]);
            Value *= 1099511628211ull;
        }
    }

    void Float(float32 v) { Bytes(&v, sizeof(v)); }
    void U32(uint32 v) { Bytes(&v, sizeof(v)); }

    void Vertex(const SG::SplineVertex& v)
    {
        Float(v.Position.x); Float(v.Position.y); Float(v.Position.z);
        Float(v.Normal.x);   Float(v.Normal.y);   Float(v.Normal.z);
        Float(v.UV.x);       Float(v.UV.y);
        Float(v.Tangent.x);  Float(v.Tangent.y);  Float(v.Tangent.z); Float(v.Tangent.w);
    }

    void Mesh(const SG::SplineStripMesh& m)
    {
        U32(m.RingVertexCount);
        U32(m.RingCount);
        U32(m.DroppedStations);
        U32(static_cast<uint32>(m.Vertices.size()));
        U32(static_cast<uint32>(m.Indices.size()));
        for (const SG::SplineVertex& v : m.Vertices)
            Vertex(v);
        for (uint32 i : m.Indices)
            U32(i);
        Float(m.MinBounds.x); Float(m.MinBounds.y); Float(m.MinBounds.z);
        Float(m.MaxBounds.x); Float(m.MaxBounds.y); Float(m.MaxBounds.z);
    }
};

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

// The controller's Channel-mode station loop, identity placer, conform off.
std::vector<SG::SplineStripStation> ChannelStations(const Spline::SplineData& data,
                                                    const SG::SplineProfile& profile,
                                                    std::vector<float32>& outDistance)
{
    const float32 arcLength = data.TotalArcLength;
    const uint32 denseCount = Editor::CenterlineSampleCount(arcLength);

    std::vector<Spline::SplineFrame> frames;
    Spline::SampleUniform(data, denseCount, frames);

    std::vector<SG::SplineStripStation> stations(frames.size());
    outDistance.assign(frames.size(), 0.0f);

    const V3 worldUp(0.0f, 1.0f, 0.0f);
    for (size_t i = 0; i < frames.size(); ++i)
    {
        if (i > 0u)
        {
            const V3 step = frames[i].Position - frames[i - 1u].Position;
            outDistance[i] = outDistance[i - 1u] + std::sqrt(V3::Dot(step, step));
        }

        const V3 ahead = frames[std::min(i + 1u, frames.size() - 1u)].Position;
        const V3 behind = frames[i == 0u ? 0u : i - 1u].Position;
        const V3 forward = Editor::NormalizedOrFallback(ahead - behind, V3(0.0f, 0.0f, 1.0f));
        const V3 right = Editor::NormalizedOrFallback(
            V3::Cross(worldUp, V3(forward.x, 0.0f, forward.z)), V3(1.0f, 0.0f, 0.0f));

        const float32 localS =
            arcLength * static_cast<float32>(i) / static_cast<float32>(frames.size() - 1u);

        SG::SplineStripStation& station = stations[i];
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
        station.Distance = outDistance[i];
    }
    return stations;
}

// Digest of every chunk the Channel path emits for the run.
uint64 ChannelRunDigest(float32 endTaperMetres)
{
    const Spline::SplineData data = RiverSpline();
    const SG::SplineProfile profile = RiverProfile();
    std::vector<float32> distance;
    std::vector<SG::SplineStripStation> stations = ChannelStations(data, profile, distance);

    SG::SplineEndTaperParams taperParams;
    taperParams.TaperMetres = endTaperMetres;
    taperParams.HeightDrop = 0.3f; // the profile's EdgeDrop, as the controller passes it
    taperParams.ClosedLoop = data.Closed;
    SG::ApplyEndTaper(stations, taperParams);

    SG::SplineStripParams stripParams;
    stripParams.WidthScale = SG::SplineProfileScale::LateralOnly;
    stripParams.TilesPerMetreU = 1.0f;
    stripParams.TilesPerMetreV = 1.0f;
    stripParams.UOriginMetres = distance.front();

    const auto ranges = Editor::CarveChunkRanges(distance, kChunkLengthMetres, kMaxChunks);

    ByteDigest digest;
    digest.U32(static_cast<uint32>(ranges.size()));
    for (const auto& [first, last] : ranges)
    {
        const std::span<const SG::SplineStripStation> span(stations.data() + first,
                                                           last - first + 1u);
        digest.Mesh(SG::BuildSplineStrip(profile, span, stripParams));
    }
    return digest.Value;
}

} // namespace

// The baselines are per-architecture, and that is not a workaround: the strip is
// built from sin/cos/normalize, libm rounds those differently on x86-64 and
// arm64, and a digest turns a single 1-ULP difference into a total mismatch. One
// constant could therefore only ever hold on the machine that captured it.
//
// PROVENANCE, because the two are not equally strong. The x86-64 pair was
// captured from the pre-fill builder — nothing in the fill slice produced it,
// which is what made it evidence that the slice moved no Channel byte. The
// arm64 pair was captured on arm64 from the post-slice code, so it is a change
// DETECTOR from here on, not independent proof of that slice. The non-circular
// half of this suite is TheTaperActuallyChangesTheRun below, which compares two
// runs against each other and needs no captured constant at all.
#if defined(__aarch64__) || defined(_M_ARM64)
constexpr uint64 kUntaperedRunDigest = 581434757012935431ull;
constexpr uint64 kTaperedRunDigest = 3985377892147645201ull;
#else
constexpr uint64 kUntaperedRunDigest = 6517947516823628650ull;
constexpr uint64 kTaperedRunDigest = 2319233531916964652ull;
#endif

// Captured from the pre-fill builder. If the fill slice moved a Channel byte,
// this is where it says so.
TEST(SplineChannelParity, TheUntaperedRunIsByteIdenticalToTheCapturedBaseline)
{
    EXPECT_EQ(ChannelRunDigest(0.0f), kUntaperedRunDigest);
}

// EndTaperMetres stays live for Channel — the fill ignores it, this must not.
TEST(SplineChannelParity, TheTaperedRunIsByteIdenticalToTheCapturedBaseline)
{
    EXPECT_EQ(ChannelRunDigest(6.0f), kTaperedRunDigest);
}

// The taper is not a no-op on this run: a digest equal to the untapered one
// would make the test above pass while proving nothing.
TEST(SplineChannelParity, TheTaperActuallyChangesTheRun)
{
    EXPECT_NE(ChannelRunDigest(0.0f), ChannelRunDigest(6.0f));
}
