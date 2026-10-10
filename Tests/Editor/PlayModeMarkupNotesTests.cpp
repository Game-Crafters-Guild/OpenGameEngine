// A play session leaves the mark-ups' notes as they were: the exit restore brings the
// same entities back, and their descriptions and threads come back with them before the
// editor hears of the structure change, so the bridge never adopts them as new mark-ups.

#include <gtest/gtest.h>

#include "Core/Application.h"
#include "Core/Engine.h"

#include "Components/Markup/Markup.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "EditorChangeNotifications.h"
#include "MarkupECS/MarkupService.h"
#include "Markups/MarkupEditorBridge.h"
#include "PlayMode/PlayModeManager.h"
#include "UndoRedo/UndoRedoService.h"

#include <filesystem>
#include <optional>

using GameEngine::ApplicationConfig;
using GameEngine::EngineCore;
using GameEngine::Editor::EditorChangeNotifications;
using GameEngine::Editor::MarkupEditorBridge;
using GameEngine::Components::Markup;
using GameEngine::Components::MarkupAuthor;
using GameEngine::Editor::PlayModeManager;
using GameEngine::Editor::PlayModeState;
using GameEngine::MarkupECS::MarkupEntryKind;
using GameEngine::MarkupECS::MarkupNotes;
using GameEngine::MarkupECS::MarkupService;

namespace
{

class PlayModeMarkupNotesTests : public ::testing::Test
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

    void SetUp() override { MarkupService::Initialize(); }
    void TearDown() override { MarkupService::Shutdown(); }
};

} // namespace

TEST_F(PlayModeMarkupNotesTests, APlaySessionKeepsEveryMarkupsDescriptionAndThread)
{
    GameEngine::Editor::UndoRedoService undo;
    EditorChangeNotifications notifications;
    MarkupEditorBridge bridge(notifications, []() -> GameEngine::int64 { return 5000; },
                              []() { return std::optional<std::filesystem::path>{}; });
    PlayModeManager playMode;
    playMode.SetUndoRedo(&undo);
    playMode.SetChangeNotifications(&notifications);
    GameEngine::ECS::World* world = EngineCore::GetInstance().EnsurePrimaryWorld();
    ASSERT_NE(world, nullptr);
    world->Clear();
    const GameEngine::ECS::EntityHandle village = world->CreateEntity();
    world->AddComponentImmediate(village, GameEngine::Components::Transform{});
    world->AddComponentImmediate(village, Markup{});
    world->ProcessCommands();
    MarkupService& service = MarkupService::Get();
    ASSERT_TRUE(service.BeginMarkup(*world, village, MarkupAuthor::User, 100));
    ASSERT_TRUE(service.SetDescription(*world, village, MarkupAuthor::User, 101, "keep me"));
    ASSERT_TRUE(service.AddComment(*world, village, MarkupAuthor::User, 102, "walls first"));
    const GameEngine::uint32 revision = service.GetRevision(*world);
    const Markup stamps = *world->GetComponent<Markup>(village);
    playMode.SetWorld(world);

    playMode.EnterPlayMode();
    ASSERT_EQ(playMode.GetState(), PlayModeState::Play);
    {
        // The debug server's set_play_mode exits inside an agent request.
        MarkupEditorBridge::AttributionScope agent(bridge, MarkupAuthor::Agent);
        playMode.ExitPlayMode();
    }
    ASSERT_EQ(playMode.GetState(), PlayModeState::Edit);

    ASSERT_TRUE(world->IsValid(village));
    const MarkupNotes* notes = service.FindNotes(*world, village);
    ASSERT_NE(notes, nullptr) << "the exit restore dropped the mark-up's notes";
    EXPECT_EQ(notes->Description, "keep me");
    ASSERT_EQ(notes->Entries.size(), 3u);
    EXPECT_EQ(notes->Entries[0].Kind, MarkupEntryKind::Created);
    EXPECT_EQ(notes->Entries[0].Author, MarkupAuthor::User);
    EXPECT_EQ(notes->Entries[1].Kind, MarkupEntryKind::Edit); // the description edit
    EXPECT_EQ(notes->Entries[2].Kind, MarkupEntryKind::Comment);
    EXPECT_EQ(notes->Entries[2].Author, MarkupAuthor::User);
    EXPECT_EQ(notes->Entries[2].Text, "walls first");
    EXPECT_EQ(service.GetRevision(*world), revision);
    EXPECT_EQ(service.GetMarkups(*world).size(), 1u);
    const Markup* markup = world->GetComponent<Markup>(village);
    ASSERT_NE(markup, nullptr);
    EXPECT_EQ(markup->Author, stamps.Author) << "the bridge re-adopted the mark-up before its notes came back";
    EXPECT_EQ(markup->CreatedUnix, stamps.CreatedUnix);
    EXPECT_EQ(markup->UpdatedBy, stamps.UpdatedBy);
    EXPECT_EQ(markup->Revision, stamps.Revision);
}
