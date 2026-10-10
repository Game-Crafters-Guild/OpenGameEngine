// Undo-stack oracle for the planet (sphere) sculpt brush stroke. The interactive sphere brush
// (and the terrain_sculpt_dab IPC driver, which shares TerrainBrushTool's Begin/EndSphereStroke)
// wraps one stroke in TerrainService's stroke capture and pushes ONE SphereSculptStrokeCommand.
// This exercises that contract end-to-end against the real service + UndoRedoService:
//   * a stroke pushes exactly one undo entry (fails-before: the sphere brush recorded none),
//   * undo restores the sculpt store byte-identical to pre-stroke,
//   * redo restores byte-identical post-stroke state,
//   * undo/redo advance the sculpt version and feed the render + physics dirty unions — the
//     restore enters the SAME edit-update pipeline a dab does (edit-driven re-tess re-engages).

#include <gtest/gtest.h>

#include "SceneView/SphereSculptStrokeCommand.h"
#include "UndoRedo/UndoRedoService.h"

#include "CBTTerrain/CBTSphereFaceMap.h"
#include "CBTTerrain/SphereSculptLayer.h"
#include "TerrainECS/TerrainService.h"

#include <array>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <vector>

namespace
{
using namespace GameEngine;
using GameEngine::CBTTerrain::SphereEditRegions;
using GameEngine::CBTTerrain::SphereSculptGeometry;
using GameEngine::CBTTerrain::SphereSculptPageState;
using GameEngine::Editor::SphereSculptStrokeCommand;
using GameEngine::Editor::UndoRedoService;
using GameEngine::TerrainECS::TerrainService;

constexpr float kPlanetRadius = 2000.0f;
constexpr float kAngularRadius = 0.05f; // ~100 m brush at R=2000

struct SculptBytes
{
    std::vector<float32> Pool;
    std::vector<uint32> Table;
};

SculptBytes CopyBytes(const TerrainService& svc)
{
    SculptBytes b;
    SphereSculptGeometry geom{};
    svc.CopySphereSculptUpload(b.Pool, b.Table, geom);
    return b;
}

bool BytesEqual(const SculptBytes& a, const SculptBytes& b)
{
    if (a.Pool.size() != b.Pool.size() || a.Table.size() != b.Table.size())
        return false;
    return std::memcmp(a.Pool.data(), b.Pool.data(), a.Pool.size() * sizeof(float32)) == 0 &&
           std::memcmp(a.Table.data(), b.Table.data(), a.Table.size() * sizeof(uint32)) == 0;
}

// Logical bit-identity: same allocated VIRTUAL page set, bit-identical published float per
// virtual texel. Redo re-allocates pages an undo reclaimed, so PHYSICAL page ids may permute (an
// allocator detail); every consumer (GPU sampler, physics mirror) resolves through the page
// table, so logical bit-identity IS byte-identical sampled output.
bool LogicalEqual(const SculptBytes& a, const SculptBytes& b)
{
    using GameEngine::CBTTerrain::kSculptNoPage;
    using GameEngine::CBTTerrain::kSculptPageTexels;
    if (a.Table.size() != b.Table.size())
        return false;
    for (size_t e = 0; e < a.Table.size(); ++e)
    {
        const bool aAlloc = a.Table[e] != kSculptNoPage;
        const bool bAlloc = b.Table[e] != kSculptNoPage;
        if (aAlloc != bAlloc)
            return false;
        if (!aAlloc)
            continue;
        const size_t aBase = static_cast<size_t>(a.Table[e]) * kSculptPageTexels;
        const size_t bBase = static_cast<size_t>(b.Table[e]) * kSculptPageTexels;
        if (aBase + kSculptPageTexels > a.Pool.size() || bBase + kSculptPageTexels > b.Pool.size())
            return false;
        if (std::memcmp(a.Pool.data() + aBase, b.Pool.data() + bBase,
                        kSculptPageTexels * sizeof(float32)) != 0)
            return false;
    }
    return true;
}

class SphereSculptStrokeCommandTests : public ::testing::Test
{
protected:
    void SetUp() override
    {
        if (TerrainService::IsInitialized())
            TerrainService::Shutdown();
        TerrainService::Initialize();
        TerrainService::Get().ConfigurePlanetSculpt(kPlanetRadius);
    }
    void TearDown() override
    {
        if (TerrainService::IsInitialized())
            TerrainService::Shutdown();
    }

    // One brush stroke through the service's stroke-capture path (the exact sequence
    // TerrainBrushTool::BeginSphereStroke / dabs / EndSphereStroke performs), committed to `undo`
    // as one already-applied command. Returns whether an entry was pushed.
    bool Stroke(UndoRedoService& undo, std::initializer_list<std::array<float, 3>> dirs,
                float strength, bool lower)
    {
        auto& svc = TerrainService::Get();
        svc.BeginSphereSculptStrokeCapture();
        for (const auto& d : dirs)
            svc.ApplySphereSculptDab(d[0], d[1], d[2], kAngularRadius, strength, lower);
        std::vector<SphereSculptPageState> before = svc.TakeSphereSculptStrokeCapture();
        if (before.empty())
            return false;
        std::vector<SphereSculptPageState> after = svc.SnapshotSphereSculptPages(before);
        undo.CommitAlreadyApplied(std::make_unique<SphereSculptStrokeCommand>(
            &svc, std::move(before), std::move(after)));
        return true;
    }
};

// Fails-before core: a sphere stroke pushes exactly ONE undo entry, undo restores the sculpt
// store byte-identical to pre-stroke, redo restores byte-identical post-stroke bytes.
TEST_F(SphereSculptStrokeCommandTests, StrokeUndoRedoRestoresBytesThroughUndoStack)
{
    auto& svc = TerrainService::Get();
    UndoRedoService undo;

    // Pre-existing committed content so the undo target is not the empty store.
    ASSERT_TRUE(Stroke(undo, {{0.0f, 1.0f, 0.0f}}, 2.0f, false));
    ASSERT_EQ(undo.GetUndoCount(), 1u);

    const SculptBytes pre = CopyBytes(svc);

    // The stroke under test: several dabs, one entry.
    ASSERT_TRUE(Stroke(undo, {{0.0f, 1.0f, 0.0f}, {0.05f, 1.0f, 0.0f}, {0.1f, 1.0f, 0.05f}},
                       5.0f, false));
    EXPECT_EQ(undo.GetUndoCount(), 2u) << "one stroke (N dabs) must be ONE undo entry";
    EXPECT_STREQ(undo.PeekUndoName(), "Planet Sculpt Stroke");

    const SculptBytes post = CopyBytes(svc);
    ASSERT_FALSE(BytesEqual(pre, post)) << "the stroke must have changed the store";

    undo.Undo();
    EXPECT_TRUE(BytesEqual(CopyBytes(svc), pre))
        << "undo must restore the sculpt pages byte-identical to pre-stroke";

    undo.Redo();
    EXPECT_TRUE(LogicalEqual(CopyBytes(svc), post))
        << "redo must restore the post-stroke state bit-identical per virtual texel "
           "(physical page ids may permute across the reclaim/re-allocate cycle)";
}

// Gate 3: undo/redo ARE edits — the sculpt version advances and the render dirty union (the
// feed for edit-driven re-tessellation / forced VertexEval) reports the restored regions.
TEST_F(SphereSculptStrokeCommandTests, UndoAdvancesVersionAndFeedsRetessDirtyRegions)
{
    auto& svc = TerrainService::Get();
    UndoRedoService undo;

    ASSERT_TRUE(Stroke(undo, {{0.0f, 1.0f, 0.0f}}, 3.0f, false));

    // Drain the stroke's own dirty regions so anything reported next comes from the undo.
    SphereEditRegions drained{};
    ASSERT_TRUE(svc.ConsumeSphereSculptDirtyRegions(drained));
    ASSERT_FALSE(svc.ConsumeSphereSculptDirtyRegions(drained));

    const uint64 versionPostStroke = svc.SphereSculptVersion();

    undo.Undo();
    EXPECT_GT(svc.SphereSculptVersion(), versionPostStroke)
        << "an undo IS an edit: the sculpt version must advance so upload gates re-fire";
    SphereEditRegions undoRegions{};
    ASSERT_TRUE(svc.ConsumeSphereSculptDirtyRegions(undoRegions))
        << "undo must feed the render dirty union that drives edit re-tess";
    EXPECT_GT(undoRegions.Count, 0u);

    const uint64 versionPostUndo = svc.SphereSculptVersion();
    undo.Redo();
    EXPECT_GT(svc.SphereSculptVersion(), versionPostUndo);
    SphereEditRegions redoRegions{};
    ASSERT_TRUE(svc.ConsumeSphereSculptDirtyRegions(redoRegions))
        << "redo must re-enter the same edit-update path";
}

// Gate 3 (physics half): the undo restore also lands in the per-face physics dirty union, so the
// planet-face colliders refresh over the reverted region like any other edit.
TEST_F(SphereSculptStrokeCommandTests, UndoFeedsPhysicsDirtyFaces)
{
    auto& svc = TerrainService::Get();
    UndoRedoService undo;

    const auto dirFace = CBTTerrain::WorldDirToFaceUV(0.0f, 1.0f, 0.0f);
    ASSERT_TRUE(Stroke(undo, {{0.0f, 1.0f, 0.0f}}, 3.0f, false));

    // Colliders consumed the stroke's dirty rect already.
    for (uint32 f = 0; f < TerrainService::kPlanetSculptFaceCount; ++f)
        svc.ClearPlanetSculptDirtyFace(f);
    ASSERT_FALSE(svc.GetPlanetSculptMirror().Faces[dirFace.Face].Touched);

    undo.Undo();
    EXPECT_TRUE(svc.GetPlanetSculptMirror().Faces[dirFace.Face].Touched)
        << "undo must mark the touched face dirty for the physics collider refresh";
}

// Gate 4 at the service level: the capture is lazy and touch-bounded — a stroke's snapshot page
// count equals the pages the stroke materialized on a fresh store.
TEST_F(SphereSculptStrokeCommandTests, CaptureIsBoundedToTouchedPages)
{
    auto& svc = TerrainService::Get();

    svc.BeginSphereSculptStrokeCapture();
    svc.ApplySphereSculptDab(0.0f, 1.0f, 0.0f, kAngularRadius, 3.0f, false);
    svc.ApplySphereSculptDab(0.0f, 1.0f, 0.0f, kAngularRadius, 3.0f, false); // same page(s)
    std::vector<SphereSculptPageState> before = svc.TakeSphereSculptStrokeCapture();

    ASSERT_FALSE(before.empty());
    EXPECT_LE(before.size(), 4u)
        << "a brush-sized stroke must snapshot a handful of pages, not the face area";
    for (const SphereSculptPageState& p : before)
        EXPECT_FALSE(p.Allocated) << "fresh store: pre-images record 'page did not exist'";
}

// An empty stroke (nothing written — e.g. zero-strength or missed planet) records no undo entry:
// TakeSphereSculptStrokeCapture returns empty and the tool pushes nothing.
TEST_F(SphereSculptStrokeCommandTests, EmptyStrokeRecordsNothing)
{
    UndoRedoService undo;

    EXPECT_FALSE(Stroke(undo, {{0.0f, 1.0f, 0.0f}}, 0.0f, false));
    EXPECT_EQ(undo.GetUndoCount(), 0u);
}
} // namespace
