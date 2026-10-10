// What the Scene View shows of the mark-ups: MarkupGizmo's volumes (nothing for nothing
// visible, the hidden set, the highlight, the status color under an unseen update, the fill
// cap, a volume the camera is inside, the fill off and one circle under the glow),
// MarkupRenderFeature's glow (the compat profile, the hand gate, the shapes, the composite and the
// contact line read back, the highlight's rim and body, the shader's rim and contact terms),
// MarkupLabelOverlay's distance rule, held pills and a member's pill yielding to its region's,
// and the frame the Mark-ups panel's double-click takes.

#include "Markups/MarkupEditorBridge.h"
#include "Markups/MarkupGizmo.h"
#include "Markups/MarkupLabelOverlay.h"
#include "Markups/MarkupPresentation.h"
#include "Markups/MarkupRenderFeature.h"

#include "Components/Markup/Markup.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "EditorChangeNotifications.h"
#include "Engine/Rendering/RenderServices.h"
#include "MarkupECS/MarkupService.h"
#include "Mathematics/HalfFloat.h"
#include "Mathematics/Matrix4x4.h"
#include "Rendering/CameraTypes.h"
#include "Rendering/Common/Utils.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Core/RenderGraph/RGResourcePool.h"
#include "Rendering/Core/RenderGraph/RGTransientPool.h"
#include "Rendering/Core/RenderGraph/RGUploadRing.h"
#include "Rendering/Materials/ShaderProfileDefines.h"
#include "SceneView/SceneViewEvents.h"
#include "SceneView/SceneViewFraming.h"
#include "SceneView/SceneViewGizmos.h"
#include "SceneViewController.h"
#include "StagedTestPaths.h"
#include "TestDeviceHelper.h"
#include "TestEnvVar.h"
#include "TestTempDir.h"

#include <gtest/gtest.h>

#include "GlslShim.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <tuple>
#include <vector>

// The glow's rim term as markup_glow.frag runs it, extracted from markup_glow_rim.glsl.
namespace GameEngine::GlslShim::MarkupGlow
{
#include "MarkupGlowRimExtracted.h"
}

using namespace GameEngine;
using Components::Markup;
using Components::MarkupAuthor;
using Components::MarkupVolume;
using Components::MarkupVolumeShape;
using Editor::MarkupEditorBridge;
using Editor::MarkupGizmo;
using Editor::MarkupHighlightState;
using Editor::MarkupLabelOverlay;
using Editor::MarkupRenderFeature;
using Editor::SceneTools::GizmoLineGroup;
using Editor::SceneTools::GizmoRenderContext;
using Editor::SceneTools::GizmoTriangleGroup;
using MarkupECS::MarkupService;
using Mathematics::Vector3;

namespace
{

constexpr Rendering::ViewId kViewId = 31;
constexpr std::size_t kBoxTriangles = 12;
constexpr std::size_t kBoxLines = 12;

class MarkupSceneViewTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        GameEngine::Testing::SetEnvVar("GE_EDITOR_USER_DATA_ROOT", (m_Root.Path() / "UserData").string().c_str());
        MarkupService::Initialize();
        m_Bridge = std::make_unique<MarkupEditorBridge>(
            m_Notifications, [this]() { return m_Now; }, []() { return std::optional<std::filesystem::path>(); });
    }

    void TearDown() override
    {
        m_Bridge.reset();
        MarkupService::Shutdown();
        GameEngine::Testing::SetEnvVar("GE_EDITOR_USER_DATA_ROOT", "");
    }

    // A volume mark-up whose transform places the unit shape: `size` is a box's full extents,
    // a sphere's size.x its diameter.
    ECS::EntityHandle CreateMarkup(const Vector3& center, const Vector3& size,
                                   MarkupVolumeShape shape = MarkupVolumeShape::Box)
    {
        const ECS::EntityHandle entity = m_World.CreateEntity();
        Components::WorldTransform transform;
        transform.matrix[0] = size.x;
        transform.matrix[5] = size.y;
        transform.matrix[10] = size.z;
        transform.matrix[12] = center.x;
        transform.matrix[13] = center.y;
        transform.matrix[14] = center.z;
        m_World.AddComponentImmediate(entity, transform);
        m_World.AddComponentImmediate(entity, Markup{});
        m_World.AddComponentImmediate(entity, MarkupVolume{shape});
        (void)MarkupService::Get().BeginMarkup(m_World, entity, MarkupAuthor::User, m_Now);
        return entity;
    }

    // One frame of the gizmo pass, seen from `camera`.
    void DrawFrame(const MarkupHighlightState& highlight = {}, const Vector3& camera = Vector3(0.0f, 50.0f, -200.0f))
    {
        Editor::SceneTools::ResetGizmoLineGroups(kViewId);
        Editor::SceneTools::ResetGizmoTriangleGroups(kViewId);
        GizmoRenderContext context(kViewId, kViewId, &camera);
        m_Gizmo.Draw(context, m_World, *m_Bridge, highlight, camera, ++m_Frame);
    }

    static std::vector<GizmoTriangleGroup> TriangleGroups()
    {
        const auto* groups = Editor::SceneTools::GetGizmoTriangleGroups(kViewId);
        return groups ? *groups : std::vector<GizmoTriangleGroup>{};
    }

    static std::vector<GizmoLineGroup> LineGroups()
    {
        const auto* groups = Editor::SceneTools::GetGizmoLineGroups(kViewId);
        return groups ? *groups : std::vector<GizmoLineGroup>{};
    }

    static std::size_t TriangleCount(const std::vector<GizmoTriangleGroup>& groups)
    {
        std::size_t count = 0;
        for (const GizmoTriangleGroup& group : groups)
            count += group.vertices.size() / 9;
        return count;
    }

    static std::size_t LineCount(const std::vector<GizmoLineGroup>& groups)
    {
        std::size_t count = 0;
        for (const GizmoLineGroup& group : groups)
            count += group.vertices.size() / 6;
        return count;
    }

    // A 1280 x 720 perspective view at `camera` looking down +Z, as the Scene View writes it.
    static Editor::SceneTools::ScenePointerEvent ViewFrom(const Vector3& camera)
    {
        Editor::SceneTools::ScenePointerEvent view;
        view.viewW = 1280.0f;
        view.viewH = 720.0f;
        view.cameraPos = camera;
        view.cameraRight = Vector3(1.0f, 0.0f, 0.0f);
        view.cameraUp = Vector3(0.0f, 1.0f, 0.0f);
        view.cameraForward = Vector3(0.0f, 0.0f, 1.0f);
        view.tanHalfFovY = std::tan(0.5f * 60.0f * 3.14159265f / 180.0f);
        return view;
    }

    bool RenderGlow(Rendering::IDevice& device, float sceneDepth, std::vector<std::uint16_t>& texels);

    GameEngine::TestUtils::ScopedTempDir m_Root{GameEngine::TestUtils::MakeUniqueTempDirectory("MarkupSceneView")};
    Editor::EditorChangeNotifications m_Notifications;
    ECS::World m_World;
    int64 m_Now = 1000;
    uint64 m_Frame = 0;
    std::unique_ptr<MarkupEditorBridge> m_Bridge;
    MarkupGizmo m_Gizmo;
};

// A reverse-Z perspective camera at `position` looking along +Z (`forwardZ` 1) or -Z (-1), as
// the Scene View fills it, for a `width` x `height` target.
Rendering::CameraData CameraLookingAlongZ(const Vector3& position, float forwardZ, float width, float height)
{
    const Mathematics::Matrix4x4 view =
        Mathematics::Matrix4x4::LookAt(position, position + Vector3(0.0f, 0.0f, forwardZ), Vector3(0.0f, 1.0f, 0.0f));
    const Mathematics::Matrix4x4 projection =
        Mathematics::Matrix4x4::PerspectiveReverseZ(60.0f * 3.14159265f / 180.0f, width / height, 0.1f, 10000.0f);
    Rendering::CameraData camera{};
    std::memcpy(camera.view, view.Data(), sizeof(camera.view));
    std::memcpy(camera.proj, projection.Data(), sizeof(camera.proj));
    std::memcpy(camera.viewProj, (projection * view).Data(), sizeof(camera.viewProj));
    std::memcpy(camera.viewRel, camera.view, sizeof(camera.viewRel));
    std::memcpy(camera.viewProjRel, camera.viewProj, sizeof(camera.viewProjRel));
    camera.cameraPos[0] = position.x;
    camera.cameraPos[1] = position.y;
    camera.cameraPos[2] = position.z;
    return camera;
}

// The highest fill vertex's z, or the lowest float when no fill was drawn.
float FarthestFillZ(const std::vector<GizmoTriangleGroup>& groups)
{
    float farthest = std::numeric_limits<float>::lowest();
    for (const GizmoTriangleGroup& group : groups)
    {
        for (std::size_t i = 2; i < group.vertices.size(); i += 3)
            farthest = std::max(farthest, group.vertices[i]);
    }
    return farthest;
}

} // namespace

// Zero cost with nothing to show: the pass packs no group, so it declares no draw.
TEST_F(MarkupSceneViewTest, NoMarkupEmitsNoGroup)
{
    DrawFrame();
    EXPECT_TRUE(TriangleGroups().empty());
    EXPECT_TRUE(LineGroups().empty());
}

TEST_F(MarkupSceneViewTest, HiddenMarkupEmitsNothing)
{
    const ECS::EntityHandle markup = CreateMarkup(Vector3(0.0f, 0.0f, 0.0f), Vector3(20.0f, 8.0f, 20.0f));
    m_Bridge->SetHidden(m_World, {&markup, 1}, true);
    DrawFrame();
    EXPECT_TRUE(TriangleGroups().empty());
    EXPECT_TRUE(LineGroups().empty());
}

// Five boxes of one status are one fill group and one outline group, whatever their count.
TEST_F(MarkupSceneViewTest, MarkupsOfOneStatusShareOneFillAndOneOutlineGroup)
{
    for (int i = 0; i < 5; ++i)
        CreateMarkup(Vector3(30.0f * static_cast<float>(i), 0.0f, 0.0f), Vector3(20.0f, 8.0f, 20.0f));
    DrawFrame();
    const std::vector<GizmoTriangleGroup> fills = TriangleGroups();
    const std::vector<GizmoLineGroup> outlines = LineGroups();
    ASSERT_EQ(fills.size(), 1u);
    ASSERT_EQ(outlines.size(), 1u);
    EXPECT_EQ(TriangleCount(fills), 5 * kBoxTriangles);
    EXPECT_EQ(LineCount(outlines), 5 * kBoxLines);
    EXPECT_EQ(fills[0].layer, 1);
    EXPECT_EQ(fills[0].depthMode, Editor::SceneTools::GizmoDepthMode::SceneDepth);
    EXPECT_EQ(outlines[0].depthMode, Editor::SceneTools::GizmoDepthMode::SceneDepth);
    EXPECT_LT(fills[0].color.a, outlines[0].color.a);
}

// The selected mark-up draws a stronger fill in a group of its own and a doubled outline.
TEST_F(MarkupSceneViewTest, SelectedMarkupDrawsAStrongerFillAndADoubledOutline)
{
    CreateMarkup(Vector3(0.0f, 0.0f, 0.0f), Vector3(20.0f, 8.0f, 20.0f));
    const ECS::EntityHandle selected = CreateMarkup(Vector3(40.0f, 0.0f, 0.0f), Vector3(20.0f, 8.0f, 20.0f));
    MarkupHighlightState highlight;
    highlight.Selected = {&selected, 1};
    DrawFrame(highlight);

    const std::vector<GizmoTriangleGroup> fills = TriangleGroups();
    ASSERT_EQ(fills.size(), 2u);
    EXPECT_NE(fills[0].color, fills[1].color);
    EXPECT_EQ(TriangleCount({fills[0]}), kBoxTriangles);
    EXPECT_EQ(TriangleCount({fills[1]}), kBoxTriangles);
    EXPECT_EQ(LineCount(LineGroups()), kBoxLines + 2 * kBoxLines);
}

// A hovered mark-up reads apart from the rest and from the selection: a lifted fill and one
// outline in the editor's accent, where the resting one keeps its status color.
TEST_F(MarkupSceneViewTest, HoveredMarkupDrawsALiftedFillAndOneAccentOutline)
{
    CreateMarkup(Vector3(0.0f, 0.0f, 0.0f), Vector3(20.0f, 8.0f, 20.0f));
    const ECS::EntityHandle hovered = CreateMarkup(Vector3(40.0f, 0.0f, 0.0f), Vector3(20.0f, 8.0f, 20.0f));
    m_Bridge->HoverMarkup(hovered);
    DrawFrame(m_Bridge->GetHighlight());

    const std::vector<GizmoTriangleGroup> fills = TriangleGroups();
    ASSERT_EQ(fills.size(), 2u);
    EXPECT_EQ(TriangleCount(fills), 2 * kBoxTriangles);
    const std::vector<GizmoLineGroup> outlines = LineGroups();
    ASSERT_EQ(outlines.size(), 2u);
    EXPECT_EQ(LineCount(outlines), 2 * kBoxLines);
    const auto isAccent = [](const Color& color) {
        return color.r == Editor::kMarkupAccentRgb[0] && color.g == Editor::kMarkupAccentRgb[1] &&
               color.b == Editor::kMarkupAccentRgb[2];
    };
    EXPECT_NE(isAccent(outlines[0].color), isAccent(outlines[1].color));
}

// An unseen agent update is the label's ring: the volume keeps its status color, so a
// Problem stays red on the volume whatever the agent wrote.
TEST_F(MarkupSceneViewTest, AnUnseenUpdateLeavesTheVolumeInItsStatusColor)
{
    const ECS::EntityHandle markup = CreateMarkup(Vector3(0.0f, 0.0f, 0.0f), Vector3(20.0f, 8.0f, 20.0f));
    DrawFrame();
    const Color restOutline = LineGroups().at(0).color;

    ++m_Now;
    ASSERT_TRUE(MarkupService::Get().AddComment(m_World, markup, MarkupAuthor::Agent, m_Now, "Moved it"));
    ASSERT_TRUE(m_Bridge->HasUnseenUpdate(m_World, markup));
    DrawFrame();
    EXPECT_EQ(LineGroups().at(0).color, restOutline);
}

// Section 4's cost rule: past kFillCap only the nearest get a fill, every one an outline.
TEST_F(MarkupSceneViewTest, FillsStopAtTheCapAndOutlinesDoNot)
{
    const std::size_t count = MarkupGizmo::kFillCap + 1;
    for (std::size_t i = 0; i < count; ++i)
        CreateMarkup(Vector3(0.0f, 0.0f, 30.0f * static_cast<float>(i)), Vector3(20.0f, 8.0f, 20.0f));
    DrawFrame();
    EXPECT_EQ(TriangleCount(TriangleGroups()), MarkupGizmo::kFillCap * kBoxTriangles);
    EXPECT_EQ(LineCount(LineGroups()), count * kBoxLines);
}

// The cap fills the nearest: the farthest of kFillCap + 1 draws its outline only, unless it
// is highlighted, which comes first.
TEST_F(MarkupSceneViewTest, TheCapFillsHighlightedMarkupsThenTheNearest)
{
    std::vector<ECS::EntityHandle> markups;
    for (std::size_t i = 0; i < MarkupGizmo::kFillCap + 1; ++i)
        markups.push_back(CreateMarkup(Vector3(0.0f, 0.0f, 30.0f * static_cast<float>(i)), Vector3(20.0f, 8.0f, 20.0f)));
    const float farthestZ = 30.0f * static_cast<float>(MarkupGizmo::kFillCap);

    DrawFrame();
    EXPECT_LT(FarthestFillZ(TriangleGroups()), farthestZ - 10.0f) << "the farthest mark-up got a fill";

    MarkupHighlightState highlight;
    highlight.Selected = {&markups.back(), 1};
    DrawFrame(highlight);
    EXPECT_NEAR(FarthestFillZ(TriangleGroups()), farthestZ + 10.0f, 1e-3f) << "the highlighted farthest got no fill";
    EXPECT_EQ(TriangleCount(TriangleGroups()), MarkupGizmo::kFillCap * kBoxTriangles);
}

// Mark-ups are edited in the edit world: in play mode neither a volume nor a label draws.
TEST_F(MarkupSceneViewTest, NothingDrawsInPlayMode)
{
    CreateMarkup(Vector3(0.0f, -4.0f, 100.0f), Vector3(20.0f, 8.0f, 20.0f));
    m_Bridge->SetPlayModeProvider([]() { return true; });
    DrawFrame();
    EXPECT_TRUE(TriangleGroups().empty());
    EXPECT_TRUE(LineGroups().empty());
    std::vector<MarkupLabelOverlay::Placement> labels;
    MarkupLabelOverlay::Collect(m_World, *m_Bridge, {}, ViewFrom(Vector3(0.0f, 0.0f, 0.0f)), labels);
    EXPECT_TRUE(labels.empty());
}

// A label shows while the volume's bounding sphere spans kMinAngularRadius from the camera.
TEST_F(MarkupSceneViewTest, LabelShowsWithinTheDistanceRuleOnly)
{
    // A 20 x 8 x 20 m box: a bounding radius of |(10, 4, 10)| m, its top 4 m above its center.
    const float radius = std::sqrt(10.0f * 10.0f + 4.0f * 4.0f + 10.0f * 10.0f);
    const float range = radius / MarkupLabelOverlay::kMinAngularRadius;
    const ECS::EntityHandle near = CreateMarkup(Vector3(0.0f, -4.0f, range * 0.9f), Vector3(20.0f, 8.0f, 20.0f));
    CreateMarkup(Vector3(0.0f, -4.0f, range * 1.1f), Vector3(20.0f, 8.0f, 20.0f));

    std::vector<MarkupLabelOverlay::Placement> labels;
    MarkupLabelOverlay::Collect(m_World, *m_Bridge, {}, ViewFrom(Vector3(0.0f, 0.0f, 0.0f)), labels);
    ASSERT_EQ(labels.size(), 1u);
    EXPECT_EQ(labels[0].Entity, near);
    EXPECT_NEAR(labels[0].X, 640.0f, 0.5f);
    EXPECT_NEAR(labels[0].Y, 360.0f, 0.5f);

    m_Bridge->SetHidden(m_World, {&near, 1}, true);
    MarkupLabelOverlay::Collect(m_World, *m_Bridge, {}, ViewFrom(Vector3(0.0f, 0.0f, 0.0f)), labels);
    EXPECT_TRUE(labels.empty());
}

// A framed volume keeps its label: its top leaves the view, its center does not, and the
// pill is held at the view's top edge above the center.
TEST_F(MarkupSceneViewTest, AVolumeWhoseTopLeavesTheViewKeepsItsLabelAtTheEdge)
{
    const ECS::EntityHandle lake =
        CreateMarkup(Vector3(0.0f, 0.0f, 10.0f), Vector3(24.0f, 24.0f, 24.0f), MarkupVolumeShape::Sphere);
    std::vector<MarkupLabelOverlay::Placement> labels;
    MarkupLabelOverlay::Collect(m_World, *m_Bridge, {}, ViewFrom(Vector3(0.0f, 0.0f, 0.0f)), labels);
    ASSERT_EQ(labels.size(), 1u);
    EXPECT_EQ(labels[0].Entity, lake);
    EXPECT_NEAR(labels[0].X, 640.0f, 0.5f);
    EXPECT_FLOAT_EQ(labels[0].Y, MarkupLabelOverlay::kMinAnchorY);
}

// From inside a volume its fill would wash over the whole view: it draws its outline only, and
// its pill holds at the view's top center while its top is out of view.
TEST_F(MarkupSceneViewTest, CameraInsideAVolumeDrawsItsOutlineOnlyAndKeepsItsLabel)
{
    const Vector3 camera(0.0f, 0.0f, 0.0f);
    const ECS::EntityHandle markup = CreateMarkup(Vector3(0.0f, 0.0f, 0.0f), Vector3(20.0f, 8.0f, 20.0f));
    DrawFrame({}, camera);
    EXPECT_TRUE(TriangleGroups().empty());
    EXPECT_EQ(LineCount(LineGroups()), kBoxLines);

    std::vector<MarkupLabelOverlay::Placement> labels;
    MarkupLabelOverlay::Collect(m_World, *m_Bridge, {}, ViewFrom(camera), labels);
    ASSERT_EQ(labels.size(), 1u);
    EXPECT_EQ(labels[0].Entity, markup);
    EXPECT_FLOAT_EQ(labels[0].X, 640.0f);
    EXPECT_FLOAT_EQ(labels[0].Y, MarkupLabelOverlay::kMinAnchorY);
}

// The container draws its pills in order: the selected and the hovered mark-ups' come after the
// rest, so a pill overlapping theirs never covers them.
TEST_F(MarkupSceneViewTest, SelectedAndHoveredLabelsComeLast)
{
    const ECS::EntityHandle nearest = CreateMarkup(Vector3(0.0f, -4.0f, 100.0f), Vector3(20.0f, 8.0f, 20.0f));
    const ECS::EntityHandle middle = CreateMarkup(Vector3(0.0f, -4.0f, 200.0f), Vector3(20.0f, 8.0f, 20.0f));
    const ECS::EntityHandle farthest = CreateMarkup(Vector3(0.0f, -4.0f, 300.0f), Vector3(20.0f, 8.0f, 20.0f));
    MarkupHighlightState highlight;
    highlight.Selected = {&nearest, 1};
    highlight.Hovered = middle;

    std::vector<MarkupLabelOverlay::Placement> labels;
    MarkupLabelOverlay::Collect(m_World, *m_Bridge, highlight, ViewFrom(Vector3(0.0f, 0.0f, 0.0f)), labels);
    ASSERT_EQ(labels.size(), 3u);
    EXPECT_EQ(labels[0].Entity, farthest);
    EXPECT_FALSE(labels[0].Selected || labels[0].Hovered);
    EXPECT_EQ(labels[1].Entity, nearest);
    EXPECT_TRUE(labels[1].Selected);
    EXPECT_EQ(labels[2].Entity, middle);
    EXPECT_TRUE(labels[2].Hovered);
}

// A pill held at a pane's top edge stays inside the pane and steps aside from the axis widget
// in the pane's top-right corner, toward the middle of the view.
TEST_F(MarkupSceneViewTest, APillKeepsClearOfThePanesAxisWidget)
{
    const float viewWidth = 1280.0f;
    const float halfWidth = 60.0f;
    const Mathematics::Rect widget{viewWidth - 94.0f, 14.0f, 80.0f, 80.0f};
    EXPECT_FLOAT_EQ(MarkupLabelOverlay::PillCenterX(viewWidth - 10.0f, 4.0f, halfWidth, viewWidth, {}),
                    viewWidth - halfWidth);
    const float centerX = MarkupLabelOverlay::PillCenterX(viewWidth - 10.0f, 4.0f, halfWidth, viewWidth, widget);
    EXPECT_LE(centerX + halfWidth, widget.X);
    // Below the widget the pill stands over its point.
    EXPECT_FLOAT_EQ(MarkupLabelOverlay::PillCenterX(viewWidth - 100.0f, 200.0f, halfWidth, viewWidth, widget),
                    viewWidth - 100.0f);
}

// A region member's pill that overlaps its region's stands under it, placed again, and is hidden
// when the view has no room below; one clear of its region's pill stays where it is.
TEST_F(MarkupSceneViewTest, AMembersPillYieldsToItsRegionsPill)
{
    const ECS::EntityHandle region = CreateMarkup(Vector3(0.0f, -4.0f, 100.0f), Vector3(20.0f, 8.0f, 20.0f));
    const ECS::EntityHandle member = CreateMarkup(Vector3(0.0f, -4.0f, 110.0f), Vector3(4.0f, 8.0f, 4.0f));
    std::vector<MarkupLabelOverlay::Placement> labels(2);
    labels[0].Entity = member;
    labels[0].X = 410.0f;
    labels[0].Region = region;
    labels[1].Entity = region;
    labels[1].X = 400.0f;
    const float viewWidth = 1280.0f;
    const float viewHeight = 720.0f;
    using Box = MarkupLabelOverlay::PillBox;

    std::vector<Box> boxes{{410.0f, 100.0f, 50.0f, false}, {400.0f, 100.0f, 40.0f, false}};
    MarkupLabelOverlay::YieldToRegions(labels, boxes, viewWidth, viewHeight, {});
    EXPECT_FALSE(boxes[0].Hidden);
    EXPECT_FLOAT_EQ(boxes[0].Top, 100.0f + MarkupLabelOverlay::kPillHeight + MarkupLabelOverlay::kPillStackGap);
    EXPECT_FLOAT_EQ(boxes[0].CenterX, 410.0f);
    EXPECT_FLOAT_EQ(boxes[1].Top, 100.0f);

    boxes = {{410.0f, viewHeight - 30.0f, 50.0f, false}, {400.0f, viewHeight - 30.0f, 40.0f, false}};
    MarkupLabelOverlay::YieldToRegions(labels, boxes, viewWidth, viewHeight, {});
    EXPECT_TRUE(boxes[0].Hidden);
    EXPECT_FALSE(boxes[1].Hidden);

    boxes = {{600.0f, 100.0f, 50.0f, false}, {400.0f, 100.0f, 40.0f, false}};
    MarkupLabelOverlay::YieldToRegions(labels, boxes, viewWidth, viewHeight, {});
    EXPECT_FALSE(boxes[0].Hidden);
    EXPECT_FLOAT_EQ(boxes[0].Top, 100.0f);

    // Two members over the region's pill, and a third already standing on the first row under it:
    // each yielding member takes the next row clear of its siblings.
    const ECS::EntityHandle second = CreateMarkup(Vector3(0.0f, -4.0f, 120.0f), Vector3(4.0f, 8.0f, 4.0f));
    const ECS::EntityHandle third = CreateMarkup(Vector3(0.0f, -4.0f, 130.0f), Vector3(4.0f, 8.0f, 4.0f));
    labels.resize(4);
    labels[2].Entity = second;
    labels[2].X = 395.0f;
    labels[2].Region = region;
    labels[3].Entity = third;
    labels[3].X = 420.0f;
    labels[3].Region = region;
    const float row = MarkupLabelOverlay::kPillHeight + MarkupLabelOverlay::kPillStackGap;
    boxes = {{410.0f, 100.0f, 50.0f, false},
             {400.0f, 100.0f, 40.0f, false},
             {395.0f, 100.0f, 30.0f, false},
             {420.0f, 100.0f + row, 40.0f, false}};
    MarkupLabelOverlay::YieldToRegions(labels, boxes, viewWidth, viewHeight, {});
    EXPECT_FLOAT_EQ(boxes[0].Top, 100.0f + 2.0f * row);
    EXPECT_FLOAT_EQ(boxes[2].Top, 100.0f + 3.0f * row);
    EXPECT_FLOAT_EQ(boxes[3].Top, 100.0f + row);
    EXPECT_FALSE(boxes[0].Hidden || boxes[2].Hidden || boxes[3].Hidden);
}

// An agent update the viewer has not seen puts the badge's dot on the label, as on the
// panel's row; the viewer's look takes it off.
TEST_F(MarkupSceneViewTest, AnUnseenUpdateMarksTheLabelUntilSeen)
{
    const ECS::EntityHandle markup = CreateMarkup(Vector3(0.0f, -4.0f, 100.0f), Vector3(20.0f, 8.0f, 20.0f));
    std::vector<MarkupLabelOverlay::Placement> labels;
    MarkupLabelOverlay::Collect(m_World, *m_Bridge, {}, ViewFrom(Vector3(0.0f, 0.0f, 0.0f)), labels);
    ASSERT_EQ(labels.size(), 1u);
    EXPECT_FALSE(labels[0].Unseen);

    ++m_Now;
    ASSERT_TRUE(MarkupService::Get().AddComment(m_World, markup, MarkupAuthor::Agent, m_Now, "Moved it"));
    MarkupLabelOverlay::Collect(m_World, *m_Bridge, {}, ViewFrom(Vector3(0.0f, 0.0f, 0.0f)), labels);
    ASSERT_EQ(labels.size(), 1u);
    EXPECT_TRUE(labels[0].Unseen);

    m_Bridge->MarkSeen(m_World, markup);
    MarkupLabelOverlay::Collect(m_World, *m_Bridge, {}, ViewFrom(Vector3(0.0f, 0.0f, 0.0f)), labels);
    ASSERT_EQ(labels.size(), 1u);
    EXPECT_FALSE(labels[0].Unseen);
}

// The framed Lake fits a 3067 x 936 pane at the default 60 degree vertical field of view: its
// bounding sphere's angular radius stays inside the narrower (vertical) half angle.
TEST_F(MarkupSceneViewTest, AFramedSphereFitsTheFieldOfView)
{
    const ECS::EntityHandle lake =
        CreateMarkup(Vector3(30.0f, 2.0f, 48.0f), Vector3(24.0f, 24.0f, 24.0f), MarkupVolumeShape::Sphere);
    const std::optional<Editor::MarkupWorldVolume> volume = Editor::ReadMarkupWorldVolume(m_World, lake);
    ASSERT_TRUE(volume.has_value());
    const float tanHalfVertical = Editor::kMarkupDefaultTanHalfFov;
    const float tanHalfHorizontal = tanHalfVertical * 3067.0f / 936.0f;
    const float distance = Editor::MarkupFrameDistance(*volume, std::min(tanHalfVertical, tanHalfHorizontal));
    const float angularRadius = std::asin(volume->BoundingRadius / distance);
    EXPECT_LT(angularRadius, std::atan(tanHalfVertical));
}

// The panel's double-click frames the volume's bounds, not the entity's origin: its center,
// far enough along the view direction that its bounding sphere fits the field of view.
TEST_F(MarkupSceneViewTest, FrameTargetsTheVolumeBounds)
{
    const ECS::EntityHandle markup = CreateMarkup(Vector3(100.0f, 5.0f, 50.0f), Vector3(20.0f, 8.0f, 20.0f));
    const std::optional<Editor::MarkupWorldVolume> volume = Editor::ReadMarkupWorldVolume(m_World, markup);
    ASSERT_TRUE(volume.has_value());

    // Looking down +Z, as a Scene View yaw of 90 degrees does.
    SceneViewCameraPose framed{};
    ASSERT_TRUE(Editor::ComputeLookAtPose(volume->Center, Vector3(0.0f, 0.0f, 1.0f),
                                          Editor::MarkupFrameDistance(*volume, Editor::kMarkupDefaultTanHalfFov), framed));
    // sin 30 degrees = 0.5: two bounding radii, with the 15 % margin.
    const float distance = std::sqrt(10.0f * 10.0f + 4.0f * 4.0f + 10.0f * 10.0f) * 2.0f * 1.15f;
    EXPECT_NEAR(framed.Distance, distance, 1e-3f);
    EXPECT_NEAR(framed.Pos[0], 100.0f, 1e-3f);
    EXPECT_NEAR(framed.Pos[1], 5.0f, 1e-3f);
    EXPECT_NEAR(framed.Pos[2], 50.0f - distance, 1e-3f);
    EXPECT_NEAR(framed.YawDeg, 90.0f, 1e-3f);
    EXPECT_NEAR(framed.PitchDeg, 0.0f, 1e-3f);
}

// Desktop: the glow draws the bodies, so the gizmo draws the outline and no fill.
TEST_F(MarkupSceneViewTest, UnderTheGlowTheGizmoDrawsTheOutlineAndNoFill)
{
    CreateMarkup(Vector3(0.0f, -4.0f, 100.0f), Vector3(20.0f, 8.0f, 20.0f));
    m_Gizmo.SetDrawsFills(false);
    DrawFrame();
    EXPECT_TRUE(TriangleGroups().empty());
    EXPECT_EQ(LineCount(LineGroups()), kBoxLines);
}

// Under the glow a sphere's outline is the one circle facing the camera, not the three rings the
// filled sphere draws: the glow's rim already draws its silhouette.
TEST_F(MarkupSceneViewTest, UnderTheGlowASphereDrawsOneCircle)
{
    const Vector3 center(0.0f, -4.0f, 100.0f);
    const Vector3 camera(0.0f, 50.0f, -200.0f);
    CreateMarkup(center, Vector3(24.0f, 24.0f, 24.0f), MarkupVolumeShape::Sphere);
    // How far the outline leaves the plane through the center facing the camera.
    const auto farthestOffPlane = [&]()
    {
        const Vector3 toCamera = (camera - center).NormalizeOrZero();
        float farthest = 0.0f;
        for (const GizmoLineGroup& group : LineGroups())
        {
            for (std::size_t i = 0; i + 2 < group.vertices.size(); i += 3)
            {
                const Vector3 point(group.vertices[i], group.vertices[i + 1], group.vertices[i + 2]);
                farthest = std::max(farthest, std::abs(Vector3::Dot(point - center, toCamera)));
            }
        }
        return farthest;
    };
    DrawFrame({}, camera);
    EXPECT_GT(farthestOffPlane(), 6.0f) << "a filled sphere draws its three rings";
    m_Gizmo.SetDrawsFills(false);
    DrawFrame({}, camera);
    EXPECT_GT(LineCount(LineGroups()), 0u);
    EXPECT_LT(farthestOffPlane(), 1e-3f) << "under the glow the sphere draws the circle facing the camera";
}

// The glow is a desktop feature: on the compat profile the registry never holds it, so the
// gizmo's fill stands in.
TEST_F(MarkupSceneViewTest, TheGlowRegistersOffTheCompatProfileOnly)
{
    const bool previous = Rendering::IsCompatShaderProfile();
    {
        Engine::Renderer::RenderServices services;
        Rendering::SetCompatShaderProfile(true);
        EXPECT_EQ(MarkupRenderFeature::Ensure(services), nullptr);
        EXPECT_EQ(services.GetFeature<MarkupRenderFeature>(), nullptr);
        Rendering::SetCompatShaderProfile(false);
        MarkupRenderFeature* glow = MarkupRenderFeature::Ensure(services);
        ASSERT_NE(glow, nullptr);
        EXPECT_EQ(services.GetFeature<MarkupRenderFeature>(), glow);
    }
    Rendering::SetCompatShaderProfile(previous);
}

// The shaders staged in the build root (its Shaders/), whatever resolver and working directory
// an earlier test in this process left.
static std::filesystem::path StagedShaderPath(const std::filesystem::path& relativePath)
{
    const std::filesystem::path staged = GameEngine::TestPaths::StagedRoot() / relativePath;
    std::error_code error;
    return std::filesystem::exists(staged, error) ? staged : std::filesystem::path();
}

// The hand gate: with no mark-up that draws a body in view the glow declares no pass (none at
// all, or one behind the camera); with one in view, its body pass and its composite.
TEST_F(MarkupSceneViewTest, TheGlowDeclaresNothingWithoutAVisibleMarkup)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    const Rendering::SamplerHandle sampler =
        device->CreateSampler(Rendering::SamplerDesc::MaterialLinearClamp("Test.MarkupGlow"));
    ASSERT_TRUE(sampler.IsValid());
    const Rendering::Utils::ShaderPathResolverFunc previousResolver = Rendering::Utils::GetShaderPathResolver();
    Rendering::Utils::SetShaderPathResolver(&StagedShaderPath);
    std::size_t passesWithout = 0;
    std::size_t passesBehind = 0;
    std::size_t passesWith = 0;
    bool drawnWithout = true;
    bool drawnBehind = true;
    bool drawnWith = false;
    {
        Rendering::RenderGraph::RGResourcePool persistent(device.get());
        Rendering::RenderGraph::RGTransientPool transient(device.get());
        Rendering::RenderGraph::RGUploadRing ring(device.get(), 2, 4096);
        Rendering::RenderGraph::RGFrame frame(device.get(), &persistent, &transient, &ring);
        frame.BeginFrame(0);
        Rendering::TextureDesc color{};
        color.width = 64;
        color.height = 64;
        color.format = static_cast<uint32_t>(Rendering::TextureFormat::R16G16B16A16_FLOAT);
        color.usage = static_cast<uint32_t>(Rendering::TextureUsage::RenderTarget);
        color.sampleCount = 1;
        Rendering::TextureDesc depthDesc = color;
        depthDesc.format = static_cast<uint32_t>(Rendering::TextureFormat::D32_FLOAT);
        depthDesc.usage =
            static_cast<uint32_t>(Rendering::TextureUsage::DepthStencil | Rendering::TextureUsage::ShaderResource);
        const auto output = frame.CreateTexture("Test.MarkupGlow.Output", color);
        const auto depth = frame.CreateTexture("Test.MarkupGlow.Depth", depthDesc);
        const Vector3 cameraPos(0.0f, 50.0f, -200.0f);
        const Rendering::CameraData facing = CameraLookingAlongZ(cameraPos, 1.0f, 64.0f, 64.0f);
        const Rendering::CameraData away = CameraLookingAlongZ(cameraPos, -1.0f, 64.0f, 64.0f);

        MarkupRenderFeature glow;
        std::size_t passesBefore = frame.Graph().PassCount();
        drawnWithout = glow.DeclareView(frame, output, depth, sampler, facing, m_World, *m_Bridge, 0);
        passesWithout = frame.Graph().PassCount() - passesBefore;
        CreateMarkup(Vector3(0.0f, -4.0f, 100.0f), Vector3(20.0f, 8.0f, 20.0f));
        passesBefore = frame.Graph().PassCount();
        drawnBehind = glow.DeclareView(frame, output, depth, sampler, away, m_World, *m_Bridge, 0);
        passesBehind = frame.Graph().PassCount() - passesBefore;
        passesBefore = frame.Graph().PassCount();
        drawnWith = glow.DeclareView(frame, output, depth, sampler, facing, m_World, *m_Bridge, 0);
        passesWith = frame.Graph().PassCount() - passesBefore;
    }
    Rendering::Utils::SetShaderPathResolver(previousResolver);
    device->DestroySampler(sampler);
    device->Shutdown();
    EXPECT_FALSE(drawnWithout);
    EXPECT_EQ(passesWithout, 0u);
    EXPECT_FALSE(drawnBehind) << "a mark-up behind the camera declared the glow";
    EXPECT_EQ(passesBehind, 0u);
    EXPECT_TRUE(drawnWith) << "the instrument: one visible mark-up must declare the glow";
    EXPECT_EQ(passesWith, 2u);
}

// The selected mark-up takes the strongest rim; the hovered one keeps the plain rim and draws its
// body lifted toward white at a denser alpha, the same for a box and a sphere; the rest the
// plain rim and body.
TEST_F(MarkupSceneViewTest, TheHighlightFollowsTheHighlightStateAlikeForBoxAndSphere)
{
    const ECS::EntityHandle plain = CreateMarkup(Vector3(0.0f, 0.0f, 100.0f), Vector3(20.0f, 8.0f, 20.0f));
    const ECS::EntityHandle selected = CreateMarkup(Vector3(0.0f, 0.0f, 200.0f), Vector3(20.0f, 8.0f, 20.0f));
    const ECS::EntityHandle hovered = CreateMarkup(Vector3(0.0f, 0.0f, 300.0f), Vector3(20.0f, 8.0f, 20.0f));
    (void)plain;
    MarkupHighlightState highlight;
    highlight.Selected = {&selected, 1};
    highlight.Hovered = hovered;
    std::vector<Editor::MarkupDrawItem> items;
    std::vector<uint32> excludedScratch;
    const std::size_t bodies =
        Editor::CollectMarkupDrawItems(m_World, *m_Bridge, highlight, Vector3(0.0f, 50.0f, -200.0f), nullptr, 0, items,
                                       excludedScratch);
    std::vector<MarkupRenderFeature::Draw> draws;
    MarkupRenderFeature glow;
    glow.BuildDraws(items, bodies, draws);
    ASSERT_EQ(draws.size(), 3u);
    static_assert(MarkupRenderFeature::kHoveredBodyAlpha > MarkupRenderFeature::kBodyAlpha);
    // Farthest first: the hovered, the selected, then the plain one.
    EXPECT_NEAR(draws[0].Model[14], 300.0f, 1e-3f);
    EXPECT_EQ(draws[0].Rim[1], 1.0f);
    EXPECT_EQ(draws[0].Color[3], MarkupRenderFeature::kHoveredBodyAlpha);
    EXPECT_GT(draws[0].Color[0] + draws[0].Color[1] + draws[0].Color[2],
              draws[2].Color[0] + draws[2].Color[1] + draws[2].Color[2]);
    EXPECT_NEAR(draws[1].Model[14], 200.0f, 1e-3f);
    EXPECT_EQ(draws[1].Rim[1], MarkupRenderFeature::kSelectedRimGain);
    EXPECT_EQ(draws[1].Color[3], MarkupRenderFeature::kBodyAlpha);
    EXPECT_NEAR(draws[2].Model[14], 100.0f, 1e-3f);
    EXPECT_EQ(draws[2].Rim[1], 1.0f);
    EXPECT_EQ(draws[2].Color[3], MarkupRenderFeature::kBodyAlpha);

    // A hovered sphere takes the same body as a hovered box.
    m_World.AddComponentImmediate(hovered, Components::MarkupVolume{Components::MarkupVolumeShape::Sphere, {}});
    const std::size_t sphereBodies =
        Editor::CollectMarkupDrawItems(m_World, *m_Bridge, highlight, Vector3(0.0f, 50.0f, -200.0f), nullptr, 0, items,
                                       excludedScratch);
    std::vector<MarkupRenderFeature::Draw> sphereDraws;
    glow.BuildDraws(items, sphereBodies, sphereDraws);
    ASSERT_EQ(sphereDraws.size(), 3u);
    for (int channel = 0; channel < 4; ++channel)
        EXPECT_EQ(sphereDraws[0].Color[channel], draws[0].Color[channel]);
    EXPECT_EQ(sphereDraws[0].Rim[1], draws[0].Rim[1]);
}

// Each shape draws as itself: a sphere with the sphere's tessellation and its fresnel rim, a box
// with the box's 36 vertices and its edge rim; a mirrored box (a negative-determinant transform)
// draws with a positive-determinant model, so its near side stays front-facing.
TEST_F(MarkupSceneViewTest, EachShapeDrawsAsItself)
{
    CreateMarkup(Vector3(0.0f, 0.0f, 100.0f), Vector3(20.0f, 8.0f, 20.0f));
    CreateMarkup(Vector3(0.0f, 0.0f, 200.0f), Vector3(24.0f, 24.0f, 24.0f), MarkupVolumeShape::Sphere);
    CreateMarkup(Vector3(0.0f, 0.0f, 300.0f), Vector3(-20.0f, 8.0f, 20.0f));
    std::vector<Editor::MarkupDrawItem> items;
    std::vector<uint32> excludedScratch;
    const std::size_t bodies =
        Editor::CollectMarkupDrawItems(m_World, *m_Bridge, {}, Vector3(0.0f, 50.0f, -200.0f), nullptr, 0, items,
                                       excludedScratch);
    std::vector<MarkupRenderFeature::Draw> draws;
    MarkupRenderFeature glow;
    glow.BuildDraws(items, bodies, draws);
    ASSERT_EQ(draws.size(), 3u);
    // Farthest first: the mirrored box, the sphere, the box.
    const auto determinant = [](const float (&m)[16])
    {
        const Vector3 x(m[0], m[1], m[2]);
        const Vector3 y(m[4], m[5], m[6]);
        const Vector3 z(m[8], m[9], m[10]);
        return Vector3::Dot(Vector3::Cross(x, y), z);
    };
    EXPECT_EQ(draws[0].Rim[3], 0.0f);
    EXPECT_EQ(draws[0].VertexCount, MarkupRenderFeature::kBoxVertexCount);
    EXPECT_GT(determinant(draws[0].Model), 0.0f) << "a mirrored box would draw its far side";
    EXPECT_EQ(draws[1].Rim[3], 1.0f);
    EXPECT_EQ(draws[1].VertexCount, MarkupRenderFeature::kSphereVertexCount);
    EXPECT_EQ(draws[2].Rim[3], 0.0f);
    EXPECT_EQ(draws[2].VertexCount, MarkupRenderFeature::kBoxVertexCount);
    EXPECT_GT(determinant(draws[2].Model), 0.0f);
}

// The readback scene: from the origin looking down +Z at a 64 x 64 pane, a box whose near face
// (z = 19) is centered at texel (15, 32) and a sphere centered at texel (48, 32).
constexpr uint32_t kReadbackSize = 64;
constexpr uint32_t kReadbackBoxTexel = 15;
constexpr uint32_t kReadbackSphereTexel = 48;
constexpr std::array<float, 4> kReadbackScene = {0.2f, 0.4f, 0.6f, 1.0f};

Rendering::CameraData ReadbackCamera()
{
    return CameraLookingAlongZ(Vector3(), 1.0f, static_cast<float>(kReadbackSize), static_cast<float>(kReadbackSize));
}

// The device depth the readback camera writes for a surface at view depth `z`.
float ReadbackDeviceDepth(float z)
{
    const Rendering::CameraData camera = ReadbackCamera();
    return (camera.proj[10] * z + camera.proj[14]) / z;
}

// The texel x of the readback pane's center row in `texels` (RGBA16F), channel `channel`.
float CenterRowTexel(const std::vector<std::uint16_t>& texels, uint32_t x, std::size_t channel)
{
    const std::size_t texel = static_cast<std::size_t>(kReadbackSize / 2) * kReadbackSize + x;
    return Mathematics::HalfToFloat(texels[texel * 4 + channel]);
}

// One executed frame of the glow over a pane cleared to kReadbackScene and to `sceneDepth`, read
// back into `texels`; false (with `texels` empty) when the glow declared nothing.
bool MarkupSceneViewTest::RenderGlow(Rendering::IDevice& device, float sceneDepth, std::vector<std::uint16_t>& texels)
{
    texels.clear();
    const Rendering::SamplerHandle sampler =
        device.CreateSampler(Rendering::SamplerDesc::MaterialLinearClamp("Test.MarkupGlow"));
    const std::size_t readbackBytes =
        static_cast<std::size_t>(kReadbackSize) * kReadbackSize * 4 * sizeof(std::uint16_t);
    const Rendering::BufferHandle readback = device.CreateReadbackBuffer(readbackBytes, "Test.MarkupGlow.Readback");
    if (!sampler.IsValid() || !readback.IsValid())
        return false;
    const Rendering::Utils::ShaderPathResolverFunc previousResolver = Rendering::Utils::GetShaderPathResolver();
    Rendering::Utils::SetShaderPathResolver(&StagedShaderPath);
    bool drawn = false;
    {
        Rendering::RenderGraph::RGResourcePool persistent(&device);
        Rendering::RenderGraph::RGTransientPool transient(&device);
        Rendering::RenderGraph::RGUploadRing ring(&device, 2, 4096);
        Rendering::RenderGraph::RGFrame frame(&device, &persistent, &transient, &ring);
        frame.BeginFrame(0);
        Rendering::TextureDesc color{};
        color.width = kReadbackSize;
        color.height = kReadbackSize;
        color.format = static_cast<uint32_t>(Rendering::TextureFormat::R16G16B16A16_FLOAT);
        color.usage =
            static_cast<uint32_t>(Rendering::TextureUsage::RenderTarget | Rendering::TextureUsage::TransferSrc);
        color.sampleCount = 1;
        Rendering::TextureDesc depthDesc = color;
        depthDesc.format = static_cast<uint32_t>(Rendering::TextureFormat::D32_FLOAT);
        depthDesc.usage =
            static_cast<uint32_t>(Rendering::TextureUsage::DepthStencil | Rendering::TextureUsage::ShaderResource);
        const auto output = frame.CreateTexture("Test.MarkupGlow.Output", color);
        const auto depth = frame.CreateTexture("Test.MarkupGlow.Depth", depthDesc);
        frame.AddPass(
            "Test.MarkupGlow.Scene", static_cast<int32_t>(Rendering::PassPhase::kOverlay),
            [&](Rendering::RenderGraph::RGPassBuilder& pass)
            {
                Rendering::RenderGraph::RGAttachmentOps colorOps{};
                colorOps.Load = Rendering::RenderGraph::RGLoadOp::Clear;
                std::copy(kReadbackScene.begin(), kReadbackScene.end(), colorOps.Clear.Color);
                pass.AttachColor(0, output, colorOps);
                Rendering::RenderGraph::RGAttachmentOps depthOps{};
                depthOps.Load = Rendering::RenderGraph::RGLoadOp::Clear;
                depthOps.Clear.Depth = sceneDepth;
                pass.AttachDepth(depth, depthOps);
            },
            [](Rendering::RenderGraph::RGContext&) {});
        MarkupRenderFeature glow;
        drawn = glow.DeclareView(frame, output, depth, sampler, ReadbackCamera(), m_World, *m_Bridge, 0);
        frame.AddPass(
            "Test.MarkupGlow.Readback", static_cast<int32_t>(Rendering::PassPhase::kFinalize),
            [&](Rendering::RenderGraph::RGPassBuilder& pass)
            {
                pass.Read(output, Rendering::RenderGraph::RGTextureRead::CopySrc);
                pass.PreventCulling();
            },
            [output, readback](Rendering::RenderGraph::RGContext& context)
            {
                context.Cmd->CopyTextureSubresourceToBuffer(context.GetTexture(output), 0, 0, readback, kReadbackSize,
                                                            kReadbackSize);
            });
        frame.Execute();
        device.WaitForIdle();
    }
    Rendering::Utils::SetShaderPathResolver(previousResolver);
    if (const auto* mapped = static_cast<const std::uint16_t*>(device.MapBuffer(readback)); drawn && mapped)
    {
        texels.assign(mapped, mapped + readbackBytes / sizeof(std::uint16_t));
        device.UnmapBuffer(readback);
    }
    device.DestroyBuffer(readback);
    device.DestroySampler(sampler);
    return drawn;
}

// The glow composites premultiplied alpha-over: where a body faces the camera with no rim (a box
// face's center, a sphere's center), the pane reads color * kBodyAlpha + scene * (1 - kBodyAlpha).
// A straight-alpha composite (SrcAlpha) would square the coverage; a sphere drawing its far side
// would add its full rim.
TEST_F(MarkupSceneViewTest, TheGlowCompositesPremultipliedOverTheScene)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    const ECS::EntityHandle box = CreateMarkup(Vector3(-6.0f, 0.0f, 20.0f), Vector3(6.0f, 6.0f, 2.0f));
    const ECS::EntityHandle sphere =
        CreateMarkup(Vector3(6.0f, 0.0f, 20.0f), Vector3(6.0f, 6.0f, 6.0f), MarkupVolumeShape::Sphere);
    std::vector<std::uint16_t> texels;
    // Depth cleared to reverse-Z far: nothing occludes and no contact line.
    const bool drawn = RenderGlow(*device, 0.0f, texels);
    device->Shutdown();
    ASSERT_TRUE(drawn) << "the instrument: two mark-ups in view must declare the glow";
    for (const auto& [entity, x, shape] : {std::tuple(box, kReadbackBoxTexel, "box"),
                                           std::tuple(sphere, kReadbackSphereTexel, "sphere")})
    {
        const std::array<float32, 3> rgb = Editor::MarkupDisplayRgb(*m_World.GetComponent<Markup>(entity));
        for (std::size_t channel = 0; channel < 3; ++channel)
        {
            const float expected = rgb[channel] * MarkupRenderFeature::kBodyAlpha +
                                   kReadbackScene[channel] * (1.0f - MarkupRenderFeature::kBodyAlpha);
            EXPECT_NEAR(CenterRowTexel(texels, x, channel), expected, 4e-3f) << shape << ", channel " << channel;
        }
    }
}

// The contact line: a box face 1 cm in front of the scene surface blends its color over the body
// at kRimBudget times MarkupGlowContactBand of the gap; with the scene far away it reads the
// plain body (TheGlowCompositesPremultipliedOverTheScene).
TEST_F(MarkupSceneViewTest, TheGlowDrawsAContactLineWhereABodyMeetsTheScene)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    const ECS::EntityHandle box = CreateMarkup(Vector3(-6.0f, 0.0f, 20.0f), Vector3(6.0f, 6.0f, 2.0f));
    constexpr float kGap = 0.01f;
    std::vector<std::uint16_t> texels;
    const bool drawn = RenderGlow(*device, ReadbackDeviceDepth(19.0f + kGap), texels);
    device->Shutdown();
    ASSERT_TRUE(drawn) << "the instrument: a mark-up in view must declare the glow";
    const float alpha = MarkupRenderFeature::kBodyAlpha;
    // A face parallel to a parallel surface: the gap is the same at every pixel, so the band takes
    // its 5 cm floor.
    const float contact = MarkupRenderFeature::kRimBudget *
                          GlslShim::MarkupGlow::MarkupGlowContactBand(
                              kGap, GlslShim::MarkupGlow::MarkupGlowContactBandWidth(0.0f, 19.0f));
    ASSERT_GT(contact, 0.5f) << "the instrument: 1 cm must sit well inside the band";
    const float coverage = alpha * (1.0f - contact) + contact;
    const std::array<float32, 3> rgb = Editor::MarkupDisplayRgb(*m_World.GetComponent<Markup>(box));
    for (std::size_t channel = 0; channel < 3; ++channel)
    {
        const float expected = rgb[channel] * coverage + kReadbackScene[channel] * (1.0f - coverage);
        // The interpolated face depth sits a few millimeters off the analytic 19 m: the band's
        // slope there moves the result by under 1e-2.
        EXPECT_NEAR(CenterRowTexel(texels, kReadbackBoxTexel, channel), expected, 1e-2f) << "channel " << channel;
    }
}

// A box's rim term from its edges: no rim at a face's center, a partial rim near an edge, the
// full rim (saturated by the gain) at a corner. The face is 10 x 4 m (half extents 5 and 2), so
// the rim band is kMarkupGlowBoxRimWidth x 2 m deep from each edge.
TEST(MarkupGlowRim, ABoxRimFollowsTheFaceEdges)
{
    using GlslShim::MarkupGlow::MarkupGlowBoxEdgeFacing;
    using GlslShim::MarkupGlow::MarkupGlowRim;
    const float band = GlslShim::MarkupGlow::kMarkupGlowBoxRimWidth * 2.0f;
    const auto rimAt = [](float u, float v, float gain)
    { return MarkupGlowRim(MarkupGlowBoxEdgeFacing(u, v, 5.0f, 2.0f), MarkupRenderFeature::kRimPower, gain); };

    EXPECT_EQ(MarkupGlowBoxEdgeFacing(0.0f, 0.0f, 5.0f, 2.0f), 1.0f);
    EXPECT_EQ(rimAt(0.0f, 0.0f, MarkupRenderFeature::kSelectedRimGain), 0.0f) << "a face's center has no rim";

    // Half the band from the v edge (the nearer one): facing 0.5.
    const float nearEdgeV = 1.0f - 0.5f * band / 2.0f;
    EXPECT_FLOAT_EQ(MarkupGlowBoxEdgeFacing(0.0f, nearEdgeV, 5.0f, 2.0f), 0.5f);
    EXPECT_FLOAT_EQ(rimAt(0.0f, nearEdgeV, 1.0f), std::pow(0.5f, MarkupRenderFeature::kRimPower));

    EXPECT_EQ(MarkupGlowBoxEdgeFacing(1.0f, -1.0f, 5.0f, 2.0f), 0.0f);
    EXPECT_EQ(rimAt(1.0f, -1.0f, 1.0f), 1.0f) << "a corner takes the full rim";
    EXPECT_EQ(rimAt(1.0f, -1.0f, MarkupRenderFeature::kSelectedRimGain), 1.0f) << "the rim saturates at 1";
}

// The contact band's depth: its 5 cm floor where the gap barely changes across the screen, two
// pixels' worth of gap where it changes faster (the line reads at every distance), and no deeper
// than kMarkupGlowContactBandMaxDepthFraction of the view depth across a jump in scene depth.
TEST(MarkupGlowRim, TheContactBandKeepsTwoPixelsAtEveryDistance)
{
    using namespace GlslShim::MarkupGlow;
    EXPECT_EQ(MarkupGlowContactBandWidth(0.001f, 20.0f), kMarkupGlowContactBandMeters);
    EXPECT_FLOAT_EQ(MarkupGlowContactBandWidth(0.5f, 200.0f), kMarkupGlowContactBandPixels * 0.5f);
    EXPECT_FLOAT_EQ(MarkupGlowContactBandWidth(1000.0f, 200.0f), kMarkupGlowContactBandMaxDepthFraction * 200.0f);
    // A point one pixel's gap from the surface sits inside the band.
    EXPECT_GT(MarkupGlowContactBand(0.5f, MarkupGlowContactBandWidth(0.5f, 200.0f)), 0.0f);
}

// The shader's rim term against its definition, (1 - n.v)^power times the gain, saturated at 1,
// at three normals: facing the camera, 60 degrees off it, and grazing.
TEST(MarkupGlowRim, MatchesItsReferenceAtThreeNormals)
{
    const auto reference = [](float nDotV, float gain)
    { return std::min(std::pow(1.0f - std::clamp(nDotV, 0.0f, 1.0f), MarkupRenderFeature::kRimPower) * gain, 1.0f); };
    for (const float nDotV : {1.0f, 0.5f, 0.0f})
    {
        for (const float gain : {1.0f, MarkupRenderFeature::kSelectedRimGain})
        {
            const float rim = GlslShim::MarkupGlow::MarkupGlowRim(nDotV, MarkupRenderFeature::kRimPower, gain);
            EXPECT_FLOAT_EQ(rim, reference(nDotV, gain)) << "n.v " << nDotV << ", gain " << gain;
            EXPECT_LE(rim, 1.0f) << "the rim adds no more than the rim budget";
        }
    }
    EXPECT_EQ(GlslShim::MarkupGlow::MarkupGlowRim(1.0f, MarkupRenderFeature::kRimPower, 1.0f), 0.0f);
    EXPECT_EQ(GlslShim::MarkupGlow::MarkupGlowRim(0.0f, MarkupRenderFeature::kRimPower,
                                                  MarkupRenderFeature::kSelectedRimGain),
              1.0f);
}
