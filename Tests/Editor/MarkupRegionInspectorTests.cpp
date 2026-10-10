// A region mark-up's authoring surfaces: the inspector's Region block (the height's commit rule,
// the member list), Convert to region on a box, the panel row's kind and outline badge, and the
// Mark-up tool's lasso.

#include "Markups/MarkupEditorBridge.h"
#include "Markups/MarkupInspector.h"
#include "Markups/MarkupKind.h"
#include "Markups/MarkupPathPort.h"
#include "Markups/MarkupPresentation.h"
#include "Markups/MarkupRegionInspector.h"
#include "Markups/MarkupRegionPort.h"
#include "Markups/MarkupTool.h"
#include "Markups/MarkupsPanelRows.h"

#include "Components/Markup/Markup.h"
#include "Components/Spline/SplineComponent.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "EditorChangeNotifications.h"
#include "InspectorRegistry.h"
#include "MarkupECS/MarkupRegionOutline.h"
#include "MarkupECS/MarkupService.h"
#include "SceneView/TransformTool.h"
#include "Mathematics/Geometry.h"
#include "Mathematics/Quaternion.h"
#include "Spline/SplineData.h"
#include "SplineECS/SplineService.h"
#include "TestEnvVar.h"
#include "TestTempDir.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/FloatField.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/Slider.h"
#include "UI/Controls/Toggle.h"
#include "UI/UiContext.h"
#include "UI/UiDispatcher.h"
#include "UndoRedo/UndoRedoService.h"

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

using namespace GameEngine;
using Components::MarkupMemberMode;
using Components::MarkupRegion;
using Mathematics::Vector2;
using Mathematics::Vector3;

namespace
{

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

bool HasClassBelow(UIElement& root, const char* className)
{
    if (root.HasClass(className))
        return true;
    for (const auto& child : root.GetChildren())
    {
        if (HasClassBelow(*child, className))
            return true;
    }
    return false;
}

class MarkupRegionInspectorTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        Testing::SetEnvVar("GE_EDITOR_USER_DATA_ROOT", (m_Root.Path() / "UserData").string().c_str());
        MarkupECS::MarkupService::Initialize();
        m_OwnsSplines = !SplineECS::SplineService::IsInitialized();
        if (m_OwnsSplines)
            SplineECS::SplineService::Initialize();
        m_Bridge = std::make_unique<Editor::MarkupEditorBridge>(
            Notifications, []() { return int64{1000}; }, []() { return std::optional<std::filesystem::path>(); });
        Editor::RegisterMarkupInspector(*m_Bridge);
    }

    void TearDown() override
    {
        m_Bridge.reset();
        if (m_OwnsSplines)
            SplineECS::SplineService::Shutdown();
        MarkupECS::MarkupService::Shutdown();
        Testing::SetEnvVar("GE_EDITOR_USER_DATA_ROOT", "");
    }

    ECS::EntityHandle BeginMarkup(const Components::Transform& transform)
    {
        const ECS::EntityHandle entity = World.CreateEntity();
        World.AddComponentImmediate(entity, transform);
        Components::WorldTransform placed{};
        std::copy(std::begin(transform.matrix), std::end(transform.matrix), std::begin(placed.matrix));
        World.AddComponentImmediate(entity, placed);
        World.AddComponentImmediate(entity, Components::Markup{});
        (void)MarkupECS::MarkupService::Get().BeginMarkup(World, entity, Components::MarkupAuthor::User, 1000);
        return entity;
    }

    ECS::EntityHandle CreateRegion(const std::vector<Vector2>& knots)
    {
        const Editor::RegionPlacement placement =
            Editor::NewRegionPlacement(knots, Components::WorldTransform{}.matrix, 0.0f);
        const ECS::EntityHandle entity = BeginMarkup(placement.Local);
        Editor::AddRegionParts(World, entity, placement.World.matrix, knots, Spline::SplineType::Linear, 8.0f);
        return entity;
    }

    // A box `size` meters across standing at `center`.
    ECS::EntityHandle CreateBox(const Vector3& center, const Vector3& size)
    {
        const ECS::EntityHandle entity = BeginMarkup(Components::Transform::FromTRS(
            center, Mathematics::Quaternion::Identity(), size));
        World.AddComponentImmediate(entity, Components::MarkupVolume{});
        return entity;
    }

    template <typename T>
    std::unique_ptr<UIElement> BuildSection(ECS::EntityHandle entity,
                                            std::vector<std::function<void()>>* frameCallbacks = nullptr,
                                            std::function<void()> requestRefresh = {})
    {
        auto section = std::make_unique<UIElement>();
        InspectorContext ctx;
        ctx.Parent = section.get();
        ctx.World = &World;
        ctx.Entity = entity;
        ctx.Undo = &Undo;
        ctx.ChangeNotifications = &Notifications;
        ctx.FrameRefreshCallbacks = frameCallbacks;
        ctx.RequestInspectorRefresh = std::move(requestRefresh);
        (*InspectorRegistry::Get().TryGetComponentInspector<T>())(ctx);
        return section;
    }

    const MarkupRegion& RegionOf(ECS::EntityHandle entity) { return *World.GetComponent<MarkupRegion>(entity); }

    Spline::SplineData& OutlineOf(ECS::EntityHandle entity)
    {
        const auto* spline = World.GetComponent<Components::SplineComponent>(entity);
        return *SplineECS::SplineService::Get().GetSplineData(
            SplineECS::SplineHandle(spline->SplineDataIndex, spline->SplineDataGeneration));
    }

    // What a scene load tells the editor: the world's entities changed.
    void NotifyLoaded()
    {
        Editor::EditorChangeNotifications::WorldStructureChangedEvent loaded{};
        loaded.world = &World;
        loaded.kind = Editor::EditorChangeNotifications::ChangeKind::Commit;
        Notifications.NotifyWorldStructureChanged(loaded);
    }

    TestUtils::ScopedTempDir m_Root{TestUtils::MakeUniqueTempDirectory("MarkupRegionInspector")};
    Editor::EditorChangeNotifications Notifications;
    Editor::UndoRedoService Undo;
    ECS::World World;
    std::unique_ptr<Editor::MarkupEditorBridge> m_Bridge;
    bool m_OwnsSplines = false;
};

const std::vector<Vector2> kForest{{0.0f, 0.0f}, {100.0f, 0.0f}, {100.0f, 100.0f}, {0.0f, 100.0f}};

} // namespace

// A height drag previews on the region every frame and its release is one undo step, which takes
// the height back to where the drag began.
TEST_F(MarkupRegionInspectorTest, TheHeightPreviewsWhileDraggedAndCommitsOneUndoStep)
{
    const ECS::EntityHandle forest = CreateRegion(kForest);
    const std::unique_ptr<UIElement> section = BuildSection<Components::Markup>(forest);
    Slider* height = nullptr;
    for (Slider* slider : FindAll<Slider>(*section))
    {
        if (slider->HasClass("markup-region-height"))
            height = slider;
    }
    ASSERT_NE(height, nullptr);

    for (const float value : {10.0f, 14.0f, 20.0f})
    {
        height->SetValueWithoutNotify(value);
        height->NotifyValueChanging();
        EXPECT_FLOAT_EQ(RegionOf(forest).ExtrudeHeight, value) << "the drag does not preview";
    }
    EXPECT_FALSE(Undo.CanUndo()) << "a preview made an undo step";
    height->SetValueWithoutNotify(24.0f);
    height->NotifyValueChanged();
    EXPECT_FLOAT_EQ(RegionOf(forest).ExtrudeHeight, 24.0f);

    Undo.Undo();
    EXPECT_FLOAT_EQ(RegionOf(forest).ExtrudeHeight, 8.0f) << "the drag took more than one undo step";
    EXPECT_FALSE(Undo.CanUndo());
}

// The Height row reads its value in meters beside the slider, follows a drag, and takes a typed
// height as one undo step, clamped to the region's range.
TEST_F(MarkupRegionInspectorTest, TheHeightRowShowsItsValueInMetersAndTakesATypedOne)
{
    const ECS::EntityHandle forest = CreateRegion(kForest);
    const std::unique_ptr<UIElement> section = BuildSection<Components::Markup>(forest);
    Slider* height = nullptr;
    for (Slider* slider : FindAll<Slider>(*section))
    {
        if (slider->HasClass("markup-region-height"))
            height = slider;
    }
    ASSERT_NE(height, nullptr);
    UIElement* row = height->GetParent();
    ASSERT_NE(row, nullptr);
    const std::vector<FloatField*> fields = FindAll<FloatField>(*row);
    ASSERT_EQ(fields.size(), 1u) << "the Height row has no value field";
    FloatField* value = fields[0];
    EXPECT_FLOAT_EQ(value->GetValue(), 8.0f);
    bool unit = false;
    for (Label* label : FindAll<Label>(*row))
        unit = unit || label->GetText() == "m";
    EXPECT_TRUE(unit) << "the Height row names no unit";

    height->SetValueWithoutNotify(14.0f);
    height->NotifyValueChanging();
    EXPECT_FLOAT_EQ(value->GetValue(), 14.0f) << "the value does not follow the drag";
    height->NotifyValueChanged();
    Undo.Undo();
    ASSERT_FLOAT_EQ(RegionOf(forest).ExtrudeHeight, 8.0f);

    value->SetValueWithoutNotify(250.0f); // typed, then committed (Enter or focus out)
    value->NotifyValueChanged();
    EXPECT_FLOAT_EQ(RegionOf(forest).ExtrudeHeight, 100.0f) << "a typed height is not clamped to the range";
    EXPECT_FLOAT_EQ(height->GetValue(), 100.0f);
    EXPECT_FLOAT_EQ(value->GetValue(), 100.0f);
    Undo.Undo();
    EXPECT_FLOAT_EQ(RegionOf(forest).ExtrudeHeight, 8.0f) << "a typed height took more than one undo step";
    EXPECT_FALSE(Undo.CanUndo());
}

// The block's rows follow the region: an outline rewritten under an open section (the agent's
// update, a knot edit, an undo), or a new mark-up the member list may offer, asks the inspector
// for one rebuild; a height change moves the slider without one.
TEST_F(MarkupRegionInspectorTest, AnOutlineChangedUnderTheOpenBlockRebuildsItOnce)
{
    const ECS::EntityHandle forest = CreateRegion(kForest);
    std::vector<std::function<void()>> frameCallbacks;
    int refreshes = 0;
    UI::UiDispatcher dispatcher;
    UI::UiContextScope scope{&dispatcher, nullptr};
    const std::unique_ptr<UIElement> section =
        BuildSection<Components::Markup>(forest, &frameCallbacks, [&refreshes]() { ++refreshes; });
    const auto runFrame = [&frameCallbacks, &dispatcher]() {
        for (const std::function<void()>& callback : frameCallbacks)
            callback();
        dispatcher.Drain();
    };
    runFrame();
    EXPECT_EQ(refreshes, 0);

    World.GetComponentForWrite<MarkupRegion>(forest)->ExtrudeHeight = 30.0f;
    runFrame();
    EXPECT_EQ(refreshes, 0) << "a height change rebuilt the block";
    for (Slider* slider : FindAll<Slider>(*section))
    {
        if (slider->HasClass("markup-region-height"))
            EXPECT_FLOAT_EQ(slider->GetValue(), 30.0f) << "the slider did not follow the height";
    }

    Editor::RewriteRegionOutline(World, forest, std::vector<Vector2>{{0, 0}, {80, 0}, {40, 60}}, std::nullopt);
    runFrame();
    runFrame();
    EXPECT_EQ(refreshes, 1);

    const std::unique_ptr<UIElement> rebuilt =
        BuildSection<Components::Markup>(forest, &frameCallbacks, [&refreshes]() { ++refreshes; });
    frameCallbacks.erase(frameCallbacks.begin());
    CreateBox(Vector3(40.0f, 4.0f, 20.0f), Vector3(10.0f, 8.0f, 10.0f));
    runFrame();
    EXPECT_EQ(refreshes, 2) << "a new overlapping mark-up did not reach the member list";
}

// Checking an overlapping mark-up makes it an Include member, its Exclude segment makes it Exclude,
// and unchecking takes it out: each one undo step that writes the region's list. The filled segment
// shows the mode; a candidate that is not a member shows neither filled, its segments disabled.
TEST_F(MarkupRegionInspectorTest, TheMemberCheckboxesAndSwitchWriteTheListOneUndoStepEach)
{
    const ECS::EntityHandle forest = CreateRegion(kForest);
    const ECS::EntityHandle lake = CreateBox(Vector3(50.0f, 4.0f, 50.0f), Vector3(20.0f, 8.0f, 20.0f));
    CreateBox(Vector3(500.0f, 4.0f, 500.0f), Vector3(20.0f, 8.0f, 20.0f)); // far away: not offered

    {
        const std::unique_ptr<UIElement> section = BuildSection<Components::Markup>(forest);
        std::vector<Toggle*> toggles;
        for (Toggle* toggle : FindAll<Toggle>(*section))
        {
            if (toggle->HasClass("markup-member-toggle"))
                toggles.push_back(toggle);
        }
        ASSERT_EQ(toggles.size(), 1u) << "the list offers the overlapping box only";
        toggles[0]->SetValue(true);
    }
    ASSERT_EQ(RegionOf(forest).MemberCount, 1u);
    EXPECT_EQ(RegionOf(forest).Members[0].Entity, lake);
    EXPECT_EQ(RegionOf(forest).Members[0].Mode, MarkupMemberMode::Include);

    const auto findSegment = [](UIElement& section, const char* segmentClass) -> Button* {
        for (Button* button : FindAll<Button>(section))
        {
            if (button->HasClass(segmentClass))
                return button;
        }
        return nullptr;
    };
    {
        const std::unique_ptr<UIElement> section = BuildSection<Components::Markup>(forest);
        Button* include = findSegment(*section, "markup-member-mode-include");
        Button* exclude = findSegment(*section, "markup-member-mode-exclude");
        ASSERT_NE(include, nullptr);
        ASSERT_NE(exclude, nullptr);
        EXPECT_TRUE(include->HasClass("active"));
        EXPECT_FALSE(exclude->HasClass("active"));
        include->TriggerClick();
        EXPECT_EQ(RegionOf(forest).Members[0].Mode, MarkupMemberMode::Include) << "the filled segment switched";
        exclude->TriggerClick();
    }
    EXPECT_EQ(RegionOf(forest).Members[0].Mode, MarkupMemberMode::Exclude);
    {
        const std::unique_ptr<UIElement> section = BuildSection<Components::Markup>(forest);
        Button* include = findSegment(*section, "markup-member-mode-include");
        Button* exclude = findSegment(*section, "markup-member-mode-exclude");
        ASSERT_NE(include, nullptr);
        ASSERT_NE(exclude, nullptr);
        EXPECT_TRUE(exclude->HasClass("active"));
        EXPECT_FALSE(include->HasClass("active"));
    }

    {
        const std::unique_ptr<UIElement> section = BuildSection<Components::Markup>(forest);
        for (Toggle* toggle : FindAll<Toggle>(*section))
        {
            if (toggle->HasClass("markup-member-toggle"))
                toggle->SetValue(false);
        }
    }
    EXPECT_EQ(RegionOf(forest).MemberCount, 0u);
    {
        const std::unique_ptr<UIElement> section = BuildSection<Components::Markup>(forest);
        Button* include = findSegment(*section, "markup-member-mode-include");
        Button* exclude = findSegment(*section, "markup-member-mode-exclude");
        ASSERT_NE(include, nullptr);
        ASSERT_NE(exclude, nullptr);
        EXPECT_FALSE(include->HasClass("active") || exclude->HasClass("active")) << "a candidate shows a mode";
        EXPECT_FALSE(include->IsEnabled() || exclude->IsEnabled()) << "a candidate's mode can be set";
    }

    Undo.Undo();
    EXPECT_EQ(RegionOf(forest).MemberCount, 1u);
    EXPECT_EQ(RegionOf(forest).Members[0].Mode, MarkupMemberMode::Exclude);
    Undo.Undo();
    EXPECT_EQ(RegionOf(forest).Members[0].Mode, MarkupMemberMode::Include);
    Undo.Undo();
    EXPECT_EQ(RegionOf(forest).MemberCount, 0u);
    EXPECT_FALSE(Undo.CanUndo());
}

// Convert to region on a box is one undo step: the box becomes a region over its footprint at its
// height, its thread says it was converted (not moved and resized), and undo gives the box back.
TEST_F(MarkupRegionInspectorTest, ConvertToRegionIsOneUndoStepAndUndoGivesTheBoxBack)
{
    const ECS::EntityHandle box = CreateBox(Vector3(10.0f, 6.0f, 20.0f), Vector3(40.0f, 12.0f, 30.0f));
    NotifyLoaded(); // the box as the scene held it: what the thread compares an edit against
    {
        const std::unique_ptr<UIElement> section = BuildSection<Components::MarkupVolume>(box);
        bool clicked = false;
        for (Button* button : FindAll<Button>(*section))
        {
            if (button->HasClass("markup-convert-to-region"))
            {
                button->TriggerClick();
                clicked = true;
            }
        }
        ASSERT_TRUE(clicked) << "a box's volume section offers no Convert to region";
    }
    const MarkupECS::MarkupNotes* notes = MarkupECS::MarkupService::Get().FindNotes(World, box);
    ASSERT_NE(notes, nullptr);
    ASSERT_FALSE(notes->Entries.empty());
    EXPECT_EQ(Editor::MarkupEntryText(notes->Entries.back()), "converted it to a region");
    ASSERT_NE(World.GetComponent<MarkupRegion>(box), nullptr);
    EXPECT_EQ(World.GetComponent<Components::MarkupVolume>(box), nullptr);
    EXPECT_FLOAT_EQ(RegionOf(box).ExtrudeHeight, 12.0f);
    const auto* spline = World.GetComponent<Components::SplineComponent>(box);
    ASSERT_NE(spline, nullptr);
    const Spline::SplineData* data = SplineECS::SplineService::Get().GetSplineData(
        SplineECS::SplineHandle(spline->SplineDataIndex, spline->SplineDataGeneration));
    ASSERT_NE(data, nullptr);
    EXPECT_EQ(data->Points.size(), 4u) << "the box's footprint is four knots";
    EXPECT_FLOAT_EQ(World.GetComponent<Components::WorldTransform>(box)->matrix[13], 0.0f)
        << "the region stands where the box's base stood";

    Undo.Undo();
    EXPECT_EQ(World.GetComponent<MarkupRegion>(box), nullptr);
    ASSERT_NE(World.GetComponent<Components::MarkupVolume>(box), nullptr);
    EXPECT_FALSE(Undo.CanUndo());

    // A sphere has no corners to keep: no button, and the conversion refuses.
    const ECS::EntityHandle sphere = CreateBox(Vector3(0.0f, 10.0f, 0.0f), Vector3(20.0f, 20.0f, 20.0f));
    World.GetComponentForWrite<Components::MarkupVolume>(sphere)->Shape = Components::MarkupVolumeShape::Sphere;
    const std::unique_ptr<UIElement> sphereSection = BuildSection<Components::MarkupVolume>(sphere);
    EXPECT_FALSE(HasClassBelow(*sphereSection, "markup-convert-to-region"));
    EXPECT_FALSE(Editor::ConvertBoxMarkupToRegion(World, sphere, &Undo, &Notifications));
}

// The panel row shows a region's kind, and a region down to two knots says its outline needs a
// third.
TEST_F(MarkupRegionInspectorTest, ARegionRowShowsItsKindAndAnOutlineThatNeedsAPoint)
{
    const ECS::EntityHandle forest = CreateRegion(kForest);
    const std::unique_ptr<UIElement> row = Editor::BuildMarkupRow(*m_Bridge, World, forest, 1000, false, {});
    EXPECT_TRUE(HasClassBelow(*row, "markup-kind-region"));
    EXPECT_FALSE(HasClassBelow(*row, "markups-outline-badge"));

    const auto* spline = World.GetComponent<Components::SplineComponent>(forest);
    Spline::SplineData* data = SplineECS::SplineService::Get().GetSplineData(
        SplineECS::SplineHandle(spline->SplineDataIndex, spline->SplineDataGeneration));
    data->Points.resize(2);
    const std::unique_ptr<UIElement> twoKnots = Editor::BuildMarkupRow(*m_Bridge, World, forest, 1000, false, {});
    bool badge = false;
    for (Label* label : FindAll<Label>(*twoKnots))
        badge = badge || (label->HasClass("markups-outline-badge") && label->GetText() == "outline needs three points");
    EXPECT_TRUE(badge);
}

// A path's Mark-up section carries its Path block (its points and Edit path) and its panel row the
// path glyph; a region's carries neither.
TEST_F(MarkupRegionInspectorTest, APathShowsItsBlockAndItsKind)
{
    const std::vector<Vector3> points{{0.0f, 0.0f, 0.0f}, {30.0f, 0.0f, 5.0f}, {60.0f, 0.0f, -5.0f}};
    const ECS::EntityHandle road = BeginMarkup(Editor::NewPathPlacement(points));
    Editor::AddPathParts(World, road, World.GetComponent<Components::Transform>(road)->matrix, points,
                         Spline::SplineType::Linear);
    const std::unique_ptr<UIElement> section = BuildSection<Components::Markup>(road);
    EXPECT_TRUE(HasClassBelow(*section, "markup-path-edit"));
    bool points3 = false;
    for (Label* label : FindAll<Label>(*section))
        points3 = points3 || (label->HasClass("markup-region-value") && label->GetText() == "3");
    EXPECT_TRUE(points3) << "the Path block does not show the path's three points";
    EXPECT_TRUE(HasClassBelow(*Editor::BuildMarkupRow(*m_Bridge, World, road, 1000, false, {}), "markup-kind-path"));

    const ECS::EntityHandle forest = CreateRegion(kForest);
    EXPECT_FALSE(HasClassBelow(*BuildSection<Components::Markup>(forest), "markup-path-edit"));
}

// The spline inspector's Closed row says why it cannot change in its owner's words: a region's
// outline is always closed, a path stays open.
TEST_F(MarkupRegionInspectorTest, TheClosedRowSaysWhyInTheOwnersWords)
{
    const ECS::EntityHandle forest = CreateRegion(kForest);
    const std::vector<Vector3> points{{0.0f, 0.0f, 0.0f}, {30.0f, 0.0f, 5.0f}};
    const ECS::EntityHandle road = BeginMarkup(Editor::NewPathPlacement(points));
    Editor::AddPathParts(World, road, World.GetComponent<Components::Transform>(road)->matrix, points,
                         Spline::SplineType::Linear);
    const Editor::SplineOwnerClaim region = Editor::QueryMarkupSpline(World, forest);
    const Editor::SplineOwnerClaim path = Editor::QueryMarkupSpline(World, road);
    ASSERT_TRUE(region.Claimed && path.Claimed);
    EXPECT_EQ(region.ClosedRowTooltip, std::string_view("A region's outline is always closed"));
    EXPECT_EQ(path.ClosedRowTooltip, std::string_view("A path stays open"));
}

// The move gizmo on a region stands at the region's own origin, its label point, not at its first
// knot: the region moves about where its title stands.
TEST_F(MarkupRegionInspectorTest, TheMoveGizmoOnARegionStandsAtItsOrigin)
{
    const ECS::EntityHandle forest = CreateRegion(kForest);
    Editor::SceneTools::TransformTool tool(World);
    tool.SetTargetEntity(forest);
    tool.RefreshPivotFromTargetEntity();
    Editor::SceneTools::GizmoCollector collector;
    tool.GatherGizmos(collector);
    ASSERT_EQ(collector.GetGizmos().size(), 1u);
    const auto* translate = dynamic_cast<Editor::SceneTools::TransformTranslateGizmo*>(collector.GetGizmos()[0]);
    ASSERT_NE(translate, nullptr);
    const float* origin = World.GetComponent<Components::WorldTransform>(forest)->matrix;
    EXPECT_NEAR(translate->GetPivot().x, origin[12], 1.0e-3f);
    EXPECT_NEAR(translate->GetPivot().y, origin[13], 1.0e-3f);
    EXPECT_NEAR(translate->GetPivot().z, origin[14], 1.0e-3f);
}

// A region is always closed: one whose outline arrives open, from a scene file or a component
// write, is closed again when the editor hears of it (a load, a committed change).
TEST_F(MarkupRegionInspectorTest, ARegionWhoseOutlineArrivesOpenIsClosedAgain)
{
    const ECS::EntityHandle forest = CreateRegion(kForest);
    OutlineOf(forest).Closed = false;
    NotifyLoaded();
    EXPECT_TRUE(OutlineOf(forest).Closed) << "a region loaded open stayed open";

    OutlineOf(forest).Closed = false;
    Editor::EditorChangeNotifications::ComponentChangedEvent written{};
    written.world = &World;
    written.entity = forest;
    written.componentType = ECS::GetComponentTypeId<Components::SplineComponent>();
    written.kind = Editor::EditorChangeNotifications::ChangeKind::Commit;
    Notifications.NotifyComponentChanged(written);
    EXPECT_TRUE(OutlineOf(forest).Closed) << "a region written open stayed open";
}

// A recorded lasso, a 200 m circle drawn at the spline brush's 1 m spacing with a hand's wobble,
// becomes tens of knots, every one on the stroke, the outline within the tolerance of it.
TEST(MarkupLasso, ARecordedStrokeKeepsTensOfKnotsWithinTheTolerance)
{
    constexpr float kRadius = 100.0f;
    constexpr float kTurn = 6.28318531f; // radians in a circle
    std::vector<Vector3> stroke;
    for (int i = 0; i < 620; ++i)
    {
        const float angle = kTurn * static_cast<float>(i) / 628.0f; // released short of the start
        const float wobble = 0.3f * std::sin(static_cast<float>(i) * 0.7f);
        stroke.emplace_back((kRadius + wobble) * std::cos(angle), 12.0f, (kRadius + wobble) * std::sin(angle));
    }
    const std::vector<Vector2> knots = Editor::SimplifyLassoStroke(stroke);
    EXPECT_GE(knots.size(), 10u);
    EXPECT_LE(knots.size(), 60u) << "a 200 m lasso keeps tens of knots, not hundreds";
    EXPECT_FALSE(MarkupECS::CheckRegionOutline(knots).has_value()) << "the lasso is not a valid outline";
    // The tolerance is 1 % of the 283 m diagonal, clamped to 2.83 m: no stroke sample is farther
    // from the outline than that.
    for (const Vector3& sample : stroke)
        EXPECT_LE(MarkupECS::RegionOutlineDistance(Vector2(sample.x, sample.z), knots), 2.9f);
    EXPECT_NEAR(MarkupECS::RegionOutlineArea(knots), 0.5f * kTurn * kRadius * kRadius, 0.015f * kTurn * kRadius * kRadius);
}

// A lasso drawn as a figure eight, simplified as the tool simplifies it, is an outline the region
// rule refuses: the tool shows the notice and makes no region.
TEST(MarkupLasso, AFigureEightLassoIsRefused)
{
    constexpr float kTurn = 6.28318531f; // radians in a circle
    std::vector<Vector3> stroke;
    for (int i = 0; i < 400; ++i)
    {
        const float angle = kTurn * static_cast<float>(i) / 400.0f;
        stroke.emplace_back(80.0f * std::sin(angle), 0.0f, 40.0f * std::sin(2.0f * angle));
    }
    const std::vector<Vector2> knots = Editor::SimplifyLassoStroke(stroke);
    ASSERT_GE(knots.size(), 4u);
    EXPECT_TRUE(MarkupECS::CheckRegionOutline(knots).has_value()) << "a crossing lasso made a region";
}
