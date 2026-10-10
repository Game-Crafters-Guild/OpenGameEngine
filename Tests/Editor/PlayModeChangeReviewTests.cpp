#include <gtest/gtest.h>

#include "Core/Application.h"
#include "Core/Engine.h"

#include "Components/Rendering/MeshGPUData.h"
#include "Components/Hierarchy.h"
#include "Components/Name.h"
#include "ECS/Entity.h" // World/Entity declarations

#include "PlayMode/PlayModeManager.h"
#include "UndoRedo/UndoRedoService.h"
#include "UndoRedo/IEditorCommand.h"
#include "UndoRedo/DeleteEntitiesCommand.h"

// World::GetComponent<T> / AddComponentImmediate<T> definitions live in the
// ECS template .inl files; Entity.h only declares them. The Engine.dll exports
// strip implicitly-instantiated template symbols, so pull in the template
// implementations here to instantiate them locally for MeshGPUData.
#include "ECS/ECSTemplates.h"

using GameEngine::ApplicationConfig;
using GameEngine::EngineCore;

using GameEngine::Editor::PlayModeManager;
using GameEngine::Editor::PlayModeState;
using GameEngine::Editor::UndoRedoService;

namespace
{

class SetIntCommand final : public GameEngine::Editor::IEditorCommand
{
  public:
    explicit SetIntCommand(int* target, int value, const char* name = "Set Int")
        : m_Target(target), m_Value(value), m_Name(name ? name : "Set Int")
    {
    }

    const char* GetName() const override { return m_Name.c_str(); }
    void Do() override
    {
        if (!m_Target)
            return;
        m_Before = *m_Target;
        *m_Target = m_Value;
    }
    void Undo() override
    {
        if (!m_Target)
            return;
        *m_Target = m_Before;
    }
    void Redo() override
    {
        if (!m_Target)
            return;
        *m_Target = m_Value;
    }

  private:
    int* m_Target = nullptr;
    int m_Before = 0;
    int m_Value = 0;
    std::string m_Name;
};

class PlayModeChangeReviewTests : public ::testing::Test
{
  protected:
    static void SetUpTestSuite()
    {
        EngineCore& engine = EngineCore::GetInstance();
        if (!engine.IsInitialized())
        {
            ApplicationConfig config{};
            config.AssetDirectory = ".";
            config.WorkspaceDirectory = ".";
            config.EnableEditor = true;
            ASSERT_TRUE(engine.Initialize(config));
        }
    }
};

TEST_F(PlayModeChangeReviewTests, ExitToChangeReview_WhenUndoCommandsWereCommittedDuringPlay)
{
    UndoRedoService undo;
    PlayModeManager pm;
    pm.SetUndoRedo(&undo);
    auto* world = EngineCore::GetInstance().EnsurePrimaryWorld();
    ASSERT_NE(world, nullptr);
    world->Clear();
    // Ensure snapshot capture is non-empty (PlayModeManager treats empty snapshot as failure).
    (void)world->CreateEntity();
    world->ProcessCommands();
    pm.SetWorld(world);

    ASSERT_EQ(pm.GetState(), PlayModeState::Edit);

    pm.EnterPlayMode();
    ASSERT_EQ(pm.GetState(), PlayModeState::Play);

    int v = 0;
    undo.Execute(std::make_unique<SetIntCommand>(&v, 123, "Set V"));
    ASSERT_EQ(v, 123);

    pm.ExitPlayMode();
    ASSERT_EQ(pm.GetState(), PlayModeState::ChangeReview);
    ASSERT_TRUE(pm.HasPendingChanges());

    auto names = pm.GetPendingChangeNames();
    ASSERT_EQ(names.size(), 1u);
    EXPECT_EQ(names[0], "Set V");
}

TEST_F(PlayModeChangeReviewTests, ApplyPendingChanges_ReplaysSelectedCommands_AndReturnsToEdit)
{
    UndoRedoService undo;
    PlayModeManager pm;
    pm.SetUndoRedo(&undo);
    auto* world = EngineCore::GetInstance().EnsurePrimaryWorld();
    ASSERT_NE(world, nullptr);
    world->Clear();
    (void)world->CreateEntity();
    world->ProcessCommands();
    pm.SetWorld(world);

    pm.EnterPlayMode();
    ASSERT_EQ(pm.GetState(), PlayModeState::Play);

    int v = 0;
    undo.Execute(std::make_unique<SetIntCommand>(&v, 123, "Set V"));
    ASSERT_EQ(v, 123);

    pm.ExitPlayMode();
    ASSERT_EQ(pm.GetState(), PlayModeState::ChangeReview);

    // Simulate snapshot restore resetting runtime state back to edit baseline.
    v = 0;

    pm.ApplyPendingChanges(std::vector<bool>{true});
    EXPECT_EQ(pm.GetState(), PlayModeState::Edit);
    EXPECT_FALSE(pm.HasPendingChanges());
    EXPECT_EQ(v, 123);
}

TEST_F(PlayModeChangeReviewTests, DiscardPendingChanges_DropsCommands_AndReturnsToEdit)
{
    UndoRedoService undo;
    PlayModeManager pm;
    pm.SetUndoRedo(&undo);
    auto* world = EngineCore::GetInstance().EnsurePrimaryWorld();
    ASSERT_NE(world, nullptr);
    world->Clear();
    (void)world->CreateEntity();
    world->ProcessCommands();
    pm.SetWorld(world);

    pm.EnterPlayMode();
    ASSERT_EQ(pm.GetState(), PlayModeState::Play);

    int v = 0;
    undo.Execute(std::make_unique<SetIntCommand>(&v, 123, "Set V"));
    ASSERT_EQ(v, 123);

    pm.ExitPlayMode();
    ASSERT_EQ(pm.GetState(), PlayModeState::ChangeReview);

    v = 0;
    pm.DiscardPendingChanges();
    EXPECT_EQ(pm.GetState(), PlayModeState::Edit);
    EXPECT_FALSE(pm.HasPendingChanges());
    EXPECT_EQ(v, 0);
}

TEST_F(PlayModeChangeReviewTests, ExitPlayModeResetsRestoredMeshGpuData)
{
    PlayModeManager pm;
    auto* world = EngineCore::GetInstance().EnsurePrimaryWorld();
    ASSERT_NE(world, nullptr);
    world->Clear();

    auto entity = world->CreateEntity();
    GameEngine::Components::MeshGPUData cached{};
    cached.meshIndex = 11u;
    cached.materialIndex = 22u;
    cached.instanceIndex = 33u;
    world->AddComponentImmediate(entity, cached);
    world->ProcessCommands();
    pm.SetWorld(world);

    pm.EnterPlayMode();
    ASSERT_EQ(pm.GetState(), PlayModeState::Play);

    pm.ExitPlayMode();
    ASSERT_EQ(pm.GetState(), PlayModeState::Edit);

    auto alive = world->GetAliveEntitiesSnapshot();
    ASSERT_EQ(alive.size(), 1u);
    auto* meshGpu = world->GetComponent<GameEngine::Components::MeshGPUData>(alive.front());
    ASSERT_NE(meshGpu, nullptr);
    EXPECT_EQ(meshGpu->meshIndex, 0xFFFFFFFFu);
    EXPECT_EQ(meshGpu->materialIndex, 0xFFFFFFFFu);
    EXPECT_EQ(meshGpu->instanceIndex, 0xFFFFFFFFu);
}

TEST_F(PlayModeChangeReviewTests, AppliedSubtreeDeletionPreservesIdentityAndUndoAcrossPlayCycles)
{
    using namespace GameEngine::ECS;
    using namespace GameEngine::Components;
    using GameEngine::Editor::DeleteEntitiesCommand;
    auto* world = EngineCore::GetInstance().EnsurePrimaryWorld();
    ASSERT_NE(world, nullptr);
    world->Clear();
    UndoRedoService undo;
    PlayModeManager pm;
    pm.SetWorld(world);
    pm.SetUndoRedo(&undo);

    const auto recycled = world->CreateEntity();
    const auto gap = world->CreateEntity();
    const auto sibling = world->CreateHandle(Name{"Sibling"});
    const auto parked = world->CreateHandle(Name{"Undo before Play"});
    world->DestroyEntityImmediate(recycled);
    const auto root = world->CreateHandle(Name{"Root"});
    ASSERT_EQ(root.index, recycled.index);
    ASSERT_NE(root.version, recycled.version);
    const auto child = world->CreateHandle(Name{"Child"}, Parent{root});
    world->DestroyEntityImmediate(gap);
    undo.Execute(std::make_unique<DeleteEntitiesCommand>("Delete before Play", world, nullptr,
        std::vector<EntityHandle>{parked}));

    pm.EnterPlayMode();
    ASSERT_EQ(pm.GetState(), PlayModeState::Play);
    undo.Execute(std::make_unique<DeleteEntitiesCommand>("Delete subtree", world, nullptr,
        DeleteEntitiesCommand::CollectSubtree(*world, root)));
    ASSERT_FALSE(world->IsValid(root));
    ASSERT_FALSE(world->IsValid(child));
    pm.ExitPlayMode();
    ASSERT_EQ(pm.GetState(), PlayModeState::ChangeReview);
    ASSERT_TRUE(world->IsValid(root));
    ASSERT_TRUE(world->IsValid(child));
    ASSERT_NE(world->GetComponent<Parent>(child), nullptr);
    EXPECT_EQ(world->GetComponent<Parent>(child)->parent, root);
    pm.ApplyPendingChanges({true});
    EXPECT_FALSE(world->IsValid(root));
    EXPECT_FALSE(world->IsValid(child));
    EXPECT_TRUE(world->IsValid(sibling));

    pm.EnterPlayMode();
    pm.ExitPlayMode();
    ASSERT_EQ(pm.GetState(), PlayModeState::Edit);
    EXPECT_FALSE(world->IsValid(root));
    EXPECT_FALSE(world->IsValid(child));
    EXPECT_TRUE(world->IsValid(sibling));
    undo.Undo();
    ASSERT_TRUE(world->IsValid(root));
    ASSERT_TRUE(world->IsValid(child));
    ASSERT_NE(world->GetComponent<Parent>(child), nullptr);
    EXPECT_EQ(world->GetComponent<Parent>(child)->parent, root);
    EXPECT_EQ(world->GetComponent<Name>(root)->View(), "Root");
    EXPECT_EQ(world->GetComponent<Name>(sibling)->View(), "Sibling");
    undo.Redo();
    EXPECT_FALSE(world->IsValid(root));
    EXPECT_FALSE(world->IsValid(child));
    undo.Undo();
    undo.Undo();
    ASSERT_TRUE(world->IsValid(parked));
    EXPECT_EQ(world->GetComponent<Name>(parked)->View(), "Undo before Play");
}

TEST_F(PlayModeChangeReviewTests, DiscardedSubtreeDeletionRestoresOriginalHandlesAndParent)
{
    using namespace GameEngine::ECS;
    using namespace GameEngine::Components;
    using GameEngine::Editor::DeleteEntitiesCommand;
    auto* world = EngineCore::GetInstance().EnsurePrimaryWorld();
    world->Clear();
    UndoRedoService undo;
    PlayModeManager pm;
    pm.SetWorld(world);
    pm.SetUndoRedo(&undo);
    const auto gap = world->CreateEntity();
    const auto parent = world->CreateHandle(Name{"Parent"});
    const auto child = world->CreateHandle(Name{"Child"}, Parent{parent});
    const auto sibling = world->CreateHandle(Name{"Sibling"});
    world->DestroyEntityImmediate(gap);
    pm.EnterPlayMode();
    undo.Execute(std::make_unique<DeleteEntitiesCommand>("Delete subtree", world, nullptr,
        DeleteEntitiesCommand::CollectSubtree(*world, parent)));
    pm.ExitPlayMode();
    ASSERT_EQ(pm.GetState(), PlayModeState::ChangeReview);
    pm.DiscardPendingChanges();
    ASSERT_TRUE(world->IsValid(parent));
    ASSERT_TRUE(world->IsValid(child));
    EXPECT_TRUE(world->IsValid(sibling));
    ASSERT_NE(world->GetComponent<Parent>(child), nullptr);
    EXPECT_EQ(world->GetComponent<Parent>(child)->parent, parent);
    EXPECT_FALSE(undo.CanUndo());
}

TEST_F(PlayModeChangeReviewTests, ApplySelectedDeletionLeavesUnselectedEntityUntouched)
{
    using namespace GameEngine::ECS;
    using namespace GameEngine::Components;
    using GameEngine::Editor::DeleteEntitiesCommand;
    auto* world = EngineCore::GetInstance().EnsurePrimaryWorld();
    world->Clear();
    UndoRedoService undo;
    PlayModeManager pm;
    pm.SetWorld(world);
    pm.SetUndoRedo(&undo);
    const auto gap = world->CreateEntity();
    const auto selected = world->CreateHandle(Name{"Selected"});
    const auto unselected = world->CreateHandle(Name{"Unselected"});
    world->DestroyEntityImmediate(gap);
    pm.EnterPlayMode();
    for (const auto entity : {selected, unselected})
        undo.Execute(std::make_unique<DeleteEntitiesCommand>("Delete", world, nullptr,
            std::vector<EntityHandle>{entity}));
    pm.ExitPlayMode();
    pm.ApplyPendingChanges({true, false});
    EXPECT_FALSE(world->IsValid(selected));
    ASSERT_TRUE(world->IsValid(unselected));
    EXPECT_EQ(world->GetComponent<Name>(unselected)->View(), "Unselected");
    undo.Undo();
    ASSERT_TRUE(world->IsValid(selected));
    EXPECT_EQ(world->GetComponent<Name>(selected)->View(), "Selected");
}

} // namespace
