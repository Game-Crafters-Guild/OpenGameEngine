// Regression tests for World::Clear() side-table state.
//
// World::Clear() resets archetypes/metadata/freeIndices but historically never
// touched the UnresolvedComponentStore (preserved scene components whose type
// was unknown at load time) or the singleton map. Because entity indices AND
// versions restart after Clear, a stale preserved-component entry keyed by an
// old EntityHandle would attach to whichever new entity recycles that handle,
// and reopening a scene accumulated duplicate preserved sections.

#include "ECS/Entity.h"
#include "ECS/UnresolvedComponentStore.h"
#include "ECS/World.h"

#include <gtest/gtest.h>

using namespace GameEngine::ECS;

namespace WorldClearStateTestTypes
{
struct ClearProbeSingleton
{
    int Value = 0;
};
} // namespace WorldClearStateTestTypes

TEST(WorldClearStateTest, ClearEmptiesUnresolvedComponentStore)
{
    World world;
    EntityHandle entity = world.CreateEntity();

    world.GetUnresolvedComponents().Add(
        entity, PreservedComponent{"jumpable", {{"height", "2.0"}}});
    world.GetUnresolvedComponents().NoteUnresolvedReference("guid-text", "Materials/Gone.material");
    ASSERT_FALSE(world.GetUnresolvedComponents().Empty());

    world.Clear();

    const UnresolvedComponentStore* store = world.TryGetUnresolvedComponents();
    ASSERT_NE(store, nullptr);
    EXPECT_TRUE(store->Empty());
    EXPECT_EQ(store->FindUnresolvedReference("guid-text"), nullptr) << "a scene replace starts with no reference paths";
}

TEST(WorldClearStateTest, RecycledHandleDoesNotInheritPreservedComponents)
{
    World world;
    EntityHandle before = world.CreateEntity();
    world.GetUnresolvedComponents().Add(before, PreservedComponent{"jumpable", {}});

    world.Clear();

    // Entity indices and versions restart after Clear, so the first entity
    // created afterwards recycles the exact same packed handle.
    EntityHandle recycled = world.CreateEntity();
    ASSERT_EQ(recycled.id, before.id)
        << "precondition: handle must actually be recycled for this test to bite";

    const UnresolvedComponentStore* store = world.TryGetUnresolvedComponents();
    ASSERT_NE(store, nullptr);
    EXPECT_EQ(store->Map().count(recycled), 0u)
        << "stale preserved component attached to a recycled EntityHandle";
}

TEST(WorldClearStateTest, ClearRemovesSingletons)
{
    using WorldClearStateTestTypes::ClearProbeSingleton;

    World world;
    world.SetSingleton(ClearProbeSingleton{42});
    ASSERT_TRUE(world.HasSingleton<ClearProbeSingleton>());

    world.Clear();

    EXPECT_FALSE(world.HasSingleton<ClearProbeSingleton>());
}

// A snapshot restore (leaving play mode, a snapshot undo or redo) brings back the same document with
// the same handles, so its unresolved scene data must come back with it: the next save writes it.
// An entity the snapshot does not hold loses its entries, since its handle may be reused.
TEST(WorldClearStateTest, SnapshotRestoreKeepsUnresolvedSceneData)
{
    World world;
    const EntityHandle kept = world.CreateEntity();
    world.GetUnresolvedComponents().Add(kept, PreservedComponent{"jumpable", {{"height", "2.0"}}});
    world.GetUnresolvedComponents().AddField(kept, PreservedField{"SplineFence", "SpanGrade", "Cantilevered"});
    world.GetUnresolvedComponents().NoteUnresolvedReference("guid-text", "Materials/Gone.material");
    const std::vector<uint8_t> snapshot = world.SerializeWorld();

    const EntityHandle later = world.CreateEntity();
    world.GetUnresolvedComponents().Add(later, PreservedComponent{"jumpable", {}});

    world.DeserializeWorld(snapshot);

    const UnresolvedComponentStore* store = world.TryGetUnresolvedComponents();
    ASSERT_NE(store, nullptr);
    EXPECT_EQ(store->Map().count(kept), 1u) << "preserved component lost by the restore";
    EXPECT_NE(store->FieldsFor(kept), nullptr) << "preserved field lost by the restore";
    const std::string* path = store->FindUnresolvedReference("guid-text");
    ASSERT_NE(path, nullptr) << "reference path lost by the restore";
    EXPECT_EQ(*path, "Materials/Gone.material");
    EXPECT_EQ(store->Map().count(later), 0u) << "an entity the snapshot does not hold kept its entries";
}
