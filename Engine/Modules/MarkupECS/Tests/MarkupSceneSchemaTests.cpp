// The Markup scene lines: a mark-up and its notes survive a save and a load, text with
// line breaks, quotes and backslashes included; the component is written once, by its
// own schema; and a tag the project's vocabulary lacks is added on load.

#include "MarkupECS/MarkupService.h"

#include "Components/Markup/Markup.h"
#include "Components/Name.h"
#include "Components/Spline/SplineComponent.h"
#include "Components/SceneEntityTag.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "Scene/SceneIO.h"
#include "Scene/SceneSchemaRegistry.h"
#include "SplineECS/SplineService.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <string>
#include <vector>

using namespace GameEngine;
using Components::Markup;
using Components::MarkupAuthor;
using MarkupECS::MarkupEntryKind;
using MarkupECS::MarkupService;

namespace
{

class MarkupSceneSchemaTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        Scene::EnsureBuiltInSchemasRegistered();
        m_OwnsService = !MarkupService::IsInitialized();
        if (m_OwnsService)
            MarkupService::Initialize();
        m_Directory = std::filesystem::temp_directory_path() /
                      ("GameEngine_MarkupScene_" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(m_Directory);
    }

    void TearDown() override
    {
        if (m_OwnsService)
            MarkupService::Shutdown();
        std::error_code ignored;
        std::filesystem::remove_all(m_Directory, ignored);
    }

    std::filesystem::path PathOf(const char* name) const { return m_Directory / name; }

    static std::string ReadText(const std::filesystem::path& path)
    {
        std::ifstream in(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }

    static ECS::EntityHandle FindByTag(ECS::World& world, std::string_view tag)
    {
        ECS::EntityHandle found = ECS::EntityHandle::Invalid();
        world.Query<ECS::Read<Components::SceneEntityTag>>().Each(
            [&](ECS::EntityHandle entity, const Components::SceneEntityTag& entityTag)
            {
                if (entityTag.View() == tag)
                    found = entity;
            });
        return found;
    }

  private:
    bool m_OwnsService = false;
    std::filesystem::path m_Directory;
};

size_t CountLinesStartingWith(const std::string& text, std::string_view prefix)
{
    size_t count = 0;
    size_t start = 0;
    while (start < text.size())
    {
        const size_t end = text.find('\n', start);
        const std::string_view line(text.data() + start, (end == std::string::npos ? text.size() : end) - start);
        if (line.substr(0, prefix.size()) == prefix)
            ++count;
        start = end == std::string::npos ? text.size() : end + 1;
    }
    return count;
}

} // namespace

TEST_F(MarkupSceneSchemaTest, AMarkupAndItsNotesRoundTripWithLineBreaksQuotesAndBackslashes)
{
    const std::string description = "Line one\nLine \"two\" ends in a backslash \\";
    const std::string comment = "Path C:\\Maps\\north | \"shore\"\r\nsecond line";
    MarkupService& service = MarkupService::Get();
    const float32 color[4] = {0.2f, 0.4f, 0.6f, 1.0f};
    const uint32 ferry = service.AddTag("Ferry", {}, color);

    ECS::World original;
    const ECS::EntityHandle lake = original.CreateEntity();
    original.AddComponentImmediate(lake, Components::SceneEntityTag{"lake"});
    Markup markup{};
    markup.Status = Components::kMarkupStatusProposed;
    markup.Color[0] = 0.25f;
    markup.Color[3] = 1.0f;
    original.AddComponentImmediate(lake, markup);
    ASSERT_TRUE(service.BeginMarkup(original, lake, MarkupAuthor::Agent, 1759750000));
    ASSERT_TRUE(service.SetDescription(original, lake, MarkupAuthor::Agent, 1759750001, description));
    ASSERT_TRUE(service.AddComment(original, lake, MarkupAuthor::User, 1759750002, comment));
    ASSERT_TRUE(service.SetStatus(original, lake, Components::kMarkupStatusRequested, MarkupAuthor::User, 1759750003));
    const uint32 tags[] = {ferry};
    ASSERT_TRUE(service.SetTags(original, lake, tags, MarkupAuthor::User, 1759750004));

    const auto path = PathOf("Lake.scene");
    ASSERT_TRUE(Scene::SaveSceneToFile(original, path));
    const std::string text = ReadText(path);
    // Written once, by the schema that owns the type: no reflected field lines beside it.
    EXPECT_EQ(CountLinesStartingWith(text, "Markup.status = "), 1u) << text;
    EXPECT_EQ(CountLinesStartingWith(text, "Markup.Status"), 0u) << text;
    EXPECT_EQ(CountLinesStartingWith(text, "Markup.CreatedUnix"), 0u) << text;
    EXPECT_EQ(CountLinesStartingWith(text, "Markup.description = \"Line one\\nLine \\\"two\\\""), 1u) << text;

    ECS::World loaded;
    ASSERT_TRUE(Scene::LoadSceneFromFile(loaded, path, Scene::LoadOptions{Scene::LoadMode::Replace}));
    const ECS::EntityHandle loadedLake = FindByTag(loaded, "lake");
    ASSERT_TRUE(loadedLake.IsValid());
    const Markup* loadedMarkup = loaded.GetComponent<Markup>(loadedLake);
    ASSERT_NE(loadedMarkup, nullptr);
    EXPECT_EQ(loadedMarkup->Status, Components::kMarkupStatusRequested);
    EXPECT_EQ(loadedMarkup->Author, MarkupAuthor::Agent);
    EXPECT_EQ(loadedMarkup->UpdatedBy, MarkupAuthor::User);
    EXPECT_EQ(loadedMarkup->CreatedUnix, 1759750000);
    EXPECT_EQ(loadedMarkup->UpdatedUnix, 1759750004);
    EXPECT_EQ(loadedMarkup->Color[0], 0.25f);

    const MarkupECS::MarkupNotes* notes = service.FindNotes(loaded, loadedLake);
    ASSERT_NE(notes, nullptr);
    EXPECT_EQ(notes->Description, description);
    EXPECT_EQ(notes->Tags, std::vector<uint32>{ferry});
    ASSERT_EQ(notes->Entries.size(), 4u);
    EXPECT_EQ(notes->Entries[0].Kind, MarkupEntryKind::Created);
    EXPECT_EQ(notes->Entries[0].Status, Components::kMarkupStatusProposed);
    EXPECT_EQ(notes->Entries[1].Kind, MarkupEntryKind::Edit); // the description edit
    EXPECT_EQ(notes->Entries[1].Author, MarkupAuthor::Agent);
    EXPECT_EQ(notes->Entries[2].Kind, MarkupEntryKind::Comment);
    EXPECT_EQ(notes->Entries[2].Author, MarkupAuthor::User);
    EXPECT_EQ(notes->Entries[2].TimeUnix, 1759750002);
    EXPECT_EQ(notes->Entries[2].Text, comment);
    EXPECT_EQ(notes->Entries[3].Kind, MarkupEntryKind::StatusChange);
    EXPECT_EQ(notes->Entries[3].Status, Components::kMarkupStatusRequested);
}

// A scene written where the vocabulary had a status and a tag this project lacks keeps
// both: the status joins the status group and the tag joins the free tags.
TEST_F(MarkupSceneSchemaTest, TagsTheVocabularyLacksAreAddedOnLoad)
{
    const auto path = PathOf("Village.scene");
    {
        std::ofstream out(path, std::ios::binary);
        out << "[scene name=\"Village\" version=1]\n"
               "\n"
               "[entity id=\"village\"]\n"
               "Markup.status = \"Blocked by weather\"\n"
               "Markup.author = User\n"
               "Markup.tagCount = 1\n"
               "Markup.tag0 = \"Market day\"\n";
    }

    ECS::World world;
    ASSERT_TRUE(Scene::LoadSceneFromFile(world, path, Scene::LoadOptions{Scene::LoadMode::Replace}));
    const ECS::EntityHandle village = FindByTag(world, "village");
    ASSERT_TRUE(village.IsValid());

    const MarkupService& service = MarkupService::Get();
    const uint32 blocked = service.FindTag("Blocked by weather");
    const uint32 market = service.FindTag("Market day");
    ASSERT_NE(blocked, MarkupService::kInvalidTag);
    ASSERT_NE(market, MarkupService::kInvalidTag);
    EXPECT_TRUE(service.IsStatusTag(blocked));
    EXPECT_FALSE(service.IsStatusTag(market));
    EXPECT_EQ(world.GetComponent<Markup>(village)->Status, blocked);
    EXPECT_EQ(service.FindNotes(world, village)->Tags, std::vector<uint32>{market});
}

namespace
{

// Writes `body` under a [scene] header to `path` and loads it, replacing `world`.
void LoadSceneText(ECS::World& world, const std::filesystem::path& path, const std::string& body)
{
    {
        std::ofstream out(path, std::ios::binary);
        out << "[scene name=\"Probe\" version=1]\n\n" << body;
    }
    (void)Scene::LoadSceneFromFile(world, path, Scene::LoadOptions{Scene::LoadMode::Replace});
}

} // namespace

// Opening scene B over scene A in the same world gives B's mark-up only B's notes, though
// the reset hands B's entity A's handle, and B's save carries nothing of A.
TEST_F(MarkupSceneSchemaTest, AReplaceLoadKeepsTheNextScenesNotesItsOwn)
{
    ECS::World world;
    LoadSceneText(world, PathOf("A.scene"),
                  "[entity id=\"a\"]\nMarkup.status = \"Requested\"\nMarkup.description = \"Scene A secret\"\n");
    LoadSceneText(world, PathOf("B.scene"), "[entity id=\"b\"]\nMarkup.status = \"Proposed\"\n");
    const ECS::EntityHandle b = FindByTag(world, "b");
    ASSERT_TRUE(b.IsValid());
    const MarkupECS::MarkupNotes* notes = MarkupService::Get().FindNotes(world, b);
    EXPECT_TRUE(notes == nullptr || notes->Description.empty());

    ASSERT_TRUE(Scene::SaveSceneToFile(world, PathOf("B-saved.scene")));
    EXPECT_EQ(ReadText(PathOf("B-saved.scene")).find("Scene A secret"), std::string::npos);
}

// An index ahead of the next one is refused, so a scene cannot make the load allocate what
// it does not hold: entry 4000000000 and a tag that skips an index load nothing.
TEST_F(MarkupSceneSchemaTest, IndexedLinesThatSkipAheadAreRefused)
{
    ECS::World world;
    LoadSceneText(world, PathOf("Huge.scene"),
                  "[entity id=\"lake\"]\n"
                  "Markup.status = \"Requested\"\n"
                  "Markup.entry4000000000 = Comment|User|5|\"\"|\"x\"\n"
                  "Markup.tag0 = \"Ferry\"\n"
                  "Markup.tag2 = \"Mill\"\n");
    const ECS::EntityHandle lake = FindByTag(world, "lake");
    ASSERT_TRUE(lake.IsValid());
    const MarkupECS::MarkupNotes* notes = MarkupService::Get().FindNotes(world, lake);
    ASSERT_NE(notes, nullptr);
    EXPECT_TRUE(notes->Entries.empty());
    EXPECT_EQ(notes->Tags.size(), 1u);
}

// A status name holding the entry separator or a quote survives a save and a load in a
// status-change entry, as it does in the status line.
TEST_F(MarkupSceneSchemaTest, AnEntrysStatusNameIsEscaped)
{
    MarkupService& service = MarkupService::Get();
    const float32 color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    const uint32 waiting = service.AddTag("Wait|ing \"now\"", MarkupECS::kMarkupStatusGroup, color);

    ECS::World original;
    const ECS::EntityHandle lake = original.CreateEntity();
    original.AddComponentImmediate(lake, Components::SceneEntityTag{"lake"});
    original.AddComponentImmediate(lake, Markup{});
    ASSERT_TRUE(service.SetStatus(original, lake, waiting, MarkupAuthor::User, 7));
    ASSERT_TRUE(Scene::SaveSceneToFile(original, PathOf("Waiting.scene")));

    ECS::World loaded;
    ASSERT_TRUE(Scene::LoadSceneFromFile(loaded, PathOf("Waiting.scene"), Scene::LoadOptions{Scene::LoadMode::Replace}));
    const ECS::EntityHandle loadedLake = FindByTag(loaded, "lake");
    ASSERT_TRUE(loadedLake.IsValid());
    EXPECT_EQ(loaded.GetComponent<Markup>(loadedLake)->Status, waiting);
    const MarkupECS::MarkupNotes* notes = service.FindNotes(loaded, loadedLake);
    ASSERT_NE(notes, nullptr);
    ASSERT_EQ(notes->Entries.size(), 1u);
    EXPECT_EQ(notes->Entries[0].Status, waiting);
}

// Loading refuses what the writers refuse: a status in a free-tag slot, an empty tag, a
// comment that carries a status. A refused entry line adds no half-read entry.
TEST_F(MarkupSceneSchemaTest, LoadingRefusesWhatTheWritersRefuseAndKeepsNoHalfEntry)
{
    ECS::World world;
    LoadSceneText(world, PathOf("Bad.scene"),
                  "[entity id=\"lake\"]\n"
                  "Markup.status = \"Requested\"\n"
                  "Markup.tag0 = \"Requested\"\n"
                  "Markup.entry0 = Created|User|5|\"Requested\"|\"made\"\n"
                  "Markup.entry1 = Comment|Nobody|9|\"\"|\"half\"\n"
                  "Markup.entry2 = Unreadable|User|1|\"Requested\"|\"x\"\n"
                  "\n"
                  "[entity id=\"pond\"]\n"
                  "Markup.status = \"Requested\"\n"
                  "Markup.description = \"kept\"\n"
                  "Markup.tag0 = \"\"\n"
                  "Markup.entry0 = Comment|User|6|\"Requested\"|\"x\"\n");
    const ECS::EntityHandle lake = FindByTag(world, "lake");
    const ECS::EntityHandle pond = FindByTag(world, "pond");
    ASSERT_TRUE(lake.IsValid());
    ASSERT_TRUE(pond.IsValid());

    // A refused line keeps its slot as a placeholder readers skip, never as half-read data.
    const MarkupECS::MarkupNotes* lakeNotes = MarkupService::Get().FindNotes(world, lake);
    ASSERT_NE(lakeNotes, nullptr);
    EXPECT_EQ(lakeNotes->Tags, std::vector<uint32>{MarkupService::kInvalidTag});
    // A scene line of the placeholder's own kind is refused too: no writer produces it.
    ASSERT_EQ(lakeNotes->Entries.size(), 3u);
    EXPECT_EQ(lakeNotes->Entries[0].Text, "made");
    EXPECT_EQ(lakeNotes->Entries[1].Kind, MarkupEntryKind::Unreadable);
    EXPECT_TRUE(lakeNotes->Entries[1].Text.empty());
    EXPECT_EQ(lakeNotes->Entries[2].Kind, MarkupEntryKind::Unreadable);
    EXPECT_TRUE(lakeNotes->Entries[2].Text.empty());

    const MarkupECS::MarkupNotes* pondNotes = MarkupService::Get().FindNotes(world, pond);
    ASSERT_NE(pondNotes, nullptr);
    EXPECT_EQ(pondNotes->Tags, std::vector<uint32>{MarkupService::kInvalidTag});
    ASSERT_EQ(pondNotes->Entries.size(), 1u);
    EXPECT_EQ(pondNotes->Entries[0].Kind, MarkupEntryKind::Unreadable);
}

// An unreadable line keeps its number: the next line still reads, the load reports one
// skip whose text it kept, and a save writes all three lines back as authored.
TEST_F(MarkupSceneSchemaTest, AnUnreadableEntryKeepsItsSlotAndSavesAsAuthored)
{
    const std::string entry0 = "Markup.entry0 = Created|User|5|\"Requested\"|\"made\"";
    const std::string entry1 = "Markup.entry1 = Comment|User|abc|\"\"|\"bad time\"";
    const std::string entry2 = "Markup.entry2 = Comment|Agent|7|\"\"|\"ok2\"";
    const auto path = PathOf("Thread.scene");
    {
        std::ofstream out(path, std::ios::binary);
        out << "[scene name=\"Thread\" version=1]\n\n[entity id=\"lake\"]\nMarkup.status = \"Requested\"\n"
            << entry0 << "\n" << entry1 << "\n" << entry2 << "\n";
    }

    ECS::World world;
    Scene::SceneLoadDegradation degradation;
    Scene::LoadOptions options{Scene::LoadMode::Replace};
    options.outDegradation = &degradation;
    ASSERT_TRUE(Scene::LoadSceneFromFile(world, path, options));
    ASSERT_EQ(degradation.skips.size(), 1u);
    EXPECT_EQ(degradation.skips[0].field, "entry1");
    EXPECT_TRUE(degradation.skips[0].preserved);

    const ECS::EntityHandle lake = FindByTag(world, "lake");
    ASSERT_TRUE(lake.IsValid());
    const MarkupECS::MarkupNotes* notes = MarkupService::Get().FindNotes(world, lake);
    ASSERT_NE(notes, nullptr);
    ASSERT_EQ(notes->Entries.size(), 3u);
    EXPECT_EQ(notes->Entries[1].Kind, MarkupEntryKind::Unreadable);
    EXPECT_EQ(notes->Entries[2].Text, "ok2");

    ASSERT_TRUE(Scene::SaveSceneToFile(world, PathOf("Thread-saved.scene")));
    const std::string saved = ReadText(PathOf("Thread-saved.scene"));
    EXPECT_NE(saved.find(entry0 + "\n"), std::string::npos) << saved;
    EXPECT_NE(saved.find(entry1 + "\n"), std::string::npos) << saved;
    EXPECT_NE(saved.find(entry2 + "\n"), std::string::npos) << saved;
}

// The revision is saved, and reopening the scene raises the world's revision to the largest
// one loaded, so an edit after the reopen orders after a `since` read before it.
TEST_F(MarkupSceneSchemaTest, TheRevisionSurvivesAReopen)
{
    MarkupService& service = MarkupService::Get();
    ECS::World original;
    const ECS::EntityHandle lake = original.CreateEntity();
    original.AddComponentImmediate(lake, Components::SceneEntityTag{"lake"});
    original.AddComponentImmediate(lake, Markup{});
    for (int edit = 0; edit < 5; ++edit)
        ASSERT_TRUE(service.Touch(original, lake, MarkupAuthor::User, 10 + edit));
    const uint32 readerSince = service.GetRevision(original);
    ASSERT_TRUE(Scene::SaveSceneToFile(original, PathOf("Reopen.scene")));

    ECS::World reopened;
    ASSERT_TRUE(Scene::LoadSceneFromFile(reopened, PathOf("Reopen.scene"), Scene::LoadOptions{Scene::LoadMode::Replace}));
    const ECS::EntityHandle loadedLake = FindByTag(reopened, "lake");
    ASSERT_TRUE(loadedLake.IsValid());
    EXPECT_EQ(reopened.GetComponent<Markup>(loadedLake)->Revision, readerSince);
    EXPECT_EQ(service.GetRevision(reopened), readerSince);
    ASSERT_TRUE(service.Touch(reopened, loadedLake, MarkupAuthor::Agent, 99));
    EXPECT_GT(reopened.GetComponent<Markup>(loadedLake)->Revision, readerSince);
}

// A mark-up in a blueprint is refused at load with the fix: its thread could not follow
// an instance. The entity loads without it.
TEST_F(MarkupSceneSchemaTest, AMarkupInABlueprintIsRefused)
{
    const auto path = PathOf("Cart.blueprint");
    {
        std::ofstream out(path, std::ios::binary);
        out << "[blueprint name=\"Cart\" version=1]\n"
               "\n"
               "[entity id=\"cart\"]\n"
               "Markup.status = \"Requested\"\n"
               "Transform.position = (1, 0, 0)\n";
    }

    ECS::World world;
    (void)Scene::LoadSceneFromFile(world, path, Scene::LoadOptions{Scene::LoadMode::Replace});
    const ECS::EntityHandle cart = FindByTag(world, "cart");
    ASSERT_TRUE(cart.IsValid());
    EXPECT_EQ(world.GetComponent<Markup>(cart), nullptr);
    EXPECT_EQ(MarkupService::Get().FindNotes(world, cart), nullptr);
}

// Setting tags after a load that held an unreadable tag line keeps that line's slot: the
// save writes the authored text back under its own key and both new tags beside it.
TEST_F(MarkupSceneSchemaTest, SetTagsKeepsAnUnreadableTagLinesSlot)
{
    const std::string unreadable = "Markup.tag1 = \"Requested\"";
    ECS::World world;
    LoadSceneText(world, PathOf("Tags.scene"),
                  "[entity id=\"lake\"]\n"
                  "Markup.status = \"Requested\"\n"
                  "Markup.tag0 = \"Ferry\"\n" +
                      unreadable + "\n");
    const ECS::EntityHandle lake = FindByTag(world, "lake");
    ASSERT_TRUE(lake.IsValid());

    MarkupService& service = MarkupService::Get();
    const float32 color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    const uint32 tags[] = {service.AddTag("Alpha", {}, color), service.AddTag("Beta", {}, color)};
    ASSERT_TRUE(service.SetTags(world, lake, tags, MarkupAuthor::User, 50));

    ASSERT_TRUE(Scene::SaveSceneToFile(world, PathOf("Tags-saved.scene")));
    const std::string saved = ReadText(PathOf("Tags-saved.scene"));
    EXPECT_NE(saved.find(unreadable + "\n"), std::string::npos) << saved;
    EXPECT_NE(saved.find("Markup.tag0 = \"Alpha\"\n"), std::string::npos) << saved;
    EXPECT_NE(saved.find("Markup.tag2 = \"Beta\"\n"), std::string::npos) << saved;
}

// Fewer tags than readable slots renumber an unreadable tag line's slot (tag2 to tag1): its
// authored text moves with it, so the save writes it under the new key and nothing is lost.
TEST_F(MarkupSceneSchemaTest, ARenumberedUnreadableTagLineKeepsItsText)
{
    ECS::World world;
    LoadSceneText(world, PathOf("Renumber.scene"),
                  "[entity id=\"lake\"]\n"
                  "Markup.status = \"Requested\"\n"
                  "Markup.tag0 = \"Ferry\"\n"
                  "Markup.tag1 = \"Ocean\"\n"
                  "Markup.tag2 = \"Requested\"\n");
    const ECS::EntityHandle lake = FindByTag(world, "lake");
    ASSERT_TRUE(lake.IsValid());

    MarkupService& service = MarkupService::Get();
    const float32 color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    const uint32 tags[] = {service.AddTag("Alpha", {}, color)};
    ASSERT_TRUE(service.SetTags(world, lake, tags, MarkupAuthor::User, 50));

    ASSERT_TRUE(Scene::SaveSceneToFile(world, PathOf("Renumber-saved.scene")));
    const std::string saved = ReadText(PathOf("Renumber-saved.scene"));
    EXPECT_NE(saved.find("Markup.tag0 = \"Alpha\"\n"), std::string::npos) << saved;
    EXPECT_NE(saved.find("Markup.tag1 = \"Requested\"\n"), std::string::npos) << saved;
    EXPECT_EQ(saved.find("Markup.tag2"), std::string::npos) << saved;
}

// An Edit entry saves its changes by name and loads them back; an unknown change name is
// refused with the names the line may hold.
TEST_F(MarkupSceneSchemaTest, AnEditEntryRoundTripsItsChanges)
{
    using MarkupECS::MarkupEditChange;
    using MarkupECS::MarkupEditChanges;
    MarkupService& service = MarkupService::Get();
    ECS::World original;
    const ECS::EntityHandle docks = original.CreateEntity();
    original.AddComponentImmediate(docks, Components::SceneEntityTag{"docks"});
    Markup colored{};
    colored.Color[0] = 242.0f / 255.0f;
    colored.Color[1] = 140.0f / 255.0f;
    colored.Color[2] = 38.0f / 255.0f;
    colored.Color[3] = 1.0f;
    original.AddComponentImmediate(docks, colored);
    ASSERT_TRUE(service.BeginMarkup(original, docks, MarkupAuthor::User, 7));
    const uint8 changes = MarkupEditChanges({MarkupEditChange::Moved, MarkupEditChange::Resized, MarkupEditChange::Recolored});
    ASSERT_TRUE(service.RecordEdit(original, docks, changes, MarkupAuthor::Agent, 9));
    ASSERT_TRUE(Scene::SaveSceneToFile(original, PathOf("Edit.scene")));
    EXPECT_NE(ReadText(PathOf("Edit.scene")).find("Markup.entry1 = Edit|Agent|9|\"\"|\"moved resized recolored=#F28C26\""),
              std::string::npos);

    ECS::World loaded;
    ASSERT_TRUE(Scene::LoadSceneFromFile(loaded, PathOf("Edit.scene"), Scene::LoadOptions{Scene::LoadMode::Replace}));
    const MarkupECS::MarkupNotes* notes = service.FindNotes(loaded, FindByTag(loaded, "docks"));
    ASSERT_NE(notes, nullptr);
    ASSERT_EQ(notes->Entries.size(), 2u);
    EXPECT_EQ(notes->Entries[1].Kind, MarkupEntryKind::Edit);
    EXPECT_EQ(notes->Entries[1].Author, MarkupAuthor::Agent);
    EXPECT_EQ(notes->Entries[1].Changes, changes);
    EXPECT_EQ(notes->Entries[1].ColorArgb, 0xFFF28C26u);
    EXPECT_TRUE(notes->Entries[1].Text.empty());

    // A rename keeps the name it gave in the entry's status field, escaped, and reads it back.
    ECS::World renamed;
    const ECS::EntityHandle quay = renamed.CreateEntity();
    renamed.AddComponentImmediate(quay, Components::SceneEntityTag{"quay"});
    renamed.AddComponentImmediate(quay, Markup{});
    Components::Name name{};
    std::strncpy(name.value, "Quay \"east\" | west", sizeof(name.value) - 1);
    renamed.AddComponentImmediate(quay, name);
    ASSERT_TRUE(service.BeginMarkup(renamed, quay, MarkupAuthor::User, 7));
    ASSERT_TRUE(service.RecordEdit(renamed, quay, MarkupEditChanges({MarkupEditChange::Moved, MarkupEditChange::Renamed}),
                                   MarkupAuthor::User, 11));
    ASSERT_TRUE(Scene::SaveSceneToFile(renamed, PathOf("Renamed.scene")));
    EXPECT_NE(ReadText(PathOf("Renamed.scene"))
                  .find("Markup.entry1 = Edit|User|11|\"Quay \\\"east\\\" | west\"|\"moved renamed\""),
              std::string::npos)
        << ReadText(PathOf("Renamed.scene"));
    ECS::World reloaded;
    ASSERT_TRUE(Scene::LoadSceneFromFile(reloaded, PathOf("Renamed.scene"), Scene::LoadOptions{Scene::LoadMode::Replace}));
    const MarkupECS::MarkupNotes* quayNotes = service.FindNotes(reloaded, FindByTag(reloaded, "quay"));
    ASSERT_NE(quayNotes, nullptr);
    ASSERT_EQ(quayNotes->Entries.size(), 2u);
    EXPECT_EQ(quayNotes->Entries[1].Text, "Quay \"east\" | west");

    const auto bad = PathOf("BadEdit.scene");
    {
        std::ofstream out(bad, std::ios::binary);
        out << "[scene name=\"Bad\" version=1]\n\n[entity id=\"d\"]\nMarkup.status = \"Proposed\"\n"
            << "Markup.entry0 = Edit|User|5|\"\"|\"teleported\"\n";
    }
    ECS::World refused;
    Scene::SceneLoadDegradation degradation;
    Scene::LoadOptions options{Scene::LoadMode::Replace};
    options.outDegradation = &degradation;
    ASSERT_TRUE(Scene::LoadSceneFromFile(refused, bad, options));
    ASSERT_EQ(degradation.skips.size(), 1u);
    const MarkupECS::MarkupNotes* badNotes = service.FindNotes(refused, FindByTag(refused, "d"));
    ASSERT_NE(badNotes, nullptr);
    ASSERT_EQ(badNotes->Entries.size(), 1u);
    EXPECT_EQ(badNotes->Entries[0].Kind, MarkupEntryKind::Unreadable);
}

namespace
{

// A tagged mark-up entity: a sphere volume, or a region whose closed linear outline is `knots`.
ECS::EntityHandle AddTaggedMarkup(ECS::World& world, const char* tag, std::initializer_list<Mathematics::Vector3> knots)
{
    const ECS::EntityHandle entity = world.CreateEntity();
    Components::SceneEntityTag entityTag{};
    std::strncpy(entityTag.value, tag, sizeof(entityTag.value) - 1);
    world.AddComponentImmediate(entity, entityTag);
    world.AddComponentImmediate(entity, Markup{});
    if (knots.size() == 0)
    {
        world.AddComponentImmediate(entity, Components::MarkupVolume{Components::MarkupVolumeShape::Sphere, {}});
        return entity;
    }
    SplineECS::SplineService& splines = SplineECS::SplineService::Get();
    const SplineECS::SplineHandle handle = splines.CreateSpline(Spline::SplineType::Linear, true);
    for (const Mathematics::Vector3& knot : knots)
        splines.GetSplineData(handle)->AddPoint(knot, 0.0f);
    Components::SplineComponent spline{};
    spline.SplineDataIndex = handle.Index();
    spline.SplineDataGeneration = handle.Generation();
    world.AddComponentImmediate(entity, spline);
    world.AddComponentImmediate(entity, Components::MarkupRegion{});
    return entity;
}

class MarkupRegionSceneSchemaTest : public MarkupSceneSchemaTest
{
  protected:
    void SetUp() override
    {
        MarkupSceneSchemaTest::SetUp();
        m_OwnsSplines = !SplineECS::SplineService::IsInitialized();
        if (m_OwnsSplines)
            SplineECS::SplineService::Initialize();
    }

    void TearDown() override
    {
        if (m_OwnsSplines)
            SplineECS::SplineService::Shutdown();
        MarkupSceneSchemaTest::TearDown();
    }

  private:
    bool m_OwnsSplines = false;
};

} // namespace

// A region saves its height and its members by scene id, once (no reflected Members lines),
// and its outline through the Spline lines; a load resolves the members to the loaded
// mark-ups, forward references included.
TEST_F(MarkupRegionSceneSchemaTest, ARegionRoundTripsItsHeightMembersAndOutline)
{
    ECS::World original;
    const ECS::EntityHandle forest = AddTaggedMarkup(
        original, "forest", {{0.0f, 0.0f, 0.0f}, {200.0f, 0.0f, 0.0f}, {200.0f, 0.0f, 150.0f}, {0.0f, 0.0f, 150.0f}});
    const ECS::EntityHandle lake = AddTaggedMarkup(original, "lake", {});
    const ECS::EntityHandle field = AddTaggedMarkup(original, "field", {});
    Components::MarkupRegion region{};
    region.ExtrudeHeight = 12.5f;
    region.MemberCount = 2;
    region.Members[0] = {lake, Components::MarkupMemberMode::Exclude};
    region.Members[1] = {field, Components::MarkupMemberMode::Include};
    original.AddComponentImmediate(forest, region);

    const auto path = PathOf("Forest.scene");
    ASSERT_TRUE(Scene::SaveSceneToFile(original, path));
    const std::string text = ReadText(path);
    EXPECT_EQ(CountLinesStartingWith(text, "MarkupRegion.extrudeHeight = 12.5"), 1u) << text;
    EXPECT_EQ(CountLinesStartingWith(text, "MarkupRegion.member0 = exclude \"lake\""), 1u) << text;
    EXPECT_EQ(CountLinesStartingWith(text, "MarkupRegion.member1 = include \"field\""), 1u) << text;
    EXPECT_EQ(CountLinesStartingWith(text, "MarkupRegion.Members"), 0u) << text;
    EXPECT_EQ(CountLinesStartingWith(text, "MarkupRegion.MemberCount"), 0u) << text;
    EXPECT_EQ(CountLinesStartingWith(text, "Spline.closed = true"), 1u) << text;

    ECS::World loaded;
    ASSERT_TRUE(Scene::LoadSceneFromFile(loaded, path, Scene::LoadOptions{Scene::LoadMode::Replace}));
    const ECS::EntityHandle loadedForest = FindByTag(loaded, "forest");
    const auto* loadedRegion = loaded.GetComponent<Components::MarkupRegion>(loadedForest);
    ASSERT_NE(loadedRegion, nullptr);
    EXPECT_EQ(loadedRegion->ExtrudeHeight, 12.5f);
    ASSERT_EQ(loadedRegion->MemberCount, 2u);
    EXPECT_EQ(loadedRegion->Members[0].Entity, FindByTag(loaded, "lake"));
    EXPECT_EQ(loadedRegion->Members[0].Mode, Components::MarkupMemberMode::Exclude);
    EXPECT_EQ(loadedRegion->Members[1].Entity, FindByTag(loaded, "field"));
    EXPECT_EQ(loadedRegion->Members[1].Mode, Components::MarkupMemberMode::Include);
    const auto* spline = loaded.GetComponent<Components::SplineComponent>(loadedForest);
    ASSERT_NE(spline, nullptr);
    const Spline::SplineData* data = SplineECS::SplineService::Get().GetSplineData(
        SplineECS::SplineHandle(spline->SplineDataIndex, spline->SplineDataGeneration));
    ASSERT_NE(data, nullptr);
    EXPECT_TRUE(data->Closed);
    EXPECT_EQ(data->Type, Spline::SplineType::Linear);
    EXPECT_EQ(data->Points.size(), 4u);
}

// A member deleted before the save is dropped and the rest renumbered; a member line naming
// no entity of the scene loads as a dangling member, which the next save drops.
TEST_F(MarkupRegionSceneSchemaTest, ADanglingMemberIsDroppedOnSave)
{
    ECS::World world;
    const ECS::EntityHandle forest =
        AddTaggedMarkup(world, "forest", {{0.0f, 0.0f, 0.0f}, {50.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 50.0f}});
    const ECS::EntityHandle lake = AddTaggedMarkup(world, "lake", {});
    const ECS::EntityHandle field = AddTaggedMarkup(world, "field", {});
    Components::MarkupRegion region{};
    region.MemberCount = 2;
    region.Members[0] = {lake, Components::MarkupMemberMode::Exclude};
    region.Members[1] = {field, Components::MarkupMemberMode::Include};
    world.AddComponentImmediate(forest, region);
    world.DestroyEntityImmediate(lake);

    ASSERT_TRUE(Scene::SaveSceneToFile(world, PathOf("Deleted.scene")));
    const std::string text = ReadText(PathOf("Deleted.scene"));
    EXPECT_EQ(CountLinesStartingWith(text, "MarkupRegion.member0 = include \"field\""), 1u) << text;
    EXPECT_EQ(CountLinesStartingWith(text, "MarkupRegion.member1"), 0u) << text;

    ECS::World dangling;
    LoadSceneText(dangling, PathOf("Dangling.scene"),
                  "[entity id=\"forest\"]\n"
                  "Markup.status = \"Requested\"\n"
                  "MarkupRegion.extrudeHeight = 8\n"
                  "MarkupRegion.member0 = exclude \"gone\"\n");
    const auto* loaded = dangling.GetComponent<Components::MarkupRegion>(FindByTag(dangling, "forest"));
    ASSERT_NE(loaded, nullptr);
    ASSERT_EQ(loaded->MemberCount, 1u);
    EXPECT_FALSE(loaded->Members[0].Entity.IsValid());
    ASSERT_TRUE(Scene::SaveSceneToFile(dangling, PathOf("Dangling-saved.scene")));
    EXPECT_EQ(CountLinesStartingWith(ReadText(PathOf("Dangling-saved.scene")), "MarkupRegion.member"), 0u);
}

// What the writers refuse, a load refuses: a member line that skips an index, one past the
// 32nd, a region listing itself and a mode that is neither include nor exclude.
TEST_F(MarkupRegionSceneSchemaTest, LoadingRefusesSkippedIndicesTheCapSelfAndUnknownModes)
{
    ECS::World world;
    LoadSceneText(world, PathOf("Refused.scene"),
                  "[entity id=\"forest\"]\n"
                  "Markup.status = \"Requested\"\n"
                  "MarkupRegion.extrudeHeight = 8\n"
                  "MarkupRegion.member1 = exclude \"lake\"\n"
                  "MarkupRegion.member32 = exclude \"lake\"\n"
                  "MarkupRegion.member0 = exclude \"forest\"\n"
                  "MarkupRegion.member0 = around \"lake\"\n"
                  "\n"
                  "[entity id=\"lake\"]\n"
                  "Markup.status = \"Requested\"\n");
    const auto* region = world.GetComponent<Components::MarkupRegion>(FindByTag(world, "forest"));
    ASSERT_NE(region, nullptr);
    EXPECT_EQ(region->MemberCount, 0u);
}

// A path is a mark-up on an open spline and nothing else: it saves its notes and its points through
// the Spline lines, open and smooth, and loads back as a path (no volume, no region), its points
// where they were, heights included.
TEST_F(MarkupRegionSceneSchemaTest, APathRoundTripsAsAMarkupOnAnOpenSpline)
{
    ECS::World original;
    const ECS::EntityHandle road = original.CreateEntity();
    Components::SceneEntityTag entityTag{};
    std::strncpy(entityTag.value, "road", sizeof(entityTag.value) - 1);
    original.AddComponentImmediate(road, entityTag);
    original.AddComponentImmediate(road, Markup{});
    MarkupService::Get().EnsureNotes(original, road).Description = "The way to the guards' base";
    SplineECS::SplineService& splines = SplineECS::SplineService::Get();
    const SplineECS::SplineHandle handle = splines.CreateSpline(Spline::SplineType::CatmullRom, false);
    for (const Mathematics::Vector3& knot : {Mathematics::Vector3(0.0f, 30.0f, 0.0f), Mathematics::Vector3(40.0f, 34.5f, 10.0f),
                                             Mathematics::Vector3(90.0f, 41.0f, -5.0f)})
        splines.GetSplineData(handle)->AddPoint(knot, 0.0f);
    Components::SplineComponent spline{};
    spline.SplineDataIndex = handle.Index();
    spline.SplineDataGeneration = handle.Generation();
    original.AddComponentImmediate(road, spline);

    const auto path = PathOf("Road.scene");
    ASSERT_TRUE(Scene::SaveSceneToFile(original, path));
    const std::string text = ReadText(path);
    EXPECT_EQ(CountLinesStartingWith(text, "Spline.closed = false"), 1u) << text;
    EXPECT_EQ(CountLinesStartingWith(text, "MarkupRegion."), 0u) << text;
    EXPECT_EQ(CountLinesStartingWith(text, "MarkupVolume."), 0u) << text;

    ECS::World loaded;
    ASSERT_TRUE(Scene::LoadSceneFromFile(loaded, path, Scene::LoadOptions{Scene::LoadMode::Replace}));
    const ECS::EntityHandle loadedRoad = FindByTag(loaded, "road");
    ASSERT_NE(loaded.GetComponent<Markup>(loadedRoad), nullptr);
    EXPECT_EQ(loaded.GetComponent<Components::MarkupRegion>(loadedRoad), nullptr);
    EXPECT_EQ(loaded.GetComponent<Components::MarkupVolume>(loadedRoad), nullptr);
    const MarkupECS::MarkupNotes* notes = MarkupService::Get().FindNotes(loaded, loadedRoad);
    ASSERT_NE(notes, nullptr);
    EXPECT_EQ(notes->Description, "The way to the guards' base");
    const auto* loadedSpline = loaded.GetComponent<Components::SplineComponent>(loadedRoad);
    ASSERT_NE(loadedSpline, nullptr);
    const Spline::SplineData* data = splines.GetSplineData(
        SplineECS::SplineHandle(loadedSpline->SplineDataIndex, loadedSpline->SplineDataGeneration));
    ASSERT_NE(data, nullptr);
    EXPECT_FALSE(data->Closed);
    EXPECT_EQ(data->Type, Spline::SplineType::CatmullRom);
    ASSERT_EQ(data->Points.size(), 3u);
    EXPECT_FLOAT_EQ(data->Points[1].Position.y, 34.5f);
    EXPECT_FLOAT_EQ(data->Points[2].Position.x, 90.0f);
    const std::vector<ECS::EntityHandle> markups = MarkupService::Get().GetMarkups(loaded);
    EXPECT_NE(std::find(markups.begin(), markups.end(), loadedRoad), markups.end())
        << "the loaded path is not in the mark-up registry";
}
