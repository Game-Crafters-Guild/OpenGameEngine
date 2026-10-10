// The pieces one fence rebuild instances, in the order and under the mesh
// signatures the controller compares between rebuilds. The controller itself
// needs a live RenderServices to resolve its pools, so the list is built by a
// leaf TU over plain pool entries and pinned here.

#include <gtest/gtest.h>

#include "Components/Spline/SplinePoolSelection.h"
#include "Placement/FenceEmission.h"
#include "Placement/FenceLayout.h"
#include "Placement/TileLayout.h"
#include "SplineLayout/SpanMitre.h"

#include <array>
#include <string>
#include <vector>

using namespace GameEngine;
using Editor::BuildFenceEmissions;
using Editor::CenterSample;
using Editor::FenceCrest;
using Editor::FenceEmission;
using Editor::FenceEmissionPools;
using Editor::FenceLayoutParams;
using Editor::FenceLayoutResult;
using Editor::FencePieceBounds;
using Editor::FencePoolPiece;
using Editor::FenceSpan;
using Editor::FenceStation;
using Editor::kSignatureCapBase;
using Editor::kSignatureCrestBase;
using Editor::kSignatureGateBase;
using Editor::kSignatureSpanBase;
using Editor::PieceAxis;
using V3 = Mathematics::Vector3;

namespace
{

constexpr uint32 kSeed = 7u;

// A piece laid along +Z: its length is its Z extent.
FencePieceBounds AlongZ(float32 length)
{
    FencePieceBounds bounds;
    bounds.HalfExtents = V3(0.05f, 0.5f, length * 0.5f);
    bounds.Center = V3(0.0f, 0.5f, 0.0f);
    return bounds;
}

// A piece laid along +X, so an emission that read the wrong pool's bounds
// carries the wrong axis.
FencePieceBounds AlongX(float32 length)
{
    FencePieceBounds bounds;
    bounds.HalfExtents = V3(length * 0.5f, 0.5f, 0.05f);
    bounds.Center = V3(0.0f, 0.5f, 0.0f);
    return bounds;
}

FenceCrest RowPiece(uint32 slot, bool isCap)
{
    FenceCrest crest;
    crest.PoolSlot = slot;
    crest.IsCap = isCap;
    return crest;
}

struct Pools
{
    std::array<FencePoolPiece, 2> Posts{};
    std::array<FencePoolPiece, 2> Spans{};
    std::array<FencePoolPiece, 2> Crests{};
    std::array<FencePoolPiece, 2> Caps{};
    std::array<FencePoolPiece, 2> Gates{};
    std::array<FencePieceBounds, 2> GateBounds{AlongX(7.35f), AlongX(4.0f)};
    std::array<FencePieceBounds, 2> SpanBounds{AlongZ(2.0f), AlongZ(3.0f)};
    std::array<FencePieceBounds, 2> CrestBounds{AlongZ(1.0f), AlongZ(0.5f)};
    std::array<FencePieceBounds, 2> CapBounds{AlongX(0.4f), AlongX(0.3f)};

    FenceEmissionPools View(bool withPosts) const
    {
        FenceEmissionPools pools;
        if (withPosts)
            pools.Posts = Posts;
        pools.Spans = Spans;
        pools.Crests = Crests;
        pools.Caps = Caps;
        pools.Gates = Gates;
        pools.SpanBounds = SpanBounds;
        pools.GateBounds = GateBounds;
        pools.CrestBounds = CrestBounds;
        pools.CapBounds = CapBounds;
        pools.PostAxis = PieceAxis::X;
        pools.Seed = kSeed;
        return pools;
    }
};

// Three stations, two spans and a crest row laid the way the layout lays one:
// a cap opening a stretch, the crests, and a cap closing it.
FenceLayoutResult HandBuiltLayout()
{
    FenceLayoutResult built;
    for (uint32 i = 0; i < 3u; ++i)
    {
        FenceStation station;
        station.Index = i;
        built.Stations.push_back(station);
    }
    FenceSpan first;
    first.PoolSlot = 1u;
    first.LengthScale = 1.1f;
    FenceSpan second;
    second.PoolSlot = 0u;
    second.LengthScale = 0.95f;
    built.Spans = {first, second};
    built.Crests = {RowPiece(1u, true), RowPiece(0u, false), RowPiece(1u, false),
                    RowPiece(0u, true)};
    return built;
}

} // namespace

// Stations first, then spans, then the crest row in the order the layout laid
// it: the order BuildFenceLayout spent the one piece budget in. Each role takes
// its own signature base and label.
TEST(FenceEmission, EmitsStationsSpansThenTheCrestRowUnderRoleSignatures)
{
    const Pools pools;
    const FenceLayoutResult built = HandBuiltLayout();
    const std::vector<FenceEmission> emissions = BuildFenceEmissions(built, pools.View(true));
    ASSERT_EQ(emissions.size(), 9u);

    for (uint32 i = 0; i < 3u; ++i)
    {
        const FenceEmission& post = emissions[i];
        const uint32 pick =
            Components::SplinePoolSelect(kSeed, Components::SplinePoolRole::Post, i, 2u);
        EXPECT_EQ(post.Pose, &built.Stations[i].Pose) << i;
        EXPECT_EQ(post.Source, &pools.Posts[pick]) << i;
        EXPECT_EQ(post.Signature, pick) << i;
        EXPECT_EQ(post.Axis, PieceAxis::X) << i;
        EXPECT_EQ(post.LengthScale, 1.0f) << i;
        EXPECT_EQ(std::string(post.RoleLabel), "Post") << i;
        EXPECT_EQ(post.LabelIndex, i) << i;
    }

    for (uint32 k = 0; k < 2u; ++k)
    {
        const FenceEmission& span = emissions[3u + k];
        const FenceSpan& source = built.Spans[k];
        EXPECT_EQ(span.Pose, &source.Pose) << k;
        EXPECT_EQ(span.Source, &pools.Spans[source.PoolSlot]) << k;
        EXPECT_EQ(span.Signature, kSignatureSpanBase + source.PoolSlot) << k;
        EXPECT_EQ(span.Axis, PieceAxis::Z) << k;
        EXPECT_EQ(span.LengthScale, source.LengthScale) << k;
        EXPECT_EQ(std::string(span.RoleLabel), "Span") << k;
        EXPECT_EQ(span.LabelIndex, k) << k;
    }

    // The row in layout order: cap, crest, crest, cap. Crests and caps count
    // their labels separately, and a cap draws the cap pool and its bounds.
    struct Expected
    {
        const FencePoolPiece* Source;
        uint32 Signature;
        PieceAxis Axis;
        const char* Label;
        uint32 LabelIndex;
    };
    const Expected row[4] = {
        {&pools.Caps[1], kSignatureCapBase + 1u, PieceAxis::X, "Cap", 0u},
        {&pools.Crests[0], kSignatureCrestBase + 0u, PieceAxis::Z, "Crest", 0u},
        {&pools.Crests[1], kSignatureCrestBase + 1u, PieceAxis::Z, "Crest", 1u},
        {&pools.Caps[0], kSignatureCapBase + 0u, PieceAxis::X, "Cap", 1u},
    };
    for (uint32 k = 0; k < 4u; ++k)
    {
        const FenceEmission& piece = emissions[5u + k];
        EXPECT_EQ(piece.Pose, &built.Crests[k].Pose) << k;
        EXPECT_EQ(piece.Source, row[k].Source) << k;
        EXPECT_EQ(piece.Signature, row[k].Signature) << k;
        EXPECT_EQ(piece.Axis, row[k].Axis) << k;
        EXPECT_EQ(piece.LengthScale, 1.0f) << k;
        EXPECT_EQ(std::string(piece.RoleLabel), row[k].Label) << k;
        EXPECT_EQ(piece.LabelIndex, row[k].LabelIndex) << k;
    }

    // The bases keep every role's signatures apart, so a rebuild that swapped a
    // crest for a cap of the same slot never reads as mesh-stable.
    EXPECT_EQ(kSignatureSpanBase, 1000u);
    EXPECT_EQ(kSignatureCrestBase, 2000u);
    EXPECT_EQ(kSignatureCapBase, 3000u);
    EXPECT_EQ(kSignatureGateBase, 4000u);
}

// A gate is a span in the list, in its place among the spans, but it draws its
// gate-pool piece with that pool's axis and signs under the gate base: a span
// that becomes a gate of the same slot is a new mesh, never a transform rewrite.
TEST(FenceEmission, AGateDrawsItsGatePieceUnderTheGateSignature)
{
    const Pools resolved;
    FenceLayoutResult built = HandBuiltLayout();
    built.Spans[1].IsGate = true;
    built.Spans[1].PoolSlot = 1u;
    const std::vector<FenceEmission> emissions = BuildFenceEmissions(built, resolved.View(false));
    ASSERT_EQ(emissions.size(), built.Spans.size() + built.Crests.size());

    const FenceEmission& wall = emissions[0];
    EXPECT_EQ(wall.Source, &resolved.Spans[1]);
    EXPECT_EQ(wall.Signature, kSignatureSpanBase + 1u);
    EXPECT_EQ(std::string(wall.RoleLabel), "Span");
    EXPECT_EQ(wall.LabelIndex, 0u);

    const FenceEmission& gate = emissions[1];
    EXPECT_EQ(gate.Pose, &built.Spans[1].Pose);
    EXPECT_EQ(gate.Source, &resolved.Gates[1]);
    EXPECT_EQ(gate.Signature, kSignatureGateBase + 1u);
    EXPECT_EQ(gate.Axis, PieceAxis::X) << "the gate was laid on its own pool's axis";
    EXPECT_EQ(gate.LengthScale, built.Spans[1].LengthScale);
    EXPECT_EQ(std::string(gate.RoleLabel), "Gate");
    EXPECT_EQ(gate.LabelIndex, 0u);

    // The same span as a wall of the same slot signs differently.
    built.Spans[1].IsGate = false;
    const std::vector<FenceEmission> walled = BuildFenceEmissions(built, resolved.View(false));
    EXPECT_NE(walled[1].Signature, gate.Signature);
}

// A fence whose post pool is empty emits no post at its stations, and the rest
// of the list is unchanged.
TEST(FenceEmission, NoPostPoolEmitsNoStations)
{
    const Pools pools;
    const FenceLayoutResult built = HandBuiltLayout();
    const std::vector<FenceEmission> emissions = BuildFenceEmissions(built, pools.View(false));
    ASSERT_EQ(emissions.size(), 6u);
    EXPECT_EQ(std::string(emissions.front().RoleLabel), "Span");
    EXPECT_EQ(emissions.front().Pose, &built.Spans.front().Pose);
}

// A crest registered to its span carries the span's scale into the transform,
// so it ends where its wall ends. The case comes from the layout itself: 18 m
// of straight run holds four 5 m walls squeezed to 0.9, and a 5 m crest
// registers to each.
TEST(FenceEmission, ARegisteredCrestCarriesItsWallsLengthScale)
{
    constexpr uint32 kSamples = 37u;
    constexpr float32 kRunMetres = 18.0f;
    std::vector<CenterSample> center;
    for (uint32 i = 0; i < kSamples; ++i)
    {
        CenterSample sample;
        sample.Pos = V3(0.0f, 0.0f,
                        kRunMetres * static_cast<float32>(i) / static_cast<float32>(kSamples - 1u));
        sample.Normal = V3(0.0f, 1.0f, 0.0f);
        center.push_back(sample);
    }
    const float32 boundaries[2] = {0.0f, static_cast<float32>(kSamples - 1u)};
    const FencePieceBounds walls[1] = {AlongZ(5.0f)};
    const FencePieceBounds crests[1] = {AlongZ(5.0f)};
    FenceLayoutParams params;
    params.RunBoundaries = boundaries;
    params.SpanPieces = walls;
    params.CrestPieces = crests;
    const FenceLayoutResult built = Editor::BuildFenceLayout(center, params);
    ASSERT_EQ(built.Spans.size(), 4u);
    ASSERT_EQ(built.Crests.size(), 4u);

    const std::array<FencePoolPiece, 1> wallPool{};
    const std::array<FencePoolPiece, 1> crestPool{};
    FenceEmissionPools pools;
    pools.Spans = wallPool;
    pools.Crests = crestPool;
    pools.SpanBounds = walls;
    pools.CrestBounds = crests;
    const std::vector<FenceEmission> emissions = BuildFenceEmissions(built, pools);
    ASSERT_EQ(emissions.size(), 8u);
    for (uint32 k = 0; k < 4u; ++k)
    {
        const FenceEmission& crest = emissions[4u + k];
        ASSERT_EQ(std::string(crest.RoleLabel), "Crest") << k;
        EXPECT_NEAR(crest.LengthScale, 0.9f, 1.0e-4f) << k;
        EXPECT_EQ(crest.LengthScale, emissions[k].LengthScale) << "the wall under it, " << k;
    }
}

// A mitred span and a registered crest draw the cut variant the pools name for
// them (fence design section 4c), keep their pose and scale, and sign with the
// variant's mesh, so a rebuild that moves a piece onto another variant is never
// read as mesh-stable. Every other piece draws its pool piece as before.
TEST(FenceEmission, MitredPiecesDrawTheirVariantsUnderTheVariantsMesh)
{
    const Pools pools;
    const FenceLayoutResult built = HandBuiltLayout();
    std::array<FencePoolPiece, 2> variants{};
    variants[0].Renderer.meshGpuHandleId = 101u;
    variants[1].Renderer.meshGpuHandleId = 202u;
    const uint32 kPool = SplineLayout::kNoMitreVariant;
    const std::array<uint32, 2> spanVariants{kPool, 0u};
    const std::array<uint32, 4> crestVariants{kPool, 1u, kPool, kPool};
    FenceEmissionPools view = pools.View(false);
    view.Variants = variants;
    view.SpanVariants = spanVariants;
    view.CrestVariants = crestVariants;
    const std::vector<FenceEmission> emissions = BuildFenceEmissions(built, view);
    ASSERT_EQ(emissions.size(), 6u);

    EXPECT_EQ(emissions[0].Source, &pools.Spans[built.Spans[0].PoolSlot]);
    EXPECT_EQ(emissions[0].Signature, kSignatureSpanBase + built.Spans[0].PoolSlot);
    EXPECT_EQ(emissions[1].Source, &variants[0]);
    EXPECT_EQ(emissions[1].Signature, Editor::kSignatureMitreVariant | 101u);
    EXPECT_EQ(emissions[1].LengthScale, built.Spans[1].LengthScale);
    EXPECT_EQ(emissions[1].Pose, &built.Spans[1].Pose);

    EXPECT_EQ(emissions[3].Source, &variants[1]) << "the row's first crest";
    EXPECT_EQ(emissions[3].Signature, Editor::kSignatureMitreVariant | 202u);
    EXPECT_EQ(std::string(emissions[3].RoleLabel), "Crest");
    EXPECT_EQ(emissions[2].Source, &pools.Caps[1]);
    EXPECT_EQ(emissions[4].Source, &pools.Crests[1]);
}
