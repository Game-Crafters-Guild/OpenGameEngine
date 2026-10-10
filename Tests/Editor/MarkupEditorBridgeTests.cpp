// MarkupEditorBridge: who an edit to a mark-up is attributed to, the duplicate rule, and
// the viewer's hidden and seen state, which are never scene edits; and the surfaces over
// it: the Mark-ups panel's rows and the inspector section.

#include "Markups/MarkupCommentText.h"
#include "Markups/MarkupEditorBridge.h"
#include "Markups/MarkupInspector.h"
#include "Markups/MarkupPresentation.h"
#include "Markups/MarkupTool.h"
#include "Markups/MarkupsPanel.h"
#include "Markups/MarkupsPanelRows.h"

#include "DebugServer/DebugServerReply.h"

#include "Components/Markup/Markup.h"
#include "Components/Name.h"
#include "Components/SceneEntityTag.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "Editor/Settings/SettingsStore.h"
#include "EditorChangeNotifications.h"
#include "Input/InputSystem.h"
#include "Input/KeyCodes.h"
#include "InspectorRegistry.h"
#include "Logger/CallbackSink.h"
#include "Logger/Logger.h"
#include "MarkupECS/MarkupService.h"
#include "TestEnvVar.h"
#include "TestTempDir.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/InspectorNotice.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/TextArea.h"
#include "UI/Controls/TextField.h"
#include "UI/StyleProperties.h"
#include "UI/UiContext.h"
#include "UI/UiDispatcher.h"
#include "UndoRedo/UndoRedoService.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <cstring>
#include <filesystem>
#include <functional>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

using namespace GameEngine;
using Components::Markup;
using Components::MarkupAuthor;
using Editor::EditorChangeNotifications;
using Editor::MarkupEditorBridge;
using MarkupECS::MarkupService;

namespace
{

// Every element of type T under `root`, depth first.
template <typename T>
void CollectAll(UIElement& root, std::vector<T*>& found)
{
    if (auto* element = dynamic_cast<T*>(&root))
        found.push_back(element);
    for (const auto& child : root.GetChildren())
        CollectAll(*child, found);
}

template <typename T>
std::vector<T*> FindAll(UIElement& root)
{
    std::vector<T*> found;
    CollectAll(root, found);
    return found;
}

// Counts the log lines whose text holds `needle`. The sink outlives the counter (the
// logger owns it); only the callback is unregistered.
class LogLineCounter
{
  public:
    explicit LogLineCounter(std::string needle) : m_Needle(std::move(needle))
    {
        Logger::Log::Initialize({Logger::LogLevel::Debug, false});
        auto sink = Logger::MakeUnique<Logger::CallbackSink>();
        m_Sink = sink.get();
        Logger::Log::AddSink(std::move(sink));
        m_CallbackId = m_Sink->RegisterCallback([this](const Logger::LogMessage& message) {
            if (message.Message.find(m_Needle) != std::string::npos)
                ++m_Count;
        });
    }
    ~LogLineCounter() { m_Sink->UnregisterCallback(m_CallbackId); }

    int Count() const
    {
        Logger::Log::Flush();
        return m_Count.load();
    }

  private:
    std::string m_Needle;
    Logger::CallbackSink* m_Sink = nullptr;
    Logger::uint64 m_CallbackId = 0;
    std::atomic<int> m_Count{0};
};

class MarkupEditorBridgeTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        // The user's project settings go under a scratch user-data root, never the
        // developer's own profile.
        GameEngine::Testing::SetEnvVar("GE_EDITOR_USER_DATA_ROOT", (m_Root.Path() / "UserData").string().c_str());
        m_Project = m_Root.Path() / "Project";
        std::filesystem::create_directories(m_Project);
        m_ScenePath = m_Project / "Scenes" / "Village.scene";
        MarkupService::Initialize();
        StartSession();
    }

    void TearDown() override
    {
        m_Bridge.reset();
        MarkupService::Shutdown();
        GameEngine::Testing::SetEnvVar("GE_EDITOR_USER_DATA_ROOT", "");
    }

    // A new editor session: a new bridge over the same notifications, clock and open scene.
    void StartSession()
    {
        m_Bridge.reset();
        m_Bridge = std::make_unique<MarkupEditorBridge>(
            Notifications, [this]() { return m_Now; }, [this]() { return m_ScenePath; });
    }

    ECS::EntityHandle CreateMarkup()
    {
        const ECS::EntityHandle entity = World.CreateEntity();
        World.AddComponentImmediate(entity, Components::Transform{});
        World.AddComponentImmediate(entity, Markup{});
        (void)MarkupService::Get().BeginMarkup(World, entity, MarkupAuthor::User, m_Now);
        return entity;
    }

    ECS::EntityHandle CreateTaggedMarkup(const std::string& tag)
    {
        const ECS::EntityHandle entity = CreateMarkup();
        Tag(entity, tag);
        return entity;
    }

    // What a scene save does to an untagged entity: it gives it a scene tag.
    void Tag(ECS::EntityHandle entity, const std::string& tag)
    {
        Components::SceneEntityTag sceneTag{};
        std::strncpy(sceneTag.value, tag.c_str(), sizeof(sceneTag.value) - 1);
        World.AddComponentImmediate(entity, sceneTag);
    }

    void Hide(ECS::EntityHandle entity) { m_Bridge->SetHidden(World, {&entity, 1}, true); }

    static void WriteSettings(Editor::SettingsStore store, const char* key, const nlohmann::json& value)
    {
        (void)store.Load();
        store.SetJson(key, value);
        ASSERT_TRUE(store.Save());
    }

    template <typename T>
    void Notify(ECS::EntityHandle entity, EditorChangeNotifications::ChangeKind kind)
    {
        Notifications.NotifyComponentChange<T>(&World, entity, kind);
    }

    const Markup& MarkupOf(ECS::EntityHandle entity) { return *World.GetComponent<Markup>(entity); }

    // A comment the agent writes through the debug port.
    void AgentComment(ECS::EntityHandle entity, const char* text)
    {
        ASSERT_TRUE(MarkupService::Get().AddComment(World, entity, MarkupAuthor::Agent, m_Now, text));
    }

    // The inspector section of component T for `entity`, as the Inspector builds it.
    template <typename T>
    std::unique_ptr<UIElement> BuildSection(ECS::EntityHandle entity, Editor::UndoRedoService& undo,
                                            std::vector<std::function<void()>>* frameCallbacks = nullptr,
                                            std::function<void()> requestRefresh = {})
    {
        Editor::RegisterMarkupInspector(*m_Bridge);
        auto section = std::make_unique<UIElement>();
        InspectorContext ctx;
        ctx.Parent = section.get();
        ctx.World = &World;
        ctx.Entity = entity;
        ctx.Undo = &undo;
        ctx.ChangeNotifications = &Notifications;
        ctx.FrameRefreshCallbacks = frameCallbacks;
        ctx.RequestInspectorRefresh = std::move(requestRefresh);
        (*InspectorRegistry::Get().TryGetComponentInspector<T>())(ctx);
        return section;
    }

    std::unique_ptr<UIElement> BuildInspectorSection(ECS::EntityHandle entity, Editor::UndoRedoService& undo)
    {
        return BuildSection<Markup>(entity, undo);
    }

    // Writes `text` into the section's comment field and presses Add comment.
    static void AddCommentThroughSection(UIElement& section, const std::string& text)
    {
        for (TextArea* area : FindAll<TextArea>(section))
        {
            if (area->HasClass("markup-inspector-comment"))
                area->SetValue(text);
        }
        for (Button* button : FindAll<Button>(section))
        {
            if (button->GetText() == "Add comment")
                button->TriggerClick();
        }
    }

    GameEngine::TestUtils::ScopedTempDir m_Root{GameEngine::TestUtils::MakeUniqueTempDirectory("MarkupBridge")};
    std::filesystem::path m_Project;
    std::optional<std::filesystem::path> m_ScenePath;
    EditorChangeNotifications Notifications;
    ECS::World World;
    int64 m_Now = 1000;
    std::unique_ptr<MarkupEditorBridge> m_Bridge;
};

} // namespace

// A drag previews every frame and commits once: only the commit stamps. Outside a scope
// the user is the author; inside the debug server's request scope, the agent; an undo
// stamps whoever issued it. A change to anything that is not a mark-up stamps nothing.
TEST_F(MarkupEditorBridgeTest, CommitsAndUndoStampTheScopesAuthorAndPreviewsNever)
{
    const ECS::EntityHandle lake = CreateMarkup();
    const uint32 created = MarkupOf(lake).Revision;

    m_Now = 1010;
    Notify<Components::Transform>(lake, EditorChangeNotifications::ChangeKind::Preview);
    EXPECT_EQ(MarkupOf(lake).Revision, created);

    Notify<Components::Transform>(lake, EditorChangeNotifications::ChangeKind::Commit);
    EXPECT_EQ(MarkupOf(lake).UpdatedBy, MarkupAuthor::User);
    EXPECT_EQ(MarkupOf(lake).UpdatedUnix, 1010);
    EXPECT_GT(MarkupOf(lake).Revision, created);

    m_Bridge->BeginAgentRequest();
    m_Now = 1020;
    Notify<Markup>(lake, EditorChangeNotifications::ChangeKind::UndoRedo);
    m_Bridge->EndAgentRequest();
    EXPECT_EQ(MarkupOf(lake).UpdatedBy, MarkupAuthor::Agent);
    EXPECT_EQ(MarkupOf(lake).UpdatedUnix, 1020);

    Notify<Markup>(lake, EditorChangeNotifications::ChangeKind::UndoRedo);
    EXPECT_EQ(MarkupOf(lake).UpdatedBy, MarkupAuthor::User);

    const ECS::EntityHandle prop = World.CreateEntity();
    World.AddComponentImmediate(prop, Components::Transform{});
    const uint32 before = MarkupService::Get().GetRevision(World);
    Notify<Components::Transform>(prop, EditorChangeNotifications::ChangeKind::Commit);
    EXPECT_EQ(MarkupService::Get().GetRevision(World), before);
}

// A duplicate copies the component bytes and nothing copies the notes: the copy gets its
// own thread, one Created entry by whoever duplicated it, and editing one leaves the other.
TEST_F(MarkupEditorBridgeTest, ADuplicatedMarkupStartsItsOwnThread)
{
    const ECS::EntityHandle lake = CreateMarkup();
    ASSERT_TRUE(MarkupService::Get().AddComment(World, lake, MarkupAuthor::User, m_Now, "Keep the shore"));

    const ECS::EntityHandle copy = World.CreateEntity();
    World.AddComponentImmediate(copy, MarkupOf(lake));
    m_Bridge->BeginAgentRequest();
    Notifications.NotifyWorldStructureChanged({&World, EditorChangeNotifications::ChangeKind::Commit});
    m_Bridge->EndAgentRequest();

    const MarkupECS::MarkupNotes* copyNotes = MarkupService::Get().FindNotes(World, copy);
    ASSERT_NE(copyNotes, nullptr);
    ASSERT_EQ(copyNotes->Entries.size(), 1u);
    EXPECT_EQ(copyNotes->Entries[0].Kind, MarkupECS::MarkupEntryKind::Created);
    EXPECT_EQ(copyNotes->Entries[0].Author, MarkupAuthor::Agent);
    EXPECT_EQ(MarkupOf(copy).Author, MarkupAuthor::Agent);
    EXPECT_EQ(MarkupService::Get().FindNotes(World, lake)->Entries.size(), 2u);
}

// Hiding is a view state: the mark-up's component, revision and scene stay as they were.
// An agent's change after the viewer's last look is unseen until the next look; the
// user's own change never is.
TEST_F(MarkupEditorBridgeTest, HidingIsNotASceneEditAndOnlyTheAgentsChangesAreUnseen)
{
    const ECS::EntityHandle lake = CreateMarkup();
    Markup before{};
    std::memcpy(&before, &MarkupOf(lake), sizeof(Markup));
    const uint32 revision = MarkupService::Get().GetRevision(World);

    Hide(lake);
    EXPECT_TRUE(m_Bridge->IsHidden(World, lake));
    EXPECT_EQ(std::memcmp(&before, &MarkupOf(lake), sizeof(Markup)), 0);
    EXPECT_EQ(MarkupService::Get().GetRevision(World), revision);
    m_Bridge->SetHidden(World, {&lake, 1}, false);
    EXPECT_FALSE(m_Bridge->IsHidden(World, lake));

    EXPECT_FALSE(m_Bridge->HasUnseenUpdate(World, lake));
    m_Bridge->MarkSeen(World, lake);
    AgentComment(lake, "On it");
    EXPECT_TRUE(m_Bridge->HasUnseenUpdate(World, lake));
    EXPECT_EQ(m_Bridge->CountUnseen(World), 1u);
    m_Bridge->MarkSeen(World, lake);
    EXPECT_FALSE(m_Bridge->HasUnseenUpdate(World, lake));
    EXPECT_EQ(m_Bridge->CountUnseen(World), 0u); // the cached count follows the seen state
    Notify<Markup>(lake, EditorChangeNotifications::ChangeKind::Commit);
    EXPECT_FALSE(m_Bridge->HasUnseenUpdate(World, lake));
}

// The project keeps its vocabulary's names and colors, and the user keeps which mark-ups
// are hidden: a second editor session on the same project reads both back.
TEST_F(MarkupEditorBridgeTest, TheVocabularyAndTheHiddenSetPersistPerProject)
{
    const ECS::EntityHandle lake = CreateTaggedMarkup("lake");
    m_Bridge->LoadProjectState(m_Project);
    ASSERT_TRUE(MarkupService::Get().RenameTag(Components::kMarkupStatusProblem, "Blocked"));
    const float32 orange[4] = {1.0f, 0.5f, 0.0f, 1.0f};
    ASSERT_TRUE(MarkupService::Get().SetTagColor(Components::kMarkupStatusProblem, orange));
    m_Bridge->SaveVocabulary();
    Hide(lake);

    MarkupService::Shutdown();
    MarkupService::Initialize();
    StartSession();
    m_Bridge->LoadProjectState(m_Project);

    EXPECT_EQ(MarkupService::Get().FindTag("Blocked"), Components::kMarkupStatusProblem);
    EXPECT_EQ(MarkupService::Get().GetTag(Components::kMarkupStatusProblem)->Color[1], 0.5f);
    EXPECT_TRUE(m_Bridge->IsHidden(World, lake));
}

// The agent's mark-up has no scene tag until the first save gives it one. Hiding it holds
// through that save, and the save's completion writes it, so the next session reads it back
// even when the editor quits right after the save.
TEST_F(MarkupEditorBridgeTest, AHiddenMarkupStaysHiddenThroughTheSaveThatTagsIt)
{
    m_Bridge->LoadProjectState(m_Project);
    const ECS::EntityHandle lake = CreateMarkup();
    Hide(lake);
    Tag(lake, "lake");
    EXPECT_TRUE(m_Bridge->IsHidden(World, lake));
    m_Bridge->OnSceneSaved(World);

    StartSession();
    m_Bridge->LoadProjectState(m_Project);
    World.Clear();
    EXPECT_TRUE(m_Bridge->IsHidden(World, CreateTaggedMarkup("lake")));
}

// A duplicate copies the original's scene tag until the next save gives it its own: it
// starts with its own viewer state, and changing it leaves the original's.
TEST_F(MarkupEditorBridgeTest, ADuplicateDoesNotShareTheOriginalsViewerState)
{
    m_Bridge->LoadProjectState(m_Project);
    const ECS::EntityHandle lake = CreateTaggedMarkup("lake");
    Hide(lake);

    const ECS::EntityHandle copy = World.CreateEntity();
    World.AddComponentImmediate(copy, MarkupOf(lake));
    Tag(copy, "lake");
    Notifications.NotifyWorldStructureChanged({&World, EditorChangeNotifications::ChangeKind::Commit});

    EXPECT_FALSE(m_Bridge->IsHidden(World, copy));
    m_Bridge->SetHidden(World, {&copy, 1}, false);
    EXPECT_TRUE(m_Bridge->IsHidden(World, lake));
}

// A duplicate hidden before its first save is written under the tag that save gives it,
// never under the original's tag it copied.
TEST_F(MarkupEditorBridgeTest, ADuplicateHiddenBeforeItsFirstSaveKeepsItsStateOffTheOriginal)
{
    m_Bridge->LoadProjectState(m_Project);
    const ECS::EntityHandle lake = CreateTaggedMarkup("lake");
    const ECS::EntityHandle copy = World.CreateEntity();
    World.AddComponentImmediate(copy, MarkupOf(lake));
    Tag(copy, "lake");
    Notifications.NotifyWorldStructureChanged({&World, EditorChangeNotifications::ChangeKind::Commit});
    Hide(copy);
    Tag(copy, "lake_2");
    m_Bridge->OnSceneSaved(World);
    EXPECT_TRUE(m_Bridge->IsHidden(World, copy));
    EXPECT_FALSE(m_Bridge->IsHidden(World, lake));

    StartSession();
    m_Bridge->LoadProjectState(m_Project);
    World.Clear();
    EXPECT_FALSE(m_Bridge->IsHidden(World, CreateTaggedMarkup("lake")));
    EXPECT_TRUE(m_Bridge->IsHidden(World, CreateTaggedMarkup("lake_2")));
}

// Scene tags are unique within one scene only: two scenes' mark-ups with one tag keep
// their own hidden state, in the session and in the next one.
TEST_F(MarkupEditorBridgeTest, TwoScenesWithTheSameTagKeepTheirOwnState)
{
    m_Bridge->LoadProjectState(m_Project);
    Hide(CreateTaggedMarkup("lake"));

    World.Clear();
    m_ScenePath = m_Project / "Scenes" / "Harbor.scene";
    EXPECT_FALSE(m_Bridge->IsHidden(World, CreateTaggedMarkup("lake")));

    StartSession();
    m_Bridge->LoadProjectState(m_Project);
    World.Clear();
    EXPECT_FALSE(m_Bridge->IsHidden(World, CreateTaggedMarkup("lake")));
    World.Clear();
    m_ScenePath = m_Project / "Scenes" / "Village.scene";
    EXPECT_TRUE(m_Bridge->IsHidden(World, CreateTaggedMarkup("lake")));
}

// Hiding every mark-up is one write of the user's project settings, not one per mark-up.
// The writes are counted by their failures: the user-data root is a file, so each write
// fails and says so once.
TEST_F(MarkupEditorBridgeTest, HidingAllWritesTheSettingsOnce)
{
    const std::filesystem::path notADirectory = m_Root.Path() / "UserDataFile";
    std::ofstream(notADirectory) << "x";
    GameEngine::Testing::SetEnvVar("GE_EDITOR_USER_DATA_ROOT", notADirectory.string().c_str());
    m_Bridge->LoadProjectState(m_Project);
    std::vector<ECS::EntityHandle> all;
    for (int i = 0; i < 50; ++i)
        all.push_back(CreateTaggedMarkup("m" + std::to_string(i)));

    const LogLineCounter writes("could not save the hidden and seen state");
    m_Bridge->SetHidden(World, all, true);
    EXPECT_EQ(writes.Count(), 1);
}

// A settings block not in the form the editor writes is ignored whole, and its defaults
// hold: a wrong type, a tag with no name, a block that is not an object at all.
TEST_F(MarkupEditorBridgeTest, AWrongTypedSettingsBlockLeavesTheDefaults)
{
    WriteSettings(Editor::OpenProjectSettings(m_Project), "markupTags",
                  nlohmann::json::array({{{"name", "Water"}, {"color", {"r", "g", "b", "a"}}}, 7}));
    WriteSettings(Editor::OpenUserProjectSettings(m_Project), "markupViewer", nlohmann::json::array());
    EXPECT_NO_THROW(m_Bridge->LoadProjectState(m_Project));
    EXPECT_EQ(MarkupService::Get().FindTag("Water"), MarkupService::kInvalidTag);
}

TEST_F(MarkupEditorBridgeTest, ATagWithNoNameLeavesTheDefaults)
{
    const float32 violet = MarkupService::Get().GetTag(Components::kMarkupStatusProposed)->Color[0];
    WriteSettings(Editor::OpenProjectSettings(m_Project), "markupTags",
                  nlohmann::json::array({{{"color", {1, 1, 1, 1}}}}));
    EXPECT_NO_THROW(m_Bridge->LoadProjectState(m_Project));
    EXPECT_EQ(MarkupService::Get().GetTag(Components::kMarkupStatusProposed)->Name, "Proposed");
    EXPECT_EQ(MarkupService::Get().GetTag(Components::kMarkupStatusProposed)->Color[0], violet);
}

TEST_F(MarkupEditorBridgeTest, AViewerBlockThatIsNotAnObjectLeavesNothingHidden)
{
    WriteSettings(Editor::OpenUserProjectSettings(m_Project), "markupViewer", "garbage");
    EXPECT_NO_THROW(m_Bridge->LoadProjectState(m_Project));
    EXPECT_FALSE(m_Bridge->IsHidden(World, CreateTaggedMarkup("lake")));
}

// The design keeps mark-up edits in the edit world: the Scene View tool refuses while the
// editor plays, as the markup_* methods do.
TEST_F(MarkupEditorBridgeTest, TheMarkupToolRefusesWhileTheEditorPlays)
{
    bool playing = true;
    m_Bridge->SetPlayModeProvider([&playing]() { return playing; });
    const std::size_t before = MarkupService::Get().GetMarkups(World).size();

    EXPECT_TRUE(Editor::IsRefusal(Editor::PlaceMarkup(*m_Bridge, World, nullptr, &Notifications, {0.0f, 0.0f, 0.0f})));
    EXPECT_EQ(MarkupService::Get().GetMarkups(World).size(), before);

    playing = false;
    EXPECT_FALSE(Editor::IsRefusal(Editor::PlaceMarkup(*m_Bridge, World, nullptr, &Notifications, {0.0f, 0.0f, 0.0f})));
    EXPECT_EQ(MarkupService::Get().GetMarkups(World).size(), before + 1);
}

// The Activity badge's count is cached between changes; an agent's entry, with nothing
// marked seen since, still counts again.
TEST_F(MarkupEditorBridgeTest, TheBadgeCountRisesOnAnAgentEditWithoutAMarkSeen)
{
    const ECS::EntityHandle lake = CreateMarkup();
    m_Bridge->MarkSeen(World, lake);
    EXPECT_EQ(m_Bridge->CountUnseen(World), 0u);
    AgentComment(lake, "On it");
    EXPECT_EQ(m_Bridge->CountUnseen(World), 1u);
}

// The badge counts the agent's entries since the last look, not the mark-ups they are on:
// three agent entries on one mark-up read 3, a look clears them, and the next entry reads 1.
TEST_F(MarkupEditorBridgeTest, TheBadgeCountsTheAgentsEntriesSinceTheLastLook)
{
    const ECS::EntityHandle lake = CreateMarkup();
    m_Bridge->MarkSeen(World, lake);
    AgentComment(lake, "Surveying the shore");
    ASSERT_TRUE(MarkupService::Get().SetStatus(World, lake, Components::kMarkupStatusInProgress, MarkupAuthor::Agent, m_Now));
    AgentComment(lake, "Reeds first");
    EXPECT_EQ(m_Bridge->CountUnseen(World), 3u);
    m_Bridge->MarkSeen(World, lake);
    EXPECT_EQ(m_Bridge->CountUnseen(World), 0u);
    AgentComment(lake, "Done with the reeds");
    EXPECT_EQ(m_Bridge->CountUnseen(World), 1u);
    EXPECT_EQ(m_Bridge->FirstUnseenEntry(World, lake), 4u);
}

// A structure change counts again: a deleted unseen mark-up leaves the count.
TEST_F(MarkupEditorBridgeTest, TheBadgeCountFollowsAStructureChange)
{
    const ECS::EntityHandle lake = CreateMarkup();
    AgentComment(lake, "On it");
    EXPECT_EQ(m_Bridge->CountUnseen(World), 1u);
    World.DestroyEntityImmediate(lake);
    EditorChangeNotifications::WorldStructureChangedEvent removed{};
    removed.world = &World;
    Notifications.NotifyWorldStructureChanged(removed);
    EXPECT_EQ(m_Bridge->CountUnseen(World), 0u);
}

// The Mark-ups tab's rows: mark-ups changed in the same second keep their order by
// revision, and each row's eye hides that row's own mark-up, not another.
TEST_F(MarkupEditorBridgeTest, EachMarkupRowsEyeHidesItsOwnMarkupInRevisionOrder)
{
    const ECS::EntityHandle lake = CreateTaggedMarkup("lake");
    const ECS::EntityHandle forest = CreateTaggedMarkup("forest"); // the same second, a later revision
    std::vector<ECS::EntityHandle> order{lake, forest};
    Editor::SortMarkupsNewestFirst(World, order);
    EXPECT_EQ(order, (std::vector<ECS::EntityHandle>{forest, lake}));

    int changes = 0;
    const Editor::MarkupRowActions actions{{}, [&changes]() { ++changes; }};
    const std::unique_ptr<UIElement> lakeRow = Editor::BuildMarkupRow(*m_Bridge, World, lake, m_Now, false, actions);
    const std::unique_ptr<UIElement> forestRow = Editor::BuildMarkupRow(*m_Bridge, World, forest, m_Now, false, actions);
    FindAll<Button>(*forestRow).front()->TriggerClick();
    EXPECT_TRUE(m_Bridge->IsHidden(World, forest));
    EXPECT_FALSE(m_Bridge->IsHidden(World, lake));
    EXPECT_EQ(changes, 1);
}

// The Activity tab lists newest first; a status change and the comment the agent wrote in
// the same second list the comment, the later entry, on top.
TEST_F(MarkupEditorBridgeTest, SameSecondActivityListsTheLaterEntryFirst)
{
    const ECS::EntityHandle lake = CreateMarkup();
    m_Now = 1100;
    ASSERT_TRUE(MarkupService::Get().SetStatus(World, lake, Components::kMarkupStatusInProgress, MarkupAuthor::Agent, m_Now));
    AgentComment(lake, "Walls first, then the gate.");
    const auto& entries = MarkupService::Get().FindNotes(World, lake)->Entries;
    ASSERT_EQ(entries.size(), 3u);

    std::vector<Editor::MarkupActivityItem> items;
    for (std::size_t index = 0; index < entries.size(); ++index)
        items.push_back({lake, &entries[index], index});
    Editor::SortActivityNewestFirst(items);
    EXPECT_EQ(items[0].Entry->Text, "Walls first, then the gate.");
    EXPECT_EQ(items[1].Entry->Kind, MarkupECS::MarkupEntryKind::StatusChange);
    EXPECT_EQ(items[2].Entry->Kind, MarkupECS::MarkupEntryKind::Created);
}

// Both tabs filter through one predicate: the search on the title, the status filter on the
// mark-up's current status, so a filtered Activity tab lists only that status's entries.
TEST_F(MarkupEditorBridgeTest, TheSearchAndTheStatusFilterSelectTheSameMarkupsOnBothTabs)
{
    const ECS::EntityHandle lake = CreateMarkup();
    const ECS::EntityHandle forest = CreateMarkup();
    ASSERT_TRUE(MarkupService::Get().SetStatus(World, forest, Components::kMarkupStatusProposed, MarkupAuthor::Agent, m_Now));
    const std::string proposed = std::to_string(Components::kMarkupStatusProposed);

    EXPECT_TRUE(Editor::MarkupMatchesFilters(World, lake, "", ""));
    EXPECT_FALSE(Editor::MarkupMatchesFilters(World, lake, "", proposed));
    EXPECT_TRUE(Editor::MarkupMatchesFilters(World, forest, "", proposed));
    EXPECT_TRUE(Editor::MarkupMatchesFilters(World, forest, "mark", proposed)); // "Mark-up", any case
    EXPECT_FALSE(Editor::MarkupMatchesFilters(World, forest, "zzz", proposed));
}

// An undo that shortens the thread below its length at the viewer's last look moves the
// unseen boundary down with it: the agent's next entry counts.
TEST_F(MarkupEditorBridgeTest, AnAgentEntryAfterAnAgentUndoCounts)
{
    const ECS::EntityHandle lake = CreateMarkup();
    Editor::UndoRedoService undo;
    const std::unique_ptr<UIElement> section = BuildInspectorSection(lake, undo);
    m_Bridge->BeginAgentRequest();
    AddCommentThroughSection(*section, "Reeds first");
    m_Bridge->EndAgentRequest();
    m_Bridge->MarkSeen(World, lake);
    EXPECT_EQ(m_Bridge->CountUnseen(World), 0u);

    m_Bridge->BeginAgentRequest();
    undo.Undo();
    m_Bridge->EndAgentRequest();
    AgentComment(lake, "Shore first after all");
    EXPECT_TRUE(m_Bridge->HasUnseenUpdate(World, lake));
    EXPECT_EQ(m_Bridge->CountUnseen(World), 1u);
}

// A Mark-ups panel no UI manager reaches (its dock tab inactive) still rebuilds after each
// change: every change posts a rebuild while none is pending, and a rebuild clears that.
TEST_F(MarkupEditorBridgeTest, APanelOnAnInactiveTabRebuildsAfterEveryChange)
{
    const ECS::EntityHandle lake = CreateMarkup();
    UI::UiDispatcher dispatcher;
    UI::UiContextScope scope{&dispatcher, nullptr};
    const auto panel = std::make_unique<Editor::MarkupsPanel>(*m_Bridge);

    Notify<Markup>(lake, EditorChangeNotifications::ChangeKind::Commit);
    EXPECT_EQ(dispatcher.PendingCount(), 1u);
    dispatcher.Drain();
    Notify<Markup>(lake, EditorChangeNotifications::ChangeKind::Commit);
    EXPECT_EQ(dispatcher.PendingCount(), 1u);
}

// A rebuild removes the rows without a leave event, so it drops the hovered row's highlight;
// otherwise a list that shrank under a still pointer would keep a volume highlighted.
TEST_F(MarkupEditorBridgeTest, APanelRebuildDropsTheHoveredRowsHighlight)
{
    const ECS::EntityHandle lake = CreateMarkup();
    UI::UiDispatcher dispatcher;
    UI::UiContextScope scope{&dispatcher, nullptr};
    const auto panel = std::make_unique<Editor::MarkupsPanel>(*m_Bridge);

    m_Bridge->HoverMarkup(lake);
    ASSERT_EQ(m_Bridge->GetHighlight().Hovered, lake);
    Notify<Markup>(lake, EditorChangeNotifications::ChangeKind::Commit);
    dispatcher.Drain();
    EXPECT_FALSE(m_Bridge->GetHighlight().Hovered.IsValid());
}

// A Hierarchy row's hover reaches the mark-up through the Scene View's hover, as its panel
// row's does; with no Scene View hover the panel row's stands.
TEST_F(MarkupEditorBridgeTest, TheSceneViewsHoverHighlightsAMarkupAsItsPanelRowDoes)
{
    const ECS::EntityHandle lake = CreateMarkup();
    const ECS::EntityHandle forest = CreateMarkup();
    EXPECT_EQ(Editor::MarkupHoveredEntity(lake, {}), lake);
    EXPECT_EQ(Editor::MarkupHoveredEntity(lake, forest), lake);
    EXPECT_EQ(Editor::MarkupHoveredEntity({}, forest), forest);
}

// The row of a selected mark-up shows as selected, and its unseen dot stays its own state.
TEST_F(MarkupEditorBridgeTest, ASelectedMarkupsRowShowsAsSelectedAndKeepsItsUnseenDot)
{
    const ECS::EntityHandle lake = CreateMarkup();
    AgentComment(lake, "On it");
    ASSERT_TRUE(m_Bridge->HasUnseenUpdate(World, lake));
    const std::unique_ptr<UIElement> selected = Editor::BuildMarkupRow(*m_Bridge, World, lake, m_Now, true, {});
    const std::unique_ptr<UIElement> other = Editor::BuildMarkupRow(*m_Bridge, World, lake, m_Now, false, {});
    EXPECT_TRUE(selected->HasClass("selected"));
    EXPECT_FALSE(other->HasClass("selected"));
    const auto unseenDot = [](const UIElement& row) {
        for (const auto& child : row.GetChildren())
        {
            if (child->HasClass("markups-update-dot"))
                return !child->HasClass("markups-update-dot-seen");
        }
        return false;
    };
    EXPECT_TRUE(unseenDot(*selected));
}

// A status the vocabulary does not hold shows as an "Unknown" pill styled by the theme's
// class, with no fill set from code.
TEST_F(MarkupEditorBridgeTest, AnUnknownStatusPillTakesTheThemesClass)
{
    const ECS::EntityHandle lake = CreateMarkup();
    Markup unknown = MarkupOf(lake);
    unknown.Status = 99;
    World.AddComponentImmediate(lake, unknown);
    const std::unique_ptr<UIElement> row = Editor::BuildMarkupRow(*m_Bridge, World, lake, m_Now, false, {});
    const UIElement& pill = *row->GetChildren().front();
    EXPECT_TRUE(pill.HasClass("markups-status-pill-unknown"));
    EXPECT_EQ(static_cast<const Label&>(pill).GetText(), "Unknown");
    EXPECT_FALSE(pill.Overrides().Get(Style::BackgroundColor).has_value());
}

// The inspector section's Add comment is one undo step, made for the user: undo takes the
// comment out again and redo puts it back.
TEST_F(MarkupEditorBridgeTest, TheInspectorSectionCommitsAnAddedCommentAsOneUndoStep)
{
    const ECS::EntityHandle lake = CreateMarkup();
    Editor::UndoRedoService undo;
    const std::unique_ptr<UIElement> section = BuildInspectorSection(lake, undo);

    AddCommentThroughSection(*section, "Keep the shore");
    const auto& entries = MarkupService::Get().FindNotes(World, lake)->Entries;
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries.back().Text, "Keep the shore");
    EXPECT_EQ(entries.back().Author, MarkupAuthor::User);

    undo.Undo();
    EXPECT_EQ(MarkupService::Get().FindNotes(World, lake)->Entries.size(), 1u);
    undo.Redo();
    EXPECT_EQ(MarkupService::Get().FindNotes(World, lake)->Entries.size(), 2u);
    EXPECT_FALSE(undo.CanRedo());
    undo.Undo();
    undo.Undo();
    EXPECT_EQ(MarkupService::Get().FindNotes(World, lake)->Entries.size(), 1u); // no second step under it
}

// Mark-ups are edited in the edit world: in play mode the section shows the tool's notice
// and an edit through it changes nothing.
TEST_F(MarkupEditorBridgeTest, TheInspectorSectionIsReadOnlyInPlay)
{
    m_Bridge->SetPlayModeProvider([]() { return true; });
    const ECS::EntityHandle lake = CreateMarkup();
    Editor::UndoRedoService undo;
    const std::unique_ptr<UIElement> section = BuildInspectorSection(lake, undo);

    EXPECT_EQ(FindAll<EditorUI::InspectorNotice>(*section).size(), 1u);
    AddCommentThroughSection(*section, "Keep the shore");
    EXPECT_EQ(MarkupService::Get().FindNotes(World, lake)->Entries.size(), 1u);
    EXPECT_FALSE(undo.CanUndo());
    for (Button* button : FindAll<Button>(*section))
    {
        if (button->GetText() == "Add comment")
            EXPECT_FALSE(button->IsEnabled());
    }
}

// A mark-up's volume is part of the mark-up: in play mode its shape refuses a change too.
TEST_F(MarkupEditorBridgeTest, TheVolumeShapeIsRefusedInPlay)
{
    bool playing = true;
    m_Bridge->SetPlayModeProvider([&playing]() { return playing; });
    const ECS::EntityHandle lake = CreateMarkup();
    World.AddComponentImmediate(lake, Components::MarkupVolume{});
    Editor::UndoRedoService undo;
    const std::unique_ptr<UIElement> section = BuildSection<Components::MarkupVolume>(lake, undo);

    FindAll<Dropdown>(*section).front()->SetSelectedIndex(static_cast<int>(Components::MarkupVolumeShape::Sphere));
    EXPECT_EQ(World.GetComponent<Components::MarkupVolume>(lake)->Shape, Components::MarkupVolumeShape::Box);
    EXPECT_FALSE(undo.CanUndo());

    playing = false;
    const std::unique_ptr<UIElement> editable = BuildSection<Components::MarkupVolume>(lake, undo);
    FindAll<Dropdown>(*editable).front()->SetSelectedIndex(static_cast<int>(Components::MarkupVolumeShape::Sphere));
    EXPECT_EQ(World.GetComponent<Components::MarkupVolume>(lake)->Shape, Components::MarkupVolumeShape::Sphere);
}

// A section open across a play-mode switch asks the inspector for one rebuild, however many
// frames pass before the rebuild comes.
TEST_F(MarkupEditorBridgeTest, APlayModeSwitchRebuildsTheOpenSectionOnce)
{
    bool playing = false;
    m_Bridge->SetPlayModeProvider([&playing]() { return playing; });
    const ECS::EntityHandle lake = CreateMarkup();
    Editor::UndoRedoService undo;
    std::vector<std::function<void()>> frameCallbacks;
    int refreshes = 0;
    UI::UiDispatcher dispatcher;
    UI::UiContextScope scope{&dispatcher, nullptr};
    const std::unique_ptr<UIElement> section =
        BuildSection<Markup>(lake, undo, &frameCallbacks, [&refreshes]() { ++refreshes; });

    const auto runFrame = [&frameCallbacks, &dispatcher]() {
        for (const std::function<void()>& callback : frameCallbacks)
            callback();
        dispatcher.Drain();
    };
    runFrame();
    EXPECT_EQ(refreshes, 0);
    playing = true;
    runFrame();
    runFrame();
    EXPECT_EQ(refreshes, 1);
}

// A committed move, resize, rename or new color of a mark-up is an edit in its thread by the
// current author, read against its state at the last structure change; a drag's Previews
// record nothing until its Commit, and the agent's edits over the port are the agent's.
TEST_F(MarkupEditorBridgeTest, ACommittedMoveRenameOrColorIsRecordedInTheThread)
{
    using MarkupECS::MarkupEditChange;
    using MarkupECS::MarkupEditChanges;
    const ECS::EntityHandle docks = CreateMarkup();
    EditorChangeNotifications::WorldStructureChangedEvent loaded;
    loaded.world = &World;
    loaded.kind = EditorChangeNotifications::ChangeKind::Commit;
    Notifications.NotifyWorldStructureChanged(loaded);
    const auto& entries = MarkupService::Get().FindNotes(World, docks)->Entries;
    ASSERT_EQ(entries.size(), 1u);

    Components::Transform moved;
    moved.matrix[12] = 5.0f;
    moved.matrix[0] = 2.0f;
    World.AddComponentImmediate(docks, moved);
    m_Now = 1010;
    Notify<Components::Transform>(docks, EditorChangeNotifications::ChangeKind::Preview);
    EXPECT_EQ(entries.size(), 1u);
    Notify<Components::Transform>(docks, EditorChangeNotifications::ChangeKind::Commit);
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[1].Kind, MarkupECS::MarkupEntryKind::Edit);
    EXPECT_EQ(entries[1].Author, MarkupAuthor::User);
    EXPECT_EQ(entries[1].Changes, MarkupEditChanges({MarkupEditChange::Moved, MarkupEditChange::Resized}));

    // The same state committed again (an inspector refresh) is no edit.
    Notify<Components::Transform>(docks, EditorChangeNotifications::ChangeKind::Commit);
    EXPECT_EQ(entries[1].Changes, MarkupEditChanges({MarkupEditChange::Moved, MarkupEditChange::Resized}));

    Markup recolored = MarkupOf(docks);
    recolored.Color[0] = 1.0f;
    recolored.Color[3] = 1.0f;
    World.AddComponentImmediate(docks, recolored);
    m_Bridge->BeginAgentRequest();
    Notify<Markup>(docks, EditorChangeNotifications::ChangeKind::UndoRedo);
    m_Bridge->EndAgentRequest();
    ASSERT_EQ(entries.size(), 3u);
    EXPECT_EQ(entries[2].Author, MarkupAuthor::Agent);
    EXPECT_EQ(entries[2].Changes, MarkupEditChanges({MarkupEditChange::Recolored}));
}

// The Activity tab and the inspector's Thread style an entry by its kind alike: an action as
// one muted line, a comment as a message; Activity rows alternate their plate.
TEST_F(MarkupEditorBridgeTest, ActionsAndCommentsReadApartInTheActivityTabAndTheThread)
{
    const ECS::EntityHandle docks = CreateMarkup();
    AgentComment(docks, "Piers go here");
    const auto& entries = MarkupService::Get().FindNotes(World, docks)->Entries;
    const Editor::MarkupActivityItem created{docks, &entries[0], 0};
    const Editor::MarkupActivityItem comment{docks, &entries[1], 1};
    const std::unique_ptr<UIElement> createdRow = Editor::BuildActivityRow(*m_Bridge, World, created, m_Now, false, {});
    const std::unique_ptr<UIElement> commentRow = Editor::BuildActivityRow(*m_Bridge, World, comment, m_Now, true, {});
    EXPECT_TRUE(createdRow->HasClass("markup-entry-action"));
    EXPECT_TRUE(createdRow->HasClass("markup-entry-created"));
    EXPECT_FALSE(createdRow->HasClass("markups-activity-row-alt"));
    EXPECT_FALSE(commentRow->HasClass("markup-entry-action"));
    EXPECT_TRUE(commentRow->HasClass("markup-entry-comment"));
    EXPECT_TRUE(commentRow->HasClass("markups-activity-row-alt"));
    EXPECT_EQ(FindAll<Label>(*commentRow)[1]->GetText(), "Agent");
    EXPECT_TRUE(FindAll<Label>(*commentRow)[1]->HasClass("markups-activity-author"));

    Editor::UndoRedoService undo;
    const std::unique_ptr<UIElement> section = BuildInspectorSection(docks, undo);
    std::vector<const UIElement*> thread;
    for (const UIElement* element : FindAll<UIElement>(*section))
    {
        if (element->HasClass("markup-inspector-entry"))
            thread.push_back(element);
    }
    ASSERT_EQ(thread.size(), 2u);
    EXPECT_TRUE(thread[0]->HasClass("markup-entry-action"));
    EXPECT_EQ(thread[0]->GetChildren().size(), 1u);
    EXPECT_FALSE(thread[1]->HasClass("markup-entry-action"));
    ASSERT_EQ(thread[1]->GetChildren().size(), 2u);
    EXPECT_EQ(static_cast<const Label&>(*thread[1]->GetChildren()[1]).GetText(), "Piers go here");
}

// The inspector's color row shows the status color until a color is set, and offers the way
// back to it once one is.
TEST_F(MarkupEditorBridgeTest, TheColorRowShowsTheStatusColorUntilOneIsSet)
{
    const ECS::EntityHandle docks = CreateMarkup();
    const auto statusText = [&](UIElement& section) {
        for (Label* label : FindAll<Label>(section))
        {
            if (label->GetText() == "Status color")
                return true;
        }
        return false;
    };
    const auto resetButton = [&](UIElement& section) -> Button* {
        for (Button* button : FindAll<Button>(section))
        {
            if (button->HasClass("markup-inspector-color-reset"))
                return button;
        }
        return nullptr;
    };
    Editor::UndoRedoService undo;
    const std::unique_ptr<UIElement> resting = BuildInspectorSection(docks, undo);
    EXPECT_TRUE(statusText(*resting));
    EXPECT_EQ(resetButton(*resting), nullptr);

    Markup colored = MarkupOf(docks);
    Editor::SetMarkupColorArgb(colored, 0xFF336699u);
    World.AddComponentImmediate(docks, colored);
    const std::unique_ptr<UIElement> custom = BuildInspectorSection(docks, undo);
    EXPECT_FALSE(statusText(*custom));
    bool hex = false;
    for (Label* label : FindAll<Label>(*custom))
        hex = hex || label->GetText() == "#336699";
    EXPECT_TRUE(hex);
    Button* reset = resetButton(*custom);
    ASSERT_NE(reset, nullptr);
    reset->TriggerClick();
    EXPECT_FALSE(Editor::MarkupHasOwnColor(MarkupOf(docks)));
    undo.Undo();
    EXPECT_TRUE(Editor::MarkupHasOwnColor(MarkupOf(docks)));
}

// A Markup removed and added again on the same entity starts from its own state: the state it had
// before the removal is not the baseline an edit is read against.
TEST_F(MarkupEditorBridgeTest, AMarkupAddedAgainRecordsNoEditAgainstItsOldState)
{
    const ECS::EntityHandle docks = CreateMarkup();
    EditorChangeNotifications::WorldStructureChangedEvent structure;
    structure.world = &World;
    structure.kind = EditorChangeNotifications::ChangeKind::Commit;
    Notifications.NotifyWorldStructureChanged(structure);

    World.RemoveComponentImmediate<Markup>(docks);
    Notifications.NotifyWorldStructureChanged(structure);
    Components::Transform moved;
    moved.matrix[12] = 9.0f;
    World.AddComponentImmediate(docks, moved);
    World.AddComponentImmediate(docks, Markup{});
    const std::size_t before = MarkupService::Get().FindNotes(World, docks)->Entries.size();
    Notify<Components::Transform>(docks, EditorChangeNotifications::ChangeKind::Commit);
    const auto& entries = MarkupService::Get().FindNotes(World, docks)->Entries;
    ASSERT_EQ(entries.size(), before);
    EXPECT_NE(entries.back().Kind, MarkupECS::MarkupEntryKind::Edit);
}

// An Activity row and a Thread action carry the kind's marker; a status change's marker takes the
// status's color, so a status change reads apart from an edit at a glance.
TEST_F(MarkupEditorBridgeTest, EveryActionCarriesItsKindsMarker)
{
    const ECS::EntityHandle docks = CreateMarkup();
    ASSERT_TRUE(MarkupService::Get().SetStatus(World, docks, Components::kMarkupStatusRequested, MarkupAuthor::User, m_Now));
    const auto& entries = MarkupService::Get().FindNotes(World, docks)->Entries;
    const Editor::MarkupActivityItem status{docks, &entries[1], 1};
    const std::unique_ptr<UIElement> row = Editor::BuildActivityRow(*m_Bridge, World, status, m_Now, false, {});
    UIElement* marker = nullptr;
    for (UIElement* element : FindAll<UIElement>(*row))
    {
        if (element->HasClass("markup-entry-marker"))
            marker = element;
    }
    ASSERT_NE(marker, nullptr);
    EXPECT_TRUE(marker->HasClass("markup-entry-status"));
    EXPECT_EQ(marker->Overrides().Get(Style::BackgroundColor), Editor::MarkupTagArgb(Components::kMarkupStatusRequested));
}

// The comment field is a full-width text area under the Thread, so a comment keeps its line
// breaks.
TEST_F(MarkupEditorBridgeTest, ACommentKeepsItsLineBreaks)
{
    const ECS::EntityHandle lake = CreateMarkup();
    Editor::UndoRedoService undo;
    const std::unique_ptr<UIElement> section = BuildInspectorSection(lake, undo);
    AddCommentThroughSection(*section, "Reeds first\nthen the path");
    const auto& entries = MarkupService::Get().FindNotes(World, lake)->Entries;
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[1].Text, "Reeds first\nthen the path");
}

// A comment carries its own marker, at the action rows' marker column, in the Activity tab and
// the Thread.
TEST_F(MarkupEditorBridgeTest, ACommentCarriesTheCommentMarker)
{
    const ECS::EntityHandle docks = CreateMarkup();
    AgentComment(docks, "Piers go here");
    const auto& entries = MarkupService::Get().FindNotes(World, docks)->Entries;
    const Editor::MarkupActivityItem comment{docks, &entries[1], 1};
    const std::unique_ptr<UIElement> row = Editor::BuildActivityRow(*m_Bridge, World, comment, m_Now, false, {});
    Editor::UndoRedoService undo;
    const std::unique_ptr<UIElement> section = BuildInspectorSection(docks, undo);
    for (UIElement* root : {row.get(), section.get()})
    {
        int markers = 0;
        for (UIElement* element : FindAll<UIElement>(*root))
            markers += element->HasClass("markup-entry-marker") && element->HasClass("markup-entry-comment") ? 1 : 0;
        EXPECT_EQ(markers, 1);
    }
}

// The empty comment field shows its hint; typing hides it.
TEST_F(MarkupEditorBridgeTest, TheEmptyCommentFieldShowsItsHintUntilTypedIn)
{
    const ECS::EntityHandle lake = CreateMarkup();
    Editor::UndoRedoService undo;
    const std::unique_ptr<UIElement> section = BuildInspectorSection(lake, undo);
    Label* hint = nullptr;
    TextArea* field = nullptr;
    for (Label* label : FindAll<Label>(*section))
        hint = label->HasClass("markup-inspector-comment-placeholder") ? label : hint;
    for (TextArea* area : FindAll<TextArea>(*section))
        field = area->HasClass("markup-inspector-comment") ? area : field;
    ASSERT_NE(hint, nullptr);
    ASSERT_NE(field, nullptr);
    EXPECT_EQ(hint->GetText(), "Add a comment: Enter sends, Shift+Enter for a new line");
    EXPECT_FALSE(hint->HasClass("hidden"));
    field->OnChar(static_cast<unsigned int>('R'));
    EXPECT_TRUE(hint->HasClass("hidden"));
}

// A comment's web links and entity links are cut out of its text: a link ends before a space and
// before the sentence's closing punctuation; a token that is not "[[entity:N]]" stays text.
TEST_F(MarkupEditorBridgeTest, ACommentsLinksAndEntityLinksAreReadFromItsText)
{
    using Kind = Editor::MarkupCommentPiece::Kind;
    const auto pieces =
        Editor::ParseMarkupComment("See https://example.com/a?b=1. Then [[entity:42]] and [[entity:x]] too");
    ASSERT_EQ(pieces.size(), 5u);
    EXPECT_EQ(pieces[0].Type, Kind::Text);
    EXPECT_EQ(pieces[0].Text, "See ");
    EXPECT_EQ(pieces[1].Type, Kind::Url);
    EXPECT_EQ(pieces[1].Text, "https://example.com/a?b=1");
    EXPECT_EQ(pieces[2].Text, ". Then ");
    EXPECT_EQ(pieces[3].Type, Kind::Entity);
    EXPECT_EQ(pieces[3].EntityId, 42u);
    EXPECT_EQ(pieces[4].Type, Kind::Text);
    EXPECT_EQ(pieces[4].Text, " and [[entity:x]] too");
    EXPECT_EQ(Editor::ParseMarkupComment("no links here").size(), 1u);
}

// An entity link shows the entity's name as it is now; one whose entity is gone says so.
TEST_F(MarkupEditorBridgeTest, AnEntityLinkShowsTheEntitysNameNowOrThatItIsGone)
{
    const ECS::EntityHandle cathedral = CreateMarkup();
    Components::Name name{};
    std::strncpy(name.value, "Cathedral", sizeof(name.value) - 1);
    World.AddComponentImmediate(cathedral, name);
    const std::string text = "Next to [[entity:" + std::to_string(cathedral.id) + "]] please";
    const std::unique_ptr<UIElement> body = Editor::BuildMarkupCommentText(text, "body", *m_Bridge, World, {});
    std::vector<std::string> chips;
    for (Label* label : FindAll<Label>(*body))
    {
        if (label->HasClass("entity-link-name"))
            chips.push_back(label->GetText());
    }
    EXPECT_EQ(chips, std::vector<std::string>{"Cathedral"});

    World.DestroyEntityImmediate(cathedral);
    const std::unique_ptr<UIElement> gone = Editor::BuildMarkupCommentText(text, "body", *m_Bridge, World, {});
    bool missing = false;
    for (Label* label : FindAll<Label>(*gone))
        missing = missing || (label->HasClass("entity-link-missing") && label->GetText() == "deleted entity");
    EXPECT_TRUE(missing);
}

// Enter in the inspector's comment field adds the comment as one undo step; Shift+Enter puts a
// line break in it first.
TEST_F(MarkupEditorBridgeTest, EnterAddsTheCommentAndShiftEnterBreaksItsLine)
{
    const ECS::EntityHandle lake = CreateMarkup();
    Editor::UndoRedoService undo;
    const std::unique_ptr<UIElement> section = BuildInspectorSection(lake, undo);
    TextArea* comment = nullptr;
    for (TextArea* area : FindAll<TextArea>(*section))
        comment = area->HasClass("markup-inspector-comment") ? area : comment;
    ASSERT_NE(comment, nullptr);
    comment->SetValue("Reeds first");
    comment->SetSelection(11, 11);
    ASSERT_TRUE(comment->OnKey(Input::kKeyCode_Enter, Input::kModShift, nullptr));
    comment->OnChar(static_cast<unsigned int>('t'));
    comment->OnChar(static_cast<unsigned int>('o'));
    EXPECT_EQ(MarkupService::Get().FindNotes(World, lake)->Entries.size(), 1u);
    ASSERT_TRUE(comment->OnKey(Input::kKeyCode_Enter, 0, nullptr));
    const auto& entries = MarkupService::Get().FindNotes(World, lake)->Entries;
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[1].Text, "Reeds first\nto");
    EXPECT_EQ(entries[1].Author, MarkupAuthor::User) << "a comment sent from the field is the user's";
    // Inside a debug-port request (the agent's scope) the same send is the agent's: the author
    // is the scope's, so an injected key that is handled inside its request is stamped Agent.
    comment->SetValue("From the port");
    m_Bridge->BeginAgentRequest();
    ASSERT_TRUE(comment->OnKey(Input::kKeyCode_Enter, 0, nullptr));
    m_Bridge->EndAgentRequest();
    ASSERT_EQ(entries.size(), 3u);
    EXPECT_EQ(entries[2].Author, MarkupAuthor::Agent);
    undo.Undo();
    undo.Undo();
    EXPECT_EQ(MarkupService::Get().FindNotes(World, lake)->Entries.size(), 1u);
}

// A recolor names the color it gave: the line reads "changed its color to" and the row and the
// Thread show the color's square; a recolor back to the status color stays "changed its color".
TEST_F(MarkupEditorBridgeTest, ARecolorShowsTheColorItGave)
{
    using MarkupECS::MarkupEditChange;
    using MarkupECS::MarkupEditChanges;
    const ECS::EntityHandle mill = CreateMarkup();
    Markup colored = MarkupOf(mill);
    Editor::SetMarkupColorArgb(colored, 0xFFF28C26u);
    World.AddComponentImmediate(mill, colored);
    ASSERT_TRUE(MarkupService::Get().RecordEdit(World, mill, MarkupEditChanges({MarkupEditChange::Recolored}),
                                                MarkupAuthor::Agent, m_Now));
    const auto& entries = MarkupService::Get().FindNotes(World, mill)->Entries;
    EXPECT_EQ(entries.back().ColorArgb, 0xFFF28C26u);
    EXPECT_EQ(Editor::MarkupEntryText(entries.back()), "changed its color to");
    const Editor::MarkupActivityItem item{mill, &entries.back(), entries.size() - 1};
    const std::unique_ptr<UIElement> row = Editor::BuildActivityRow(*m_Bridge, World, item, m_Now, false, {});
    int swatches = 0;
    for (UIElement* element : FindAll<UIElement>(*row))
        swatches += element->HasClass("markup-entry-color") ? 1 : 0;
    EXPECT_EQ(swatches, 1);

    MarkupECS::MarkupEntry statusColor = entries.back();
    statusColor.ColorArgb = 0;
    EXPECT_EQ(Editor::MarkupEntryText(statusColor), "changed its color");
}

// A comment holding a link is one row in the text's class; its words take the word class, so the
// row's column sizing (an Activity row's text grows to fill) is never given to each word.
TEST_F(MarkupEditorBridgeTest, ALinkedCommentsWordsDoNotTakeTheRowsTextClass)
{
    const std::unique_ptr<UIElement> body = Editor::BuildMarkupCommentText(
        "The square follows https://example.com/green today", "markups-entry-text", *m_Bridge, World, {});
    EXPECT_TRUE(body->HasClass("markups-entry-text"));
    int words = 0;
    for (const auto& child : body->GetChildren())
    {
        EXPECT_FALSE(child->HasClass("markups-entry-text"));
        words += child->HasClass("markup-comment-word") ? 1 : 0;
    }
    EXPECT_EQ(words, 5); // "The ", "square ", "follows ", the space after the link, "today"
}

// Hovering an entity link highlights the entity as its panel row's hover does; leaving clears it.
TEST_F(MarkupEditorBridgeTest, HoveringAnEntityLinkHighlightsTheEntity)
{
    const ECS::EntityHandle cathedral = CreateMarkup();
    const std::string text = "See [[entity:" + std::to_string(cathedral.id) + "]]";
    const std::unique_ptr<UIElement> body = Editor::BuildMarkupCommentText(text, "body", *m_Bridge, World, {});
    UIElement* chip = nullptr;
    for (UIElement* element : FindAll<UIElement>(*body))
        chip = element->HasClass("entity-link") ? element : chip;
    ASSERT_NE(chip, nullptr);
    UIEvent enter;
    enter.Id = kEventMouseEnter;
    chip->DispatchEvent(enter);
    EXPECT_EQ(m_Bridge->GetHighlight().Hovered, cathedral);
    UIEvent leave;
    leave.Id = kEventMouseLeave;
    chip->DispatchEvent(leave);
    EXPECT_FALSE(m_Bridge->GetHighlight().Hovered.IsValid());
}

// An entity chip is the entity's name, whose click selects, and a frame glyph at its end, whose
// click frames: a single click never has to wait to tell a select from a frame.
TEST_F(MarkupEditorBridgeTest, AnEntityChipSelectsFromItsNameAndFramesFromItsGlyph)
{
    const ECS::EntityHandle cathedral = CreateMarkup();
    const std::string text = "See [[entity:" + std::to_string(cathedral.id) + "]]";
    std::vector<std::pair<ECS::EntityHandle, bool>> selects;
    const std::unique_ptr<UIElement> body = Editor::BuildMarkupCommentText(
        text, "body", *m_Bridge, World,
        [&selects](ECS::EntityHandle entity, bool frame) { selects.emplace_back(entity, frame); });
    UIElement* chip = nullptr;
    for (UIElement* element : FindAll<UIElement>(*body))
        chip = element->HasClass("entity-link") ? element : chip;
    ASSERT_NE(chip, nullptr);
    ASSERT_EQ(chip->GetChildren().size(), 2u);
    UIElement& name = *chip->GetChildren()[0];
    UIElement& frame = *chip->GetChildren()[1];
    EXPECT_TRUE(name.HasClass("entity-link-name"));
    EXPECT_TRUE(frame.HasClass("entity-link-frame"));
    EXPECT_EQ(frame.GetTooltip(), "Frame in Scene View");
    UIEvent release;
    release.Id = kEventMouseUp;
    release.Button = 0;
    name.DispatchEvent(release);
    UIEvent glyphRelease;
    glyphRelease.Id = kEventMouseUp;
    glyphRelease.Button = 0;
    frame.DispatchEvent(glyphRelease);
    const std::vector<std::pair<ECS::EntityHandle, bool>> expected{{cathedral, false}, {cathedral, true}};
    EXPECT_EQ(selects, expected);
}
