#include <gtest/gtest.h>

#include "UndoRedo/DeleteEntitiesCommand.h"

#include "EditorChangeNotifications.h"
#include "CBTTerrainECS/TerrainProvisioning.h"
#include "Components/Name.h"
#include "Components/Terrain/Terrain.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "TerrainECS/Systems/TerrainWorldHooks.h"
#include "TerrainECS/TerrainHandleHygiene.h"
#include "TerrainECS/TerrainService.h"

namespace
{
using namespace GameEngine;
using GameEngine::ECS::World;
using GameEngine::Editor::DeleteEntitiesCommand;
using GameEngine::Editor::EditorChangeNotifications;
using GameEngine::TerrainECS::TerrainService;

// The terrain brush resolves the active terrain fresh every pointer event via
// FindActiveTerrain (the #492 unified resolver) — it caches no terrain handle.
// So the "brush unbound after undo/redo of the terrain entity" regression is a
// re-resolution question: does the terrain entity remain FindActiveTerrain-live
// across the delete command's preserve-handle destroy/revive cycle?
//
// Deleting the entity DOES free its runtime service data: the Components::Terrain
// OnRemove hook is what gives scene close and entity delete an owner. The delete
// command snapshots the component bytes before the destroy, so undo restores a
// handle pair that no longer resolves — provisioning treats that as stale, clears
// it and builds fresh. Both halves are asserted below, because a fix that frees
// on delete without clearing on revive strands the terrain permanently.
//
// Undo therefore hands back a DIFFERENT terrain than the one deleted, and that is
// the intended contract. Nothing user-authored rides on the destroyed object:
// every TerrainData / TiledTerrainData field is re-derivable from the component's
// own serialized fields plus the live modifier entities — heightfield and splatmap
// are bake outputs, quadtree and normalmap are rebuilt from those, and the rest are
// caches, dirty logs and streaming cursors. Sculpt and paint strokes are not in
// there at all: planar strokes live in TerrainService's GUID-keyed zone-payload
// store and spherical ones in its sphere-sculpt layer, both of which DestroyTerrain
// leaves untouched, and the next bake re-resolves the payloads by GUID. Undo costs
// the re-bake, not the edits. (The planet dab layer has a separate lifecycle:
// CBTUpdateSystem resets it when the active planet entity changes, independent of
// this hook.)
//
// Retaining the terrain across the delete window instead — the SkeletonRef
// treatment in DeleteEntitiesCommand — is deliberately NOT done: the undo stack
// would pin a whole TerrainData per deleted terrain until it trimmed, reopening
// through undo history the residency this hook exists to close.
class TerrainEntityUndoResolutionTests : public ::testing::Test
{
protected:
    void SetUp() override
    {
        if (!TerrainService::IsInitialized())
            TerrainService::Initialize();
    }
};

TEST_F(TerrainEntityUndoResolutionTests, ActiveTerrainResolvesAgainAfterDeleteUndoRedo)
{
    auto& svc = TerrainService::Get();

    // Real tiled-terrain data on the service; the entity references it by handle.
    TerrainECS::TiledTerrainConfig cfg{};
    cfg.WorldSizeX = 2048.0f;
    cfg.WorldSizeZ = 2048.0f;
    cfg.HeightScale = 64.0f;
    cfg.SamplesPerMeter = 1.0f;
    const auto tiled = svc.CreateTiledTerrain(cfg);
    ASSERT_NE(svc.GetTiledTerrainData(tiled), nullptr);

    World world;
    TerrainECS::RegisterTerrainWorldHooks(world); // as EnsurePrimaryWorld does
    EditorChangeNotifications notifications;
    ECS::EntityHandle e = world.CreateEntity();
    Components::Transform xf{};
    Components::Name nm{};
    Components::Terrain t{};
    t.Domain = Components::TerrainDomain::Planar;
    t.SizeX = cfg.WorldSizeX;
    t.SizeZ = cfg.WorldSizeZ;
    t.TiledTerrainHandle = tiled.Index; // non-zero -> FindActiveTerrain treats it as live
    t.TiledTerrainGeneration = tiled.Generation;
    world.AddComponentImmediate(e, xf);
    world.AddComponentImmediate(e, nm);
    world.AddComponentImmediate(e, t);

    Components::Terrain resolved{};
    ASSERT_TRUE(CBTTerrainECS::FindActiveTerrain(world, resolved))
        << "a freshly-provisioned terrain must resolve";
    EXPECT_EQ(resolved.TiledTerrainHandle, tiled.Index);

    DeleteEntitiesCommand cmd("Delete Terrain", &world, &notifications, {e});

    cmd.Redo(); // delete (preserve-handle)
    Components::Terrain afterDelete{};
    EXPECT_FALSE(CBTTerrainECS::FindActiveTerrain(world, afterDelete))
        << "a deleted terrain must not resolve";
    EXPECT_EQ(svc.GetTiledTerrainData(tiled), nullptr)
        << "the delete must release the runtime terrain data — that is the scene-close leak";

    // FindActiveTerrain tests the handle PAIR for non-zero; it does not ask the
    // service to resolve it. So "bound" below means the entity still names a
    // terrain — which is exactly why the next assertion can show that terrain is
    // gone without contradicting it.
    cmd.Undo(); // revive + restore component bytes
    Components::Terrain afterUndo{};
    ASSERT_TRUE(CBTTerrainECS::FindActiveTerrain(world, afterUndo))
        << "undo must leave the entity naming a terrain — else the brush stays unbound";
    EXPECT_EQ(afterUndo.TiledTerrainHandle, tiled.Index)
        << "the revived bytes carry the pre-delete handle";
    EXPECT_EQ(svc.GetTiledTerrainData(tiled), nullptr)
        << "that handle is stale: the slot it named is gone";

    // Provisioning's stale-handle rule is what stops the revived entity being
    // permanently terrainless. Applied to the entity's LIVE component, because
    // the state the next extraction tick finds is the whole point — a detached
    // copy would only restate what the helper does to its argument.
    auto* liveTerrain = world.GetComponentForWrite<Components::Terrain>(e);
    ASSERT_NE(liveTerrain, nullptr);
    TerrainECS::ClearUnresolvedTerrainHandles(*liveTerrain);
    EXPECT_EQ(liveTerrain->TiledTerrainHandle, 0u);
    EXPECT_EQ(liveTerrain->TiledTerrainGeneration, 0u);

    // Zeroed is provisioning's "not provisioned yet" state: the entity stops
    // reading as bound, which is what lets extraction build fresh rather than
    // veto on a non-zero handle.
    Components::Terrain afterClear{};
    EXPECT_FALSE(CBTTerrainECS::FindActiveTerrain(world, afterClear))
        << "a cleared terrain still read as bound — provisioning would veto and the entity would stay terrainless";

    // And the re-provision re-binds the entity, which is the half the brush needs.
    // TerrainExtractionSystem::Update owns this in production but is gated on a
    // graphics device, so the service call and the write-back are exercised
    // directly here; the system tick itself is NOT covered by this test.
    const auto reprovisioned = svc.CreateTiledTerrain(cfg);
    liveTerrain = world.GetComponentForWrite<Components::Terrain>(e);
    ASSERT_NE(liveTerrain, nullptr);
    liveTerrain->TiledTerrainHandle = reprovisioned.Index;
    liveTerrain->TiledTerrainGeneration = reprovisioned.Generation;

    Components::Terrain afterReprovision{};
    ASSERT_TRUE(CBTTerrainECS::FindActiveTerrain(world, afterReprovision))
        << "the re-provisioned terrain must bind again";
    EXPECT_EQ(afterReprovision.TiledTerrainHandle, reprovisioned.Index);
    EXPECT_NE(svc.GetTiledTerrainData(reprovisioned), nullptr)
        << "the re-provisioned handle must resolve to live data";

    cmd.Redo(); // delete again
    EXPECT_FALSE(CBTTerrainECS::FindActiveTerrain(world, resolved));

    cmd.Undo(); // revive again
    Components::Terrain afterSecondUndo{};
    ASSERT_TRUE(CBTTerrainECS::FindActiveTerrain(world, afterSecondUndo))
        << "the resolver must stay stable across repeated undo/redo";
    EXPECT_EQ(afterSecondUndo.TiledTerrainHandle, tiled.Index);
}
} // namespace
