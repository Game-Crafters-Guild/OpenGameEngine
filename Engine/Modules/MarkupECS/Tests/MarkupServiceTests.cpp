// MarkupService: the one Updated writer (Touch), the writers that touch, the notes'
// keying by the full entity handle across delete and revive, and the tag vocabulary.

#include "MarkupECS/MarkupService.h"

#include "Components/Markup/Markup.h"
#include "Components/Name.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"

#include <gtest/gtest.h>

#include <cstring>

using namespace GameEngine;
using Components::Markup;
using Components::MarkupAuthor;
using MarkupECS::MarkupEntryKind;
using MarkupECS::MarkupService;

namespace
{

class MarkupServiceTest : public ::testing::Test
{
  protected:
    ECS::EntityHandle CreateMarkup()
    {
        const ECS::EntityHandle entity = World.CreateEntity();
        World.AddComponentImmediate(entity, Markup{});
        return entity;
    }

    MarkupService Service;
    ECS::World World;
};

} // namespace

// Touch stamps who and when, and gives the mark-up the world's next revision: strictly
// increasing, so a reader asking for "after revision r" sees every later edit, even two
// in the same second. The writers touch with the author they are given and keep that
// author on the entry they append.
TEST_F(MarkupServiceTest, TouchAndTheWritersStampTheAuthorTimeAndANewRevision)
{
    const ECS::EntityHandle lake = CreateMarkup();
    const ECS::EntityHandle forest = CreateMarkup();
    EXPECT_EQ(Service.GetRevision(World), 0u);

    ASSERT_TRUE(Service.BeginMarkup(World, lake, MarkupAuthor::Agent, 1000));
    ASSERT_TRUE(Service.BeginMarkup(World, forest, MarkupAuthor::Agent, 1000));
    ASSERT_TRUE(Service.Touch(World, lake, MarkupAuthor::User, 1000));

    const Markup& lakeMarkup = *World.GetComponent<Markup>(lake);
    EXPECT_EQ(lakeMarkup.Author, MarkupAuthor::Agent);
    EXPECT_EQ(lakeMarkup.UpdatedBy, MarkupAuthor::User);
    EXPECT_EQ(lakeMarkup.CreatedUnix, 1000);
    EXPECT_EQ(lakeMarkup.UpdatedUnix, 1000);
    EXPECT_EQ(lakeMarkup.Revision, 3u);
    EXPECT_EQ(World.GetComponent<Markup>(forest)->Revision, 2u);
    EXPECT_EQ(Service.GetRevision(World), 3u);

    ASSERT_TRUE(Service.AddComment(World, lake, MarkupAuthor::Agent, 1060, "Moved the shore"));
    const auto& entries = Service.FindNotes(World, lake)->Entries;
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[0].Kind, MarkupEntryKind::Created);
    EXPECT_EQ(entries[1].Kind, MarkupEntryKind::Comment);
    EXPECT_EQ(entries[1].Author, MarkupAuthor::Agent);
    EXPECT_EQ(entries[1].Text, "Moved the shore");
    EXPECT_EQ(World.GetComponent<Markup>(lake)->UpdatedBy, MarkupAuthor::Agent);
    EXPECT_EQ(World.GetComponent<Markup>(lake)->Revision, 4u);

    const ECS::EntityHandle prop = World.CreateEntity();
    EXPECT_FALSE(Service.Touch(World, prop, MarkupAuthor::User, 1000));
    EXPECT_FALSE(Service.AddComment(World, prop, MarkupAuthor::User, 1000, "not a mark-up"));
    EXPECT_EQ(Service.GetRevision(World), 4u);
}

// A status is a tag of the status group: setting one appends a StatusChange entry, and a
// free tag is refused as a status, as a status is refused as a free tag.
TEST_F(MarkupServiceTest, StatusesComeFromTheStatusGroupAndFreeTagsFromOutsideIt)
{
    const ECS::EntityHandle village = CreateMarkup();
    const float32 color[4] = {0.5f, 0.5f, 0.5f, 1.0f};
    const uint32 market = Service.AddTag("Market", {}, color);

    EXPECT_FALSE(Service.SetStatus(World, village, market, MarkupAuthor::User, 10));
    ASSERT_TRUE(Service.SetStatus(World, village, Components::kMarkupStatusInProgress, MarkupAuthor::Agent, 20));
    EXPECT_EQ(World.GetComponent<Markup>(village)->Status, Components::kMarkupStatusInProgress);
    const auto& entry = Service.FindNotes(World, village)->Entries.back();
    EXPECT_EQ(entry.Kind, MarkupEntryKind::StatusChange);
    EXPECT_EQ(entry.Status, Components::kMarkupStatusInProgress);

    const uint32 statusAsTag[] = {Components::kMarkupStatusComplete};
    EXPECT_FALSE(Service.SetTags(World, village, statusAsTag, MarkupAuthor::User, 30));
    const uint32 freeTags[] = {market};
    ASSERT_TRUE(Service.SetTags(World, village, freeTags, MarkupAuthor::User, 30));
    EXPECT_EQ(Service.FindNotes(World, village)->Tags, std::vector<uint32>{market});
}

// The notes stay with the full handle: a delete leaves them dormant, a delete that keeps
// the handle is revived with them, and an entity that reuses the index carries a new
// generation and starts with none.
TEST_F(MarkupServiceTest, NotesAreDormantAfterADeleteFoundOnReviveAndNeverInheritedByAReusedIndex)
{
    const ECS::EntityHandle lake = CreateMarkup();
    ASSERT_TRUE(Service.SetDescription(World, lake, MarkupAuthor::User, 5, "Keep the north shore"));

    World.DestroyEntityImmediatePreserveHandle(lake);
    EXPECT_TRUE(Service.GetMarkups(World).empty());
    ASSERT_TRUE(World.ReviveEntityImmediatePreserveHandle(lake));
    World.AddComponentImmediate(lake, Markup{});
    ASSERT_NE(Service.FindNotes(World, lake), nullptr);
    EXPECT_EQ(Service.FindNotes(World, lake)->Description, "Keep the north shore");
    EXPECT_EQ(Service.GetMarkups(World), std::vector<ECS::EntityHandle>{lake});

    World.DestroyEntityImmediate(lake);
    ECS::EntityHandle reused = World.CreateEntity();
    for (int attempt = 0; attempt < 4096 && reused.index != lake.index; ++attempt)
        reused = World.CreateEntity();
    ASSERT_EQ(reused.index, lake.index);
    ASSERT_NE(reused.version, lake.version);
    EXPECT_EQ(Service.FindNotes(World, reused), nullptr);
}

// The vocabulary starts with the six statuses at the ids the component names; a tag's
// name is unique and holds no line break.
TEST_F(MarkupServiceTest, TheVocabularyStartsWithTheSixStatusesAndKeepsNamesUnique)
{
    const char* const statuses[] = {"Proposed", "Requested", "InProgress", "Complete", "Revision", "Problem"};
    for (uint32 id = 0; id < 6; ++id)
    {
        ASSERT_NE(Service.GetTag(id), nullptr);
        EXPECT_EQ(Service.GetTag(id)->Name, statuses[id]);
        EXPECT_TRUE(Service.IsStatusTag(id));
    }
    EXPECT_EQ(Service.FindTag("Problem"), Components::kMarkupStatusProblem);
    EXPECT_NE(Service.GetTag(Components::kMarkupStatusComplete)->Color[1],
              Service.GetTag(Components::kMarkupStatusProblem)->Color[1]);

    const float32 color[4] = {0.1f, 0.2f, 0.3f, 1.0f};
    const uint32 bridge = Service.AddTag("Bridge", {}, color);
    EXPECT_EQ(Service.AddTag("Bridge", MarkupECS::kMarkupStatusGroup, color), bridge);
    EXPECT_FALSE(Service.IsStatusTag(bridge));
    EXPECT_EQ(Service.AddTag("Two\nlines", {}, color), MarkupService::kInvalidTag);
    EXPECT_EQ(Service.AddTag("", {}, color), MarkupService::kInvalidTag);
}

// Only the newest tag can be removed (ids are indices), never a status, and never while a
// mark-up's notes hold it: what an undone edit added goes, and nothing it did not add.
TEST_F(MarkupServiceTest, OnlyTheNewestUnusedFreeTagCanBeRemoved)
{
    const ECS::EntityHandle village = CreateMarkup();
    const float32 color[4] = {0.1f, 0.2f, 0.3f, 1.0f};
    const uint32 market = Service.AddTag("Market", {}, color);
    const uint32 bridge = Service.AddTag("Bridge", {}, color);
    const uint32 holds[] = {bridge};
    ASSERT_TRUE(Service.SetTags(World, village, holds, MarkupAuthor::User, 10));

    EXPECT_FALSE(Service.RemoveTag(market));
    EXPECT_FALSE(Service.RemoveTag(bridge));
    ASSERT_TRUE(Service.SetTags(World, village, {}, MarkupAuthor::User, 20));
    EXPECT_TRUE(Service.RemoveTag(bridge));
    EXPECT_EQ(Service.FindTag("Bridge"), MarkupService::kInvalidTag);
    EXPECT_TRUE(Service.RemoveTag(market));
    EXPECT_EQ(Service.GetTagCount(), 6u);
    EXPECT_FALSE(Service.RemoveTag(Components::kMarkupStatusProblem));
}

// A world reset (a scene load that replaces the world) restarts entity versions, so the
// next scene's entity can carry an old handle: the reset drops the world's notes and
// revision. A new world, even at a dead one's address, starts with none.
TEST_F(MarkupServiceTest, AWorldResetOrANewWorldStartsWithNoNotes)
{
    const ECS::EntityHandle lake = CreateMarkup();
    ASSERT_TRUE(Service.SetDescription(World, lake, MarkupAuthor::User, 5, "Scene A secret"));
    ASSERT_GT(Service.GetRevision(World), 0u);

    World.Clear();
    const ECS::EntityHandle next = CreateMarkup();
    ASSERT_EQ(next.id, lake.id);
    EXPECT_EQ(Service.FindNotes(World, next), nullptr);
    EXPECT_EQ(Service.GetRevision(World), 0u);
    EXPECT_TRUE(Service.GetMarkups(World).empty());

    for (int pass = 0; pass < 2; ++pass)
    {
        ECS::World fresh;
        const ECS::EntityHandle entity = fresh.CreateEntity();
        fresh.AddComponentImmediate(entity, Markup{});
        EXPECT_EQ(Service.FindNotes(fresh, entity), nullptr) << "pass " << pass;
        ASSERT_TRUE(Service.AddComment(fresh, entity, MarkupAuthor::User, 5, "only this world's"));
        EXPECT_EQ(Service.FindNotes(fresh, entity)->Entries.size(), 1u) << "pass " << pass;
    }
}

// Edits one author makes close together are one thread entry holding every change; another
// author, a comment between them or a gap past kEditCoalesceSeconds starts a new one.
TEST_F(MarkupServiceTest, CloseEditsByOneAuthorAreOneEntry)
{
    using MarkupECS::MarkupEditChange;
    using MarkupECS::MarkupEditChanges;
    const ECS::EntityHandle docks = CreateMarkup();
    ASSERT_TRUE(Service.BeginMarkup(World, docks, MarkupAuthor::User, 1000));
    const auto& entries = Service.FindNotes(World, docks)->Entries;

    ASSERT_TRUE(Service.RecordEdit(World, docks, MarkupEditChanges({MarkupEditChange::Moved}), MarkupAuthor::User, 1010));
    ASSERT_TRUE(Service.RecordEdit(World, docks, MarkupEditChanges({MarkupEditChange::Resized}), MarkupAuthor::User,
                                   1010 + MarkupService::kEditCoalesceSeconds));
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[1].Kind, MarkupEntryKind::Edit);
    EXPECT_EQ(entries[1].Changes, MarkupEditChanges({MarkupEditChange::Moved, MarkupEditChange::Resized}));
    EXPECT_EQ(entries[1].TimeUnix, 1010 + MarkupService::kEditCoalesceSeconds);

    ASSERT_TRUE(Service.RecordEdit(World, docks, MarkupEditChanges({MarkupEditChange::Moved}), MarkupAuthor::Agent, 1200));
    ASSERT_TRUE(Service.AddComment(World, docks, MarkupAuthor::Agent, 1201, "Moved it to the shore"));
    ASSERT_TRUE(Service.RecordEdit(World, docks, MarkupEditChanges({MarkupEditChange::Renamed}), MarkupAuthor::Agent, 1202));
    ASSERT_TRUE(Service.RecordEdit(World, docks, MarkupEditChanges({MarkupEditChange::Renamed}), MarkupAuthor::Agent,
                                   1203 + MarkupService::kEditCoalesceSeconds));
    ASSERT_EQ(entries.size(), 6u);
    EXPECT_EQ(entries[2].Author, MarkupAuthor::Agent);
    EXPECT_EQ(entries[4].Changes, MarkupEditChanges({MarkupEditChange::Renamed}));
    EXPECT_EQ(entries[5].Kind, MarkupEntryKind::Edit);

    EXPECT_FALSE(Service.RecordEdit(World, docks, 0, MarkupAuthor::User, 1300));

    // A rename keeps the name it gave, the latest one when renames join.
    Components::Name name{};
    std::strncpy(name.value, "The Docks (east)", sizeof(name.value) - 1);
    World.AddComponentImmediate(docks, name);
    ASSERT_TRUE(Service.RecordEdit(World, docks, MarkupEditChanges({MarkupEditChange::Renamed}), MarkupAuthor::Agent,
                                   1203 + MarkupService::kEditCoalesceSeconds));
    EXPECT_EQ(entries.back().Text, "The Docks (east)");
    EXPECT_EQ(World.GetComponent<Markup>(docks)->UpdatedBy, MarkupAuthor::Agent);
}

// A description edit is recorded in the thread, and joins the author's close edits.
TEST_F(MarkupServiceTest, ADescriptionEditIsRecordedInTheThread)
{
    using MarkupECS::MarkupEditChange;
    using MarkupECS::MarkupEditChanges;
    const ECS::EntityHandle docks = CreateMarkup();
    ASSERT_TRUE(Service.BeginMarkup(World, docks, MarkupAuthor::User, 1000));
    ASSERT_TRUE(Service.RecordEdit(World, docks, MarkupEditChanges({MarkupEditChange::Moved}), MarkupAuthor::User, 1001));
    ASSERT_TRUE(Service.SetDescription(World, docks, MarkupAuthor::User, 1002, "Piers on the east shore"));
    const auto& entries = Service.FindNotes(World, docks)->Entries;
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[1].Changes, MarkupEditChanges({MarkupEditChange::Moved, MarkupEditChange::Described}));
    EXPECT_EQ(Service.FindNotes(World, docks)->Description, "Piers on the east shore");
}
