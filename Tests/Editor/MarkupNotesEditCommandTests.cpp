// A notes edit made in the inspector (Markups/MarkupNotesEditCommand.h) is one undo step:
// undo restores the notes and the component, and the tags the edit added to the
// vocabulary; redo applies them again.

#include "Markups/MarkupNotesEditCommand.h"

#include "Components/Markup/Markup.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "EditorChangeNotifications.h"
#include "MarkupECS/MarkupService.h"
#include "UndoRedo/UndoRedoService.h"

#include <gtest/gtest.h>

using namespace GameEngine;
using Components::Markup;
using Components::MarkupAuthor;
using MarkupECS::MarkupService;

TEST(MarkupNotesEditCommand, ACommentAndAStatusChangeUndoAndRedoAsOneStepEach)
{
    MarkupService::Initialize();
    ECS::World world;
    Editor::EditorChangeNotifications notifications;
    Editor::UndoRedoService undo;
    const ECS::EntityHandle lake = world.CreateEntity();
    world.AddComponentImmediate(lake, Markup{});
    MarkupService& service = MarkupService::Get();
    ASSERT_TRUE(service.BeginMarkup(world, lake, MarkupAuthor::User, 10));

    ASSERT_TRUE(Editor::CommitMarkupNotesEdit(world, lake, &undo, &notifications, "Add Mark-up Comment", [&]() {
        return service.AddComment(world, lake, MarkupAuthor::User, 20, "Keep the shore");
    }, {}));
    ASSERT_TRUE(Editor::CommitMarkupNotesEdit(world, lake, &undo, &notifications, "Set Mark-up Status", [&]() {
        return service.SetStatus(world, lake, Components::kMarkupStatusComplete, MarkupAuthor::User, 30);
    }, {}));
    EXPECT_EQ(service.FindNotes(world, lake)->Entries.size(), 3u);
    EXPECT_EQ(world.GetComponent<Markup>(lake)->Status, Components::kMarkupStatusComplete);

    undo.Undo();
    EXPECT_EQ(service.FindNotes(world, lake)->Entries.size(), 2u);
    EXPECT_EQ(world.GetComponent<Markup>(lake)->Status, Components::kMarkupStatusRequested);
    undo.Undo();
    EXPECT_EQ(service.FindNotes(world, lake)->Entries.size(), 1u);
    undo.Redo();
    ASSERT_EQ(service.FindNotes(world, lake)->Entries.size(), 2u);
    EXPECT_EQ(service.FindNotes(world, lake)->Entries.back().Text, "Keep the shore");

    // A refused edit records no step.
    EXPECT_FALSE(Editor::CommitMarkupNotesEdit(world, lake, &undo, &notifications, "Set Mark-up Status", [&]() {
        return service.SetStatus(world, lake, MarkupService::kInvalidTag, MarkupAuthor::User, 40);
    }, {}));
    undo.Undo();
    EXPECT_EQ(service.FindNotes(world, lake)->Entries.size(), 1u);
    MarkupService::Shutdown();
}

// A new tag name the edit added leaves the vocabulary on undo and returns on redo, and the
// vocabulary is saved each time.
TEST(MarkupNotesEditCommand, ATagNameTheEditAddedLeavesTheVocabularyOnUndo)
{
    MarkupService::Initialize();
    ECS::World world;
    Editor::EditorChangeNotifications notifications;
    Editor::UndoRedoService undo;
    const ECS::EntityHandle lake = world.CreateEntity();
    world.AddComponentImmediate(lake, Markup{});
    MarkupService& service = MarkupService::Get();
    ASSERT_TRUE(service.BeginMarkup(world, lake, MarkupAuthor::User, 10));
    const uint32 vocabulary = service.GetTagCount();
    int saves = 0;

    ASSERT_TRUE(Editor::CommitMarkupNotesEdit(world, lake, &undo, &notifications, "Set Mark-up Tags", [&]() {
        constexpr float32 kColor[4] = {0.6f, 0.6f, 0.6f, 1.0f};
        const uint32 tags[] = {service.AddTag("Brigde", {}, kColor)};
        return service.SetTags(world, lake, tags, MarkupAuthor::User, 20);
    }, [&saves]() { ++saves; }));
    EXPECT_EQ(service.GetTagCount(), vocabulary + 1);
    EXPECT_EQ(saves, 1);

    undo.Undo();
    EXPECT_EQ(service.GetTagCount(), vocabulary);
    EXPECT_EQ(service.FindTag("Brigde"), MarkupService::kInvalidTag);
    EXPECT_TRUE(service.FindNotes(world, lake)->Tags.empty());
    EXPECT_EQ(saves, 2);

    undo.Redo();
    ASSERT_NE(service.FindTag("Brigde"), MarkupService::kInvalidTag);
    EXPECT_EQ(service.FindNotes(world, lake)->Tags, std::vector<uint32>{service.FindTag("Brigde")});
    EXPECT_EQ(saves, 3);
    MarkupService::Shutdown();
}
