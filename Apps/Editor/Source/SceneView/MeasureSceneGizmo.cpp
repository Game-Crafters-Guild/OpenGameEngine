#include "SceneView/MeasureSceneGizmo.h"

#include "Components/Hierarchy.h"
#include "Components/Measure/MeasureComponent.h"
#include "Components/Transform.h"
#include "Core/Engine.h"
#include "ECS/Components.h"
#include "ECS/ECS.h"
#include "ECS/ECSTemplates.h"
#include "Mathematics/MatrixOps.h"
#include "Mathematics/Ray.h"
#include "Mathematics/Vector3.h"
#include "Types/Color.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace GameEngine::Editor::SceneTools
{
namespace
{
constexpr std::uint32_t kEndpointHitHandle = 1u;
constexpr float kEndpointPickRadius = 0.16f;
constexpr float kEndpointScreenWorldFactor = 0.0048f;
constexpr float kSelectedEndpointScale = 1.35f;
constexpr float kHoveredEndpointScale = 1.18f;
constexpr float kEndpointMinWorldRadius = 0.025f;
constexpr float kEndpointMaxWorldRadius = 0.25f;

using Mathematics::Vector3;

Color MeasureColor(const Components::MeasureComponent& measure, float alphaScale)
{
    return Color(std::clamp(measure.Color[0], 0.0f, 1.0f),
                 std::clamp(measure.Color[1], 0.0f, 1.0f),
                 std::clamp(measure.Color[2], 0.0f, 1.0f),
                 std::clamp(measure.Color[3] * alphaScale, 0.0f, 1.0f));
}

Vector3 Lerp(const Vector3& a, const Vector3& b, float t)
{
    return a + (b - a) * t;
}

void DrawDottedWorldLine(GizmoRenderContext& context,
                         const Vector3& from,
                         const Vector3& to,
                         const Color& color,
                         float thickness,
                         float segmentLength)
{
    const float length = (to - from).Length();
    if (length <= 0.0001f)
        return;

    const float period = std::max(segmentLength, 0.001f) * 2.0f;
    for (float d = 0.0f; d < length; d += period)
    {
        context.DrawColoredLine(Lerp(from, to, d / length),
                                Lerp(from, to, std::min(d + segmentLength, length) / length),
                                color, thickness);
    }
}

float DistancePointToRaySq(const Vector3& point, const GizmoRay& ray, float& outRayT)
{
    const float t = std::max(0.0f, Vector3::Dot(point - ray.origin, ray.direction));
    outRayT = t;
    return (point - Mathematics::RayPointAt(ray, t)).LengthSquared();
}

bool GetEntityWorldMatrix(ECS::World* world,
                          ECS::EntityHandle entity,
                          Mathematics::Matrix4x4& out,
                          int depth = 0)
{
    if (!world || !entity.IsValid() || !world->IsValid(entity) || depth > 64)
        return false;

    if (const auto* transform = world->GetComponent<Components::Transform>(entity))
    {
        Mathematics::Matrix4x4 local = Mathematics::Matrix4x4::FromColumnMajor(transform->matrix);
        if (const auto* parent = world->GetComponent<Components::Parent>(entity);
            parent && parent->parent.IsValid())
        {
            Mathematics::Matrix4x4 parentWorld;
            if (GetEntityWorldMatrix(world, parent->parent, parentWorld, depth + 1))
            {
                out = parentWorld * local;
                return true;
            }
        }

        out = local;
        return true;
    }

    if (const auto* worldTransform = world->GetComponent<Components::WorldTransform>(entity))
    {
        out = Mathematics::Matrix4x4::FromColumnMajor(worldTransform->matrix);
        return true;
    }

    return false;
}

bool GetEntityWorldPosition(ECS::World* world, ECS::EntityHandle entity, Vector3& out)
{
    if (!world || !entity.IsValid() || !world->IsValid(entity))
        return false;

    Mathematics::Matrix4x4 worldMatrix;
    if (GetEntityWorldMatrix(world, entity, worldMatrix))
    {
        out = Vector3(worldMatrix[3].x, worldMatrix[3].y, worldMatrix[3].z);
        return true;
    }

    return false;
}

ECS::EntityHandle ResolveMeasureEndpointEntity(ECS::World* world,
                                               ECS::EntityHandle measureEntity,
                                               const Components::MeasureComponent& measure,
                                               std::uint8_t endpoint)
{
    (void)measureEntity;
    if (!world)
        return {};

    const ECS::EntityHandle stored = endpoint == 1u ? measure.StartEntity : measure.EndEntity;
    if (stored.IsValid() && world->IsValid(stored))
        return stored;

    return {};
}

bool GetMeasureEndpointPosition(ECS::World* world,
                                ECS::EntityHandle measureEntity,
                                const Components::MeasureComponent& measure,
                                std::uint8_t endpoint,
                                Vector3& out)
{
    if (!world)
        return false;

    const ECS::EntityHandle endpointEntity =
        ResolveMeasureEndpointEntity(world, measureEntity, measure, endpoint);
    return GetEntityWorldPosition(world, endpointEntity, out);
}

bool GetMeasureEndpointPosition(ECS::World* world, ECS::EntityHandle entity, std::uint8_t endpoint, Vector3& out)
{
    if (!world || !entity.IsValid() || !world->IsValid(entity))
        return false;

    const auto* measure = world->GetComponent<Components::MeasureComponent>(entity);
    if (!measure || !ECS::Entity(world, entity).IsEnabled<Components::MeasureComponent>())
        return false;

    return GetMeasureEndpointPosition(world, entity, *measure, endpoint, out);
}

bool IntersectEndpointDragPlane(const GizmoRay& ray,
                                const Vector3& origin,
                                const Vector3& normal,
                                Vector3& out)
{
    const Vector3 planeNormal = normal.LengthSquared() <= 1.0e-8f ? Vector3(0.0f, 0.0f, 1.0f) : normal;

    float t = 0.0f;
    return Mathematics::IntersectRayPlane(ray, origin, planeNormal, t, out);
}

float ComputeEndpointWorldRadius(const GizmoRenderContext& context, const Vector3& point, bool selected)
{
    float radius = 0.055f;
    if (context.HasOrthoHeight())
    {
        radius = std::max(0.0001f, context.GetOrthoHeight()) * kEndpointScreenWorldFactor;
    }
    else if (const Vector3* camera = context.GetCameraWorldPosition())
    {
        const float distance = (point - *camera).Length();
        radius = std::clamp(distance * kEndpointScreenWorldFactor,
                            kEndpointMinWorldRadius,
                            kEndpointMaxWorldRadius);
    }

    return selected ? radius * kSelectedEndpointScale : radius;
}

void DrawEndpointTick(GizmoRenderContext& context,
                      const Vector3& center,
                      const Color& color,
                      float endpointRadius)
{
    const Vector3* cameraUp = context.GetCameraUp();
    const Vector3 up = cameraUp ? *cameraUp : Vector3(0.0f, 1.0f, 0.0f);
    const float halfLength = endpointRadius * 3.2f;
    context.DrawColoredLine(center - up * halfLength, center + up * halfLength, color, 1.5f);
}

} // namespace

MeasureSceneGizmo::MeasureSceneGizmo(ECS::World& world)
    : m_World(&world), m_WorldGeneration(world.GetLifecycleResetGeneration())
{
}

void MeasureSceneGizmo::RefreshSelection() const
{
    const uint64 generation = m_World->GetLifecycleResetGeneration();
    if (generation != m_WorldGeneration)
    {
        m_WorldGeneration = generation;
        m_Selection = {};
    }
}

void MeasureSceneGizmo::SetEndpointPickedCallback(EndpointPickedCallback callback)
{
    m_OnEndpointPicked = std::move(callback);
}

void MeasureSceneGizmo::SetEndpointDragStartedCallback(EndpointDragStartedCallback callback)
{
    m_OnEndpointDragStarted = std::move(callback);
}

void MeasureSceneGizmo::SetEndpointDraggedCallback(EndpointDraggedCallback callback)
{
    m_OnEndpointDragged = std::move(callback);
}

void MeasureSceneGizmo::SetEndpointDragEndedCallback(EndpointDragEndedCallback callback)
{
    m_OnEndpointDragEnded = std::move(callback);
}

void MeasureSceneGizmo::SetHovered(ECS::EntityHandle entity)
{
    m_HoveredEntity = entity;
}

void MeasureSceneGizmo::SetViewportScaleCompensation(float scale)
{
    m_ViewportScaleCompensation = std::max(0.1f, scale);
}

void MeasureSceneGizmo::Render(GizmoRenderContext& context)
{
    ECS::World* world = m_World;
    if (!world)
        return;

    const MeasureEndpointSelection selected = Selection();

    world->Query<ECS::Read<Components::MeasureComponent>>()
        .Each([&](ECS::EntityHandle entity, const Components::MeasureComponent& measure)
        {

            Vector3 start;
            Vector3 end;
            if (!GetMeasureEndpointPosition(world, entity, measure, 1u, start) ||
                !GetMeasureEndpointPosition(world, entity, measure, 2u, end))
            {
                return;
            }

            const Color lineColor = MeasureColor(measure, 1.0f);
            const Color controlDotColor(0.0f, 0.0f, 0.0f, 0.92f);
            const Color hoveredControlDotColor(0.08f, 0.07f, 0.04f, 1.0f);
            const Color selectedControlDotColor(0.08f, 0.08f, 0.08f, 1.0f);
            const Color controlDotCenterColor(1.0f, 0.78f, 0.22f, 1.0f);
            const Color hoveredControlDotCenterColor(1.0f, 0.92f, 0.38f, 1.0f);

            const bool measureHovered = m_HoveredEntity == entity;
            if (!measure.Is2D)
            {
                if (measureHovered)
                {
                    const Color hoverLineColor(lineColor.r + (1.0f - lineColor.r) * 0.35f,
                                               lineColor.g + (1.0f - lineColor.g) * 0.35f,
                                               lineColor.b + (1.0f - lineColor.b) * 0.35f,
                                               std::clamp(lineColor.a * 1.15f, 0.0f, 1.0f));
                    DrawDottedWorldLine(context, start, end, hoverLineColor, 2.4f, 0.12f);
                }
                DrawDottedWorldLine(context, start, end, lineColor, 1.5f, 0.12f);
            }

            const bool startSelected = selected.Entity == entity && selected.Endpoint == 1u;
            const bool endSelected = selected.Entity == entity && selected.Endpoint == 2u;
            const bool startHovered =
                measureHovered ||
                (m_HoveredEntity == measure.StartEntity) ||
                (m_LastHit.Entity == entity && m_LastHit.Endpoint == 1u);
            const bool endHovered =
                measureHovered ||
                (m_HoveredEntity == measure.EndEntity) ||
                (m_LastHit.Entity == entity && m_LastHit.Endpoint == 2u);
            const float startRadius =
                ComputeEndpointWorldRadius(context, start, startSelected) * m_ViewportScaleCompensation *
                (startHovered && !startSelected ? kHoveredEndpointScale : 1.0f);
            const float endRadius =
                ComputeEndpointWorldRadius(context, end, endSelected) * m_ViewportScaleCompensation *
                (endHovered && !endSelected ? kHoveredEndpointScale : 1.0f);
            const Color& startControlColor = startSelected
                ? selectedControlDotColor
                : (startHovered ? hoveredControlDotColor : controlDotColor);
            const Color& endControlColor = endSelected
                ? selectedControlDotColor
                : (endHovered ? hoveredControlDotColor : controlDotColor);
            const Color& startCenterColor =
                startHovered ? hoveredControlDotCenterColor : controlDotCenterColor;
            const Color& endCenterColor =
                endHovered ? hoveredControlDotCenterColor : controlDotCenterColor;
            const float startCenterScale = startHovered ? 0.50f : 0.42f;
            const float endCenterScale = endHovered ? 0.50f : 0.42f;
            DrawEndpointTick(context, start, startControlColor, startRadius);
            DrawEndpointTick(context, end, endControlColor, endRadius);
            context.DrawSolidSphere(start, startRadius, startControlColor);
            context.DrawSolidSphere(end, endRadius, endControlColor);
            context.DrawSolidSphere(start, startRadius * startCenterScale, startCenterColor);
            context.DrawSolidSphere(end, endRadius * endCenterScale, endCenterColor);
        });
}

GizmoHitResult MeasureSceneGizmo::HitTest(const GizmoRay& ray)
{
    ECS::World* world = m_World;
    if (!world)
        return {};

    bool hasBest = false;
    float bestDistance = 0.0f;
    EndpointHit best{};
    const float pickRadius = kEndpointPickRadius * m_ViewportScaleCompensation;
    const float pickRadiusSq = pickRadius * pickRadius;

    auto testEndpoint = [&](ECS::EntityHandle entity, std::uint8_t endpoint, const Vector3& point)
    {
        float rayT = 0.0f;
        const float distanceSq = DistancePointToRaySq(point, ray, rayT);
        if (distanceSq > pickRadiusSq)
            return;
        if (!hasBest || rayT < bestDistance)
        {
            hasBest = true;
            bestDistance = rayT;
            best = {entity, endpoint};
        }
    };

    world->Query<ECS::Read<Components::MeasureComponent>>()
        .Each([&](ECS::EntityHandle entity, const Components::MeasureComponent& measure)
        {

            Vector3 start;
            Vector3 end;
            if (!GetMeasureEndpointPosition(world, entity, measure, 1u, start) ||
                !GetMeasureEndpointPosition(world, entity, measure, 2u, end))
            {
                return;
            }

            testEndpoint(entity, 1u, start);
            testEndpoint(entity, 2u, end);
        });

    if (!hasBest)
    {
        m_LastHit = {};
        return {};
    }

    m_LastHit = best;
    GizmoHitResult result{};
    result.hit = true;
    result.info.distance = bestDistance;
    result.info.kind = GizmoHitKind::Custom;
    result.info.handleId = kEndpointHitHandle;
    return result;
}

bool MeasureSceneGizmo::HandlePointerEvent(const ScenePointerEvent& event, const GizmoHit& hit)
{
    if (hit.handleId != kEndpointHitHandle)
    {
        return false;
    }

    if (event.button == PointerButton::Left && event.phase == PointerPhase::Down && m_LastHit.Entity.IsValid())
    {
        Selection() = {m_LastHit.Entity, m_LastHit.Endpoint};
        if (m_OnEndpointPicked)
            m_OnEndpointPicked(m_LastHit.Entity, m_LastHit.Endpoint);

        ECS::World* world = m_World;
        if (GetMeasureEndpointPosition(world, m_LastHit.Entity, m_LastHit.Endpoint, m_DragPlaneOrigin))
        {
            m_DragPlaneNormal = event.cameraForward;
            if (!IntersectEndpointDragPlane(event.ray, m_DragPlaneOrigin, m_DragPlaneNormal, m_LastDragHit))
                m_LastDragHit = m_DragPlaneOrigin;

            m_DraggingEndpoint = true;
            if (m_OnEndpointDragStarted)
                m_OnEndpointDragStarted(m_LastHit.Entity, m_LastHit.Endpoint);
        }
        return true;
    }

    if (!m_DraggingEndpoint)
        return false;

    if (event.phase == PointerPhase::Move)
    {
        Vector3 hitPoint;
        if (!IntersectEndpointDragPlane(event.ray, m_DragPlaneOrigin, m_DragPlaneNormal, hitPoint))
            return true;

        const Vector3 delta = hitPoint - m_LastDragHit;
        m_LastDragHit = hitPoint;


        if (m_OnEndpointDragged)
            m_OnEndpointDragged(delta);
        return true;
    }

    if (event.phase == PointerPhase::Up)
    {
        m_DraggingEndpoint = false;
        if (m_OnEndpointDragEnded)
            m_OnEndpointDragEnded();
        return true;
    }

    return false;
}

} // namespace GameEngine::Editor::SceneTools
