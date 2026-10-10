#include <cmath>
#include <cstring>
#include <vector>
#include <gtest/gtest.h>

#include <GLFW/glfw3.h>

#include "Core/Application.h"
#include "Core/Engine.h"

#include "SceneView/TransformTool.h"
#include "SceneViewController.h"

#include "Components/Hierarchy.h"
#include "Components/Rendering/LocalBounds.h"
#include "Components/Spline/SplineComponent.h"
#include "Components/Transform.h"
#include "Editor/Settings/SplineEditorSettings.h"
#include "ECS/ECSTemplates.h" // template implementations for World::GetComponent<T>
#include "ECS/Entity.h"
#include "Mathematics/Quaternion.h"
#include "Mathematics/Vector3.h"
#include "SplineECS/SplineService.h"

#include "ECSModules/Rendering/Systems/TransformHierarchySystem.h"
#include "Scripting/ScriptingABI.h"

using GameEngine::ApplicationConfig;
using GameEngine::EngineCore;

using GameEngine::ECS::Entity;
using GameEngine::ECS::World;

using GameEngine::Components::Parent;
using GameEngine::Components::LocalBounds;
using GameEngine::Components::SplineComponent;
using GameEngine::Components::Transform;
using GameEngine::Components::WorldTransform;

using GameEngine::Engine::Renderer::TransformHierarchySystem;

using GameEngine::Mathematics::Quaternion;
using GameEngine::Mathematics::Vector3;

using GameEngine::Editor::SceneTools::GetGizmoLineGroups;
using GameEngine::Editor::SceneTools::GetGizmoTriangleGroups;
using GameEngine::Editor::SceneTools::GizmoCollector;
using GameEngine::Editor::SceneTools::GizmoDepthMode;
using GameEngine::Editor::SceneTools::GizmoHit;
using GameEngine::Editor::SceneTools::GizmoHitKind;
using GameEngine::Editor::SceneTools::GizmoRenderContext;
using GameEngine::Editor::SceneTools::PointerButton;
using GameEngine::Editor::SceneTools::PointerPhase;
using GameEngine::Editor::SceneTools::ResetGizmoLineGroups;
using GameEngine::Editor::SceneTools::ResetGizmoTriangleGroups;
using GameEngine::Editor::SceneTools::ScenePointerEvent;
using GameEngine::Editor::SceneTools::TransformMode;
using GameEngine::Editor::SceneTools::TransformTool;
using GameEngine::Editor::SceneTools::TransformTranslateGizmo;
using GameEngine::Rendering::CameraId;
using GameEngine::Rendering::ViewId;

namespace
{

class ScopedSplineCenterPivotSetting
{
  public:
    explicit ScopedSplineCenterPivotSetting(bool value)
        : m_Previous(GameEngine::Editor::SplineEditorSettings::Get().GetPivotFromSplineCenter())
    {
        GameEngine::Editor::SplineEditorSettings::Get().SetPivotFromSplineCenter(value);
    }

    ~ScopedSplineCenterPivotSetting()
    {
        GameEngine::Editor::SplineEditorSettings::Get().SetPivotFromSplineCenter(m_Previous);
    }

  private:
    bool m_Previous = false;
};

class SceneViewTransformToolTests : public ::testing::Test
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
        if (!GameEngine::SplineECS::SplineService::IsInitialized())
            GameEngine::SplineECS::SplineService::Initialize();
    }

    void SetUp() override
    {
        World* world = GetWorld();
        ASSERT_NE(world, nullptr);
        world->Clear();
    }

    World* GetWorld() const
    {
        return EngineCore::GetInstance().EnsurePrimaryWorld();
    }

    static constexpr float kPosEpsilon = 1.0e-4f;
    static constexpr float kRotEpsilon = 1.0e-4f;
};

TEST_F(SceneViewTransformToolTests, RootEntityTranslationUpdatesWorldAndPivot)
{
    World* world = GetWorld();

    Entity entity = world->Create();

    Vector3 startPos(1.0f, 2.0f, -3.0f);
    Quaternion startRot(1.0f, 0.0f, 0.0f, 0.0f);
    Vector3 startScale(1.0f, 1.0f, 1.0f);

    Transform local = Transform::FromTRS(startPos, startRot, startScale);
    entity.Set(local);

    world->ProcessCommands();

    TransformHierarchySystem hierarchySystem;
    hierarchySystem.Update(*world, 0.0f);

    auto* worldBefore = world->GetComponent<WorldTransform>(entity.GetHandle());
    ASSERT_NE(worldBefore, nullptr);

    Vector3 initialPivot(worldBefore->matrix[12], worldBefore->matrix[13], worldBefore->matrix[14]);

    TransformTool tool(*GetWorld());
    tool.SetTargetEntity(entity.GetHandle());
    tool.SetPivot(initialPivot);

    GizmoCollector collector;
    tool.GatherGizmos(collector);
    ASSERT_FALSE(collector.Empty());
    ASSERT_EQ(collector.GetGizmos().size(), 1u);

    auto* translateGizmo = dynamic_cast<TransformTranslateGizmo*>(collector.GetGizmos()[0]);
    ASSERT_NE(translateGizmo, nullptr);

    const Vector3 gizmoPivotBefore = translateGizmo->GetPivot();
    EXPECT_NEAR(gizmoPivotBefore[0], initialPivot[0], kPosEpsilon);
    EXPECT_NEAR(gizmoPivotBefore[1], initialPivot[1], kPosEpsilon);
    EXPECT_NEAR(gizmoPivotBefore[2], initialPivot[2], kPosEpsilon);

    const Vector3 deltaWorld(0.5f, -1.25f, 2.0f);
    tool.ApplyTranslationDelta(deltaWorld);

    world->ProcessCommands();
    hierarchySystem.Update(*world, 0.0f);

    auto* worldAfter = world->GetComponent<WorldTransform>(entity.GetHandle());
    ASSERT_NE(worldAfter, nullptr);

    Vector3 worldPosAfter(worldAfter->matrix[12], worldAfter->matrix[13], worldAfter->matrix[14]);

    EXPECT_NEAR(worldPosAfter[0], initialPivot[0] + deltaWorld[0], kPosEpsilon);
    EXPECT_NEAR(worldPosAfter[1], initialPivot[1] + deltaWorld[1], kPosEpsilon);
    EXPECT_NEAR(worldPosAfter[2], initialPivot[2] + deltaWorld[2], kPosEpsilon);

    const Vector3 gizmoPivotAfter = translateGizmo->GetPivot();

    EXPECT_NEAR(gizmoPivotAfter[0], worldPosAfter[0], kPosEpsilon);
    EXPECT_NEAR(gizmoPivotAfter[1], worldPosAfter[1], kPosEpsilon);
    EXPECT_NEAR(gizmoPivotAfter[2], worldPosAfter[2], kPosEpsilon);
}

TEST_F(SceneViewTransformToolTests, TranslationKeepsOffsetPivotInSync)
{
    World* world = GetWorld();

    Entity entity = world->Create();
    entity.Set(Transform::FromTRS(
        Vector3(0.0f, 0.0f, 0.0f),
        Quaternion(1.0f, 0.0f, 0.0f, 0.0f),
        Vector3(1.0f, 1.0f, 1.0f)));

    world->ProcessCommands();

    TransformHierarchySystem hierarchySystem;
    hierarchySystem.Update(*world, 0.0f);

    TransformTool tool(*GetWorld());
    tool.SetTargetEntity(entity.GetHandle());
    const Vector3 offsetPivot(10.0f, 0.0f, 5.0f);
    tool.SetPivot(offsetPivot);

    GizmoCollector collector;
    tool.GatherGizmos(collector);
    ASSERT_FALSE(collector.Empty());
    ASSERT_EQ(collector.GetGizmos().size(), 1u);

    auto* translateGizmo = dynamic_cast<TransformTranslateGizmo*>(collector.GetGizmos()[0]);
    ASSERT_NE(translateGizmo, nullptr);

    const Vector3 deltaWorld(2.0f, 0.0f, -1.5f);
    tool.ApplyTranslationDelta(deltaWorld);

    world->ProcessCommands();
    hierarchySystem.Update(*world, 0.0f);

    const auto* localAfter = world->GetComponent<Transform>(entity.GetHandle());
    ASSERT_NE(localAfter, nullptr);
    const Vector3 posAfter = localAfter->GetPosition();
    EXPECT_NEAR(posAfter.x, deltaWorld[0], kPosEpsilon);
    EXPECT_NEAR(posAfter.y, deltaWorld[1], kPosEpsilon);
    EXPECT_NEAR(posAfter.z, deltaWorld[2], kPosEpsilon);

    const Vector3 gizmoPivotAfter = translateGizmo->GetPivot();
    EXPECT_NEAR(gizmoPivotAfter[0], offsetPivot[0] + deltaWorld[0], kPosEpsilon);
    EXPECT_NEAR(gizmoPivotAfter[1], offsetPivot[1] + deltaWorld[1], kPosEpsilon);
    EXPECT_NEAR(gizmoPivotAfter[2], offsetPivot[2] + deltaWorld[2], kPosEpsilon);
}

TEST_F(SceneViewTransformToolTests, InteractiveMultiSelectionAppliesSequentialPointerMoves)
{
    World* world = GetWorld();
    Entity first = world->Create(Transform::FromTRS(
        Vector3(0.0f, 0.0f, 0.0f), Quaternion{}, Vector3(1.0f, 1.0f, 1.0f)));
    Entity second = world->Create(Transform::FromTRS(
        Vector3(10.0f, 0.0f, 0.0f), Quaternion{}, Vector3(1.0f, 1.0f, 1.0f)));
    world->ProcessCommands();

    TransformHierarchySystem hierarchySystem;
    hierarchySystem.Update(*world, 0.0f);

    TransformTool tool(*GetWorld());
    tool.SetTargetEntity(first.GetHandle());
    tool.SetSelectedEntities({first.GetHandle(), second.GetHandle()});
    const Vector3 pivot(0.0f, 0.0f, 0.0f);
    tool.SetPivot(pivot);

    GizmoCollector collector;
    tool.GatherGizmos(collector);
    ASSERT_EQ(collector.GetGizmos().size(), 1u);
    auto* translateGizmo = dynamic_cast<TransformTranslateGizmo*>(collector.GetGizmos()[0]);
    ASSERT_NE(translateGizmo, nullptr);

    GizmoHit planeHit{};
    planeHit.kind = GizmoHitKind::Plane;
    planeHit.handleId = 0u; // XY plane

    auto makeEvent = [](PointerPhase phase, float rayOriginX) {
        ScenePointerEvent event{};
        event.button = PointerButton::Left;
        event.phase = phase;
        event.ray.origin = Vector3(rayOriginX, 0.0f, -10.0f);
        event.ray.direction = Vector3(0.0f, 0.0f, 1.0f);
        return event;
    };

    ASSERT_TRUE(translateGizmo->HandlePointerEvent(
        makeEvent(PointerPhase::Down, 0.0f), planeHit));
    ASSERT_TRUE(tool.IsInteractiveEditActive());
    ASSERT_TRUE(translateGizmo->HandlePointerEvent(
        makeEvent(PointerPhase::Move, 1.0f), planeHit));
    ASSERT_TRUE(translateGizmo->HandlePointerEvent(
        makeEvent(PointerPhase::Move, 3.0f), planeHit));

    // UIManager dispatches one coalesced pointer position per UI frame. The
    // transform tool applies each delivered delta directly to every target.
    EXPECT_NEAR(world->GetComponent<Transform>(first.GetHandle())->GetPosition().x,
                3.0f, kPosEpsilon);
    EXPECT_NEAR(world->GetComponent<Transform>(second.GetHandle())->GetPosition().x,
                13.0f, kPosEpsilon);
    ASSERT_TRUE(translateGizmo->HandlePointerEvent(
        makeEvent(PointerPhase::Up, 3.0f), planeHit));
}

TEST_F(SceneViewTransformToolTests, RefreshPivotUsesLocalBoundsCenter)
{
    World* world = GetWorld();

    Entity entity = world->Create();
    entity.Set(Transform::FromTRS(
        Vector3(1.0f, 2.0f, 3.0f),
        Quaternion(1.0f, 0.0f, 0.0f, 0.0f),
        Vector3(1.0f, 1.0f, 1.0f)));

    LocalBounds bounds{};
    bounds.Box.center = Vector3(8.0f, 0.0f, -2.0f);
    bounds.Box.halfExtents = Vector3(1.0f, 1.0f, 1.0f);
    entity.Set(bounds);

    world->ProcessCommands();

    TransformHierarchySystem hierarchySystem;
    hierarchySystem.Update(*world, 0.0f);

    TransformTool tool(*GetWorld());
    tool.SetTargetEntity(entity.GetHandle());
    tool.RefreshPivotFromTargetEntity();

    GizmoCollector collector;
    tool.GatherGizmos(collector);
    ASSERT_FALSE(collector.Empty());
    ASSERT_EQ(collector.GetGizmos().size(), 1u);

    auto* translateGizmo = dynamic_cast<TransformTranslateGizmo*>(collector.GetGizmos()[0]);
    ASSERT_NE(translateGizmo, nullptr);

    const Vector3 pivot = translateGizmo->GetPivot();
    EXPECT_NEAR(pivot[0], 9.0f, kPosEpsilon);
    EXPECT_NEAR(pivot[1], 2.0f, kPosEpsilon);
    EXPECT_NEAR(pivot[2], 1.0f, kPosEpsilon);
}

TEST_F(SceneViewTransformToolTests, RefreshPivotUsesSplineFirstPointInsteadOfTransformOrigin)
{
    ScopedSplineCenterPivotSetting centerPivot(false);

    World* world = GetWorld();
    auto* splineService = GameEngine::SplineECS::SplineService::TryGet();
    ASSERT_NE(splineService, nullptr);

    const auto handle = splineService->CreateSpline(GameEngine::Spline::SplineType::Linear, false);
    auto* data = splineService->GetSplineData(handle);
    ASSERT_NE(data, nullptr);
    data->AddPoint(Vector3(8.0f, 0.0f, -2.0f), 1.0f);
    data->AddPoint(Vector3(12.0f, 0.0f, -2.0f), 1.0f);
    splineService->RebuildCache(handle);

    Entity entity = world->Create();
    entity.Set(Transform::FromTRS(
        Vector3(1.0f, 2.0f, 3.0f),
        Quaternion(1.0f, 0.0f, 0.0f, 0.0f),
        Vector3(1.0f, 1.0f, 1.0f)));

    SplineComponent spline{};
    spline.SplineDataIndex = handle.Index();
    spline.SplineDataGeneration = handle.Generation();
    entity.Set(spline);

    LocalBounds bounds{};
    bounds.Box.center = Vector3(-20.0f, 5.0f, 4.0f);
    bounds.Box.halfExtents = Vector3(1.0f, 1.0f, 1.0f);
    entity.Set(bounds);

    world->ProcessCommands();

    TransformHierarchySystem hierarchySystem;
    hierarchySystem.Update(*world, 0.0f);

    TransformTool tool(*GetWorld());
    tool.SetTargetEntity(entity.GetHandle());
    tool.RefreshPivotFromTargetEntity();

    GizmoCollector collector;
    tool.GatherGizmos(collector);
    ASSERT_FALSE(collector.Empty());
    ASSERT_EQ(collector.GetGizmos().size(), 1u);

    auto* translateGizmo = dynamic_cast<TransformTranslateGizmo*>(collector.GetGizmos()[0]);
    ASSERT_NE(translateGizmo, nullptr);

    const Vector3 pivot = translateGizmo->GetPivot();
    EXPECT_NEAR(pivot[0], 9.0f, kPosEpsilon);
    EXPECT_NEAR(pivot[1], 2.0f, kPosEpsilon);
    EXPECT_NEAR(pivot[2], 1.0f, kPosEpsilon);

    const Vector3 deltaWorld(2.0f, -1.0f, 0.5f);
    tool.ApplyTranslationDelta(deltaWorld);

    world->ProcessCommands();
    hierarchySystem.Update(*world, 0.0f);

    const Vector3 movedPivot = translateGizmo->GetPivot();
    EXPECT_NEAR(movedPivot[0], 11.0f, kPosEpsilon);
    EXPECT_NEAR(movedPivot[1], 1.0f, kPosEpsilon);
    EXPECT_NEAR(movedPivot[2], 1.5f, kPosEpsilon);

    ASSERT_GE(data->Points.size(), 1u);
    EXPECT_NEAR(data->Points.front().Position.x, 8.0f, kPosEpsilon);
    EXPECT_NEAR(data->Points.front().Position.y, 0.0f, kPosEpsilon);
    EXPECT_NEAR(data->Points.front().Position.z, -2.0f, kPosEpsilon);
}

TEST_F(SceneViewTransformToolTests, RefreshPivotCanUseSplineCenter)
{
    ScopedSplineCenterPivotSetting centerPivot(true);

    World* world = GetWorld();
    auto* splineService = GameEngine::SplineECS::SplineService::TryGet();
    ASSERT_NE(splineService, nullptr);

    const auto handle = splineService->CreateSpline(GameEngine::Spline::SplineType::Linear, false);
    auto* data = splineService->GetSplineData(handle);
    ASSERT_NE(data, nullptr);
    data->AddPoint(Vector3(-2.0f, 0.0f, 4.0f), 1.0f);
    data->AddPoint(Vector3(6.0f, 2.0f, 12.0f), 1.0f);
    data->AddPoint(Vector3(4.0f, -4.0f, 8.0f), 1.0f);
    splineService->RebuildCache(handle);

    Entity entity = world->Create();
    entity.Set(Transform::FromTRS(
        Vector3(10.0f, 20.0f, 30.0f),
        Quaternion(1.0f, 0.0f, 0.0f, 0.0f),
        Vector3(1.0f, 1.0f, 1.0f)));

    SplineComponent spline{};
    spline.SplineDataIndex = handle.Index();
    spline.SplineDataGeneration = handle.Generation();
    entity.Set(spline);

    world->ProcessCommands();

    TransformHierarchySystem hierarchySystem;
    hierarchySystem.Update(*world, 0.0f);

    TransformTool tool(*GetWorld());
    tool.SetTargetEntity(entity.GetHandle());
    tool.RefreshPivotFromTargetEntity();

    GizmoCollector collector;
    tool.GatherGizmos(collector);
    ASSERT_FALSE(collector.Empty());
    ASSERT_EQ(collector.GetGizmos().size(), 1u);

    auto* translateGizmo = dynamic_cast<TransformTranslateGizmo*>(collector.GetGizmos()[0]);
    ASSERT_NE(translateGizmo, nullptr);

    const Vector3 pivot = translateGizmo->GetPivot();
    EXPECT_NEAR(pivot[0], 12.0f, kPosEpsilon);
    EXPECT_NEAR(pivot[1], 19.0f, kPosEpsilon);
    EXPECT_NEAR(pivot[2], 38.0f, kPosEpsilon);
}

TEST_F(SceneViewTransformToolTests, ChildUnderRotatedScaledParentPreservesWorldDelta)
{
    World* world = GetWorld();

    Entity parent = world->Create();
    Entity child = world->Create();

    Vector3 parentPos(0.0f, 1.0f, 0.0f);
    // 45 degrees around Y with non-uniform scale to exercise full TRS math.
    const float s = std::sqrt(0.5f);
    Quaternion parentRot(s, 0.0f, s, 0.0f);
    Vector3 parentScale(2.0f, 1.5f, 0.5f);

    Transform parentLocal = Transform::FromTRS(parentPos, parentRot, parentScale);

    Vector3 childLocalPos(1.0f, 0.0f, 0.0f);
    Quaternion childRot(1.0f, 0.0f, 0.0f, 0.0f);
    Vector3 childScale(1.0f, 1.0f, 1.0f);
    Transform childLocal = Transform::FromTRS(childLocalPos, childRot, childScale);

    Parent parentComp{};
    parentComp.parent = parent.GetHandle();

    parent.Set(parentLocal);
    child.Set(childLocal);
    child.Set(parentComp);

    world->ProcessCommands();

    TransformHierarchySystem hierarchySystem;
    hierarchySystem.Update(*world, 0.0f);

    auto* parentWorldBefore = world->GetComponent<WorldTransform>(parent.GetHandle());
    auto* childWorldBefore = world->GetComponent<WorldTransform>(child.GetHandle());
    ASSERT_NE(parentWorldBefore, nullptr);
    ASSERT_NE(childWorldBefore, nullptr);

    Vector3 childWorldPosBefore(childWorldBefore->matrix[12],
                                childWorldBefore->matrix[13],
                                childWorldBefore->matrix[14]);

    TransformTool tool(*GetWorld());
    tool.SetTargetEntity(child.GetHandle());
    tool.SetPivot(childWorldPosBefore);

    GizmoCollector collector;
    tool.GatherGizmos(collector);
    ASSERT_FALSE(collector.Empty());
    ASSERT_EQ(collector.GetGizmos().size(), 1u);

    auto* translateGizmo = dynamic_cast<TransformTranslateGizmo*>(collector.GetGizmos()[0]);
    ASSERT_NE(translateGizmo, nullptr);

    const Vector3 deltaWorld(1.25f, -0.5f, 2.0f);
    tool.ApplyTranslationDelta(deltaWorld);

    world->ProcessCommands();
    hierarchySystem.Update(*world, 0.0f);

    auto* parentWorldAfter = world->GetComponent<WorldTransform>(parent.GetHandle());
    auto* childWorldAfter = world->GetComponent<WorldTransform>(child.GetHandle());
    ASSERT_NE(parentWorldAfter, nullptr);
    ASSERT_NE(childWorldAfter, nullptr);

    // Parent world transform must remain unchanged by child translation.
    for (int i = 0; i < 16; ++i)
    {
        EXPECT_NEAR(parentWorldAfter->matrix[i], parentWorldBefore->matrix[i], 1.0e-5f);
    }

    Vector3 childWorldPosAfter(childWorldAfter->matrix[12],
                               childWorldAfter->matrix[13],
                               childWorldAfter->matrix[14]);

    EXPECT_NEAR(childWorldPosAfter[0], childWorldPosBefore[0] + deltaWorld[0], kPosEpsilon);
    EXPECT_NEAR(childWorldPosAfter[1], childWorldPosBefore[1] + deltaWorld[1], kPosEpsilon);
    EXPECT_NEAR(childWorldPosAfter[2], childWorldPosBefore[2] + deltaWorld[2], kPosEpsilon);

    const Vector3 gizmoPivotAfter = translateGizmo->GetPivot();
    EXPECT_NEAR(gizmoPivotAfter[0], childWorldPosAfter[0], kPosEpsilon);
    EXPECT_NEAR(gizmoPivotAfter[1], childWorldPosAfter[1], kPosEpsilon);
    EXPECT_NEAR(gizmoPivotAfter[2], childWorldPosAfter[2], kPosEpsilon);
}

TEST_F(SceneViewTransformToolTests, ChildUnderIdentityParentPreservesWorldDelta)
{
    World* world = GetWorld();

    Entity parent = world->Create();
    Entity child = world->Create();

    // Parent at an arbitrary world position but with identity rotation and scale.
    Vector3 parentPos(10.0f, -3.0f, 5.0f);
    Quaternion parentRot(1.0f, 0.0f, 0.0f, 0.0f);
    Vector3 parentScale(1.0f, 1.0f, 1.0f);

    Transform parentLocal = Transform::FromTRS(parentPos, parentRot, parentScale);

    Vector3 childLocalPos(1.0f, 2.0f, -4.0f);
    Quaternion childRot(1.0f, 0.0f, 0.0f, 0.0f);
    Vector3 childScale(1.0f, 1.0f, 1.0f);
    Transform childLocal = Transform::FromTRS(childLocalPos, childRot, childScale);

    Parent parentComp{};
    parentComp.parent = parent.GetHandle();

    parent.Set(parentLocal);
    child.Set(childLocal);
    child.Set(parentComp);

    world->ProcessCommands();

    TransformHierarchySystem hierarchySystem;
    hierarchySystem.Update(*world, 0.0f);

    auto* parentWorldBefore = world->GetComponent<WorldTransform>(parent.GetHandle());
    auto* childWorldBefore = world->GetComponent<WorldTransform>(child.GetHandle());
    ASSERT_NE(parentWorldBefore, nullptr);
    ASSERT_NE(childWorldBefore, nullptr);

    Vector3 childWorldPosBefore(childWorldBefore->matrix[12],
                                childWorldBefore->matrix[13],
                                childWorldBefore->matrix[14]);

    TransformTool tool(*GetWorld());
    tool.SetTargetEntity(child.GetHandle());
    tool.SetPivot(childWorldPosBefore);

    GizmoCollector collector;
    tool.GatherGizmos(collector);
    ASSERT_FALSE(collector.Empty());
    ASSERT_EQ(collector.GetGizmos().size(), 1u);

    auto* translateGizmo = dynamic_cast<TransformTranslateGizmo*>(collector.GetGizmos()[0]);
    ASSERT_NE(translateGizmo, nullptr);

    const Vector3 deltaWorld(-0.75f, 1.5f, 3.25f);
    tool.ApplyTranslationDelta(deltaWorld);

    world->ProcessCommands();
    hierarchySystem.Update(*world, 0.0f);

    auto* parentWorldAfter = world->GetComponent<WorldTransform>(parent.GetHandle());
    auto* childWorldAfter = world->GetComponent<WorldTransform>(child.GetHandle());
    ASSERT_NE(parentWorldAfter, nullptr);
    ASSERT_NE(childWorldAfter, nullptr);

    // Parent world transform must remain unchanged by child translation.
    for (int i = 0; i < 16; ++i)
    {
        EXPECT_NEAR(parentWorldAfter->matrix[i], parentWorldBefore->matrix[i], 1.0e-5f);
    }

    Vector3 childWorldPosAfter(childWorldAfter->matrix[12],
                               childWorldAfter->matrix[13],
                               childWorldAfter->matrix[14]);

    EXPECT_NEAR(childWorldPosAfter[0], childWorldPosBefore[0] + deltaWorld[0], kPosEpsilon);
    EXPECT_NEAR(childWorldPosAfter[1], childWorldPosBefore[1] + deltaWorld[1], kPosEpsilon);
    EXPECT_NEAR(childWorldPosAfter[2], childWorldPosBefore[2] + deltaWorld[2], kPosEpsilon);

    const Vector3 gizmoPivotAfter = translateGizmo->GetPivot();
    EXPECT_NEAR(gizmoPivotAfter[0], childWorldPosAfter[0], kPosEpsilon);
    EXPECT_NEAR(gizmoPivotAfter[1], childWorldPosAfter[1], kPosEpsilon);
    EXPECT_NEAR(gizmoPivotAfter[2], childWorldPosAfter[2], kPosEpsilon);
}

TEST_F(SceneViewTransformToolTests, FrameLoop_OrderMatchesEngine_KeepsGizmoInSync)
{
    World* world = GetWorld();

    Entity entity = world->Create();

    Vector3 startPos(1.0f, 2.0f, -3.0f);
    Quaternion startRot(1.0f, 0.0f, 0.0f, 0.0f);
    Vector3 startScale(1.0f, 1.0f, 1.0f);

    Transform local = Transform::FromTRS(startPos, startRot, startScale);
    entity.Set(local);

    world->ProcessCommands();

    TransformHierarchySystem hierarchySystem;
    hierarchySystem.Update(*world, 0.0f);

    auto* worldBefore = world->GetComponent<WorldTransform>(entity.GetHandle());
    ASSERT_NE(worldBefore, nullptr);

    Vector3 initialWorldPos(worldBefore->matrix[12], worldBefore->matrix[13], worldBefore->matrix[14]);

    TransformTool tool(*GetWorld());
    tool.SetTargetEntity(entity.GetHandle());
    tool.SetPivot(initialWorldPos);

    GizmoCollector collector;
    tool.GatherGizmos(collector);
    ASSERT_FALSE(collector.Empty());
    ASSERT_EQ(collector.GetGizmos().size(), 1u);

    auto* translateGizmo = dynamic_cast<TransformTranslateGizmo*>(collector.GetGizmos()[0]);
    ASSERT_NE(translateGizmo, nullptr);

    ViewId viewId = 1;
    CameraId cameraId = 1;
    GizmoRenderContext context(viewId, cameraId);

    // Frame 0: baseline render after hierarchy update; gizmo should snap to WorldTransform.
    ResetGizmoLineGroups(viewId);
    ResetGizmoTriangleGroups(viewId);
    translateGizmo->Render(context);

    const Vector3 gizmoPivotFrame0 = translateGizmo->GetPivot();
    EXPECT_NEAR(gizmoPivotFrame0[0], initialWorldPos[0], kPosEpsilon);
    EXPECT_NEAR(gizmoPivotFrame0[1], initialWorldPos[1], kPosEpsilon);
    EXPECT_NEAR(gizmoPivotFrame0[2], initialWorldPos[2], kPosEpsilon);

    // Frame 1: simulate the real engine ordering:
    //   ApplyTranslationDelta (Editor update)
    //   world->ProcessCommands()
    //   TransformHierarchySystem::Update()
    //   gizmo Render() during Scene View overlay.
    const Vector3 deltaWorld(0.5f, -1.25f, 2.0f);
    tool.ApplyTranslationDelta(deltaWorld);

    world->ProcessCommands();
    hierarchySystem.Update(*world, 0.0f);

    auto* worldAfter = world->GetComponent<WorldTransform>(entity.GetHandle());
    ASSERT_NE(worldAfter, nullptr);

    Vector3 worldPosAfter(worldAfter->matrix[12], worldAfter->matrix[13], worldAfter->matrix[14]);

    // Let the gizmo resample its pivot from ECS state for this frame.
    ResetGizmoLineGroups(viewId);
    ResetGizmoTriangleGroups(viewId);
    translateGizmo->Render(context);

    const Vector3 gizmoPivotAfter = translateGizmo->GetPivot();

    // Sanity: world transform applied the expected delta.
    EXPECT_NEAR(worldPosAfter[0], initialWorldPos[0] + deltaWorld[0], kPosEpsilon);
    EXPECT_NEAR(worldPosAfter[1], initialWorldPos[1] + deltaWorld[1], kPosEpsilon);
    EXPECT_NEAR(worldPosAfter[2], initialWorldPos[2] + deltaWorld[2], kPosEpsilon);

    // Primary invariant: gizmo pivot for this frame matches the WorldTransform
    // used by rendering, i.e., no one-frame latency.
    EXPECT_NEAR(gizmoPivotAfter[0], worldPosAfter[0], kPosEpsilon);
    EXPECT_NEAR(gizmoPivotAfter[1], worldPosAfter[1], kPosEpsilon);
    EXPECT_NEAR(gizmoPivotAfter[2], worldPosAfter[2], kPosEpsilon);
}

TEST_F(SceneViewTransformToolTests, FrameLoop_BadOrderingProducesOneFrameDesync)
{
    World* world = GetWorld();

    Entity entity = world->Create();

    Vector3 startPos(1.0f, 2.0f, -3.0f);
    Quaternion startRot(1.0f, 0.0f, 0.0f, 0.0f);
    Vector3 startScale(1.0f, 1.0f, 1.0f);

    Transform local = Transform::FromTRS(startPos, startRot, startScale);
    entity.Set(local);

    world->ProcessCommands();

    TransformHierarchySystem hierarchySystem;
    hierarchySystem.Update(*world, 0.0f);

    auto* worldBefore = world->GetComponent<WorldTransform>(entity.GetHandle());
    ASSERT_NE(worldBefore, nullptr);

    Vector3 worldPosFrame0(worldBefore->matrix[12], worldBefore->matrix[13], worldBefore->matrix[14]);

    TransformTool tool(*GetWorld());
    tool.SetTargetEntity(entity.GetHandle());
    tool.SetPivot(worldPosFrame0);

    GizmoCollector collector;
    tool.GatherGizmos(collector);
    ASSERT_FALSE(collector.Empty());
    ASSERT_EQ(collector.GetGizmos().size(), 1u);

    auto* translateGizmo = dynamic_cast<TransformTranslateGizmo*>(collector.GetGizmos()[0]);
    ASSERT_NE(translateGizmo, nullptr);

    ViewId viewId = 2;
    CameraId cameraId = 2;
    GizmoRenderContext context(viewId, cameraId);

    // Frame 0: baseline render; gizmo should be aligned with WorldTransform.
    ResetGizmoLineGroups(viewId);
    ResetGizmoTriangleGroups(viewId);
    translateGizmo->Render(context);

    const Vector3 gizmoPivotFrame0 = translateGizmo->GetPivot();
    EXPECT_NEAR(gizmoPivotFrame0[0], worldPosFrame0[0], kPosEpsilon);
    EXPECT_NEAR(gizmoPivotFrame0[1], worldPosFrame0[1], kPosEpsilon);
    EXPECT_NEAR(gizmoPivotFrame0[2], worldPosFrame0[2], kPosEpsilon);

    // Frame 1: deliberately use a "bad" ordering where gizmo Render() runs
    // before the TransformHierarchySystem update for this frame. With the
    // current design, the TransformTool updates the gizmo pivot immediately
    // when ApplyTranslationDelta() is called, while the world transform
    // (and thus the mesh) would still be using the previous frame's
    // WorldTransform until the hierarchy system runs.
    const Vector3 deltaWorld(1.0f, -2.0f, 3.0f);
    tool.ApplyTranslationDelta(deltaWorld);

    // BAD ORDERING: render gizmo against stale WorldTransform before running
    // the hierarchy system for this frame.
    ResetGizmoLineGroups(viewId);
    ResetGizmoTriangleGroups(viewId);
    translateGizmo->Render(context);

    const Vector3 gizmoPivotFrame1 = translateGizmo->GetPivot();

    // Now run the hierarchy update that would normally precede rendering in
    // the real engine frame loop.
    world->ProcessCommands();
    hierarchySystem.Update(*world, 0.0f);

    auto* worldAfter = world->GetComponent<WorldTransform>(entity.GetHandle());
    ASSERT_NE(worldAfter, nullptr);

    Vector3 worldPosFrame1(worldAfter->matrix[12], worldAfter->matrix[13], worldAfter->matrix[14]);

    // Sanity: the world transform has moved by deltaWorld relative to frame 0.
    EXPECT_NEAR(worldPosFrame1[0], worldPosFrame0[0] + deltaWorld[0], kPosEpsilon);
    EXPECT_NEAR(worldPosFrame1[1], worldPosFrame0[1] + deltaWorld[1], kPosEpsilon);
    EXPECT_NEAR(worldPosFrame1[2], worldPosFrame0[2] + deltaWorld[2], kPosEpsilon);

    // Because we updated the tool (and thus the gizmo pivot) before updating
    // the hierarchy, a bad frame ordering where the gizmo overlay renders
    // before the world pass would show the gizmo at the *new* position while
    // the mesh is still drawn at the previous frame's WorldTransform. That
    // is still a one-frame gizmo/mesh desync, just in the opposite direction
    // from the original implementation that resampled the pivot from ECS in
    // Render().

    // Sanity: gizmo pivot reflects the new world position after the move.
    EXPECT_NEAR(gizmoPivotFrame1[0], worldPosFrame1[0], kPosEpsilon);
    EXPECT_NEAR(gizmoPivotFrame1[1], worldPosFrame1[1], kPosEpsilon);
    EXPECT_NEAR(gizmoPivotFrame1[2], worldPosFrame1[2], kPosEpsilon);

    // And under this bad ordering, the position that the mesh would still be
    // using for frame 1 (worldPosFrame0) no longer matches the gizmo pivot;
    // this encodes the kind of one-frame desync we want to avoid.
    EXPECT_GT(std::fabs(gizmoPivotFrame1[0] - worldPosFrame0[0]), kPosEpsilon);
    EXPECT_GT(std::fabs(gizmoPivotFrame1[1] - worldPosFrame0[1]), kPosEpsilon);
    EXPECT_GT(std::fabs(gizmoPivotFrame1[2] - worldPosFrame0[2]), kPosEpsilon);
}

TEST_F(SceneViewTransformToolTests, RootEntityRotationAroundWorldYAxisMatchesExpectedQuaternion)
{
    World* world = GetWorld();

    Entity entity = world->Create();

    Vector3 startPos(0.0f, 0.0f, 0.0f);
    Quaternion startRot(1.0f, 0.0f, 0.0f, 0.0f);
    Vector3 startScale(1.0f, 1.0f, 1.0f);

    Transform local = Transform::FromTRS(startPos, startRot, startScale);
    entity.Set(local);

    world->ProcessCommands();

    TransformHierarchySystem hierarchySystem;
    hierarchySystem.Update(*world, 0.0f);

    auto* worldBefore = world->GetComponent<WorldTransform>(entity.GetHandle());
    ASSERT_NE(worldBefore, nullptr);

    Vector3 pivot(worldBefore->matrix[12], worldBefore->matrix[13], worldBefore->matrix[14]);

    TransformTool tool(*GetWorld());
    tool.SetTargetEntity(entity.GetHandle());
    tool.SetPivot(pivot);
    tool.SetMode(GameEngine::Editor::SceneTools::TransformMode::Rotate);

    // Rotate 90 degrees (pi/2 radians) around world Y axis (axisIndex = 1).
    const float kAngle = 0.5f * 3.14159265358979323846f;
    tool.ApplyRotationDelta(1u, kAngle);

    world->ProcessCommands();
    hierarchySystem.Update(*world, 0.0f);

    auto* worldAfter = world->GetComponent<WorldTransform>(entity.GetHandle());
    ASSERT_NE(worldAfter, nullptr);

    // Extract rotation from WorldTransform by reconstructing a Transform.
    Transform worldTrsAfter;
    std::memcpy(worldTrsAfter.matrix, worldAfter->matrix, sizeof(worldAfter->matrix));
    Quaternion worldRotAfter = worldTrsAfter.GetRotation();

    const float s = std::sqrt(0.5f);
    Quaternion expectedRot(s, 0.0f, s, 0.0f);

    {
        const auto& a = worldRotAfter.GetGLM();
        const auto& b = expectedRot.GetGLM();
        EXPECT_NEAR(a.x, b.x, kRotEpsilon);
        EXPECT_NEAR(a.y, b.y, kRotEpsilon);
        EXPECT_NEAR(a.z, b.z, kRotEpsilon);
        EXPECT_NEAR(a.w, b.w, kRotEpsilon);
    }
}

TEST_F(SceneViewTransformToolTests, RootEntityScaleDeltaUpdatesLocalAndWorldScale)
{
    World* world = GetWorld();

    Entity entity = world->Create();

    Vector3 startPos(1.0f, -2.0f, 3.0f);
    Quaternion startRot(1.0f, 0.0f, 0.0f, 0.0f);
    Vector3 startScale(1.0f, 2.0f, 3.0f);

    Transform local = Transform::FromTRS(startPos, startRot, startScale);
    entity.Set(local);

    world->ProcessCommands();

    TransformHierarchySystem hierarchySystem;
    hierarchySystem.Update(*world, 0.0f);

    auto* worldBefore = world->GetComponent<WorldTransform>(entity.GetHandle());
    ASSERT_NE(worldBefore, nullptr);

    Vector3 pivot(worldBefore->matrix[12], worldBefore->matrix[13], worldBefore->matrix[14]);

    TransformTool tool(*GetWorld());
    tool.SetTargetEntity(entity.GetHandle());
    tool.SetPivot(pivot);
    tool.SetMode(GameEngine::Editor::SceneTools::TransformMode::Scale);

    const Vector3 deltaScale(2.0f, 0.5f, 1.5f);
    tool.ApplyScaleDelta(deltaScale);

    world->ProcessCommands();
    hierarchySystem.Update(*world, 0.0f);

    auto* localAfter = world->GetComponent<Transform>(entity.GetHandle());
    ASSERT_NE(localAfter, nullptr);
    Vector3 newLocalScale = localAfter->GetScale();

    EXPECT_NEAR(newLocalScale.x, startScale.x * deltaScale[0], kPosEpsilon);
    EXPECT_NEAR(newLocalScale.y, startScale.y * deltaScale[1], kPosEpsilon);
    EXPECT_NEAR(newLocalScale.z, startScale.z * deltaScale[2], kPosEpsilon);

    auto* worldAfter = world->GetComponent<WorldTransform>(entity.GetHandle());
    ASSERT_NE(worldAfter, nullptr);

    Transform worldTrsAfter;
    std::memcpy(worldTrsAfter.matrix, worldAfter->matrix, sizeof(worldAfter->matrix));
    Vector3 worldScaleAfter = worldTrsAfter.GetScale();

    EXPECT_NEAR(worldScaleAfter.x, newLocalScale.x, kPosEpsilon);
    EXPECT_NEAR(worldScaleAfter.y, newLocalScale.y, kPosEpsilon);
    EXPECT_NEAR(worldScaleAfter.z, newLocalScale.z, kPosEpsilon);
}

TEST_F(SceneViewTransformToolTests, WorldAndLocalAxisSpacesAreSwappedForPreRotatedRootEntity)
{
    World* world = GetWorld();

    Entity entity = world->Create();

    // Start with a non-trivial rotation: 45 degrees around X axis.
    Vector3 startPos(0.0f, 0.0f, 0.0f);
    const float s = std::sqrt(0.5f);
    Quaternion startRot(s, s, 0.0f, 0.0f);
    Vector3 startScale(1.0f, 1.0f, 1.0f);

    Transform local = Transform::FromTRS(startPos, startRot, startScale);
    entity.Set(local);

    world->ProcessCommands();

    TransformHierarchySystem hierarchySystem;
    hierarchySystem.Update(*world, 0.0f);

    auto* worldBefore = world->GetComponent<WorldTransform>(entity.GetHandle());
    ASSERT_NE(worldBefore, nullptr);

    // Extract starting world rotation.
    Transform worldTrsBefore;
    std::memcpy(worldTrsBefore.matrix, worldBefore->matrix, sizeof(worldBefore->matrix));
    Quaternion worldRotBefore = worldTrsBefore.GetRotation();

    // Compute expected world rotations for pure world-axis and local-axis semantics
    // using the Mathematics module, independent of TransformTool.
    const auto& q0g = worldRotBefore.GetGLM();
    Quaternion q0(q0g.w, q0g.x, q0g.y, q0g.z);

    const float kAngle = 0.5f * 3.14159265358979323846f; // 90 degrees

    // World-axis semantics: rotate around canonical world Y.
    Quaternion qWorldSemantic = Quaternion::FromAxisAngle(Vector3(0.0f, 1.0f, 0.0f), kAngle) * q0;

    // Local-axis semantics: rotate around the entity's local Y axis in world space.
    Vector3 localYAxisWorld = q0.Rotate(Vector3(0.0f, 1.0f, 0.0f));
    Quaternion qLocalSemantic = Quaternion::FromAxisAngle(localYAxisWorld, kAngle) * q0;

    // 1) World axis space should now behave like the previous "local" semantics.
    {
        Vector3 pivot(worldBefore->matrix[12], worldBefore->matrix[13], worldBefore->matrix[14]);

        TransformTool tool(*GetWorld());
        tool.SetTargetEntity(entity.GetHandle());
        tool.SetPivot(pivot);
        tool.SetMode(TransformMode::Rotate);
        tool.SetAxisSpace(GameEngine::Editor::SceneTools::TransformAxisSpace::World);

        tool.ApplyRotationDelta(1u, kAngle);

        world->ProcessCommands();
        hierarchySystem.Update(*world, 0.0f);

        auto* worldAfter = world->GetComponent<WorldTransform>(entity.GetHandle());
        ASSERT_NE(worldAfter, nullptr);

        Transform worldTrsAfter;
        std::memcpy(worldTrsAfter.matrix, worldAfter->matrix, sizeof(worldAfter->matrix));
        Quaternion worldRotAfter = worldTrsAfter.GetRotation();

        const glm::quat gLocal = qLocalSemantic.GetGLM();
        Quaternion expectedLocalSemRot(gLocal.w, gLocal.x, gLocal.y, gLocal.z);

        // Quaternions represent the same rotation up to a global sign flip.
        // If the dot product is negative, flip the expected quaternion so we
        // compare against the equivalent representation.
        const auto& wra = worldRotAfter.GetGLM();
        auto els = expectedLocalSemRot.GetGLM();
        float dotLocal = wra.x * els.x + wra.y * els.y + wra.z * els.z + wra.w * els.w;
        if (dotLocal < 0.0f)
        {
            els.x = -els.x;
            els.y = -els.y;
            els.z = -els.z;
            els.w = -els.w;
        }

        EXPECT_NEAR(wra.x, els.x, kRotEpsilon);
        EXPECT_NEAR(wra.y, els.y, kRotEpsilon);
        EXPECT_NEAR(wra.z, els.z, kRotEpsilon);
        EXPECT_NEAR(wra.w, els.w, kRotEpsilon);
    }

    // Reset entity to starting local transform.
    entity.Set(local);
    world->ProcessCommands();
    hierarchySystem.Update(*world, 0.0f);

    auto* worldBefore2 = world->GetComponent<WorldTransform>(entity.GetHandle());
    ASSERT_NE(worldBefore2, nullptr);

    // 2) Local axis space should now behave like the previous "world" semantics.
    {
        Vector3 pivot(worldBefore2->matrix[12], worldBefore2->matrix[13], worldBefore2->matrix[14]);

        TransformTool tool(*GetWorld());
        tool.SetTargetEntity(entity.GetHandle());
        tool.SetPivot(pivot);
        tool.SetMode(TransformMode::Rotate);
        tool.SetAxisSpace(GameEngine::Editor::SceneTools::TransformAxisSpace::Local);

        tool.ApplyRotationDelta(1u, kAngle);

        world->ProcessCommands();
        hierarchySystem.Update(*world, 0.0f);

        auto* worldAfter = world->GetComponent<WorldTransform>(entity.GetHandle());
        ASSERT_NE(worldAfter, nullptr);

        Transform worldTrsAfter;
        std::memcpy(worldTrsAfter.matrix, worldAfter->matrix, sizeof(worldAfter->matrix));
        Quaternion worldRotAfter = worldTrsAfter.GetRotation();

        const glm::quat gWorld = qWorldSemantic.GetGLM();
        Quaternion expectedWorldSemRot(gWorld.w, gWorld.x, gWorld.y, gWorld.z);

        const auto& wra2 = worldRotAfter.GetGLM();
        auto ews = expectedWorldSemRot.GetGLM();
        float dotWorld = wra2.x * ews.x + wra2.y * ews.y + wra2.z * ews.z + wra2.w * ews.w;
        if (dotWorld < 0.0f)
        {
            ews.x = -ews.x;
            ews.y = -ews.y;
            ews.z = -ews.z;
            ews.w = -ews.w;
        }

        EXPECT_NEAR(wra2.x, ews.x, kRotEpsilon);
        EXPECT_NEAR(wra2.y, ews.y, kRotEpsilon);
        EXPECT_NEAR(wra2.z, ews.z, kRotEpsilon);
        EXPECT_NEAR(wra2.w, ews.w, kRotEpsilon);
    }
}

TEST_F(SceneViewTransformToolTests, KeyEventsDoNotSwitchModeOnRAndU)
{
    TransformTool tool(*GetWorld());

    EXPECT_EQ(tool.GetMode(), TransformMode::Translate);

    GameEngine::Editor::SceneTools::SceneKeyEvent event{};
    event.pressed = true;
    event.alt = false;
    event.ctrl = false;
    event.shift = false;

    // Tool-mode keys are handled by SceneViewPanel catalog bindings, not
    // TransformTool. OnKeyEvent only toggles World/Local via the catalog,
    // which the EditorTests stub never matches.
    event.keyCode = static_cast<std::uint32_t>(GLFW_KEY_R);
    tool.OnKeyEvent(event);
    EXPECT_EQ(tool.GetMode(), TransformMode::Translate);

    event.keyCode = static_cast<std::uint32_t>(GLFW_KEY_U);
    tool.OnKeyEvent(event);
    EXPECT_EQ(tool.GetMode(), TransformMode::Translate);
}

TEST_F(SceneViewTransformToolTests, ClearingMissingExternalPointTargetDoesNotCancelEntityDrag)
{
    World* world = GetWorld();
    ASSERT_NE(world, nullptr);

    Entity entity = world->Create();

    Vector3 startPos(0.0f, 0.0f, 0.0f);
    Quaternion startRot(1.0f, 0.0f, 0.0f, 0.0f);
    Vector3 startScale(1.0f, 1.0f, 1.0f);
    entity.Set(Transform::FromTRS(startPos, startRot, startScale));
    world->ProcessCommands();

    TransformTool tool(*GetWorld());
    tool.SetTargetEntity(entity.GetHandle());
    Vector3 pivot(startPos.x, startPos.y, startPos.z);
    tool.SetPivot(pivot);

    GizmoCollector collector;
    tool.GatherGizmos(collector);
    ASSERT_FALSE(collector.Empty());
    ASSERT_EQ(collector.GetGizmos().size(), 1u);

    auto* translateGizmo = dynamic_cast<TransformTranslateGizmo*>(collector.GetGizmos()[0]);
    ASSERT_NE(translateGizmo, nullptr);

    using GameEngine::Editor::SceneTools::GizmoHit;
    using GameEngine::Editor::SceneTools::GizmoHitKind;
    using GameEngine::Editor::SceneTools::PointerButton;
    using GameEngine::Editor::SceneTools::PointerPhase;
    using GameEngine::Editor::SceneTools::ScenePointerEvent;

    GizmoHit hit{};
    hit.kind = GizmoHitKind::Axis;
    hit.handleId = 0u;
    hit.distance = 1.0f;

    ScenePointerEvent down{};
    down.button = PointerButton::Left;
    down.phase = PointerPhase::Down;
    down.ray.origin = Vector3(startPos.x, startPos.y, startPos.z + 5.0f);
    down.ray.direction = Vector3(0.0f, 0.0f, -1.0f);

    ASSERT_TRUE(translateGizmo->HandlePointerEvent(down, hit));
    EXPECT_TRUE(tool.IsInteractiveEditActive());

    // The spline tool calls this while the spline entity itself is selected
    // and no knot/handle is selected. It must not reset the active root drag.
    tool.ClearExternalPointTarget();
    EXPECT_TRUE(tool.IsInteractiveEditActive());

    ScenePointerEvent move1 = down;
    move1.phase = PointerPhase::Move;
    move1.ray.origin.x += 1.0f;
    ASSERT_TRUE(translateGizmo->HandlePointerEvent(move1, hit));

    tool.ClearExternalPointTarget();
    EXPECT_TRUE(tool.IsInteractiveEditActive());

    ScenePointerEvent move2 = down;
    move2.phase = PointerPhase::Move;
    move2.ray.origin.x += 2.0f;
    ASSERT_TRUE(translateGizmo->HandlePointerEvent(move2, hit));

    ScenePointerEvent up = down;
    up.phase = PointerPhase::Up;
    ASSERT_TRUE(translateGizmo->HandlePointerEvent(up, hit));
    EXPECT_FALSE(tool.IsInteractiveEditActive());

    auto* tAfter = world->GetComponent<Transform>(entity.GetHandle());
    ASSERT_NE(tAfter, nullptr);
    const Vector3 posAfter = tAfter->GetPosition();
    EXPECT_NEAR(posAfter.x, 2.0f, kPosEpsilon);
    EXPECT_NEAR(posAfter.y, 0.0f, kPosEpsilon);
    EXPECT_NEAR(posAfter.z, 0.0f, kPosEpsilon);
}

TEST_F(SceneViewTransformToolTests, GizmoDragTranslateCoalescesToSingleUndoAndUndoRedoRestoresTransform)
{
    World* world = GetWorld();
    ASSERT_NE(world, nullptr);

    Entity entity = world->Create();

    Vector3 startPos(0.0f, 0.0f, 0.0f);
    Quaternion startRot(1.0f, 0.0f, 0.0f, 0.0f);
    Vector3   startScale(1.0f, 1.0f, 1.0f);
    Transform local = Transform::FromTRS(startPos, startRot, startScale);
    entity.Set(local);
    world->ProcessCommands();

    GameEngine::Editor::UndoRedoService undo;
    GameEngine::Editor::EditorChangeNotifications changes;

    TransformTool tool(*GetWorld());
    tool.SetUndoRedoService(&undo);
    tool.SetChangeNotifications(&changes);
    tool.SetTargetEntity(entity.GetHandle());

    Vector3 pivot(startPos.x, startPos.y, startPos.z);
    tool.SetPivot(pivot);

    GizmoCollector collector;
    tool.GatherGizmos(collector);
    ASSERT_FALSE(collector.Empty());
    ASSERT_EQ(collector.GetGizmos().size(), 1u);

    auto* translateGizmo = dynamic_cast<TransformTranslateGizmo*>(collector.GetGizmos()[0]);
    ASSERT_NE(translateGizmo, nullptr);

    // Simulate a simple X-axis drag via pointer events:
    // - ray origin shifts along +X while pointing toward -Z
    // - this produces deterministic line projections for the gizmo math
    using GameEngine::Editor::SceneTools::GizmoHit;
    using GameEngine::Editor::SceneTools::GizmoHitKind;
    using GameEngine::Editor::SceneTools::PointerButton;
    using GameEngine::Editor::SceneTools::PointerPhase;
    using GameEngine::Editor::SceneTools::ScenePointerEvent;
    using GameEngine::Mathematics::Vector3;

    GizmoHit hit{};
    hit.kind = GizmoHitKind::Axis;
    hit.handleId = 0u; // X axis
    hit.distance = 1.0f;

    ScenePointerEvent down{};
    down.button = PointerButton::Left;
    down.phase = PointerPhase::Down;
    down.ray.origin = Vector3(startPos.x, startPos.y, startPos.z + 5.0f);
    down.ray.direction = Vector3(0.0f, 0.0f, -1.0f);

    ASSERT_TRUE(translateGizmo->HandlePointerEvent(down, hit));
    EXPECT_EQ(undo.GetUndoCount(), 0u);
    EXPECT_EQ(undo.GetRedoCount(), 0u);

    ScenePointerEvent move1 = down;
    move1.phase = PointerPhase::Move;
    move1.ray.origin.x += 1.0f;
    ASSERT_TRUE(translateGizmo->HandlePointerEvent(move1, hit));
    EXPECT_EQ(undo.GetUndoCount(), 0u); // still previewing

    ScenePointerEvent move2 = down;
    move2.phase = PointerPhase::Move;
    move2.ray.origin.x += 2.0f;
    ASSERT_TRUE(translateGizmo->HandlePointerEvent(move2, hit));
    EXPECT_EQ(undo.GetUndoCount(), 0u); // still previewing

    ScenePointerEvent up = down;
    up.phase = PointerPhase::Up;
    ASSERT_TRUE(translateGizmo->HandlePointerEvent(up, hit));

    // One command for the whole drag.
    EXPECT_EQ(undo.GetUndoCount(), 1u);
    EXPECT_EQ(undo.GetRedoCount(), 0u);

    auto* tAfter = world->GetComponent<Transform>(entity.GetHandle());
    ASSERT_NE(tAfter, nullptr);
    Vector3 posAfter = tAfter->GetPosition();
    EXPECT_NEAR(posAfter.x, 2.0f, kPosEpsilon);
    EXPECT_NEAR(posAfter.y, 0.0f, kPosEpsilon);
    EXPECT_NEAR(posAfter.z, 0.0f, kPosEpsilon);

    undo.Undo();
    auto* tUndo = world->GetComponent<Transform>(entity.GetHandle());
    ASSERT_NE(tUndo, nullptr);
    Vector3 posUndo = tUndo->GetPosition();
    EXPECT_NEAR(posUndo.x, startPos.x, kPosEpsilon);
    EXPECT_NEAR(posUndo.y, startPos.y, kPosEpsilon);
    EXPECT_NEAR(posUndo.z, startPos.z, kPosEpsilon);

    undo.Redo();
    auto* tRedo = world->GetComponent<Transform>(entity.GetHandle());
    ASSERT_NE(tRedo, nullptr);
    Vector3 posRedo = tRedo->GetPosition();
    EXPECT_NEAR(posRedo.x, 2.0f, kPosEpsilon);
    EXPECT_NEAR(posRedo.y, 0.0f, kPosEpsilon);
    EXPECT_NEAR(posRedo.z, 0.0f, kPosEpsilon);
}

// Manipulation handles are drawn unoccluded. Every group a transform gizmo
// emits must carry AlwaysOnTop: a single SceneDepth group is enough to leave
// part of a handle dimmed to 30% alpha wherever geometry sits in front of it.
TEST_F(SceneViewTransformToolTests, TransformHandlesRenderAlwaysOnTop)
{
    World* world = GetWorld();

    Entity entity = world->Create();
    entity.Set(Transform::FromTRS(
        Vector3(0.0f, 0.0f, 0.0f),
        Quaternion(1.0f, 0.0f, 0.0f, 0.0f),
        Vector3(1.0f, 1.0f, 1.0f)));
    world->ProcessCommands();

    TransformHierarchySystem hierarchySystem;
    hierarchySystem.Update(*world, 0.0f);

    struct ModeCase
    {
        TransformMode Mode;
        const char*   Name;
    };
    const ModeCase cases[] = {
        {TransformMode::Translate, "Translate"},
        {TransformMode::Rotate, "Rotate"},
        {TransformMode::Scale, "Scale"},
    };

    const ViewId viewId = 7;
    const CameraId cameraId = 7;

    for (const ModeCase& testCase : cases)
    {
        TransformTool tool(*GetWorld());
        tool.SetTargetEntity(entity.GetHandle());
        const Vector3 pivot(0.0f, 0.0f, 0.0f);
        tool.SetPivot(pivot);
        tool.SetMode(testCase.Mode);

        GizmoCollector collector;
        tool.GatherGizmos(collector);
        ASSERT_EQ(collector.GetGizmos().size(), 1u) << testCase.Name;

        GizmoRenderContext context(viewId, cameraId);
        ResetGizmoLineGroups(viewId);
        ResetGizmoTriangleGroups(viewId);
        collector.GetGizmos()[0]->Render(context);

        // Either collection may legitimately be absent — the accessors return
        // null for an empty set, and the rotate rings are solid tubes, so that
        // gizmo emits triangles and no lines. What must hold is that the gizmo
        // drew something and that everything it drew is always-on-top.
        const auto* lineGroups = GetGizmoLineGroups(viewId);
        const auto* triGroups = GetGizmoTriangleGroups(viewId);

        std::size_t vertexCount = 0;
        if (lineGroups)
        {
            for (const auto& group : *lineGroups)
            {
                vertexCount += group.vertices.size();
                EXPECT_EQ(group.depthMode, GizmoDepthMode::AlwaysOnTop)
                    << testCase.Name << " line group";
            }
        }
        if (triGroups)
        {
            for (const auto& group : *triGroups)
            {
                vertexCount += group.vertices.size();
                EXPECT_EQ(group.depthMode, GizmoDepthMode::AlwaysOnTop)
                    << testCase.Name << " triangle group";
            }
        }
        EXPECT_GT(vertexCount, 0u) << testCase.Name << " emitted no geometry";

        // The mode is sticky context state shared by every gizmo in the frame,
        // so the scope must restore it or the descriptive gizmos rendered after
        // this one inherit always-on-top.
        EXPECT_EQ(context.GetDepthMode(), GizmoDepthMode::SceneDepth) << testCase.Name;
    }
}

// The opt-in must stay an opt-in: an unqualified draw still depth-tests, so a
// gizmo added later gets occlusion without knowing the mode exists.
TEST_F(SceneViewTransformToolTests, UnqualifiedDrawsStaySceneDepth)
{
    const ViewId viewId = 8;
    const CameraId cameraId = 8;

    GizmoRenderContext context(viewId, cameraId);
    EXPECT_EQ(context.GetDepthMode(), GizmoDepthMode::SceneDepth);

    ResetGizmoLineGroups(viewId);

    const Vector3 from(0.0f, 0.0f, 0.0f);
    const Vector3 to(1.0f, 0.0f, 0.0f);
    const GameEngine::Color color(1.0f, 1.0f, 1.0f, 1.0f);
    context.DrawColoredLine(from, to, color, 1.0f);

    const auto* lineGroups = GetGizmoLineGroups(viewId);
    ASSERT_NE(lineGroups, nullptr);
    ASSERT_EQ(lineGroups->size(), 1u);
    EXPECT_EQ((*lineGroups)[0].depthMode, GizmoDepthMode::SceneDepth);
}

} // namespace

TEST_F(SceneViewTransformToolTests, IndependentWorldBindingsDoNotEditCollidingHandles)
{
    World first;
    World second;
    auto a = first.Create();
    auto b = second.Create();
    ASSERT_EQ(a.GetHandle(), b.GetHandle());
    a.Set(Transform::FromTRS(Vector3(1, 0, 0), Quaternion(1, 0, 0, 0), Vector3(1, 1, 1)));
    b.Set(Transform::FromTRS(Vector3(10, 0, 0), Quaternion(1, 0, 0, 0), Vector3(1, 1, 1)));
    first.ProcessCommands();
    second.ProcessCommands();
    TransformHierarchySystem hierarchy;
    hierarchy.Update(first, 0);
    hierarchy.Update(second, 0);
    TransformTool selected(first);
    TransformTool unselected(second);
    selected.SetTargetEntity(a.GetHandle());
    selected.ApplyTranslationDelta(Vector3(5, 0, 0));
    unselected.ApplyTranslationDelta(Vector3(5, 0, 0));
    first.ProcessCommands();
    second.ProcessCommands();
    hierarchy.Update(first, 0);
    hierarchy.Update(second, 0);
    EXPECT_FLOAT_EQ(first.GetComponent<WorldTransform>(a.GetHandle())->matrix[12], 6);
    EXPECT_FLOAT_EQ(second.GetComponent<WorldTransform>(b.GetHandle())->matrix[12], 10);
}

TEST(SceneViewOwnership, MeasureSelectionIsLocalAndResetsWithItsWorld)
{
    World first;
    World second;
    const auto a = first.Create().GetHandle();
    const auto b = second.Create().GetHandle();
    ASSERT_EQ(a, b);
    GameEngine::Editor::SceneTools::MeasureSceneGizmo gizmoA(first);
    GameEngine::Editor::SceneTools::MeasureSceneGizmo gizmoB(second);
    gizmoA.Selection() = {a, 1};
    EXPECT_FALSE(gizmoB.Selection().IsValid());
    gizmoB.Selection() = {b, 2};
    first.Clear();
    EXPECT_FALSE(gizmoA.Selection().IsValid());
    EXPECT_EQ(gizmoB.Selection().Endpoint, 2);
}

// A press whose pick is deferred (the scene TLAS is refitting on the job
// system) is neither a miss nor a hit: the tool arms no marquee, and the
// release does not pick again. The owner applies the press once, as a click,
// when the refit finishes.
TEST_F(SceneViewTransformToolTests, DeferredPressArmsNoMarqueeAndIsNotPickedAgain)
{
    using GameEngine::Editor::SceneTools::PointerButton;
    using GameEngine::Editor::SceneTools::PointerPhase;
    using GameEngine::Editor::SceneTools::ScenePickOutcome;
    using GameEngine::Editor::SceneTools::ScenePointerEvent;

    TransformTool tool(*GetWorld());
    int picks = 0;
    int marquees = 0;
    tool.SetScenePickDelegate([&picks](const ScenePointerEvent&, bool)
    {
        ++picks;
        return ScenePickOutcome{{}, /*Deferred=*/true};
    });
    tool.SetOnEntitiesMarqueeCallback(
        [&marquees](const std::vector<GameEngine::ECS::EntityHandle>&, bool) { ++marquees; });

    ScenePointerEvent down{};
    down.button = PointerButton::Left;
    down.phase = PointerPhase::Down;
    down.viewX = 100.0f;
    down.viewY = 100.0f;
    down.ray.origin = Vector3(0.0f, 5.0f, 0.0f);
    down.ray.direction = Vector3(0.0f, -1.0f, 0.0f);
    // A marquee arms only with a camera forward to build its plane from.
    down.cameraForward = down.ray.direction;
    ScenePointerEvent up = down;
    up.phase = PointerPhase::Up;

    // Released in place.
    tool.OnPointerEvent(down);
    tool.OnPointerEvent(up);
    EXPECT_EQ(picks, 1) << "the release picked the deferred press again";
    EXPECT_EQ(marquees, 0);

    // Dragged past the marquee threshold before the release.
    ScenePointerEvent move = down;
    move.phase = PointerPhase::Move;
    move.viewX = 160.0f;
    move.viewY = 160.0f;
    ScenePointerEvent upAway = move;
    upAway.phase = PointerPhase::Up;
    tool.OnPointerEvent(down);
    tool.OnPointerEvent(move);
    tool.OnPointerEvent(upAway);
    EXPECT_EQ(picks, 2) << "one pick per press";
    EXPECT_EQ(marquees, 0) << "a deferred press armed a marquee";
}
