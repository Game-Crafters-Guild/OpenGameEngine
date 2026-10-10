#include "SceneViewController.h"
#include "SceneView/SceneViewToolStripRegistry.h"
#include "SceneView/SplineTool.h"
#include "Components/Rendering/ReflectionProbe.h"
#include "Core/CpuProfiler.h"
#include "Core/Engine.h"
#include "Engine/GameUI/GameUIHost.h" // complete type for m_GameUI's unique_ptr dtor
#include "ECS/ECSTemplates.h"
#include "Engine/Rendering/ExposureReadbackFeature.h"
#include "Engine/Rendering/PlanetCameraFraming.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/ViewReadbackUtils.h"
#include "Scene/SceneTlas.h"
#include "Mathematics/Geometry.h"
#include "Mathematics/MatrixOps.h"
#include "Mathematics/VectorOps.h"
#include "Rendering/CameraTypes.h"
#include "SceneView/SceneViewCameraRig.h"
#include "SceneView/SceneViewPostProcessSwitches.h"
#include "Rendering/Common/Math.h"
#include "Rendering/Common/Utils.h"
#include "Engine/Rendering/Pipeline/PipelineFrameResources.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Core/HzbCullingStrategy.h"
#include "Rendering/Core/NoneCullingStrategy.h"
#include "Rendering/Core/PipelineDescTranslator.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"

#include <algorithm>
#include <utility>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <vector>

#include "Components/Rendering/Camera.h"
#include "Components/Rendering/PostProcessVolume.h"
#include "Components/Rendering/LocalBounds.h"
#include "Components/Rendering/Light.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Rendering/TonemapMode.h"
#include "Components/Measure/MeasureComponent.h"
#include "Components/Hierarchy.h"
#include "Components/HierarchyQueries.h"
#include "ECS/Components.h"
#include "Components/Transform.h"
#include "Components/TransformDirtyFeed.h"
#include "Engine/Rendering/Camera.h"
#include "Engine/Rendering/CameraAspectRatio.h"
#include "Engine/Rendering/CameraExposureHelpers.h"
#include "Engine/Rendering/Exposure.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "ECSModules/Rendering/SkeletonStore.h"
#include "Components/Animation/SkeletonRef.h"
#include "Rendering/Geometry/VertexAttributeFlags.h"
#include "Rendering/Geometry/VertexLayoutBuilder.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Logger/Logger.h"
#include "Picking/MeshPickingService.h"
#include "Picking/PickRootResolver.h"
#include "UI/Controls/Widgets/CameraBookmarksWidget.h"
#include "Core/Application.h" // PathUtils::GetExecutableDirectory
#include "Editor/Settings/SceneViewSettings.h"
#include "SceneView/SceneViewFraming.h"
#include "SceneView/SceneViewOverlayShared.h"
#include "TerrainECS/TerrainService.h"

namespace
{
inline GameEngine::Rendering::GraphicsPipelineId InternEditorGraphicsId(
    GameEngine::Rendering::IDevice& device, const GameEngine::Rendering::PipelineDesc& base)
{
    return GameEngine::Rendering::PipelineDescTranslator::InternGraphics(device, base);
}

inline GameEngine::Rendering::PipelineFormatKey EditorFormatKey(const GameEngine::Rendering::PipelineDesc& base)
{
    return GameEngine::Rendering::PipelineDescTranslator::BuildFormatKey(base);
}

std::shared_ptr<GameEngine::Rendering::ICullingStrategy> FixedOrthographicSceneViewCullingStrategy()
{
    static std::shared_ptr<GameEngine::Rendering::ICullingStrategy> s_Strategy =
        std::make_shared<GameEngine::Rendering::NoneCullingStrategy>();
    return s_Strategy;
}

bool GetEntityWorldMatrixForMeasure(GameEngine::ECS::World* world,
                                    GameEngine::ECS::EntityHandle entity,
                                    GameEngine::Mathematics::Matrix4x4& out,
                                    int depth = 0);

bool GetEntityPositionForMeasure(GameEngine::ECS::World* world,
                                 GameEngine::ECS::EntityHandle entity,
                                 GameEngine::Mathematics::Vector3& out)
{
    if (!world || !entity.IsValid() || !world->IsValid(entity))
        return false;

    GameEngine::Mathematics::Matrix4x4 worldMatrix;
    if (GetEntityWorldMatrixForMeasure(world, entity, worldMatrix))
    {
        out = GameEngine::Mathematics::Vector3(worldMatrix[3].x, worldMatrix[3].y, worldMatrix[3].z);
        return true;
    }

    return false;
}

bool GetEntityWorldMatrixForMeasure(GameEngine::ECS::World* world,
                                    GameEngine::ECS::EntityHandle entity,
                                    GameEngine::Mathematics::Matrix4x4& out,
                                    int depth)
{
    if (!world || !entity.IsValid() || !world->IsValid(entity) || depth > 64)
        return false;

    if (const auto* transform = world->GetComponent<GameEngine::Components::Transform>(entity))
    {
        GameEngine::Mathematics::Matrix4x4 local = GameEngine::Mathematics::Matrix4x4::FromColumnMajor(transform->matrix);
        if (const auto* parent = world->GetComponent<GameEngine::Components::Parent>(entity);
            parent && parent->parent.IsValid())
        {
            GameEngine::Mathematics::Matrix4x4 parentWorld;
            if (GetEntityWorldMatrixForMeasure(world, parent->parent, parentWorld, depth + 1))
            {
                out = parentWorld * local;
                return true;
            }
        }

        out = local;
        return true;
    }

    if (const auto* worldTransform = world->GetComponent<GameEngine::Components::WorldTransform>(entity))
    {
        out = GameEngine::Mathematics::Matrix4x4::FromColumnMajor(worldTransform->matrix);
        return true;
    }

    return false;
}

void SetEntityPositionForMeasure(GameEngine::ECS::World* world,
                                 GameEngine::ECS::EntityHandle entity,
                                 const GameEngine::Mathematics::Vector3& point)
{
    if (!world || !entity.IsValid() || !world->IsValid(entity))
        return;

    if (auto* transform = world->GetComponentForWrite<GameEngine::Components::Transform>(entity))
    {
        GameEngine::Mathematics::Vector3 localPoint = point;
        if (const auto* parent = world->GetComponent<GameEngine::Components::Parent>(entity);
            parent && parent->parent.IsValid())
        {
            GameEngine::Mathematics::Matrix4x4 parentWorld;
            if (GetEntityWorldMatrixForMeasure(world, parent->parent, parentWorld))
            {
                const GameEngine::Mathematics::Matrix4x4 invParent =
                    GameEngine::Mathematics::Inverse(parentWorld);
                localPoint = invParent.TransformPoint(point);
            }
        }

        transform->matrix[12] = localPoint.x;
        transform->matrix[13] = localPoint.y;
        transform->matrix[14] = localPoint.z;
    }

    if (auto* worldTransform = world->GetComponentForWrite<GameEngine::Components::WorldTransform>(entity))
    {
        worldTransform->matrix[12] = point.x;
        worldTransform->matrix[13] = point.y;
        worldTransform->matrix[14] = point.z;
        GameEngine::Components::BumpWorldTransform(*world, entity, *worldTransform);
    }
}

GameEngine::ECS::EntityHandle ResolveMeasureEndpointEntity(
    GameEngine::ECS::World* world,
    GameEngine::ECS::EntityHandle measureEntity,
    const GameEngine::Components::MeasureComponent& measure,
    std::uint8_t endpoint)
{
    (void)measureEntity;
    if (!world)
        return {};

    const GameEngine::ECS::EntityHandle stored = endpoint == 1u ? measure.StartEntity : measure.EndEntity;
    if (stored.IsValid() && world->IsValid(stored))
        return stored;

    return {};
}

void ClearStaleSplineSelection(GameEngine::Editor::SceneTools::SplineInteractionState& state, const std::vector<GameEngine::ECS::EntityHandle>& selection,
                               bool clearControlsForEntitySelection = false)
{
    auto& splineSelection = state.Selection();
    splineSelection.Controls.erase(
        std::remove_if(splineSelection.Controls.begin(), splineSelection.Controls.end(),
                       [&](const GameEngine::Editor::SceneTools::SplineControlSelection& c) {
                           return std::find(selection.begin(), selection.end(), c.Entity) == selection.end();
                       }),
        splineSelection.Controls.end());

    if (clearControlsForEntitySelection &&
        (splineSelection.Entity.IsValid() || !splineSelection.Controls.empty()))
    {
        splineSelection = GameEngine::Editor::SceneTools::SplineSelection{};
        auto& hover = state.Hover();
        hover = GameEngine::Editor::SceneTools::SplineHover{};
        return;
    }

    if (!splineSelection.Entity.IsValid() && splineSelection.Controls.empty())
        return;

    if (std::find(selection.begin(), selection.end(), splineSelection.Entity) != selection.end())
        return;

    if (!splineSelection.Controls.empty())
    {
        const auto& active = splineSelection.Controls.back();
        splineSelection.Entity = active.Entity;
        splineSelection.Kind = active.Kind;
        splineSelection.PointIndex = active.PointIndex;
        splineSelection.KnotIndices.clear();
        for (const auto& c : splineSelection.Controls)
        {
            if (c.Entity == active.Entity && c.Kind == 1)
                splineSelection.KnotIndices.push_back(c.PointIndex);
        }
        return;
    }

    splineSelection = GameEngine::Editor::SceneTools::SplineSelection{};

    auto& hover = state.Hover();
    hover = GameEngine::Editor::SceneTools::SplineHover{};
}

inline void GetSceneViewClipPlanes(GameEngine::ECS::World* world, float& nearClip, float& farClip)
{
    nearClip = 0.1f;
    farClip = 200.0f;

    using GameEngine::Editor::SceneViewSettings;

    SceneViewSettings& settings = SceneViewSettings::Get();
    nearClip = settings.GetNearClip();
    farClip = settings.GetFarClip();

    constexpr float kMinNear = 0.001f;
    if (nearClip < kMinNear)
        nearClip = kMinNear;

    // Ensure far is reasonably in front of near to avoid precision issues.
    constexpr float kMinFarDelta = 1.0f;
    if (farClip <= nearClip + kMinFarDelta)
        farClip = nearClip + kMinFarDelta;

    // An enabled spherical (planet) terrain auto-extends the far plane to fit its
    // bounding sphere, never below the user's setting — the #1 "my planet vanished"
    // cause. Cheap: one terrain query, typically a single entity.
    if (world)
        farClip = GameEngine::Engine::Renderer::ExpandFarClipForSphericalTerrain(*world, farClip);
}

// Minimum camera–pivot distance for orbit / dolly. Matches the sanitized near
// plane so zoom limits stay consistent with Scene View projection settings.
inline float GetSceneViewMinOrbitDistance(GameEngine::ECS::World* world)
{
    float nearClip = 0.01f;
    float farUnused = 1000.0f;
    GetSceneViewClipPlanes(world, nearClip, farUnused);
    return nearClip;
}

inline float GetSceneViewFieldOfViewDeg()
{
    float fovDeg = 60.0f;

    using GameEngine::Editor::SceneViewSettings;

    SceneViewSettings& settings = SceneViewSettings::Get();
    fovDeg = settings.GetFieldOfViewDeg();

    if (!(fovDeg > 0.0f))
        fovDeg = 60.0f;
    if (fovDeg < 20.0f)
        fovDeg = 20.0f;
    if (fovDeg > 120.0f)
        fovDeg = 120.0f;

    return fovDeg;
}

GameEngine::Engine::Renderer::ViewRegistry::CameraExposure SceneViewLensExposure(GameEngine::ECS::World* world)
{
    using namespace GameEngine;
    using Engine::Renderer::ViewRegistry;

    // Scene View owns its exposure controls, but physical DoF still needs an
    // authorable lens. Mirror the active scene camera's lens while deriving
    // focal length from Scene View's own projection so changing Focus Distance
    // in the Camera inspector is visible in both Scene and Game views.
    ViewRegistry::CameraExposure result{};
    if (!world)
        return result;
    const auto camera = Engine::Renderer::FindActiveCamera(*world);
    if (!camera)
        return result;

    Components::Camera lens = camera->params;
    lens.ExposureControl = Components::ExposureMode::Auto;
    lens.Exposure = 1.0f;
    lens.ManualExposureEV = Components::kDefaultManualExposureEv;
    lens.ExposureCompensation = 0.0f;
    lens.AutoExposureMinEv = 4.0f;
    lens.AutoExposureMaxEv = 18.0f;
    lens.AutoExposureSpeedUp = 1.0f;
    lens.AutoExposureSpeedDown = 3.0f;
    lens.Perspective = true;
    lens.FovY = GetSceneViewFieldOfViewDeg();
    return Engine::Renderer::ToCameraExposure(lens);
}

inline float GetSceneViewMoveSpeed()
{
    using GameEngine::Editor::SceneViewSettings;
    return SceneViewSettings::Get().GetMoveSpeed();
}

inline float GetSceneViewFastMoveMultiplier()
{
    using GameEngine::Editor::SceneViewSettings;
    return SceneViewSettings::Get().GetFastMoveMultiplier();
}

inline float GetSceneViewMoveAccelerationTime()
{
    using GameEngine::Editor::SceneViewSettings;
    return SceneViewSettings::Get().GetMoveAccelerationTime();
}

} // namespace

namespace GameEngine
{

bool SceneViewController::IsEntityPickable(ECS::EntityHandle entity) const
{
    if (!m_IsEntityPickable)
        return true;
    if (!m_IsEntityPickable(entity))
        return false;
    // Also reject if any ancestor is locked — submeshes of a locked parent
    // should be non-selectable even though they aren't locked themselves.
    auto* world = &GetWorld();
    using GameEngine::Components::Parent;
    int safety = 64;
    const Parent* p = world->GetComponent<Parent>(entity);
    while (p && p->parent.IsValid() && safety-- > 0)
    {
        if (!m_IsEntityPickable(p->parent))
            return false;
        p = world->GetComponent<Parent>(p->parent);
    }
    return true;
}
using namespace Rendering;
using Rendering::Matrix4x4;
using Rendering::Math::ToRadians;

namespace Editor
{
namespace SceneTools
{

class HitPointGizmo : public IGizmo
{
  public:
    void SetPosition(const Mathematics::Vector3& position)
    {
        m_Valid = true;
        m_Position = position;
    }

    void SetDebugRay(const GizmoRay& ray, bool hasHit, const Mathematics::Vector3& hitPosition)
    {
        m_HasRay = true;
        m_RayOrigin = ray.origin;
        m_RayDirection = ray.direction;

        if (hasHit)
        {
            SetPosition(hitPosition);
        }
        else
        {
            m_Valid = false;
        }
    }

    void Clear()
    {
        m_Valid = false;
        m_HasRay = false;
    }

    void Render(GizmoRenderContext& context) override
    {
        using Mathematics::Vector3;

        // Always render the last ray (if any) as a long debug line so we can
        // visually confirm origin/direction even when we miss the plane.
        if (m_HasRay)
        {
            // Long ray from the camera origin to make direction obvious.
            constexpr float kRayLength = 200.0f;
            const Color rayColor(1.0f, 1.0f, 0.0f, 1.0f); // yellow

            const Vector3 end = m_RayOrigin + m_RayDirection * kRayLength;
            // Slightly thicker so it stands out from grid lines.
            context.DrawColoredLine(m_RayOrigin, end, rayColor, 2.0f);
        }

        if (!m_Valid)
        {
            return;
        }

        // Draw a small, clearly visible cross slightly above the hit point so it
        // does not z-fight with the ground grid. Size is in world units.
        constexpr float kHalf = 0.5f;     // half-extent of the cross arms
        constexpr float kYOffset = 0.02f; // lift cross slightly above the plane

        const Color xColor(1.0f, 0.3f, 0.3f, 1.0f);
        const Color yColor(0.3f, 1.0f, 0.3f, 1.0f);
        const Color zColor(0.3f, 0.3f, 1.0f, 1.0f);

        const Vector3 crossCenter(m_Position.x, m_Position.y + kYOffset, m_Position.z);

        // X axis segment
        Vector3 a(crossCenter.x - kHalf, crossCenter.y, crossCenter.z);
        Vector3 b(crossCenter.x + kHalf, crossCenter.y, crossCenter.z);
        context.DrawColoredLine(a, b, xColor, 2.0f);

        // Y axis segment (vertical)
        a = Vector3(crossCenter.x, crossCenter.y - kHalf * 0.5f, crossCenter.z);
        b = Vector3(crossCenter.x, crossCenter.y + kHalf * 0.5f, crossCenter.z);
        context.DrawColoredLine(a, b, yColor, 2.0f);

        // Z axis segment
        a = Vector3(crossCenter.x, crossCenter.y, crossCenter.z - kHalf);
        b = Vector3(crossCenter.x, crossCenter.y, crossCenter.z + kHalf);
        context.DrawColoredLine(a, b, zColor, 2.0f);

        // Additionally, draw a short magenta segment centered on the hit
        // point and aligned with the ray direction. This makes it very
        // obvious where the ray pierces the plane, even if the long
        // camera-space ray is hard to see against scene geometry.
        if (m_HasRay)
        {
            constexpr float kLocalRayLen = 1.5f;
            const Color localColor(1.0f, 0.0f, 1.0f, 1.0f); // magenta

            // Place the local ray exactly on the plane (y = m_Position.y)
            // while the cross is slightly above. This avoids them perfectly
            // overlapping when the ray direction is close to +Z, so the blue
            // cross arm should remain visible.
            const Vector3 aLocal = m_Position - m_RayDirection * kLocalRayLen;
            const Vector3 bLocal = m_Position + m_RayDirection * kLocalRayLen;

            context.DrawColoredLine(aLocal, bLocal, localColor, 2.0f);
        }
    }

  private:
    bool m_Valid = false;
    bool m_HasRay = false;
    Mathematics::Vector3 m_Position{0.0f, 0.0f, 0.0f};
    Mathematics::Vector3 m_RayOrigin{0.0f, 0.0f, 0.0f};
    Mathematics::Vector3 m_RayDirection{0.0f, 0.0f, 1.0f};
};

// NOTE: Selection gizmo and selection tool implementations now live in
// Editor/SceneView/SelectionTool.{h,cpp} so they can be shared and kept
// independent of SceneViewController. See SelectionGizmo and SelectionTool
// in the Editor::SceneTools namespace.

class DebugHitTool : public ISceneTool
{
  public:
    const char* GetName() const override { return "Debug Hit"; }

    void OnActivated() override
    {
        m_HasHit = false;
        m_HitGizmo.Clear();
    }

    void OnDeactivated() override {}

    void OnPointerEvent(const ScenePointerEvent& event) override
    {
        if (event.button != PointerButton::Left || event.phase != PointerPhase::Down)
        {
            return;
        }

        // Always record the ray so we can visualize it even if we miss the plane.
        m_HasHit = false;
        m_HitGizmo.SetDebugRay(event.ray, /*hasHit*/ false, Mathematics::Vector3());

        Mathematics::Plane plane;
        plane.normal = Mathematics::Vector3(0.0f, 1.0f, 0.0f);
        plane.d = 0.0f; // y = 0 plane

        float t = 0.0f;
        Mathematics::Vector3 hitPoint{};
        if (Mathematics::IntersectRayPlane(event.ray, plane, t, hitPoint))
        {
            m_HasHit = true;
            m_HitPoint = hitPoint;
            m_HitGizmo.SetDebugRay(event.ray, /*hasHit*/ true, hitPoint);

            Logger::Log::Info(
                "DebugHitTool: plane hit t={} pos=({}, {}, {}), rayOrigin=({}, {}, {}), rayDir=({}, {}, {})",
                t,
                hitPoint.x, hitPoint.y, hitPoint.z,
                event.ray.origin.x, event.ray.origin.y, event.ray.origin.z,
                event.ray.direction.x, event.ray.direction.y, event.ray.direction.z);
        }
        else
        {
            Logger::Log::Info(
                "DebugHitTool: ray missed y=0 plane, rayOrigin=({}, {}, {}), rayDir=({}, {}, {})",
                event.ray.origin.x, event.ray.origin.y, event.ray.origin.z,
                event.ray.direction.x, event.ray.direction.y, event.ray.direction.z);
        }
    }

    void OnKeyEvent(const SceneKeyEvent& /*event*/) override
    {
        // No keyboard behaviour for the debug hit tool yet.
    }

    void GatherGizmos(GizmoCollector& collector) override
    {
        // Always register the hit gizmo; it internally decides whether there
        // is anything to draw (ray and/or hit point). This guarantees that
        // the debug ray is visible even when the plane intersection fails.
        collector.AddGizmo(&m_HitGizmo);
    }

  private:
    bool m_HasHit = false;
    Mathematics::Vector3 m_HitPoint{0.0f, 0.0f, 0.0f};
    HitPointGizmo m_HitGizmo;
};

} // namespace SceneTools
} // namespace Editor

std::optional<Engine::Renderer::CameraAspectResolution> ResolveEditorSceneViewCameraAspect(
    ECS::World* world,
    uint32_t viewportWidth,
    uint32_t viewportHeight,
    bool orthographicView)
{
    if (!orthographicView)
        return std::nullopt;

    if (!world)
        return std::nullopt;

    const std::optional<Engine::Renderer::Camera> activeCamera =
        Engine::Renderer::FindActiveCamera(*world);
    if (!activeCamera)
        return std::nullopt;

    if (Engine::Renderer::GetCameraAspectPreset(activeCamera->params)
        == Engine::Renderer::CameraAspectPreset::Native)
    {
        return std::nullopt;
    }

    return Engine::Renderer::ResolveCameraAspect(
        activeCamera->params, viewportWidth, viewportHeight);
}

uint32_t SceneViewEffectiveViewportHeight(ECS::World* world, uint32_t viewportWidth,
                                          uint32_t viewportHeight,
                                          bool orthographicView)
{
    if (const auto sceneAspect =
            ResolveEditorSceneViewCameraAspect(world, viewportWidth, viewportHeight, orthographicView))
    {
        if (sceneAspect->letterbox.active)
            return sceneAspect->letterbox.height;
    }
    return viewportHeight;
}

using GameEngine::Mathematics::Cross3;
using GameEngine::Mathematics::Normalize3;

SceneViewController::SceneViewController(Engine::Renderer::RenderServices* renderServices, ECS::World& world)
    : m_RenderServices(renderServices), m_World(&world),
      m_MeasureSceneGizmo(world), m_SplineSceneGizmo(world)
{
    using namespace Editor::SceneTools;

    m_SelectionTool = std::make_unique<SelectionTool>(*this);
    m_TransformTool = std::make_unique<TransformTool>(world);
    m_NavGridBrushTool = std::make_unique<NavGridBrushTool>(*this);
    m_SplineSceneGizmo.SetSelectionQuery([this]() -> GameEngine::ECS::EntityHandle {
        return m_SelectedEntities.empty() ? GameEngine::ECS::EntityHandle{} : m_SelectedEntities.front();
    });
    m_SplineSceneGizmo.SetIsEntitySelectedQuery([this](GameEngine::ECS::EntityHandle entity) -> bool {
        return std::find(m_SelectedEntities.begin(), m_SelectedEntities.end(), entity) != m_SelectedEntities.end();
    });
    m_MeasureSceneGizmo.SetEndpointPickedCallback(
        [this](GameEngine::ECS::EntityHandle entity, std::uint8_t endpoint)
        {
            GameEngine::ECS::EntityHandle selectable = entity;
            {
                GameEngine::ECS::World* world = &GetWorld();
                if (const auto* measure = world->GetComponent<Components::MeasureComponent>(entity))
                {
                    const GameEngine::ECS::EntityHandle endpointEntity =
                        ResolveMeasureEndpointEntity(world, entity, *measure, endpoint);
                    if (endpointEntity.IsValid())
                        selectable = endpointEntity;
                }
            }
            OnEntityPicked(selectable, false, false, true);
            m_MeasureSceneGizmo.Selection() = {entity, endpoint};
            if (m_TransformTool)
                m_TransformTool->SetMode(Editor::SceneTools::TransformMode::Translate);
            SetActiveTool(ToolKind::Transform);
        });
    m_MeasureSceneGizmo.SetEndpointDragStartedCallback(
        [this](GameEngine::ECS::EntityHandle entity, std::uint8_t endpoint)
        {
            m_MeasureSceneGizmo.Selection() = {entity, endpoint};
            if (m_TransformTool)
                m_TransformTool->SetMode(Editor::SceneTools::TransformMode::Translate);
            SetActiveTool(ToolKind::Transform);
            BeginMeasureEndpointTransformEdit("Move Measure Point");
        });
    m_MeasureSceneGizmo.SetEndpointDraggedCallback(
        [this](const Mathematics::Vector3& deltaWorld)
        {
            ApplyMeasureEndpointTransformDelta(deltaWorld);
        });
    m_MeasureSceneGizmo.SetEndpointDragEndedCallback(
        [this]()
        {
            const Editor::SceneTools::MeasureEndpointSelection selected =
                m_MeasureSceneGizmo.Selection();
            CommitMeasureEndpointTransformEdit();
            if (selected.IsValid())
            {
                GameEngine::ECS::EntityHandle selectable = selected.Entity;
                {
                    GameEngine::ECS::World* world = &GetWorld();
                    if (const auto* measure = world->GetComponent<Components::MeasureComponent>(selected.Entity))
                    {
                        const GameEngine::ECS::EntityHandle endpointEntity =
                            ResolveMeasureEndpointEntity(world, selected.Entity, *measure, selected.Endpoint);
                        if (endpointEntity.IsValid())
                            selectable = endpointEntity;
                    }
                }
                OnEntityPicked(selectable, false, false, true);
                if (m_TransformTool)
                    m_TransformTool->SetMode(Editor::SceneTools::TransformMode::Translate);
                SetActiveTool(ToolKind::Transform);
            }
        });
    m_ToolContext.AddGlobalGizmo(&m_MeasureSceneGizmo);

    // TransformTool delegates entity picking to the shared click-through path
    // so root/exact/cycle and icon behavior match SelectionTool exactly.
    m_TransformTool->SetScenePickDelegate(
        [this](const Editor::SceneTools::ScenePointerEvent& event, bool clearOnMiss)
        {
            return PickViaClickThrough(event, clearOnMiss);
        });

    // Wire up marquee-selection callback so drag-select commits apply to the
    // shared selection set and propagate to inspector / hierarchy.
    m_TransformTool->SetOnEntitiesMarqueeCallback(
        [this](const std::vector<GameEngine::ECS::EntityHandle>& entities, bool additive)
        {
            OnEntitiesMarqueeSelected(entities, additive);
        });
    m_TransformTool->SetIsEntityPickable([this](GameEngine::ECS::EntityHandle e)
        {
            return IsEntityPickable(e);
        });

    m_ActiveToolKind = ToolKind::Selection;
    m_ToolContext.SetActiveTool(m_SelectionTool.get());

    m_ShowGrid = Editor::SceneViewSettings::Get().GetGridVisibleOnStartup();
    m_ShowCameraFrameGuide = Editor::SceneViewSettings::Get().GetCameraFrameGuide();
    m_PostProcessingEnabled = Editor::SceneViewSettings::Get().GetPostProcessingEnabled();
}

void SceneViewController::SetCameraFrameGuide(bool v)
{
    if (m_ShowCameraFrameGuide == v)
        return;
    m_ShowCameraFrameGuide = v;
    Editor::SceneViewSettings::Get().SetCameraFrameGuide(v);
}

void SceneViewController::TogglePostProcessing()
{
    SetPostProcessingEnabled(!m_PostProcessingEnabled);
}

void SceneViewController::SetPostProcessingEnabled(bool enabled)
{
    if (m_PostProcessingEnabled == enabled)
        return;
    m_PostProcessingEnabled = enabled;
    Editor::SceneViewSettings::Get().SetPostProcessingEnabled(enabled);
}

SceneViewController::~SceneViewController()
{
    Rendering::IDevice* device = m_RenderServices ? m_RenderServices->GetDevice() : nullptr;
    m_PreviewSnapshot.Destroy(device);
    m_PresentationSnapshot.Destroy(device);

    DeactivateRenderView();

    if (m_ChangeNotifications && m_ChangeSub)
    {
        m_ChangeNotifications->Unsubscribe(m_ChangeSub);
    }
    m_ChangeSub = {};
    m_ChangeNotifications = nullptr;
}

bool SceneViewController::UpdatePresentedSnapshotRG(Rendering::RenderGraph::RGFrame& frame,
                                                    Rendering::RenderGraph::RGTexture source,
                                                    UI::UITextureSpace sourceSpace)
{
    auto* device = m_RenderServices ? m_RenderServices->GetDevice() : nullptr;
    const std::string name = "Editor.SceneView.PresentationSnapshot." + std::to_string(m_ViewId);
    return device && m_PresentationSnapshot.Update(frame, *device, source, sourceSpace, name.c_str());
}

void SceneViewController::SetFixedViewOrientation(FixedViewOrientation orientation)
{
    m_FixedViewOrientation = orientation;
    if (orientation == FixedViewOrientation::Free)
        return;

    m_Is2DMode = false;
    m_Orthographic = true;
    m_CamDistance = std::max(m_CamDistance, 10.0f);

    switch (orientation)
    {
    case FixedViewOrientation::Top:
        m_CamPos[0] = 0.0f;
        m_CamPos[1] = 10.0f;
        m_CamPos[2] = 0.0f;
        m_CamYawDeg = 90.0f;
        m_CamPitchDeg = -89.0f;
        break;
    case FixedViewOrientation::Front:
        m_CamPos[0] = 0.0f;
        m_CamPos[1] = 0.0f;
        m_CamPos[2] = -10.0f;
        m_CamYawDeg = 90.0f;
        m_CamPitchDeg = 0.0f;
        break;
    case FixedViewOrientation::Side:
        m_CamPos[0] = -10.0f;
        m_CamPos[1] = 0.0f;
        m_CamPos[2] = 0.0f;
        m_CamYawDeg = 0.0f;
        m_CamPitchDeg = 0.0f;
        break;
    case FixedViewOrientation::Free:
        break;
    }
}

void SceneViewController::DeactivateRenderView()
{
    // Release any registered RenderServices views/cameras to avoid leaking
    // per-view cached passes/resources across controller lifetimes.
    if (m_RenderServices)
    {
        if (m_ViewId != 0)
        {
            // Release the exposure readback with the view — the registration
            // is sticky and would otherwise keep the ring alive for the app's
            // lifetime if this view died while auto exposure was metering.
            if (auto* exposureReadback =
                    m_RenderServices->GetFeature<Engine::Renderer::ExposureReadbackFeature>())
                exposureReadback->SetReadbackEnabled(m_ViewId, false);
            m_RenderServices->Views().ReleaseView(m_ViewId);
            m_ViewId = 0;
        }
        if (m_PreviewViewId != 0)
        {
            m_RenderServices->Views().ReleaseView(m_PreviewViewId);
            m_PreviewViewId = 0;
        }
        // Zero BOTH serials: leaving pending > submitted with the view gone
        // would leave a request nothing can ever close.
        m_PreviewLastSubmittedSerial = 0;
        m_PreviewPendingSerial = 0;
        m_PreviewSnapshotPending = false;

        // Release cameras last (they may be referenced by views).
        if (m_CameraId != 0)
        {
            m_RenderServices->Views().ReleaseCamera(m_CameraId);
            m_CameraId = 0;
        }
        if (m_PreviewCameraId != 0)
        {
            m_RenderServices->Views().ReleaseCamera(m_PreviewCameraId);
            m_PreviewCameraId = 0;
        }
    }
}

void SceneViewController::SetOnSelectEntityCallback(OnSelectEntityCallback callback)
{
    m_OnSelectEntityCallback = std::move(callback);
}

void SceneViewController::SetOnSelectEntitiesCallback(OnSelectEntitiesCallback callback)
{
    m_OnSelectEntitiesCallback = std::move(callback);
}

void SceneViewController::SetOnActiveToolChangedCallback(OnActiveToolChangedCallback callback)
{
    m_OnActiveToolChangedCallback = std::move(callback);
}

void SceneViewController::SetChangeNotifications(Editor::EditorChangeNotifications* notifications)
{
    if (m_ChangeNotifications && m_ChangeSub)
    {
        m_ChangeNotifications->Unsubscribe(m_ChangeSub);
    }

    m_ChangeNotifications = notifications;
    m_ChangeSub = {};

    if (!m_ChangeNotifications)
    {
        return;
    }

    const ECS::ComponentTypeId transformType = ECS::GetComponentTypeId<Components::Transform>();
    m_ChangeSub = m_ChangeNotifications->SubscribeComponentChanged(
        [this, transformType](const Editor::EditorChangeNotifications::ComponentChangedEvent& e)
        {
            if (!m_TransformTool)
            {
                return;
            }

            if (e.componentType != transformType)
            {
                return;
            }

            SyncMeasureFromEndpointEntityTransform(e.entity, e.kind);

            // Only refresh the active gizmo target (keeps work bounded for preview spam).
            if (e.entity != m_TransformTool->GetTargetEntity())
            {
                return;
            }

            // PERF: During an active gizmo drag the TransformTool already keeps its pivot
            // in sync. Avoid re-reading ECS hierarchy every preview tick.
            if (e.kind == Editor::EditorChangeNotifications::ChangeKind::Preview &&
                m_TransformTool->IsInteractiveEditActive())
            {
                return;
            }

            m_TransformTool->RefreshPivotFromTargetEntity();
        });
}

std::uint64_t SceneViewController::RequestBookmarkPreview(const SceneViewCameraPose& pose)
{
    m_PreviewRequestSerial++;
    m_PreviewPendingSerial = m_PreviewRequestSerial;
    m_PreviewNoDrawRetryFrames = 0;
    m_PreviewHadDrawItemsThisFrame = false;
    m_RequestPreview = true;
    m_PreviewPose = pose;
    // Targets always clear per declared frame (DeclarePreviewTargetsRG) —
    // the popup is gated on the serial match so stale content from a
    // previous bookmark is never shown.
    // Keep preview active until the serial is matched (geometry rendered).
    // No fixed frame count — extraction runs before Record(), so the first
    // frame always has zero draw items. The preview stays enabled until the
    // post-pipeline check confirms real geometry rendered.
    m_PreviewPendingFreeze = false;

    // Ensure the preview camera/view exist *before* the next render extraction pass.
    // This avoids "self-starving" where we request a preview, but extraction can't
    // submit anything because the view isn't registered yet.
    if (!m_RenderServices)
    {
        return m_PreviewRequestSerial;
    }

    if (m_PreviewCameraId == 0)
    {
        m_PreviewCameraId = m_RenderServices->Views().AllocateCamera("SceneView.BookmarkPreview.Camera");
    }
    if (m_PreviewViewId == 0)
    {
        // OnDemand: the preview view is fed only on frames we arm it via
        // RequestViewFrame (the arm path in DeclarePreviewTargetsRG). Once the
        // captured frame is frozen and we stop arming, the view lapses to dormant
        // on its own — no per-frame culling / extraction / shadow-dispatch work
        // for a preview that isn't currently rendering (the #256 / #272 pattern).
        // An Always view keeps doing that work every frame until manually masked
        // off, which is exactly the hand-rolled disarm this replaces.
        m_PreviewViewId = m_RenderServices->Views().AllocateView(
            "SceneView.BookmarkPreview", m_PreviewCameraId, Rendering::ViewPurpose::EditorPreview,
            Rendering::ViewParticipation::OnDemand);
    }
    else
    {
        m_RenderServices->Views().SetViewCamera(m_PreviewViewId, m_PreviewCameraId);
    }

    // Set world ID for the preview view.
    if (m_PreviewViewId != 0)
        m_RenderServices->Views().SetViewWorldId(m_PreviewViewId, GetWorld().GetWorldId());
    return m_PreviewRequestSerial;
}

bool SceneViewController::FreezePreviewViewIfPending(Engine::Renderer::RenderServices* rs)
{
    // Consume the freeze flag once the captured frame's serial is matched. The
    // popup shows the standalone snapshot texture (FinalizePreviewRG), so the
    // preview view no longer needs to render: DeclarePreviewTargetsRG stops
    // arming it (early-returns once the serial closes), and the OnDemand view
    // lapses to dormant on its own — ending its per-frame culling / extraction /
    // shadow-dispatch work without releasing the view. Release the camera so the
    // dormant view is headless.
    if (!rs || m_PreviewViewId == 0 || !m_PreviewPendingFreeze ||
        m_PreviewLastSubmittedSerial < m_PreviewPendingSerial)
        return false;

    m_PreviewPendingFreeze = false;
    m_PreviewSubmittedThisFrame = false;
    rs->Views().SetViewCamera(m_PreviewViewId, 0);
    if (m_PreviewTargetsBound)
    {
        rs->Views().SetViewTargets(
            m_PreviewViewId,
            /*color*/ 0,
            /*depth*/ 0,
            /*resolve*/ 0,
            Rendering::ViewClearConfig{});
        m_PreviewTargetsBound = false;
    }
    return true;
}

void SceneViewController::CancelBookmarkPreview()
{
    m_RequestPreview = false;
    m_PreviewPendingFreeze = false;
    if (m_RenderServices && m_PreviewViewId != 0)
    {
        // Cancelling stops arming the OnDemand view: DeclarePreviewTargetsRG
        // early-returns while the preview is neither requested nor pending, so
        // RequestViewFrame is no longer called and the view lapses to dormant on
        // its own — no manual mask-off needed. Release the camera so the dormant
        // view is headless (avoids per-frame diagnostics for a camera-backed view
        // with no attachments).
        if (m_PreviewTargetsBound)
        {
            m_RenderServices->Views().SetViewCamera(m_PreviewViewId, 0);
            m_RenderServices->Views().SetViewTargets(
                m_PreviewViewId,
                /*color*/ 0,
                /*depth*/ 0,
                /*resolve*/ 0,
                Rendering::ViewClearConfig{});
            m_PreviewTargetsBound = false;
        }
        else
        {
            m_RenderServices->Views().SetViewCamera(m_PreviewViewId, 0);
        }
    }

    // Close the serial gap so CreateRenderPreview does not re-enable the
    // view on subsequent frames (previewPending would be true otherwise,
    // causing up to ~10 frames of wasted GPU work).
    m_PreviewLastSubmittedSerial = m_PreviewPendingSerial;
    // Kill the per-frame ExportTexture too — Record (which resets this)
    // never runs again once the window returns to RenderGraph.
    m_PreviewSubmittedThisFrame = false;

    // Keep the view/camera allocated — releasing and re-allocating per hover
    // causes monotonically increasing ViewIds, and all per-view pipeline
    // resources (passes, buffers, shadow maps) accumulate as orphans in the
    // render graph. Reusing the same view/camera avoids the leak entirely.
    m_PreviewNoDrawRetryFrames = 0;
    m_PreviewHadDrawItemsThisFrame = false;
}

std::shared_ptr<Rendering::RGReadbackTicket> SceneViewController::RequestPreviewReadbackRG(
    Rendering::RenderGraph::RGFrame& frame)
{
    if (!m_RenderServices || m_PreviewViewId == 0 || !m_RenderServices->GetDevice())
        return nullptr;
    const auto out = m_RenderServices->GetPipelineOutputRG(frame, m_PreviewViewId);
    if (!out.IsValid())
        return nullptr;
    return Rendering::RequestTextureReadbackRG(m_RenderServices->GetDevice(), frame, out.Out,
                                               "SceneView.BookmarkPreview.Readback");
}

void SceneViewController::SetActiveTool(ToolKind kind)
{
    const bool changed = m_ActiveToolKind != kind;
    if (kind != ToolKind::Transform)
        ClearMeasureEndpointTransformTarget();
    m_ActiveToolKind = kind;

    switch (m_ActiveToolKind)
    {
    case ToolKind::Selection:
        m_ToolContext.SetActiveTool(m_SelectionTool.get());
        break;
    case ToolKind::Transform:
        m_ToolContext.SetActiveTool(m_TransformTool.get());
        break;
    case ToolKind::NavGridBrush:
        m_ToolContext.SetActiveTool(m_NavGridBrushTool.get());
        break;
    case ToolKind::Registered:
        if (const auto it = m_RegisteredTools.find(m_ActiveRegisteredToolId); it != m_RegisteredTools.end())
        {
            m_ToolContext.SetActiveTool(it->second.get());
            break;
        }
        m_ActiveToolKind = ToolKind::Selection;
        m_ToolContext.SetActiveTool(m_SelectionTool.get());
        break;
    default:
        m_ToolContext.SetActiveTool(m_SelectionTool.get());
        break;
    }

    if (changed && m_OnActiveToolChangedCallback)
        m_OnActiveToolChangedCallback(m_ActiveToolKind);
}

Editor::SceneTools::ISceneTool* SceneViewController::GetRegisteredTool(std::string_view id)
{
    if (const auto it = m_RegisteredTools.find(std::string(id)); it != m_RegisteredTools.end())
        return it->second.get();
    const Editor::SceneViewToolStripEntry* entry = Editor::SceneViewToolStripRegistry::Get().Find(id);
    if (!entry || !entry->CreateTool)
        return nullptr;
    return m_RegisteredTools.emplace(std::string(id), entry->CreateTool(*this)).first->second.get();
}

bool SceneViewController::SetActiveRegisteredTool(std::string_view id)
{
    if (!Editor::RegisteredToolRefusal(Editor::SceneViewToolStripRegistry::Get(), id, &GetWorld()).empty() ||
        !GetRegisteredTool(id))
        return false;
    // A switch between two registered tools is a change too: leave Registered first so
    // SetActiveTool deactivates the old tool and reports the change.
    if (m_ActiveToolKind == ToolKind::Registered && m_ActiveRegisteredToolId != id)
        SetActiveTool(ToolKind::Transform);
    m_ActiveRegisteredToolId = std::string(id);
    SetActiveTool(ToolKind::Registered);
    return true;
}

std::string_view SceneViewController::GetActiveRegisteredToolId() const
{
    return m_ActiveToolKind == ToolKind::Registered ? std::string_view(m_ActiveRegisteredToolId) : std::string_view();
}

void SceneViewController::ActivateRegisteredToolForSelection(ECS::EntityHandle primary)
{
    ECS::World* world = &GetWorld();
    for (const Editor::SceneViewToolStripEntry& entry : Editor::SceneViewToolStripRegistry::Get().Entries())
    {
        if (entry.ActivatesForSelection && entry.ActivatesForSelection(*world, primary, m_SelectedEntities))
        {
            SetActiveRegisteredTool(entry.Id);
            return;
        }
    }
}

void SceneViewController::ClearMeasureEndpointTransformTarget()
{
    m_MeasureSceneGizmo.Selection() = {};
    m_MeasureEndpointEdit = {};
    m_MeasureEndpointUndoEdit = {};
    if (m_MeasureEndpointTargetSynced && m_TransformTool)
        m_TransformTool->ClearExternalPointTarget();
    m_MeasureEndpointTargetSynced = false;
}

bool SceneViewController::GetSelectedMeasureEndpointWorldPosition(Mathematics::Vector3& outWorldPosition) const
{
    const Editor::SceneTools::MeasureEndpointSelection selected =
        m_MeasureSceneGizmo.Selection();
    if (!selected.IsValid())
        return false;

    ECS::World* world = &GetWorld();
    if (!world->IsValid(selected.Entity))
        return false;

    const auto* measure = world->GetComponent<Components::MeasureComponent>(selected.Entity);
    if (!measure || !ECS::Entity(world, selected.Entity).IsEnabled<Components::MeasureComponent>())
        return false;

    const ECS::EntityHandle endpointEntity =
        ResolveMeasureEndpointEntity(world, selected.Entity, *measure, selected.Endpoint);
    return GetEntityPositionForMeasure(world, endpointEntity, outWorldPosition);
}

void SceneViewController::SyncMeasureEndpointTransformTarget()
{
    if (!m_TransformTool)
        return;

    Mathematics::Vector3 pivot(0.0f, 0.0f, 0.0f);
    const bool shouldSync =
        m_ActiveToolKind == ToolKind::Transform && GetSelectedMeasureEndpointWorldPosition(pivot);
    if (!shouldSync)
    {
        if (m_MeasureEndpointTargetSynced)
            m_TransformTool->ClearExternalPointTarget();
        m_MeasureEndpointTargetSynced = false;
        return;
    }

    m_TransformTool->SetExternalPointTarget({
        [this](Mathematics::Vector3& outWorldPosition) -> bool
        {
            return GetSelectedMeasureEndpointWorldPosition(outWorldPosition);
        },
        [this](const char* editName)
        {
            BeginMeasureEndpointTransformEdit(editName);
        },
        [this](const Mathematics::Vector3& deltaWorld)
        {
            ApplyMeasureEndpointTransformDelta(deltaWorld);
        },
        [this]()
        {
            CommitMeasureEndpointTransformEdit();
        }
    });
    m_MeasureEndpointTargetSynced = true;
}

void SceneViewController::BeginMeasureEndpointTransformEdit(const char* editName)
{
    const Editor::SceneTools::MeasureEndpointSelection selected =
        m_MeasureSceneGizmo.Selection();
    if (!selected.IsValid())
        return;

    Mathematics::Vector3 point(0.0f, 0.0f, 0.0f);
    if (!GetSelectedMeasureEndpointWorldPosition(point))
        return;

    ECS::World* world = &GetWorld();
    ECS::EntityHandle endpointEntity{};
    if (world->IsValid(selected.Entity))
    {
        if (const auto* measure = world->GetComponent<Components::MeasureComponent>(selected.Entity))
            endpointEntity = ResolveMeasureEndpointEntity(world, selected.Entity, *measure, selected.Endpoint);
    }

    m_MeasureEndpointEdit.Active = true;
    m_MeasureEndpointEdit.Entity = selected.Entity;
    m_MeasureEndpointEdit.Endpoint = selected.Endpoint;
    m_MeasureEndpointEdit.StartPosition = point;
    m_MeasureEndpointEdit.AccumulatedDelta = Mathematics::Vector3(0.0f, 0.0f, 0.0f);

    m_MeasureEndpointUndoEdit = {};
    if (!m_UndoRedo || !endpointEntity.IsValid())
        return;

    const ECS::EntityHandle measureEntity = selected.Entity;
    Editor::UndoRedoService::SnapshotTarget target{};
    target.debugLabel = "Measure endpoint";
    target.Capture = [world, measureEntity, endpointEntity](Editor::UndoRedoService::SnapshotTarget::Snapshot& out) -> bool
    {
        out.clear();
        auto appendU32 = [&out](std::uint32_t value)
        {
            const std::uint8_t* bytes = reinterpret_cast<const std::uint8_t*>(&value);
            out.insert(out.end(), bytes, bytes + sizeof(value));
        };
        auto appendComponent = [&](ECS::EntityHandle entity, ECS::ComponentTypeId typeId) -> bool
        {
            std::vector<std::uint8_t> bytes;
            const bool present = entity.IsValid() && world->IsValid(entity) &&
                                 world->CaptureComponentBytes(entity, typeId, bytes);
            appendU32(present ? 1u : 0u);
            appendU32(static_cast<std::uint32_t>(bytes.size()));
            out.insert(out.end(), bytes.begin(), bytes.end());
            return true;
        };

        appendComponent(measureEntity, ECS::GetComponentTypeId<Components::MeasureComponent>());
        appendComponent(endpointEntity, ECS::GetComponentTypeId<Components::Transform>());
        appendComponent(endpointEntity, ECS::GetComponentTypeId<Components::WorldTransform>());
        return true;
    };
    target.Apply = [world, measureEntity, endpointEntity](const Editor::UndoRedoService::SnapshotTarget::Snapshot& in) -> bool
    {
        std::size_t offset = 0;
        auto readU32 = [&in, &offset](std::uint32_t& value) -> bool
        {
            if (offset + sizeof(value) > in.size())
                return false;
            std::memcpy(&value, in.data() + offset, sizeof(value));
            offset += sizeof(value);
            return true;
        };
        auto applyComponent = [&](ECS::EntityHandle entity, ECS::ComponentTypeId typeId) -> bool
        {
            std::uint32_t present = 0u;
            std::uint32_t size = 0u;
            if (!readU32(present) || !readU32(size) || offset + size > in.size())
                return false;
            if (!entity.IsValid() || !world->IsValid(entity))
                return false;

            if (present != 0u)
            {
                std::vector<std::uint8_t> bytes(size);
                if (size > 0u)
                    std::memcpy(bytes.data(), in.data() + offset, size);
                offset += size;
                return world->ApplyComponentBytesImmediate(entity, typeId, bytes);
            }

            offset += size;
            (void)world->RemoveComponentByTypeIdImmediate(entity, typeId);
            return true;
        };

        if (!applyComponent(measureEntity, ECS::GetComponentTypeId<Components::MeasureComponent>()))
            return false;
        if (!applyComponent(endpointEntity, ECS::GetComponentTypeId<Components::Transform>()))
            return false;
        if (!applyComponent(endpointEntity, ECS::GetComponentTypeId<Components::WorldTransform>()))
            return false;
        return offset == in.size();
    };
    target.Notify = [this, world, measureEntity, endpointEntity](Editor::EditorChangeNotifications::ChangeKind kind)
    {
        if (!m_ChangeNotifications)
            return;
        m_ChangeNotifications->NotifyComponentChange<Components::MeasureComponent>(world, measureEntity, kind);
        m_ChangeNotifications->NotifyComponentChange<Components::Transform>(world, endpointEntity, kind);
    };
    m_MeasureEndpointUndoEdit = m_UndoRedo->BeginInteractiveEdit(
        editName && *editName ? editName : "Move Measure Point",
        std::move(target));
}

void SceneViewController::ApplyMeasureEndpointTransformDelta(const Mathematics::Vector3& deltaWorld)
{
    const Editor::SceneTools::MeasureEndpointSelection selected =
        m_MeasureSceneGizmo.Selection();
    if (!selected.IsValid())
        return;

    ECS::World* world = &GetWorld();
    if (!world->IsValid(selected.Entity))
        return;

    if (!ECS::Entity(world, selected.Entity).IsEnabled<Components::MeasureComponent>())
        return;
    auto* measure = world->GetComponentForWrite<Components::MeasureComponent>(selected.Entity);
    if (!measure)
        return;

    if (!m_MeasureEndpointEdit.Active || m_MeasureEndpointEdit.Entity != selected.Entity ||
        m_MeasureEndpointEdit.Endpoint != selected.Endpoint)
    {
        BeginMeasureEndpointTransformEdit("Move Measure Point");
    }

    m_MeasureEndpointEdit.AccumulatedDelta = m_MeasureEndpointEdit.AccumulatedDelta + deltaWorld;
    const Mathematics::Vector3 next = m_MeasureEndpointEdit.StartPosition + m_MeasureEndpointEdit.AccumulatedDelta;

    // MeasureComponent keeps its endpoints as float[3] fields.

    float* point = (selected.Endpoint == 1u) ? measure->Start : measure->End;
    point[0] = next.x;
    point[1] = next.y;
    point[2] = next.z;

    ECS::EntityHandle endpointEntity =
        ResolveMeasureEndpointEntity(world, selected.Entity, *measure, selected.Endpoint);
    if (!endpointEntity.IsValid())
        return;

    SetEntityPositionForMeasure(world, endpointEntity, next);
    if (m_ChangeNotifications)
    {
        m_ChangeNotifications->NotifyComponentChange<Components::Transform>(
            world, endpointEntity, Editor::EditorChangeNotifications::ChangeKind::Preview);
    }

    NotifyMeasureEndpointChanged(selected.Entity, Editor::EditorChangeNotifications::ChangeKind::Preview);
}

void SceneViewController::CommitMeasureEndpointTransformEdit()
{
    if (m_MeasureEndpointEdit.Active && m_MeasureEndpointEdit.Entity.IsValid())
    {
        ECS::World* world = &GetWorld();
        if (world->IsValid(m_MeasureEndpointEdit.Entity))
        {
            if (const auto* measure =
                    world->GetComponent<Components::MeasureComponent>(m_MeasureEndpointEdit.Entity))
            {
                const ECS::EntityHandle endpointEntity =
                    ResolveMeasureEndpointEntity(world,
                                                 m_MeasureEndpointEdit.Entity,
                                                 *measure,
                                                 m_MeasureEndpointEdit.Endpoint);
                if (endpointEntity.IsValid() && m_ChangeNotifications)
                {
                    m_ChangeNotifications->NotifyComponentChange<Components::Transform>(
                        world, endpointEntity, Editor::EditorChangeNotifications::ChangeKind::Commit);
                }
            }
        }
        NotifyMeasureEndpointChanged(m_MeasureEndpointEdit.Entity,
                                     Editor::EditorChangeNotifications::ChangeKind::Commit);
        if (m_MeasureEndpointUndoEdit)
            m_MeasureEndpointUndoEdit.Commit();
    }
    m_MeasureEndpointEdit = {};
    m_MeasureEndpointUndoEdit = {};
}

void SceneViewController::SyncMeasureFromEndpointEntityTransform(
    ECS::EntityHandle endpointEntity,
    Editor::EditorChangeNotifications::ChangeKind kind)
{
    if (!m_ChangeNotifications)
        return;

    ECS::World* world = &GetWorld();
    if (!endpointEntity.IsValid() || !world->IsValid(endpointEntity))
        return;

    Mathematics::Vector3 point(0.0f, 0.0f, 0.0f);
    if (!GetEntityPositionForMeasure(world, endpointEntity, point))
        return;

    world->Query<ECS::Write<Components::MeasureComponent>>()
        .Each([&](ECS::EntityHandle measureEntity, Components::MeasureComponent& measure)
        {

            std::uint8_t endpoint = 0;
            if (measure.StartEntity == endpointEntity)
                endpoint = 1u;
            else if (measure.EndEntity == endpointEntity)
                endpoint = 2u;

            if (endpoint == 0u)
                return;

            float* target = endpoint == 1u ? measure.Start : measure.End;
            if (target[0] == point.x && target[1] == point.y && target[2] == point.z)
                return;

            target[0] = point.x;
            target[1] = point.y;
            target[2] = point.z;
            m_ChangeNotifications->NotifyComponentChange<Components::MeasureComponent>(
                world, measureEntity, kind);
        });
}

void SceneViewController::NotifyMeasureEndpointChanged(
    ECS::EntityHandle entity,
    Editor::EditorChangeNotifications::ChangeKind kind)
{
    if (!m_ChangeNotifications)
        return;

    ECS::World* world = &GetWorld();
    if (!entity.IsValid() || !world->IsValid(entity))
        return;

    m_ChangeNotifications->NotifyComponentChange<Components::MeasureComponent>(world, entity, kind);
    if (kind == Editor::EditorChangeNotifications::ChangeKind::Commit &&
        world->GetComponent<Components::Transform>(entity))
    {
        m_ChangeNotifications->NotifyComponentChange<Components::Transform>(world, entity, kind);
    }
}

bool SceneViewController::HasMeasureEndpointAtPointer(const Editor::SceneTools::ScenePointerEvent& event)
{
    if (event.button != Editor::SceneTools::PointerButton::Left ||
        event.phase != Editor::SceneTools::PointerPhase::Down)
    {
        return false;
    }

    Editor::SceneTools::GizmoHitResult hit = m_MeasureSceneGizmo.HitTest(event.ray);
    return hit.hit;
}

void SceneViewController::RebuildSelectionDescendants()
{
    m_SelectionDescendants.clear();
    auto* world = &GetWorld();
    // Union each selected root's subtree. DescendantsOf clears its out-param per
    // call, so accumulate through a scratch buffer (the previous single-call loop
    // left only the last root's descendants), then de-dup across roots — a root
    // can be a descendant of another selected root.
    for (const auto& entity : m_SelectedEntities)
    {
        GameEngine::Components::DescendantsOf(*world, entity, m_DescendantScratch);
        m_SelectionDescendants.insert(m_SelectionDescendants.end(), m_DescendantScratch.begin(),
                                      m_DescendantScratch.end());
    }
    if (m_SelectedEntities.size() > 1)
    {
        std::unordered_set<ECS::EntityHandle, ECS::EntityHandleHash> seen;
        seen.reserve(m_SelectionDescendants.size());
        m_SelectionDescendants.erase(
            std::remove_if(m_SelectionDescendants.begin(), m_SelectionDescendants.end(),
                           [&](ECS::EntityHandle e) { return !seen.insert(e).second; }),
            m_SelectionDescendants.end());
    }
    m_SelectionGizmo.SetDescendants(m_SelectionDescendants);
}

void SceneViewController::SilentlyClearSelection()
{
    if (m_SelectedEntities.empty())
        return;
    m_DepthCycleAnchor = false;
    m_PickCycleIndex = -1;
    m_PickCycleCount = 0;
    ClearMeasureEndpointTransformTarget();
    m_SelectedEntities.clear();
    ClearStaleSplineSelection(m_SplineSceneGizmo.State(), m_SelectedEntities, true);
    m_SelectionGizmo.SetSelection(m_SelectedEntities);
    m_ComponentGizmos.SetSelection(m_SelectedEntities);
    m_ReflectionProbeGizmo.SetSelection(m_SelectedEntities);
    m_DDGIVolumeGizmo.SetSelection(m_SelectedEntities);
    m_TerrainModifierGizmo.SetSelection(m_SelectedEntities);
    m_LightGizmo.SetSelection(m_SelectedEntities);
    RebuildSelectionDescendants();
    if (m_TransformTool)
    {
        m_TransformTool->ClearPivot();
        m_TransformTool->SetTargetEntity({});
        m_TransformTool->SetSelectedEntities(m_SelectedEntities);
    }
}

void SceneViewController::SilentlySetSelection(const std::vector<GameEngine::ECS::EntityHandle>& entities)
{
    m_DepthCycleAnchor = false;
    m_PickCycleIndex = -1;
    m_PickCycleCount = 0;
    ClearMeasureEndpointTransformTarget();
    m_SelectedEntities.clear();
    m_SelectedEntities.reserve(entities.size());

    GameEngine::ECS::World* world = &GetWorld();
    for (const auto& entity : entities)
    {
        if (!entity.IsValid())
            continue;
        if (!world->IsValid(entity))
            continue;
        if (std::find(m_SelectedEntities.begin(), m_SelectedEntities.end(), entity) == m_SelectedEntities.end())
            m_SelectedEntities.push_back(entity);
    }

    ClearStaleSplineSelection(m_SplineSceneGizmo.State(), m_SelectedEntities, true);

    m_SelectionGizmo.SetSelection(m_SelectedEntities);
    m_ComponentGizmos.SetSelection(m_SelectedEntities);
    m_ReflectionProbeGizmo.SetSelection(m_SelectedEntities);
    m_DDGIVolumeGizmo.SetSelection(m_SelectedEntities);
    m_TerrainModifierGizmo.SetSelection(m_SelectedEntities);
    m_LightGizmo.SetSelection(m_SelectedEntities);
    RebuildSelectionDescendants();

    GameEngine::ECS::EntityHandle primary{};
    if (!m_SelectedEntities.empty())
        primary = m_SelectedEntities.back();

    if (!m_TransformTool)
        return;

    if (!primary.IsValid() || !world->IsValid(primary))
    {
        m_TransformTool->ClearPivot();
        m_TransformTool->SetTargetEntity({});
        m_TransformTool->SetSelectedEntities(m_SelectedEntities);
        return;
    }

    m_TransformTool->SetTargetEntity(primary);
    m_TransformTool->SetSelectedEntities(m_SelectedEntities);
    m_TransformTool->RefreshPivotFromTargetEntity();
}

GameEngine::ECS::EntityHandle SceneViewController::PickIconAtRay(const Editor::SceneTools::ScenePointerEvent& event,
                                                                 bool revealHidden)
{
    GameEngine::ECS::World* world = &GetWorld();

    using GameEngine::Components::Light;
    using GameEngine::Components::ReflectionProbe;
    using GameEngine::Components::WorldTransform;

    const Mathematics::Vector3 camPos = GetCameraWorldPos();
    const float orthoHeight = GetGizmoOrthoHeightOrZero();

    // Each icon is picked through a sphere padded past its radius, so the
    // icon's outer details (light rays, probe ring) stay clickable.
    using Editor::SceneTools::GizmoIconPickSphere;
    std::vector<GizmoIconPickSphere> lightIcons;
    std::vector<GameEngine::ECS::EntityHandle> lightEntities;
    std::vector<GizmoIconPickSphere> probeIcons;
    std::vector<GameEngine::ECS::EntityHandle> probeEntities;

    auto lightQuery = world->Query<
        GameEngine::ECS::Read<WorldTransform>,
        GameEngine::ECS::Read<Light>>();
    // The reveal modifier is what makes inactive entities pickable; without it
    // the query never visits them, self-disabled or disabled by an ancestor.
    if (revealHidden)
        lightQuery.IncludeDisabled();
    lightQuery.Each([&](GameEngine::ECS::EntityHandle e,
                        const WorldTransform& xf,
                        const Light& light)
    {
        if (!revealHidden && !IsEntityPickable(e))
            return;
        const Mathematics::Vector3 pos(xf.matrix[12], xf.matrix[13], xf.matrix[14]);
        const float r = Editor::SceneTools::ComputeGizmoIconWorldRadius(pos, camPos, orthoHeight);
        if (r <= 0.0f)
            return;
        lightIcons.push_back({pos, r * 1.1f});
        lightEntities.push_back(e);
    });

    auto probeQuery = world->Query<
        GameEngine::ECS::Read<WorldTransform>,
        GameEngine::ECS::Read<ReflectionProbe>>();
    if (revealHidden)
        probeQuery.IncludeDisabled();
    probeQuery.Each([&](GameEngine::ECS::EntityHandle e,
                        const WorldTransform& xf,
                        const ReflectionProbe& probe)
    {
        if (!revealHidden && !IsEntityPickable(e))
            return;
        const Mathematics::Vector3 pos(xf.matrix[12], xf.matrix[13], xf.matrix[14]);
        const float r = Editor::SceneTools::ComputeGizmoIconWorldRadius(pos, camPos, orthoHeight);
        if (r <= 0.0f)
            return;
        probeIcons.push_back({pos, r * 1.15f});
        probeEntities.push_back(e);
    });

    // Probe icons take precedence over light icons, matching their draw order.
    if (const auto probe = Editor::SceneTools::PickNearestGizmoIcon(event.ray, probeIcons))
        return probeEntities[*probe];
    if (const auto light = Editor::SceneTools::PickNearestGizmoIcon(event.ray, lightIcons))
        return lightEntities[*light];
    return {};
}

void SceneViewController::UpdatePickCandidates(GameEngine::ECS::World& world,
                                               const Mathematics::Ray3D& ray)
{
    const bool exactPick = GameEngine::Editor::SceneViewSettings::Get().GetExactPickMode();
    const std::uint64_t writeVersion = world.GetGlobalSystemVersion();
    const std::size_t structuralVersion = world.GetStructuralChangeVersion();

    auto rayEqual = [](const Mathematics::Ray3D& a, const Mathematics::Ray3D& b)
    {
        constexpr float kEps = 1.0e-6f;
        return std::fabs(a.origin.x - b.origin.x) < kEps &&
               std::fabs(a.origin.y - b.origin.y) < kEps &&
               std::fabs(a.origin.z - b.origin.z) < kEps &&
               std::fabs(a.direction.x - b.direction.x) < kEps &&
               std::fabs(a.direction.y - b.direction.y) < kEps &&
               std::fabs(a.direction.z - b.direction.z) < kEps;
    };

    // Same ray, same pick mode, and no component write or structural change
    // since the last pick: the hit and candidate lists are still valid. Any
    // write anywhere invalidates (conservative), so a stationary cursor over
    // animating content simply re-raycasts.
    if (m_PickCacheValid &&
        m_PickCacheExactMode == exactPick &&
        m_PickCacheWriteVersion == writeVersion &&
        m_PickCacheStructuralVersion == structuralVersion &&
        rayEqual(ray, m_PickCacheRay))
        return;

    // Per-triangle raycast, sorted front-to-back. Locked entities and their
    // descendants are filtered out without blocking picks of entities behind.
    // Terrain is included (PickOptions default): it is scene geometry the user
    // clicks like any other, and the scene view has no other way to select it.
    Editor::Picking::PickOptions options;
    options.WaitForSceneTlas = false; // the caller saw the scene TLAS ready (SceneTlasReadyForPick)
    GameEngine::Editor::Picking::RaycastSceneAll(ray, world, m_PickHits, options);
    m_PickHits.erase(std::remove_if(m_PickHits.begin(), m_PickHits.end(),
                                    [this](const Editor::Picking::PickHit& h)
                                    { return !IsEntityPickable(h.Entity); }),
                     m_PickHits.end());
    Editor::Picking::BuildPickCandidates(world, m_PickHits, exactPick, m_PickCandidates);

    m_PickCacheRay = ray;
    m_PickCacheExactMode = exactPick;
    // Sample versions after the raycast: RaycastSceneAll's TLAS sync may
    // itself count as a write grant, which would otherwise invalidate the
    // fresh cache immediately.
    m_PickCacheWriteVersion = world.GetGlobalSystemVersion();
    m_PickCacheStructuralVersion = world.GetStructuralChangeVersion();
    m_PickCacheValid = true;
}

Editor::SceneTools::ScenePickOutcome SceneViewController::PickViaClickThrough(
    const Editor::SceneTools::ScenePointerEvent& event,
    bool clearOnMiss)
{
    GameEngine::ECS::World* world = &GetWorld();

    const bool shift = event.shift;
    const bool ctrl  = event.ctrl;

    auto rememberClickPos = [this, &event]()
    {
        m_LastClickViewX = event.viewX;
        m_LastClickViewY = event.viewY;
    };

    // Icon picks (lights, probes) override mesh hits and bypass click-through:
    // the icon is rendered on top of scene geometry.
    if (GameEngine::ECS::EntityHandle iconPick = PickIconAtRay(event); iconPick.IsValid())
    {
        m_DepthCycleAnchor = false;
        OnEntityPicked(iconPick, shift, ctrl, /*skipPickRootResolve=*/true);
        rememberClickPos();
        return {iconPick};
    }

    // Deferred while the scene TLAS refits, and applied when it finishes as a
    // plain click: no marquee follows a deferred press, so a miss then clears
    // the selection like any click on empty space.
    if (!SceneTlasReadyForPick(*world, PickPurpose::Click))
    {
        m_DeferredClick = event;
        return {{}, /*Deferred=*/true};
    }

    // A depth-cycle pick (Ctrl/Cmd+scroll) is "anchored": the first click
    // after cycling confirms it instead of restarting the click-through
    // sequence. Consumed only by a pick that runs, so a deferred press keeps it.
    const bool depthCycleAnchor = std::exchange(m_DepthCycleAnchor, false);

    UpdatePickCandidates(*world, event.ray);
    const auto& candidates = m_PickCandidates;

    if (candidates.empty())
    {
        if (!shift && !ctrl && clearOnMiss)
        {
            m_SelectionGizmo.Clear();
            OnEntityPicked(GameEngine::ECS::EntityHandle{}, false, false);
        }
        rememberClickPos();
        return {};
    }

    const GameEngine::ECS::EntityHandle front = candidates.front();
    const GameEngine::ECS::EntityHandle nearestExact = m_PickHits.front().Entity;

    // Modifier clicks toggle/add the primary pick target; no click-through.
    // Prefer an already-selected member of the clicked instance (the exact
    // hit first, then any selected descendant) so Ctrl+click deselects what
    // the user drilled to instead of toggling the root in alongside it.
    if (shift || ctrl)
    {
        GameEngine::ECS::EntityHandle target = front;
        const bool exactPick = GameEngine::Editor::SceneViewSettings::Get().GetExactPickMode();
        if (!exactPick)
        {
            if (std::find(m_SelectedEntities.begin(), m_SelectedEntities.end(), nearestExact) !=
                m_SelectedEntities.end())
            {
                target = nearestExact;
            }
            else
            {
                for (const GameEngine::ECS::EntityHandle& e : m_SelectedEntities)
                {
                    if (e != front && world->IsValid(e) &&
                        Editor::Picking::ResolvePickRoot(*world, e) == front)
                    {
                        target = e;
                        break;
                    }
                }
            }
        }
        OnEntityPicked(target, shift, ctrl, /*skipPickRootResolve=*/true);
        rememberClickPos();
        return {target};
    }

    const GameEngine::ECS::EntityHandle primary =
        m_SelectedEntities.empty() ? GameEngine::ECS::EntityHandle{} : m_SelectedEntities.back();

    const auto primaryIt = std::find(candidates.begin(), candidates.end(), primary);
    const int primaryIdx = primaryIt == candidates.end()
        ? -1
        : static_cast<int>(std::distance(candidates.begin(), primaryIt));

    constexpr float kSameSpotPx = 4.0f;
    const bool sameSpot = std::fabs(event.viewX - m_LastClickViewX) <= kSameSpotPx &&
                          std::fabs(event.viewY - m_LastClickViewY) <= kSameSpotPx;

    const bool exactPick = GameEngine::Editor::SceneViewSettings::Get().GetExactPickMode();

    GameEngine::ECS::EntityHandle target;
    int cycleIdx = -1; // depth position for the pill indicator; -1 = fresh pick
    if (primaryIdx >= 0 && depthCycleAnchor)
    {
        target = primary; // confirm the Ctrl+scroll pick
        cycleIdx = primaryIdx;
    }
    else if (primaryIdx >= 0 && sameSpot)
    {
        const int nextIdx = (primaryIdx + 1) % static_cast<int>(candidates.size());
        target = candidates[nextIdx];
        cycleIdx = nextIdx;
    }
    else if (!exactPick && primary.IsValid() && world->IsValid(primary) &&
             Editor::Picking::ResolvePickRoot(*world, primary) == front)
    {
        // Selection is already inside this instance (its root, or a child
        // reached by drilling): select the exact entity under the cursor.
        target = nearestExact;
        cycleIdx = static_cast<int>(std::distance(
            candidates.begin(), std::find(candidates.begin(), candidates.end(), nearestExact)));
    }
    else
    {
        target = front;
    }

    const int candidateCount = static_cast<int>(candidates.size());
    OnEntityPicked(target, false, false, /*skipPickRootResolve=*/true);
    if (cycleIdx >= 0)
    {
        m_PickCycleIndex = cycleIdx;
        m_PickCycleCount = candidateCount;
        // Sync the hover pill to the cycled pick so the depth indicator shows.
        if (!m_SelectedEntities.empty())
            SetHoverEntity(m_SelectedEntities.back(), false);
    }
    rememberClickPos();
    return {target};
}

void SceneViewController::OnEntityPicked(GameEngine::ECS::EntityHandle entity,
                                         bool shift,
                                         bool ctrl,
                                         bool skipPickRootResolve)
{
    if (entity.IsValid() && !IsEntityPickable(entity))
        return;
    m_DepthCycleAnchor = false;
    m_PickCycleIndex = -1;
    m_PickCycleCount = 0;
    ClearMeasureEndpointTransformTarget();

    // Root-pick mode: resolve to the model-instance root (Unity-style
    // outermost-prefab pick). Plain grouping nodes are never crossed.
    if (entity.IsValid() &&
        !GameEngine::Editor::SceneViewSettings::Get().GetExactPickMode() &&
        !skipPickRootResolve)
    {
        GameEngine::ECS::World* world = &GetWorld();
        entity = Editor::Picking::ResolvePickRoot(*world, entity);
    }

    // Selection modifiers:
    //   No modifier:   Replace.
    //   Shift or Ctrl: Toggle (add if absent, remove if present).
    //   Shift+Ctrl:    Add only.
    if (shift || ctrl)
    {
        if (entity.IsValid())
        {
            auto it = std::find(m_SelectedEntities.begin(), m_SelectedEntities.end(), entity);
            const bool present = (it != m_SelectedEntities.end());

            if (shift && ctrl)
            {
                if (!present)
                    m_SelectedEntities.push_back(entity); // add only
            }
            else // shift or ctrl alone: toggle
            {
                if (present)
                    m_SelectedEntities.erase(it);
                else
                    m_SelectedEntities.push_back(entity);
            }
        }
        // With modifiers, clicking empty space preserves selection.
    }
    else
    {
        // No modifiers: replace selection.
        m_SelectedEntities.clear();
        if (entity.IsValid())
            m_SelectedEntities.push_back(entity);
    }

    ClearStaleSplineSelection(m_SplineSceneGizmo.State(), m_SelectedEntities, true);

    // Update selection gizmo with full selection set.
    m_SelectionGizmo.SetSelection(m_SelectedEntities);
    m_ComponentGizmos.SetSelection(m_SelectedEntities);
    m_ReflectionProbeGizmo.SetSelection(m_SelectedEntities);
    m_DDGIVolumeGizmo.SetSelection(m_SelectedEntities);
    m_TerrainModifierGizmo.SetSelection(m_SelectedEntities);
    m_LightGizmo.SetSelection(m_SelectedEntities);
    RebuildSelectionDescendants();

    // The primary (anchor) entity for the transform tool is the last picked entity,
    // or the last entity in the list if toggle-removing.
    GameEngine::ECS::EntityHandle primary = entity.IsValid() ? entity : GameEngine::ECS::EntityHandle{};
    if (!m_SelectedEntities.empty())
    {
        // If the picked entity was removed by toggle, use the last remaining entity.
        if (std::find(m_SelectedEntities.begin(), m_SelectedEntities.end(), primary) == m_SelectedEntities.end())
            primary = m_SelectedEntities.back();
    }
    else
    {
        primary = {};
    }

    // Keep the TransformTool's pivot in sync with the primary entity.
    if (m_TransformTool)
    {
        if (!primary.IsValid())
        {
            m_TransformTool->ClearPivot();
            m_TransformTool->SetTargetEntity({});
            m_TransformTool->SetSelectedEntities(m_SelectedEntities);
        }
        else
        {
            GameEngine::ECS::World* world = &GetWorld();
            if (!world->IsValid(primary))
            {
                m_TransformTool->ClearPivot();
                m_TransformTool->SetTargetEntity({});
                m_TransformTool->SetSelectedEntities(m_SelectedEntities);
            }
            else
            {
                m_TransformTool->SetTargetEntity(primary);
                m_TransformTool->SetSelectedEntities(m_SelectedEntities);
                m_TransformTool->RefreshPivotFromTargetEntity();
            }
        }
    }

    ActivateRegisteredToolForSelection(primary);

    // Notify single-entity callback (gizmo anchor / hierarchy sync).
    if (m_OnSelectEntityCallback)
        m_OnSelectEntityCallback(primary);

    // Notify multi-entity callback (inspector multi-edit).
    if (m_OnSelectEntitiesCallback && !m_SelectedEntities.empty())
        m_OnSelectEntitiesCallback(m_SelectedEntities);
}

void SceneViewController::OnEntitiesMarqueeSelected(
    const std::vector<GameEngine::ECS::EntityHandle>& entities,
    bool additive)
{
    m_DepthCycleAnchor = false;
    m_PickCycleIndex = -1;
    m_PickCycleCount = 0;
    ClearMeasureEndpointTransformTarget();
    if (!additive)
    {
        m_SelectedEntities.clear();
    }

    for (const auto& e : entities)
    {
        if (!e.IsValid())
            continue;
        if (m_IsEntityPickable && !m_IsEntityPickable(e))
            continue;
        auto it = std::find(m_SelectedEntities.begin(), m_SelectedEntities.end(), e);
        if (additive)
        {
            if (it == m_SelectedEntities.end())
                m_SelectedEntities.push_back(e);
        }
        else
        {
            if (it == m_SelectedEntities.end())
                m_SelectedEntities.push_back(e);
        }
    }

    ClearStaleSplineSelection(m_SplineSceneGizmo.State(), m_SelectedEntities, true);

    m_SelectionGizmo.SetSelection(m_SelectedEntities);
    m_ComponentGizmos.SetSelection(m_SelectedEntities);
    m_ReflectionProbeGizmo.SetSelection(m_SelectedEntities);
    m_DDGIVolumeGizmo.SetSelection(m_SelectedEntities);
    m_TerrainModifierGizmo.SetSelection(m_SelectedEntities);
    m_LightGizmo.SetSelection(m_SelectedEntities);
    RebuildSelectionDescendants();

    // Drive the TransformTool pivot to the last selected entity for consistent
    // gizmo placement after a marquee commit.
    GameEngine::ECS::EntityHandle primary{};
    if (!m_SelectedEntities.empty())
        primary = m_SelectedEntities.back();

    if (m_TransformTool)
    {
        if (!primary.IsValid())
        {
            m_TransformTool->ClearPivot();
            m_TransformTool->SetTargetEntity({});
            m_TransformTool->SetSelectedEntities(m_SelectedEntities);
        }
        else
        {
            GameEngine::ECS::World* world = &GetWorld();
            if (!world->IsValid(primary))
            {
                m_TransformTool->ClearPivot();
                m_TransformTool->SetTargetEntity({});
                m_TransformTool->SetSelectedEntities(m_SelectedEntities);
            }
            else
            {
                m_TransformTool->SetTargetEntity(primary);
                m_TransformTool->SetSelectedEntities(m_SelectedEntities);
                m_TransformTool->RefreshPivotFromTargetEntity();
            }
        }
    }

    ActivateRegisteredToolForSelection(primary);

    // Marquee/lasso selection commits the full list through
    // m_OnSelectEntitiesCallback. The single-entity callback would overwrite
    // the hierarchy with just the primary, so only fire it when the marquee
    // produced at most one entity. Always fire the multi-entity callback
    // (even when empty) so observers can reset their state.
    if (m_SelectedEntities.size() <= 1 && m_OnSelectEntityCallback)
        m_OnSelectEntityCallback(primary);
    if (m_OnSelectEntitiesCallback)
        m_OnSelectEntitiesCallback(m_SelectedEntities);
}

void SceneViewController::SetHoverEntity(GameEngine::ECS::EntityHandle entity,
                                         bool allowDisabled,
                                         HoverEntitySource source)
{
    // Drop any hover that targets a disabled entity or a descendant of one.
    // This centralizes the rule for scene-view ray-picking, hierarchy-panel
    // hover forwarding, and drag/drop preview paths.
    // `allowDisabled` bypasses the filter when the reveal-hidden modifier is held.
    if (!allowDisabled && entity.IsValid())
    {
        if (!GameEngine::ECS::Entity(&GetWorld(), entity).IsEnabledInHierarchy())
            entity = GameEngine::ECS::EntityHandle{};
    }

    const bool suppressPill = entity.IsValid() && (source == HoverEntitySource::Panel);
    if (entity == m_HoveredEntity)
    {
        // Same target, but the driving source can change (Hierarchy then Scene View over the same
        // entity) — refresh pill suppression without rebuilding hover gizmo state.
        m_SuppressHoverNamePill = suppressPill;
        return;
    }

    m_HoveredEntity = entity;
    m_SuppressHoverNamePill = suppressPill;
    m_HoveredDescendants.clear();

    if (!m_HoveredEntity.IsValid())
    {
        m_HoverGizmo.Clear();
    }
    else
    {
        auto* world = &GetWorld();
        GameEngine::Components::DescendantsOf(*world, m_HoveredEntity, m_HoveredDescendants);

        m_HoverGizmo.SetHover(m_HoveredEntity);
        m_HoverGizmo.SetDescendants(m_HoveredDescendants);
    }
    m_ComponentGizmos.SetHovered(m_HoveredEntity);
    m_ReflectionProbeGizmo.SetHovered(m_HoveredEntity);
    m_DDGIVolumeGizmo.SetHovered(m_HoveredEntity);
    m_TerrainModifierGizmo.SetHovered(m_HoveredEntity);
    m_LightGizmo.SetHovered(m_HoveredEntity);
    m_MeasureSceneGizmo.SetHovered(m_HoveredEntity);
}

void SceneViewController::HandleHoverDetection(const Editor::SceneTools::ScenePointerEvent& event)
{
    using namespace Editor::SceneTools;

    if (event.phase != PointerPhase::Move || event.button != PointerButton::None)
        return;

    // Keep measure endpoint hover responsive even when generic scene hover
    // highlighting is disabled or waiting for a modifier key.
    m_MeasureSceneGizmo.HitTest(event.ray);

    using GameEngine::Editor::HoverHighlightModifier;
    auto modifierHeld = [&event](HoverHighlightModifier mod)
    {
        switch (mod)
        {
        case HoverHighlightModifier::Control: return event.ctrl;
        case HoverHighlightModifier::Shift:   return event.shift;
        case HoverHighlightModifier::Grave:   return event.grave;
        }
        return false;
    };

    auto& settings = GameEngine::Editor::SceneViewSettings::Get();
    const bool revealHidden = modifierHeld(settings.GetRevealHiddenHoverModifier());

    auto mode = settings.GetHoverHighlightMode();
    // The reveal-hidden modifier forces hover on, so "Never" still responds
    // to it (otherwise the user couldn't inspect hidden objects at all).
    if (!revealHidden)
    {
        if (mode == GameEngine::Editor::HoverHighlightMode::Never)
        {
            SetHoverEntity({});
            m_LastHoverX = -1.0f;
            m_LastHoverY = -1.0f;
            m_LastHoverRayValid = false;
            return;
        }
        if (mode == GameEngine::Editor::HoverHighlightMode::WithModifier)
        {
            if (!modifierHeld(settings.GetHoverHighlightModifier()))
            {
                SetHoverEntity({});
                m_LastHoverX = -1.0f;
                m_LastHoverY = -1.0f;
                m_LastHoverRayValid = false;
                return;
            }
        }
    }

    // Cheap throttle: ignore tiny mouse moves.
    if (m_LastHoverX >= 0.0f && m_LastHoverY >= 0.0f)
    {
        const float dx = event.viewX - m_LastHoverX;
        const float dy = event.viewY - m_LastHoverY;
        constexpr float kMinMovePx = 2.0f;
        if ((dx * dx + dy * dy) < (kMinMovePx * kMinMovePx))
            return;
    }
    m_LastHoverX = event.viewX;
    m_LastHoverY = event.viewY;

    // Skip the per-triangle pass entirely when neither the screen position
    // nor the camera has changed since last call (the ray captures both).
    auto rayEqual = [](const Mathematics::Ray3D& a, const Mathematics::Ray3D& b) {
        constexpr float kEps = 1.0e-6f;
        return std::fabs(a.origin.x - b.origin.x) < kEps &&
               std::fabs(a.origin.y - b.origin.y) < kEps &&
               std::fabs(a.origin.z - b.origin.z) < kEps &&
               std::fabs(a.direction.x - b.direction.x) < kEps &&
               std::fabs(a.direction.y - b.direction.y) < kEps &&
               std::fabs(a.direction.z - b.direction.z) < kEps;
    };
    if (m_LastHoverRayValid && rayEqual(event.ray, m_LastHoverRay))
        return;
    m_LastHoverRay = event.ray;
    m_LastHoverRayValid = true;

    GameEngine::ECS::World* world = &GetWorld();
    if (!SceneTlasReadyForPick(*world, PickPurpose::Hover))
    {
        m_DeferredHover = event;
        m_LastHoverX = -1.0f; // the replay must not be throttled as the same position
        m_LastHoverY = -1.0f;
        m_LastHoverRayValid = false;
        return;
    }

    Editor::Picking::PickOptions options;
    options.WaitForSceneTlas = false; // ready, checked above
    // Hover highlights by drawing the entity's LocalBounds box, which terrain does not
    // carry — a hovered terrain would put a default 1 m box under the cursor everywhere
    // over the ground. Clicking still selects it; only the preview box is mesh-only.
    options.IncludeTerrain  = false;
    options.IncludeDisabled = revealHidden; // honor reveal-hidden modifier
    std::vector<Editor::Picking::PickHit> hits;
    GameEngine::Editor::Picking::RaycastSceneAll(event.ray, *world, hits, options);

    GameEngine::ECS::EntityHandle hoverEntity{};
    for (const auto& hit : hits)
    {
        // Hidden-via-ancestor filter (RaycastSceneAll already filtered the
        // entity itself when IncludeDisabled is false).
        if (!revealHidden && !GameEngine::ECS::Entity(world, hit.Entity).IsEnabledInHierarchy())
            continue;
        // Locked / locked-descendant filter — locked entities should be
        // transparent to hover so the user can hover entities behind them.
        if (!revealHidden && m_IsEntityPickable && !m_IsEntityPickable(hit.Entity))
            continue;
        hoverEntity = hit.Entity;
        break;
    }

    // Root-pick mode: highlight what a click would select, not the raw
    // submesh. Cached per raw entity — the resolve walks the hierarchy and
    // hover runs on every (throttled) mouse move.
    if (hoverEntity.IsValid() && !settings.GetExactPickMode())
    {
        if (hoverEntity != m_LastRawSceneHover)
        {
            m_LastRawSceneHover = hoverEntity;
            m_LastResolvedSceneHover = Editor::Picking::ResolvePickRoot(*world, hoverEntity);
        }
        hoverEntity = m_LastResolvedSceneHover;
    }

    // Icon hover (lights, probes): same unified ray test as clicking, honoring
    // the reveal-hidden modifier. Icon hits override mesh hover because the
    // icons render above scene geometry.
    if (GameEngine::ECS::EntityHandle icon = PickIconAtRay(event, revealHidden); icon.IsValid())
        hoverEntity = icon;

    SetHoverEntity(hoverEntity, revealHidden);
}

void SceneViewController::CycleEntityUnderCursor(float viewX, float viewY, float viewW, float viewH, int direction)
{
    GameEngine::ECS::World* world = &GetWorld();

    auto ray = MakeGizmoRay(viewX, viewY, viewW, viewH);
    // A scroll answers from the last finished scene TLAS and then requests the
    // next one, so it is dropped only while that work is in flight: requesting
    // first would start work, and so drop the scroll, whenever anything moved.
    // Hits are tested against live transforms; only the broad phase can lag
    // by one scroll.
    Scene::SceneTlas& tlas = Scene::GetSceneTlas(*world);
    if (tlas.CheckRequest(*world) != Scene::SceneTlas::Readiness::Ready)
        return; // a depth-cycle scroll while the scene TLAS refits is dropped, not queued

    UpdatePickCandidates(*world, ray);
    tlas.RequestCurrent(*world);
    const auto& candidates = m_PickCandidates;
    if (candidates.empty())
        return;

    // Find the current primary selection in the candidate list.
    GameEngine::ECS::EntityHandle current{};
    if (!m_SelectedEntities.empty())
        current = m_SelectedEntities.back();

    const auto currentIt = std::find(candidates.begin(), candidates.end(), current);
    const int currentIdx = currentIt == candidates.end()
        ? -1
        : static_cast<int>(std::distance(candidates.begin(), currentIt));

    // Already under the cursor: advance by direction, wrapping. Otherwise
    // start at the nearest candidate regardless of scroll direction.
    int nextIdx = 0;
    if (currentIdx >= 0)
        nextIdx = (currentIdx + direction + static_cast<int>(candidates.size())) % static_cast<int>(candidates.size());

    if (candidates[nextIdx] == current)
        return; // sole candidate already selected

    const int candidateCount = static_cast<int>(candidates.size());
    OnEntityPicked(candidates[nextIdx], false, false, /*skipPickRootResolve=*/true);
    m_DepthCycleAnchor = true;
    m_PickCycleIndex = nextIdx;
    m_PickCycleCount = candidateCount;

    // Keep hover highlight + name pill in sync with the cycled pick (scroll does not run hover raycasts).
    if (!m_SelectedEntities.empty())
        SetHoverEntity(m_SelectedEntities.back(), false);
}

void SceneViewController::TweenUpdate(float deltaSeconds, bool cameraMoved)
{
    if (!m_CameraTween.Active)
    {
        return;
    }

    if (cameraMoved)
    {
        OnCameraTweenFinish();
    }
    else
    {
        // Run Tween
        m_CameraTween.Elapsed += deltaSeconds;

        // Calculate Progress
        float rawT = std::clamp(m_CameraTween.Elapsed / m_CameraTween.Duration, 0.0f, 1.0f);
        float t = 1.0f - powf(1.0f - rawT, 3.0f); // ease-out -> Starting fast, slowing down near the end

        // Lerp position using t
        m_CamPos[0] = std::lerp(m_CameraTween.From.Pos[0], m_CameraTween.To.Pos[0], t);
        m_CamPos[1] = std::lerp(m_CameraTween.From.Pos[1], m_CameraTween.To.Pos[1], t);
        m_CamPos[2] = std::lerp(m_CameraTween.From.Pos[2], m_CameraTween.To.Pos[2], t);

        // Lerp orbit Distance
        m_CamDistance = std::lerp(m_CameraTween.From.Distance, m_CameraTween.To.Distance, t);

        // Lerp camera angle along the shortest arc. fmodf preserves the sign
        // of the dividend, so normalize into [-180, 180] explicitly.
        auto lerpAngle = [](const float StartingAngle, const float TargetAngle, const float Tt) -> float
        {
            float delta = fmodf(TargetAngle - StartingAngle, 360.0f);
            if (delta < -180.0f) delta += 360.0f;
            else if (delta > 180.0f) delta -= 360.0f;
            return StartingAngle + delta * Tt;
        };

        m_CamYawDeg = lerpAngle(m_CameraTween.From.YawDeg, m_CameraTween.To.YawDeg, t);
        m_CamPitchDeg = lerpAngle(m_CameraTween.From.PitchDeg, m_CameraTween.To.PitchDeg, t);

        // checking if done using rawT to avoid early finish because of easing curve
        if (rawT >= 1.0f)
        {
            OnCameraTweenFinish();
        }

        // Update SceneView after running tween
        UpdateCamera(0.0f, false, false, false, false);
    }
}

static Rendering::CameraData
BuildCameraDataFromPose(
    ECS::World* world, const SceneViewCameraPose& pose,
    float viewportWidth,
    float viewportHeight)
{
    using namespace GameEngine::Mathematics;

    // View matrix — same double-precision yaw/pitch build as the live Scene
    // View camera, so bookmark previews at planetary coordinates don't inherit
    // the MakeLookAtLH(eye, eye + forward) direction quantization.
    Matrix4x4 view;
    Editor::CameraRig::BuildViewMatrixLH(
        Editor::CameraRig::Vec3d{pose.Pos[0], pose.Pos[1], pose.Pos[2]},
        pose.YawDeg, pose.PitchDeg, view.Data());

    // Projection (match SceneView!)
    const float aspect =
        (viewportHeight > 0.0f)
            ? (viewportWidth / viewportHeight)
            : 1.0f;

    float nearClip = 0.1f;
    float farClip = 200.0f;
    GetSceneViewClipPlanes(world, nearClip, farClip);
    const float fovDeg = GetSceneViewFieldOfViewDeg();
    const float fovRad = ToRadians(fovDeg);
    Matrix4x4 proj = MakePerspectiveLH_ZO_ReverseZ(fovRad, aspect, nearClip, farClip);

    Matrix4x4 viewProj = proj * view;

    Rendering::CameraData cam{};
    memcpy(cam.view, view.Data(), sizeof(float) * 16);
    memcpy(cam.proj, proj.Data(), sizeof(float) * 16);
    memcpy(cam.viewProj, viewProj.Data(), sizeof(float) * 16);

    return cam;
}

bool SceneViewController::DeclarePreviewTargetsRG(
    Rendering::RenderGraph::RGFrame& frame, uint64_t windowId,
    Engine::Renderer::Pipeline::ViewTargetsRG& outTargets)
{
    namespace RenderGraph = Rendering::RenderGraph;
    auto* rs = m_RenderServices;
    if (!rs || !rs->GetDevice() || m_PreviewViewId == 0)
        return false;

    // m_PreviewSubmittedThisFrame is consumed by Cancel/Freeze; reset before
    // the pending gate so a non-rendering frame never reports a submission.
    m_PreviewSubmittedThisFrame = false;
    m_PreviewHadDrawItemsThisFrame = false;

    const bool previewPending = (m_PreviewPendingSerial > m_PreviewLastSubmittedSerial);
    if (!m_RequestPreview && !previewPending)
        return false;

    if (m_PreviewCameraId != 0)
        rs->Views().SetViewCamera(m_PreviewViewId, m_PreviewCameraId);
    // Arm the OnDemand preview view for this frame: RequestViewFrame feeds
    // extraction, batch-key build, and the culling loop once, then the effective
    // arm (ActiveRenderLayerMask) auto-expires. The configured RenderLayerMask
    // stays 1 so the arm selects render layer 0; a frame we stop declaring the
    // preview simply isn't armed and the view lapses to dormant.
    rs->Views().RequestViewFrame(m_PreviewViewId);
    rs->Views().SetViewRenderLayerMask(m_PreviewViewId, 1u);
    m_PreviewSubmittedThisFrame = true;

    // Always clear (deliberate divergence from the old arm's one-shot
    // ClearNext): a pool-imported target can be a FRESH physical on ANY
    // re-import after idle age-out, and Load-on-undefined is never safe. The
    // preview fully re-renders every pending frame anyway.
    {
        Rendering::ViewClearConfig clear{};
        clear.clearColor = true;
        const uint32_t bg = Editor::SceneViewSettings::Get().GetBackgroundColor();
        clear.clearColorValue[0] = static_cast<float>((bg >> 16) & 0xFFu) / 255.0f;
        clear.clearColorValue[1] = static_cast<float>((bg >> 8) & 0xFFu) / 255.0f;
        clear.clearColorValue[2] = static_cast<float>(bg & 0xFFu) / 255.0f;
        clear.clearColorValue[3] = 1.0f;
        clear.clearDepth = true;
        clear.clearDepthValue = 0.0f;
        rs->Views().SetViewClearConfig(m_PreviewViewId, clear);
    }

    // Lighting UBO, same as the old Record path — the preview is its own
    // view; without this the sky renders but lit meshes sample zeroed data.
    rs->WriteViewLightBuffer(m_PreviewViewId);

    // Preview must match the world pipelines' sample count (single-sampled
    // attachments against 4x MSAA pipelines assert in SetPipeline).
    uint32_t samples = rs->GetDefaultMSAASampleCount();
    if (samples != 1u && samples != 2u && samples != 4u && samples != 8u)
        samples = 4u;

    char nameBuf[96];
    Rendering::TextureDesc td{};
    td.width = 128;
    td.height = 72;
    td.depth = 1;
    td.mipLevels = 1;
    td.arrayLayers = 1;

    RenderGraph::RGTexture msaaColor{};
    if (samples > 1u)
    {
        std::snprintf(nameBuf, sizeof(nameBuf), "Editor.W%llu.SceneView.BookmarkPreview.ColorMsaa",
                      static_cast<unsigned long long>(windowId));
        td.sampleCount = samples;
        td.format = static_cast<uint32_t>(Rendering::TextureFormat::R16G16B16A16_FLOAT);
        td.usage = static_cast<uint32_t>(Rendering::TextureUsage::RenderTarget);
        td.debugName = "BookmarkPreview.ColorMsaa";
        msaaColor = frame.ImportPersistentTexture(nameBuf, td);
    }

    std::snprintf(nameBuf, sizeof(nameBuf), "Editor.W%llu.SceneView.BookmarkPreview.Color",
                  static_cast<unsigned long long>(windowId));
    td.sampleCount = 1;
    td.format = static_cast<uint32_t>(Rendering::TextureFormat::R16G16B16A16_FLOAT);
    // TransferSrc: FinalizePreviewRG copies this 1-sample target into the
    // frozen snapshot the popup samples on later frames. Same flag as SceneView.Color.
    td.usage = static_cast<uint32_t>(Rendering::TextureUsage::RenderTarget) |
               static_cast<uint32_t>(Rendering::TextureUsage::ShaderResource) |
               static_cast<uint32_t>(Rendering::TextureUsage::TransferSrc);
    td.debugName = "BookmarkPreview.Color";
    const RenderGraph::RGTexture color = frame.ImportPersistentTexture(nameBuf, td);

    std::snprintf(nameBuf, sizeof(nameBuf), "Editor.W%llu.SceneView.BookmarkPreview.Depth",
                  static_cast<unsigned long long>(windowId));
    td.sampleCount = samples;
    // Reverse-Z requires float depth; ShaderResource so Forward+ samples it.
    td.format = static_cast<uint32_t>(Rendering::TextureFormat::D32_FLOAT);
    td.usage = static_cast<uint32_t>(Rendering::TextureUsage::DepthStencil) |
               static_cast<uint32_t>(Rendering::TextureUsage::ShaderResource);
    td.debugName = "BookmarkPreview.Depth";
    const RenderGraph::RGTexture depth = frame.ImportPersistentTexture(nameBuf, td);

    if (!color.IsValid() || !depth.IsValid() || (samples > 1u && !msaaColor.IsValid()))
        return false;

    outTargets.View = m_PreviewViewId;
    outTargets.Color = (samples > 1u) ? msaaColor : color;
    outTargets.Depth = depth;
    if (samples > 1u)
        outTargets.Resolve = color;

    if (previewPending)
    {
        const auto keys = rs->GetEntityBatchKeys(m_PreviewViewId);
        m_PreviewHadDrawItemsThisFrame = !keys.empty();
        if (!m_PreviewHadDrawItemsThisFrame)
            ++m_PreviewNoDrawRetryFrames;
    }

    if (m_RequestPreview)
    {
        m_RequestPreview = false;
        const CameraData cam = BuildCameraDataFromPose(&GetWorld(), m_PreviewPose, 128.0f, 72.0f);
        if (m_PreviewCameraId != 0)
        {
            rs->Views().SetCameraData(m_PreviewCameraId, cam);
            // Participate in per-view post-process filtering.
            rs->Views().SetCameraPostProcessMask(m_PreviewCameraId, 0xFFFFFFFFu);
        }
    }

    // Serial close — in immediate mode this declaration IS the submission
    // (the spine declares the preview pipeline for this entry right after).
    // The freeze + snapshot copy run from the post-spine half this frame;
    // the view deactivates next frame via FreezePreviewViewIfPending. Cold
    // PSO compiles can eat the first draws: exhaustion closes regardless (a
    // grey thumbnail self-heals on re-hover; a stuck-open serial used to pin
    // the whole window off the RenderGraph arm).
    constexpr int kMaxPreviewNoDrawRetryFrames = 16;
    if (previewPending &&
        (m_PreviewHadDrawItemsThisFrame ||
         m_PreviewNoDrawRetryFrames >= kMaxPreviewNoDrawRetryFrames))
    {
        m_PreviewLastSubmittedSerial = m_PreviewPendingSerial;
        m_PreviewNoDrawRetryFrames = 0;
        m_PreviewPendingFreeze = true;
        m_PreviewSnapshotPending = true;
    }
    return true;
}

void SceneViewController::FinalizePreviewRG(Rendering::RenderGraph::RGFrame& frame)
{
    namespace RenderGraph = Rendering::RenderGraph;
    auto* rs = m_RenderServices;
    if (!rs || m_PreviewViewId == 0)
        return;
    Rendering::IDevice* device = rs->GetDevice();
    if (!device)
        return;

    // Widget screenshot tickets only on frames where the preview rendered —
    // the pipeline output value is frame-local and only exists post-spine.
    if (m_BookmarksWidget && m_PreviewSubmittedThisFrame)
        m_BookmarksWidget->ProcessPendingScreenshotRequestsRG(frame);

    if (!m_PreviewSnapshotPending)
        return;
    m_PreviewSnapshotPending = false;

    const auto out = rs->GetPipelineOutputRG(frame, m_PreviewViewId);
    if (!out.IsValid())
    {
        static bool sWarnedMissing = false;
        if (!sWarnedMissing)
        {
            Logger::Log::Warning(
                "[SceneView] bookmark preview snapshot skipped: view {} has no FinalColor",
                static_cast<uint32_t>(m_PreviewViewId));
            sWarnedMissing = true;
        }
        return;
    }
    const RenderGraph::RGTexture src = out.Out;
    // The preview targets are 128×72 1-sample; a mismatched output cannot
    // CopyTexture into the frozen snapshot (extent/format must match).
    {
        const auto& sd = frame.Graph().ResourceDesc(src.Id);
        if (sd.SampleCount > 1 || sd.Width != 128 || sd.Height != 72)
        {
            static bool sWarned = false;
            if (!sWarned)
            {
                Logger::Log::Warning(
                    "[SceneView] bookmark preview snapshot skipped: pipeline output "
                    "{}x{} fmt={} samples={} is not copy-compatible with the 128x72 snapshot",
                    sd.Width, sd.Height, sd.Format, sd.SampleCount);
                sWarned = true;
            }
            return;
        }
    }

    // The copy, the frozen texture and its space stamp (#767) all live in the
    // device-scoped snapshot: the popup samples the result descriptor-direct on
    // LATER frames, so the handle outlives this frame and a device rebuild in
    // between must not hand a freed id back to the bind.
    m_PreviewSnapshot.Update(frame, *device, src, rs->GetPipelineOutputSpaceRG(),
                             "SceneView.BookmarkPreview.Snapshot");
}

void SceneViewController::StartCameraTween(const SceneViewCameraPose& target, const float duration)
{
    m_CameraTween.From = GetCameraPose();
    m_CameraTween.To = target;
    m_CameraTween.Duration = duration > 0.0f ? duration : 0.7f;
    m_CameraTween.Elapsed = 0.0f;
    m_CameraTween.Active = true;
}

void SceneViewController::OnCameraTweenFinish()
{
    m_CameraTween.Active = false;

    SetCameraPose(m_CameraTween.To);

    // After a transition into 2D mode, snap the ortho height to the pixel-perfect
    // target so we don't inherit the tween's end distance when pixel-perfect is on.
    ApplyPixelPerfectIfActive();
}

namespace
{
bool CameraPoseApproxEqual(const SceneViewCameraPose& a, const SceneViewCameraPose& b)
{
    constexpr float kPosEpsSq = 1e-4f;   // ~1cm combined
    constexpr float kAngleEps = 0.25f;   // degrees
    constexpr float kDistEps = 1e-3f;
    const float dx = a.Pos[0] - b.Pos[0];
    const float dy = a.Pos[1] - b.Pos[1];
    const float dz = a.Pos[2] - b.Pos[2];
    if (dx * dx + dy * dy + dz * dz > kPosEpsSq) return false;
    if (std::fabs(a.YawDeg - b.YawDeg) > kAngleEps) return false;
    if (std::fabs(a.PitchDeg - b.PitchDeg) > kAngleEps) return false;
    if (std::fabs(a.Distance - b.Distance) > kDistEps) return false;
    if (a.Is2D != b.Is2D) return false;
    return true;
}
} // namespace

void SceneViewController::PushCameraHistorySnapshot(const SceneViewCameraPose& pose)
{
    constexpr std::size_t kMaxEntries = 32;
    auto& h = m_CameraHistory;

    if (h.Cursor >= 0 && h.Cursor < static_cast<int>(h.Entries.size())
        && CameraPoseApproxEqual(h.Entries[h.Cursor], pose))
    {
        return;
    }

    if (h.Cursor + 1 < static_cast<int>(h.Entries.size()))
        h.Entries.erase(h.Entries.begin() + (h.Cursor + 1), h.Entries.end());

    h.Entries.push_back(pose);
    if (h.Entries.size() > kMaxEntries)
    {
        const std::size_t drop = h.Entries.size() - kMaxEntries;
        h.Entries.erase(h.Entries.begin(), h.Entries.begin() + drop);
    }
    h.Cursor = static_cast<int>(h.Entries.size()) - 1;
}

void SceneViewController::UpdateCameraHistoryTracking(float deltaSeconds)
{
    if (m_CameraTween.Active)
        return;

    constexpr float kStableThresholdSeconds = 0.4f;

    auto& h = m_CameraHistory;
    const SceneViewCameraPose pose = GetCameraPose();

    if (!h.HasLastStable || !CameraPoseApproxEqual(h.LastStable, pose))
    {
        h.LastStable = pose;
        h.HasLastStable = true;
        h.StableSeconds = 0.0f;
        return;
    }

    h.StableSeconds += deltaSeconds;
    if (h.StableSeconds < kStableThresholdSeconds)
        return;

    if (h.SuppressAutoPush)
    {
        // A history-driven tween just finished; treat its destination as the new
        // cursor without appending a duplicate entry.
        h.SuppressAutoPush = false;
        h.StableSeconds = 0.0f;
        return;
    }

    PushCameraHistorySnapshot(pose);
    h.StableSeconds = 0.0f;
}

void SceneViewController::StartHistoryTweenTo(int targetIndex)
{
    auto& h = m_CameraHistory;
    if (targetIndex < 0 || targetIndex >= static_cast<int>(h.Entries.size()))
        return;

    // Capture the pose we're leaving so the user can forward-navigate back to it.
    const SceneViewCameraPose current = GetCameraPose();
    if (h.Cursor >= 0 && h.Cursor < static_cast<int>(h.Entries.size())
        && !CameraPoseApproxEqual(h.Entries[h.Cursor], current))
    {
        PushCameraHistorySnapshot(current);
        // PushCameraHistorySnapshot may have truncated the forward chain; re-resolve.
        if (targetIndex >= static_cast<int>(h.Entries.size()))
            return;
    }

    h.Cursor = targetIndex;
    h.SuppressAutoPush = true;
    h.HasLastStable = false;
    h.StableSeconds = 0.0f;
    StartCameraTween(h.Entries[targetIndex], 0.25f);
}

bool SceneViewController::StepCameraHistoryBack()
{
    if (IsTweenActive())
        return false;
    auto& h = m_CameraHistory;
    if (h.Entries.empty())
        return false;

    const SceneViewCameraPose current = GetCameraPose();
    int target = h.Cursor - 1;
    // If the current pose drifted from the cursor entry, the back step should
    // snap to the cursor first (since we'll append current as cursor+1).
    if (h.Cursor >= 0 && h.Cursor < static_cast<int>(h.Entries.size())
        && !CameraPoseApproxEqual(h.Entries[h.Cursor], current))
    {
        target = h.Cursor;
    }
    if (target < 0)
        return false;
    StartHistoryTweenTo(target);
    return true;
}

bool SceneViewController::StepCameraHistoryForward()
{
    if (IsTweenActive())
        return false;
    auto& h = m_CameraHistory;
    if (h.Entries.empty())
        return false;
    const int target = h.Cursor + 1;
    if (target >= static_cast<int>(h.Entries.size()))
        return false;
    StartHistoryTweenTo(target);
    return true;
}

void SceneViewController::Toggle2DMode()
{
    // Don't allow re-toggle while a transition is still playing — let it finish.
    if (IsTweenActive())
        return;

    constexpr float kTransitionDuration = 0.2f;

    if (!m_Is2DMode)
    {
        // Entering 2D: save current pose, tween to orthographic side view.
        // Don't set m_Is2DMode yet — keep perspective projection during the
        // tween so the transition is visible. OnCameraTweenFinish will apply
        // the target pose which sets m_Is2DMode = true via SetCameraPose.
        m_Saved3DPose = GetCameraPose();
        m_Saved3DPose.Is2D = false;

        SceneViewCameraPose target = m_Saved3DPose;
        target.YawDeg = 90.0f;
        target.PitchDeg = 0.0f;
        target.Is2D = true;
        StartCameraTween(target, kTransitionDuration);
    }
    else
    {
        // Leaving 2D: switch to perspective immediately so the transition
        // renders with depth, then tween back to the saved 3D pose.
        m_Is2DMode = false;
        Sync2DModeToTransformTool();
        m_Saved3DPose.Is2D = false;

        StartCameraTween(m_Saved3DPose, kTransitionDuration);
    }
}

void SceneViewController::Set2DMode(bool is2D)
{
    if (is2D == m_Is2DMode)
        return;

    if (is2D)
    {
        // Entering 2D: save current 3D pose so Toggle can restore it later.
        m_Saved3DPose = GetCameraPose();
    }
    m_Is2DMode = is2D;
    Sync2DModeToTransformTool();
}

void SceneViewController::Sync2DModeToTransformTool()
{
    if (m_TransformTool)
    {
        m_TransformTool->SetIs2DMode(m_Is2DMode);
    }
}

void SceneViewController::ApplyPixelPerfectIfActive()
{
    if (!m_Is2DMode)
        return;
    if (!Editor::SceneViewSettings::Get().GetPixelPerfect2D())
        return;
    const uint32_t effectiveH = SceneViewEffectiveViewportHeight(&GetWorld(),
        m_LastW,
        m_LastH,
        m_Is2DMode || m_Orthographic);
    if (effectiveH == 0)
        return;

    // In 2D mode, m_CamDistance is the ortho visible height in world units.
    // One world unit maps to one source texel, so pixelScale screen pixels cover
    // one world unit: scale 1 is true 1:1 (one texel = one pixel), scale 2 is 2x,
    // and so on.
    int scale = Editor::SceneViewSettings::Get().GetPixelPerfectScale();
    if (scale < 1) scale = 1;
    if (scale > 64) scale = 64;
    const float newHeight = static_cast<float>(effectiveH) / static_cast<float>(scale);
    if (newHeight > 0.0f)
        m_CamDistance = newHeight;
}

void SceneViewController::RefreshPixelPerfect2D()
{
    ApplyPixelPerfectIfActive();
}

void SceneViewController::SetOrthoHeight2D(float requestedHeight)
{
    if (requestedHeight <= 0.0f)
        return;

    auto& settings = Editor::SceneViewSettings::Get();
    const uint32_t effectiveH = SceneViewEffectiveViewportHeight(&GetWorld(),
        m_LastW,
        m_LastH,
        m_Is2DMode || m_Orthographic);
    if (settings.GetPixelPerfect2D() && effectiveH > 0)
    {
        // Visible height = effectiveH / scale. Pick the largest power-of-2 scale
        // (most zoomed in) that still fits the requested height.
        const float ratio = static_cast<float>(effectiveH) / requestedHeight;
        int snapped = 1;
        while (snapped * 2 <= static_cast<int>(ratio) && snapped < 64)
            snapped *= 2;
        if (snapped < 1) snapped = 1;
        if (snapped > 64) snapped = 64;
        settings.SetPixelPerfectScale(snapped);
        ApplyPixelPerfectIfActive();
        return;
    }

    m_CamDistance = requestedHeight;
}

bool SceneViewController::ZoomPixelPerfectAtCursor(int scaleDelta, float /*localX*/, float /*localY*/, float viewW, float viewH)
{
    if (!m_Is2DMode || !Editor::SceneViewSettings::Get().GetPixelPerfect2D())
        return false;
    if (scaleDelta == 0 || viewW <= 0.0f || viewH <= 0.0f)
        return false;

    auto& settings = Editor::SceneViewSettings::Get();
    const int oldScale = settings.GetPixelPerfectScale();
    // Each scroll step doubles (zoom in) or halves (zoom out) the pixel scale,
    // so the sequence is 1x → 2x → 4x → 8x → 16x → 32x → 64x.
    int newScale = oldScale;
    if (scaleDelta > 0)
    {
        for (int i = 0; i < scaleDelta; ++i)
            newScale *= 2;
    }
    else if (scaleDelta < 0)
    {
        for (int i = 0; i < -scaleDelta; ++i)
            newScale = (newScale > 1) ? (newScale / 2) : 1;
    }
    if (newScale < 1) newScale = 1;
    if (newScale > 64) newScale = 64;
    if (newScale == oldScale)
        return false;

    // Apply the new scale and recompute ortho height. In pixel-perfect mode the
    // zoom is anchored to the current screen center — m_CamPos is left alone so
    // the world point at the center of the viewport stays put after the scale
    // change, regardless of where the cursor is.
    settings.SetPixelPerfectScale(newScale);
    ApplyPixelPerfectIfActive();

    // Keep the orbit pivot in front of the camera along the look axis.
    m_OrbitPivot[0] = m_CamPos[0];
    m_OrbitPivot[1] = m_CamPos[1];
    m_OrbitPivot[2] = m_CamPos[2] + m_CamDistance;

    return true;
}

void SceneViewController::SetTransformMode(Editor::SceneTools::TransformMode mode)
{
    if (!m_TransformTool)
        return;
    if (mode != Editor::SceneTools::TransformMode::Translate)
        ClearMeasureEndpointTransformTarget();
    m_TransformTool->SetMode(mode);

    // Switching to a transform mode also activates the transform tool if not already active.
    if (m_ActiveToolKind != ToolKind::Transform)
        SetActiveTool(ToolKind::Transform);
}

Editor::SceneTools::TransformMode SceneViewController::GetTransformMode() const
{
    if (!m_TransformTool)
        return Editor::SceneTools::TransformMode::Translate;
    return m_TransformTool->GetMode();
}

void SceneViewController::ToggleTransformSpace()
{
    if (!m_TransformTool)
        return;
    using Space = Editor::SceneTools::TransformAxisSpace;
    const bool isLocal = m_TransformTool->GetAxisSpace() == Space::Local;
    m_TransformTool->SetAxisSpace(isLocal ? Space::World : Space::Local);
}

bool SceneViewController::IsTransformSpaceLocal() const
{
    if (!m_TransformTool)
        return false;
    return m_TransformTool->GetAxisSpace() == Editor::SceneTools::TransformAxisSpace::Local;
}

bool SceneViewController::SceneTlasReadyForPick(ECS::World& world, PickPurpose purpose)
{
    Scene::SceneTlas& tlas = Scene::GetSceneTlas(world);
    bool request = !m_ResolvingDeferredPicks;
    if (request && purpose == PickPurpose::Hover)
    {
        const auto now = std::chrono::steady_clock::now();
        request = now - m_LastHoverTlasRequest >= kHoverTlasRefresh;
        if (request)
            m_LastHoverTlasRequest = now;
    }
    const Scene::SceneTlas::Readiness readiness = request ? tlas.RequestCurrent(world) : tlas.CheckRequest(world);
    return readiness == Scene::SceneTlas::Readiness::Ready;
}

void SceneViewController::ResolveDeferredPicks()
{
    if (!m_DeferredClick && !m_DeferredHover)
        return;
    ECS::World& world = GetWorld();
    if (Scene::GetSceneTlas(world).CheckRequest(world) != Scene::SceneTlas::Readiness::Ready)
        return;
    m_ResolvingDeferredPicks = true;
    if (std::optional<Editor::SceneTools::ScenePointerEvent> click = std::exchange(m_DeferredClick, std::nullopt))
        PickViaClickThrough(*click, /*clearOnMiss=*/true);
    if (std::optional<Editor::SceneTools::ScenePointerEvent> hover = std::exchange(m_DeferredHover, std::nullopt))
        HandleHoverDetection(*hover);
    m_ResolvingDeferredPicks = false;
}

void SceneViewController::Update(float deltaSeconds, bool moveForward, bool moveBackward, bool moveLeft, bool moveRight,
                                 bool moveUp, bool moveDown, float speedMultiplier)
{
    ResolveDeferredPicks();

    // Camera integration happens in the caller's dedicated UpdateCamera call,
    // which runs after this (post-orbit, post-tween) so per-frame ordering is
    // explicit. Running it here too double-stepped the fly camera each frame
    // (2x speed, halved easing constant).

    const bool interactiveTransform =
        m_TransformTool && m_TransformTool->IsInteractiveEditActive();

    // Signal an in-progress transform-gizmo drag to the terrain modifier system so
    // it coalesces the expensive per-frame re-bake of a dragged modifier/zone and
    // settles once on release (TerrainService::SetInteractiveModifierEdit). Runs
    // before the engine world update, so the flag is current when the modifier
    // system reads it this frame.
    //
    // This line writes its source unconditionally EVERY frame from the gizmo's
    // live state, which is why the signal is source-keyed rather than one bool:
    // the Inspector's own drag lifecycle holds InteractiveEditSource::InspectorDrag,
    // and a shared bool would have this frame's write clear it.
    if (auto* terrainService = TerrainECS::TerrainService::TryGet())
        terrainService->SetInteractiveModifierEdit(
            TerrainECS::TerrainService::InteractiveEditSource::TransformGizmo,
            interactiveTransform);

    // Update Tween if called & SceneView camera not moving
    TweenUpdate(deltaSeconds,
                moveForward || moveBackward || moveLeft || moveRight || moveUp || moveDown);

    // Track camera-pose stability so [ / ] history can auto-record waypoints.
    UpdateCameraHistoryTracking(deltaSeconds);

    // Keep gizmo pivot following the selected entity even when transforms are
    // updated by runtime systems (e.g., PhysicsWriteback) that don't emit editor
    // change notifications.
    if (m_TransformTool)
    {
        // In 2D mode, snap to the adaptive grid step so snapping matches
        // the visible grid lines regardless of zoom level.
        float effectiveSnapSize = m_GridSnapSize;
        if (m_Is2DMode && m_GridSnapEnabled)
        {
            constexpr int kTargetLines = 40;
            float raw = m_CamDistance / static_cast<float>(kTargetLines);
            float decade = std::pow(10.0f, std::floor(std::log10(raw)));
            float mantissa = raw / decade;
            if (mantissa < 1.5f)       effectiveSnapSize = decade;
            else if (mantissa < 3.5f)  effectiveSnapSize = decade * 2.0f;
            else if (mantissa < 7.5f)  effectiveSnapSize = decade * 5.0f;
            else                        effectiveSnapSize = decade * 10.0f;
            if (effectiveSnapSize < 0.001f) effectiveSnapSize = 0.001f;
        }
        m_TransformTool->SetGridSnap(m_GridSnapEnabled, effectiveSnapSize);

        if (m_TransformTool->GetTargetEntity().IsValid() &&
            !m_TransformTool->IsInteractiveEditActive())
        {
            m_TransformTool->RefreshPivotFromTargetEntity();
        }
    }
}

void SceneViewController::UpdateCamera(float deltaSeconds, bool moveForward, bool moveBackward, bool moveLeft,
                                       bool moveRight, bool moveUp, bool moveDown, float speedMultiplier)
{
    if (deltaSeconds <= 0.0f)
        return;

    // Fly momentum must not fight navigation that owns the position outright:
    // orbit pins the camera to a sphere and tweens/bookmarks lerp it, so any
    // residual eased velocity is dropped the moment one of them takes over.
    if (m_IsOrbiting || IsTweenActive())
        ResetFlyVelocity();

    // Derive a look direction from yaw/pitch. This is the direction from the camera
    // towards what it's looking at (used by the view matrix via MakeLookAtLH).
    float yawR = m_CamYawDeg * 3.1415926535f / 180.0f;
    float pitR = m_CamPitchDeg * 3.1415926535f / 180.0f;
    float look[3] = {
        std::cos(pitR) * std::cos(yawR),
        std::sin(pitR),
        std::cos(pitR) * std::sin(yawR)};
    Normalize3(look);

    // Movement forward is along +look so that "W" matches the visual forward direction
    // in our left-handed (+Z forward) convention.
    float forwardMove[3] = {look[0], look[1], look[2]};

    // Horizontal right vector derived from the horizontal projection of the look direction
    // (A/D strafe without changing height).
    float lookFlat[3] = {look[0], 0.0f, look[2]};
    float lenFlat = lookFlat[0] * lookFlat[0] + lookFlat[2] * lookFlat[2];
    if (lenFlat > 1e-6f)
    {
        float invLen = 1.0f / std::sqrt(lenFlat);
        lookFlat[0] *= invLen;
        lookFlat[2] *= invLen;
    }
    else
    {
        // Looking straight up/down: fall back to a sensible horizontal forward
        lookFlat[0] = 0.0f;
        lookFlat[2] = 1.0f;
    }
    // Right vector consistent with a left-handed basis where +Z is forward:
    // right = normalize(cross(worldUp, forwardFlat)). With forwardFlat = (lookFlat.x, 0, lookFlat.z)
    // and worldUp = (0,1,0), this yields (lookFlat.z, 0, -lookFlat.x).
    float right[3] = {lookFlat[2], 0.0f, -lookFlat[0]};

    float moveDir[3] = {0.0f, 0.0f, 0.0f};
    if (moveForward)
    {
        moveDir[0] += forwardMove[0];
        moveDir[1] += forwardMove[1];
        moveDir[2] += forwardMove[2];
    }
    if (moveBackward)
    {
        moveDir[0] -= forwardMove[0];
        moveDir[1] -= forwardMove[1];
        moveDir[2] -= forwardMove[2];
    }
    if (moveRight)
    {
        moveDir[0] += right[0];
        moveDir[2] += right[2];
    }
    if (moveLeft)
    {
        moveDir[0] -= right[0];
        moveDir[2] -= right[2];
    }
    // World vertical, not camera-relative: Q/E must raise/lower the eye even
    // when the look direction is pitched, matching WASD's flattened strafe.
    if (moveUp)
        moveDir[1] += 1.0f;
    if (moveDown)
        moveDir[1] -= 1.0f;

    // Rest thresholds for the eased fly velocity (squared magnitudes).
    constexpr float kMoveInputEpsSq = 1e-6f;
    constexpr float kVelocityRestEpsSq = 1e-4f; // ~0.01 u/s — imperceptible coast

    float targetVel[3] = {0.0f, 0.0f, 0.0f};
    const float inputLenSq = moveDir[0] * moveDir[0] + moveDir[1] * moveDir[1] + moveDir[2] * moveDir[2];
    if (inputLenSq > kMoveInputEpsSq)
    {
        Normalize3(moveDir);
        m_MoveSpeed = GetSceneViewMoveSpeed();
        const float speed = m_MoveSpeed * speedMultiplier;
        targetVel[0] = moveDir[0] * speed;
        targetVel[1] = moveDir[1] * speed;
        targetVel[2] = moveDir[2] * speed;
    }

    // Ease the velocity toward the input direction with the acceleration-time
    // setting as an exponential time constant; 0 keeps the legacy instant response.
    const float accelTime = GetSceneViewMoveAccelerationTime();
    if (accelTime <= 0.0f)
    {
        m_CamVelocity[0] = targetVel[0];
        m_CamVelocity[1] = targetVel[1];
        m_CamVelocity[2] = targetVel[2];
    }
    else
    {
        const float blend = 1.0f - std::exp(-deltaSeconds / accelTime);
        m_CamVelocity[0] += (targetVel[0] - m_CamVelocity[0]) * blend;
        m_CamVelocity[1] += (targetVel[1] - m_CamVelocity[1]) * blend;
        m_CamVelocity[2] += (targetVel[2] - m_CamVelocity[2]) * blend;
    }

    // Snap the exponential tail to zero once input has stopped so the camera
    // comes to a true rest instead of drifting by ever-smaller steps.
    const float velLenSq = m_CamVelocity[0] * m_CamVelocity[0] +
                           m_CamVelocity[1] * m_CamVelocity[1] +
                           m_CamVelocity[2] * m_CamVelocity[2];
    if (inputLenSq <= kMoveInputEpsSq && velLenSq < kVelocityRestEpsSq)
        ResetFlyVelocity();

    // Integrate in double: at planetary coordinates an fp32 += would round
    // sub-ULP steps (e.g. 4 mm at |pos| 5e4) to zero and freeze slow movement.
    m_CamPos[0] += static_cast<double>(m_CamVelocity[0]) * deltaSeconds;
    m_CamPos[1] += static_cast<double>(m_CamVelocity[1]) * deltaSeconds;
    m_CamPos[2] += static_cast<double>(m_CamVelocity[2]) * deltaSeconds;

    // Update shared camera data early (in Update) so that ECS systems like
    // RenderGraphBuildSystem (which runs culling) see the current frame's camera
    // position. Using the previous frame's viewport size for aspect ratio is
    // acceptable here to avoid 1-frame culling lag.
    if (m_CameraId != 0)
    {
        float viewportW = static_cast<float>(m_LastW > 0 ? m_LastW : 1);
        float viewportH = static_cast<float>(m_LastH > 0 ? m_LastH : 1);
        float aspect = viewportW / viewportH;
        if (const auto sceneAspect = ResolveEditorSceneViewCameraAspect(&GetWorld(),
                m_LastW, m_LastH, m_Is2DMode || m_Orthographic))
        {
            aspect = sceneAspect->projectionAspect;
        }

        // Build a left-handed view/projection consistent with the runtime
        // CameraSystem (MakeLookAtLH layout + MakePerspectiveLH_ZO). This keeps
        // culling, world rendering and gizmo depth testing in strict agreement.
        // The basis comes straight from yaw/pitch in double: the old
        // MakeLookAtLH(eye, eye + look) round-trip re-derived the direction as
        // (eye+look)-eye in fp32, which quantizes the rotation basis to
        // ULP(|eye|)-sized angular steps at planetary coordinates (~0.22 deg at
        // |eye| 5e4) — the scene-view rotation jitter at planet scale.
        Matrix4x4 viewM;
        Editor::CameraRig::BuildViewMatrixLH(
            Editor::CameraRig::Vec3d{m_CamPos[0], m_CamPos[1], m_CamPos[2]},
            m_CamYawDeg, m_CamPitchDeg, viewM.Data());

        float nearClip = 0.1f;
        float farClip = 200.0f;
        GetSceneViewClipPlanes(&GetWorld(), nearClip, farClip);

        Matrix4x4 projM;
        if (m_Is2DMode || m_Orthographic)
        {
            const float halfH = m_CamDistance * 0.5f;
            const float halfW = halfH * aspect;
            projM = GameEngine::Mathematics::MakeOrthographicLH_ZO_ReverseZ(
                -halfW, halfW, -halfH, halfH, nearClip, farClip);
        }
        else
        {
            const float fovDeg = GetSceneViewFieldOfViewDeg();
            const float fovRad = fovDeg * 3.1415926535f / 180.0f;
            projM = GameEngine::Mathematics::MakePerspectiveLH_ZO_ReverseZ(
                fovRad, aspect, nearClip, farClip);
        }
        Matrix4x4 viewProjM = projM * viewM;

        CameraData cam{};
        const float* viewSrc = viewM.Data();
        const float* projSrc = projM.Data();
        const float* viewProjSrc = viewProjM.Data();
        for (int i = 0; i < 16; ++i)
        {
            cam.view[i] = viewSrc[i];
            cam.proj[i] = projSrc[i];
            cam.viewProj[i] = viewProjSrc[i];
        }
        cam.cameraPos[0] = static_cast<float>(m_CamPos[0]);
        cam.cameraPos[1] = static_cast<float>(m_CamPos[1]);
        cam.cameraPos[2] = static_cast<float>(m_CamPos[2]);
        cam.cameraPos[3] = m_Is2DMode ? 1.0f : 0.0f;
        if (m_RenderServices)
        {
            m_RenderServices->Views().SetCameraData(m_CameraId, cam);
            // Register every frame alongside SetCameraData (before rendering systems run),
            // so RenderExtractionSystem's per-view post-process path sees HasCameraPostProcessMask true
            // in the same phase as world/culling — mask was previously only applied in Record.
            m_RenderServices->Views().SetCameraPostProcessMask(m_CameraId, 0xFFFFFFFFu);
            m_RenderServices->Views().SetCameraExposure(m_CameraId, SceneViewLensExposure(&GetWorld()));
            if (m_ViewId != 0)
            {
                if (const auto sceneAspect = ResolveEditorSceneViewCameraAspect(&GetWorld(),
                        m_LastW, m_LastH, m_Is2DMode || m_Orthographic))
                {
                    m_RenderServices->Views().SetViewLetterbox(m_ViewId, sceneAspect->letterbox);
                }
                else
                {
                    m_RenderServices->Views().SetViewLetterbox(m_ViewId, {});
                }
            }
        }
    }

    // Note: Gizmo sizes for constant screen-space mode are computed
    // directly in each gizmo's Render() function using the camera position
    // from GizmoRenderContext, eliminating any frame delay.
}

void SceneViewController::UpdateOrbit(bool orbiting)
{
    if (!orbiting || m_Is2DMode || m_FixedViewOrientation != FixedViewOrientation::Free)
    {
        m_IsOrbiting = false;
        return;
    }

    // Orbit runs entirely in double: recomputing the camera from the pivot in
    // fp32 would land the position on the ULP grid of |pos| every frame
    // (~4 mm at 5e4, ~0.5 m at Earth radius) — visible parallax snapping when
    // orbiting close to a surface at planetary coordinates.
    using Editor::CameraRig::Vec3d;
    const Vec3d camPos{m_CamPos[0], m_CamPos[1], m_CamPos[2]};

    if (!m_IsOrbiting)
    {
        // First orbit frame: choose a pivot some distance along the forward
        // direction (the same direction we use for "W" movement).
        const Vec3d pivot = Editor::CameraRig::OrbitPivotFromCamera(
            camPos, m_CamYawDeg, m_CamPitchDeg, m_CamDistance);
        m_OrbitPivot[0] = pivot.X;
        m_OrbitPivot[1] = pivot.Y;
        m_OrbitPivot[2] = pivot.Z;
        m_IsOrbiting = true;
        // Do not move the camera on the first frame to avoid a visible jump.
        return;
    }

    // Subsequent frames: keep the camera on a sphere around the pivot at m_CamDistance.
    const Vec3d pivot{m_OrbitPivot[0], m_OrbitPivot[1], m_OrbitPivot[2]};
    const Vec3d newCam = Editor::CameraRig::OrbitCameraPosition(
        pivot, m_CamYawDeg, m_CamPitchDeg, m_CamDistance);
    m_CamPos[0] = newCam.X;
    m_CamPos[1] = newCam.Y;
    m_CamPos[2] = newCam.Z;
}

void SceneViewController::UpdatePan(float deltaX, float deltaY, float viewportHeightPx)
{
    ResetFlyVelocity();
    // Pan camera horizontally and vertically (strafe) based on mouse delta.
    // Convert pixel deltas to world-space movement scaled by distance to pivot.
    // Runs in double: at planetary coordinates fp32 += rounds sub-ULP steps to
    // zero (pan sticks/steps near a planet surface; dead entirely at Earth
    // radius for steps under ~0.25 m).
    using Editor::CameraRig::Vec3d;
    Vec3d right{};
    Vec3d up{};
    Editor::CameraRig::PanBasis(m_CamYawDeg, m_CamPitchDeg, right, up);

    // Perspective keeps the tuned distance multiplier. Orthographic panning
    // needs exact viewport scaling so the world point under the cursor stays
    // locked while dragging: visibleHeight / inputViewportHeight.
    double panSpeed = static_cast<double>(m_CamDistance) * 0.002;
    if ((m_Is2DMode || m_Orthographic) && viewportHeightPx > 1.0f)
        panSpeed = static_cast<double>(m_CamDistance) / viewportHeightPx;

    // Move camera and orbit pivot together (camera-relative pan)
    const double moveX = -static_cast<double>(deltaX) * panSpeed;
    const double moveY = static_cast<double>(deltaY) * panSpeed;

    const double dx = right.X * moveX + up.X * moveY;
    const double dy = right.Y * moveX + up.Y * moveY;
    const double dz = right.Z * moveX + up.Z * moveY;

    m_CamPos[0] += dx;
    m_CamPos[1] += dy;
    m_CamPos[2] += dz;

    m_OrbitPivot[0] += dx;
    m_OrbitPivot[1] += dy;
    m_OrbitPivot[2] += dz;
}

bool SceneViewController::ZoomOrthographicStep(int direction)
{
    if (!m_Orthographic || m_Is2DMode || direction == 0) return false;
    uint32_t rasterHeight = SceneViewEffectiveViewportHeight(&GetWorld(), m_LastW, m_LastH, true);
    if (rasterHeight == 0) return false;
    const double next = Editor::CameraRig::StepOrthographicPixelZoom(m_CamDistance, rasterHeight, direction);
    if (next < GetSceneViewMinOrbitDistance(&GetWorld()) ||
        (next > Editor::SceneViewSettings::Get().GetMaxDistance() && next > m_CamDistance)) return false;
    ResetFlyVelocity();
    m_CameraTween.Active = false;
    m_CamDistance = static_cast<float>(next);
    // Orthographic zoom changes the visible height without moving the camera
    // or its screen-center ray, so depth-band distances and panning stay put.
    const auto pivot = Editor::CameraRig::OrbitPivotFromCamera(
        {m_CamPos[0], m_CamPos[1], m_CamPos[2]}, m_CamYawDeg, m_CamPitchDeg, m_CamDistance);
    m_OrbitPivot[0] = pivot.X; m_OrbitPivot[1] = pivot.Y; m_OrbitPivot[2] = pivot.Z;
    return true;
}

void SceneViewController::UpdateDolly(float deltaY)
{
    ResetFlyVelocity();
    // Dolly camera (zoom in/out) by adjusting the distance to the orbit pivot.
    // Moving mouse up (negative deltaY) zooms in; down (positive deltaY) zooms out.

    // Pixel-perfect 2D scale cycling is handled by SceneViewPanel's scroll
    // handler via ZoomPixelPerfectAtCursor() so the cursor world-point can
    // stay anchored while the scale changes.
    if (m_Is2DMode && Editor::SceneViewSettings::Get().GetPixelPerfect2D())
        return;

    constexpr float kDollySpeed = 0.01f;
    constexpr float kDollyReferenceDistance = 0.5f;
    const float dollyScale = std::max(m_CamDistance, kDollyReferenceDistance);

    if (m_Is2DMode || m_Orthographic)
    {
        const float prevDistance = m_CamDistance;
        m_CamDistance += deltaY * kDollySpeed * dollyScale;

        const float minDist = GetSceneViewMinOrbitDistance(&GetWorld());
        if (m_CamDistance < minDist)
            m_CamDistance = minDist;
        const float maxDist = Editor::SceneViewSettings::Get().GetMaxDistance();
        // Never clamp BELOW the distance we started the scroll at: framing a
        // planet-sized object legitimately puts the rig beyond MaxDistance, and
        // snapping to the setting would teleport the camera tens of km forward
        // on the first wheel tick. Beyond the cap, zooming in is free and
        // zooming out just holds.
        if (m_CamDistance > maxDist)
            m_CamDistance = std::max(maxDist, std::min(m_CamDistance, prevDistance));
        return;
    }

    // If we have not orbited yet, the pivot is still at default (origin). Establish it from
    // the current camera so the first scroll just zooms in place instead of snapping the view.
    using Editor::CameraRig::Vec3d;
    if (!m_IsOrbiting)
    {
        const Vec3d pivot = Editor::CameraRig::OrbitPivotFromCamera(
            Vec3d{m_CamPos[0], m_CamPos[1], m_CamPos[2]},
            m_CamYawDeg, m_CamPitchDeg, m_CamDistance);
        m_OrbitPivot[0] = pivot.X;
        m_OrbitPivot[1] = pivot.Y;
        m_OrbitPivot[2] = pivot.Z;
    }

    // Scale zoom by distance, but keep a reference floor so the wheel does not
    // crawl to a stop well above the minimum (multiplicative-only damping).
    const float prevDistance = m_CamDistance;
    m_CamDistance += deltaY * kDollySpeed * dollyScale;

    // Clamp distance to reasonable range (user-configurable max; min matches near clip)
    const float minDist = GetSceneViewMinOrbitDistance(&GetWorld());
    if (m_CamDistance < minDist)
        m_CamDistance = minDist;
    {
        using GameEngine::Editor::SceneViewSettings;
        const float maxDist = SceneViewSettings::Get().GetMaxDistance();
        // No snap below the entry distance — see the orthographic branch above.
        if (m_CamDistance > maxDist)
            m_CamDistance = std::max(maxDist, std::min(m_CamDistance, prevDistance));
    }

    // Update camera position to maintain view of the orbit pivot at the new
    // distance (double: fp32 would quantize the position to the ULP grid of
    // |pos| — visible stepping at planetary coordinates).
    const Vec3d newCam = Editor::CameraRig::OrbitCameraPosition(
        Vec3d{m_OrbitPivot[0], m_OrbitPivot[1], m_OrbitPivot[2]},
        m_CamYawDeg, m_CamPitchDeg, m_CamDistance);
    m_CamPos[0] = newCam.X;
    m_CamPos[1] = newCam.Y;
    m_CamPos[2] = newCam.Z;
}

void SceneViewController::FrameOrigin()
{
    // Frame the selected entity if one exists, otherwise frame world origin
    Mathematics::Vector3 target(0.0f, 0.0f, 0.0f);
    float frameDistance = m_CamDistance;
    bool framedToolSelection = false;
    bool framedMeasureEndpoint = false;

    // A registered tool's selection finer than an entity (a spline knot) frames first.
    for (const Editor::SceneViewToolStripEntry& entry : Editor::SceneViewToolStripRegistry::Get().Entries())
    {
        Mathematics::Vector3 toolTarget{};
        if (entry.FrameTarget && entry.FrameTarget(GetWorld(), toolTarget))
        {
            target = toolTarget;
            frameDistance = 2.0f;
            framedToolSelection = true;
            break;
        }
    }

    if (!framedToolSelection && GetSelectedMeasureEndpointWorldPosition(target))
    {
        frameDistance = 2.0f;
        framedMeasureEndpoint = true;
    }

    if (m_TransformTool)
    {
        auto targetEntity = m_TransformTool->GetTargetEntity();
        if (targetEntity.IsValid() && !framedToolSelection && !framedMeasureEndpoint)
        {
            // Get the entity's world transform position
            auto* world = &GetWorld();
            if (world->IsValid(targetEntity))
            {
                Mathematics::Matrix4x4 targetWorld = Mathematics::Matrix4x4::Identity();
                if (GetEntityWorldMatrixForMeasure(world, targetEntity, targetWorld))
                {
                    // Extract position from world transform matrix
                    target = Mathematics::Vector3(targetWorld[3].x, targetWorld[3].y, targetWorld[3].z);

                    // Frame the bounds union of the whole subtree: multi-submesh
                    // model roots carry no LocalBounds of their own, so framing
                    // only the root's bounds would zoom inside the model.
                    Editor::SubtreeWorldBounds subtree;
                    if (Editor::ComputeSubtreeWorldBounds(*world, targetEntity, subtree) && subtree.HasBounds)
                    {
                        frameDistance = std::max(subtree.Radius() * 1.8f, 0.5f);

                        target = subtree.Center();
                    }
                    else
                    {
                        // No bounds, zoom in to a reasonable close distance
                        frameDistance = 3.0f;
                    }
                }
            }
        }
    }

    // Place the camera to look at the target from the current viewing direction
    float yawR = m_CamYawDeg * 3.1415926535f / 180.0f;
    float pitR = m_CamPitchDeg * 3.1415926535f / 180.0f;
    const Mathematics::Vector3 dir = Mathematics::Vector3(std::cos(pitR) * std::cos(yawR),
                                                          std::sin(pitR),
                                                          std::cos(pitR) * std::sin(yawR)).NormalizeOrZero();

    if (m_Is2DMode || m_Orthographic)
    {
        // In orthographic modes, m_CamDistance controls the visible height.
        // Set it so the framed object fits within the viewport with padding.
        const float aspect = (m_LastH > 0) ? static_cast<float>(m_LastW) / static_cast<float>(m_LastH) : 1.0f;
        float orthoH = frameDistance * 2.0f; // diameter → visible height
        // If the object is wider than tall relative to the viewport, fit by width.
        if (aspect > 0.001f)
            orthoH = std::max(orthoH, (frameDistance * 2.0f) / aspect);
        if (m_Is2DMode)
        {
            SetOrthoHeight2D(orthoH * 1.2f); // snaps to pixel-perfect step when active
        }
        else
        {
            const float minDist = GetSceneViewMinOrbitDistance(&GetWorld());
            const float maxDist = Editor::SceneViewSettings::Get().GetMaxDistance();
            m_CamDistance = std::clamp(orthoH * 1.2f, minDist, maxDist);
        }
    }
    else
    {
        m_CamDistance = frameDistance;
    }

    // The camera position and orbit pivot are stored in double precision.
    m_CamPos[0] = target.x - dir.x * m_CamDistance;
    m_CamPos[1] = target.y - dir.y * m_CamDistance;
    m_CamPos[2] = target.z - dir.z * m_CamDistance;

    m_OrbitPivot[0] = target.x;
    m_OrbitPivot[1] = target.y;
    m_OrbitPivot[2] = target.z;
}

void SceneViewController::FrameAll()
{
    // Frame all objects in the scene by calculating a bounding box that encompasses everything
    using GameEngine::Components::LocalBounds;
    using GameEngine::Components::WorldTransform;

    auto* world = &GetWorld();

    // Calculate combined bounds
    float minBounds[3] = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    float maxBounds[3] = {std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest()};
    bool foundAny = false;

    // Iterate through all entities that have LocalBounds and compute world AABBs
    auto archetypes = world->GetAllArchetypes();
    for (auto* arch : archetypes)
    {
        if (!arch)
            continue;

        auto entities = arch->CollectEntities();
        for (const auto& entity : entities)
        {
            const LocalBounds* bounds = world->GetComponent<LocalBounds>(entity);
            if (!bounds || bounds->Box.RadiusSquared() <= 0.0f)
                continue;

            const auto* worldXf = world->GetComponent<WorldTransform>(entity);
            if (!worldXf)
                continue;

            const auto aabb = bounds->Box.TransformToAABB(worldXf->matrix);

            minBounds[0] = std::min(minBounds[0], aabb.min.x);
            minBounds[1] = std::min(minBounds[1], aabb.min.y);
            minBounds[2] = std::min(minBounds[2], aabb.min.z);

            maxBounds[0] = std::max(maxBounds[0], aabb.max.x);
            maxBounds[1] = std::max(maxBounds[1], aabb.max.y);
            maxBounds[2] = std::max(maxBounds[2], aabb.max.z);

            foundAny = true;
        }
    }

    if (!foundAny)
    {
        // No objects found, frame the world origin
        FrameOrigin();
        return;
    }

    // Calculate center of combined bounds
    float target[3] = {
        (minBounds[0] + maxBounds[0]) * 0.5f,
        (minBounds[1] + maxBounds[1]) * 0.5f,
        (minBounds[2] + maxBounds[2]) * 0.5f};

    // Calculate the radius of the bounding sphere that encompasses the box
    float extent[3] = {
        (maxBounds[0] - minBounds[0]) * 0.5f,
        (maxBounds[1] - minBounds[1]) * 0.5f,
        (maxBounds[2] - minBounds[2]) * 0.5f};
    float radius = std::sqrt(extent[0] * extent[0] + extent[1] * extent[1] + extent[2] * extent[2]);

    // Set frame distance to show all objects with some padding
    float frameDistance = std::max(radius * 2.0f, 1.0f);

    // Place the camera to look at the target from the current viewing direction
    float yawR = m_CamYawDeg * 3.1415926535f / 180.0f;
    float pitR = m_CamPitchDeg * 3.1415926535f / 180.0f;
    float dir[3] = {
        std::cos(pitR) * std::cos(yawR),
        std::sin(pitR),
        std::cos(pitR) * std::sin(yawR)};
    Normalize3(dir);

    if (m_Is2DMode || m_Orthographic)
    {
        // In orthographic modes, m_CamDistance is the visible height.
        // Use the scene extent projected onto the screen axes for best fit.
        const float aspect = (m_LastH > 0) ? static_cast<float>(m_LastW) / static_cast<float>(m_LastH) : 1.0f;
        const float worldUp[3] = {0.0f, 1.0f, 0.0f};
        float right[3];
        Cross3(worldUp, dir, right);
        Normalize3(right);

        float up[3];
        Cross3(dir, right, up);
        Normalize3(up);

        const float halfW = std::abs(right[0]) * extent[0] + std::abs(right[1]) * extent[1] + std::abs(right[2]) * extent[2];
        const float halfH = std::abs(up[0]) * extent[0] + std::abs(up[1]) * extent[1] + std::abs(up[2]) * extent[2];
        float orthoH = std::max(halfH * 2.0f, 1.0f);
        if (aspect > 0.001f)
            orthoH = std::max(orthoH, (halfW * 2.0f) / aspect);
        if (m_Is2DMode)
        {
            SetOrthoHeight2D(orthoH * 1.2f); // snaps to pixel-perfect step when active
        }
        else
        {
            const float minDist = GetSceneViewMinOrbitDistance(&GetWorld());
            const float maxDist = Editor::SceneViewSettings::Get().GetMaxDistance();
            m_CamDistance = std::clamp(orthoH * 1.2f, minDist, maxDist);
        }
    }
    else
    {
        m_CamDistance = frameDistance;
    }

    m_CamPos[0] = target[0] - dir[0] * m_CamDistance;
    m_CamPos[1] = target[1] - dir[1] * m_CamDistance;
    m_CamPos[2] = target[2] - dir[2] * m_CamDistance;

    m_OrbitPivot[0] = target[0];
    m_OrbitPivot[1] = target[1];
    m_OrbitPivot[2] = target[2];
}

void SceneViewController::PopulatePointerCameraState(Editor::SceneTools::ScenePointerEvent& ev) const
{
    ev.cameraPos = GetCameraWorldPos();

    const float yawR = m_CamYawDeg * 3.1415926535f / 180.0f;
    const float pitR = m_CamPitchDeg * 3.1415926535f / 180.0f;

    using Mathematics::Vector3;
    ev.cameraForward = Vector3(std::cos(pitR) * std::cos(yawR),
                               std::sin(pitR),
                               std::cos(pitR) * std::sin(yawR)).NormalizeOrZero();
    ev.cameraRight = Vector3::Cross(Vector3(0.0f, 1.0f, 0.0f), ev.cameraForward).NormalizeOrZero();
    ev.cameraUp = Vector3::Cross(ev.cameraForward, ev.cameraRight).NormalizeOrZero();

    if (m_Is2DMode || m_Orthographic)
    {
        ev.tanHalfFovY = 0.0f;
        ev.orthoHeight = m_CamDistance;
    }
    else
    {
        const float fovYDeg = GetSceneViewFieldOfViewDeg();
        ev.tanHalfFovY = std::tan(fovYDeg * 3.1415926535f / 360.0f);
        ev.orthoHeight = 0.0f;
    }
}

Editor::SceneTools::GizmoRay SceneViewController::MakeGizmoRay(float viewX, float viewY, float viewWidth, float viewHeight) const
{
    using namespace Editor::SceneTools;

    GizmoRay ray{};

    // Always provide a sensible origin based on the current editor camera.
    ray.origin.x = static_cast<float>(m_CamPos[0]);
    ray.origin.y = static_cast<float>(m_CamPos[1]);
    ray.origin.z = static_cast<float>(m_CamPos[2]);

    // Fallback: if the viewport has no meaningful size, just shoot along the view direction.
    if (viewWidth <= 0.0f || viewHeight <= 0.0f)
    {
        float yawR = m_CamYawDeg * 3.1415926535f / 180.0f;
        float pitR = m_CamPitchDeg * 3.1415926535f / 180.0f;
        ray.direction = Mathematics::Vector3(std::cos(pitR) * std::cos(yawR),
                                             std::sin(pitR),
                                             std::cos(pitR) * std::sin(yawR)).NormalizeOrZero();
        return ray;
    }

    // Convert from view-local pixels to normalized device coordinates in [-1, 1].
    const float ndcX = (2.0f * viewX) / viewWidth - 1.0f;
    const float ndcY = 1.0f - (2.0f * viewY) / viewHeight; // UI Y-down -> NDC Y-up

    const float aspect = (viewHeight > 0.0f) ? (viewWidth / viewHeight) : 1.0f;

    // Build a world-space camera basis from yaw/pitch (matches UpdateCamera).
    const float yawR = m_CamYawDeg * 3.1415926535f / 180.0f;
    const float pitR = m_CamPitchDeg * 3.1415926535f / 180.0f;

    using Mathematics::Vector3;
    const Vector3 forward = Vector3(std::cos(pitR) * std::cos(yawR),
                                    std::sin(pitR),
                                    std::cos(pitR) * std::sin(yawR)).NormalizeOrZero();
    const Vector3 right = Vector3::Cross(Vector3(0.0f, 1.0f, 0.0f), forward).NormalizeOrZero();
    const Vector3 up = Vector3::Cross(forward, right).NormalizeOrZero();

    if (m_Is2DMode || m_Orthographic)
    {
        // Orthographic ray: all rays are parallel (same direction = camera forward).
        // The origin varies across the viewport based on the ortho half-extents.
        const float halfH = m_CamDistance * 0.5f;
        const float halfW = halfH * aspect;

        // Offset in double so the sub-ULP viewport offsets survive at
        // planetary coordinates; a single fp32 rounding on store.
        const Vector3 offset = right * ndcX * halfW + up * ndcY * halfH;
        ray.origin.x = static_cast<float>(m_CamPos[0] + static_cast<double>(offset.x));
        ray.origin.y = static_cast<float>(m_CamPos[1] + static_cast<double>(offset.y));
        ray.origin.z = static_cast<float>(m_CamPos[2] + static_cast<double>(offset.z));

        ray.direction = forward;
    }
    else
    {
        const float fovYDeg = GetSceneViewFieldOfViewDeg();
        const float tanHalfFovY = std::tan(fovYDeg * 3.1415926535f / 360.0f);

        // View-space ray direction (camera at origin looking down +Z).
        const float tanHalfFovX = tanHalfFovY * aspect;
        const Vector3 dirCamera = Vector3(ndcX * tanHalfFovX, ndcY * tanHalfFovY, 1.0f).NormalizeOrZero();

        // Transform view-space ray into world space.
        ray.direction = (right * dirCamera.x + up * dirCamera.y + forward * dirCamera.z).NormalizeOrZero();
    }

    return ray;
}

void SceneViewController::SchedulePipelinePreWarm()
{
    if (m_PipelinePreWarmScheduled)
        return;
    if (!m_RenderServices)
        return;
    Rendering::IDevice* device = m_RenderServices->GetDevice();
    if (!device)
        return;
    if (!GameEngine::EngineCore::GetInstance().IsInitialized())
        return;

    // Load the SPV bytecodes up front on the calling thread. Both loaders
    // cache statically, so this is cheap after the first call and avoids
    // touching the disk from a worker thread for the one-shot pre-warm.
    std::vector<uint8_t> vsStaticBytes;
    std::vector<uint8_t> fsBytes;
    if (!LoadSelectionMaskShaderBytes(vsStaticBytes, fsBytes))
        return;
    std::vector<uint8_t> vsSkinnedBytes;
    if (!LoadSelectionMaskSkinnedVertexBytes(vsSkinnedBytes))
        return;

    m_PipelinePreWarmScheduled = true;

    auto preWarm =
        [device,
         vsStatic = std::move(vsStaticBytes),
         fs = std::move(fsBytes),
         vsSkinned = std::move(vsSkinnedBytes)]() mutable
        {
            using namespace Rendering;

            const auto t0 = std::chrono::high_resolution_clock::now();

            // Shared settings that match the runtime pipeline creation in
            // maskExecute. The mask target is R8_UNORM, single-sample, no
            // depth — hashing to those exact values is required for the
            // runtime GetOrCreatePipelineVariant to hit the cache.
            constexpr uint32_t kMaskColorFormat = static_cast<uint32_t>(TextureFormat::R8_UNORM);

            // --- Static variant ---
            PipelineDesc pdStatic{};
            pdStatic.type = PipelineType::Graphics;
            pdStatic.vertexShader = vsStatic;
            pdStatic.pixelShader = fs;
            pdStatic.debugName = "SV_Selection_Mask";
            pdStatic.topology = PrimitiveTopology::TriangleList;
            pdStatic.AddDynamicState(DynamicState::Viewport);
            pdStatic.AddDynamicState(DynamicState::Scissor);
            pdStatic.colorAttachmentFormats.push_back(kMaskColorFormat);
            pdStatic.depthAttachmentFormat = 0;
            pdStatic.rasterizationSamples = 1;
            pdStatic.SetCullingMode(CullModeFlagBits::None);
            pdStatic.EnableDepthTest(false, CompareOp::Always);
            pdStatic.EnableBlending(false);
            {
                VertexInputBinding vb0{};
                vb0.binding = VertexBinding::Core;
                vb0.stride = 32u;
                vb0.inputRate = 0;
                pdStatic.vertexBindings.push_back(vb0);
                VertexInputAttribute a0{};
                a0.location = VertexLocation::Position;
                a0.binding = VertexBinding::Core;
                a0.format = Format::R32G32B32_FLOAT;
                a0.offset = 0;
                pdStatic.vertexAttributes.push_back(a0);
            }
            {
                PipelineDesc::PushConstantRangeDesc pr{};
                pr.offset = 0;
                pr.size = 128u; // must match MaskPC + selection_mask*.vert (SceneViewOverlaysRG.cpp)
                pr.stagesMask = kShaderStageVertex;
                pdStatic.pushConstantRanges = {pr};
            }
            const auto staticId = InternEditorGraphicsId(*device, pdStatic);
            const auto staticFk = EditorFormatKey(pdStatic);
            const bool okStatic = device->PrewarmGraphicsPipeline(staticId, {&staticFk, 1}) > 0
                                  || device->GetOrCreateGraphicsPipeline(staticId, staticFk).IsValid();

            // --- Skinned variant ---
            PipelineDesc pdSkinned{};
            pdSkinned.type = PipelineType::Graphics;
            pdSkinned.vertexShader = vsSkinned;
            pdSkinned.pixelShader = fs;
            pdSkinned.debugName = "SV_Selection_Mask_Skinned";
            pdSkinned.topology = PrimitiveTopology::TriangleList;
            pdSkinned.AddDynamicState(DynamicState::Viewport);
            pdSkinned.AddDynamicState(DynamicState::Scissor);
            pdSkinned.colorAttachmentFormats.push_back(kMaskColorFormat);
            pdSkinned.depthAttachmentFormat = 0;
            pdSkinned.rasterizationSamples = 1;
            pdSkinned.SetCullingMode(CullModeFlagBits::None);
            pdSkinned.EnableDepthTest(false, CompareOp::Always);
            pdSkinned.EnableBlending(false);
            {
                VertexInputBinding vb0{};
                vb0.binding = VertexBinding::Core;
                vb0.stride = 32u;
                vb0.inputRate = 0;
                pdSkinned.vertexBindings.push_back(vb0);
                VertexInputAttribute a0{};
                a0.location = VertexLocation::Position;
                a0.binding = VertexBinding::Core;
                a0.format = Format::R32G32B32_FLOAT;
                a0.offset = 0;
                pdSkinned.vertexAttributes.push_back(a0);
            }
            {
                VertexInputBinding vbJ{};
                vbJ.binding = VertexBinding::Joints;
                vbJ.stride = 4u * sizeof(uint16_t);
                vbJ.inputRate = 0;
                pdSkinned.vertexBindings.push_back(vbJ);
                VertexInputAttribute aJ{};
                aJ.location = VertexLocation::Joints;
                aJ.binding = VertexBinding::Joints;
                aJ.format = Format::R16G16B16A16_UINT;
                aJ.offset = 0;
                pdSkinned.vertexAttributes.push_back(aJ);
            }
            {
                VertexInputBinding vbW{};
                vbW.binding = VertexBinding::Weights;
                vbW.stride = 4u * sizeof(float);
                vbW.inputRate = 0;
                pdSkinned.vertexBindings.push_back(vbW);
                VertexInputAttribute aW{};
                aW.location = VertexLocation::Weights;
                aW.binding = VertexBinding::Weights;
                aW.format = Format::R32G32B32A32_FLOAT;
                aW.offset = 0;
                pdSkinned.vertexAttributes.push_back(aW);
            }
            {
                DescriptorSetLayoutDesc set0{};
                set0.debugName = "SV_Selection_Mask_Skinned_Set0";
                DescriptorBinding b{};
                b.binding = 12; // BonePaletteAtlas SSBO
                b.type = DescriptorType::StorageBuffer;
                b.count = 1;
                b.shaderStages = kShaderStageVertex;
                set0.bindings.push_back(b);
                pdSkinned.descriptorSetLayouts.push_back(set0);
            }
            {
                PipelineDesc::PushConstantRangeDesc pr{};
                pr.offset = 0;
                pr.size = 128u; // must match MaskPC + selection_mask*.vert (SceneViewOverlaysRG.cpp)
                pr.stagesMask = kShaderStageVertex;
                pdSkinned.pushConstantRanges = {pr};
            }
            const auto skinnedId = InternEditorGraphicsId(*device, pdSkinned);
            const auto skinnedFk = EditorFormatKey(pdSkinned);
            const bool okSkinned = device->PrewarmGraphicsPipeline(skinnedId, {&skinnedFk, 1}) > 0
                                   || device->GetOrCreateGraphicsPipeline(skinnedId, skinnedFk).IsValid();

            const double ms = std::chrono::duration<double, std::milli>(
                std::chrono::high_resolution_clock::now() - t0).count();
            Logger::Log::Info(
                "[SelectionMaskPreWarm] static={} skinned={} in {:.1f}ms",
                okStatic ? 1 : 0, okSkinned ? 1 : 0, ms);
        };

    // Compiling these pipelines off the frame thread is the point of the
    // pre-warm. A device that confines its GPU objects to the thread that made
    // them cannot be touched from a worker at all, so there the warm-up runs
    // here and pays its milliseconds up front instead.
    if (!device->GetCapabilities().supportsMultithreadedResourceCreation)
    {
        preWarm();
        return;
    }
    GameEngine::EngineCore::GetInstance().GetJobSystem().Submit(std::move(preWarm));
}

bool SceneViewController::DeclareTargetsRG(Rendering::RenderGraph::RGFrame& frame, uint32_t width,
                                            uint32_t height, uint64_t windowId,
                                            Engine::Renderer::Pipeline::ViewTargetsRG& outTargets)
{
    GE_CPU_PROFILE_SCOPE("SceneView.DeclareTargetsRG");
    outTargets = {};
    m_WaitingForExtractionRG = false;

    Engine::Renderer::RenderServices* rs = m_RenderServices;
    if (!rs || !rs->GetDevice())
        return false;

    // Consume a pending preview freeze on RenderGraph frames too: the confirm runs
    // in old-arm Record, but the window is back on RenderGraph by the next frame —
    // without this the preview view stays active and the old retained graph
    // renders it every frame (the post-hover FPS leak).
    FreezePreviewViewIfPending(rs);

    // Old-arm→RenderGraph handoff for THIS view: an old-arm excursion (bookmark
    // preview / screenshot / thumbnail frames) re-published registry targets
    // via SetViewTargetsFromRefs, and the old graph's retained world pass
    // activates on them — without the one-shot clear, the full old Forward+
    // pipeline keeps rendering the main view on top of the RenderGraph spine after
    // every excursion. NEVER touch the WorldDrawBuilder here (the 7d
    // batch-key-wipe lesson).
    if (m_OldArmTargetsPublished && m_ViewId != 0)
    {
        rs->Views().ClearViewTargets(m_ViewId);
        // Targets only deactivate the world pass; the excursion's retained
        // PIPELINE passes (CSM cascades, sky, post-FX) and the frame-scope
        // GPU-driven passes (culling slices, bucketer, visibility union)
        // have no liveness predicate and would keep executing forever —
        // burning frame time and clobbering the pooled shadow array +
        m_OldArmTargetsPublished = false;
    }

    const uint32_t w = width > 0 ? width : 1;
    const uint32_t h = height > 0 ? height : 1;
    if (w != m_LastW || h != m_LastH)
    {
        m_LastW = w;
        m_LastH = h;
        ApplyPixelPerfectIfActive();
    }
    SchedulePipelinePreWarm();

    // AA policy: the engine-wide mode picks between MSAA samples and TAA
    // (mutually exclusive by construction — TAA renders single-sample). A
    // layout override still asks for cheaper MSAA. Quad panes declare here
    // too (8e-3) — each pane runs the full pipeline at its own extent.
    const auto aa = rs->ResolveAntiAliasing(0u, 0u);
    uint32_t samples = aa.SampleCount;
    if (m_RenderSampleCountOverride != 0)
        samples = m_RenderSampleCountOverride;
    if (samples != 1u && samples != 2u && samples != 4u && samples != 8u)
        samples = 4u;
    // TAA and TemporalFXAA take the jittered single-sample path; FXAA and SMAA
    // register the same per-view AA state so their nodes can gate on the mode,
    // but stay unjittered (ViewRegistry zeroes their offsets). MSAA/off and
    // the fixed 2D/ortho views register nothing.
    const bool registerViewAA =
        Engine::Renderer::UsesPerViewAntiAliasingState(aa.Mode) && samples == 1u &&
        m_FixedViewOrientation == FixedViewOrientation::Free && !m_Is2DMode && !m_Orthographic;

    // View + camera registry bookkeeping — the same RenderServices calls as
    // the old arm (graph-agnostic).
    if (m_CameraId == 0)
        m_CameraId = rs->Views().AllocateCamera("Scene View Camera");
    if (m_ViewId == 0)
        m_ViewId = rs->Views().AllocateView("Scene View", m_CameraId, Rendering::ViewPurpose::EditorScene,
                                    Rendering::ViewParticipation::OnDemand);
    else
        rs->Views().SetViewCamera(m_ViewId, m_CameraId);
    // Arm this OnDemand view for the frame. A controller that stops declaring
    // its pane (quad view toggled off, a collapsed/hidden pane, an inactive
    // split) then lapses to dormant within two frames: its GPU culling,
    // extraction, and shadow dispatches stop without releasing the view, so its
    // HZB visibility history (keyed by viewId) survives and re-declaring revives
    // it at once.
    rs->Views().RequestViewFrame(m_ViewId);
    rs->Views().SetViewRenderLayerMask(m_ViewId, 1u);
    rs->Views().SetViewActiveRenderPipeline(m_ViewId, true);
    rs->Views().SetViewAntiAliasing(m_ViewId, registerViewAA, aa.Mode, rs->GetTaaSequenceLength());
    rs->Views().SetViewWorldId(m_ViewId, GetWorld().GetWorldId());
    // Two-phase HZB occlusion is DEFAULT-ON for the free (perspective) scene
    // view (gates passed 2026-07-08: interiors −32.5% GPU, zero false culls
    // open-field — PR #253); GE_HZB_OCCLUSION=0 is the kill switch back to
    // frustum-only. Never for the fixed-orthographic 2D view (the main-view
    // HZB doesn't apply to an ortho frustum), and never for thumbnails /
    // probes / GameView (those controllers set their own strategy). Read
    // once; the strategy is stateless (prevVisible history lives in
    // GPUCullingPipeline keyed by viewId).
    static const bool kHzbOcclusionEnabled = []
    {
        if (const char* env = std::getenv("GE_HZB_OCCLUSION"))
            return env[0] != '0';
        return true;
    }();
    if (m_FixedViewOrientation != FixedViewOrientation::Free)
        rs->Views().SetViewCullingStrategy(m_ViewId, FixedOrthographicSceneViewCullingStrategy());
    else if (kHzbOcclusionEnabled)
        rs->Views().SetViewCullingStrategy(m_ViewId,
                                   std::make_shared<Rendering::HzbCullingStrategy>());
    else
        rs->Views().SetViewCullingStrategy(m_ViewId, nullptr);

    // Authoritative CameraData from the exact viewport dims — identical math
    // to the old arm so both arms produce the same projection.
    const auto sceneAspect =
        ResolveEditorSceneViewCameraAspect(&GetWorld(), w, h, m_Is2DMode || m_Orthographic);
    {
        float viewportW = static_cast<float>(w);
        float viewportH = static_cast<float>(h);
        float aspect = viewportW / viewportH;
        if (sceneAspect)
            aspect = sceneAspect->projectionAspect;

        // Double-precision view build — identical math to UpdateCamera so both
        // arms produce the same matrices (see the rotation-jitter note there).
        Matrix4x4 viewM;
        Editor::CameraRig::BuildViewMatrixLH(
            Editor::CameraRig::Vec3d{m_CamPos[0], m_CamPos[1], m_CamPos[2]},
            m_CamYawDeg, m_CamPitchDeg, viewM.Data());

        float nearClip = 0.1f;
        float farClip = 200.0f;
        GetSceneViewClipPlanes(&GetWorld(), nearClip, farClip);

        Matrix4x4 projM;
        if (m_Is2DMode || m_Orthographic)
        {
            const float halfH = m_CamDistance * 0.5f;
            const float halfW = halfH * aspect;
            projM = GameEngine::Mathematics::MakeOrthographicLH_ZO_ReverseZ(
                -halfW, halfW, -halfH, halfH, nearClip, farClip);
        }
        else
        {
            const float fovDeg = GetSceneViewFieldOfViewDeg();
            const float fovRad = fovDeg * 3.1415926535f / 180.0f;
            projM = GameEngine::Mathematics::MakePerspectiveLH_ZO_ReverseZ(fovRad, aspect,
                                                                           nearClip, farClip);
        }
        Matrix4x4 viewProjM = projM * viewM;

        CameraData cam{};
        const float* viewSrc = viewM.Data();
        const float* projSrc = projM.Data();
        const float* viewProjSrc = viewProjM.Data();
        for (int i = 0; i < 16; ++i)
        {
            cam.view[i] = viewSrc[i];
            cam.proj[i] = projSrc[i];
            cam.viewProj[i] = viewProjSrc[i];
        }
        cam.cameraPos[0] = static_cast<float>(m_CamPos[0]);
        cam.cameraPos[1] = static_cast<float>(m_CamPos[1]);
        cam.cameraPos[2] = static_cast<float>(m_CamPos[2]);
        cam.cameraPos[3] = m_Is2DMode ? 1.0f : 0.0f;

        m_FrameCameraData = cam;
        m_FrameCameraViewportW = w;
        m_FrameCameraViewportH = h;
        m_HasFrameCameraData = true;

        rs->Views().SetCameraData(m_CameraId, cam);
        rs->Views().SetCameraPostProcessMask(m_CameraId, 0xFFFFFFFFu);
        rs->Views().SetCameraExposure(m_CameraId, SceneViewLensExposure(&GetWorld()));
    }

    // Letterbox + clear publish. RenderGraph targets are VALUES (below) — the
    // registry record carries only the clear config.
    if (sceneAspect)
        rs->Views().SetViewLetterbox(m_ViewId, sceneAspect->letterbox);
    else
        rs->Views().SetViewLetterbox(m_ViewId, {});

    Rendering::ViewClearConfig clear{};
    clear.clearColor = true;
    if (sceneAspect && sceneAspect->letterbox.active)
    {
        clear.clearColorValue[0] = 0.0f;
        clear.clearColorValue[1] = 0.0f;
        clear.clearColorValue[2] = 0.0f;
        clear.clearColorValue[3] = 1.0f;
    }
    else
    {
        const uint32_t bg = Editor::SceneViewSettings::Get().GetBackgroundColor();
        clear.clearColorValue[0] = static_cast<float>((bg >> 16) & 0xFFu) / 255.0f;
        clear.clearColorValue[1] = static_cast<float>((bg >> 8) & 0xFFu) / 255.0f;
        clear.clearColorValue[2] = static_cast<float>(bg & 0xFFu) / 255.0f;
        clear.clearColorValue[3] = 1.0f;
    }
    clear.clearDepth = true;
    clear.clearDepthValue = 0.0f;
    rs->Views().SetViewClearConfig(m_ViewId, clear);

    // Scene-view-local post-process override — identical to the old arm.
    {
        const auto& svSettings = Editor::SceneViewSettings::Get();
        const bool masterOn = m_PostProcessingEnabled;
        const bool bloomOn = svSettings.GetPostFxBloomEnabled();
        const bool tonemapOn = svSettings.GetPostFxTonemapEnabled();
        const bool colorFilterOn = svSettings.GetPostFxColorFilterEnabled();
        const bool casOn = svSettings.GetPostFxCasEnabled();
        const bool crtOn = svSettings.GetPostFxCrtEnabled();
        const bool autoExposureOn = svSettings.GetPostFxAutoExposureEnabled();
        const bool useAdaptationDelay =
            svSettings.GetPostFxAutoExposureUseAdaptationDelay();

        // Feed the Auto -> Fixed EV handoff: while this view meters, keep the
        // GPU exposure readback enabled (AutoExposureNode copies the adapted
        // scale into the feature's ring) and cache the EV100 that would
        // reproduce the current image if pinned as Fixed. Compensation is
        // ADDED: the adapted scale already contains it (folded into the
        // metering key below) and Fixed mode re-applies it on top, so folding
        // it into the seed keeps the toggle image-invariant. The feature is
        // fetched lazily so views that never meter never create it.
        if (autoExposureOn && !m_ExposureReadback)
            m_ExposureReadback = &rs->EnsureFeature<Engine::Renderer::ExposureReadbackFeature>();
        if (m_ExposureReadback)
        {
            m_ExposureReadback->SetReadbackEnabled(m_ViewId, autoExposureOn);
            float adaptedScale = 0.0f;
            if (autoExposureOn &&
                m_ExposureReadback->TryResolveAdaptedExposure(m_ViewId, adaptedScale))
            {
                Editor::SceneViewSettings::Get().SetLastMeteredExposureEv(
                    Rendering::LinearExposureToEv(adaptedScale) +
                    svSettings.GetPostFxExposureCompensation());
            }
        }

        // Auto-metering tune: engages the override when the adaptation clamps or the
        // compensation were moved off their defaults, or temporal adaptation was
        // disabled, while Auto Exposure is on. The epsilon absorbs slider-step float
        // drift so landing back on a default tick releases the override (0.001 stops
        // is far below perception).
        constexpr float kExposureTuneEpsilonEv = 1e-3f;
        auto differsEv = [](float a, float b) { return std::fabs(a - b) > kExposureTuneEpsilonEv; };
        const bool autoExposureTuned =
            autoExposureOn &&
            (differsEv(svSettings.GetPostFxAutoExposureMinEv(), Editor::SceneViewSettings::kDefaultAutoExposureMinEv) ||
             differsEv(svSettings.GetPostFxAutoExposureMaxEv(), Editor::SceneViewSettings::kDefaultAutoExposureMaxEv) ||
             differsEv(svSettings.GetPostFxExposureCompensation(), 0.0f) ||
             !useAdaptationDelay);
        const bool needsToolbarOverride =
            !masterOn || !bloomOn || !tonemapOn || !colorFilterOn || !casOn || !crtOn ||
            !autoExposureOn;
        const bool pp2dAlignCrt = m_Is2DMode && svSettings.GetPixelPerfect2D() && crtOn;

        // Extraction has already resolved the camera mask and spatial volume blend for
        // this view. Keep that result as the toolbar baseline, then stamp the live scene
        // camera lens every frame. Clearing the override here used to discard extraction's
        // camera lens whenever all toolbar switches were at their defaults, making Scene
        // View DoF silently fall back to the world defaults (focus 10, f/16).
        const uint64 worldId = GetWorld().GetWorldId();
        Engine::Renderer::PostProcessSettings pp =
            rs->GetEffectivePostProcessSettings(m_ViewId, worldId);
        // The Scene View keeps a complete per-view override alive so toolbar
        // switches and its camera lens survive extraction. That also means the
        // previous frame's Auto-off value can feed back through GetEffective...
        // after the user turns Auto back on. Make the live toggle authoritative
        // before any optional tuning; otherwise default Auto settings never enter
        // autoExposureTuned and the meter remains latched off indefinitely.
        pp.AutoExposureActive = autoExposureOn;
        const Engine::Renderer::ViewRegistry::CameraExposure lens = SceneViewLensExposure(&GetWorld());
        pp.DofFocusDistance = std::max(lens.FocusDistance, 0.01f);
        pp.DofFocalLengthMm = std::max(lens.FocalLengthMm, 1.0f);
        pp.DofAperture = std::max(lens.Aperture, Components::Camera::kApertureMin);
        pp.DofSensorHeightMm = std::max(lens.SensorHeightMm, 1.0f);
        pp.DofApertureBladeCount = std::clamp(
            lens.ApertureBladeCount,
            static_cast<int32>(Components::Camera::kApertureBladeCountMin),
            static_cast<int32>(Components::Camera::kApertureBladeCountMax));
        pp.DofApertureRoundness = std::clamp(lens.ApertureRoundness, 0.0f, 1.0f);
        pp.DofApertureRotation = std::clamp(lens.ApertureRotation, 0.0f, 360.0f);
        pp.DofAnamorphicSqueeze = std::clamp(lens.AnamorphicSqueeze, 1.0f, 4.0f);
        pp.DofDebugMode = lens.FocusDebugMode != 0 ? 1 : 0;
        pp.DofDebugAlpha = std::clamp(lens.FocusDebugAlpha, 0.0f, 1.0f);

        if (needsToolbarOverride || pp2dAlignCrt || autoExposureTuned)
        {
            if (pp2dAlignCrt)
            {
                pp.CrtEmulatedResolutionDiv =
                    static_cast<float>(std::clamp(svSettings.GetPixelPerfectScale(), 1, 64));
            }
            if (needsToolbarOverride)
            {
                // "Tonemapping off" maps to a concrete curve chosen per display: on an HDR
                // output, Linear (true identity) routes scene values into the panel's
                // headroom; on SDR, Linear would hard-clip bright pixels to flat white, so
                // fall back to Neutral (gentle highlight roll-off). Same HDR check the
                // Tonemap node uses, so editor and node agree on what "HDR" means.
                const bool hdrOutputActive =
                    rs->GetDevice() &&
                    Rendering::IsHdrOutputModeActive(rs->GetDevice()->GetActiveHdrOutputMode());
                const int32_t offTonemapMode = static_cast<int32_t>(
                    hdrOutputActive ? Components::TonemapMode::Linear
                                    : Components::TonemapMode::Neutral);
                Editor::ApplySceneViewPostProcessSwitches(
                    {.PostProcessing = masterOn,
                     .Bloom = bloomOn,
                     .Tonemap = tonemapOn,
                     .ColorFilter = colorFilterOn,
                     .ContrastAdaptiveSharpening = casOn,
                     .Crt = crtOn},
                    offTonemapMode, pp);
                // Auto Exposure off: pin the Scene View to the user's fixed EV100 instead of
                // leaving a metered/stale scale. This override is authoritative for the view, so
                // exposure is set here rather than via the clobbered camera path. Compensation
                // trims the pinned EV exactly as the camera-sensor resolve does
                // (ResolveExposureScale: a +/- stop multiplier in every mode). Stack the
                // Scene View's relative knob with the blended volume adjustment so
                // ExposureAdjustmentEffect remains effective while the meter is off.
                if (!autoExposureOn)
                {
                    pp.AutoExposureActive = false;
                    pp.Exposure = Rendering::ResolveExposureScale(
                        Components::ExposureMode::Manual, 1.0f,
                        svSettings.GetPostFxFixedExposureEv(),
                        svSettings.GetPostFxExposureCompensation() +
                            pp.ExposureCompensationEv);
                }
            }
            if (autoExposureTuned)
            {
                // Mirror the camera-sensor resolve (RenderExtractionSystem::applyCameraExposure):
                // metering stays active, the popup's EV envelope replaces the world-default
                // sensor's, and compensation adds to the post-clamp metering bias. Active is
                // set explicitly because the world settings default to inactive until
                // extraction stamps the world sensor. Volume ExposureAdjustmentEffect
                // modifiers still stack: the clamp fields riding the copied world settings
                // re-narrow the popup's envelope, and the volume compensation is already in
                // the bias, so the popup compensation is a view nudge on top of scene content.
                pp.AutoExposureActive = true;
                pp.AutoExposureMinEv = svSettings.GetPostFxAutoExposureMinEv();
                pp.AutoExposureMaxEv = svSettings.GetPostFxAutoExposureMaxEv();
                Rendering::NarrowAutoExposureEnvelope(pp.AutoExposureMinEv, pp.AutoExposureMaxEv,
                                                      pp.ExposureClampMinEv, pp.ExposureClampMaxEv);
                pp.AutoExposureBiasEv += svSettings.GetPostFxExposureCompensation();
                if (!useAdaptationDelay)
                {
                    // Instant adaptation = adaptation speed high enough that the resolve
                    // shader's smoothing factor (1 - exp2(-dt * speed)) saturates to exactly
                    // 1.0 in float32, snapping to the freshly metered target every frame.
                    // Only this view's copy is touched; scene-authored sensor speeds are not.
                    constexpr float kInstantAdaptationSpeed = 1.0e9f;
                    pp.AutoExposureSpeedUp = kInstantAdaptationSpeed;
                    pp.AutoExposureSpeedDown = kInstantAdaptationSpeed;
                }
            }
        }
        rs->Views().SetViewPostProcessOverride(m_ViewId, pp);
    }

    if (!rs->Views().IsViewExtractionCurrent(m_ViewId))
    {
        // A hidden Scene View can be activated after extraction has already
        // run. Preserve its last complete image until the request feeds the
        // next frame instead of replacing it with a sky-only render.
        m_WaitingForExtractionRG = true;
        return true;
    }

    rs->WriteViewLightBuffer(m_ViewId);

    // Viewport render scale is per-view pipeline state, not a smaller target:
    // the targets below stay 1:1 with the viewport rect, and the pipeline
    // rasterizes the world half at scale x this extent then crosses back before
    // the overlays. Republished every frame so the settings sliders stay live.
    // The SceneViewSettings value is an editor-side OVERRIDE: at its default
    // 1.0 the view follows the engine default instead, so the project Render
    // Scale (and the SSAA mode driving it) reaches editor viewports.
    {
        const float svScale = Editor::SceneViewSettings::Get().GetRenderScale();
        rs->Views().SetViewRenderScale(m_ViewId, svScale == 1.0f
                                                     ? std::nullopt
                                                     : std::optional<float>(svScale));
    }

    // Pool imports — window-namespaced names (the persistent pool keyspace is
    // global; "SceneView.Color" would collide across windows). A resize is
    // just a new desc: the pool reallocs and the old physical defer-destroys.
    const std::string baseName =
        "Editor.W" + std::to_string(windowId) + "." + m_RenderNamePrefix;
    Rendering::TextureDesc colorDesc{};
    colorDesc.width = w;
    colorDesc.height = h;
    colorDesc.depth = 1;
    colorDesc.mipLevels = 1;
    colorDesc.arrayLayers = 1;
    colorDesc.sampleCount = samples;
    colorDesc.format = static_cast<uint32_t>(TextureFormat::R16G16B16A16_FLOAT);
    // TransferSrc: the presentation snapshot copies whichever colour target is
    // single-sampled — this one when MSAA is off, the resolve (which inherits
    // this desc) when it is on. Pool-backed, so the desc is the only declaration.
    colorDesc.usage = static_cast<uint32_t>(Rendering::TextureUsage::RenderTarget) |
                      static_cast<uint32_t>(Rendering::TextureUsage::ShaderResource) |
                      static_cast<uint32_t>(Rendering::TextureUsage::TransferSrc);
    colorDesc.debugName = "Editor.SceneView.Color";

    // TransferSrc: viewport picking reads depth back through
    // CopyTextureSubresourceToBuffer.
    Rendering::TextureDesc depthDesc = colorDesc;
    depthDesc.format = static_cast<uint32_t>(TextureFormat::D32_FLOAT);
    depthDesc.usage = static_cast<uint32_t>(Rendering::TextureUsage::DepthStencil) |
                      static_cast<uint32_t>(Rendering::TextureUsage::ShaderResource) |
                      static_cast<uint32_t>(Rendering::TextureUsage::TransferSrc);
    depthDesc.debugName = "Editor.SceneView.Depth";

    const Rendering::RenderGraph::RGTexture color =
        frame.ImportPersistentTexture((baseName + ".Color").c_str(), colorDesc);
    const Rendering::RenderGraph::RGTexture depth =
        frame.ImportPersistentTexture((baseName + ".Depth").c_str(), depthDesc);
    Rendering::RenderGraph::RGTexture resolve{};
    if (samples > 1)
    {
        Rendering::TextureDesc resolveDesc = colorDesc;
        resolveDesc.sampleCount = 1;
        resolveDesc.debugName = "Editor.SceneView.Resolve";
        resolve = frame.ImportPersistentTexture((baseName + ".Resolve").c_str(), resolveDesc);
    }

    outTargets.View = m_ViewId;
    outTargets.Color = color;
    outTargets.Depth = depth;
    outTargets.Resolve = resolve;
    return true;
}

void SceneViewController::PrepareForActivation()
{
    if (m_RenderServices && m_ViewId != 0)
        m_RenderServices->Views().RequestViewFrame(m_ViewId);
}

} // namespace GameEngine
