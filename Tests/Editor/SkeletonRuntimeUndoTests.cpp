// DeleteEntitiesCommand restores component BYTES. For a component holding a
// handle into an external store that is not a complete restore: the destroy
// runs the store's release hook, and the freed id can be reissued to another
// spawn before Undo replays the snapshot. The revived entity would then share
// a stranger's skeleton runtime — and the one repair site that exists
// (ModelRenderSetup's runtimeId == 0 / skeletonId mismatch test) cannot see a
// stale-but-non-zero id whose skeleton still matches.
//
// These pin the command's ownership of the references its snapshots name while
// the entities are deleted.

#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "UndoRedo/DeleteEntitiesCommand.h"

#include "Components/Animation/SkeletonRef.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "ECSModules/Rendering/SkeletonStore.h"

using GameEngine::Components::SkeletonRef;
using GameEngine::ECS::EntityHandle;
using GameEngine::ECS::World;
using GameEngine::Editor::DeleteEntitiesCommand;
using GameEngine::Engine::Renderer::SkeletonStore;

namespace
{

class SkeletonRuntimeUndoTests : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        auto& store = SkeletonStore::Instance();
        m_SkelId = store.CreateSkeleton(3);
        auto* skel = store.Get(m_SkelId);
        ASSERT_NE(skel, nullptr);
        skel->SkinJointCount = 3;
        skel->JointNodes = {0, 1, 2};

        m_World.RegisterOnRemove<SkeletonRef>([](SkeletonRef& ref) {
            if (ref.runtimeId != 0)
                SkeletonStore::Instance().ReleaseRuntime(ref.runtimeId);
        });
    }

    // ModelEntityFactory's spawn shape: one runtime, N entities naming it.
    std::vector<EntityHandle> SpawnInstance(uint32_t entityCount, uint32_t& outRuntimeId)
    {
        auto& store = SkeletonStore::Instance();
        outRuntimeId = store.CreateRuntime(m_SkelId);

        std::vector<EntityHandle> entities;
        for (uint32_t i = 0; i < entityCount; ++i)
        {
            SkeletonRef ref{};
            ref.skeletonId = m_SkelId;
            ref.runtimeId = outRuntimeId;
            entities.push_back(m_World.Create<SkeletonRef>(ref).GetHandle());
            store.RetainRuntime(outRuntimeId);
        }
        store.ReleaseRuntime(outRuntimeId); // construction reference
        return entities;
    }

    World m_World;
    uint32_t m_SkelId = 0;
};

} // namespace

// The core defect: while the entities are deleted, an unrelated spawn must not
// be able to claim the runtime the pending undo will restore.
TEST_F(SkeletonRuntimeUndoTests, DeletedRuntimeIsNotReissuedWhileUndoIsPending)
{
    auto& store = SkeletonStore::Instance();

    uint32_t runtimeId = 0;
    auto entities = SpawnInstance(2, runtimeId);
    ASSERT_NE(runtimeId, 0u);

    auto* pose = store.GetRuntime(runtimeId);
    ASSERT_NE(pose, nullptr);
    pose->CompactSkinMatrices[13] = 4.25f; // a pose the undo should bring back

    DeleteEntitiesCommand cmd("Delete", &m_World, nullptr, entities);
    cmd.Do();

    ASSERT_FALSE(m_World.IsValid(entities[0]));
    EXPECT_NE(store.GetRuntime(runtimeId), nullptr)
        << "the runtime a pending undo still references was freed";

    // Another model spawns while the delete is undoable.
    uint32_t intruderRuntime = 0;
    auto intruder = SpawnInstance(1, intruderRuntime);
    EXPECT_NE(intruderRuntime, runtimeId)
        << "a new spawn was handed the runtime the pending undo names";

    cmd.Undo();

    ASSERT_TRUE(m_World.IsValid(entities[0]));
    for (auto e : entities)
    {
        const auto* ref = m_World.GetComponent<SkeletonRef>(e);
        ASSERT_NE(ref, nullptr);
        EXPECT_EQ(ref->runtimeId, runtimeId);
        EXPECT_EQ(ref->skeletonId, m_SkelId);
    }

    auto* restored = store.GetRuntime(runtimeId);
    ASSERT_NE(restored, nullptr) << "the revived entities point at a freed runtime";
    EXPECT_FLOAT_EQ(restored->CompactSkinMatrices[13], 4.25f)
        << "undo restored the entity but not the pose it was deleted with";

    // Undo handed its references back to the restored components: exactly the
    // two the two entities hold, no more.
    EXPECT_EQ(store.GetRuntimeRefCount(runtimeId), 2u);

    for (auto e : entities)
        m_World.DestroyEntityImmediate(e);
    EXPECT_EQ(store.GetRuntime(runtimeId), nullptr)
        << "undo left an extra reference behind — the runtime now leaks";

    for (auto e : intruder)
        m_World.DestroyEntityImmediate(e);
}

// A command discarded while its entities are still deleted (undo stack trimmed,
// redo branch dropped) must not pin the runtime forever.
TEST_F(SkeletonRuntimeUndoTests, DiscardingTheCommandFreesTheRuntime)
{
    auto& store = SkeletonStore::Instance();

    uint32_t runtimeId = 0;
    auto entities = SpawnInstance(2, runtimeId);
    ASSERT_NE(runtimeId, 0u);

    {
        auto cmd = std::make_unique<DeleteEntitiesCommand>("Delete", &m_World, nullptr, entities);
        cmd->Do();
        EXPECT_NE(store.GetRuntime(runtimeId), nullptr);
    }

    EXPECT_EQ(store.GetRuntime(runtimeId), nullptr)
        << "the command was discarded without undoing, so nothing will ever free this runtime";
}

// Repeated undo/redo must net to zero on every cycle rather than accumulating
// or dropping references.
TEST_F(SkeletonRuntimeUndoTests, RedoUndoCyclesKeepTheReferenceCountExact)
{
    auto& store = SkeletonStore::Instance();

    uint32_t runtimeId = 0;
    auto entities = SpawnInstance(3, runtimeId);
    ASSERT_NE(runtimeId, 0u);
    ASSERT_EQ(store.GetRuntimeRefCount(runtimeId), 3u);

    DeleteEntitiesCommand cmd("Delete", &m_World, nullptr, entities);

    for (int cycle = 0; cycle < 3; ++cycle)
    {
        cmd.Redo();
        EXPECT_EQ(store.GetRuntimeRefCount(runtimeId), 3u)
            << "cycle " << cycle << ": the command should hold exactly what the components held";

        cmd.Undo();
        ASSERT_NE(store.GetRuntime(runtimeId), nullptr) << "cycle " << cycle;
        EXPECT_EQ(store.GetRuntimeRefCount(runtimeId), 3u) << "cycle " << cycle;
    }

    for (auto e : entities)
        m_World.DestroyEntityImmediate(e);
    EXPECT_EQ(store.GetRuntime(runtimeId), nullptr);
}
