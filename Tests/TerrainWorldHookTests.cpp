// Scene close has an owner: the Components::Terrain OnRemove hook is the single
// body that releases a terrain's TerrainService slot and GPU texture set, and it
// has to fire from every teardown shape — World::Clear (scene replace-open),
// World::DestroyEntity (hierarchy delete), and a component remove.
//
// Slot INDEX REUSE, not the active count, is the anti-ratchet property under test:
// a fix that frees the data but never returns the index to the free list still
// grows TerrainService::m_Slots and the render feature's per-index texture vectors
// once per scene open. Every release test therefore asserts the next CreateTerrain
// comes back on the recycled index.
//
// These run without a device: EngineCore has no RenderServices here, so the hook
// resolves a null render feature and exercises the service half. That is also the
// shutdown-safety shape (§6.2 test 7).

#include <gtest/gtest.h>

#include "Components/Terrain/Terrain.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "Scene/SceneIOContext.h"
#include "Scene/SceneSchemaRegistry.h"
#include "TerrainECS/Scene/TerrainSceneSchemas.h"
#include "TerrainECS/Systems/TerrainWorldHooks.h"
#include "TerrainECS/TerrainHandleHygiene.h"
#include "TerrainECS/TerrainService.h"
#include "Terrain/TerrainTypes.h"

#include <string>

namespace
{
using GameEngine::Components::Terrain;
using GameEngine::ECS::EntityHandle;
using GameEngine::ECS::World;
using GameEngine::TerrainECS::TerrainHandle;
using GameEngine::TerrainECS::TerrainService;
using GameEngine::TerrainECS::TiledTerrainHandle;

struct ScopedTerrainService
{
    ScopedTerrainService()
    {
        if (TerrainService::IsInitialized())
            TerrainService::Shutdown();
        TerrainService::Initialize();
    }
    ~ScopedTerrainService()
    {
        if (TerrainService::IsInitialized())
            TerrainService::Shutdown();
    }
};

GameEngine::Terrain::TerrainConfig SingleConfig()
{
    GameEngine::Terrain::TerrainConfig cfg{};
    cfg.HeightmapWidth = 129;
    cfg.HeightmapHeight = 129;
    cfg.WorldSizeX = 256.0f;
    cfg.WorldSizeZ = 256.0f;
    cfg.HeightScale = 64.0f;
    cfg.LODLevels = 4;
    return cfg;
}

GameEngine::TerrainECS::TiledTerrainConfig TiledConfig()
{
    GameEngine::TerrainECS::TiledTerrainConfig cfg{};
    cfg.WorldSizeX = 4096.0f;
    cfg.WorldSizeZ = 4096.0f;
    cfg.HeightScale = 256.0f;
    cfg.SamplesPerMeter = 2.0f;
    return cfg;
}

// A terrain entity as a loaded scene produces it: the component carries the
// runtime handle pair the extraction system provisioned.
EntityHandle CreateTerrainEntity(World& world, TerrainHandle handle)
{
    const auto entity = world.CreateEntity();
    Terrain t{};
    t.SizeX = 256.0f;
    t.SizeZ = 256.0f;
    t.HeightScale = 64.0f;
    t.TerrainDataHandle = handle.Index;
    t.TerrainDataGeneration = handle.Generation;
    world.AddComponentImmediate(entity, t);
    return entity;
}

EntityHandle CreateTiledTerrainEntity(World& world, TiledTerrainHandle handle)
{
    const auto entity = world.CreateEntity();
    Terrain t{};
    t.SizeX = 4096.0f;
    t.SizeZ = 4096.0f;
    t.SamplesPerMeter = 2.0f;
    t.TiledTerrainHandle = handle.Index;
    t.TiledTerrainGeneration = handle.Generation;
    world.AddComponentImmediate(entity, t);
    return entity;
}

} // namespace

// §6.2 test 1. The scene-replace path. Index reuse is the assertion that
// distinguishes a real release from a data-only free.
TEST(TerrainWorldHooks, WorldClearReleasesTerrainAndRecyclesSlot)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();

    World world;
    GameEngine::TerrainECS::RegisterTerrainWorldHooks(world);
    const auto handle = svc.CreateTerrain(SingleConfig());
    CreateTerrainEntity(world, handle);
    ASSERT_EQ(svc.GetActiveTerrainCount(), 1u);

    world.Clear();

    EXPECT_EQ(svc.GetActiveTerrainCount(), 0u) << "scene close must release the terrain slot";
    EXPECT_EQ(svc.GetTerrainData(handle), nullptr);

    const auto reopened = svc.CreateTerrain(SingleConfig());
    EXPECT_EQ(reopened.Index, handle.Index)
        << "the next open must land on the recycled slot — a fresh index every open is the ratchet";
    EXPECT_NE(reopened.Generation, handle.Generation);
}

// §6.2 test 1, repeated: eight opens of the same scene must not grow the slot table.
TEST(TerrainWorldHooks, RepeatedClearAndCreateDoesNotRatchetSlots)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();

    GameEngine::uint32 firstIndex = 0;
    for (int open = 0; open < 8; ++open)
    {
        World world;
        GameEngine::TerrainECS::RegisterTerrainWorldHooks(world);
        const auto handle = svc.CreateTerrain(SingleConfig());
        if (open == 0)
            firstIndex = handle.Index;
        EXPECT_EQ(handle.Index, firstIndex) << "open " << open << " took a fresh slot index";
        CreateTerrainEntity(world, handle);
        world.Clear();
        EXPECT_EQ(svc.GetActiveTerrainCount(), 0u) << "open " << open << " leaked its terrain";
    }
}

// §6.2 test 2. The hierarchy-delete leak nobody reported.
TEST(TerrainWorldHooks, DestroyEntityReleasesTerrain)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();

    World world;
    GameEngine::TerrainECS::RegisterTerrainWorldHooks(world);
    const auto handle = svc.CreateTerrain(SingleConfig());
    const auto entity = CreateTerrainEntity(world, handle);

    world.DestroyEntityImmediate(entity);

    EXPECT_EQ(svc.GetActiveTerrainCount(), 0u);
    EXPECT_EQ(svc.GetTerrainData(handle), nullptr);
    EXPECT_EQ(svc.CreateTerrain(SingleConfig()).Index, handle.Index);
}

// §6.2 test 2, component-remove shape.
TEST(TerrainWorldHooks, RemoveComponentReleasesTerrain)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();

    World world;
    GameEngine::TerrainECS::RegisterTerrainWorldHooks(world);
    const auto handle = svc.CreateTerrain(SingleConfig());
    const auto entity = CreateTerrainEntity(world, handle);

    world.RemoveComponentImmediate<Terrain>(entity);

    EXPECT_EQ(svc.GetActiveTerrainCount(), 0u);
    EXPECT_EQ(svc.CreateTerrain(SingleConfig()).Index, handle.Index);
}

// §6.2 test 4. Extraction ZEROES TerrainDataHandle on a tiled terrain and carries
// only the tiled pair, so a fix that checks the single handle alone passes test 1
// and still strands the tiled store plus its unified GPU set.
TEST(TerrainWorldHooks, WorldClearReleasesTiledTerrain)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();

    World world;
    GameEngine::TerrainECS::RegisterTerrainWorldHooks(world);
    const auto tiled = svc.CreateTiledTerrain(TiledConfig());
    ASSERT_NE(svc.GetTiledTerrainData(tiled), nullptr);
    CreateTiledTerrainEntity(world, tiled);

    world.Clear();

    EXPECT_EQ(svc.GetTiledTerrainData(tiled), nullptr) << "scene close must release the tiled store";
    EXPECT_EQ(svc.CreateTiledTerrain(TiledConfig()).Index, tiled.Index)
        << "the tiled slot index must be recycled too";
}

// §6.2 test 5. Two entities duplicated from one another carry the same handle
// pair; the first release frees it and the second must resolve nothing rather
// than destroying whatever now occupies the recycled slot.
TEST(TerrainWorldHooks, DoubleReleaseIsANoOp)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();

    World world;
    GameEngine::TerrainECS::RegisterTerrainWorldHooks(world);
    const auto handle = svc.CreateTerrain(SingleConfig());
    CreateTerrainEntity(world, handle);
    CreateTerrainEntity(world, handle); // duplicate: same handle pair

    world.Clear();
    EXPECT_EQ(svc.GetActiveTerrainCount(), 0u);

    // The recycled slot's new occupant must survive a stale-handle release.
    const auto reused = svc.CreateTerrain(SingleConfig());
    ASSERT_EQ(reused.Index, handle.Index);
    Terrain stale{};
    stale.TerrainDataHandle = handle.Index;
    stale.TerrainDataGeneration = handle.Generation;
    GameEngine::TerrainECS::ReleaseTerrainOwnedResources(stale);
    EXPECT_NE(svc.GetTerrainData(reused), nullptr)
        << "a stale-generation release must not destroy the slot's current occupant";
    EXPECT_EQ(stale.TerrainDataHandle, 0u);
}

// §6.2 test 7. The hook also fires from ~World at process shutdown, when
// TerrainService may already be gone.
TEST(TerrainWorldHooks, ReleaseWithServiceTornDownDoesNotCrash)
{
    if (TerrainService::IsInitialized())
        TerrainService::Shutdown();

    Terrain t{};
    t.TerrainDataHandle = 3;
    t.TerrainDataGeneration = 1;
    t.TiledTerrainHandle = 5;
    t.TiledTerrainGeneration = 2;
    GameEngine::TerrainECS::ReleaseTerrainOwnedResources(t);

    // Nothing to release and nothing to dereference: the handles stay as they were.
    EXPECT_EQ(t.TerrainDataHandle, 3u);
}

// §6.2 test 8, schema parity. TerrainSchema::Remove is reached by the blueprint
// `removedComponents` override directive (SceneIO), the only production caller of
// ISceneComponentSchema::Remove — not by the editor inspector, which calls
// World::RemoveComponentImmediate directly. Collapsing the schema's inline release
// body onto the hook must not drop either half.
TEST(TerrainWorldHooks, SchemaRemoveReleasesThroughTheHook)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();

    GameEngine::Scene::EnsureTerrainSceneSchemasRegistered();
    const auto* schema = GameEngine::Scene::SceneSchemaRegistry::Find("Terrain");
    ASSERT_NE(schema, nullptr);

    World world;
    GameEngine::TerrainECS::RegisterTerrainWorldHooks(world);
    const auto handle = svc.CreateTerrain(SingleConfig());
    const auto tiled = svc.CreateTiledTerrain(TiledConfig());
    const auto entity = world.CreateEntity();
    Terrain t{};
    t.TerrainDataHandle = handle.Index;
    t.TerrainDataGeneration = handle.Generation;
    t.TiledTerrainHandle = tiled.Index;
    t.TiledTerrainGeneration = tiled.Generation;
    world.AddComponentImmediate(entity, t);

    EXPECT_TRUE(schema->Remove(world, entity));

    EXPECT_EQ(svc.GetTerrainData(handle), nullptr) << "schema remove dropped the single half";
    EXPECT_EQ(svc.GetTiledTerrainData(tiled), nullptr) << "schema remove dropped the tiled half";
    EXPECT_EQ(world.GetComponent<Terrain>(entity), nullptr);
}

// §6.2 test 6, shape (a): undo of a terrain-entity delete. The delete command
// snapshots the component bytes BEFORE the destroy, so undo restores the handle
// pair the hook has since released. Provisioning must treat that as "not
// provisioned" and build fresh — the pre-fix guard refused on any non-zero handle
// and left the terrain permanently dead.
TEST(TerrainWorldHooks, RevivedComponentWithReleasedHandleProvisionsFresh)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();

    World world;
    GameEngine::TerrainECS::RegisterTerrainWorldHooks(world);
    const auto handle = svc.CreateTerrain(SingleConfig());
    const auto entity = CreateTerrainEntity(world, handle);

    // Snapshot as the delete command does, then delete.
    const Terrain snapshot = *world.GetComponent<Terrain>(entity);
    world.DestroyEntityImmediate(entity);
    ASSERT_EQ(svc.GetTerrainData(handle), nullptr);

    // Undo: the pre-delete bytes come back, carrying the released handle.
    // Slot index 0 is a legitimate handle, so Generation is the liveness bit the
    // provisioning guard reads — assert on that, not on the index.
    Terrain revived = snapshot;
    EXPECT_NE(revived.TerrainDataGeneration, 0u) << "the snapshot must carry the pre-release handle";

    GameEngine::TerrainECS::ClearUnresolvedTerrainHandles(revived);
    EXPECT_EQ(revived.TerrainDataHandle, 0u)
        << "a handle the service cannot resolve must not survive as a provisioning veto";
    EXPECT_EQ(revived.TerrainDataGeneration, 0u);

    // Provisioning now runs and hands back a live terrain on a NEW generation.
    const auto reprovisioned = svc.CreateTerrain(SingleConfig());
    EXPECT_NE(svc.GetTerrainData(reprovisioned), nullptr);
    EXPECT_NE(reprovisioned.Generation, handle.Generation);
}

// §6.2 test 6, shape (b): undo of an inspector component-remove, tiled half.
TEST(TerrainWorldHooks, RevivedComponentWithReleasedTiledHandleProvisionsFresh)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();

    World world;
    GameEngine::TerrainECS::RegisterTerrainWorldHooks(world);
    const auto tiled = svc.CreateTiledTerrain(TiledConfig());
    const auto entity = CreateTiledTerrainEntity(world, tiled);

    const Terrain snapshot = *world.GetComponent<Terrain>(entity);
    world.RemoveComponentImmediate<Terrain>(entity);
    ASSERT_EQ(svc.GetTiledTerrainData(tiled), nullptr);

    Terrain revived = snapshot;
    ASSERT_NE(revived.TiledTerrainGeneration, 0u);
    GameEngine::TerrainECS::ClearUnresolvedTerrainHandles(revived);
    EXPECT_EQ(revived.TiledTerrainHandle, 0u);
    EXPECT_EQ(revived.TiledTerrainGeneration, 0u);
}

// A handle that DOES resolve is an in-place edit of a live terrain and must be
// left alone — the same-slot debounced re-provision depends on it.
TEST(TerrainWorldHooks, ResolvableHandlesAreLeftAlone)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();

    const auto handle = svc.CreateTerrain(SingleConfig());
    const auto tiled = svc.CreateTiledTerrain(TiledConfig());

    Terrain t{};
    t.TerrainDataHandle = handle.Index;
    t.TerrainDataGeneration = handle.Generation;
    t.TiledTerrainHandle = tiled.Index;
    t.TiledTerrainGeneration = tiled.Generation;

    GameEngine::TerrainECS::ClearUnresolvedTerrainHandles(t);

    EXPECT_EQ(t.TerrainDataHandle, handle.Index);
    EXPECT_EQ(t.TerrainDataGeneration, handle.Generation);
    EXPECT_EQ(t.TiledTerrainHandle, tiled.Index);
    EXPECT_EQ(t.TiledTerrainGeneration, tiled.Generation);
}

// §6.2 test 9. RegisterOnRemove refreshes the cached per-archetype hook flag over
// every existing archetype, so a terrain entity created BEFORE the registrar runs
// still releases. Pins that contract so registration order stays a non-issue.
TEST(TerrainWorldHooks, HookFiresForEntitiesCreatedBeforeRegistration)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();

    World world;
    const auto handle = svc.CreateTerrain(SingleConfig());
    CreateTerrainEntity(world, handle);

    GameEngine::TerrainECS::RegisterTerrainWorldHooks(world);
    world.Clear();

    EXPECT_EQ(svc.GetActiveTerrainCount(), 0u);
}
