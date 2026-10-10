#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"

#include <gtest/gtest.h>

namespace
{
using namespace GameEngine::ECS;
struct SnapshotValue { int Value = 0; };
struct SnapshotReference { EntityHandle Target; };

TEST(WorldSnapshotIdentity, PreservesSparseHandlesAndReusedGenerations)
{
    World world(nullptr);
    const auto old = world.CreateHandle(SnapshotValue{1});
    const auto hole = world.CreateHandle(SnapshotValue{2});
    const auto sibling = world.CreateHandle(SnapshotValue{3});
    world.DestroyEntityImmediate(old);
    const auto reused = world.CreateHandle(SnapshotValue{4});
    ASSERT_EQ(reused.index, old.index);
    ASSERT_NE(reused.version, old.version);
    world.DestroyEntityImmediate(hole);
    const auto snapshot = world.SerializeWorld();
    const auto generation = world.GetLifecycleResetGeneration();

    world.DestroyEntityImmediate(reused);
    world.DeserializeWorld(snapshot);

    ASSERT_TRUE(world.IsValid(reused));
    ASSERT_TRUE(world.IsValid(sibling));
    EXPECT_FALSE(world.IsValid(old));
    EXPECT_FALSE(world.IsValid(hole));
    EXPECT_EQ(world.GetComponent<SnapshotValue>(reused)->Value, 4);
    EXPECT_EQ(world.GetComponent<SnapshotValue>(sibling)->Value, 3);
    EXPECT_NE(world.GetLifecycleResetGeneration(), generation);
}

TEST(WorldSnapshotIdentity, PreservesEntityReferencesAndComponentlessEntities)
{
    World world(nullptr);
    const auto hole = world.CreateEntity();
    const auto empty = world.CreateEntity();
    const auto parent = world.CreateHandle(SnapshotValue{7});
    const auto child = world.CreateHandle(SnapshotReference{parent});
    world.DestroyEntityImmediate(hole);
    const auto snapshot = world.SerializeWorld();

    world.DeserializeWorld(snapshot);

    ASSERT_TRUE(world.IsValid(empty));
    ASSERT_TRUE(world.IsValid(parent));
    ASSERT_TRUE(world.IsValid(child));
    ASSERT_NE(world.GetComponent<SnapshotReference>(child), nullptr);
    EXPECT_EQ(world.GetComponent<SnapshotReference>(child)->Target, parent);
    EXPECT_EQ(world.GetComponent<SnapshotValue>(parent)->Value, 7);
}

TEST(WorldSnapshotIdentity, PreservesReservedUndoHandlesAndFreeIndexOrder)
{
    World world(nullptr);
    const auto firstFree = world.CreateEntity();
    const auto reserved = world.CreateHandle(SnapshotValue{9});
    const auto lastFree = world.CreateEntity();
    world.DestroyEntityImmediate(firstFree);
    world.DestroyEntityImmediatePreserveHandle(reserved);
    world.DestroyEntityImmediate(lastFree);
    const auto snapshot = world.SerializeWorld();
    const auto expectedFirst = world.CreateEntity();
    const auto expectedSecond = world.CreateEntity();

    world.DeserializeWorld(snapshot);

    EXPECT_TRUE(world.ReviveEntityImmediatePreserveHandle(reserved));
    EXPECT_EQ(world.CreateEntity(), expectedFirst);
    EXPECT_EQ(world.CreateEntity(), expectedSecond);
    EXPECT_TRUE(world.IsValid(reserved));
}

TEST(WorldSnapshotIdentity, DeletionReplayAndUndoSurviveAnotherSnapshotCycle)
{
    World world(nullptr);
    const auto gap = world.CreateEntity();
    const auto target = world.CreateHandle(SnapshotValue{11});
    const auto sibling = world.CreateHandle(SnapshotValue{12});
    std::vector<std::uint8_t> targetBytes;
    ASSERT_TRUE(world.CaptureComponentBytes(target, GetComponentTypeId<SnapshotValue>(), targetBytes));
    world.DestroyEntityImmediate(gap);
    const auto beforePlay = world.SerializeWorld();
    world.DestroyEntityImmediatePreserveHandle(target);
    world.DeserializeWorld(beforePlay);
    ASSERT_TRUE(world.IsValid(target));

    // The existing delete command replays the exact handle captured during Play.
    world.DestroyEntityImmediatePreserveHandle(target);
    ASSERT_FALSE(world.IsValid(target));
    ASSERT_TRUE(world.IsValid(sibling));
    world.DeserializeWorld(world.SerializeWorld());
    EXPECT_FALSE(world.IsValid(target));
    EXPECT_TRUE(world.IsValid(sibling));
    EXPECT_TRUE(world.ReviveEntityImmediatePreserveHandle(target));
    EXPECT_TRUE(world.ApplyComponentBytesImmediate(target, GetComponentTypeId<SnapshotValue>(),
                                                   targetBytes));
    EXPECT_EQ(world.GetComponent<SnapshotValue>(target)->Value, 11);
    EXPECT_EQ(world.GetComponent<SnapshotValue>(sibling)->Value, 12);
}

TEST(WorldSnapshotIdentity, RejectsTruncatedAndInvalidEnvelopesWithoutClearingTheWorld)
{
    World world(nullptr);
    const auto firstFree = world.CreateEntity();
    const auto secondFree = world.CreateEntity();
    const auto live = world.CreateHandle(SnapshotValue{19});
    world.DestroyEntityImmediate(firstFree);
    world.DestroyEntityImmediate(secondFree);
    const auto snapshot = world.SerializeWorld();
    const auto generation = world.GetLifecycleResetGeneration();
    const auto expectUnchanged = [&] {
        ASSERT_TRUE(world.IsValid(live));
        EXPECT_EQ(world.GetComponent<SnapshotValue>(live)->Value, 19);
        EXPECT_EQ(world.GetLifecycleResetGeneration(), generation);
    };
    for (std::size_t length = 0; length < snapshot.size(); ++length)
    {
        SCOPED_TRACE(length);
        world.DeserializeWorld({snapshot.begin(), snapshot.begin() + length});
        expectUnchanged();
    }
    const auto corrupt = [&](std::size_t offset, auto value) {
        auto invalid = snapshot;
        std::memcpy(invalid.data() + offset, &value, sizeof(value));
        world.DeserializeWorld(invalid);
        expectUnchanged();
    };
    corrupt(4, uint32_t{99}); // Unsupported format version.
    corrupt(8, uint32_t{kEntityIndexMask + 2}); // Unrepresentable slot count.
    corrupt(16, EntityVersion{kEntityVersionMask + 1}); // Invalid generation bits.
    corrupt(18, uint32_t{0xFFFFFFFF}); // Invalid entity record length.
    corrupt(snapshot.size() - 4, EntityIndex{firstFree.index}); // Duplicate free index.
    corrupt(snapshot.size() - 4, EntityIndex{live.index}); // Live/free overlap.
    corrupt(snapshot.size() - 4, EntityIndex{kEntityIndexMask}); // Free index out of range.
    auto trailing = snapshot;
    trailing.push_back(0);
    world.DeserializeWorld(trailing);
    expectUnchanged();
}
} // namespace
