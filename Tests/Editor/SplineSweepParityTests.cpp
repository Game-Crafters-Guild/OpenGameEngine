// Byte-identity oracle for the paths and roads the generic sweep builds.
//
// SplineExtrude's Bevel (path) and Crown (road) profiles are frozen: a change to
// how the sweep treats walls must not move one byte of them. This digests the
// extrude's Channel pipeline over open and closed, Linear and smooth splines,
// flat and sloped ground, under a rotated, scaled, translated placer: the
// controller's own SweepShapeForExtrude, SampleSweepCenterline,
// DrapeInsertedSamples and BuildSweepStations, then the end taper, the
// closed-loop U snap, the chunk carving and the strip sweep. Each digest is
// pinned to a literal captured before the wall work began; the tight smooth rows
// are the ones a curve-following sweep would move.
//
// The conform ray is replaced by an analytic ground (what ConformRayDown returns
// on a plane): its height and its normal. Everything after the conform is the
// controller's code or a line-for-line restatement of Rebuild's tail (taper,
// U snap, chunking), which this suite pins together with it.

#include "Mathematics/Matrix4x4.h"
#include "Placement/CenterlineSampling.h"
#include "Placement/PieceEntity.h"
#include "Placement/SplineChunkSlots.h"
#include "Placement/SplineSweepStations.h"
#include "Placement/TileLayout.h"
#include "Spline/SplineData.h"
#include "Spline/SplineEvaluator.h"
#include "SplineGeometry/SplineEndTaper.h"
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

// SplineExtrudeController.cpp pins these.
constexpr float32 kChunkLengthMetres = 50.0f;
constexpr uint32 kMaxChunks = 200;

// FNV-1a over raw bytes: a float that moves by one ULP changes the digest.
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
    void Vec3(const V3& v)
    {
        Float(v.x);
        Float(v.y);
        Float(v.z);
    }
    void Mesh(const SG::SplineStripMesh& mesh)
    {
        U32(static_cast<uint32>(mesh.Vertices.size()));
        for (const SG::SplineVertex& v : mesh.Vertices)
        {
            Vec3(v.Position);
            Vec3(v.Normal);
            Float(v.UV.x);
            Float(v.UV.y);
            Float(v.Tangent.x);
            Float(v.Tangent.y);
            Float(v.Tangent.z);
            Float(v.Tangent.w);
        }
        U32(static_cast<uint32>(mesh.Indices.size()));
        Bytes(mesh.Indices.data(), mesh.Indices.size() * sizeof(uint32));
        Vec3(mesh.MinBounds);
        Vec3(mesh.MaxBounds);
    }
};

enum class Shape
{
    OpenSmooth,
    OpenLinear,
    ClosedLinear,
    ClosedSmooth,
    // Bends tight enough (about 1 m radius in the world, under the placer's 1.5
    // scale) that a sweep following its curve at the 2 cm sagitta would add
    // samples between the drape's own; paths and roads must not.
    TightSmooth,
};

Spline::SplineData MakeSpline(Shape shape)
{
    Spline::SplineData data;
    const bool linear = shape == Shape::OpenLinear || shape == Shape::ClosedLinear;
    data.Type = linear ? Spline::SplineType::Linear : Spline::SplineType::CatmullRom;
    data.Closed = shape == Shape::ClosedLinear || shape == Shape::ClosedSmooth;
    switch (shape)
    {
    case Shape::OpenSmooth:
        data.AddPoint(V3(-30.0f, 0.0f, -20.0f), 2.0f);
        data.AddPoint(V3(-5.0f, 1.0f, -8.0f), 2.5f);
        data.AddPoint(V3(10.0f, 0.5f, 12.0f), 3.0f);
        data.AddPoint(V3(40.0f, 2.0f, 30.0f), 2.2f);
        break;
    case Shape::OpenLinear:
        data.AddPoint(V3(0.0f, 0.0f, 0.0f), 1.5f);
        data.AddPoint(V3(0.0f, 0.0f, 17.3f), 1.5f);
        data.AddPoint(V3(12.1f, 0.0f, 17.3f), 2.0f);
        data.AddPoint(V3(20.0f, 0.0f, 5.0f), 1.0f);
        break;
    case Shape::ClosedLinear:
        data.AddPoint(V3(0.0f, 0.0f, 0.0f), 1.0f);
        data.AddPoint(V3(0.0f, 0.0f, 23.7f), 1.0f);
        data.AddPoint(V3(31.2f, 0.0f, 23.7f), 1.0f);
        data.AddPoint(V3(31.2f, 0.0f, 0.0f), 1.0f);
        break;
    case Shape::ClosedSmooth:
        data.AddPoint(V3(0.0f, 0.0f, -25.0f), 2.0f);
        data.AddPoint(V3(25.0f, 0.0f, 0.0f), 2.0f);
        data.AddPoint(V3(0.0f, 0.0f, 25.0f), 2.0f);
        data.AddPoint(V3(-25.0f, 0.0f, 0.0f), 2.0f);
        break;
    case Shape::TightSmooth:
        data.AddPoint(V3(0.0f, 0.0f, 0.0f), 0.3f);
        data.AddPoint(V3(0.7f, 0.0f, 0.7f), 0.3f);
        data.AddPoint(V3(0.0f, 0.0f, 1.4f), 0.3f);
        data.AddPoint(V3(-0.7f, 0.0f, 2.1f), 0.3f);
        data.AddPoint(V3(0.0f, 0.0f, 2.8f), 0.3f);
        data.AddPoint(V3(0.7f, 0.0f, 3.5f), 0.3f);
        break;
    }
    Spline::RebuildSplineCache(data);
    return data;
}

SG::SplineProfile MakeProfile(SG::SplineProfileShape shape)
{
    SG::SplineProfileParams params;
    params.Shape = shape;
    params.Width = 3.0f;
    params.Height = 1.5f;
    return SG::BuildProfile(params);
}

// A tilted plane: y = kSlopeX * x + kSlopeZ * z, or level.
constexpr float32 kSlopeX = 0.3f;
constexpr float32 kSlopeZ = -0.12f;

// The extrude's Channel path for one spline, profile and ground.
uint64 SweepDigest(Shape shape, SG::SplineProfileShape profileShape, bool sloped,
                   float32 taperMetres)
{
    const Spline::SplineData data = MakeSpline(shape);
    const SG::SplineProfile profile = MakeProfile(profileShape);

    // Rotated 30 degrees about Y, uniformly scaled by 1.5, and translated.
    Mathematics::Matrix4x4 placerWorld = Mathematics::Matrix4x4::Identity();
    const float32 c = std::cos(0.5235988f) * 1.5f;
    const float32 s = std::sin(0.5235988f) * 1.5f;
    float32* m = placerWorld.Data();
    m[0] = c;
    m[2] = -s;
    m[5] = 1.5f;
    m[8] = s;
    m[10] = c;
    m[12] = 7.0f;
    m[13] = 2.0f;
    m[14] = -4.0f;
    const Mathematics::Matrix4x4 invPlacerWorld = Editor::InvertPlacerWorld(m);

    const float32 worldArcLength =
        Editor::WorldCenterlineLength(data.TotalArcLength, placerWorld);
    const Editor::SweepShape sweepShape = Editor::SweepShapeForExtrude();
    const Editor::SweepSamples samples = Editor::SampleSweepCenterline(
        data, Editor::CenterlineSampleCount(worldArcLength), placerWorld, sweepShape);
    const std::vector<Spline::SplineFrame>& frames = samples.Frames;

    const V3 groundNormal = sloped ? Editor::NormalizedOrFallback(V3(-kSlopeX, 1.0f, -kSlopeZ),
                                                                  V3(0.0f, 1.0f, 0.0f))
                                   : V3(0.0f, 1.0f, 0.0f);
    std::vector<Editor::CenterSample> center;
    center.reserve(frames.size());
    for (const Spline::SplineFrame& frame : frames)
    {
        Editor::CenterSample sample;
        sample.Pos = placerWorld.TransformPoint(frame.Position);
        sample.Pos.y = sloped ? kSlopeX * sample.Pos.x + kSlopeZ * sample.Pos.z : 0.0f;
        sample.Normal = groundNormal;
        center.push_back(sample);
    }

    Editor::DrapeInsertedSamples(samples, placerWorld, center);
    Editor::SweepStationStream stream = Editor::BuildSweepStations(
        data, samples, center, invPlacerWorld, profile.NominalHalfWidth,
        SG::SplineProfileScale::LateralOnly, false, sweepShape);
    std::vector<SG::SplineStripStation>& stations = stream.Local;
    const std::vector<float32>& worldDistance = stream.WorldDistance;

    SG::SplineEndTaperParams taperParams;
    taperParams.TaperMetres = taperMetres;
    taperParams.HeightDrop = 0.1f;
    taperParams.ClosedLoop = data.IsEffectivelyClosed();
    SG::ApplyEndTaper(stations, taperParams);

    SG::SplineStripParams stripParams;
    stripParams.WidthScale = SG::SplineProfileScale::LateralOnly;
    stripParams.TilesPerMetreU = 1.0f;
    stripParams.TilesPerMetreV = 1.5f; // the placer's largest axis scale
    stripParams.UOriginMetres = worldDistance.front();
    if (data.IsEffectivelyClosed())
    {
        const float32 runLength = worldDistance.back() - worldDistance.front();
        const float32 wholeTiles = std::max(1.0f, std::round(runLength * stripParams.TilesPerMetreU));
        stripParams.TilesPerMetreU = wholeTiles / runLength;
    }

    ByteDigest digest;
    const auto ranges = Editor::CarveChunkRanges(worldDistance, kChunkLengthMetres, kMaxChunks);
    digest.U32(static_cast<uint32>(ranges.size()));
    for (const auto& [first, last] : ranges)
    {
        const std::span<const SG::SplineStripStation> span(stations.data() + first,
                                                           last - first + 1u);
        digest.Mesh(SG::BuildSplineStrip(profile, span, stripParams));
    }
    return digest.Value;
}

// One case of the frozen sweep: a spline, a profile, the ground and the taper.
struct PinnedSweep
{
    Shape SplineShape;
    SG::SplineProfileShape Profile;
    bool Sloped;
    float32 TaperMetres;
    uint64 Digest;
};

// x86-64 digests, captured at the commit that moved Rebuild's station loop into
// BuildSweepStations unchanged. The TightSmooth rows were captured later from
// that commit's station loop (BuildSweepStations at 9497decc1d, run on uniform
// samples) and equal what the current code produces. A closed loop has no end to taper, so its two
// taper rows agree, which is the taper's own contract.
constexpr PinnedSweep kPinnedSweeps[] = {
    {Shape::OpenSmooth, SG::SplineProfileShape::Bevel, false, 0.0f, 6295118567144022544ull},
    {Shape::OpenSmooth, SG::SplineProfileShape::Bevel, false, 4.0f, 1801945760721396901ull},
    {Shape::OpenSmooth, SG::SplineProfileShape::Bevel, true, 0.0f, 17232723792148063129ull},
    {Shape::OpenSmooth, SG::SplineProfileShape::Bevel, true, 4.0f, 1951753440421040710ull},
    {Shape::OpenSmooth, SG::SplineProfileShape::Crown, false, 0.0f, 7767381105715063714ull},
    {Shape::OpenSmooth, SG::SplineProfileShape::Crown, false, 4.0f, 13248406322439487354ull},
    {Shape::OpenSmooth, SG::SplineProfileShape::Crown, true, 0.0f, 6475957710055047723ull},
    {Shape::OpenSmooth, SG::SplineProfileShape::Crown, true, 4.0f, 8917908830601802881ull},
    {Shape::OpenLinear, SG::SplineProfileShape::Bevel, false, 0.0f, 1371921007763296156ull},
    {Shape::OpenLinear, SG::SplineProfileShape::Bevel, false, 4.0f, 3794570381577466339ull},
    {Shape::OpenLinear, SG::SplineProfileShape::Bevel, true, 0.0f, 11161437672050738518ull},
    {Shape::OpenLinear, SG::SplineProfileShape::Bevel, true, 4.0f, 10802760464274003901ull},
    {Shape::OpenLinear, SG::SplineProfileShape::Crown, false, 0.0f, 620027757363107849ull},
    {Shape::OpenLinear, SG::SplineProfileShape::Crown, false, 4.0f, 8375100995976150380ull},
    {Shape::OpenLinear, SG::SplineProfileShape::Crown, true, 0.0f, 6565048871467142406ull},
    {Shape::OpenLinear, SG::SplineProfileShape::Crown, true, 4.0f, 2924765662695163242ull},
    {Shape::ClosedLinear, SG::SplineProfileShape::Bevel, false, 0.0f, 7351135784818963869ull},
    {Shape::ClosedLinear, SG::SplineProfileShape::Bevel, false, 4.0f, 7351135784818963869ull},
    {Shape::ClosedLinear, SG::SplineProfileShape::Bevel, true, 0.0f, 14379041125448657710ull},
    {Shape::ClosedLinear, SG::SplineProfileShape::Bevel, true, 4.0f, 14379041125448657710ull},
    {Shape::ClosedLinear, SG::SplineProfileShape::Crown, false, 0.0f, 13608269128011325029ull},
    {Shape::ClosedLinear, SG::SplineProfileShape::Crown, false, 4.0f, 13608269128011325029ull},
    {Shape::ClosedLinear, SG::SplineProfileShape::Crown, true, 0.0f, 2824548862174490669ull},
    {Shape::ClosedLinear, SG::SplineProfileShape::Crown, true, 4.0f, 2824548862174490669ull},
    {Shape::ClosedSmooth, SG::SplineProfileShape::Bevel, false, 0.0f, 2846440844871494950ull},
    {Shape::ClosedSmooth, SG::SplineProfileShape::Bevel, false, 4.0f, 2846440844871494950ull},
    {Shape::ClosedSmooth, SG::SplineProfileShape::Bevel, true, 0.0f, 738330747194583963ull},
    {Shape::ClosedSmooth, SG::SplineProfileShape::Bevel, true, 4.0f, 738330747194583963ull},
    {Shape::ClosedSmooth, SG::SplineProfileShape::Crown, false, 0.0f, 12051221030685000218ull},
    {Shape::ClosedSmooth, SG::SplineProfileShape::Crown, false, 4.0f, 12051221030685000218ull},
    {Shape::ClosedSmooth, SG::SplineProfileShape::Crown, true, 0.0f, 10926821511298854250ull},
    {Shape::ClosedSmooth, SG::SplineProfileShape::Crown, true, 4.0f, 10926821511298854250ull},
    {Shape::TightSmooth, SG::SplineProfileShape::Bevel, false, 0.0f, 14523063826769687631ull},
    {Shape::TightSmooth, SG::SplineProfileShape::Bevel, false, 4.0f, 13701747720586172934ull},
    {Shape::TightSmooth, SG::SplineProfileShape::Bevel, true, 0.0f, 1430864795836523488ull},
    {Shape::TightSmooth, SG::SplineProfileShape::Bevel, true, 4.0f, 12846096937886378494ull},
    {Shape::TightSmooth, SG::SplineProfileShape::Crown, false, 0.0f, 1095516256937690480ull},
    {Shape::TightSmooth, SG::SplineProfileShape::Crown, false, 4.0f, 4089853489085835376ull},
    {Shape::TightSmooth, SG::SplineProfileShape::Crown, true, 0.0f, 18102174590188065999ull},
    {Shape::TightSmooth, SG::SplineProfileShape::Crown, true, 4.0f, 271227334266255824ull},
};

} // namespace

TEST(SplineSweepParity, PathsAndRoadsAreByteIdenticalToTheCapturedBaseline)
{
#if defined(__aarch64__) || defined(_M_ARM64)
    // sin/cos/normalize round differently in arm64 libm, and a byte digest
    // turns one ULP into a mismatch; no arm64 baseline has been captured.
    GTEST_SKIP() << "x86-64 baseline only";
#endif
    for (const PinnedSweep& pinned : kPinnedSweeps)
    {
        EXPECT_EQ(SweepDigest(pinned.SplineShape, pinned.Profile, pinned.Sloped, pinned.TaperMetres),
                  pinned.Digest)
            << "spline " << static_cast<int>(pinned.SplineShape) << ", profile "
            << static_cast<int>(pinned.Profile) << ", sloped " << pinned.Sloped << ", taper "
            << pinned.TaperMetres;
    }
}

// The digest can fail at all: the slope moves every case it touches.
TEST(SplineSweepParity, TheGroundActuallyChangesTheSweep)
{
    EXPECT_NE(SweepDigest(Shape::OpenSmooth, SG::SplineProfileShape::Bevel, false, 0.0f),
              SweepDigest(Shape::OpenSmooth, SG::SplineProfileShape::Bevel, true, 0.0f));
}
