// The agent's mark-up methods (Markups/MarkupRequests.h), one test per method, run the
// way the debug server runs them: inside the bridge's agent scope.

#include "Markups/MarkupRequests.h"

#include "Components/Hierarchy.h"
#include "Components/Markup/Markup.h"
#include "Components/Name.h"
#include "Components/Spline/SplineComponent.h"
#include "Components/Terrain/Terrain.h"
#include "Components/Transform.h"
#include "Core/Application.h"
#include "Core/Engine.h"
#include "DebugServer/DebugServerReply.h"
#include "Editor/Entities/EntityDuplicate.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "EditorChangeNotifications.h"
#include "MarkupECS/MarkupRegionArea.h"
#include "MarkupECS/MarkupService.h"
#include "Markups/MarkupEditorBridge.h"
#include "SceneView/SceneViewFraming.h"
#include "SceneViewController.h"
#include "Scripting/ScriptsConfig.h"
#include "SplineECS/SplineService.h"
#include "Terrain/TerrainTypes.h"
#include "TerrainECS/TerrainService.h"
#include "UndoRedo/DeleteEntitiesCommand.h"
#include "UndoRedo/UndoRedoService.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <filesystem>
#include <optional>
#include <utility>
#include <vector>

using namespace GameEngine;
using Components::Markup;
using Components::MarkupAuthor;
using Editor::IsRefusal;
using Editor::MarkupEditorBridge;
using MarkupECS::MarkupService;
using nlohmann::json;

namespace
{

class MarkupRequestsTest : public ::testing::Test
{
  protected:
    // A path stands on the terrain through the engine's scene picking.
    static void SetUpTestSuite()
    {
        EngineCore& engine = EngineCore::GetInstance();
        if (engine.IsInitialized())
            return;
        ScriptsConfig scriptsConfig{};
        scriptsConfig.disableClr = true;
        scriptsConfig.enableHotReload = false;
        scriptsConfig.enableAsyncHotReload = false;
        scriptsConfig.enableAutoProjectGeneration = false;
        engine.SetScriptsConfig(scriptsConfig);
        ApplicationConfig config{};
        config.AssetDirectory = ".";
        config.WorkspaceDirectory = ".";
        config.EnableEditor = true;
        ASSERT_TRUE(engine.Initialize(config));
    }

    void SetUp() override
    {
        MarkupService::Initialize();
        m_OwnsSplines = !SplineECS::SplineService::IsInitialized();
        if (m_OwnsSplines)
            SplineECS::SplineService::Initialize();
        m_Bridge = std::make_unique<MarkupEditorBridge>(
            Notifications, [this]() { return Now; }, []() { return std::optional<std::filesystem::path>(); });
        Context.World = &World;
        Context.Bridge = m_Bridge.get();
        Context.Undo = &Undo;
        Context.Notifications = &Notifications;
        m_Bridge->BeginAgentRequest();
    }

    void TearDown() override
    {
        m_Bridge->EndAgentRequest();
        m_Bridge.reset();
        if (m_OwnsSplines)
            SplineECS::SplineService::Shutdown();
        MarkupService::Shutdown();
    }

    uint32 Create(const char* title, const json& extra = json::object())
    {
        json params{{"title", title}, {"center", {10.0f, 0.0f, 20.0f}}, {"halfExtents", {5.0f, 2.0f, 5.0f}}};
        params.update(extra);
        const json result = Editor::CreateMarkup(Context, params);
        EXPECT_FALSE(IsRefusal(result)) << result.dump();
        const uint32 id = result.value("entityId", 0u);
        SettleWorldTransform(id, Mathematics::Vector3(0.0f, 0.0f, 0.0f));
        return id;
    }

    // What the editor's transform pass gives the mark-up before the next request: its world
    // placement, here its local one moved by a parent's translation `parentOffset`.
    void SettleWorldTransform(uint32 id, const Mathematics::Vector3& parentOffset)
    {
        const ECS::EntityHandle entity(id);
        const auto* local = World.GetComponent<Components::Transform>(entity);
        if (!local)
            return;
        Components::WorldTransform world;
        std::copy(std::begin(local->matrix), std::end(local->matrix), std::begin(world.matrix));
        world.matrix[12] += parentOffset.x;
        world.matrix[13] += parentOffset.y;
        world.matrix[14] += parentOffset.z;
        if (World.GetComponent<Components::WorldTransform>(entity))
            *World.GetComponentForWrite<Components::WorldTransform>(entity) = world;
        else
            World.AddComponentImmediate(entity, world);
    }

    const Markup& MarkupOf(uint32 id) { return *World.GetComponent<Markup>(ECS::EntityHandle(id)); }

    Editor::EditorChangeNotifications Notifications;
    Editor::UndoRedoService Undo;
    ECS::World World;
    Editor::MarkupRequestContext Context;
    int64 Now = 1759750000;
    std::unique_ptr<MarkupEditorBridge> m_Bridge;
    bool m_OwnsSplines = false;
};

} // namespace

// The agent's mark-up is Proposed and the agent's, one undo step: undo removes it, redo
// brings back the same entity with its notes and its stamp, and stamps the redo with
// whoever asked for it, so a reader's `since` finds it again. A kind that does not exist and a
// volume without its shape are refused.
TEST_F(MarkupRequestsTest, CreateMakesAProposedVolumeAsOneUndoStep)
{
    const int64 created = Now;
    const uint32 id = Create("Village center", {{"description", "Market square"}});
    const ECS::EntityHandle village(id);
    EXPECT_EQ(MarkupOf(id).Status, Components::kMarkupStatusProposed);
    EXPECT_EQ(MarkupOf(id).Author, MarkupAuthor::Agent);
    EXPECT_EQ(World.GetComponent<Components::Name>(village)->View(), "Village center");
    EXPECT_EQ(World.GetComponent<Components::Transform>(village)->GetScale().x, 10.0f);
    EXPECT_EQ(MarkupService::Get().FindNotes(World, village)->Description, "Market square");
    (void)Create("Forest");
    const uint32 since = Editor::ListMarkups(Context, json::object())["revision"];

    Undo.Undo();
    Undo.Undo();
    EXPECT_FALSE(World.IsValid(village));
    Now += 60;
    {
        MarkupEditorBridge::AttributionScope user(*m_Bridge, MarkupAuthor::User);
        Undo.Redo();
    }
    ASSERT_TRUE(World.IsValid(village));
    EXPECT_EQ(MarkupService::Get().FindNotes(World, village)->Description, "Market square");
    EXPECT_EQ(MarkupOf(id).Author, MarkupAuthor::Agent);
    EXPECT_EQ(MarkupOf(id).CreatedUnix, created);
    EXPECT_EQ(MarkupOf(id).UpdatedBy, MarkupAuthor::User);
    EXPECT_EQ(MarkupOf(id).UpdatedUnix, Now);
    const json changed = Editor::ListMarkups(Context, {{"since", since}});
    ASSERT_EQ(changed["markups"].size(), 1u);
    EXPECT_EQ(changed["markups"][0]["entityId"], id);

    EXPECT_TRUE(IsRefusal(Editor::CreateMarkup(
        Context, {{"title", "River"}, {"kind", "canal"}, {"center", {0, 0, 0}}, {"halfExtents", {1, 1, 1}}})));
    EXPECT_TRUE(IsRefusal(Editor::CreateMarkup(Context, {{"title", "Lake"}})));
}

// Rows come newest first; since is strict over the scene revision, so an edit made in the
// same second as the reader's last answer is still returned; filters narrow by status and
// by who changed it last; hidden rows are listed and say so.
TEST_F(MarkupRequestsTest, ListSortsNewestFirstAndSinceIsStrictOverTheRevision)
{
    const uint32 lake = Create("Lake");
    Now += 10;
    const uint32 forest = Create("Forest");
    const json first = Editor::ListMarkups(Context, json::object());
    ASSERT_EQ(first["markups"].size(), 2u);
    EXPECT_EQ(first["markups"][0]["entityId"], forest);
    const uint32 revision = first["revision"];

    {
        MarkupEditorBridge::AttributionScope user(*m_Bridge, MarkupAuthor::User);
        (void)Editor::UpdateMarkup(Context, {{"entityId", lake}, {"center", {1, 0, 1}}, {"author", "User"}});
    }
    const ECS::EntityHandle lakeHandle(lake);
    m_Bridge->SetHidden(World, {&lakeHandle, 1}, true);
    const json changed = Editor::ListMarkups(Context, {{"since", revision}, {"changedBy", "User"}});
    ASSERT_EQ(changed["markups"].size(), 1u);
    EXPECT_EQ(changed["markups"][0]["entityId"], lake);
    EXPECT_EQ(changed["markups"][0]["hidden"], true);
    EXPECT_EQ(Editor::ListMarkups(Context, {{"since", changed["revision"]}})["markups"].size(), 0u);
    EXPECT_EQ(Editor::ListMarkups(Context, {{"status", "Requested"}})["markups"].size(), 0u);
    EXPECT_EQ(Editor::ListMarkups(Context, {{"includeHidden", false}})["markups"].size(), 1u);
}

TEST_F(MarkupRequestsTest, GetReturnsTheThreadAndTheShape)
{
    const uint32 lake = Create("Lake", {{"shape", "sphere"}, {"halfExtents", {30.0f, 30.0f, 30.0f}}});
    (void)Editor::CommentOnMarkup(Context, {{"entityId", lake}, {"text", "Is the shore right?"}});

    const json answer = Editor::GetMarkup(Context, {{"entityId", lake}});
    ASSERT_FALSE(IsRefusal(answer)) << answer.dump();
    const json& row = answer["markup"];
    EXPECT_EQ(row["shape"]["shape"], "sphere");
    EXPECT_FLOAT_EQ(row["shape"]["halfExtents"][0].get<float>(), 30.0f);
    EXPECT_FLOAT_EQ(row["radius"].get<float>(), 30.0f);
    ASSERT_EQ(row["entries"].size(), 2u);
    EXPECT_EQ(row["entries"][0]["kind"], "created");
    EXPECT_EQ(row["entries"][1]["text"], "Is the shore right?");
    EXPECT_TRUE(IsRefusal(Editor::GetMarkup(Context, {{"entityId", 123456u}})));
}

// An explicit author holds for the whole write: a scripted demonstration acting as the
// user stamps User although the request itself is the agent's. A status change appends
// its entry; a status that is not in the status group is refused with the list.
TEST_F(MarkupRequestsTest, UpdateStampsTheGivenAuthorAndRecordsAStatusChange)
{
    const uint32 village = Create("Village");
    const json row = Editor::UpdateMarkup(Context, {{"entityId", village}, {"status", "Requested"}, {"author", "User"}});
    ASSERT_FALSE(IsRefusal(row)) << row.dump();
    EXPECT_EQ(MarkupOf(village).Status, Components::kMarkupStatusRequested);
    EXPECT_EQ(MarkupOf(village).UpdatedBy, MarkupAuthor::User);
    const auto& entries = MarkupService::Get().FindNotes(World, ECS::EntityHandle(village))->Entries;
    EXPECT_EQ(entries.back().Kind, MarkupECS::MarkupEntryKind::StatusChange);
    EXPECT_EQ(entries.back().Author, MarkupAuthor::User);

    (void)Editor::UpdateMarkup(Context, {{"entityId", village}, {"title", "Village square"}, {"tags", {"Market"}}});
    EXPECT_EQ(MarkupOf(village).UpdatedBy, MarkupAuthor::Agent);
    EXPECT_EQ(row["markup"]["status"], "Requested");
    EXPECT_TRUE(IsRefusal(Editor::UpdateMarkup(Context, {{"entityId", village}, {"status", "Market"}})));
}

// An update is one undo step: undo puts back the title, the volume, the color, the status, the
// description, the tags and the thread, and redo makes the update again. A refused update and
// a hidden-only one record no step.
TEST_F(MarkupRequestsTest, UpdateIsOneUndoStep)
{
    const uint32 id = Create("Lake");
    const ECS::EntityHandle lake(id);
    const Markup before = MarkupOf(id);
    const size_t entriesBefore = MarkupService::Get().FindNotes(World, lake)->Entries.size();
    const size_t steps = Undo.GetUndoCount();

    const json row = Editor::UpdateMarkup(Context, {{"entityId", id},
                                                    {"title", "Pond"},
                                                    {"center", {1.0f, 0.0f, 1.0f}},
                                                    {"color", {0.1f, 0.2f, 0.3f, 1.0f}},
                                                    {"status", "Requested"},
                                                    {"description", "Shallow"},
                                                    {"tags", {"Water"}}});
    ASSERT_FALSE(IsRefusal(row)) << row.dump();
    ASSERT_EQ(Undo.GetUndoCount(), steps + 1);
    EXPECT_STREQ(Undo.PeekUndoName(), "Update Mark-up");

    Undo.Undo();
    const MarkupECS::MarkupNotes* notes = MarkupService::Get().FindNotes(World, lake);
    EXPECT_EQ(World.GetComponent<Components::Name>(lake)->View(), "Lake");
    EXPECT_EQ(World.GetComponent<Components::Transform>(lake)->GetPosition().x, 10.0f);
    EXPECT_EQ(MarkupOf(id).Color[0], before.Color[0]);
    EXPECT_EQ(MarkupOf(id).Status, Components::kMarkupStatusProposed);
    EXPECT_EQ(notes->Description, "");
    EXPECT_TRUE(notes->Tags.empty());
    EXPECT_EQ(notes->Entries.size(), entriesBefore);

    Undo.Redo();
    notes = MarkupService::Get().FindNotes(World, lake);
    EXPECT_EQ(World.GetComponent<Components::Name>(lake)->View(), "Pond");
    EXPECT_EQ(World.GetComponent<Components::Transform>(lake)->GetPosition().x, 1.0f);
    EXPECT_EQ(MarkupOf(id).Color[0], 0.1f);
    EXPECT_EQ(MarkupOf(id).Status, Components::kMarkupStatusRequested);
    EXPECT_EQ(notes->Description, "Shallow");
    EXPECT_EQ(notes->Tags.size(), 1u);

    EXPECT_TRUE(IsRefusal(Editor::UpdateMarkup(Context, {{"entityId", id}, {"title", ""}})));
    (void)Editor::UpdateMarkup(Context, {{"entityId", id}, {"hidden", true}});
    EXPECT_EQ(Undo.GetUndoCount(), steps + 1);
}

TEST_F(MarkupRequestsTest, CommentAppendsAnEntryByItsAuthor)
{
    const uint32 forest = Create("Forest");
    const json result = Editor::CommentOnMarkup(Context, {{"entityId", forest}, {"text", "Reaches the river"}, {"author", "User"}});
    ASSERT_FALSE(IsRefusal(result)) << result.dump();
    EXPECT_EQ(result["entry"]["author"], "User");
    EXPECT_EQ(result["entry"]["text"], "Reaches the river");
    EXPECT_TRUE(IsRefusal(Editor::CommentOnMarkup(Context, {{"entityId", forest}, {"text", ""}})));
}

// markup_frame is look_at's pose over the shape's bounding sphere, fitted to the field of
// view: a 20 m radius at the default 60 degrees stands 20 / sin 30 degrees x 1.15 = 46 m off.
TEST_F(MarkupRequestsTest, FrameIsLookAtsPoseOverTheBoundingSphere)
{
    const uint32 lake = Create("Lake", {{"shape", "sphere"}, {"halfExtents", {20.0f, 20.0f, 20.0f}}});
    SceneViewCameraPose framed{};
    const json result = Editor::FrameMarkup(Context, {{"entityId", lake}}, framed);
    ASSERT_FALSE(IsRefusal(result)) << result.dump();

    SceneViewCameraPose lookAt{};
    ASSERT_TRUE(Editor::ComputeLookAtPose(Mathematics::Vector3(10.0f, 0.0f, 20.0f), Mathematics::Vector3(1.0f, -0.5f, 1.0f),
                                          46.0f, lookAt));
    for (int axis = 0; axis < 3; ++axis)
        EXPECT_FLOAT_EQ(framed.Pos[axis], lookAt.Pos[axis]);
    EXPECT_FLOAT_EQ(framed.YawDeg, lookAt.YawDeg);
    EXPECT_FLOAT_EQ(framed.PitchDeg, lookAt.PitchDeg);
    EXPECT_FLOAT_EQ(framed.Distance, 46.0f);
}

// A parented mark-up stands at its world position, which is where markup_frame looks: the
// volume the Scene View draws, not its local placement.
TEST_F(MarkupRequestsTest, FrameLooksAtAParentedMarkupsWorldPosition)
{
    const uint32 lake = Create("Lake", {{"shape", "sphere"}, {"halfExtents", {20.0f, 20.0f, 20.0f}}});
    SettleWorldTransform(lake, Mathematics::Vector3(100.0f, 0.0f, 0.0f));
    SceneViewCameraPose framed{};
    const json result = Editor::FrameMarkup(Context, {{"entityId", lake}}, framed);
    ASSERT_FALSE(IsRefusal(result)) << result.dump();
    EXPECT_EQ(result["target"], json::array({110.0f, 0.0f, 20.0f}));

    SceneViewCameraPose lookAt{};
    ASSERT_TRUE(Editor::ComputeLookAtPose(Mathematics::Vector3(110.0f, 0.0f, 20.0f),
                                          Mathematics::Vector3(1.0f, -0.5f, 1.0f), 46.0f, lookAt));
    for (int axis = 0; axis < 3; ++axis)
        EXPECT_FLOAT_EQ(framed.Pos[axis], lookAt.Pos[axis]);
}

// Visibility is the viewer's: hiding all changes no component and no revision; every
// method refuses in play mode.
TEST_F(MarkupRequestsTest, SetVisibleHidesWithoutASceneEditAndPlayModeRefuses)
{
    const uint32 lake = Create("Lake");
    (void)Create("Forest");
    const uint32 revision = MarkupService::Get().GetRevision(World);

    const json hidden = Editor::SetMarkupsVisible(Context, {{"all", true}, {"visible", false}});
    ASSERT_FALSE(IsRefusal(hidden)) << hidden.dump();
    EXPECT_EQ(hidden["markups"].size(), 2u);
    EXPECT_TRUE(m_Bridge->IsHidden(World, ECS::EntityHandle(lake)));
    EXPECT_EQ(MarkupService::Get().GetRevision(World), revision);
    (void)Editor::SetMarkupsVisible(Context, {{"entityId", lake}, {"visible", true}});
    EXPECT_FALSE(m_Bridge->IsHidden(World, ECS::EntityHandle(lake)));

    Context.PlayMode = true;
    EXPECT_TRUE(IsRefusal(Editor::ListMarkups(Context, json::object())));
}

// Readers skip the placeholders an unreadable scene line leaves, and every answer names the
// run of revisions it belongs to: a world reset changes the epoch, telling the reader its
// `since` no longer applies.
TEST_F(MarkupRequestsTest, ReadersSkipUnreadableSlotsAndAResetChangesTheEpoch)
{
    const uint32 lake = Create("Lake");
    MarkupECS::MarkupNotes& notes = MarkupService::Get().EnsureNotes(World, ECS::EntityHandle(lake));
    notes.Entries.push_back(MarkupECS::MarkupEntry{MarkupECS::MarkupEntryKind::Unreadable});
    notes.Tags.push_back(MarkupService::kInvalidTag);

    const json answer = Editor::GetMarkup(Context, {{"entityId", lake}});
    ASSERT_FALSE(IsRefusal(answer)) << answer.dump();
    EXPECT_EQ(answer["markup"]["entries"].size(), 1u);
    EXPECT_EQ(answer["markup"]["entryCount"], 1u);
    EXPECT_TRUE(answer["markup"]["tags"].empty());

    const std::string before = Editor::ListMarkups(Context, json::object())["epoch"];
    World.Clear();
    const std::string after = Editor::ListMarkups(Context, json::object())["epoch"];
    EXPECT_NE(before, after);
}

// Update refuses what create refuses: a size that is not positive and an empty title.
TEST_F(MarkupRequestsTest, UpdateRefusesAShapeWithNoSize)
{
    const uint32 lake = Create("Lake");
    const json result = Editor::UpdateMarkup(Context, {{"entityId", lake}, {"halfExtents", {0.0f, -1.0f, 0.0f}}});
    EXPECT_TRUE(IsRefusal(result)) << result.dump();
    EXPECT_EQ(World.GetComponent<Components::Transform>(ECS::EntityHandle(lake))->GetScale().x, 10.0f);
}

TEST_F(MarkupRequestsTest, UpdateRefusesAnEmptyTitle)
{
    const uint32 lake = Create("Lake");
    const json result = Editor::UpdateMarkup(Context, {{"entityId", lake}, {"title", ""}});
    EXPECT_TRUE(IsRefusal(result)) << result.dump();
    EXPECT_EQ(World.GetComponent<Components::Name>(ECS::EntityHandle(lake))->View(), "Lake");
}

// A parameter of the wrong type is refused with the fix, never thrown as a parse error.
TEST_F(MarkupRequestsTest, WrongTypedParametersAreRefusedWithTheFix)
{
    const uint32 lake = Create("Lake");
    SceneViewCameraPose pose{};
    const json answers[] = {
        Editor::ListMarkups(Context, {{"includeHidden", "no"}}),
        Editor::SetMarkupsVisible(Context, {{"all", "yes"}, {"visible", false}}),
        Editor::FrameMarkup(Context, {{"entityId", lake}, {"distance", "far"}}, pose),
        Editor::UpdateMarkup(Context, {{"entityId", lake}, {"rotation", {"a", 0, 0, 1}}}),
    };
    for (const json& answer : answers)
    {
        ASSERT_TRUE(IsRefusal(answer)) << answer.dump();
        EXPECT_EQ(answer.dump().find("json.exception"), std::string::npos) << answer.dump();
    }
}

// An entityId wider than a handle names no mark-up, rather than the one its low bits name.
TEST_F(MarkupRequestsTest, AnEntityIdWiderThanAHandleIsRefused)
{
    const uint32 lake = Create("Lake");
    const json result = Editor::GetMarkup(Context, {{"entityId", (uint64_t(1) << 32) + lake}});
    EXPECT_TRUE(IsRefusal(result)) << result.dump().substr(0, 200);
}

TEST_F(MarkupRequestsTest, FrameRefusesADistanceThatIsNotPositive)
{
    const uint32 lake = Create("Lake");
    SceneViewCameraPose pose{};
    EXPECT_TRUE(IsRefusal(Editor::FrameMarkup(Context, {{"entityId", lake}, {"distance", 0.0f}}, pose)));
    EXPECT_TRUE(IsRefusal(Editor::FrameMarkup(Context, {{"entityId", lake}, {"distance", -10.0f}}, pose)));
}

// Every answer carries the scene's revision as `revision`, and the epoch, so a reader can
// keep its `since` from whichever answer came last. The forest is created last, so the
// scene's revision is above the lake's own.
TEST_F(MarkupRequestsTest, EveryAnswerCarriesTheScenesRevisionAndTheEpoch)
{
    const uint32 lake = Create("Lake");
    (void)Create("Forest");
    SceneViewCameraPose pose{};
    std::vector<std::pair<json, uint32>> answers;
    const auto answer = [&](json reply) { answers.emplace_back(std::move(reply), MarkupService::Get().GetRevision(World)); };
    answer(Editor::ListMarkups(Context, json::object()));
    answer(Editor::GetMarkup(Context, {{"entityId", lake}}));
    answer(Editor::UpdateMarkup(Context, {{"entityId", lake}, {"color", {0, 0, 0, 0}}}));
    answer(Editor::CommentOnMarkup(Context, {{"entityId", lake}, {"text", "Moved it"}}));
    answer(Editor::FrameMarkup(Context, {{"entityId", lake}}, pose));
    answer(Editor::SetMarkupsVisible(Context, {{"entityId", lake}, {"visible", false}}));
    (void)Create("Meadow");
    answer(Editor::GetMarkup(Context, {{"entityId", lake}}));
    for (const auto& [reply, sceneRevision] : answers)
    {
        ASSERT_FALSE(IsRefusal(reply)) << reply.dump();
        EXPECT_TRUE(reply.contains("epoch")) << reply.dump();
        EXPECT_EQ(reply.value("revision", 0u), sceneRevision) << reply.dump();
    }
}

namespace
{

json Square(float minX, float minZ, float size)
{
    return json::array({json::array({minX, minZ}), json::array({minX + size, minZ}),
                        json::array({minX + size, minZ + size}), json::array({minX, minZ + size})});
}

// A forest region 200 m across with a clearing exclusion in its middle and `lake` as an
// Exclude member over its south-west corner.
json ForestParams(uint32 lake)
{
    return json{{"title", "Forest"},
                {"kind", "region"},
                {"outline", Square(0.0f, 0.0f, 200.0f)},
                {"exclusions", json::array({Square(80.0f, 80.0f, 40.0f)})},
                {"members", json::array({json{{"entityId", lake}, {"mode", "exclude"}}})}};
}

std::vector<bool> InsideAnswers(const json& answer)
{
    std::vector<bool> inside;
    for (const json& value : answer["inside"])
        inside.push_back(value.get<bool>());
    return inside;
}

} // namespace

// The agent's forest with a clearing and the lake cut out, as one undo step: the region is a
// closed linear spline over the outline, the clearing an Exclude region parented under it,
// and markup_contains answers with both cuts applied. Undo removes both entities; redo brings
// the same handles back with their outlines.
TEST_F(MarkupRequestsTest, ARegionWithAClearingAndALakeIsOneUndoStepAndContainsAppliesTheCuts)
{
    const uint32 lake = Create("Lake", {{"shape", "sphere"}, {"center", {20.0f, 0.0f, 20.0f}},
                                        {"halfExtents", {15.0f, 15.0f, 15.0f}}});
    const json created = Editor::CreateMarkup(Context, ForestParams(lake));
    ASSERT_FALSE(IsRefusal(created)) << created.dump();
    const ECS::EntityHandle forest(created["entityId"].get<uint32>());
    ASSERT_EQ(created["exclusionIds"].size(), 1u);
    const ECS::EntityHandle clearing(created["exclusionIds"][0].get<uint32>());
    EXPECT_EQ(created["markup"]["kind"], "region");
    ASSERT_NE(World.GetComponent<Components::MarkupRegion>(forest), nullptr);
    EXPECT_EQ(World.GetComponent<Components::MarkupRegion>(forest)->MemberCount, 2u);
    EXPECT_EQ(World.GetComponent<Components::Parent>(clearing)->parent, forest);
    EXPECT_EQ(World.GetComponent<Components::MarkupVolume>(forest), nullptr);

    const json points = json::array({json::array({150.0f, 30.0f}), json::array({100.0f, 100.0f}),
                                     json::array({20.0f, 20.0f}), json::array({250.0f, 0.0f})});
    const json contains = Editor::ContainsInMarkup(Context, {{"entityId", forest.id}, {"points", points}});
    ASSERT_FALSE(IsRefusal(contains)) << contains.dump();
    EXPECT_EQ(InsideAnswers(contains), (std::vector<bool>{true, false, false, false}));
    EXPECT_EQ(contains["insideCount"], 1u);

    const json knotsBefore = Editor::GetMarkup(Context, {{"entityId", forest.id}})["markup"]["shape"]["knots"];
    Undo.Undo();
    EXPECT_FALSE(World.IsValid(forest));
    EXPECT_FALSE(World.IsValid(clearing));
    Undo.Redo();
    ASSERT_TRUE(World.IsValid(forest));
    ASSERT_TRUE(World.IsValid(clearing));
    EXPECT_EQ(Editor::GetMarkup(Context, {{"entityId", forest.id}})["markup"]["shape"]["knots"], knotsBefore);
    EXPECT_EQ(InsideAnswers(Editor::ContainsInMarkup(Context, {{"entityId", forest.id}, {"points", points}})),
              (std::vector<bool>{true, false, false, false}));
}

// markup_get's area and perimeter are the area markup_contains tests: the forest's 200 m square
// less the clearing (40 m square) and the lake (a 32-gon of radius 15), every ring's length.
TEST_F(MarkupRequestsTest, GetGivesTheAreaAndPerimeterWithTheMembersApplied)
{
    const uint32 lake = Create("Lake", {{"shape", "sphere"}, {"center", {20.0f, 0.0f, 20.0f}},
                                        {"halfExtents", {15.0f, 15.0f, 15.0f}}});
    const uint32 forest = Editor::CreateMarkup(Context, ForestParams(lake))["entityId"];
    const json shape = Editor::GetMarkup(Context, {{"entityId", forest}})["markup"]["shape"];
    EXPECT_NEAR(shape["area"].get<float>(), 37697.7f, 1.0f) << "the area left out a member";
    EXPECT_NEAR(shape["perimeter"].get<float>(), 1054.1f, 0.5f) << "the perimeter left out a hole";
}

// A second region's exclusion is titled with the next index no mark-up uses, as its tag is.
TEST_F(MarkupRequestsTest, AnExclusionTakesTheNextFreeTitle)
{
    const json params{{"title", "Forest"},
                      {"kind", "region"},
                      {"outline", Square(0.0f, 0.0f, 200.0f)},
                      {"exclusions", json::array({Square(80.0f, 80.0f, 40.0f)})}};
    const json first = Editor::CreateMarkup(Context, params);
    const json second = Editor::CreateMarkup(Context, params);
    const auto titleOf = [&](const json& created) {
        return Editor::GetMarkup(Context, {{"entityId", created["exclusionIds"][0]}})["markup"]["title"];
    };
    EXPECT_EQ(titleOf(first), "Forest exclusion 1");
    EXPECT_EQ(titleOf(second), "Forest exclusion 2");
}

// A long region title is cut before its exclusions' index, so each exclusion keeps its own
// index whole within a stored title's 63 characters.
TEST_F(MarkupRequestsTest, ALongRegionTitlesExclusionsKeepTheirIndex)
{
    const std::string title(70, 'F');
    const json created = Editor::CreateMarkup(
        Context, {{"title", title},
                  {"kind", "region"},
                  {"outline", Square(0.0f, 0.0f, 200.0f)},
                  {"exclusions", json::array({Square(20.0f, 20.0f, 40.0f), Square(120.0f, 120.0f, 40.0f)})}});
    ASSERT_EQ(created["exclusionIds"].size(), 2u) << created.dump();
    const auto titleOf = [&](const json& id) {
        return Editor::GetMarkup(Context, {{"entityId", id}})["markup"]["title"].get<std::string>();
    };
    EXPECT_EQ(titleOf(created["exclusionIds"][0]), std::string(51, 'F') + " exclusion 1");
    EXPECT_EQ(titleOf(created["exclusionIds"][1]), std::string(51, 'F') + " exclusion 2");
}

// Each refusal names the fix, and nothing is created.
TEST_F(MarkupRequestsTest, ARegionOutlineIsRefusedWithTheFix)
{
    const auto refusal = [&](json outline, json extra = json::object()) {
        json params{{"title", "Forest"}, {"kind", "region"}, {"outline", std::move(outline)}};
        params.update(extra);
        const json answer = Editor::CreateMarkup(Context, params);
        EXPECT_TRUE(IsRefusal(answer)) << answer.dump();
        return answer.dump();
    };
    EXPECT_NE(refusal(json::array({json::array({0, 0}), json::array({10, 0})})).find("3 to 256 points"),
              std::string::npos);
    json many = json::array();
    for (int i = 0; i < 257; ++i)
        many.push_back(json::array({100.0 * std::cos(i * 0.0244), 100.0 * std::sin(i * 0.0244)}));
    EXPECT_NE(refusal(many).find("3 to 256 points"), std::string::npos);
    const json bowtie = json::array({json::array({0, 0}), json::array({10, 0}), json::array({0, 10}), json::array({10, 10})});
    EXPECT_NE(refusal(bowtie).find("crosses itself between points 1-2 and 3-0"), std::string::npos);
    EXPECT_NE(refusal(json::array({json::array({0, 0}), json::array({10, 0}), json::array({20, 0})})).find("encloses no area"),
              std::string::npos);
    EXPECT_NE(refusal(Square(0.0f, 0.0f, 10.0f), {{"type", "bezier"}}).find("linear"), std::string::npos);
    EXPECT_NE(refusal(Square(0.0f, 0.0f, 10.0f), {{"extrudeHeight", 0.5}}).find("1 to 100"), std::string::npos);
    EXPECT_TRUE(MarkupService::Get().GetMarkups(World).empty());
}

// markup_get gives a smooth region's evaluated ring, decimated, as `outline`, which is exactly
// the ring markup_contains tests, its knots as sent, and each member with its footprint.
TEST_F(MarkupRequestsTest, GetReturnsTheEvaluatedRingTheKnotsAndTheMembers)
{
    const uint32 lake = Create("Lake", {{"shape", "sphere"}, {"center", {20.0f, 0.0f, 20.0f}},
                                        {"halfExtents", {15.0f, 15.0f, 15.0f}}});
    json params = ForestParams(lake);
    params["type"] = "smooth";
    const uint32 forest = Editor::CreateMarkup(Context, params)["entityId"];
    const json shape = Editor::GetMarkup(Context, {{"entityId", forest}})["markup"]["shape"];
    EXPECT_EQ(shape["shape"], "region");
    EXPECT_EQ(shape["type"], "smooth");
    EXPECT_EQ(shape["knots"], Square(0.0f, 0.0f, 200.0f));
    EXPECT_GT(shape["outline"].size(), 4u);
    EXPECT_LE(shape["outline"].size(), 256u);

    const std::optional<MarkupECS::MarkupRegionArea> area =
        MarkupECS::MarkupRegionArea::Read(World, ECS::EntityHandle(forest));
    ASSERT_TRUE(area.has_value());
    const std::vector<Mathematics::Vector2>& tested = area->GetBase().Ring;
    ASSERT_EQ(shape["outline"].size(), tested.size());
    for (std::size_t i = 0; i < tested.size(); ++i)
    {
        EXPECT_FLOAT_EQ(shape["outline"][i][0].get<float>(), tested[i].x) << i;
        EXPECT_FLOAT_EQ(shape["outline"][i][1].get<float>(), tested[i].y) << i;
    }

    ASSERT_EQ(shape["members"].size(), 2u);
    EXPECT_EQ(shape["members"][0]["entityId"], lake);
    EXPECT_EQ(shape["members"][0]["mode"], "exclude");
    EXPECT_EQ(shape["members"][0]["kind"], "volume");
    EXPECT_FLOAT_EQ(shape["members"][0]["footprint"]["radius"].get<float>(), 15.0f);
    EXPECT_EQ(shape["members"][1]["kind"], "region");
    EXPECT_EQ(shape["members"][1]["footprint"]["outline"], Square(80.0f, 80.0f, 40.0f));
    EXPECT_GT(shape["area"].get<float>(), 0.0f);
    EXPECT_EQ(Editor::ListMarkups(Context, json::object())["markups"][0]["kind"], "region");
}

// A path is a mark-up on an open spline through its points: markup_get gives the points back in
// the world, markup_list lists it as a path at the sphere around them, markup_frame frames it, and
// it is one undo step. What a path cannot be is refused, naming the fix: one point, a point
// repeated, a volume's or a region's fields in an update, an inside test.
TEST_F(MarkupRequestsTest, APathRoundTripsItsPointsAndRefusesWhatItIsNot)
{
    const json points = json::array({json::array({0.0f, 30.0f, 0.0f}), json::array({40.0f, 34.0f, 10.0f}),
                                      json::array({90.0f, 41.0f, -5.0f})});
    const json created = Editor::CreateMarkup(
        Context, {{"title", "To the guards"}, {"kind", "path"}, {"points", points}, {"type", "smooth"}});
    ASSERT_FALSE(IsRefusal(created)) << created.dump();
    const uint32 road = created["entityId"];
    const ECS::EntityHandle entity(road);
    EXPECT_EQ(World.GetComponent<Components::MarkupVolume>(entity), nullptr);
    EXPECT_EQ(World.GetComponent<Components::MarkupRegion>(entity), nullptr);
    ASSERT_NE(World.GetComponent<Components::SplineComponent>(entity), nullptr);

    const json shape = Editor::GetMarkup(Context, {{"entityId", road}})["markup"]["shape"];
    EXPECT_EQ(shape["shape"], "path");
    EXPECT_EQ(shape["type"], "smooth");
    ASSERT_EQ(shape["points"].size(), 3u);
    for (std::size_t i = 0; i < 3; ++i)
    {
        for (std::size_t k = 0; k < 3; ++k)
            EXPECT_NEAR(shape["points"][i][k].get<float>(), points[i][k].get<float>(), 1.0e-4f);
    }
    EXPECT_GT(shape["length"].get<float>(), 90.0f);
    const json row = Editor::ListMarkups(Context, json::object())["markups"][0];
    EXPECT_EQ(row["kind"], "path");
    EXPECT_NEAR(row["center"][0].get<float>(), 45.0f, 1.0e-3f);
    SceneViewCameraPose pose{};
    EXPECT_FALSE(IsRefusal(Editor::FrameMarkup(Context, {{"entityId", road}}, pose)));

    EXPECT_TRUE(IsRefusal(Editor::UpdateMarkup(Context, {{"entityId", road}, {"center", {0, 0, 0}}})));
    EXPECT_TRUE(IsRefusal(Editor::UpdateMarkup(Context, {{"entityId", road}, {"outline", Square(0.0f, 0.0f, 10.0f)}})));
    EXPECT_TRUE(IsRefusal(Editor::UpdateMarkup(Context, {{"entityId", road}, {"kind", "region"}})));
    EXPECT_NE(Editor::UpdateMarkup(Context, {{"entityId", road}, {"points", {{0, 0, 0}, {10, 0, 10}}}}).dump().find("Edit path"),
              std::string::npos)
        << "points on a path answered ok and changed nothing";
    EXPECT_FALSE(IsRefusal(Editor::UpdateMarkup(Context, {{"entityId", road}, {"status", "Requested"}})));
    EXPECT_NE(Editor::ContainsInMarkup(Context, {{"entityId", road}, {"points", {{0, 0}}}}).dump().find("encloses no area"),
              std::string::npos);
    EXPECT_NE(Editor::CreateMarkup(Context, {{"title", "Stub"}, {"kind", "path"}, {"points", {{0, 0, 0}}}}).dump().find("2 to 256"),
              std::string::npos);
    EXPECT_NE(Editor::CreateMarkup(Context, {{"title", "Stub"}, {"kind", "path"}, {"points", {{0, 0, 0}, {0, 0, 0}}}})
                  .dump()
                  .find("same place"),
              std::string::npos);

    Undo.Undo(); // the status
    Undo.Undo(); // the path
    EXPECT_FALSE(World.IsValid(entity));

    // Over a terrain (400 m, flat at y = 0 but for a 20 m ridge across x = 20), points authored 110
    // to 170 m below it stand on it: the knots the port returns are on the ground, and the length is
    // the line's along the ground, over the ridge (10 + 2 x 22.36 + 10 + 40 = 104.72 m), not the
    // 80 m between the knots.
    const bool ownsTerrains = !TerrainECS::TerrainService::IsInitialized();
    if (ownsTerrains)
        TerrainECS::TerrainService::Initialize();
    Terrain::TerrainConfig config{};
    config.HeightmapWidth = 401; // a sample every meter
    config.HeightmapHeight = 401;
    config.WorldSizeX = 400.0f;
    config.WorldSizeZ = 400.0f;
    config.HeightScale = 20.0f;
    config.LODLevels = 1;
    config.PatchGridSize = 8;
    const TerrainECS::TerrainHandle ridge = TerrainECS::TerrainService::Get().CreateTerrain(config);
    TerrainECS::TerrainData* heights = TerrainECS::TerrainService::Get().GetTerrainData(ridge);
    ASSERT_NE(heights, nullptr);
    for (uint32 line = 0; line < 401; ++line)
    {
        for (uint32 column = 0; column < 401; ++column)
        {
            const float x = static_cast<float>(column) - 200.0f;
            heights->Heightfield.GetMutableSamples()[line * 401 + column] = std::max(0.0f, 1.0f - std::fabs(x - 20.0f) / 10.0f);
        }
    }
    TerrainECS::TerrainService::Get().RebuildQuadtree(ridge);
    const ECS::EntityHandle ground = World.CreateEntity();
    Components::Terrain terrain{};
    terrain.SizeX = config.WorldSizeX;
    terrain.SizeZ = config.WorldSizeZ;
    terrain.HeightScale = config.HeightScale;
    terrain.TerrainDataHandle = ridge.Index;
    terrain.TerrainDataGeneration = ridge.Generation;
    World.AddComponentImmediate(ground, terrain);
    World.AddComponentImmediate(ground, Components::WorldTransform{});
    const json below = Editor::CreateMarkup(
        Context, {{"title", "Under"}, {"kind", "path"}, {"points", {{0, -150, 0}, {40, -110, 0}, {80, -170, 0}}}});
    ASSERT_FALSE(IsRefusal(below)) << below.dump();
    const json belowShape = Editor::GetMarkup(Context, {{"entityId", below["entityId"]}})["markup"]["shape"];
    for (const json& point : belowShape["points"])
        EXPECT_NEAR(point[1].get<float>(), 0.0f, 0.05f) << "a knot stayed under the ground";
    EXPECT_NEAR(belowShape["length"].get<float>(), 104.72f, 1.05f) << "the length is not the line's along the ground";
    TerrainECS::TerrainService::Get().DestroyTerrain(ridge);
    if (ownsTerrains)
        TerrainECS::TerrainService::Shutdown();
}

// A region listing itself, a 33rd member and closed: false are refused; outline,
// extrudeHeight and members are written; volume fields on a region are refused.
TEST_F(MarkupRequestsTest, UpdateWritesARegionAndRefusesWhatItCannotHold)
{
    const json created = Editor::CreateMarkup(
        Context, {{"title", "Village"}, {"kind", "region"}, {"outline", Square(0.0f, 0.0f, 100.0f)}});
    const uint32 village = created["entityId"];
    const json self = json::array({json{{"entityId", village}, {"mode", "exclude"}}});
    EXPECT_NE(Editor::UpdateMarkup(Context, {{"entityId", village}, {"members", self}}).dump().find("cannot list itself"),
              std::string::npos);
    json many = json::array();
    for (int i = 0; i < 33; ++i)
        many.push_back(json{{"entityId", Create("Well", {{"shape", "sphere"}})}, {"mode", "exclude"}});
    EXPECT_NE(Editor::UpdateMarkup(Context, {{"entityId", village}, {"members", many}}).dump().find("at most 32"),
              std::string::npos);
    EXPECT_TRUE(IsRefusal(Editor::UpdateMarkup(Context, {{"entityId", village}, {"closed", false}})));
    EXPECT_TRUE(IsRefusal(Editor::UpdateMarkup(Context, {{"entityId", village}, {"center", {0, 0, 0}}})));
    EXPECT_EQ(World.GetComponent<Components::MarkupRegion>(ECS::EntityHandle(village))->MemberCount, 0u);

    many.erase(many.begin() + 1, many.end());
    const json updated = Editor::UpdateMarkup(
        Context, {{"entityId", village}, {"outline", Square(0.0f, 0.0f, 300.0f)}, {"extrudeHeight", 20}, {"members", many}});
    ASSERT_FALSE(IsRefusal(updated)) << updated.dump();
    EXPECT_EQ(World.GetComponent<Components::MarkupRegion>(ECS::EntityHandle(village))->ExtrudeHeight, 20.0f);
    EXPECT_EQ(World.GetComponent<Components::MarkupRegion>(ECS::EntityHandle(village))->MemberCount, 1u);
    EXPECT_EQ(Editor::GetMarkup(Context, {{"entityId", village}})["markup"]["shape"]["knots"], Square(0.0f, 0.0f, 300.0f));
}

// Members and exclusions share the 32 cap: a create or an update over it is refused and
// creates nothing, rather than making exclusion regions the list cannot hold.
TEST_F(MarkupRequestsTest, MembersAndExclusionsTogetherHoldAtMost32)
{
    json members = json::array();
    for (int i = 0; i < 20; ++i)
        members.push_back(json{{"entityId", Create("Well", {{"shape", "sphere"}})}, {"mode", "exclude"}});
    json exclusions = json::array();
    for (int i = 0; i < 13; ++i)
        exclusions.push_back(Square(10.0f * i, 0.0f, 5.0f));
    const std::size_t markupsBefore = MarkupService::Get().GetMarkups(World).size();
    const json refused = Editor::CreateMarkup(Context, {{"title", "Forest"},
                                                        {"kind", "region"},
                                                        {"outline", Square(0.0f, 0.0f, 200.0f)},
                                                        {"members", members},
                                                        {"exclusions", exclusions}});
    ASSERT_TRUE(IsRefusal(refused));
    EXPECT_NE(refused.dump().find("at most 32"), std::string::npos) << refused.dump();
    EXPECT_EQ(MarkupService::Get().GetMarkups(World).size(), markupsBefore);

    const uint32 forest = Editor::CreateMarkup(Context, {{"title", "Forest"},
                                                         {"kind", "region"},
                                                         {"outline", Square(0.0f, 0.0f, 200.0f)},
                                                         {"members", members}})["entityId"];
    const std::size_t markupsWithForest = MarkupService::Get().GetMarkups(World).size();
    const json over = Editor::UpdateMarkup(Context, {{"entityId", forest}, {"exclusions", exclusions}});
    ASSERT_TRUE(IsRefusal(over));
    EXPECT_NE(over.dump().find("at most 32"), std::string::npos) << over.dump();
    EXPECT_EQ(MarkupService::Get().GetMarkups(World).size(), markupsWithForest);
    EXPECT_EQ(World.GetComponent<Components::MarkupRegion>(ECS::EntityHandle(forest))->MemberCount, 20u);
}

// Exclusions added by markup_update and their place in the member list are one undo step.
TEST_F(MarkupRequestsTest, ExclusionsAddedByAnUpdateAreOneUndoStep)
{
    const uint32 forest = Editor::CreateMarkup(
        Context, {{"title", "Forest"}, {"kind", "region"}, {"outline", Square(0.0f, 0.0f, 200.0f)}})["entityId"];
    const json updated = Editor::UpdateMarkup(
        Context, {{"entityId", forest}, {"exclusions", json::array({Square(80.0f, 80.0f, 40.0f)})}});
    ASSERT_FALSE(IsRefusal(updated)) << updated.dump();
    const auto* region = World.GetComponent<Components::MarkupRegion>(ECS::EntityHandle(forest));
    ASSERT_EQ(region->MemberCount, 1u);
    const ECS::EntityHandle clearing = region->Members[0].Entity;
    EXPECT_EQ(World.GetComponent<Components::Parent>(clearing)->parent, ECS::EntityHandle(forest));
    const json middle = json::array({json::array({100.0f, 100.0f})});
    EXPECT_FALSE(Editor::ContainsInMarkup(Context, {{"entityId", forest}, {"points", middle}})["inside"][0].get<bool>());

    Undo.Undo();
    EXPECT_FALSE(World.IsValid(clearing));
    EXPECT_EQ(World.GetComponent<Components::MarkupRegion>(ECS::EntityHandle(forest))->MemberCount, 0u);
    EXPECT_TRUE(Editor::ContainsInMarkup(Context, {{"entityId", forest}, {"points", middle}})["inside"][0].get<bool>());
    Undo.Redo();
    EXPECT_TRUE(World.IsValid(clearing));
    EXPECT_FALSE(Editor::ContainsInMarkup(Context, {{"entityId", forest}, {"points", middle}})["inside"][0].get<bool>());
}

// A region's update is one undo step too: a conversion with members in the same call comes back
// with its members on redo, and an outline and height change undoes to the old ones.
TEST_F(MarkupRequestsTest, ARegionUpdateIsOneUndoStepAndRedoKeepsItsMembers)
{
    const uint32 field = Create("Field", {{"halfExtents", {30.0f, 6.0f, 20.0f}}});
    const uint32 well = Create("Well", {{"shape", "sphere"}});
    const ECS::EntityHandle entity(field);
    const size_t steps = Undo.GetUndoCount();
    const json converted = Editor::UpdateMarkup(
        Context, {{"entityId", field},
                  {"kind", "region"},
                  {"title", "Pasture"},
                  {"members", json::array({json{{"entityId", well}, {"mode", "exclude"}}})}});
    ASSERT_FALSE(IsRefusal(converted)) << converted.dump();
    ASSERT_EQ(Undo.GetUndoCount(), steps + 1);
    EXPECT_STREQ(Undo.PeekUndoName(), "Update Mark-up");

    Undo.Undo();
    EXPECT_EQ(World.GetComponent<Components::MarkupRegion>(entity), nullptr);
    EXPECT_EQ(World.GetComponent<Components::Name>(entity)->View(), "Field");
    Undo.Redo();
    ASSERT_NE(World.GetComponent<Components::MarkupRegion>(entity), nullptr);
    EXPECT_EQ(World.GetComponent<Components::MarkupRegion>(entity)->MemberCount, 1u);
    EXPECT_EQ(World.GetComponent<Components::Name>(entity)->View(), "Pasture");

    const json knots = Editor::GetMarkup(Context, {{"entityId", field}})["markup"]["shape"]["knots"];
    const float height = World.GetComponent<Components::MarkupRegion>(entity)->ExtrudeHeight;
    const json reshaped = Editor::UpdateMarkup(
        Context, {{"entityId", field}, {"outline", Square(0.0f, 0.0f, 300.0f)}, {"extrudeHeight", 20}});
    ASSERT_FALSE(IsRefusal(reshaped)) << reshaped.dump();
    ASSERT_EQ(Undo.GetUndoCount(), steps + 2);
    Undo.Undo();
    EXPECT_EQ(Editor::GetMarkup(Context, {{"entityId", field}})["markup"]["shape"]["knots"], knots);
    EXPECT_EQ(World.GetComponent<Components::MarkupRegion>(entity)->ExtrudeHeight, height);
}

// A box converts to a region in one undo step, one way: its outline from above becomes four
// linear knots and its height the region's; its notes, status and thread stay. Undo gives the
// box back as it was.
TEST_F(MarkupRequestsTest, ABoxConvertsToARegionAsOneUndoStepKeepingItsNotes)
{
    const uint32 field = Create("Field", {{"halfExtents", {30.0f, 6.0f, 20.0f}}, {"description", "Wheat"}});
    (void)Editor::CommentOnMarkup(Context, {{"entityId", field}, {"text", "Fence it"}});
    (void)Editor::UpdateMarkup(Context, {{"entityId", field}, {"status", "Requested"}});
    const std::size_t entries = MarkupService::Get().FindNotes(World, ECS::EntityHandle(field))->Entries.size();
    const Components::Transform boxTransform = *World.GetComponent<Components::Transform>(ECS::EntityHandle(field));

    const json converted = Editor::UpdateMarkup(Context, {{"entityId", field}, {"kind", "region"}});
    ASSERT_FALSE(IsRefusal(converted)) << converted.dump();
    const ECS::EntityHandle entity(field);
    EXPECT_EQ(World.GetComponent<Components::MarkupVolume>(entity), nullptr);
    ASSERT_NE(World.GetComponent<Components::MarkupRegion>(entity), nullptr);
    EXPECT_EQ(World.GetComponent<Components::MarkupRegion>(entity)->ExtrudeHeight, 12.0f);
    const json shape = Editor::GetMarkup(Context, {{"entityId", field}})["markup"]["shape"];
    EXPECT_EQ(shape["type"], "linear");
    EXPECT_EQ(shape["knots"].size(), 4u);
    EXPECT_NEAR(shape["area"].get<float>(), 60.0f * 40.0f, 0.5f);
    EXPECT_EQ(MarkupOf(field).Status, Components::kMarkupStatusRequested);
    EXPECT_GE(MarkupService::Get().FindNotes(World, entity)->Entries.size(), entries);
    EXPECT_EQ(MarkupService::Get().FindNotes(World, entity)->Description, "Wheat");
    EXPECT_TRUE(IsRefusal(Editor::UpdateMarkup(Context, {{"entityId", field}, {"kind", "volume"}})));

    Undo.Undo();
    EXPECT_EQ(World.GetComponent<Components::MarkupRegion>(entity), nullptr);
    ASSERT_NE(World.GetComponent<Components::MarkupVolume>(entity), nullptr);
    EXPECT_TRUE(std::equal(std::begin(boxTransform.matrix), std::end(boxTransform.matrix),
                           std::begin(World.GetComponent<Components::Transform>(entity)->matrix)));
    Undo.Redo();
    EXPECT_NE(World.GetComponent<Components::MarkupRegion>(entity), nullptr);
}

// A box converted to a path keeps its thread, notes, status and color, as one undo step: the
// agent's stand-in box for a way becomes the way without losing its conversation.
TEST_F(MarkupRequestsTest, ABoxConvertsToAPathAsOneUndoStepKeepingItsThread)
{
    const uint32 road = Create("Road", {{"description", "To the gate"}, {"color", {0.2f, 0.4f, 0.8f, 1.0f}}});
    (void)Editor::CommentOnMarkup(Context, {{"entityId", road}, {"text", "Follow the river"}});
    (void)Editor::UpdateMarkup(Context, {{"entityId", road}, {"status", "Requested"}});
    const ECS::EntityHandle entity(road);
    const std::size_t entries = MarkupService::Get().FindNotes(World, entity)->Entries.size();
    const Components::Markup before = MarkupOf(road);

    const json points = json::array({json::array({0.0f, 0.0f, 0.0f}), json::array({40.0f, 0.0f, 10.0f}),
                                     json::array({80.0f, 0.0f, 0.0f})});
    const json converted = Editor::UpdateMarkup(Context, {{"entityId", road}, {"kind", "path"}, {"points", points}});
    ASSERT_FALSE(IsRefusal(converted)) << converted.dump();
    EXPECT_EQ(World.GetComponent<Components::MarkupVolume>(entity), nullptr);
    EXPECT_EQ(World.GetComponent<Components::MarkupRegion>(entity), nullptr);
    EXPECT_EQ(converted["markup"]["kind"], "path");
    EXPECT_EQ(Editor::GetMarkup(Context, {{"entityId", road}})["markup"]["shape"]["points"].size(), 3u);
    EXPECT_EQ(MarkupOf(road).Status, Components::kMarkupStatusRequested);
    EXPECT_TRUE(std::equal(std::begin(before.Color), std::end(before.Color), std::begin(MarkupOf(road).Color)))
        << "the conversion dropped the color";
    EXPECT_GE(MarkupService::Get().FindNotes(World, entity)->Entries.size(), entries) << "the thread was lost";
    EXPECT_EQ(MarkupService::Get().FindNotes(World, entity)->Description, "To the gate");

    Undo.Undo();
    EXPECT_NE(World.GetComponent<Components::MarkupVolume>(entity), nullptr) << "one undo did not bring the box back";
    EXPECT_EQ(World.GetComponent<Components::SplineComponent>(entity), nullptr);
    Undo.Redo();
    EXPECT_EQ(Editor::GetMarkup(Context, {{"entityId", road}})["markup"]["kind"], "path");
}

// A region frames over the sphere around its ring and its height.
TEST_F(MarkupRequestsTest, FrameFitsARegionsRingAndHeight)
{
    const uint32 forest = Editor::CreateMarkup(
        Context, {{"title", "Forest"}, {"kind", "region"}, {"outline", Square(-50.0f, -50.0f, 100.0f)}})["entityId"];
    SceneViewCameraPose framed{};
    const json result = Editor::FrameMarkup(Context, {{"entityId", forest}}, framed);
    ASSERT_FALSE(IsRefusal(result)) << result.dump();
    EXPECT_NEAR(result["target"][0].get<float>(), 0.0f, 0.5f);
    EXPECT_NEAR(result["target"][1].get<float>(), 4.0f, 1e-3f);
    EXPECT_NEAR(result["target"][2].get<float>(), 0.0f, 0.5f);
}

// A deleted member is skipped, so its cut closes; the editor's delete keeps the handle, so
// undo revives it and the region's list names it again.
TEST_F(MarkupRequestsTest, ADeletedMemberIsSkippedAndUndoRestoresIt)
{
    const uint32 lake = Create("Lake", {{"shape", "sphere"}, {"center", {20.0f, 0.0f, 20.0f}},
                                        {"halfExtents", {15.0f, 15.0f, 15.0f}}});
    const uint32 forest = Editor::CreateMarkup(Context, ForestParams(lake))["entityId"];
    const json shore = json::array({json::array({20.0f, 20.0f})});
    ASSERT_FALSE(Editor::ContainsInMarkup(Context, {{"entityId", forest}, {"points", shore}})["inside"][0].get<bool>());

    Undo.Execute(std::make_unique<Editor::DeleteEntitiesCommand>("Delete", &World, &Notifications,
                                                                 std::vector<ECS::EntityHandle>{ECS::EntityHandle(lake)}));
    EXPECT_FALSE(World.IsValid(ECS::EntityHandle(lake)));
    EXPECT_TRUE(Editor::ContainsInMarkup(Context, {{"entityId", forest}, {"points", shore}})["inside"][0].get<bool>());
    EXPECT_EQ(Editor::GetMarkup(Context, {{"entityId", forest}})["markup"]["shape"]["members"][0]["deleted"], true);

    Undo.Undo();
    ASSERT_TRUE(World.IsValid(ECS::EntityHandle(lake)));
    EXPECT_FALSE(Editor::ContainsInMarkup(Context, {{"entityId", forest}, {"points", shore}})["inside"][0].get<bool>());
}

// Duplicated with its clearing (its child), a region's copy lists the clearing's copy and
// keeps sharing the lake, which was not duplicated; its outline is its own spline. A region
// duplicated alone shares every member.
TEST_F(MarkupRequestsTest, ADuplicateRemapsTheMembersCopiedWithItAndSharesTheRest)
{
    const uint32 lake = Create("Lake", {{"shape", "sphere"}, {"center", {20.0f, 0.0f, 20.0f}},
                                        {"halfExtents", {15.0f, 15.0f, 15.0f}}});
    const json created = Editor::CreateMarkup(Context, ForestParams(lake));
    const ECS::EntityHandle forest(created["entityId"].get<uint32>());
    const ECS::EntityHandle clearing(created["exclusionIds"][0].get<uint32>());

    const Editor::EntityDuplicateResult duplicate = Editor::DuplicateEntitySubtreeRoots(World, {forest});
    ASSERT_EQ(duplicate.NewRoots.size(), 1u);
    const ECS::EntityHandle copy = duplicate.NewRoots[0];
    const auto* copied = World.GetComponent<Components::MarkupRegion>(copy);
    ASSERT_NE(copied, nullptr);
    ASSERT_EQ(copied->MemberCount, 2u);
    EXPECT_EQ(copied->Members[0].Entity, ECS::EntityHandle(lake));
    const ECS::EntityHandle copiedClearing = copied->Members[1].Entity;
    EXPECT_NE(copiedClearing, clearing);
    ASSERT_TRUE(World.IsValid(copiedClearing));
    EXPECT_EQ(World.GetComponent<Components::Parent>(copiedClearing)->parent, copy);
    EXPECT_NE(World.GetComponent<Components::SplineComponent>(copy)->SplineDataIndex,
              World.GetComponent<Components::SplineComponent>(forest)->SplineDataIndex);

    const json village = Editor::CreateMarkup(
        Context, {{"title", "Village"},
                  {"kind", "region"},
                  {"outline", Square(0.0f, 0.0f, 50.0f)},
                  {"members", json::array({json{{"entityId", lake}, {"mode", "exclude"}},
                                           json{{"entityId", clearing.id}, {"mode", "include"}}})}});
    const Editor::EntityDuplicateResult alone =
        Editor::DuplicateEntitySubtreeRoots(World, {ECS::EntityHandle(village["entityId"].get<uint32>())});
    ASSERT_EQ(alone.NewRoots.size(), 1u);
    const auto* shared = World.GetComponent<Components::MarkupRegion>(alone.NewRoots[0]);
    ASSERT_EQ(shared->MemberCount, 2u);
    EXPECT_EQ(shared->Members[0].Entity, ECS::EntityHandle(lake));
    EXPECT_EQ(shared->Members[1].Entity, clearing);
}

// A committed outline edit stamps the region; a knot selection (an inspector rebuild) does not.
TEST_F(MarkupRequestsTest, AnOutlineCommitStampsTheRegionAndAKnotSelectionDoesNot)
{
    const uint32 forest = Editor::CreateMarkup(
        Context, {{"title", "Forest"}, {"kind", "region"}, {"outline", Square(0.0f, 0.0f, 200.0f)}})["entityId"];
    const ECS::EntityHandle entity(forest);
    const uint32 created = MarkupOf(forest).Revision;
    Notifications.NotifyComponentChange<Components::SplineComponent>(
        &World, entity, Editor::EditorChangeNotifications::ChangeKind::InspectorRebuild);
    EXPECT_EQ(MarkupOf(forest).Revision, created);
    Notifications.NotifyComponentChange<Components::SplineComponent>(
        &World, entity, Editor::EditorChangeNotifications::ChangeKind::Commit);
    EXPECT_GT(MarkupOf(forest).Revision, created);
}
