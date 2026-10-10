// The fence's cut span variants from layout to registry: the plan the layout
// makes (SplineLayout/SpanMitre.h) committed through the chunk commit the way
// SplineFenceController::Rebuild commits it (Placement/FenceMitreVariants.h).
// The controller itself is compiled into no test target — it resolves its pools
// through live RenderServices — so this drives the unit it hands the plan to,
// on a registry with no device.

#include "Placement/FenceMitreVariants.h"

#include "Assets/ModelAsset.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "Placement/FenceLayout.h"
#include "Placement/SplineChunkCommit.h"
#include "SplineLayout/SpanMitre.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace GameEngine;
using Editor::CommitFenceMitreVariants;
using Editor::FenceMitreCuts;
using Editor::FenceMitreSource;
using Editor::FenceMitreVariants;
using Editor::SplineChunkCommit;
using Rendering::MeshGPURegistry;
using SplineLayout::CenterSample;
using SplineLayout::FenceLayoutParams;
using SplineLayout::FenceLayoutResult;
using SplineLayout::FencePieceBounds;
using SplineLayout::FenceSpan;
using SplineLayout::MitreVariantPlan;
using SplineLayout::PlanMitreVariants;
using V3 = Mathematics::Vector3;

namespace
{

constexpr float kPi = 3.14159265358979f;

// The castle kit's 5 m wall: 5 x 5 x 0.5 m along local X, corner-pivoted.
FencePieceBounds CastleWall()
{
    FencePieceBounds b;
    b.HalfExtents = V3(2.5f, 2.5f, 0.25f);
    b.Center = V3(-2.5f, 2.5f, 0.0f);
    return b;
}

void AddQuad(Mesh& mesh, const V3& a, const V3& b, const V3& c, const V3& d, const V3& normal)
{
    const uint32 base = static_cast<uint32>(mesh.Vertices.size());
    for (const V3& p : {a, b, c, d})
    {
        Vertex v{};
        v.Position[0] = p.x;
        v.Position[1] = p.y;
        v.Position[2] = p.z;
        v.Normal[0] = normal.x;
        v.Normal[1] = normal.y;
        v.Normal[2] = normal.z;
        mesh.Vertices.push_back(v);
    }
    if (V3::Dot(V3::Cross(b - a, c - a), normal) > 0.0f)
        mesh.Indices.insert(mesh.Indices.end(), {base, base + 1u, base + 2u, base, base + 2u, base + 3u});
    else
        mesh.Indices.insert(mesh.Indices.end(), {base, base + 2u, base + 1u, base, base + 3u, base + 2u});
}

// The wall's bounds as a closed box; `open` leaves its top off.
Mesh WallMesh(bool open = false)
{
    const FencePieceBounds b = CastleWall();
    const V3 lo = b.Center - b.HalfExtents;
    const V3 hi = b.Center + b.HalfExtents;
    Mesh mesh;
    mesh.Name = open ? "OpenWall" : "Wall";
    AddQuad(mesh, V3(lo.x, lo.y, lo.z), V3(lo.x, hi.y, lo.z), V3(lo.x, hi.y, hi.z), V3(lo.x, lo.y, hi.z), V3(-1, 0, 0));
    AddQuad(mesh, V3(hi.x, lo.y, lo.z), V3(hi.x, lo.y, hi.z), V3(hi.x, hi.y, hi.z), V3(hi.x, hi.y, lo.z), V3(1, 0, 0));
    AddQuad(mesh, V3(lo.x, lo.y, lo.z), V3(lo.x, lo.y, hi.z), V3(hi.x, lo.y, hi.z), V3(hi.x, lo.y, lo.z), V3(0, -1, 0));
    if (!open)
        AddQuad(mesh, V3(lo.x, hi.y, lo.z), V3(hi.x, hi.y, lo.z), V3(hi.x, hi.y, hi.z), V3(lo.x, hi.y, hi.z), V3(0, 1, 0));
    AddQuad(mesh, V3(lo.x, lo.y, lo.z), V3(hi.x, lo.y, lo.z), V3(hi.x, hi.y, lo.z), V3(lo.x, hi.y, lo.z), V3(0, 0, -1));
    AddQuad(mesh, V3(lo.x, lo.y, hi.z), V3(lo.x, hi.y, hi.z), V3(hi.x, hi.y, hi.z), V3(hi.x, lo.y, hi.z), V3(0, 0, 1));
    mesh.MinBounds[0] = lo.x;
    mesh.MinBounds[1] = lo.y;
    mesh.MinBounds[2] = lo.z;
    mesh.MaxBounds[0] = hi.x;
    mesh.MaxBounds[1] = hi.y;
    mesh.MaxBounds[2] = hi.z;
    return mesh;
}

// Castle walls on a level arc of `radius` metres turning right by `degrees`,
// no posts. A zero turn is a straight run.
FenceLayoutResult BareWall(float radius, float degrees)
{
    std::vector<CenterSample> center;
    const float step = 0.05f;
    if (degrees <= 0.0f)
    {
        for (uint32 i = 0; i <= 400u; ++i)
            center.push_back({V3(0.0f, 0.0f, step * static_cast<float>(i)), V3(0, 1, 0), true});
    }
    else
    {
        const float arc = radius * degrees * kPi / 180.0f;
        const uint32 steps = static_cast<uint32>(std::lround(arc / step));
        for (uint32 i = 0; i <= steps; ++i)
        {
            const float angle = arc * static_cast<float>(i) / static_cast<float>(steps) / radius;
            center.push_back({V3(radius - radius * std::cos(angle), 0.0f, radius * std::sin(angle)),
                              V3(0, 1, 0), true});
        }
    }
    const float boundaries[2] = {0.0f, static_cast<float>(center.size() - 1u)};
    const FencePieceBounds spans[1] = {CastleWall()};
    FenceLayoutParams params;
    params.RunBoundaries = boundaries;
    params.SpanPieces = spans;
    return SplineLayout::BuildFenceLayout(center, params);
}

// The controller's view of its span pool: one slot.
std::vector<FenceMitreSource> Sources(const Mesh& mesh)
{
    FenceMitreSource source;
    source.Model = GUID::Derive(GUID::Null(), "castle-wall");
    source.Name = mesh.Name;
    source.SourceMesh = &mesh;
    source.Bounds = CastleWall();
    return {source};
}

constexpr uint32 kEntity = 42u;

} // namespace

// A straight run mitres nothing, so it registers nothing.
TEST(FenceMitreVariants, AStraightRunRegistersNoVariant)
{
    MeshGPURegistry registry;
    SplineChunkCommit commit;
    FenceMitreCuts cuts;
    const Mesh wall = WallMesh();
    const FenceLayoutResult layout = BareWall(0.0f, 0.0f);
    ASSERT_EQ(layout.Spans.size(), 4u);
    const MitreVariantPlan plan = PlanMitreVariants(layout);
    const FenceMitreVariants variants =
        CommitFenceMitreVariants(registry, commit, cuts, kEntity, plan, {Sources(wall)});
    EXPECT_TRUE(plan.Variants.empty());
    EXPECT_TRUE(variants.Meshes.empty());
    EXPECT_TRUE(cuts.empty());
    EXPECT_EQ(registry.GetEntryCount(), 0u);
}

// The equal spans of a uniform arc share one variant, so they stay
// instanced: an arc of many walls registers three meshes — the opening span,
// every interior one, the closing span.
TEST(FenceMitreVariants, EqualJoinsShareOneVariant)
{
    MeshGPURegistry registry;
    SplineChunkCommit commit;
    FenceMitreCuts cuts;
    const Mesh wall = WallMesh();
    const FenceLayoutResult layout = BareWall(20.0f, 90.0f);
    ASSERT_GE(layout.Spans.size(), 5u);
    const MitreVariantPlan plan = PlanMitreVariants(layout);
    ASSERT_EQ(plan.SpanVariants.size(), layout.Spans.size());
    EXPECT_EQ(plan.Variants.size(), 3u);
    const FenceMitreVariants variants =
        CommitFenceMitreVariants(registry, commit, cuts, kEntity, plan, {Sources(wall)});
    EXPECT_EQ(variants.Meshes.size(), 3u);
    EXPECT_EQ(registry.GetEntryCount(), 3u);
    for (size_t k = 1; k + 1u < layout.Spans.size(); ++k)
        EXPECT_EQ(plan.SpanVariants[k], plan.SpanVariants[1]);
    EXPECT_TRUE(variants.Report.empty());
}

// A gate is a span at a bare join, so it is mitred, and its variant is cut
// from the gate pool's piece: a gate whose pool offers no mesh draws uncut
// while the walls beside it are still cut from theirs.
TEST(FenceMitreVariants, AMitredGateIsCutFromTheGatePool)
{
    std::vector<CenterSample> center;
    for (uint32 i = 0; i <= 400u; ++i)
    {
        const float angle = 20.0f * kPi / 180.0f * static_cast<float>(i) / 400.0f;
        const float radius = 20.0f / (20.0f * kPi / 180.0f);
        center.push_back({V3(radius - radius * std::cos(angle), 0.0f, radius * std::sin(angle)),
                          V3(0, 1, 0), true});
    }
    const float boundaries[2] = {0.0f, 400.0f};
    const FencePieceBounds spans[1] = {CastleWall()};
    const FencePieceBounds gates[1] = {CastleWall()};
    const Components::SplineSpanOverride overrides[1] = {
        {0u, 1u, Components::SplineSpanOverrideKind::Gate, 0u}};
    FenceLayoutParams params;
    params.RunBoundaries = boundaries;
    params.SpanPieces = spans;
    params.GatePieces = gates;
    params.SpanOverrides = overrides;
    const FenceLayoutResult layout = SplineLayout::BuildFenceLayout(center, params);
    ASSERT_EQ(layout.Spans.size(), 4u);
    ASSERT_TRUE(layout.Spans[1].IsGate);
    ASSERT_TRUE(layout.Spans[1].Start.Mitred && layout.Spans[1].End.Mitred);
    const MitreVariantPlan plan = PlanMitreVariants(layout);
    const uint32 gateVariant = plan.SpanVariants[1];
    ASSERT_NE(gateVariant, SplineLayout::kNoMitreVariant);
    EXPECT_EQ(plan.Variants[gateVariant].Role, SplineLayout::MitrePieceRole::Gate);

    const Mesh wall = WallMesh();
    std::vector<FenceMitreSource> noGateMesh = Sources(wall);
    noGateMesh[0].SourceMesh = nullptr;
    MeshGPURegistry registry;
    SplineChunkCommit commit;
    FenceMitreCuts cuts;
    const FenceMitreVariants uncutGate = CommitFenceMitreVariants(
        registry, commit, cuts, kEntity, plan, {Sources(wall), {}, noGateMesh});
    EXPECT_FALSE(uncutGate.Meshes[gateVariant].Handle.IsValid());
    EXPECT_TRUE(uncutGate.Meshes[plan.SpanVariants[0]].Handle.IsValid());

    MeshGPURegistry cutRegistry;
    SplineChunkCommit cutCommit;
    FenceMitreCuts cutCuts;
    const FenceMitreVariants cutGate = CommitFenceMitreVariants(
        cutRegistry, cutCommit, cutCuts, kEntity, plan, {Sources(wall), {}, Sources(wall)});
    EXPECT_TRUE(cutGate.Meshes[gateVariant].Handle.IsValid());
}

// Two identical rebuilds draw the same meshes under the same handles; the
// second cuts nothing and registers nothing new.
TEST(FenceMitreVariants, VariantKeysAreStableAcrossTwoIdenticalRebuilds)
{
    MeshGPURegistry registry;
    SplineChunkCommit commit;
    FenceMitreCuts cuts;
    const Mesh wall = WallMesh();
    const FenceLayoutResult layout = BareWall(20.0f, 90.0f);
    const MitreVariantPlan plan = PlanMitreVariants(layout);
    const FenceMitreVariants first =
        CommitFenceMitreVariants(registry, commit, cuts, kEntity, plan, {Sources(wall)});
    const size_t entries = registry.GetEntryCount();
    const FenceMitreVariants second =
        CommitFenceMitreVariants(registry, commit, cuts, kEntity, PlanMitreVariants(layout),
                                 {Sources(wall)});
    ASSERT_EQ(first.Meshes.size(), second.Meshes.size());
    for (size_t v = 0; v < first.Meshes.size(); ++v)
    {
        EXPECT_EQ(first.Meshes[v].Handle, second.Meshes[v].Handle) << v;
        EXPECT_EQ(first.Meshes[v].Bounds.center.x, second.Meshes[v].Bounds.center.x) << v;
        EXPECT_EQ(first.Meshes[v].Bounds.halfExtents.x, second.Meshes[v].Bounds.halfExtents.x) << v;
    }
    EXPECT_EQ(registry.GetEntryCount(), entries);
}

// Straightening the corner releases the keys its joins registered: the
// registry holds nothing for a fence that no longer mitres.
TEST(FenceMitreVariants, StraighteningACornerReleasesItsKeys)
{
    MeshGPURegistry registry;
    SplineChunkCommit commit;
    FenceMitreCuts cuts;
    const Mesh wall = WallMesh();
    CommitFenceMitreVariants(registry, commit, cuts, kEntity, PlanMitreVariants(BareWall(20.0f, 90.0f)),
                             {Sources(wall)});
    ASSERT_GT(registry.GetEntryCount(), 0u);
    CommitFenceMitreVariants(registry, commit, cuts, kEntity, PlanMitreVariants(BareWall(0.0f, 0.0f)),
                             {Sources(wall)});
    EXPECT_EQ(registry.GetEntryCount(), 0u);
    EXPECT_TRUE(cuts.empty());
}

// The budget is bytes, not a count of variants: 300 distinct joins of a small
// piece all get their variant under the default 16 MB, while a budget one byte
// short of three variants keeps the first two and leaves every later mitred
// piece uncut, reported once with the budget in KB, since it is under 1 MB.
TEST(FenceMitreVariants, TheBudgetCountsBytesNotVariants)
{
    const auto layoutOf = [](size_t distinct)
    {
        FenceLayoutResult layout;
        for (size_t k = 0; k < distinct; ++k)
        {
            FenceSpan span;
            span.LengthScale = 1.0f + 0.001f * static_cast<float>(k);
            span.End.Mitred = true;
            span.End.TurnRadians = 10.0f * kPi / 180.0f;
            layout.Spans.push_back(span);
        }
        return layout;
    };
    const Mesh wall = WallMesh();

    {
        MeshGPURegistry registry;
        SplineChunkCommit commit;
        FenceMitreCuts cuts;
        const MitreVariantPlan many = PlanMitreVariants(layoutOf(300u));
        ASSERT_EQ(many.Variants.size(), 300u);
        const FenceMitreVariants variants =
            CommitFenceMitreVariants(registry, commit, cuts, kEntity, many, {Sources(wall)});
        EXPECT_EQ(registry.GetEntryCount(), 300u) << "a variant count must not cap the fence";
        EXPECT_EQ(variants.PiecesPastBudget, 0u);
        EXPECT_TRUE(variants.Report.empty());
    }

    const MitreVariantPlan three = PlanMitreVariants(layoutOf(3u));
    MeshGPURegistry registry;
    SplineChunkCommit commit;
    FenceMitreCuts cuts;
    CommitFenceMitreVariants(registry, commit, cuts, kEntity, three, {Sources(wall)});
    ASSERT_EQ(cuts.size(), 3u);
    uint64 allThree = 0;
    for (const auto& [key, cut] : cuts)
        allThree += cut.Bytes;
    ASSERT_GT(allThree, 0u);

    MeshGPURegistry tightRegistry;
    SplineChunkCommit tightCommit;
    FenceMitreCuts tightCuts;
    const FenceMitreVariants tight = CommitFenceMitreVariants(
        tightRegistry, tightCommit, tightCuts, kEntity, three, {Sources(wall)}, allThree - 1u);
    EXPECT_EQ(tightRegistry.GetEntryCount(), 2u);
    EXPECT_TRUE(tight.Meshes[1].Handle.IsValid());
    EXPECT_FALSE(tight.Meshes[2].Handle.IsValid());
    EXPECT_EQ(tight.PiecesPastBudget, 1u);
    ASSERT_EQ(tight.Report.size(), 1u);
    const std::string budget = std::to_string((allThree - 1u) / 1024u) + " KB budget";
    ASSERT_NE(budget, "0 KB budget") << "the fixture must print a nonzero budget";
    EXPECT_NE(tight.Report[0].find("1 mitred piece(s) are past the " + budget), std::string::npos)
        << tight.Report[0];
}

// A span mesh that is not closed where the join cuts it is named, once, and
// stays named on the rebuild that keeps its variants rather than cutting them
// again.
TEST(FenceMitreVariants, AnOpenMeshIsNamedOnEveryRebuild)
{
    MeshGPURegistry registry;
    SplineChunkCommit commit;
    FenceMitreCuts cuts;
    const Mesh open = WallMesh(true);
    const FenceLayoutResult layout = BareWall(20.0f, 90.0f);
    for (int build = 0; build < 2; ++build)
    {
        const FenceMitreVariants variants = CommitFenceMitreVariants(
            registry, commit, cuts, kEntity, PlanMitreVariants(layout), {Sources(open)});
        ASSERT_EQ(variants.Report.size(), 1u) << "build " << build;
        EXPECT_NE(variants.Report[0].find("'OpenWall'"), std::string::npos) << variants.Report[0];
    }
}

// A reimported piece keeps its model GUID, its handle and its bounds, so its
// content is part of the variant key: the same joins cut from new content are
// new variants, and the old ones are released.
TEST(FenceMitreVariants, AReimportedSourceNamesNewVariantsAndReleasesTheOld)
{
    MeshGPURegistry registry;
    SplineChunkCommit commit;
    FenceMitreCuts cuts;
    const Mesh wall = WallMesh();
    const MitreVariantPlan plan = PlanMitreVariants(BareWall(20.0f, 90.0f));
    std::vector<FenceMitreSource> sources = Sources(wall);
    sources[0].Content = 1u;
    const FenceMitreVariants before =
        CommitFenceMitreVariants(registry, commit, cuts, kEntity, plan, {sources});
    const size_t entries = registry.GetEntryCount();
    ASSERT_GT(entries, 0u);
    sources[0].Content = 2u;
    const FenceMitreVariants after =
        CommitFenceMitreVariants(registry, commit, cuts, kEntity, plan, {sources});
    ASSERT_EQ(before.Meshes.size(), after.Meshes.size());
    for (size_t v = 0; v < before.Meshes.size(); ++v)
    {
        EXPECT_NE(before.Meshes[v].Handle, after.Meshes[v].Handle) << v;
        EXPECT_EQ(registry.Find(before.Meshes[v].Handle), nullptr) << v;
    }
    EXPECT_EQ(registry.GetEntryCount(), entries);
}

// A mitred piece draws the variant's mesh within the variant's bounds and
// keeps everything else of its pool piece, the model GUID included: picking
// and the bakers resolve the piece by it.
TEST(FenceMitreVariants, AVariantPieceKeepsItsModelAndSwapsItsMeshAndBounds)
{
    Editor::FencePoolPiece pool;
    pool.Renderer.modelAssetGuid = Components::ModelRef(GUID::Derive(GUID::Null(), "castle-wall"));
    pool.Renderer.meshGpuHandleId = 7u;
    pool.Renderer.castShadows = true;
    pool.Bounds.Box = Mathematics::BoundingBox::FromMinMax(V3(-5, 0, -0.25f), V3(0, 5, 0.25f));
    const Mesh wall = WallMesh();
    pool.SourceMesh = &wall;
    Editor::FenceMitreMesh variant;
    variant.Handle = Rendering::MeshGPUHandle(uint64{11u});
    variant.Bounds = Mathematics::BoundingBox::FromMinMax(V3(-5.1f, 0, -0.25f), V3(0.1f, 5, 0.25f));
    const Editor::FencePoolPiece piece = Editor::MitreVariantPiece(pool, variant);
    EXPECT_EQ(piece.Renderer.modelAssetGuid, pool.Renderer.modelAssetGuid);
    EXPECT_EQ(piece.Renderer.meshGpuHandleId, static_cast<uint64>(variant.Handle));
    EXPECT_NE(piece.Renderer.meshGpuHandleId, pool.Renderer.meshGpuHandleId);
    EXPECT_TRUE(piece.Renderer.castShadows);
    EXPECT_EQ(piece.Bounds.Box.center.x, variant.Bounds.center.x);
    EXPECT_EQ(piece.Bounds.Box.halfExtents.x, variant.Bounds.halfExtents.x);
    EXPECT_EQ(piece.SourceMesh, &wall);
}

// One model listed in two span pool slots (two weights for one wall) plans a
// variant per slot, but they are one mesh: registered once, drawn by both, and
// its bytes counted once against the budget.
TEST(FenceMitreVariants, OneModelInTwoSlotsIsOneMesh)
{
    FenceLayoutResult layout = BareWall(20.0f, 90.0f);
    ASSERT_GE(layout.Spans.size(), 5u);
    for (size_t k = 0; k < layout.Spans.size(); ++k)
        layout.Spans[k].PoolSlot = static_cast<uint32>(k % 2u);
    const MitreVariantPlan plan = PlanMitreVariants(layout);
    const Mesh wall = WallMesh();
    std::vector<FenceMitreSource> sources = Sources(wall);
    sources.push_back(sources[0]);

    MeshGPURegistry single;
    SplineChunkCommit singleCommit;
    FenceMitreCuts singleCuts;
    CommitFenceMitreVariants(single, singleCommit, singleCuts, kEntity,
                             PlanMitreVariants(BareWall(20.0f, 90.0f)), {Sources(wall)});

    MeshGPURegistry registry;
    SplineChunkCommit commit;
    FenceMitreCuts cuts;
    const FenceMitreVariants variants =
        CommitFenceMitreVariants(registry, commit, cuts, kEntity, plan, {sources});
    ASSERT_GT(plan.Variants.size(), single.GetEntryCount());
    EXPECT_EQ(registry.GetEntryCount(), single.GetEntryCount());
    EXPECT_EQ(cuts.size(), singleCuts.size());
    for (size_t v = 0; v < plan.Variants.size(); ++v)
    {
        EXPECT_TRUE(variants.Meshes[v].Handle.IsValid()) << v;
        for (size_t w = 0; w < plan.Variants.size(); ++w)
        {
            if (plan.Variants[v].Shape == plan.Variants[w].Shape)
                EXPECT_EQ(variants.Meshes[v].Handle, variants.Meshes[w].Handle) << v << "," << w;
        }
    }

    uint64 singleBytes = 0;
    for (const auto& [key, cut] : singleCuts)
        singleBytes += cut.Bytes;
    MeshGPURegistry tightRegistry;
    SplineChunkCommit tightCommit;
    FenceMitreCuts tightCuts;
    const FenceMitreVariants tight = CommitFenceMitreVariants(
        tightRegistry, tightCommit, tightCuts, kEntity, plan, {sources}, singleBytes);
    EXPECT_EQ(tight.PiecesPastBudget, 0u) << "a shared mesh must count its bytes once";
}

