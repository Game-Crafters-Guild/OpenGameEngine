#include <cmath>
#include <gtest/gtest.h>
#include <limits>

#include "Core/Application.h"
#include "Core/Engine.h"

#include "Components/Spline/SplineComponent.h"
#include "Components/Terrain/Terrain.h"
#include "Components/Transform.h"
#include "Editor/Settings/SplineEditorSettings.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "Input/KeyCodes.h"
#include "Terrain/TerrainTypes.h"
#include "Mathematics/Quaternion.h"
#include "Mathematics/Vector3.h"
#include "SceneView/SplineOwnerQuery.h"
#include "SceneView/SplineSceneGizmo.h"
#include "SceneView/SplineTool.h"
#include "SceneView/SplineToolStripEntry.h"
#include "Scripting/ScriptsConfig.h"
#include "Spline/SplineData.h"
#include "SplineECS/SplineService.h"
#include "TerrainECS/TerrainService.h"
#include "Types/ColorUtils.h"
#include "UndoRedo/UndoRedoService.h"

using GameEngine::ApplicationConfig;
using GameEngine::EngineCore;

using GameEngine::Components::SplineComponent;
using GameEngine::Components::Terrain;
using GameEngine::Components::Transform;
using GameEngine::Components::WorldTransform;
using GameEngine::ECS::Entity;
using GameEngine::ECS::World;
using GameEngine::Mathematics::Quaternion;
using GameEngine::Mathematics::Vector3;

using GameEngine::Editor::SceneTools::ComputeSplineScreenScale;
using GameEngine::Editor::SceneTools::AcquireSplineInteractionState;
using GameEngine::Editor::SceneTools::SplineInteractionState;
using GameEngine::Editor::SceneTools::GizmoRay;
using GameEngine::Editor::SceneTools::GizmoRenderContext;
using GameEngine::Editor::SceneTools::PickScreenScale;
using GameEngine::Editor::SceneTools::PointerButton;
using GameEngine::Editor::SceneTools::PointerPhase;
using GameEngine::Editor::SceneTools::ScenePointerEvent;
using GameEngine::Editor::SceneTools::SplineTool;
using GameEngine::Rendering::CameraId;
using GameEngine::Rendering::ViewId;

namespace
{

class SplineToolTests : public ::testing::Test
{
  protected:
    static void SetUpTestSuite()
    {
        EngineCore& engine = EngineCore::GetInstance();
        if (!engine.IsInitialized())
        {
            GameEngine::ScriptsConfig scriptsConfig{};
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
        if (!GameEngine::SplineECS::SplineService::IsInitialized())
            GameEngine::SplineECS::SplineService::Initialize();
        if (!GameEngine::TerrainECS::TerrainService::IsInitialized())
            GameEngine::TerrainECS::TerrainService::Initialize();
    }

    void SetUp() override
    {
        World* world = GetWorld();
        ASSERT_NE(world, nullptr);
        world->Clear();
        State = AcquireSplineInteractionState(*world);
        State->Selection() = {};
        State->Hover() = {};
    }

    std::shared_ptr<SplineInteractionState> State;

    World* GetWorld() const
    {
        return EngineCore::GetInstance().EnsurePrimaryWorld();
    }
};

class ScopedSplineStickToMeshSetting
{
  public:
    explicit ScopedSplineStickToMeshSetting(bool value)
        : m_Previous(GameEngine::Editor::SplineEditorSettings::Get().GetStickToMesh())
    {
        GameEngine::Editor::SplineEditorSettings::Get().SetStickToMesh(value);
    }

    ~ScopedSplineStickToMeshSetting()
    {
        GameEngine::Editor::SplineEditorSettings::Get().SetStickToMesh(m_Previous);
    }

  private:
    bool m_Previous = false;
};

Vector3 Normalized(Vector3 v)
{
    const float lenSq = Vector3::Dot(v, v);
    if (lenSq <= 0.0f)
        return Vector3(0.0f, 0.0f, -1.0f);
    return v * (1.0f / std::sqrt(lenSq));
}

ScenePointerEvent MakePointer(PointerPhase phase,
                              float viewX,
                              float viewY,
                              const Vector3& rayOrigin,
                              const Vector3& rayDirection)
{
    ScenePointerEvent event{};
    event.phase = phase;
    event.button = PointerButton::Left;
    event.viewX = viewX;
    event.viewY = viewY;
    event.viewW = 800.0f;
    event.viewH = 600.0f;
    event.ray.origin = rayOrigin;
    event.ray.direction = Normalized(rayDirection);
    event.cameraPos = rayOrigin;
    event.cameraRight = Vector3(1.0f, 0.0f, 0.0f);
    event.cameraUp = Vector3(0.0f, 1.0f, 0.0f);
    event.cameraForward = Vector3(0.0f, 0.0f, -1.0f);
    event.tanHalfFovY = 0.5f;
    return event;
}

TEST_F(SplineToolTests, DirectKnotDragUsesStableViewPlaneWhenSceneRayMisses)
{
    World* world = GetWorld();
    auto* splineService = GameEngine::SplineECS::SplineService::TryGet();
    ASSERT_NE(splineService, nullptr);

    const auto splineHandle = splineService->CreateSpline(GameEngine::Spline::SplineType::Linear, false);
    auto* splineData = splineService->GetSplineData(splineHandle);
    ASSERT_NE(splineData, nullptr);
    splineData->AddPoint(Vector3(0.0f, 10.0f, 0.0f), 1.0f);
    splineData->AddPoint(Vector3(4.0f, 10.0f, 0.0f), 1.0f);
    splineService->RebuildCache(splineHandle);

    Entity entity = world->Create();
    entity.Set(Transform::FromTRS(
        Vector3(0.0f, 0.0f, 0.0f),
        Quaternion(1.0f, 0.0f, 0.0f, 0.0f),
        Vector3(1.0f, 1.0f, 1.0f)));

    SplineComponent spline{};
    spline.SplineDataIndex = splineHandle.Index();
    spline.SplineDataGeneration = splineHandle.Generation();
    entity.Set(spline);
    world->ProcessCommands();

    SplineTool tool(*GetWorld());
    tool.SetSelectionListQuery([handle = entity.GetHandle()] {
        return std::vector<GameEngine::ECS::EntityHandle>{handle};
    });

    const Vector3 camera(0.0f, 10.0f, 10.0f);
    tool.OnPointerEvent(MakePointer(
        PointerPhase::Down,
        100.0f,
        100.0f,
        camera,
        Vector3(0.0f, 0.0f, -1.0f)));

    tool.OnPointerEvent(MakePointer(
        PointerPhase::Move,
        120.0f,
        100.0f,
        camera,
        Vector3(2.0f, 0.0f, -10.0f)));

    ASSERT_GE(splineData->Points.size(), 1u);
    EXPECT_NEAR(splineData->Points[0].Position.x, 2.0f, 1.0e-4f);
    EXPECT_NEAR(splineData->Points[0].Position.y, 10.0f, 1.0e-4f);
    EXPECT_NEAR(splineData->Points[0].Position.z, 0.0f, 1.0e-4f);

    tool.OnPointerEvent(MakePointer(
        PointerPhase::Move,
        140.0f,
        100.0f,
        camera,
        Vector3(3.0f, 0.0f, -10.0f)));

    EXPECT_NEAR(splineData->Points[0].Position.x, 3.0f, 1.0e-4f);
    EXPECT_NEAR(splineData->Points[0].Position.y, 10.0f, 1.0e-4f);
    EXPECT_NEAR(splineData->Points[0].Position.z, 0.0f, 1.0e-4f);

    tool.OnPointerEvent(MakePointer(
        PointerPhase::Up,
        140.0f,
        100.0f,
        camera,
        Vector3(3.0f, 0.0f, -10.0f)));
}

// The edit-drag plane normal comes from the camera forward behind a fallback
// ladder: camera forward, then the pointer ray, then +Z. Only the normalize's
// boolean can step that ladder, so a guard that reports success on a
// non-finite input pins a NaN plane and every pointer position projects onto
// it. The ray rung here carries the same direction the healthy camera does, so
// a working ladder must land the knot exactly where the sibling drag test
// above lands it.
TEST_F(SplineToolTests, DirectKnotDragFallsBackWhenCameraForwardIsNonFinite)
{
    World* world = GetWorld();
    auto* splineService = GameEngine::SplineECS::SplineService::TryGet();
    ASSERT_NE(splineService, nullptr);

    const auto splineHandle = splineService->CreateSpline(GameEngine::Spline::SplineType::Linear, false);
    auto* splineData = splineService->GetSplineData(splineHandle);
    ASSERT_NE(splineData, nullptr);
    splineData->AddPoint(Vector3(0.0f, 10.0f, 0.0f), 1.0f);
    splineData->AddPoint(Vector3(4.0f, 10.0f, 0.0f), 1.0f);
    splineService->RebuildCache(splineHandle);

    Entity entity = world->Create();
    entity.Set(Transform::FromTRS(
        Vector3(0.0f, 0.0f, 0.0f),
        Quaternion(1.0f, 0.0f, 0.0f, 0.0f),
        Vector3(1.0f, 1.0f, 1.0f)));

    SplineComponent spline{};
    spline.SplineDataIndex = splineHandle.Index();
    spline.SplineDataGeneration = splineHandle.Generation();
    entity.Set(spline);
    world->ProcessCommands();

    SplineTool tool(*GetWorld());
    tool.SetSelectionListQuery([handle = entity.GetHandle()] {
        return std::vector<GameEngine::ECS::EntityHandle>{handle};
    });

    const float nan = std::numeric_limits<float>::quiet_NaN();
    auto withNonFiniteForward = [nan](ScenePointerEvent event) {
        event.cameraForward = Vector3(nan, nan, nan);
        return event;
    };

    const Vector3 camera(0.0f, 10.0f, 10.0f);
    tool.OnPointerEvent(withNonFiniteForward(MakePointer(
        PointerPhase::Down,
        100.0f,
        100.0f,
        camera,
        Vector3(0.0f, 0.0f, -1.0f))));

    tool.OnPointerEvent(withNonFiniteForward(MakePointer(
        PointerPhase::Move,
        120.0f,
        100.0f,
        camera,
        Vector3(2.0f, 0.0f, -10.0f))));

    ASSERT_GE(splineData->Points.size(), 1u);
    EXPECT_TRUE(std::isfinite(splineData->Points[0].Position.x));
    EXPECT_TRUE(std::isfinite(splineData->Points[0].Position.y));
    EXPECT_TRUE(std::isfinite(splineData->Points[0].Position.z));
    EXPECT_NEAR(splineData->Points[0].Position.x, 2.0f, 1.0e-4f);
    EXPECT_NEAR(splineData->Points[0].Position.y, 10.0f, 1.0e-4f);
    EXPECT_NEAR(splineData->Points[0].Position.z, 0.0f, 1.0e-4f);

    tool.OnPointerEvent(withNonFiniteForward(MakePointer(
        PointerPhase::Up,
        140.0f,
        100.0f,
        camera,
        Vector3(3.0f, 0.0f, -10.0f))));
}

TEST_F(SplineToolTests, BrushStrokeCanContinueAcrossTerrain)
{
    ScopedSplineStickToMeshSetting stickToMesh(true);

    World* world = GetWorld();
    auto* terrainService = GameEngine::TerrainECS::TerrainService::TryGet();
    auto* splineService = GameEngine::SplineECS::SplineService::TryGet();
    ASSERT_NE(terrainService, nullptr);
    ASSERT_NE(splineService, nullptr);

    GameEngine::Terrain::TerrainConfig config{};
    config.HeightmapWidth = 9;
    config.HeightmapHeight = 9;
    config.WorldSizeX = 20.0f;
    config.WorldSizeZ = 20.0f;
    config.HeightScale = 1.0f;
    config.LODLevels = 1;
    config.PatchGridSize = 8;

    const auto terrainHandle = terrainService->CreateTerrain(config);
    ASSERT_NE(terrainHandle.Generation, 0u);

    Entity terrainEntity = world->Create();
    Terrain terrain{};
    terrain.SizeX = config.WorldSizeX;
    terrain.SizeZ = config.WorldSizeZ;
    terrain.HeightScale = config.HeightScale;
    terrain.TerrainDataHandle = terrainHandle.Index;
    terrain.TerrainDataGeneration = terrainHandle.Generation;
    terrainEntity.Set(terrain);
    terrainEntity.Set(WorldTransform{});
    world->ProcessCommands();

    SplineTool tool(*GetWorld());
    tool.OnPointerEvent(MakePointer(
        PointerPhase::Down,
        100.0f,
        100.0f,
        Vector3(-2.0f, 10.0f, 0.0f),
        Vector3(0.0f, -1.0f, 0.0f)));

    tool.OnPointerEvent(MakePointer(
        PointerPhase::Move,
        120.0f,
        100.0f,
        Vector3(0.0f, 10.0f, 0.0f),
        Vector3(0.0f, -1.0f, 0.0f)));

    tool.OnPointerEvent(MakePointer(
        PointerPhase::Up,
        140.0f,
        100.0f,
        Vector3(2.0f, 10.0f, 0.0f),
        Vector3(0.0f, -1.0f, 0.0f)));

    int splineEntityCount = 0;
    const GameEngine::Spline::SplineData* createdSpline = nullptr;
    world->Query<GameEngine::ECS::Read<SplineComponent>>()
        .Each([&](GameEngine::ECS::EntityHandle, const SplineComponent& spline)
        {
            ++splineEntityCount;
            const GameEngine::SplineECS::SplineHandle handle(
                spline.SplineDataIndex, spline.SplineDataGeneration);
            createdSpline = splineService->GetSplineData(handle);
        });

    EXPECT_EQ(splineEntityCount, 1);
    ASSERT_NE(createdSpline, nullptr);
    EXPECT_GE(createdSpline->Points.size(), 2u);
}

// Escape cancels a brush stroke mid-drag: the Scene View hands the tool the input layer's key
// codes, so the rest of the drag and its release make no spline.
TEST_F(SplineToolTests, EscapeCancelsABrushStrokeMidDrag)
{
    World* world = GetWorld();
    auto* terrainService = GameEngine::TerrainECS::TerrainService::TryGet();
    ASSERT_NE(terrainService, nullptr);
    GameEngine::Terrain::TerrainConfig config{};
    config.HeightmapWidth = 9;
    config.HeightmapHeight = 9;
    config.WorldSizeX = 20.0f;
    config.WorldSizeZ = 20.0f;
    config.HeightScale = 1.0f;
    config.LODLevels = 1;
    config.PatchGridSize = 8;
    const auto terrainHandle = terrainService->CreateTerrain(config);
    ASSERT_NE(terrainHandle.Generation, 0u);
    Entity terrainEntity = world->Create();
    Terrain terrain{};
    terrain.SizeX = config.WorldSizeX;
    terrain.SizeZ = config.WorldSizeZ;
    terrain.HeightScale = config.HeightScale;
    terrain.TerrainDataHandle = terrainHandle.Index;
    terrain.TerrainDataGeneration = terrainHandle.Generation;
    terrainEntity.Set(terrain);
    terrainEntity.Set(WorldTransform{});
    world->ProcessCommands();

    SplineTool tool(*world);
    const auto pointer = [](PointerPhase phase, float x) {
        return MakePointer(phase, 100.0f + x * 10.0f, 100.0f, Vector3(x, 10.0f, 0.0f), Vector3(0.0f, -1.0f, 0.0f));
    };
    tool.OnPointerEvent(pointer(PointerPhase::Down, -3.0f));
    tool.OnPointerEvent(pointer(PointerPhase::Move, -1.0f));
    GameEngine::Editor::SceneTools::SceneKeyEvent escape{};
    escape.keyCode = static_cast<std::uint32_t>(GameEngine::Input::kKeyCode_Escape);
    escape.pressed = true;
    tool.OnKeyEvent(escape);
    tool.OnPointerEvent(pointer(PointerPhase::Move, 1.0f));
    tool.OnPointerEvent(pointer(PointerPhase::Up, 3.0f));

    int splines = 0;
    world->Query<GameEngine::ECS::Read<SplineComponent>>().Each(
        [&splines](GameEngine::ECS::EntityHandle, const SplineComponent&) { ++splines; });
    EXPECT_EQ(splines, 0) << "the stroke Escape cancelled made a spline at its release";
}

// ---- Control point deletion ----

struct SplineFixture
{
    GameEngine::SplineECS::SplineHandle Handle{};
    GameEngine::Spline::SplineData* Data = nullptr;
    GameEngine::ECS::EntityHandle SplineEntity{};
};

SplineFixture MakeSplineEntity(World* world, uint32_t pointCount, bool closed = false)
{
    SplineFixture fixture;
    auto* splineService = GameEngine::SplineECS::SplineService::TryGet();
    EXPECT_NE(splineService, nullptr);

    fixture.Handle = splineService->CreateSpline(GameEngine::Spline::SplineType::Linear, closed);
    fixture.Data = splineService->GetSplineData(fixture.Handle);
    EXPECT_NE(fixture.Data, nullptr);

    for (uint32_t i = 0; i < pointCount; ++i)
    {
        fixture.Data->AddPoint(Vector3(static_cast<float>(i) * 4.0f, 10.0f, 0.0f),
                               1.0f + static_cast<float>(i));
        fixture.Data->SetPointRoll(i, 0.25f * static_cast<float>(i + 1));
        fixture.Data->SetPointTangents(i,
                                       Vector3(-1.0f * static_cast<float>(i + 1), 0.0f, 0.0f),
                                       Vector3(1.0f * static_cast<float>(i + 1), 0.0f, 0.0f));
    }
    splineService->RebuildCache(fixture.Handle);

    Entity entity = world->Create();
    entity.Set(Transform::FromTRS(
        Vector3(0.0f, 0.0f, 0.0f),
        Quaternion(1.0f, 0.0f, 0.0f, 0.0f),
        Vector3(1.0f, 1.0f, 1.0f)));

    SplineComponent spline{};
    spline.SplineDataIndex = fixture.Handle.Index();
    spline.SplineDataGeneration = fixture.Handle.Generation();
    entity.Set(spline);
    world->ProcessCommands();

    fixture.SplineEntity = entity.GetHandle();
    return fixture;
}

void SelectKnot(SplineInteractionState& state, GameEngine::ECS::EntityHandle entity, int32_t index)
{
    auto& selection = state.Selection();
    selection = {};
    selection.Entity = entity;
    selection.PointIndex = index;
    selection.Kind = 1;
    selection.KnotIndices = {index};
    selection.Controls = {{entity, index, 1}};
}

TEST_F(SplineToolTests, DeleteSelectedControlPointErasesOnlyThatKnot)
{
    World* world = GetWorld();
    SplineFixture fixture = MakeSplineEntity(world, 4);

    const Vector3 p0 = fixture.Data->Points[0].Position;
    const Vector3 p2 = fixture.Data->Points[2].Position;
    const Vector3 p3 = fixture.Data->Points[3].Position;

    SplineTool tool(*GetWorld());
    SelectKnot(*State, fixture.SplineEntity, 1);

    EXPECT_TRUE(tool.DeleteSelectedControlPoints());

    ASSERT_EQ(fixture.Data->Points.size(), 3u);
    EXPECT_FLOAT_EQ(fixture.Data->Points[0].Position.x, p0.x);
    EXPECT_FLOAT_EQ(fixture.Data->Points[1].Position.x, p2.x);
    EXPECT_FLOAT_EQ(fixture.Data->Points[2].Position.x, p3.x);
}

// The whole point of the feature: with no knot selected the tool must decline
// the key so the scene view can still delete the entity.
TEST_F(SplineToolTests, DeleteWithNoKnotSelectedIsDeclined)
{
    World* world = GetWorld();
    SplineFixture fixture = MakeSplineEntity(world, 4);

    SplineTool tool(*GetWorld());
    State->Selection() = {};

    EXPECT_FALSE(tool.DeleteSelectedControlPoints());
    EXPECT_EQ(fixture.Data->Points.size(), 4u);
}

// A Bezier handle is a property of its point, not a deletable control.
TEST_F(SplineToolTests, DeleteWithOnlyAHandleSelectedIsDeclined)
{
    World* world = GetWorld();
    SplineFixture fixture = MakeSplineEntity(world, 4);

    SplineTool tool(*GetWorld());
    auto& selection = State->Selection();
    selection = {};
    selection.Entity = fixture.SplineEntity;
    selection.PointIndex = 1;
    selection.Kind = 2; // handle in
    selection.Controls = {{fixture.SplineEntity, 1, 2}};

    EXPECT_FALSE(tool.DeleteSelectedControlPoints());
    EXPECT_EQ(fixture.Data->Points.size(), 4u);
}

// At the two-point floor the removal is refused — but the key is still
// reported as handled, because falling through would delete the entity, which
// is precisely the behaviour this path exists to prevent.
TEST_F(SplineToolTests, DeleteAtTwoPointFloorIsRefusedYetHandled)
{
    World* world = GetWorld();
    SplineFixture fixture = MakeSplineEntity(world, 2);

    SplineTool tool(*GetWorld());
    SelectKnot(*State, fixture.SplineEntity, 0);

    EXPECT_TRUE(tool.DeleteSelectedControlPoints());
    EXPECT_EQ(fixture.Data->Points.size(), 2u);
}

TEST_F(SplineToolTests, DeleteMovesSelectionToASurvivingNeighbour)
{
    World* world = GetWorld();
    SplineFixture fixture = MakeSplineEntity(world, 4);

    SplineTool tool(*GetWorld());

    // Erasing a middle knot leaves the selection on the point that took its
    // index, so a repeated Delete keeps walking the spline.
    SelectKnot(*State, fixture.SplineEntity, 1);
    ASSERT_TRUE(tool.DeleteSelectedControlPoints());
    EXPECT_EQ(State->Selection().PointIndex, 1);
    EXPECT_EQ(State->Selection().Kind, 1);
    EXPECT_EQ(State->Selection().Entity, fixture.SplineEntity);

    // Erasing the tail clamps onto the new last point.
    ASSERT_EQ(fixture.Data->Points.size(), 3u);
    SelectKnot(*State, fixture.SplineEntity, 2);
    ASSERT_TRUE(tool.DeleteSelectedControlPoints());
    ASSERT_EQ(fixture.Data->Points.size(), 2u);
    EXPECT_EQ(State->Selection().PointIndex, 1);
}

TEST_F(SplineToolTests, DeleteErasesEverySelectedKnotInOneStep)
{
    World* world = GetWorld();
    SplineFixture fixture = MakeSplineEntity(world, 5);

    const Vector3 p0 = fixture.Data->Points[0].Position;
    const Vector3 p2 = fixture.Data->Points[2].Position;
    const Vector3 p4 = fixture.Data->Points[4].Position;

    SplineTool tool(*GetWorld());
    auto& selection = State->Selection();
    selection = {};
    selection.Entity = fixture.SplineEntity;
    selection.PointIndex = 3;
    selection.Kind = 1;
    selection.KnotIndices = {1, 3};
    selection.Controls = {{fixture.SplineEntity, 1, 1},
                          {fixture.SplineEntity, 3, 1}};

    EXPECT_TRUE(tool.DeleteSelectedControlPoints());

    ASSERT_EQ(fixture.Data->Points.size(), 3u);
    EXPECT_FLOAT_EQ(fixture.Data->Points[0].Position.x, p0.x);
    EXPECT_FLOAT_EQ(fixture.Data->Points[1].Position.x, p2.x);
    EXPECT_FLOAT_EQ(fixture.Data->Points[2].Position.x, p4.x);
}

// Deleting more knots than the floor allows is all-or-nothing: a partial erase
// would leave the author guessing which knots survived.
TEST_F(SplineToolTests, DeleteIsRefusedWhenTheSelectionWouldBreachTheFloor)
{
    World* world = GetWorld();
    SplineFixture fixture = MakeSplineEntity(world, 3);

    SplineTool tool(*GetWorld());
    auto& selection = State->Selection();
    selection = {};
    selection.Entity = fixture.SplineEntity;
    selection.PointIndex = 2;
    selection.Kind = 1;
    selection.KnotIndices = {0, 1, 2};
    selection.Controls = {{fixture.SplineEntity, 0, 1},
                          {fixture.SplineEntity, 1, 1},
                          {fixture.SplineEntity, 2, 1}};

    EXPECT_TRUE(tool.DeleteSelectedControlPoints());
    EXPECT_EQ(fixture.Data->Points.size(), 3u);
}

// Undo must put the point back at its index with every authored channel
// intact, not merely restore the count.
TEST_F(SplineToolTests, UndoRestoresTheDeletedPointExactly)
{
    World* world = GetWorld();
    SplineFixture fixture = MakeSplineEntity(world, 4);

    const std::vector<GameEngine::Spline::SplineControlPoint> before = fixture.Data->Points;

    GameEngine::Editor::UndoRedoService undo;
    SplineTool tool(*GetWorld());
    tool.SetUndoRedoService(&undo);
    SelectKnot(*State, fixture.SplineEntity, 1);

    ASSERT_TRUE(tool.DeleteSelectedControlPoints());
    ASSERT_EQ(fixture.Data->Points.size(), 3u);
    ASSERT_TRUE(undo.CanUndo());

    undo.Undo();

    ASSERT_EQ(fixture.Data->Points.size(), before.size());
    for (size_t i = 0; i < before.size(); ++i)
    {
        EXPECT_FLOAT_EQ(fixture.Data->Points[i].Position.x, before[i].Position.x) << "Position " << i;
        EXPECT_FLOAT_EQ(fixture.Data->Points[i].Position.y, before[i].Position.y) << "Position " << i;
        EXPECT_FLOAT_EQ(fixture.Data->Points[i].Radius, before[i].Radius) << "Radius " << i;
        EXPECT_FLOAT_EQ(fixture.Data->Points[i].Roll, before[i].Roll) << "Roll " << i;
        EXPECT_FLOAT_EQ(fixture.Data->Points[i].TangentIn.x, before[i].TangentIn.x) << "TangentIn " << i;
        EXPECT_FLOAT_EQ(fixture.Data->Points[i].TangentOut.x, before[i].TangentOut.x) << "TangentOut " << i;
    }

    undo.Redo();
    EXPECT_EQ(fixture.Data->Points.size(), 3u);
}

// The spline tool the tool strip registers edits through its view's undo service, and
// takes the Delete key through the tool hook the view calls.
TEST_F(SplineToolTests, TheRegisteredSplineToolUndoesTheEditItsDeleteHookMade)
{
    World* world = GetWorld();
    SplineFixture fixture = MakeSplineEntity(world, 4);

    GameEngine::Editor::UndoRedoService undo;
    GameEngine::Editor::SplineToolServices services;
    services.Undo = &undo;
    const std::unique_ptr<GameEngine::Editor::SceneTools::ISceneTool> tool =
        GameEngine::Editor::CreateSplineTool(*world, std::move(services));
    SelectKnot(*State, fixture.SplineEntity, 1);

    ASSERT_TRUE(tool->DeleteSelection());
    ASSERT_EQ(fixture.Data->Points.size(), 3u);
    ASSERT_TRUE(undo.CanUndo());
    undo.Undo();
    EXPECT_EQ(fixture.Data->Points.size(), 4u);
}

// Selecting a spline makes the spline entry claim the view, and a selected knot is what
// Frame Selected centres on.
TEST_F(SplineToolTests, TheSplineEntryClaimsASplineSelectionAndFramesItsKnot)
{
    World* world = GetWorld();
    SplineFixture fixture = MakeSplineEntity(world, 4);
    const GameEngine::ECS::EntityHandle plain = world->Create().GetHandle();
    const GameEngine::Editor::SceneViewToolStripEntry entry = GameEngine::Editor::MakeSplineToolStripEntry();
    ASSERT_TRUE(entry.ActivatesForSelection);
    ASSERT_TRUE(entry.FrameTarget);

    EXPECT_FALSE(entry.ActivatesForSelection(*world, plain, {plain}));
    EXPECT_TRUE(entry.ActivatesForSelection(*world, plain, {plain, fixture.SplineEntity}));

    Vector3 target{};
    EXPECT_FALSE(entry.FrameTarget(*world, target));
    SelectKnot(*State, fixture.SplineEntity, 2);
    ASSERT_TRUE(entry.FrameTarget(*world, target));
    EXPECT_FLOAT_EQ(target.x, fixture.Data->Points[2].Position.x);
}

// A spline another feature owns as its shape (a mark-up region's outline) is left out of "show
// all controls": a road's edit never offers its knots, its own edit offers no road's, and a
// hidden owner's knots never show, while the selected region still offers its own.
TEST_F(SplineToolTests, AClaimedSplineOffersItsKnotsOnlyWhileSelectedAndShown)
{
    World* world = GetWorld();
    auto& settings = GameEngine::Editor::SplineEditorSettings::Get();
    const bool showAllBefore = settings.GetShowAllControls();
    settings.SetShowAllControls(true);
    const SplineFixture road = MakeSplineEntity(world, 2); // knots at x = 0 and 4
    const SplineFixture region = MakeSplineEntity(world, 3, /*closed=*/true);
    for (std::size_t i = 0; i < region.Data->Points.size(); ++i)
        region.Data->Points[i].Position.x += 20.0f; // knots at x = 20, 24 and 28
    GameEngine::SplineECS::SplineService::TryGet()->RebuildCache(region.Handle);
    bool regionHidden = false;
    GameEngine::Editor::SetSplineOwnerQuery(
        [&](const GameEngine::ECS::World&, GameEngine::ECS::EntityHandle entity) {
            return GameEngine::Editor::SplineOwnerClaim{entity == region.SplineEntity,
                                                         entity == region.SplineEntity && regionHidden};
        });

    SplineTool tool(*world);
    GameEngine::ECS::EntityHandle selected = road.SplineEntity;
    tool.SetSelectionListQuery([&selected] { return std::vector<GameEngine::ECS::EntityHandle>{selected}; });
    // The entity whose knot the pointer hovers when aimed straight at the knot at `x`.
    const auto hoveredAt = [&](float x) {
        ScenePointerEvent move = MakePointer(PointerPhase::Move, 100.0f, 100.0f, Vector3(x, 10.0f, 10.0f),
                                             Vector3(0.0f, 0.0f, -1.0f));
        move.button = PointerButton::None;
        tool.OnPointerEvent(move);
        return State->Hover().Entity;
    };

    EXPECT_EQ(hoveredAt(4.0f), road.SplineEntity) << "positive control: the selected road's own knot";
    EXPECT_FALSE(hoveredAt(24.0f) == region.SplineEntity) << "a road edit offered the region's knot";
    selected = region.SplineEntity;
    EXPECT_EQ(hoveredAt(24.0f), region.SplineEntity) << "the selected region did not offer its own knot";
    EXPECT_FALSE(hoveredAt(4.0f) == road.SplineEntity) << "a region edit offered the road's knot";
    regionHidden = true;
    EXPECT_FALSE(hoveredAt(24.0f).IsValid()) << "a hidden region offered its knot";

    GameEngine::Editor::SetSplineOwnerQuery({});
    settings.SetShowAllControls(showAllBefore);
}

// The spline gizmo draws knots by the same rule: while a region is selected, "show all controls"
// draws no road's knots (the vertices drawn match the setting off), while a selected road's edit
// draws the other road's (the positive control: the count sees knots).
TEST_F(SplineToolTests, AClaimedSplinesEditDrawsNoOtherSplinesKnots)
{
    World* world = GetWorld();
    auto& settings = GameEngine::Editor::SplineEditorSettings::Get();
    const bool showAllBefore = settings.GetShowAllControls();
    const bool drapeBefore = settings.GetDrapeToSurface();
    settings.SetDrapeToSurface(false);
    const SplineFixture road = MakeSplineEntity(world, 2);    // knots at x = 0 and 4
    const SplineFixture otherRoad = MakeSplineEntity(world, 2);
    const SplineFixture region = MakeSplineEntity(world, 3, /*closed=*/true);
    // Creating a spline can move the service's spline storage: write through the handles.
    auto* splines = GameEngine::SplineECS::SplineService::TryGet();
    for (auto& point : splines->GetSplineData(otherRoad.Handle)->Points)
        point.Position.z += 20.0f;
    for (auto& point : splines->GetSplineData(region.Handle)->Points)
        point.Position.x += 20.0f;
    splines->RebuildCache(otherRoad.Handle);
    splines->RebuildCache(region.Handle);
    for (const GameEngine::ECS::EntityHandle entity : {road.SplineEntity, otherRoad.SplineEntity, region.SplineEntity})
        world->AddComponentImmediate(entity, WorldTransform{});
    GameEngine::Editor::SetSplineOwnerQuery([&](const GameEngine::ECS::World&, GameEngine::ECS::EntityHandle entity) {
        return GameEngine::Editor::SplineOwnerClaim{entity == region.SplineEntity, false};
    });

    constexpr GameEngine::Rendering::ViewId kView = 9081;
    GameEngine::Editor::SceneTools::SplineSceneGizmo gizmo(*world);
    GameEngine::ECS::EntityHandle selected = region.SplineEntity;
    gizmo.SetSelectionQuery([&selected] { return selected; });
    const auto drawnVertices = [&](bool showAll) {
        settings.SetShowAllControls(showAll);
        GameEngine::Editor::SceneTools::ResetGizmoLineGroups(kView);
        GameEngine::Editor::SceneTools::ResetGizmoTriangleGroups(kView);
        const Vector3 camera(10.0f, 40.0f, -40.0f);
        GameEngine::Editor::SceneTools::GizmoRenderContext context(kView, kView, &camera, nullptr, world);
        gizmo.Render(context);
        std::size_t count = 0;
        if (const auto* lines = GameEngine::Editor::SceneTools::GetGizmoLineGroups(kView))
            for (const auto& group : *lines)
                count += group.vertices.size();
        if (const auto* triangles = GameEngine::Editor::SceneTools::GetGizmoTriangleGroups(kView))
            for (const auto& group : *triangles)
                count += group.vertices.size();
        return count;
    };

    const std::size_t regionAlone = drawnVertices(false);
    EXPECT_EQ(drawnVertices(true), regionAlone) << "a region edit drew a road's knots";
    selected = road.SplineEntity;
    const std::size_t roadAlone = drawnVertices(false);
    EXPECT_GT(drawnVertices(true), roadAlone) << "positive control: a road edit draws the other road's knots";

    GameEngine::Editor::SceneTools::ResetGizmoLineGroups(kView);
    GameEngine::Editor::SceneTools::ResetGizmoTriangleGroups(kView);
    GameEngine::Editor::SetSplineOwnerQuery({});
    settings.SetShowAllControls(showAllBefore);
    settings.SetDrapeToSurface(drapeBefore);
}

// A stroke ending on a region's knot makes a spline of its own: the auto-connect never joins a
// region's outline (it skips closed splines), while the same stroke ending on a road's end joins it.
TEST_F(SplineToolTests, AStrokeEndingOnARegionsKnotNeverJoinsIt)
{
    World* world = GetWorld();
    auto* terrainService = GameEngine::TerrainECS::TerrainService::TryGet();
    ASSERT_NE(terrainService, nullptr);
    GameEngine::Terrain::TerrainConfig config{};
    config.HeightmapWidth = 9;
    config.HeightmapHeight = 9;
    config.WorldSizeX = 40.0f;
    config.WorldSizeZ = 40.0f;
    config.HeightScale = 1.0f;
    config.LODLevels = 1;
    config.PatchGridSize = 8;
    const auto terrainHandle = terrainService->CreateTerrain(config);
    ASSERT_NE(terrainHandle.Generation, 0u);
    Entity terrainEntity = world->Create();
    Terrain terrain{};
    terrain.SizeX = config.WorldSizeX;
    terrain.SizeZ = config.WorldSizeZ;
    terrain.HeightScale = config.HeightScale;
    terrain.TerrainDataHandle = terrainHandle.Index;
    terrain.TerrainDataGeneration = terrainHandle.Generation;
    terrainEntity.Set(terrain);
    terrainEntity.Set(WorldTransform{});
    world->ProcessCommands();

    auto& settings = GameEngine::Editor::SplineEditorSettings::Get();
    const bool autoConnectBefore = settings.GetAutoConnect();
    settings.SetAutoConnect(true);
    // Knots on the ground (y = 0): the region's first at x = 0, the road's last at x = 4 + 10.
    const SplineFixture region = MakeSplineEntity(world, 3, /*closed=*/true);
    const SplineFixture road = MakeSplineEntity(world, 2);
    // Creating a spline can move the service's spline storage: every read and write goes through
    // the handle, never a SplineData pointer taken before.
    auto* splines = GameEngine::SplineECS::SplineService::TryGet();
    for (auto& point : splines->GetSplineData(region.Handle)->Points)
        point.Position = Vector3(point.Position.x, 0.0f, point.Position.x * 0.5f - 8.0f);
    for (auto& point : splines->GetSplineData(road.Handle)->Points)
        point.Position = Vector3(point.Position.x + 10.0f, 0.0f, 0.0f);
    splines->RebuildCache(region.Handle);
    splines->RebuildCache(road.Handle);
    GameEngine::Editor::SetSplineOwnerQuery([&](const GameEngine::ECS::World&, GameEngine::ECS::EntityHandle entity) {
        return GameEngine::Editor::SplineOwnerClaim{entity == region.SplineEntity, false};
    });

    SplineTool tool(*world);
    // A brush stroke along z = `z` from x = -6 to `endX`, straight down onto the terrain.
    const auto stroke = [&tool](float z, float endX) {
        const auto pointer = [z](PointerPhase phase, float x) {
            return MakePointer(phase, 100.0f + x * 10.0f, 100.0f, Vector3(x, 10.0f, z), Vector3(0.0f, -1.0f, 0.0f));
        };
        tool.OnPointerEvent(pointer(PointerPhase::Down, -6.0f));
        tool.OnPointerEvent(pointer(PointerPhase::Move, (endX - 6.0f) * 0.5f));
        tool.OnPointerEvent(pointer(PointerPhase::Up, endX));
    };
    const auto knotsOf = [splines](const SplineFixture& fixture) -> std::size_t {
        const auto* data = splines->GetSplineData(fixture.Handle);
        return data ? data->Points.size() : 0u;
    };
    const auto splineCount = [world] {
        int count = 0;
        world->Query<GameEngine::ECS::Read<SplineComponent>>().Each(
            [&count](GameEngine::ECS::EntityHandle, const SplineComponent&) { ++count; });
        return count;
    };

    stroke(-8.0f, -0.5f); // ends 0.5 m from the region's knot at (0, 0, -8)
    EXPECT_EQ(knotsOf(region), 3u) << "the stroke joined the region's outline";
    EXPECT_EQ(splineCount(), 3) << "the stroke made no spline of its own";

    const std::size_t roadKnots = knotsOf(road);
    stroke(0.0f, 9.5f); // ends 0.5 m from the road's first knot at (10, 0, 0)
    EXPECT_GT(knotsOf(road), roadKnots) << "positive control: the stroke did not join the road";
    EXPECT_EQ(splineCount(), 3);

    GameEngine::Editor::SetSplineOwnerQuery({});
    settings.SetAutoConnect(autoConnectBefore);
}

// Picking sizes a knot's hit radius with PickScreenScale and drawing sizes the
// marker with ComputeSplineScreenScale; from the same eye at the same distance
// the two must agree, or the pick area drifts from the drawn circle.
TEST(SplineScreenScaleTests, PickAndDrawScaleAgreeFromTheSameEye)
{
    const Vector3 camera(0.0f, 2.0f, -10.0f);
    GizmoRenderContext context(ViewId{31}, CameraId{31}, &camera);
    GizmoRay ray;
    ray.origin = camera;
    const Vector3 knot(1.0f, 0.0f, 3.0f);

    for (const bool smartDistance : {false, true})
    {
        const float drawScale = ComputeSplineScreenScale(context, knot, true, smartDistance);
        EXPECT_NE(drawScale, 1.0f) << "smartDistance " << smartDistance;
        EXPECT_FLOAT_EQ(PickScreenScale(ray, knot, true, smartDistance), drawScale)
            << "smartDistance " << smartDistance;
    }
}

TEST(ColorUtilsTests, UnpackArgbReadsArgbChannelForChannel)
{
    const GameEngine::Color color = GameEngine::ColorUtils::UnpackArgb(0x80FF4000u);
    EXPECT_FLOAT_EQ(color.r, 1.0f);
    EXPECT_FLOAT_EQ(color.g, 64.0f / 255.0f);
    EXPECT_FLOAT_EQ(color.b, 0.0f);
    EXPECT_FLOAT_EQ(color.a, 128.0f / 255.0f);
}

} // namespace


TEST(SplineInteractionStateTests, CollidingHandlesKeepSelectionAndHoverInTheirWorld)
{
    World first;
    World second;
    const auto a = first.Create().GetHandle();
    const auto b = second.Create().GetHandle();
    ASSERT_EQ(a, b);
    const auto stateA = AcquireSplineInteractionState(first);
    const auto stateB = AcquireSplineInteractionState(second);
    stateA->Selection().Entity = a;
    stateA->Selection().PointIndex = 3;
    stateA->Hover().Entity = a;
    stateA->Hover().PointIndex = 2;
    EXPECT_FALSE(stateB->Selection().Entity.IsValid());
    EXPECT_FALSE(stateB->Hover().Entity.IsValid());
    stateB->Selection().Entity = b;
    stateB->Selection().PointIndex = 7;
    EXPECT_EQ(stateA->Selection().PointIndex, 3);
    EXPECT_EQ(AcquireSplineInteractionState(first), stateA);
    first.Clear();
    EXPECT_FALSE(stateA->Selection().Entity.IsValid());
    EXPECT_FALSE(stateA->Hover().Entity.IsValid());
    EXPECT_EQ(stateB->Selection().PointIndex, 7);
}

TEST(SplineInteractionStateTests, IndexDoesNotOwnTheStateAfterItsConsumersClose)
{
    World world;
    std::weak_ptr<SplineInteractionState> weak;
    {
        const auto state = AcquireSplineInteractionState(world);
        state->Selection().PointIndex = 4;
        weak = state;
    }
    EXPECT_TRUE(weak.expired());
    EXPECT_EQ(AcquireSplineInteractionState(world)->Selection().PointIndex, -1);
}
