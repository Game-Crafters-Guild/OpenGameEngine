#include "SceneView/TransformTool.h"

#include "Core/Engine.h"
#include "Core/CpuProfiler.h"

#include "ECS/Components.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/Query.h"
#include "ECS/World.h"

#include "Components/Hierarchy.h"
#include "Components/Measure/MeasureComponent.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Rendering/Light.h"
#include "Editor/Settings/SceneViewSettings.h"
#include "Editor/Settings/SplineEditorSettings.h"
#include "Editor/Shortcuts/EditorShortcuts.h"
#include "Components/Rendering/LocalBounds.h"
#include "Components/Spline/SplineComponent.h"
#include "Components/Transform.h"
#include "Components/Rendering/WorldSectorCoord.h"
#include "Editor/Entities/EditorComponentTraits.h"
#include "SceneView/LightGizmo.h"
#include "SceneView/SceneViewProjection.h"
#include "SplineECS/SplineService.h"
#include "Input/InputSystem.h"
#include "Input/KeyCodes.h"
#include "Logger/Logger.h"
#include "Mathematics/Geometry.h"
#include "Mathematics/Matrix4x4.h"
#include "Mathematics/Quaternion.h"
#include "Mathematics/Ray.h"
#include "Mathematics/VectorOps.h"
#include "Mathematics/Vector2.h"
#include "Mathematics/Vector3.h"
#include "PhysicsECS/Components/PhysicsBody.h"
#include "PhysicsECS/PhysicsWorldService.h"
#include "Picking/MeshPickingService.h"
#include "Types/Color.h"
#include "Types/ColorUtils.h"
#include "UndoRedo/MultiEntityComponentSnapshot.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

namespace GameEngine
{
namespace Editor
{
namespace SceneTools
{

using GameEngine::Components::MeshRenderer;
using GameEngine::Components::LocalBounds;
using GameEngine::Components::WorldTransform;

namespace
{
inline bool IsFrameTraceEnabled()
{
    static bool s_Enabled = []()
    {
        const char* env = std::getenv("GE_FRAME_TRACE");
        if (!env || !*env)
        {
            return false;
        }

        // Treat "0" as explicit off; anything else enables tracing.
        return !(env[0] == '0' && env[1] == '\0');
    }();
    return s_Enabled;
}

// Default axis length (in world units) used by the translate gizmo until
// we introduce dynamic, view-dependent scaling.
constexpr float kDefaultAxisLength = 1.5f;

// Base multipliers for constant-screen-size mode. With user scale = 1.0 these
// produce the default apparent gizmo size.
constexpr float kTranslateScreenBase = 0.1875f; // distance * this = axis length
constexpr float kScaleScreenBase     = 0.16875f;
constexpr float kRotateScreenBase    = 0.09375f; // rotate ring radius (diameter ≈ translate axis length)

// Hold S while dragging the rotate gizmo to snap applied rotation to 45° steps.
constexpr float kRotateGizmoSnapIncrementRad = GameEngine::Mathematics::Pi * 0.25f;

inline float ComputeEffectiveGizmoDistance(const TransformTool* owner,
                                           const Mathematics::Vector3& pivot,
                                           const Mathematics::Vector3& camPos)
{
    if (owner && owner->HasOrthoHeight())
    {
        return owner->GetOrthoHeight() * kOrthoEffectiveDistanceFactor;
    }
    return (camPos - pivot).Length();
}
constexpr float kDefaultAxisThickness = 3.0f; // a bit thicker for better visibility
constexpr float kAxisPickRadius = 0.15f;      // world-space picking radius for translate axes
// Translate axis pick-tube radius as a fraction of the on-screen axis length.
// Keeps the grab tube a stable fraction of the visible axis at any distance
// (0.16 reproduces the previous feel at the tuned ~5-unit viewing distance).
constexpr float kAxisPickRadiusFraction = 0.16f;

const Color kAxisXColor(1.0f, 0.0f, 0.0f, 1.0f);
const Color kAxisYColor(0.0f, 1.0f, 0.0f, 1.0f);
const Color kAxisZColor(0.0f, 0.0f, 1.0f, 1.0f);

// Slightly brighter variants used when an axis/handle is actively dragged.
const Color kAxisXHighlightColor(1.0f, 0.4f, 0.4f, 1.0f);
const Color kAxisYHighlightColor(0.4f, 1.0f, 0.4f, 1.0f);
const Color kAxisZHighlightColor(0.4f, 0.4f, 1.0f, 1.0f);

// Planar translate handle visual parameters.
// Fraction controls the overall plane extent relative to axis length.
// 0.0875f yields planes half the previous size.
constexpr float kPlaneHandleSizeFraction = 0.0875f;
constexpr float kPlaneFillAlpha = 0.35f;
constexpr float kPlaneFillHighlightAlpha = 0.70f;
constexpr float kPlaneEdgeAlpha = 0.90f;
constexpr float kPlaneHitThicknessFraction = 0.03f; // picking thickness along normal for translate planes
constexpr float kPlaneEdgeThickness = kDefaultAxisThickness * 0.75f;

// Plane colours (by axis pair): XZ = green, XY = blue, YZ = red.
const Color kPlaneXYFillColor(0.0f, 0.0f, 1.0f, kPlaneFillAlpha); // XY = blue
const Color kPlaneYZFillColor(1.0f, 0.0f, 0.0f, kPlaneFillAlpha); // YZ = red
const Color kPlaneZXFillColor(0.0f, 1.0f, 0.0f, kPlaneFillAlpha); // ZX/XZ = green
const Color kPlaneXYEdgeColor(0.0f, 0.0f, 1.0f, kPlaneEdgeAlpha);
const Color kPlaneYZEdgeColor(1.0f, 0.0f, 0.0f, kPlaneEdgeAlpha);
const Color kPlaneZXEdgeColor(0.0f, 1.0f, 0.0f, kPlaneEdgeAlpha);

// Hover/active highlights: yellow-tinted fill (like axis highlights) at
// a much higher alpha so the small squares clearly light up, with a yellow
// edge so the outline pops against the gizmo arrows.
const Color kPlaneFillHighlightColor(1.0f, 0.9f, 0.2f, kPlaneFillHighlightAlpha);
const Color kPlaneEdgeHighlightColor(1.0f, 0.95f, 0.3f, 1.0f);
constexpr float kPlaneEdgeHighlightThicknessScale = 1.6f;

// Rotation gizmo parameters.
constexpr float kDefaultRotationRadius = 1.0f;
constexpr float kRotationRingTubeRadius = 0.02f; // tube thickness for torus

// Screen-space ("view-plane") rotation ring. Sits just outside the X/Y/Z rings
// and rotates the target around the camera forward axis (roll). Rendered as a
// neutral light-grey ring so it reads as the "screen" handle, not an axis.
constexpr float kViewRingRadiusScale = 1.26f;
const Color kViewRingColor(0.82f, 0.82f, 0.88f, 0.85f);
const Color kViewRingHighlightColor(1.0f, 1.0f, 1.0f, 1.0f);

// Scale gizmo parameters.
constexpr float kScaleAxisThickness = kDefaultAxisThickness * 0.5f;
constexpr float kScaleHandleSizeFraction = 0.14f;
constexpr float kScaleCenterHandleSizeFraction = 0.12f;
const Color kUniformScaleHandleColor(1.0f, 1.0f, 0.0f, 1.0f); // yellow
const Color kUniformScaleHandleHighlightColor(1.0f, 1.0f, 0.4f, 1.0f);

constexpr float kScaleAxisMinFactor = 0.1f;
constexpr float kScaleAxisMaxFactor = 10.0f;
// Sensitivity for uniform scale - controls the exponent for exponential scaling.
// Using exponential scaling ensures consistent *perceptual* scaling at all sizes.
constexpr float kUniformScaleScreenSensitivity = 0.005f;


using namespace Components;
using namespace ECS;
using GameEngine::Mathematics::Quaternion;
using GameEngine::Mathematics::Vector3;

const GizmoAxisDirections kWorldAxisDirections = {
    Vector3(1.0f, 0.0f, 0.0f),
    Vector3(0.0f, 1.0f, 0.0f),
    Vector3(0.0f, 0.0f, 1.0f)};

struct TRS
{
    Vector3 position;
    Quaternion rotation;
    Vector3 scale{1.0f, 1.0f, 1.0f};
};

inline TRS IdentityTRS()
{
    return TRS{};
}

inline TRS GetLocalTRS(Entity& e)
{
    TRS t = IdentityTRS();
    if (const Transform* xf = e.Get<Transform>())
    {
        t.position = xf->GetPosition();
        t.rotation = xf->GetRotation();
        t.scale = xf->GetScale();
    }
    return t;
}

inline TRS CombineTRS(const TRS& parent, const TRS& local)
{
    TRS out{};
    // Scale
    out.scale = Vector3(
        parent.scale.x * local.scale.x,
        parent.scale.y * local.scale.y,
        parent.scale.z * local.scale.z);

    // Rotation: compose in the same space as the engine matrices.
    out.rotation = parent.rotation * local.rotation;

    // Position: match FromTRS / TransformHierarchySystem.
    //   worldPos = parentPos + R(parentRot) * (parentScale * localPos)
    Vector3 scaledLocal(
        local.position.x * parent.scale.x,
        local.position.y * parent.scale.y,
        local.position.z * parent.scale.z);
    Vector3 rotated = parent.rotation.Rotate(scaledLocal);
    out.position = parent.position + rotated;

    return out;
}

inline TRS ComputeWorldTRSForEntity(World* world, EntityHandle handle)
{
    if (!world)
    {
        return IdentityTRS();
    }

    if (!handle.IsValid())
    {
        return IdentityTRS();
    }

    // Reconstruct world TRS from the local Transform + Parent hierarchy.
    //
    // We intentionally *do not* rely on the cached WorldTransform here,
    // because the editor applies local transform deltas in response to
    // pointer events, while the WorldTransform component is updated later
    // by TransformHierarchySystem. Using WorldTransform would therefore lag
    // one frame behind the gizmo interactions, causing rotation deltas to
    // be applied on top of an out-of-date world orientation.
    Entity e(world, handle);
    if (!e.IsValid())
    {
        return IdentityTRS();
    }

    TRS local = GetLocalTRS(e);

    if (auto* parentComp = e.Get<Parent>())
    {
        TRS parentWorld = ComputeWorldTRSForEntity(world, parentComp->parent);
        return CombineTRS(parentWorld, local);
    }

    // No valid parent: world == local
    return local;
}

inline void GetParentWorldTRS(World* world,
                              EntityHandle handle,
                              Vector3& outPos,
                              Quaternion& outRot,
                              Vector3& outScale)
{
    TRS identity = IdentityTRS();
    outPos = identity.position;
    outRot = identity.rotation;
    outScale = identity.scale;

    if (!world)
    {
        return;
    }

    Entity e(world, handle);
    if (!e.IsValid())
    {
        return;
    }

    auto* parentComp = e.Get<Parent>();
    if (!parentComp)
    {
        return; // identity parent
    }

    TRS parentWorld = ComputeWorldTRSForEntity(world, parentComp->parent);
    outPos = parentWorld.position;
    outRot = parentWorld.rotation;
    outScale = parentWorld.scale;
}

inline Vector3 ComputeLocalPositionFromWorld(const Vector3& worldPos,
                                              const Vector3& parentPos,
                                              const Quaternion& parentRot,
                                              const Vector3& parentScale)
{
    // Undo parent translation
    Vector3 delta = worldPos - parentPos;

    // Undo parent rotation.
    //   parentScale * localPos = R(parentRot^-1) * deltaWorld
    Vector3 unrotated = parentRot.Conjugated().Rotate(delta);

    // Undo parent scale (component-wise, safe against zeros)
    return Vector3(
        (parentScale.x != 0.0f) ? (unrotated.x / parentScale.x) : unrotated.x,
        (parentScale.y != 0.0f) ? (unrotated.y / parentScale.y) : unrotated.y,
        (parentScale.z != 0.0f) ? (unrotated.z / parentScale.z) : unrotated.z);
}

inline bool HasAncestorInSet(World* world,
                             EntityHandle entity,
                             const std::vector<EntityHandle>& candidates)
{
    if (!world || !entity.IsValid())
        return false;

    Entity e(world, entity);
    while (e.IsValid())
    {
        const auto* parent = e.Get<Parent>();
        if (!parent || !parent->parent.IsValid())
            return false;

        if (std::find(candidates.begin(), candidates.end(), parent->parent) != candidates.end())
            return true;

        e = Entity(world, parent->parent);
    }

    return false;
}

inline bool TryBuildWorldMatrixForEntity(World* world, EntityHandle entity, Mathematics::Matrix4x4& outMatrix)
{
    if (!world || !entity.IsValid() || !world->IsValid(entity))
        return false;

    WorldTransform worldXf{};
    if (const auto* cached = world->GetComponent<WorldTransform>(entity))
    {
        worldXf = *cached;
    }
    else if (world->GetComponent<Transform>(entity))
    {
        const TRS worldTrs = ComputeWorldTRSForEntity(world, entity);
        const Transform localMatrix = Transform::FromTRS(worldTrs.position, worldTrs.rotation, worldTrs.scale);
        std::memcpy(worldXf.matrix, localMatrix.matrix, sizeof(localMatrix.matrix));
    }
    else
    {
        return false;
    }

    // Compose the sector so the gizmo, world-position queries, and measure
    // endpoints land on the SAME absolute world position the mesh renders and
    // picks at. ComposeEffectiveWorldTransform is the single source of truth
    // (also used by SceneTlas + MeshPickingService); a null/zero sector returns
    // the matrix unchanged (dark-ship for untagged entities).
    const auto* sector = world->GetComponent<Components::WorldSectorCoord>(entity);
    const WorldTransform composed =
        Components::ComposeEffectiveWorldTransform(worldXf, sector, Components::kWorldSectorSize);
    outMatrix = Mathematics::Matrix4x4::FromColumnMajor(composed.matrix);
    return true;
}

inline bool TryGetEntityWorldPosition(World* world, EntityHandle entity, Vector3& outPosition)
{
    Mathematics::Matrix4x4 matrix;
    if (!TryBuildWorldMatrixForEntity(world, entity, matrix))
        return false;

    const auto& translation = matrix[3];
    outPosition = Vector3(translation.x, translation.y, translation.z);
    return true;
}

inline bool TryComputeMeasureEndpointCenterForEntity(World* world, EntityHandle entity, Vector3& outPivot)
{
    if (!world || !entity.IsValid() || !world->IsValid(entity))
        return false;

    const auto* measure = world->GetComponent<Components::MeasureComponent>(entity);
    if (!measure || !Entity(world, entity).IsEnabled<Components::MeasureComponent>())
        return false;

    Vector3 start(measure->Start[0], measure->Start[1], measure->Start[2]);
    Vector3 end(measure->End[0], measure->End[1], measure->End[2]);

    if (measure->StartEntity.IsValid() && world->IsValid(measure->StartEntity))
        (void)TryGetEntityWorldPosition(world, measure->StartEntity, start);
    if (measure->EndEntity.IsValid() && world->IsValid(measure->EndEntity))
        (void)TryGetEntityWorldPosition(world, measure->EndEntity, end);

    outPivot = (start + end) * 0.5f;
    return true;
}

inline bool TryComputeEditorPivotForEntity(World* world, EntityHandle entity, Vector3& outPivot)
{
    Mathematics::Matrix4x4 worldMatrix;
    if (!TryBuildWorldMatrixForEntity(world, entity, worldMatrix))
        return false;

    if (TryComputeMeasureEndpointCenterForEntity(world, entity, outPivot))
        return true;

    Vector3 localPivot(0.0f, 0.0f, 0.0f);

    // Plugin-owned components whose gizmo pivots at the entity origin
    // (registered traits) short-circuit the rendered-content anchors below.
    for (const auto& [typeId, traits] : Editor::EditorComponentTraitsRegistry::Get().Snapshot())
    {
        if (traits.GizmoPivotAtEntityOrigin && world->HasComponent(entity, typeId))
        {
            outPivot = worldMatrix.TransformPoint(localPivot);
            return true;
        }
    }

    const auto* spline = world->GetComponent<Components::SplineComponent>(entity);
    if (spline && Entity(world, entity).IsEnabled<Components::SplineComponent>())
    {
        // A spline entity's visible root is its first local control point.
        // Keep the entity gizmo on that rendered anchor; putting the drag
        // plane at the raw transform origin can place it at a different
        // perspective depth than the curve, which makes the curve appear to
        // move faster/slower than the gizmo.
        if (auto* splineService = SplineECS::SplineService::TryGet())
        {
            const SplineECS::SplineHandle handle(spline->SplineDataIndex, spline->SplineDataGeneration);
            const auto* data = splineService->GetSplineData(handle);
            if (data && !data->Points.empty())
            {
                if (Editor::SplineEditorSettings::Get().GetPivotFromSplineCenter())
                {
                    Vector3 minPoint = data->Points.front().Position;
                    Vector3 maxPoint = data->Points.front().Position;
                    for (const auto& point : data->Points)
                    {
                        minPoint.x = std::min(minPoint.x, point.Position.x);
                        minPoint.y = std::min(minPoint.y, point.Position.y);
                        minPoint.z = std::min(minPoint.z, point.Position.z);
                        maxPoint.x = std::max(maxPoint.x, point.Position.x);
                        maxPoint.y = std::max(maxPoint.y, point.Position.y);
                        maxPoint.z = std::max(maxPoint.z, point.Position.z);
                    }
                    const Vector3 localCenter = (minPoint + maxPoint) * 0.5f;
                    outPivot = worldMatrix.TransformPoint(localCenter);
                    return true;
                }

                outPivot = worldMatrix.TransformPoint(data->Points.front().Position);
                return true;
            }
        }

        outPivot = worldMatrix.TransformPoint(localPivot);
        return true;
    }

    if (const auto* bounds = world->GetComponent<LocalBounds>(entity))
    {
        outPivot = worldMatrix.TransformPoint(bounds->Box.center);
        return true;
    }

    outPivot = worldMatrix.TransformPoint(localPivot);
    return true;
}

inline void PreviewWorldTransform(ECS::World* world,
                                  ECS::EntityHandle entity,
                                  const Vector3& worldPos,
                                  const Quaternion& worldRot,
                                  const Vector3& worldScale)
{
    if (!world || !entity.IsValid() || !world->IsValid(entity))
        return;

    const Transform worldMatrix = Transform::FromTRS(worldPos, worldRot, worldScale);
    WorldTransform preview{};
    if (const auto* existing = world->GetComponent<WorldTransform>(entity))
        preview = *existing;

    if (std::memcmp(preview.matrix, worldMatrix.matrix, sizeof(preview.matrix)) == 0 &&
        world->GetComponent<WorldTransform>(entity))
    {
        return;
    }

    std::memcpy(preview.matrix, worldMatrix.matrix, sizeof(preview.matrix));
    ++preview.Version;
    world->AddComponentImmediate(entity, preview);
}

inline bool MoveEntityWorldPositionPreservingRotationScale(ECS::World* world,
                                                           ECS::EntityHandle entity,
                                                           const Vector3& newWorldPos)
{
    if (!world || !entity.IsValid() || !world->IsValid(entity))
        return false;

    auto* transform = world->GetComponent<Transform>(entity);
    if (!transform)
        return false;

    TRS worldTrs = ComputeWorldTRSForEntity(world, entity);
    Vector3 parentPos;
    Quaternion parentRot;
    Vector3 parentScale{1.0f, 1.0f, 1.0f};
    GetParentWorldTRS(world, entity, parentPos, parentRot, parentScale);

    const Vector3 newLocalPos = ComputeLocalPositionFromWorld(newWorldPos,
                                                              parentPos,
                                                              parentRot,
                                                              parentScale);
    const Transform updated = Transform::FromTRS(newLocalPos,
                                                 transform->GetRotation(),
                                                 transform->GetScale());
    {
        GE_CPU_PROFILE_SCOPE("ECS.AddComponentImmediate.Transform");
        world->AddComponentImmediate(entity, updated);
    }
    PreviewWorldTransform(world, entity, newWorldPos, worldTrs.rotation, worldTrs.scale);
    return true;
}

inline bool TrySetPhysicsBodyWorldPose(ECS::World* world,
                                       ECS::EntityHandle entity,
                                       const Vector3& worldPos,
                                      const Quaternion& worldRot)
{
    if (!world || !entity.IsValid() || !world->IsValid(entity))
    {
        return false;
    }

    auto* pw = PhysicsECS::PhysicsWorldService::TryGet();
    if (!pw)
    {
        return false;
    }

    auto* body = world->GetComponentForWrite<PhysicsBody>(entity);
    if (!body || !body->initialized || !pw->IsBodyValid(body->body))
    {
        return false;
    }

    Physics::Transform pt{};
    pt.position = Physics::Vector3(worldPos.x, worldPos.y, worldPos.z);
    const auto& wq = worldRot.GetGLM();
    pt.rotation = Physics::Quaternion(wq.w, wq.x, wq.y, wq.z);
    pw->SetBodyTransform(body->body, pt, Physics::ActivationMode::Activate);
    pw->SetLinearVelocity(body->body, Physics::Vector3(0.0f, 0.0f, 0.0f));
    pw->SetAngularVelocity(body->body, Physics::Vector3(0.0f, 0.0f, 0.0f));

    // Avoid a one-frame interpolation smear after teleports/edits.
    body->prevPhysicsTransform = pt;
    body->hasPrevPhysicsTransform = true;

    return true;
}
} // namespace

void TransformTranslateGizmo::SetPivot(const Vector3& position)
{
    m_Pivot = position;
}

void TransformTranslateGizmo::SetAxisLength(float length)
{
    if (length > 0.0f)
    {
        m_AxisLength = length;
    }
    else
    {
        m_AxisLength = kDefaultAxisLength;
    }
}

void TransformTranslateGizmo::SetHover(const GizmoHit& hit)
{
    m_HoverKind = hit.kind;
    if (hit.kind == GizmoHitKind::Axis)
    {
        m_HoverAxis = hit.handleId;
        m_HoverPlane = 0u;
    }
    else if (hit.kind == GizmoHitKind::Plane)
    {
        m_HoverPlane = hit.handleId;
        m_HoverAxis = 0u;
    }
    else
    {
        m_HoverAxis = 0u;
        m_HoverPlane = 0u;
    }
}

void TransformTranslateGizmo::Render(GizmoRenderContext& context)
{
    // Handles are a control surface, not a place in the world: occluding them
    // against the geometry they move only makes them harder to grab. Covers
    // every draw below, plane quads included — those are drag targets too.
    GizmoDepthModeScope depthScope(context, GizmoDepthMode::AlwaysOnTop);

    // Draw planar handles (XY, YZ, ZX) first on the base gizmo triangle
    // layer, then the axis cylinders/arrows on a higher layer so that the
    // arrow geometry reliably hides any plane seams without fighting the
    // general opaque/transparent sort used by the Scene View overlay.
    
    // Compute axis length and screen scale - use constant screen size if enabled
    float axisLength = m_AxisLength;
    float screenScale = 1.0f; // Scale factor for world-space sizes to maintain screen appearance
    const bool constantSize = m_Owner && m_Owner->GetConstantScreenSize();
    const bool editor2D = context.IsEditor2DMode() || (m_Owner && m_Owner->IsIn2DMode());
    if (constantSize && context.HasCameraWorldPosition())
    {
        // Compute size directly from camera position for zero-latency response.
        // In orthographic mode, ComputeEffectiveGizmoDistance uses the ortho
        // height instead of the literal cam-to-pivot distance so the gizmo
        // stays at a constant pixel size regardless of scroll zoom / pixel
        // scale changes.
        const float distance =
            ComputeEffectiveGizmoDistance(m_Owner, m_Pivot, *context.GetCameraWorldPosition());
        const float gizmoScale = m_Owner->GetTranslateGizmoScale();
        axisLength = std::max(0.1f, distance * kTranslateScreenBase * gizmoScale);
        screenScale = axisLength / kDefaultAxisLength;
    }

    const float shaftFraction = 0.75f;
    const float shaftLength = axisLength * shaftFraction;
    // Use appropriate thickness based on constant screen size mode
    const float translateThickness = m_Owner 
        ? (constantSize ? m_Owner->GetTranslateConstantThickness() : m_Owner->GetTranslateGizmoThickness()) 
        : kDefaultAxisThickness;
    const float thicknessScale = translateThickness / 3.0f; // normalize to default
    const float shaftRadius = 0.04f * thicknessScale * screenScale;
    const float headRadius = 0.10f * screenScale; // Scale with distance for constant screen size
    const float axisLineWidth = translateThickness * 0.5f * screenScale;
    const float planeHalfExtent = axisLength * kPlaneHandleSizeFraction;

    const bool draggingAxis = m_IsDragging && m_DragKind == GizmoHitKind::Axis;
    const bool draggingPlane = m_IsDragging && m_DragKind == GizmoHitKind::Plane;
    const bool hoverAxis = !m_IsDragging && m_HoverKind == GizmoHitKind::Axis;
    const bool hoverPlane = !m_IsDragging && m_HoverKind == GizmoHitKind::Plane;

    const bool xHighlighted = (draggingAxis && m_ActiveAxis == 0u) || (hoverAxis && m_HoverAxis == 0u);
    const bool yHighlighted = (draggingAxis && m_ActiveAxis == 1u) || (hoverAxis && m_HoverAxis == 1u);
    const bool zHighlighted = (draggingAxis && m_ActiveAxis == 2u) || (hoverAxis && m_HoverAxis == 2u);

    const Color& xColor = xHighlighted ? kAxisXHighlightColor : kAxisXColor;
    const Color& yColor = yHighlighted ? kAxisYHighlightColor : kAxisYColor;
    const Color& zColor = zHighlighted ? kAxisZHighlightColor : kAxisZColor;

    const Vector3 origin = m_Pivot;

    // Get axis directions from owner so that translation respects the
    // current axis space (World vs Local). In World space the gizmo aligns
    // with the global axes; in Local space it follows the entity's rotation.
    GizmoAxisDirections axisDirs = kWorldAxisDirections;
    if (m_Owner)
    {
        m_Owner->GetAxisDirections(axisDirs);
    }

    if (!editor2D)
    {
        const float planeExtent = 2.0f * planeHalfExtent;

        // Planar handles live on the default triangle layer (0).
        context.SetTriangleLayer(0);

        // Planar handles (XY, YZ, ZX) as flat, transparent double-sided quads with crisp
        // edges. Quads cover the same extents as the previous discs: they span from the
        // gizmo origin out to axisLength * 2 * kPlaneHandleSizeFraction along the two
        // relevant axes.

        const float edgeThickness = translateThickness * 0.75f;

        // Plane P spans axes (P, P+1): XY (blue), YZ (red), ZX (green).
        const Color* planeFillColors[3] = {&kPlaneXYFillColor, &kPlaneYZFillColor, &kPlaneZXFillColor};
        const Color* planeEdgeColors[3] = {&kPlaneXYEdgeColor, &kPlaneYZEdgeColor, &kPlaneZXEdgeColor};
        for (std::uint32_t plane = 0; plane < 3u; ++plane)
        {
            const bool active = draggingPlane && m_ActivePlane == plane;
            const bool hover = hoverPlane && m_HoverPlane == plane;
            const Color& fillColor = (active || hover) ? kPlaneFillHighlightColor : *planeFillColors[plane];
            const Color& edgeColor = (active || hover) ? kPlaneEdgeHighlightColor : *planeEdgeColors[plane];

            // Quad corners using axis directions: origin, +A, +A+B, +B.
            const Vector3 edgeA = axisDirs[plane] * planeExtent;
            const Vector3 edgeB = axisDirs[(plane + 1u) % 3u] * planeExtent;
            const Vector3 bl = origin;
            const Vector3 br = origin + edgeA;
            const Vector3 tr = origin + edgeA + edgeB;
            const Vector3 tl = origin + edgeB;

            // Front face, then the back face with opposite winding.
            const Vector3 front[6] = {bl, br, tr, bl, tr, tl};
            context.DrawTriangles(front, 6u, fillColor);
            const Vector3 back[6] = {bl, tr, br, bl, tl, tr};
            context.DrawTriangles(back, 6u, fillColor);

            // Edges as lines, more opaque than the fill.
            context.DrawColoredLine(bl, br, edgeColor, edgeThickness);
            context.DrawColoredLine(br, tr, edgeColor, edgeThickness);
            context.DrawColoredLine(tr, tl, edgeColor, edgeThickness);
            context.DrawColoredLine(tl, bl, edgeColor, edgeThickness);
        }
    }

    // Axis arrows (cylinders + cones + axis lines) rendered on a higher
    // triangle layer so they appear on top of the planar handles while
    // still respecting the usual opaque/transparent ordering within that
    // layer.
    context.SetTriangleLayer(1);

    // Line-only mode when thickness is at exact minimum (0.5) - thin constant-width lines
    const bool lineOnlyMode = translateThickness <= 0.5f;

    // X, Y and (outside 2D) Z axes: shaft cylinder, arrow cone and axis line.
    const Color* axisColors[3] = {&xColor, &yColor, &zColor};
    const std::uint32_t axisCount = editor2D ? 2u : 3u;
    for (std::uint32_t axis = 0; axis < axisCount; ++axis)
    {
        const Color& color = *axisColors[axis];
        const Vector3 shaftEnd = origin + axisDirs[axis] * shaftLength;
        const Vector3 tip = origin + axisDirs[axis] * axisLength;

        if (!lineOnlyMode)
        {
            context.DrawSolidCylinder(origin, shaftEnd, shaftRadius, color);
        }
        // Always draw arrows (cones) regardless of mode
        context.DrawSolidCone(tip, shaftEnd, headRadius, color);
        context.DrawColoredLine(origin, tip, color, lineOnlyMode ? 1.5f : axisLineWidth);
    }

    if (editor2D && !lineOnlyMode)
    {
        const float outerRadius = axisLength * kScaleCenterHandleSizeFraction * 0.32f * thicknessScale;
        const float innerRadius = outerRadius * 0.62f;
        const Color centerHandleOuterColor(0.0f, 0.0f, 0.0f, 0.92f);
        context.DrawSolidSphere(origin, outerRadius, centerHandleOuterColor);
        context.DrawSolidSphere(origin, innerRadius, kUniformScaleHandleColor);
    }

    // Small 3D sphere at the pivot, similar in visual weight to the scale
    // gizmo's center cube. This gives a clearer sense of the gizmo's origin
    // without depending on camera-facing orientation.
    if (!lineOnlyMode && !editor2D)
    {
        const float baseRadius = axisLength * kScaleCenterHandleSizeFraction * 0.5f * thicknessScale;
        // Reuse the uniform scale handle colour (yellow) so center handles
        // share a consistent visual language across gizmos.
        context.DrawSolidSphere(origin, baseRadius, kUniformScaleHandleColor);
    }
}

GizmoHitResult TransformTranslateGizmo::HitTest(const GizmoRay& ray)
{
    GizmoHitResult result{};

    if (!m_Owner)
    {
        return result;
    }

    if (!m_Owner->GetTargetEntity().IsValid())
    {
        return result;
    }

    // Compute axis length - use constant screen size if enabled (same logic as Render)
    float axisLength = m_AxisLength;
    if (m_Owner->GetConstantScreenSize())
    {
        const float distance = ComputeEffectiveGizmoDistance(m_Owner, m_Pivot, ray.origin);
        const float gizmoScale = m_Owner->GetTranslateGizmoScale();
        axisLength = std::max(0.1f, distance * kTranslateScreenBase * gizmoScale);
    }

    // Axes are treated as finite world-space line segments originating at the
    // pivot. Their directions come from the owning TransformTool so that they
    // respect the current axis space (World vs Local).
    GizmoAxisDirections axisDirs = kWorldAxisDirections;
    m_Owner->GetAxisDirections(axisDirs);

    const Vector3 origin = m_Pivot;

    bool hasHit = false;
    GizmoHit bestHit = {};
    const bool editor2D = m_Owner->IsIn2DMode();

    // Axis hit-testing -------------------------------------------------------------
    // Pick radius is a flat fraction of the on-screen axis length. Since the axis
    // length already tracks camera distance in constant-screen-size mode, this
    // keeps the grab tube a stable fraction of the visible axis at any distance,
    // instead of the old fixed-radius x distance scaling that drifted with zoom.
    const float scaledAxisPickRadius = axisLength * kAxisPickRadiusFraction;
    
    for (std::uint32_t axis = 0; axis < 3u; ++axis)
    {
        if (editor2D && axis == 2u)
            continue;

        float rayT = 0.0f;
        float lineT = 0.0f;
        Vector3 onAxis;

        if (!ProjectRayOntoLine(ray, origin, axisDirs[axis], rayT, lineT, onAxis))
        {
            continue;
        }

        // Ignore intersections behind the camera.
        if (rayT <= 0.0f)
        {
            continue;
        }

        // Restrict to the finite arrow length.
        if (lineT < 0.0f || lineT > axisLength)
        {
            continue;
        }

        // Distance from the ray to the axis at the closest point.
        const Vector3 rayPoint = Mathematics::RayPointAt(ray, rayT);
        if ((onAxis - rayPoint).LengthSquared() > scaledAxisPickRadius * scaledAxisPickRadius)
        {
            continue;
        }

        if (!hasHit || rayT < bestHit.distance)
        {
            hasHit = true;
            bestHit.kind = GizmoHitKind::Axis;
            bestHit.handleId = axis;
            bestHit.distance = rayT;
        }
    }

    // Planar handle hit-testing (XY, YZ, ZX) --------------------------------------
    // Quads are spanned by (axisDirs[A], axisDirs[B]) from origin to
    // origin + (A+B)*planeExtent. Picking must run in that local 2D basis,
    // not against a world-aligned AABB, or local-mode and rotated axes miss.
    const float planeRadius = axisLength * kPlaneHandleSizeFraction;
    const float planeExtent = planeRadius * 2.0f;

    if (!editor2D)
    {
        for (std::uint32_t plane = 0; plane < 3u; ++plane)
        {
            // Plane P picks axes (A, B) = ((P+0)%3, (P+1)%3):
            //   P=0 -> XY (A=X, B=Y)
            //   P=1 -> YZ (A=Y, B=Z)
            //   P=2 -> ZX (A=Z, B=X)
            const std::uint32_t axisA = plane;
            const std::uint32_t axisB = (plane + 1u) % 3u;

            const Vector3& dA = axisDirs[axisA];
            const Vector3& dB = axisDirs[axisB];

            // Plane normal is dA x dB.
            Vector3 normal = Vector3::Cross(dA, dB);
            const float nLenSq = normal.LengthSquared();
            if (nLenSq < 1.0e-8f)
                continue;
            normal = normal * (1.0f / std::sqrt(nLenSq));

            // Skip planes seen nearly edge-on and hits behind the camera.
            float s = 0.0f;
            Vector3 p;
            if (!Mathematics::IntersectRayPlane(ray, origin, normal, s, p) || s <= 0.0f)
            {
                continue;
            }

            // Project (p - origin) into the (dA, dB) basis and check the local
            // coordinates against the finite quad [0, planeExtent]^2.
            const Vector3 rel = p - origin;
            const float uA = Vector3::Dot(rel, dA);
            const float uB = Vector3::Dot(rel, dB);

            if (uA < 0.0f || uA > planeExtent || uB < 0.0f || uB > planeExtent)
            {
                continue;
            }

            // Landing inside the finite plane quad is a strong, unambiguous signal,
            // so planes win over any axis hit here. (The axis cylinder still extends
            // well past the plane extent, so the user can grab the axis outside the
            // plane region without ambiguity.)
            const bool planeBeatsAxis = hasHit && bestHit.kind == GizmoHitKind::Axis;
            if (!hasHit || planeBeatsAxis || s < bestHit.distance)
            {
                hasHit = true;
                bestHit.kind = GizmoHitKind::Plane;
                bestHit.handleId = plane;
                bestHit.distance = s;
            }
        }
    }

    // Center handle in 2D mode: when the scene is in 2D editing mode, expose
    // a sphere-shaped hit region at the gizmo origin so the user can grab and
    // drag the gizmo from its center without having to land on an axis arrow
    // or an offset plane handle. The drag uses the plane perpendicular to the
    // dominant camera axis (inferred from the ray direction, which equals the
    // camera forward for orthographic 2D views).
    if (m_Owner->IsIn2DMode())
    {
        const float centerRadius = axisLength * kPlaneHandleSizeFraction;

        const float t = Vector3::Dot(origin - ray.origin, ray.direction);

        if (t > 0.0f)
        {
            const float distSq = (Mathematics::RayPointAt(ray, t) - origin).LengthSquared();

            if (distSq <= centerRadius * centerRadius)
            {
                // Choose the plane whose normal is most aligned with the ray
                // direction: this is the plane facing the camera in 2D mode.
                const float ax = std::fabs(ray.direction.x);
                const float ay = std::fabs(ray.direction.y);
                const float az = std::fabs(ray.direction.z);

                std::uint32_t plane = 0; // 0=XY (normal=Z), 1=YZ (normal=X), 2=ZX (normal=Y)
                if (ax >= ay && ax >= az)
                    plane = 1; // YZ plane
                else if (ay >= ax && ay >= az)
                    plane = 2; // ZX plane
                else
                    plane = 0; // XY plane

                // Center hit always wins in 2D mode — the whole point is to
                // let the user grab the gizmo anywhere near its origin.
                hasHit = true;
                bestHit.kind = GizmoHitKind::Plane;
                bestHit.handleId = plane;
                bestHit.distance = t;
            }
        }
    }

    if (!hasHit)
    {
        return result;
    }

    result.hit = true;
    result.info = bestHit;
    return result;
}

bool TransformTranslateGizmo::HandlePointerEvent(const ScenePointerEvent& event,
                                                 const GizmoHit& hit)
{
    if (!m_Owner)
    {
        return false;
    }

    if (!m_Owner->HasTransformTarget())
    {
        return false;
    }

    if (event.button != PointerButton::Left)
    {
        return false;
    }

    if (event.phase == PointerPhase::Down)
    {
        // Start a new drag based on what was hit (axis or plane).
        if (hit.kind == GizmoHitKind::Axis)
        {
            std::uint32_t axis = hit.handleId;
            if (axis > 2u)
            {
                return false;
            }

            // Resolve the drag axis from the owning tool so that we respect the
            // current axis space (World vs Local).
            GizmoAxisDirections axisDirs = kWorldAxisDirections;
            m_Owner->GetAxisDirections(axisDirs);

            m_DragOrigin = m_Pivot;
            m_DragAxis = axisDirs[axis];

            float rayT = 0.0f;
            float lineT = 0.0f;
            Vector3 onAxis;

            if (!ProjectRayOntoLine(event.ray, m_DragOrigin, m_DragAxis, rayT, lineT, onAxis))
            {
                return false;
            }

            if (rayT <= 0.0f)
            {
                return false;
            }

            m_IsDragging = true;
            m_DragKind = GizmoHitKind::Axis;
            m_ActiveAxis = axis;
            m_DragStartAxisParam = lineT;
            m_DragCurrentAxisParam = 0.0f;
            m_Owner->BeginTransformEdit("Transform Translate");
            return true;
        }
        else if (hit.kind == GizmoHitKind::Plane)
        {
            std::uint32_t plane = hit.handleId;
            if (plane > 2u)
            {
                return false;
            }

            // Use current axis directions so local-mode rotation is respected.
            GizmoAxisDirections axisDirs = kWorldAxisDirections;
            m_Owner->GetAxisDirections(axisDirs);

            const std::uint32_t axisA = plane;
            const std::uint32_t axisB = (plane + 1u) % 3u;

            Vector3 normal = Vector3::Cross(axisDirs[axisA], axisDirs[axisB]);
            const float nLenSq = normal.LengthSquared();
            if (nLenSq < 1.0e-8f)
            {
                return false;
            }
            normal = normal * (1.0f / std::sqrt(nLenSq));

            float rayT = 0.0f;
            Vector3 hitPoint;
            if (!Mathematics::IntersectRayPlane(event.ray, m_Pivot, normal, rayT, hitPoint))
            {
                return false;
            }
            if (rayT <= 0.0f)
            {
                return false; // behind camera
            }

            m_IsDragging = true;
            m_DragKind = GizmoHitKind::Plane;
            m_ActivePlane = plane;
            m_PlaneOrigin = m_Pivot;
            m_PlaneNormal = normal;
            m_LastPlanePoint = hitPoint;
            m_Owner->BeginTransformEdit("Transform Translate");
            return true;
        }

        return false;
    }

    if (!m_IsDragging)
    {
        return false;
    }

    if (event.phase == PointerPhase::Move)
    {
        if (m_DragKind == GizmoHitKind::Axis)
        {
            float rayT = 0.0f;
            float lineT = 0.0f;
            Vector3 onAxis;

            if (!ProjectRayOntoLine(event.ray, m_DragOrigin, m_DragAxis, rayT, lineT, onAxis))
            {
                return true; // keep ownership of drag even if projection fails
            }

            if (rayT <= 0.0f)
            {
                return true;
            }

            float paramFromStart = lineT - m_DragStartAxisParam;
            float deltaParam = paramFromStart - m_DragCurrentAxisParam;
            if (deltaParam == 0.0f)
            {
                return true;
            }

            m_DragCurrentAxisParam = paramFromStart;

            m_Owner->ApplyTranslationDelta(m_DragAxis * deltaParam);
            return true;
        }
        else if (m_DragKind == GizmoHitKind::Plane)
        {
            float rayT = 0.0f;
            Vector3 hitPoint;
            if (!Mathematics::IntersectRayPlane(event.ray, m_PlaneOrigin, m_PlaneNormal, rayT, hitPoint))
            {
                return true; // nearly parallel; keep drag but do not move
            }
            if (rayT <= 0.0f)
            {
                return true; // behind camera; keep drag
            }

            const Vector3 deltaWorld = hitPoint - m_LastPlanePoint;
            m_LastPlanePoint = hitPoint;

            if (deltaWorld.x == 0.0f &&
                deltaWorld.y == 0.0f &&
                deltaWorld.z == 0.0f)
            {
                return true;
            }

            m_Owner->ApplyTranslationDelta(deltaWorld);
            return true;
        }

        return true;
    }

    if (event.phase == PointerPhase::Up)
    {
        m_Owner->CommitTransformEdit();
        m_IsDragging = false;
        m_DragKind = GizmoHitKind::None;
        m_ActiveAxis = 0;
        m_ActivePlane = 0;
        m_DragStartAxisParam = 0.0f;
        m_DragCurrentAxisParam = 0.0f;
        return true;
    }

    return false;
}

void TransformRotateGizmo::SetPivot(const Vector3& position)
{
    m_Pivot = position;
}

void TransformRotateGizmo::SetRadius(float radius)
{
    if (radius > 0.0f)
    {
        m_Radius = radius;
    }
    else
    {
        m_Radius = kDefaultRotationRadius;
    }
}

void TransformRotateGizmo::SetHover(const GizmoHit& hit)
{
    m_HoverKind = hit.kind;
    if (hit.kind == GizmoHitKind::Axis)
    {
        m_HoverAxis = hit.handleId;
    }
    else
    {
        m_HoverAxis = 0u;
    }
}

void TransformRotateGizmo::Render(GizmoRenderContext& context)
{
    // Manipulation, not description — the rings are grab targets, and the drag
    // feedback (angle arc, start/end lines, snap guides) is only readable at
    // full strength.
    GizmoDepthModeScope depthScope(context, GizmoDepthMode::AlwaysOnTop);

    const Vector3 center = m_Pivot;

    // Compute radius - use constant screen size if enabled
    float radius = m_Radius;
    float screenScale = 1.0f;
    const bool constantSize = m_Owner && m_Owner->GetConstantScreenSize();
    if (constantSize && context.HasCameraWorldPosition())
    {
        const float distance =
            ComputeEffectiveGizmoDistance(m_Owner, m_Pivot, *context.GetCameraWorldPosition());

        const float gizmoScale = m_Owner->GetRotateGizmoScale();
        radius = std::max(0.2f, distance * kRotateScreenBase * gizmoScale);
        screenScale = radius / (kDefaultAxisLength * 2.0f);
    }

    // Use appropriate thickness based on constant screen size mode
    const float rotateThickness = m_Owner
        ? (constantSize ? m_Owner->GetRotateConstantThickness() : m_Owner->GetRotateGizmoThickness())
        : kDefaultAxisThickness;
    const float thicknessScale = rotateThickness / 3.0f; // normalize to default
    const float baseTube = kRotationRingTubeRadius * thicknessScale * screenScale;

    // Line-only mode when thickness is at exact minimum (0.5) - thin constant-width lines
    const bool lineOnlyMode = rotateThickness <= 0.5f;

    const bool draggingAxis = m_IsDragging && m_DragKind == GizmoHitKind::Axis;
    const bool hoverAxis = !m_IsDragging && m_HoverKind == GizmoHitKind::Axis;

    const bool xHighlighted = (draggingAxis && m_ActiveAxis == 0u) || (hoverAxis && m_HoverAxis == 0u);
    const bool yHighlighted = (draggingAxis && m_ActiveAxis == 1u) || (hoverAxis && m_HoverAxis == 1u);
    const bool zHighlighted = (draggingAxis && m_ActiveAxis == 2u) || (hoverAxis && m_HoverAxis == 2u);
    const bool viewHighlighted =
        (draggingAxis && m_ActiveAxis == kViewPlaneHandle) || (hoverAxis && m_HoverAxis == kViewPlaneHandle);

    const Color& xColor = xHighlighted ? kAxisXHighlightColor : kAxisXColor;
    const Color& yColor = yHighlighted ? kAxisYHighlightColor : kAxisYColor;
    const Color& zColor = zHighlighted ? kAxisZHighlightColor : kAxisZColor;

    float tubeX = xHighlighted ? baseTube * 1.5f : baseTube;
    float tubeY = yHighlighted ? baseTube * 1.5f : baseTube;
    float tubeZ = zHighlighted ? baseTube * 1.5f : baseTube;

    // Enhanced behavior (front-face culling + view-plane ring) is gated by a
    // user setting; when off the gizmo renders as full rings with no screen ring.
    const bool enhanced = !m_Owner || m_Owner->GetRotateGizmoEnhanced();

    // Camera position drives front-facing ring culling. When dragging an axis
    // ring we draw it in full (the swept-sector feedback reads better against a
    // complete ring), so culling only applies to the idle/hover display.
    const bool hasCullCamPos = enhanced && context.HasCameraWorldPosition();
    Vector3 cullCamPos;
    if (hasCullCamPos)
    {
        cullCamPos = *context.GetCameraWorldPosition();
    }

    // Get axis directions from owner (world or local space).
    GizmoAxisDirections axisDirs = kWorldAxisDirections;
    if (m_Owner)
    {
        m_Owner->GetAxisDirections(axisDirs);
    }

    bool rotateSnapHeld = false;
    if (Input::InputSystem* input = EngineCore::GetInstance().GetInputSystem())
        rotateSnapHeld = input->IsKeyDown(static_cast<Input::KeyCode>(Input::kKeyCode_S));

    // Snap increment is user-configurable (defaults to 45°).
    const float snapStep = m_Owner ? m_Owner->GetRotateSnapIncrementRad() : kRotateGizmoSnapIncrementRad;

    // Helper to draw a circle using line segments (for line-only mode)
    auto drawLineCircleRadius = [&](const Vector3& normal, const Color& color, float lineWidth, float circleRadius) {
        // Build tangent and bitangent perpendicular to normal
        const Vector3 seed = (std::abs(normal.y) < 0.99f) ? Vector3(0.0f, 1.0f, 0.0f) : Vector3(1.0f, 0.0f, 0.0f);
        Vector3 tangent = Vector3::Cross(seed, normal);
        const float len = tangent.Length();
        if (len > 0.0001f) { tangent = tangent / len; }

        const Vector3 bitangent = Vector3::Cross(normal, tangent);

        // Front-facing cull: in-plane direction toward the camera. Segments
        // whose midpoint radial faces away are the occluded back arc.
        bool cull = false;
        Vector3 cdir;
        if (hasCullCamPos)
        {
            const Vector3 toCam = cullCamPos - center;
            const float along = Vector3::Dot(toCam, normal);
            cdir = toCam - normal * along;
            const float cl = cdir.Length();
            if (cl > 1.0e-4f) { cdir = cdir / cl; cull = true; }
        }

        constexpr int segments = 48;
        for (int i = 0; i < segments; ++i)
        {
            float a0 = (float(i) / segments) * 2.0f * GameEngine::Mathematics::Pi;
            float a1 = (float(i + 1) / segments) * 2.0f * GameEngine::Mathematics::Pi;
            float c0 = std::cos(a0), s0 = std::sin(a0);
            float c1 = std::cos(a1), s1 = std::sin(a1);
            if (cull)
            {
                const Vector3 md = tangent * (c0 + c1) + bitangent * (s0 + s1);
                if (Vector3::Dot(md, cdir) < -0.24f)
                    continue;
            }
            const Vector3 p0 = center + (tangent * c0 + bitangent * s0) * circleRadius;
            const Vector3 p1 = center + (tangent * c1 + bitangent * s1) * circleRadius;
            context.DrawColoredLine(p0, p1, color, lineWidth);
        }
    };

    // Axis rings draw at the gizmo radius; the view ring overrides it.
    auto drawLineCircle = [&](const Vector3& normal, const Color& color, float lineWidth) {
        drawLineCircleRadius(normal, color, lineWidth, radius);
    };

    // Build a camera-facing frame for the screen-space ("view-plane") ring.
    // Its normal is the view direction (camera -> pivot); it always faces the
    // camera so it is never culled. Needs both camera position and up vector.
    const float viewRadius = radius * kViewRingRadiusScale;
    Vector3 viewNormal(0.0f, 0.0f, 1.0f);
    bool hasViewRing = false;
    if (enhanced && context.HasCameraWorldPosition() && context.HasCameraUp())
    {
        const Vector3 vd = center - cullCamPos;
        const float vl = vd.Length();
        if (vl > 1.0e-4f)
        {
            viewNormal = vd / vl;
            hasViewRing = true;
        }
    }
    const Color& viewColor = viewHighlighted ? kViewRingHighlightColor : kViewRingColor;
    const float viewTube = (viewHighlighted ? baseTube * 1.5f : baseTube) * 0.85f;

    auto drawViewRing = [&]()
    {
        if (!hasViewRing)
            return;
        if (lineOnlyMode)
            drawLineCircleRadius(viewNormal, viewColor, viewHighlighted ? 2.5f : 1.5f, viewRadius);
        else
            context.DrawSolidTorus(center, viewNormal, viewRadius, viewTube, viewColor);
    };

    // Always render all three axis rings plus the screen-space ring, including
    // while dragging, so the full orientation frame stays visible during a
    // rotation. The active ring is drawn in full (no front-face cull) so the
    // whole circle reads against the swept-sector feedback.
    if (lineOnlyMode)
    {
        const float lineWidth = 1.5f;
        drawLineCircle(axisDirs[0], xColor, xHighlighted ? 2.5f : lineWidth);
        drawLineCircle(axisDirs[1], yColor, yHighlighted ? 2.5f : lineWidth);
        drawLineCircle(axisDirs[2], zColor, zHighlighted ? 2.5f : lineWidth);
        drawViewRing();
    }
    else
    {
        for (std::uint32_t a = 0; a < 3u; ++a)
        {
            const Color& col = (a == 0u) ? xColor : (a == 1u ? yColor : zColor);
            const float tube = (a == 0u) ? tubeX : (a == 1u ? tubeY : tubeZ);
            const bool cullRing = hasCullCamPos && !(draggingAxis && m_ActiveAxis == a);
            context.DrawSolidTorus(center, axisDirs[a], radius, tube, col, 32, 8,
                                   cullRing ? &cullCamPos : nullptr);
        }
        drawViewRing();
    }

    // While dragging, draw guides from the pivot to the start/end of the arc
    // plus a filled sector so the user can visualize the accumulated delta.
    if (draggingAxis && m_DragKind == GizmoHitKind::Axis)
    {
        const Color& activeColor = (m_ActiveAxis == 0u)
                                       ? xColor
                                       : (m_ActiveAxis == 1u ? yColor : zColor);

        // The ring lies in the plane spanned by m_Tangent/m_Bitangent. The
        // torus has radius "radius" and a tube radius of baseTube, so the
        // inner visual edge of the ring sits roughly at radius - baseTube.
        const float arcRadius = std::max(radius - baseTube, 0.0f);
        const float endLineRadius = radius + baseTube * 0.5f;

        auto computePointOnRing = [&](float angle, float r)
        {
            return center + (m_Tangent * std::cos(angle) + m_Bitangent * std::sin(angle)) * r;
        };

        // End line: from center through the current pointer direction (or the
        // snapped angle when S is held), extended slightly beyond the ring.
        float endAngleForLine = m_CurrentAngle;
        if (rotateSnapHeld)
        {
            const float step = snapStep;
            const float snapTotal = std::round(m_TotalAngleDelta / step) * step;
            endAngleForLine = m_StartAngle + snapTotal;
        }
        const Vector3 currentEndPoint = computePointOnRing(endAngleForLine, endLineRadius);
        const Color endLineColor(1.0f, 1.0f, 1.0f, 1.0f);
        context.DrawColoredLine(center, currentEndPoint, endLineColor, rotateThickness);

        // Filled sector ("pie slice") representing the net rotation since the
        // drag started. We accumulate incremental angle deltas in
        // m_TotalAngleDelta so the sector keeps growing smoothly past 180°
        // instead of flipping to the shorter arc. With S held, the displayed
        // sweep snaps to 45° steps to match ApplyRotationDelta.
        float total = m_TotalAngleDelta;
        if (rotateSnapHeld)
        {
            const float step = snapStep;
            total = std::round(m_TotalAngleDelta / step) * step;
        }
        float absTotal = std::fabs(total);
        if (absTotal > 1.0e-3f)
        {
            const float fullTurn = 2.0f * GameEngine::Mathematics::Pi;

            // Reduce the displayed sweep to [0, 2π) so that each full
            // revolution empties the wedge and it begins filling again.
            float displayed = std::fmod(absTotal, fullTurn);
            if (displayed < 1.0e-3f)
            {
                // Exactly at (or extremely close to) a full revolution:
                // nothing to draw.
                return;
            }

            float signedDisplayed = (total >= 0.0f) ? displayed : -displayed;
            float startAngle = m_StartAngle;

            int segments = 32;

            const Color fillColor = rotateSnapHeld
                ? Color(0.18f, 0.72f, 0.36f, 0.48f)
                : Color(activeColor.r, activeColor.g, activeColor.b, activeColor.a * 0.5f);

            // Start line: from center to the beginning of the sector, tinted
            // by the active axis colour.
            const Vector3 startPoint = computePointOnRing(startAngle, arcRadius);
            const Color startLineColor(activeColor.r * 0.9f, activeColor.g * 0.9f, activeColor.b * 0.9f, 1.0f);
            context.DrawColoredLine(center, startPoint, startLineColor, rotateThickness * 0.75f);

            // Render the filled sector on the same triangle layer as the rings so
            // it is clearly visible on top of scene geometry.
            context.SetTriangleLayer(1);

            for (int i = 0; i < segments; ++i)
            {
                float t0 = static_cast<float>(i) / static_cast<float>(segments);
                float t1 = static_cast<float>(i + 1) / static_cast<float>(segments);
                float a0 = startAngle + signedDisplayed * t0;
                float a1 = startAngle + signedDisplayed * t1;

                const Vector3 p0 = computePointOnRing(a0, arcRadius);
                const Vector3 p1 = computePointOnRing(a1, arcRadius);

                // Front face triangle (center, p0, p1), then the back face for
                // double-sided visibility.
                const Vector3 front[3] = {center, p0, p1};
                context.DrawTriangles(front, 3u, fillColor);
                const Vector3 back[3] = {center, p1, p0};
                context.DrawTriangles(back, 3u, fillColor);
            }
        }
    }

    // Dotted radials at 45° snap positions only while dragging a ring with S held
    // (same increment as ApplyRotationDelta).
    if (rotateSnapHeld && draggingAxis)
    {
        const Color snapGuideColor(0.78f, 0.78f, 0.82f, 0.9f);
        const Color snapHighlightColor(1.0f, 0.92f, 0.45f, 1.0f);
        const float guideThickness = std::max(1.0f, rotateThickness * 0.75f);
        const float innerGuideR = lineOnlyMode ? 0.0f : std::max(radius * 0.12f, baseTube * 0.5f);
        const float outerGuideR = radius + baseTube * 0.65f;
        const float dashLen = std::max(radius * 0.045f, 0.025f);
        const float gapLen = dashLen * 0.9f;

        const float step = snapStep;
        // Number of evenly spaced snap spokes around the ring (8 for 45°). Clamp
        // so a tiny increment doesn't flood the view with radial lines.
        const int spokeCount = std::min(72, std::max(1, static_cast<int>(std::lround(
            (2.0f * GameEngine::Mathematics::Pi) / std::max(step, 1.0e-3f)))));
        const float snapTotal = std::round(m_TotalAngleDelta / step) * step;
        const long long kSnap = static_cast<long long>(std::llround(snapTotal / step));
        const long long spokeMod = static_cast<long long>(spokeCount);
        const int snapHighlightSpoke = static_cast<int>(((kSnap % spokeMod) + spokeMod) % spokeMod);

        auto drawDottedRadial = [&](const Vector3& dir, const Color& color, float thick)
        {
            float u = innerGuideR;
            while (u < outerGuideR - 1.0e-4f)
            {
                const float u1 = std::min(u + dashLen, outerGuideR);
                const Vector3 p0 = center + dir * u;
                const Vector3 p1 = center + dir * u1;
                context.DrawColoredLine(p0, p1, color, thick);
                u = u1 + gapLen;
            }
        };

        auto drawSnapLinesForAxis = [&](std::uint32_t axis, float angleOffset, int highlightSpoke)
        {
            // Spokes lie in the ring plane of `axis`, spanned by the next two axes.
            const Vector3& tangentAxis = axisDirs[(axis + 1u) % 3u];
            const Vector3& bitangentAxis = axisDirs[(axis + 2u) % 3u];

            for (int n = 0; n < spokeCount; ++n)
            {
                const float ang = angleOffset + static_cast<float>(n) * step;
                Vector3 dir = tangentAxis * std::cos(ang) + bitangentAxis * std::sin(ang);
                const float len = dir.Length();
                if (len < 1.0e-5f)
                    continue;
                dir = dir / len;
                const bool isHighlight = (highlightSpoke >= 0 && n == highlightSpoke);
                if (isHighlight)
                {
                    const Vector3 p0 = center + dir * innerGuideR;
                    const Vector3 p1 = center + dir * outerGuideR;
                    context.DrawColoredLine(p0, p1, snapHighlightColor, guideThickness * 2.0f);
                }
                else
                {
                    drawDottedRadial(dir, snapGuideColor, guideThickness);
                }
            }
        };

        const float offset = m_StartAngle;
        if (m_ActiveAxis == 0u)
            drawSnapLinesForAxis(0u, offset, snapHighlightSpoke);
        else if (m_ActiveAxis == 1u)
            drawSnapLinesForAxis(1u, offset, snapHighlightSpoke);
        else if (m_ActiveAxis == 2u)
            drawSnapLinesForAxis(2u, offset, snapHighlightSpoke);
    }
}

GizmoHitResult TransformRotateGizmo::HitTest(const GizmoRay& ray)
{
    GizmoHitResult result{};

    if (!m_Owner)
    {
        return result;
    }

    if (!m_Owner->GetTargetEntity().IsValid())
    {
        return result;
    }

    const Vector3& center = m_Pivot;

    // Compute radius - use constant screen size if enabled (same logic as Render)
    float radius = m_Radius;
    if (m_Owner->GetConstantScreenSize())
    {
        const float distance = ComputeEffectiveGizmoDistance(m_Owner, m_Pivot, ray.origin);
        const float gizmoScale = m_Owner->GetRotateGizmoScale();
        radius = std::max(0.2f, distance * kRotateScreenBase * gizmoScale);
    }
    
    const bool enhanced = m_Owner->GetRotateGizmoEnhanced();

    // Pick band half-width: a flat fraction of the ring radius. Because the
    // radius already tracks camera distance in constant-screen-size mode, this
    // keeps the pick tolerance a stable fraction of the on-screen ring at any
    // distance. Kept generous (15% of radius) so the rings — especially a
    // near-edge-on one viewed as a thin sliver — stay comfortably clickable.
    // This replaces an earlier radius×distance scaling that made the band grow
    // with distance² — thin/fiddly up close and overly fat far away.
    const float scaledHalfWidth = radius * 0.15f;

    // Get axis directions from owner (world or local space).
    GizmoAxisDirections axisDirs = kWorldAxisDirections;
    m_Owner->GetAxisDirections(axisDirs);

    bool hasHit = false;
    GizmoHit bestHit{};

    for (std::uint32_t axis = 0; axis < 3u; ++axis)
    {
        const Vector3& planeNormal = axisDirs[axis];

        float rayT = 0.0f;
        const float denom = Vector3::Dot(ray.direction, planeNormal);

        // When the ring is nearly edge-on, the plane intersection is unstable
        // or fails outright. Fall back to a ray-vs-segment test against the
        // ring's screen silhouette so the handle can still be picked.
        constexpr float kEdgeOnThreshold = 0.1f;
        if (std::abs(denom) < kEdgeOnThreshold)
        {
            Vector3 silhouette = Vector3::Cross(planeNormal, ray.direction);
            const float silLen = silhouette.Length();
            if (silLen < 1e-5f) continue;
            silhouette = silhouette * (1.0f / silLen);

            // Closest point on the ray to the pivot.
            rayT = Vector3::Dot(center - ray.origin, ray.direction);
            if (rayT <= 0.0f) continue;

            const Vector3 delta = Mathematics::RayPointAt(ray, rayT) - center;

            // Project delta onto the silhouette direction to see how far along
            // the visible segment we are, and the perpendicular component tells
            // us how close we are to the segment itself.
            const float along = Vector3::Dot(delta, silhouette);
            if (std::abs(along) > radius + scaledHalfWidth) continue;

            const float perpDist = (delta - silhouette * along).Length();
            if (perpDist > scaledHalfWidth) continue;

            // The edge-on ring projects to a line segment of length 2*radius
            // through the pivot; the whole segment is the visible silhouette, so
            // keep all of it pickable (the |along| <= radius + halfWidth bound
            // above already clamps to the segment). Previously the interior was
            // excluded for 3D rings, leaving a near-edge-on ring grabbable only
            // at its two tips — a tiny, fiddly hitbox.
        }
        else
        {
            Vector3 hitPoint;
            if (!Mathematics::IntersectRayPlane(ray, center, planeNormal, rayT, hitPoint))
            {
                continue;
            }

            if (rayT <= 0.0f)
            {
                continue;
            }

            const Vector3 radial = hitPoint - center;
            const float dist = radial.Length();

            if (dist < radius - scaledHalfWidth || dist > radius + scaledHalfWidth)
            {
                continue;
            }

            // Back-facing cull: reject hits on the occluded far arc so the
            // visible front half is the only pickable region (matches Render).
            // Gated by the enhanced-gizmo setting.
            if (enhanced)
            {
                const Vector3 toCam = ray.origin - center;
                const float along = Vector3::Dot(toCam, planeNormal);
                const Vector3 d = toCam - planeNormal * along;
                const float dl = d.Length();
                if (dl > 1.0e-4f && dist > 1.0e-5f)
                {
                    const float radDot = Vector3::Dot(radial, d) / (dist * dl);
                    if (radDot < -0.12f)
                    {
                        continue;
                    }
                }
            }
        }

        if (!hasHit || rayT < bestHit.distance)
        {
            hasHit = true;
            bestHit.kind = GizmoHitKind::Axis;
            bestHit.handleId = axis;
            bestHit.distance = rayT;
        }
    }

    // Screen-space ("view-plane") ring: a camera-facing ring just outside the
    // axis rings. Its plane normal is the view direction (camera -> pivot).
    if (enhanced)
    {
        Vector3 viewDir = center - ray.origin;
        const float vl = viewDir.Length();
        if (vl > 1.0e-4f)
        {
            viewDir = viewDir * (1.0f / vl);
            const float viewRadius = radius * kViewRingRadiusScale;
            float rayT = 0.0f;
            Vector3 hitPoint;
            if (Mathematics::IntersectRayPlane(ray, center, viewDir, rayT, hitPoint) && rayT > 0.0f)
            {
                const float dist = (hitPoint - center).Length();
                if (dist >= viewRadius - scaledHalfWidth && dist <= viewRadius + scaledHalfWidth)
                {
                    if (!hasHit || rayT < bestHit.distance)
                    {
                        hasHit = true;
                        bestHit.kind = GizmoHitKind::Axis;
                        bestHit.handleId = kViewPlaneHandle;
                        bestHit.distance = rayT;
                    }
                }
            }
        }
    }

    if (!hasHit)
    {
        return result;
    }

    result.hit = true;
    result.info = bestHit;
    return result;
}

bool TransformRotateGizmo::HandlePointerEvent(const ScenePointerEvent& event,
                                              const GizmoHit& hit)
{
    if (!m_Owner)
    {
        return false;
    }

    if (!m_Owner->GetTargetEntity().IsValid())
    {
        return false;
    }

    if (event.button != PointerButton::Left)
    {
        return false;
    }

    if (event.phase == PointerPhase::Down)
    {
        if (hit.kind != GizmoHitKind::Axis)
        {
            return false;
        }

        const bool enhanced = m_Owner->GetRotateGizmoEnhanced();
        std::uint32_t axis = hit.handleId;
        const bool isViewPlane = (axis == kViewPlaneHandle) && enhanced;
        if (axis > 2u && !isViewPlane)
        {
            return false;
        }

        Vector3 normal;
        Vector3 tangent;
        Vector3 bitangent;
        bool edgeOn = false;

        if (isViewPlane)
        {
            // Screen-space ring: rotate around the camera forward axis. The
            // (right, up) frame measures the angle directly in screen space, so
            // it is never edge-on and needs no fallback.
            normal = event.cameraForward;
            tangent = event.cameraRight;
            bitangent = event.cameraUp;
        }
        else
        {
            // Get axis directions from owner (world or local space).
            GizmoAxisDirections axisDirs = kWorldAxisDirections;
            m_Owner->GetAxisDirections(axisDirs);

            // normal = the axis we're rotating around
            normal = axisDirs[axis];

            // tangent and bitangent form the plane perpendicular to the rotation
            // axis. Choose them so that (tangent, bitangent, normal) form a
            // right-handed basis, which keeps drag direction and rotation
            // direction consistent for all three rings in both axis spaces.
            //
            //   Axis 0 (X): normal = X,  tangent = Y, bitangent = Z  (Y x Z = X)
            //   Axis 1 (Y): normal = Y,  tangent = Z, bitangent = X  (Z x X = Y)
            //   Axis 2 (Z): normal = Z,  tangent = X, bitangent = Y  (X x Y = Z)
            tangent = axisDirs[(axis + 1u) % 3u];
            bitangent = axisDirs[(axis + 2u) % 3u];

            // Edge-on case: the ring plane is nearly parallel to the view
            // direction, so a plane intersection is unstable and atan2 around the
            // pivot becomes hypersensitive near the center. Substitute a
            // camera-facing plane and a view-aligned (tangent=silhouette,
            // bitangent=in-view-perp) frame; the Move handler then maps
            // perpendicular screen displacement linearly to angle.
            constexpr float kEdgeOnDragThreshold = 0.1f;
            const float denom = Vector3::Dot(event.ray.direction, normal);
            if (std::abs(denom) < kEdgeOnDragThreshold)
            {
                const Vector3& viewDir = event.ray.direction;
                Vector3 silhouette = Vector3::Cross(normal, viewDir);
                const float silLen = silhouette.Length();
                if (silLen < 1e-5f)
                {
                    return false;
                }
                silhouette = silhouette * (1.0f / silLen);
                Vector3 perpInView = Vector3::Cross(viewDir, silhouette);
                const float perpLen = perpInView.Length();
                if (perpLen < 1e-5f)
                {
                    return false;
                }
                perpInView = perpInView * (1.0f / perpLen);

                // Keep the true rotation axis (normal) so the applied rotation
                // stays around the picked ring; only the measurement frame and
                // intersection plane become camera-facing.
                tangent = silhouette;
                bitangent = perpInView;
                edgeOn = true;
            }
        }

        // Intersection plane: the rotation plane for normal rings, or a
        // camera-facing plane for the edge-on fallback (-viewDir).
        const Vector3 planeNormal = edgeOn ? (event.ray.direction * -1.0f) : normal;

        float rayT = 0.0f;
        Vector3 hitPoint;
        if (!Mathematics::IntersectRayPlane(event.ray, m_Pivot, planeNormal, rayT, hitPoint))
        {
            return false;
        }
        if (rayT <= 0.0f)
        {
            return false;
        }

        const Vector3 v = hitPoint - m_Pivot;
        const float x = Vector3::Dot(v, tangent);
        const float y = Vector3::Dot(v, bitangent);

        // Ring radius at drag start, used by the edge-on linear angle mapping.
        float dragRadius = m_Radius;
        if (m_Owner->GetConstantScreenSize())
        {
            const float distance = ComputeEffectiveGizmoDistance(m_Owner, m_Pivot, event.ray.origin);
            dragRadius = std::max(0.2f, distance * kRotateScreenBase * m_Owner->GetRotateGizmoScale());
        }

        // Linear screen-displacement mapping for edge-on drags is part of the
        // enhanced behavior; legacy mode keeps atan2 with the camera-facing plane.
        const bool useLinear = edgeOn && enhanced;

        float angle = useLinear
            ? (y / std::max(dragRadius, 1.0e-4f)) * (GameEngine::Mathematics::Pi * 0.5f)
            : std::atan2(y, x);

        m_IsDragging = true;
        m_DragKind = GizmoHitKind::Axis;
        m_ActiveAxis = axis;
        m_DragIsViewPlane = isViewPlane;
        m_EdgeOnDrag = useLinear;
        m_DragRadius = dragRadius;
        m_DragWorldAxis = normal;
        m_PlaneNormal = planeNormal;
        m_Tangent = tangent;
        m_Bitangent = bitangent;

        m_StartAngle = angle;
        m_CurrentAngle = angle;
        m_TotalAngleDelta = 0.0f;
        m_CumulativeAppliedRotation = 0.0f;
        m_Owner->BeginTransformEdit("Transform Rotate");
        return true;
    }

    if (!m_IsDragging)
    {
        return false;
    }

    if (event.phase == PointerPhase::Move)
    {
        if (m_DragKind != GizmoHitKind::Axis)
        {
            return true;
        }

        float rayT = 0.0f;
        Vector3 hitPoint;
        if (!Mathematics::IntersectRayPlane(event.ray, m_Pivot, m_PlaneNormal, rayT, hitPoint))
        {
            return true;
        }
        if (rayT <= 0.0f)
        {
            return true;
        }

        const Vector3 v = hitPoint - m_Pivot;
        const float x = Vector3::Dot(v, m_Tangent);
        const float y = Vector3::Dot(v, m_Bitangent);

        float delta = 0.0f;
        if (m_EdgeOnDrag)
        {
            // Linear screen-displacement mapping: perpendicular travel of one
            // ring radius corresponds to a quarter turn. Smooth near the pivot
            // where atan2 would otherwise be hypersensitive.
            const float angle = (y / std::max(m_DragRadius, 1.0e-4f)) * (GameEngine::Mathematics::Pi * 0.5f);
            delta = angle - m_CurrentAngle;
            m_CurrentAngle = angle;
        }
        else
        {
            float angle = std::atan2(y, x);
            delta = angle - m_CurrentAngle;

            // Ensure we take the shortest path across the wrap boundary.
            if (delta > GameEngine::Mathematics::Pi)
            {
                delta -= 2.0f * GameEngine::Mathematics::Pi;
            }
            else if (delta < -GameEngine::Mathematics::Pi)
            {
                delta += 2.0f * GameEngine::Mathematics::Pi;
            }
            m_CurrentAngle = angle;
        }

        if (std::fabs(delta) < 1.0e-4f)
        {
            return true;
        }

        m_TotalAngleDelta += delta;

        // Flip the sign so that dragging clockwise around the ring (from the
        // camera's point of view) produces a clockwise rotation of the target
        // object on screen. The underlying rotation math keeps its right-handed,
        // test-verified semantics; this inversion only affects the interaction
        // mapping.
        const float snapStep = m_Owner->GetRotateSnapIncrementRad();
        float toApply = 0.0f;
        if (event.rotateSnap45)
        {
            const float snapTotal = std::round(m_TotalAngleDelta / snapStep) * snapStep;
            toApply = -snapTotal - m_CumulativeAppliedRotation;
        }
        else
        {
            toApply = -delta;
        }

        if (std::fabs(toApply) < 1.0e-5f)
        {
            return true;
        }

        m_CumulativeAppliedRotation += toApply;
        if (m_DragIsViewPlane)
        {
            m_Owner->ApplyRotationDeltaWorldAxis(m_DragWorldAxis, toApply);
        }
        else
        {
            m_Owner->ApplyRotationDelta(m_ActiveAxis, toApply);
        }
        return true;
    }

    if (event.phase == PointerPhase::Up)
    {
        m_Owner->CommitTransformEdit();
        m_IsDragging = false;
        m_DragKind = GizmoHitKind::None;
        m_ActiveAxis = 0;
        m_DragIsViewPlane = false;
        m_EdgeOnDrag = false;
        m_StartAngle = 0.0f;
        m_CurrentAngle = 0.0f;
        m_TotalAngleDelta = 0.0f;
        m_CumulativeAppliedRotation = 0.0f;
        return true;
    }

    return false;
}

void TransformScaleGizmo::SetPivot(const Vector3& position)
{
    m_Pivot = position;
}

void TransformScaleGizmo::SetAxisLength(float length)
{
    if (length > 0.0f)
    {
        m_AxisLength = length;
    }
    else
    {
        m_AxisLength = kDefaultAxisLength;
    }
}

void TransformScaleGizmo::SetHover(const GizmoHit& hit)
{
    m_HoverKind = hit.kind;
    m_HoverHandle = hit.handleId;
}

void TransformScaleGizmo::Render(GizmoRenderContext& context)
{
    // Manipulation, not description — the axis boxes and the centre cube are
    // grab targets.
    GizmoDepthModeScope depthScope(context, GizmoDepthMode::AlwaysOnTop);

    // Compute axis length - use constant screen size if enabled
    float axisLength = m_AxisLength;
    const bool constantSize = m_Owner && m_Owner->GetConstantScreenSize();
    if (constantSize && context.HasCameraWorldPosition())
    {
        const float distance =
            ComputeEffectiveGizmoDistance(m_Owner, m_Pivot, *context.GetCameraWorldPosition());

        const float gizmoScale = m_Owner->GetScaleGizmoScale();
        axisLength = std::max(0.1f, distance * kScaleScreenBase * gizmoScale);
    }
    
    // Use appropriate thickness based on constant screen size mode.
    // When constant size is off, blend in the translate thickness to keep
    // scale lines visually consistent with the rest of the gizmo.
    float scaleThickness = m_Owner
        ? (constantSize ? m_Owner->GetScaleConstantThickness() : m_Owner->GetScaleGizmoThickness())
        : kScaleAxisThickness;
    if (m_Owner && !constantSize)
    {
        const float translateThickness = m_Owner->GetTranslateGizmoThickness();
        scaleThickness = scaleThickness * 0.7f + translateThickness * 0.3f;
    }
    const float thicknessScale = scaleThickness / 1.5f; // normalize to default
    const float handleSize = axisLength * kScaleHandleSizeFraction * thicknessScale;
    const float centerSize = axisLength * kScaleCenterHandleSizeFraction * thicknessScale;
    const float axisLineWidth = scaleThickness;

    const bool draggingAxis = m_IsDragging && m_DragKind == GizmoHitKind::Axis;
    const bool draggingUniform = m_IsDragging && m_DragKind == GizmoHitKind::Volume && m_ActiveHandle == 3u;
    const bool hoverAxis = !m_IsDragging && m_HoverKind == GizmoHitKind::Axis;
    const bool hoverUniform = !m_IsDragging && m_HoverKind == GizmoHitKind::Volume;

    const bool xHighlighted = (draggingAxis && m_ActiveHandle == 0u) || (hoverAxis && m_HoverHandle == 0u);
    const bool yHighlighted = (draggingAxis && m_ActiveHandle == 1u) || (hoverAxis && m_HoverHandle == 1u);
    const bool zHighlighted = (draggingAxis && m_ActiveHandle == 2u) || (hoverAxis && m_HoverHandle == 2u);
    const bool centerHighlighted = (draggingUniform && m_ActiveHandle == 3u) || (hoverUniform && m_HoverHandle == 3u);

    const Color& xColor = xHighlighted ? kAxisXHighlightColor : kAxisXColor;
    const Color& yColor = yHighlighted ? kAxisYHighlightColor : kAxisYColor;
    const Color& zColor = zHighlighted ? kAxisZHighlightColor : kAxisZColor;
    const Color& centerColor = centerHighlighted ? kUniformScaleHandleHighlightColor
                                                 : kUniformScaleHandleColor;

    const Vector3 origin = m_Pivot;

    GizmoAxisDirections axisDirs = kWorldAxisDirections;
    if (m_Owner)
    {
        m_Owner->GetAxisDirections(axisDirs);
    }

    context.SetTriangleLayer(1);

    // X (red), Y (green) and Z (blue) axes: a line with a cube at its end.
    const Color* axisColors[3] = {&xColor, &yColor, &zColor};
    const Vector3 handleHalfExtents(handleSize * 0.5f, handleSize * 0.5f, handleSize * 0.5f);
    for (std::uint32_t axis = 0; axis < 3u; ++axis)
    {
        const Color& color = *axisColors[axis];
        const Vector3 end = origin + axisDirs[axis] * axisLength;
        context.DrawColoredLine(origin, end, color, axisLineWidth);
        context.DrawSolidOrientedBox(end, handleHalfExtents, axisDirs, color);
    }

    // Center uniform scale cube (also oriented with object axes)
    const Vector3 centerHalfExtents(centerSize * 0.5f, centerSize * 0.5f, centerSize * 0.5f);
    context.DrawSolidOrientedBox(origin, centerHalfExtents, axisDirs, centerColor);
}

GizmoHitResult TransformScaleGizmo::HitTest(const GizmoRay& ray)
{
    GizmoHitResult result{};

    if (!m_Owner)
    {
        return result;
    }

    if (!m_Owner->GetTargetEntity().IsValid())
    {
        return result;
    }

    // Compute axis length - use constant screen size if enabled (same logic as Render)
    float axisLength = m_AxisLength;
    if (m_Owner->GetConstantScreenSize())
    {
        const float distance = ComputeEffectiveGizmoDistance(m_Owner, m_Pivot, ray.origin);
        const float gizmoScale = m_Owner->GetScaleGizmoScale();
        axisLength = std::max(0.1f, distance * kScaleScreenBase * gizmoScale);
    }

    const float handleSize = axisLength * kScaleHandleSizeFraction;
    const float centerSize = axisLength * kScaleCenterHandleSizeFraction;

    const Vector3 origin = m_Pivot;

    GizmoAxisDirections axisDirs = kWorldAxisDirections;
    m_Owner->GetAxisDirections(axisDirs);

    bool hasHit = false;
    GizmoHit bestHit{};

    auto testBox = [&](std::uint32_t handleId, GizmoHitKind kind,
                       const Vector3& center, float halfExtent)
    {
        const Vector3 half(halfExtent, halfExtent, halfExtent);
        const Mathematics::AABB box{center - half, center + half};

        float tMin = 0.0f;
        float tMax = 0.0f;
        if (!GameEngine::Mathematics::IntersectRayAABB(ray, box, tMin, tMax))
            return;

        float tHit = (tMin >= 0.0f) ? tMin : tMax;
        if (tHit < 0.0f)
            return;

        if (!hasHit || tHit < bestHit.distance)
        {
            hasHit = true;
            bestHit.kind = kind;
            bestHit.handleId = handleId;
            bestHit.distance = tHit;
        }
    };

    // Pick radius for axis line segments - already scales with distance via handleSize.
    const float axisPickRadius = handleSize * 0.6f;

    // Axis line + end handle: 0=X, 1=Y, 2=Z.
    for (std::uint32_t axis = 0; axis < 3u; ++axis)
    {
        const Vector3 endPt = origin + axisDirs[axis] * axisLength;

        // Test the cube handle at the axis tip.
        testBox(axis, GizmoHitKind::Axis, endPt, handleSize * 0.5f);

        // Test the axis line segment (closest-point distance to ray).
        float rayT = 0.0f, lineT = 0.0f;
        Vector3 onAxis;
        if (ProjectRayOntoLine(ray, origin, axisDirs[axis], rayT, lineT, onAxis) &&
            rayT > 0.0f && lineT > 0.0f && lineT < axisLength)
        {
            const float dist2 = (Mathematics::RayPointAt(ray, rayT) - onAxis).LengthSquared();
            if (dist2 <= axisPickRadius * axisPickRadius)
            {
                if (!hasHit || rayT < bestHit.distance)
                {
                    hasHit = true;
                    bestHit.kind = GizmoHitKind::Axis;
                    bestHit.handleId = axis;
                    bestHit.distance = rayT;
                }
            }
        }
    }

    // Center uniform scale handle: 3.
    testBox(3u, GizmoHitKind::Volume, origin, centerSize * 0.5f);

    // 2D mode: when the scene camera looks straight down one axis, that axis's
    // end cube sits at the same screen position as the gizmo origin but closer
    // to the camera, so it beats the center handle in the distance comparison
    // above. Mirror the Translate gizmo's 2D override — if the ray passes close
    // enough to the pivot, force the center uniform-scale handle to win.
    if (m_Owner->IsIn2DMode())
    {
        const float centerPickRadius = axisLength * kScaleCenterHandleSizeFraction;

        const float t = Vector3::Dot(origin - ray.origin, ray.direction);

        if (t > 0.0f)
        {
            const float distSq = (Mathematics::RayPointAt(ray, t) - origin).LengthSquared();

            if (distSq <= centerPickRadius * centerPickRadius)
            {
                hasHit = true;
                bestHit.kind = GizmoHitKind::Volume;
                bestHit.handleId = 3u;
                bestHit.distance = t;
            }
        }
    }

    if (!hasHit)
    {
        return result;
    }

    result.hit = true;
    result.info = bestHit;
    return result;
}

bool TransformScaleGizmo::HandlePointerEvent(const ScenePointerEvent& event,
                                             const GizmoHit& hit)
{
    if (!m_Owner)
    {
        return false;
    }

    if (!m_Owner->GetTargetEntity().IsValid())
    {
        return false;
    }

    if (event.button != PointerButton::Left)
    {
        return false;
    }

    if (event.phase == PointerPhase::Down)
    {
        if (hit.kind != GizmoHitKind::Axis && hit.kind != GizmoHitKind::Volume)
        {
            return false;
        }

        std::uint32_t handle = hit.handleId;
        if (handle > 3u)
        {
            return false;
        }

        m_IsDragging = true;
        m_ActiveHandle = handle;

        if (handle <= 2u)
        {
            // Axis scaling handle. Use the same axis directions as the gizmo
            // visuals so the drag direction matches the rendered scale handle.
            // Respects World/Local toggle.
            m_DragOrigin = m_Pivot;

            GizmoAxisDirections axisDirs = kWorldAxisDirections;
            m_Owner->GetAxisDirections(axisDirs);

            // Ensure the drag axis is normalized for stable projection.
            m_DragAxis = axisDirs[handle];
            const float lenSq = m_DragAxis.LengthSquared();
            if (lenSq > 1.0e-8f)
            {
                m_DragAxis = m_DragAxis * (1.0f / std::sqrt(lenSq));
            }

            float rayT = 0.0f;
            float lineT = 0.0f;
            Vector3 onAxis;
            if (!ProjectRayOntoLine(event.ray, m_DragOrigin, m_DragAxis, rayT, lineT, onAxis))
            {
                m_IsDragging = false;
                return false;
            }
            if (rayT <= 0.0f)
            {
                m_IsDragging = false;
                return false;
            }

            m_DragKind = GizmoHitKind::Axis;
            m_StartAxisParam = lineT;
            m_LastAxisParam = lineT;
            m_TotalAxisFactor = 1.0f; // no scale applied yet
        }
        else
        {
            // Uniform center handle.
            m_DragKind = GizmoHitKind::Volume;
            m_StartViewX = static_cast<float>(event.viewX);
            m_StartViewY = static_cast<float>(event.viewY);
            m_CurrentViewDelta = 0.0f;
        }

        m_Owner->BeginTransformEdit("Transform Scale");
        return true;
    }

    if (!m_IsDragging)
    {
        return false;
    }

    if (event.phase == PointerPhase::Move)
    {
        if (m_DragKind == GizmoHitKind::Axis)
        {
            float rayT = 0.0f;
            float lineT = 0.0f;
            Vector3 onAxis;
            if (!ProjectRayOntoLine(event.ray, m_DragOrigin, m_DragAxis, rayT, lineT, onAxis))
            {
                return true;
            }
            if (rayT <= 0.0f)
            {
                return true;
            }

            // Map pointer motion along the axis to a total scale factor based on
            // the ratio of current vs. initial projected distance. This keeps the
            // behavior intuitive even when the object becomes very small.
            if (std::fabs(m_StartAxisParam) < 1.0e-4f)
            {
                return true;
            }

            float totalFactor = std::fabs(lineT / m_StartAxisParam);
            if (totalFactor <= 0.0f)
            {
                return true;
            }

            if (m_TotalAxisFactor <= 0.0f)
            {
                m_TotalAxisFactor = 1.0f;
            }

            // Convert from desired total factor into an incremental delta factor
            // to apply this frame.
            float factor = totalFactor / m_TotalAxisFactor;
            if (factor < kScaleAxisMinFactor)
            {
                factor = kScaleAxisMinFactor;
            }
            else if (factor > kScaleAxisMaxFactor)
            {
                factor = kScaleAxisMaxFactor;
            }

            if (std::fabs(factor - 1.0f) < 1.0e-3f)
            {
                return true;
            }

            m_TotalAxisFactor *= factor;
            m_LastAxisParam = lineT;

            Vector3 deltaScale(1.0f, 1.0f, 1.0f);
            deltaScale[static_cast<int>(m_ActiveHandle)] = factor;

            m_Owner->ApplyScaleDelta(deltaScale);
            return true;
        }
        else if (m_DragKind == GizmoHitKind::Volume && m_ActiveHandle == 3u)
        {
            float dx = static_cast<float>(event.viewX) - m_StartViewX;

            float newDelta = dx; // dragging right (larger viewX) increases scale
            float deltaStep = newDelta - m_CurrentViewDelta;
            m_CurrentViewDelta = newDelta;

            // Use exponential scaling for consistent feel at all scale levels.
            // This ensures the same mouse movement produces the same *percentage*
            // change whether the object is small or large.
            float factor = std::exp(deltaStep * kUniformScaleScreenSensitivity);
            if (factor < kScaleAxisMinFactor)
            {
                factor = kScaleAxisMinFactor;
            }
            else if (factor > kScaleAxisMaxFactor)
            {
                factor = kScaleAxisMaxFactor;
            }

            if (std::fabs(factor - 1.0f) < 1.0e-3f)
            {
                return true;
            }

            m_Owner->ApplyScaleDelta(Vector3(factor, factor, factor));

            return true;
        }

        return true;
    }

    if (event.phase == PointerPhase::Up)
    {
        m_Owner->CommitTransformEdit();
        m_IsDragging = false;
        m_DragKind = GizmoHitKind::None;
        m_ActiveHandle = 0;
        m_StartAxisParam = 0.0f;
        m_LastAxisParam = 0.0f;
        m_TotalAxisFactor = 0.0f;
        m_CurrentViewDelta = 0.0f;
        return true;
    }

    return false;
}

namespace
{
using GameEngine::Editor::SceneTools::TransformAxisSpace;

// Map the user-facing axis space toggle to the **effective** axis space used by
// the gizmo visuals and transform math. We intentionally flip the semantics so
// that selecting "World" in the UI gives behaviour that previously lived under
// "Local" (object-aligned axes), and vice versa.
inline TransformAxisSpace GetEffectiveAxisSpace(TransformAxisSpace space)
{
    return (space == TransformAxisSpace::World)
               ? TransformAxisSpace::Local
               : TransformAxisSpace::World;
}
} // namespace

TransformTool::TransformTool(GameEngine::ECS::World& world) : m_World(&world)
{
    const Vector3 origin(0.0f, 0.0f, 0.0f);
    m_TranslateGizmo.SetPivot(origin);
    m_TranslateGizmo.SetAxisLength(kDefaultAxisLength);
    m_TranslateGizmo.SetOwner(this);

    m_RotateGizmo.SetPivot(origin);
    m_RotateGizmo.SetRadius(kDefaultRotationRadius);
    m_RotateGizmo.SetOwner(this);

    m_ScaleGizmo.SetPivot(origin);
    m_ScaleGizmo.SetAxisLength(kDefaultAxisLength);
    m_ScaleGizmo.SetOwner(this);

    m_HasPivot = false;
}

void TransformTool::OnActivated()
{
    // No-op for now. A later pass can reset drag state or cache selection
    // information when the tool becomes active.
}

void TransformTool::OnDeactivated()
{
    CancelDirectEntityDrag();
    CancelMarquee();
}

void TransformTool::OnPointerEvent(const ScenePointerEvent& event)
{
    // Any mode: arm a marquee on pointer-down when no transform gizmo handle
    // consumed the event (SceneToolContext routes here only in that case). The
    // marquee promotes to a drag-select once the cursor moves past a small
    // pixel threshold; a pure click (no drag) falls back to a single-entity
    // ray pick so click-to-select keeps working in Translate/Rotate/Scale.
    if (event.button == PointerButton::Left)
    {
        if (event.phase == PointerPhase::Down)
        {
            // Delegate the pick (raycast + icon test + click-through) to the
            // owner. clearOnMiss=false: an empty-space press arms a marquee
            // without deselecting; the click-commit fallback in
            // PerformEntityPick does the clearing for pure clicks.
            const ScenePickOutcome pick =
                m_ScenePick ? m_ScenePick(event, /*clearOnMiss=*/false) : ScenePickOutcome{};
            // Deferred while the scene TLAS refits: the owner applies this
            // press as a click when the refit finishes. It is neither a miss
            // (no marquee) nor a hit yet (no drag), so pointer-up has nothing
            // to finish and does not pick again.
            if (pick.Deferred)
                return;
            const GameEngine::ECS::EntityHandle picked = pick.Entity;
            const bool directDragMode =
                (m_Mode == TransformMode::Select || m_Mode == TransformMode::Translate) &&
                !event.shift && !event.ctrl && !event.alt;
            if (picked.IsValid())
            {
                if (directDragMode && HasTransformTarget())
                {
                    BeginDirectEntityDragArm(event);
                    return;
                }
                return;
            }
            BeginMarqueeArm(event);
            return;
        }
        if (event.phase == PointerPhase::Move && m_DirectDragArmed)
        {
            UpdateDirectEntityDrag(event);
            return;
        }
        if (event.phase == PointerPhase::Up && m_DirectDragArmed)
        {
            FinishDirectEntityDrag();
            return;
        }
        if (event.phase == PointerPhase::Move && m_MarqueeArmed)
        {
            UpdateMarquee(event);
            return;
        }
        if (event.phase == PointerPhase::Up && m_MarqueeArmed)
        {
            FinishMarquee(event);
            return;
        }
    }

    // For move events: perform lightweight hover hit-testing so gizmos can
    // provide visual hover feedback even when no drag is active.
    if (event.phase != PointerPhase::Move)
    {
        return;
    }

    // Without a valid pivot or target there is nothing to hover.
    if (!m_HasPivot)
    {
        return;
    }

    GizmoCollector collector;
    GatherGizmos(collector);
    if (collector.Empty())
    {
        // Clear any previous hover state if no gizmos are currently available.
        if (m_HoverGizmo)
        {
            GizmoHit clearHit{};
            if (m_HoverGizmo == &m_TranslateGizmo)
            {
                m_TranslateGizmo.SetHover(clearHit);
            }
            else if (m_HoverGizmo == &m_RotateGizmo)
            {
                m_RotateGizmo.SetHover(clearHit);
            }
            else if (m_HoverGizmo == &m_ScaleGizmo)
            {
                m_ScaleGizmo.SetHover(clearHit);
            }
            m_HoverGizmo = nullptr;
            m_HoverHit = GizmoHit{};
        }
        return;
    }

    bool hasHit = false;
    GizmoHit bestHit = {};
    IGizmo* bestGizmo = nullptr;

    for (IGizmo* gizmo : collector.GetGizmos())
    {
        if (!gizmo)
        {
            continue;
        }

        GizmoHitResult hitResult = gizmo->HitTest(event.ray);
        if (!hitResult.hit)
        {
            continue;
        }

        if (!hasHit || hitResult.info.distance < bestHit.distance)
        {
            hasHit = true;
            bestHit = hitResult.info;
            bestGizmo = gizmo;
        }
    }

    IGizmo* newHoverGizmo = hasHit ? bestGizmo : nullptr;
    GizmoHit newHoverHit = hasHit ? bestHit : GizmoHit{};

    // Early out if hover state did not actually change.
    if (newHoverGizmo == m_HoverGizmo &&
        (!newHoverGizmo || (newHoverHit.handleId == m_HoverHit.handleId &&
                            newHoverHit.kind == m_HoverHit.kind)))
    {
        return;
    }

    // Clear previous hover state.
    if (m_HoverGizmo)
    {
        GizmoHit clearHit{};
        if (m_HoverGizmo == &m_TranslateGizmo)
        {
            m_TranslateGizmo.SetHover(clearHit);
        }
        else if (m_HoverGizmo == &m_RotateGizmo)
        {
            m_RotateGizmo.SetHover(clearHit);
        }
        else if (m_HoverGizmo == &m_ScaleGizmo)
        {
            m_ScaleGizmo.SetHover(clearHit);
        }
    }

    // Apply new hover hit if any.
    if (newHoverGizmo)
    {
        if (newHoverGizmo == &m_TranslateGizmo)
        {
            m_TranslateGizmo.SetHover(newHoverHit);
        }
        else if (newHoverGizmo == &m_RotateGizmo)
        {
            m_RotateGizmo.SetHover(newHoverHit);
        }
        else if (newHoverGizmo == &m_ScaleGizmo)
        {
            m_ScaleGizmo.SetHover(newHoverHit);
        }
    }

    m_HoverGizmo = newHoverGizmo;
    m_HoverHit = newHoverHit;
}

void TransformTool::OnKeyEvent(const SceneKeyEvent& event)
{
    // Toggle World/Local via the Shortcuts catalog binding.
    if (!event.pressed)
        return;

    int mods = 0;
    if (event.shift)
        mods |= Input::kModShift;
    if (event.ctrl)
        mods |= Input::kModControl;
    if (event.alt)
        mods |= Input::kModAlt;

    if (Editor::MatchesCatalogShortcut("Transform Tool", "Toggle World/Local",
                                       static_cast<int>(event.keyCode), mods))
    {
        if (m_AxisSpace == TransformAxisSpace::World)
            SetAxisSpace(TransformAxisSpace::Local);
        else
            SetAxisSpace(TransformAxisSpace::World);
    }
}

void TransformTool::GatherGizmos(GizmoCollector& collector)
{
    // Marquee overlay always renders while active, regardless of pivot state.
    if (m_MarqueeGizmo.IsActive())
    {
        collector.AddGizmo(&m_MarqueeGizmo);
    }

    if (!m_HasPivot)
    {
        return;
    }

    if (HasExternalPointTarget())
    {
        collector.AddGizmo(&m_TranslateGizmo);
        return;
    }

    switch (m_Mode)
    {
    case TransformMode::Translate:
        collector.AddGizmo(&m_TranslateGizmo);
        break;
    case TransformMode::Rotate:
        collector.AddGizmo(&m_RotateGizmo);
        break;
    case TransformMode::Scale:
        collector.AddGizmo(&m_ScaleGizmo);
        break;
    default:
        break;
    }
}

void TransformTool::SetMode(TransformMode mode)
{
    if (m_Mode == mode)
        return;
    m_Mode = mode;
    if (m_OnModeChanged)
        m_OnModeChanged(m_Mode);
}

void TransformTool::SetAxisSpace(TransformAxisSpace space)
{
    if (m_AxisSpace == space)
        return;
    m_AxisSpace = space;
    if (m_OnAxisSpaceChanged)
        m_OnAxisSpaceChanged(m_AxisSpace);
}

void TransformTool::SetPivot(const Vector3& position)
{
    m_TranslateGizmo.SetPivot(position);
    m_RotateGizmo.SetPivot(position);
    m_ScaleGizmo.SetPivot(position);
    m_HasPivot = true;
}

void TransformTool::ClearPivot()
{
    m_HasPivot = false;
}

bool TransformTool::GetAxisDirections(GizmoAxisDirections& outAxisDirs) const
{
    // Start from canonical world axes. This guarantees a sensible default
    // even if we early-out below.
    outAxisDirs = kWorldAxisDirections;

    // Visual axis space remains intuitive:
    // - World  : canonical world basis.
    // - Local  : axes rotated by the entity's world rotation.
    // The semantic swap between World/Local is handled separately in
    // ApplyRotationDelta via GetEffectiveAxisSpace; we keep the visuals
    // unchanged.
    if (m_AxisSpace == TransformAxisSpace::World || !m_TargetEntity.IsValid())
    {
        return false;
    }

    GameEngine::ECS::World* world = &GetWorld();
    if (!world->IsValid(m_TargetEntity))
    {
        return false;
    }

    // Use the same world TRS reconstruction as the gizmo application code so
    // that the visual axes always match the transform being edited, even
    // within the same frame.
    TRS worldTrs = ComputeWorldTRSForEntity(world, m_TargetEntity);
    Quaternion worldRot = worldTrs.rotation;
    // FromTRS is LH: matrix * v == worldRot.Rotate(v). Gizmo axes follow that.
    Quaternion visualRot = worldRot;
    for (std::uint32_t axis = 0; axis < 3u; ++axis)
    {
        Vector3 localAxis(0.0f, 0.0f, 0.0f);
        localAxis[axis] = 1.0f;

        const Vector3 worldAxis = visualRot.Rotate(localAxis);
        const float lenSq = worldAxis.LengthSquared();
        if (lenSq > 1.0e-8f)
        {
            outAxisDirs[axis] = worldAxis * (1.0f / std::sqrt(lenSq));
        }
    }

    return true;
}

void TransformTool::SetTargetEntity(GameEngine::ECS::EntityHandle entity)
{
    if (entity.IsValid())
        ClearExternalPointTarget();
    m_TargetEntity = entity;
    if (!entity.IsValid())
    {
        m_SelectedEntities.clear();
        ClearTransformDragBaselines();
        return;
    }

    if (std::find(m_SelectedEntities.begin(), m_SelectedEntities.end(), entity) == m_SelectedEntities.end())
    {
        m_SelectedEntities.clear();
        m_SelectedEntities.push_back(entity);
    }
}

void TransformTool::SetExternalPointTarget(ExternalPointTarget target)
{
    // The spline tool resynchronizes its optional control-point target while
    // rendering/hovering. If an entity-root transform drag is already active,
    // do not let that resync swap the drag into point-edit mode mid-stream.
    if (m_TransformDragActive && !m_HasExternalPointTarget)
        return;

    m_ExternalPointTarget = std::move(target);
    m_HasExternalPointTarget =
        static_cast<bool>(m_ExternalPointTarget.GetWorldPosition) &&
        static_cast<bool>(m_ExternalPointTarget.ApplyTranslationDelta);
    if (m_HasExternalPointTarget)
    {
        ClearTransformDragBaselines();
        RefreshPivotFromExternalPointTarget();
    }
}

void TransformTool::ClearExternalPointTarget()
{
    const bool hadExternalPointTarget = m_HasExternalPointTarget;

    // Clearing a non-existent spline-control target is common when the spline
    // tool is active but the selected item is the spline entity itself. That
    // must not cancel an in-progress entity-root transform drag or reset its
    // accumulated drag baseline.
    if (!hadExternalPointTarget)
    {
        m_ExternalPointTarget = {};
        return;
    }

    m_ExternalPointTarget = {};
    m_HasExternalPointTarget = false;
    m_TransformDragActive = false;
    ClearTransformDragBaselines();
}

bool TransformTool::HasExternalPointTarget() const
{
    return m_HasExternalPointTarget &&
           static_cast<bool>(m_ExternalPointTarget.GetWorldPosition) &&
           static_cast<bool>(m_ExternalPointTarget.ApplyTranslationDelta);
}

bool TransformTool::HasTransformTarget() const
{
    return HasExternalPointTarget() || m_TargetEntity.IsValid();
}

void TransformTool::SetSelectedEntities(const std::vector<GameEngine::ECS::EntityHandle>& entities)
{
    m_SelectedEntities.clear();
    m_SelectedEntities.reserve(entities.size());
    for (const auto& entity : entities)
    {
        if (!entity.IsValid())
            continue;
        if (std::find(m_SelectedEntities.begin(), m_SelectedEntities.end(), entity) == m_SelectedEntities.end())
            m_SelectedEntities.push_back(entity);
    }
    if (m_TargetEntity.IsValid() &&
        std::find(m_SelectedEntities.begin(), m_SelectedEntities.end(), m_TargetEntity) == m_SelectedEntities.end())
    {
        m_SelectedEntities.push_back(m_TargetEntity);
    }
    ClearTransformDragBaselines();
}

void TransformTool::RefreshPivotFromTargetEntity()
{
    if (RefreshPivotFromExternalPointTarget())
    {
        return;
    }

    if (!m_TargetEntity.IsValid())
    {
        ClearPivot();
        return;
    }

    GameEngine::ECS::World* world = &GetWorld();
    if (!world->IsValid(m_TargetEntity))
    {
        ClearPivot();
        return;
    }

    Vector3 pivotWorld{};
    if (!TryComputeEditorPivotForEntity(world, m_TargetEntity, pivotWorld))
    {
        ClearPivot();
        return;
    }

    SetPivot(pivotWorld);
}

bool TransformTool::RefreshPivotFromExternalPointTarget()
{
    if (!HasExternalPointTarget())
        return false;

    Vector3 pivot(0.0f, 0.0f, 0.0f);
    if (!m_ExternalPointTarget.GetWorldPosition(pivot))
    {
        ClearExternalPointTarget();
        ClearPivot();
        return true;
    }
    SetPivot(pivot);
    return true;
}

void TransformTool::BeginTransformEdit(const char* name)
{
    if (m_ActiveEdit || m_TransformDragActive)
    {
        return; // already in an interactive edit (shouldn't happen, but safe)
    }

    // New drag: invalidate any cached per-drag state.
    m_TranslateCache.valid = false;
    m_TranslateCache.accumulatedWorldDelta = Vector3{};
    ClearTransformDragBaselines();

    if (HasExternalPointTarget())
    {
        m_TransformDragActive = true;
        if (m_ExternalPointTarget.BeginEdit)
            m_ExternalPointTarget.BeginEdit((name && *name) ? name : "Transform Edit");
        return;
    }

    if (!m_TargetEntity.IsValid())
    {
        return;
    }

    GameEngine::ECS::World* world = &GetWorld();
    if (!world->IsValid(m_TargetEntity))
    {
        return;
    }

    const std::vector<GameEngine::ECS::EntityHandle> targets = GetTransformEditTargets(world);
    if (targets.empty())
    {
        return;
    }
    m_TransformDragActive = true;

    if (!m_UndoRedo)
    {
        return;
    }

    const char* labelCStr = (name && *name) ? name : "Transform Edit";
    std::string label(labelCStr);

    const GameEngine::ECS::ComponentTypeId typeId =
        GameEngine::ECS::GetComponentTypeId<GameEngine::Components::Transform>();

    // Capture a stable pointer to notifications (service lifetime should outlive commands).
    EditorChangeNotifications* notifications = m_ChangeNotifications;

    UndoRedoService::SnapshotTarget target =
        MultiEntityUndo::MakeComponentSnapshotTarget(world, targets, typeId, notifications, label);

    if (!m_UndoRedo)
        return; // no undo service (a private preview world): edits apply directly
    m_ActiveEdit = m_UndoRedo->BeginInteractiveEdit(std::move(label), std::move(target));
}

void TransformTool::CommitTransformEdit()
{
    if (HasExternalPointTarget())
    {
        if (m_ExternalPointTarget.CommitEdit)
            m_ExternalPointTarget.CommitEdit();
        m_TransformDragActive = false;
        m_TranslateCache.valid = false;
        m_TranslateCache.accumulatedWorldDelta = Vector3{};
        ClearTransformDragBaselines();
        RefreshPivotFromExternalPointTarget();
        return;
    }

    if (m_ActiveEdit)
    {
        m_ActiveEdit.Commit();
    }
    m_ActiveEdit = {};
    m_TransformDragActive = false;

    // Drag ended: invalidate caches so next drag recomputes baseline.
    m_TranslateCache.valid = false;
    m_TranslateCache.accumulatedWorldDelta = Vector3{};
    ClearTransformDragBaselines();
}

std::vector<GameEngine::ECS::EntityHandle> TransformTool::GetTransformEditTargets(GameEngine::ECS::World* world) const
{
    std::vector<GameEngine::ECS::EntityHandle> candidates;
    auto addIfEditable = [&](GameEngine::ECS::EntityHandle entity)
    {
        if (!world || !entity.IsValid() || !world->IsValid(entity))
            return;
        if (!world->GetComponent<Transform>(entity))
            return;
        if (std::find(candidates.begin(), candidates.end(), entity) == candidates.end())
            candidates.push_back(entity);
    };

    for (const auto& entity : m_SelectedEntities)
        addIfEditable(entity);
    addIfEditable(m_TargetEntity);

    std::vector<GameEngine::ECS::EntityHandle> targets;
    targets.reserve(candidates.size());
    for (const auto& entity : candidates)
    {
        if (!HasAncestorInSet(world, entity, candidates))
            targets.push_back(entity);
    }
    return targets;
}

void TransformTool::ClearTransformDragBaselines()
{
    m_TransformDragBaselines.clear();
}

void TransformTool::EnsureTransformDragBaselines(GameEngine::ECS::World* world)
{
    if (!m_TransformDragBaselines.empty())
        return;

    const std::vector<GameEngine::ECS::EntityHandle> targets = GetTransformEditTargets(world);
    m_TransformDragBaselines.reserve(targets.size());
    for (const auto& entity : targets)
    {
        auto* t = world ? world->GetComponent<Transform>(entity) : nullptr;
        if (!t)
            continue;

        TransformDragBaseline base{};
        base.entity = entity;
        base.localPos = t->GetPosition();
        base.localRot = t->GetRotation();
        base.localScale = t->GetScale();

        TRS worldTrs = ComputeWorldTRSForEntity(world, entity);
        base.worldPos = worldTrs.position;
        base.worldRot = worldTrs.rotation;
        base.worldScale = worldTrs.scale;
        GetParentWorldTRS(world, entity, base.parentPos, base.parentRot, base.parentScale);
        m_TransformDragBaselines.push_back(base);
    }
}

void TransformTool::ApplyTranslationDelta(const Vector3& deltaWorld)
{
    GE_CPU_PROFILE_SCOPE("TransformTool.ApplyTranslationDelta");

    if (HasExternalPointTarget())
    {
        m_ExternalPointTarget.ApplyTranslationDelta(deltaWorld);
        RefreshPivotFromExternalPointTarget();
        return;
    }

    if (!m_TargetEntity.IsValid())
    {
        return;
    }

    GameEngine::ECS::World* world = &GetWorld();
    if (!world->IsValid(m_TargetEntity))
    {
        return;
    }

    auto apply = [&]()
    {
        EnsureTransformDragBaselines(world);
        if (m_TransformDragBaselines.empty())
            return;

        const TransformDragBaseline* primaryBase = nullptr;
        for (const auto& base : m_TransformDragBaselines)
        {
            if (base.entity == m_TargetEntity)
            {
                primaryBase = &base;
                break;
            }
        }
        if (!primaryBase)
            primaryBase = &m_TransformDragBaselines.front();

        if (!m_TranslateCache.valid)
        {
            m_TranslateCache.startPivotWorld = m_HasPivot ? m_TranslateGizmo.GetPivot() : primaryBase->worldPos;
        }
        m_TranslateCache.valid = true;
        m_TranslateCache.accumulatedWorldDelta = m_TranslateCache.accumulatedWorldDelta + deltaWorld;

        Vector3 newWorldPos = primaryBase->worldPos + m_TranslateCache.accumulatedWorldDelta;
        m_TranslateCache.worldPos = newWorldPos;

        // Snap to grid when enabled.
        if (m_SnapEnabled && m_SnapSize > 0.0f)
        {
            newWorldPos.x = std::round(newWorldPos.x / m_SnapSize) * m_SnapSize;
            newWorldPos.y = std::round(newWorldPos.y / m_SnapSize) * m_SnapSize;
            newWorldPos.z = std::round(newWorldPos.z / m_SnapSize) * m_SnapSize;
        }

    const bool trace = IsFrameTraceEnabled();
    if (trace)
    {
        const uint32_t gpuSceneFrameIndex = 0u;
        Logger::Log::Debug(
            "[FrameTrace] frame={} phase=TransformTool.ApplyTranslationDelta worldPosBefore=({},{},{}) "
            "deltaWorld=({},{},{}) worldPosAfter=({},{},{})",
            gpuSceneFrameIndex,
            m_TranslateCache.worldPos.x, m_TranslateCache.worldPos.y, m_TranslateCache.worldPos.z,
            deltaWorld.x, deltaWorld.y, deltaWorld.z,
            newWorldPos.x, newWorldPos.y, newWorldPos.z);
    }

        const Vector3 appliedWorldDelta = newWorldPos - primaryBase->worldPos;
        for (const auto& base : m_TransformDragBaselines)
        {
            const Vector3 targetWorldPos = base.worldPos + appliedWorldDelta;
            const Vector3 newLocalPos = ComputeLocalPositionFromWorld(targetWorldPos,
                                                                      base.parentPos,
                                                                      base.parentRot,
                                                                      base.parentScale);

            (void)TrySetPhysicsBodyWorldPose(world, base.entity, targetWorldPos, base.worldRot);

            Transform updated = Transform::FromTRS(newLocalPos, base.localRot, base.localScale);
            {
                GE_CPU_PROFILE_SCOPE("ECS.AddComponentImmediate.Transform");
                world->AddComponentImmediate(base.entity, updated);
            }
            PreviewWorldTransform(world, base.entity, targetWorldPos, base.worldRot, base.worldScale);
        }

        // Keep all gizmo pivots in sync with the same world-space point that
        // render gizmos use. For splines this is the first local control point
        // transformed by the current WorldTransform preview, so the root gizmo
        // and rendered curve cannot drift apart during a drag.
        Vector3 newPivotWorld = m_TranslateCache.startPivotWorld + appliedWorldDelta;
        const auto* splineAnchor = world->GetComponent<Components::SplineComponent>(m_TargetEntity);
        const bool hasRenderedAnchor =
            (splineAnchor && ECS::Entity(world, m_TargetEntity).IsEnabled<Components::SplineComponent>()) ||
            world->GetComponent<Components::MeasureComponent>(m_TargetEntity) ||
            world->GetComponent<LocalBounds>(m_TargetEntity);
        Vector3 recomputedPivotWorld{};
        if (hasRenderedAnchor && TryComputeEditorPivotForEntity(world, m_TargetEntity, recomputedPivotWorld))
            newPivotWorld = recomputedPivotWorld;
        m_TranslateGizmo.SetPivot(newPivotWorld);
        m_RotateGizmo.SetPivot(newPivotWorld);
        m_ScaleGizmo.SetPivot(newPivotWorld);
        m_HasPivot = true;
    };

    if (m_ActiveEdit)
    {
        m_ActiveEdit.Preview(apply);
    }
    else
    {
        apply();
    }
}

float TransformTool::GetRotateSnapIncrementRad() const
{
    return m_RotateSnapIncrementDeg * (GameEngine::Mathematics::Pi / 180.0f);
}

void TransformTool::ApplyRotationDelta(std::uint32_t axisIndex, float deltaRadians)
{
    if (axisIndex > 2u)
    {
        return;
    }

    if (std::fabs(deltaRadians) < 1.0e-5f)
    {
        return;
    }

    // Resolve the world-space rotation axis from the index + effective axis
    // space, then defer to the arbitrary-axis path so both the X/Y/Z rings and
    // the screen-space ring share one implementation.
    const Vector3& worldIndexAxis = kWorldAxisDirections[axisIndex];
    Vector3 axis = worldIndexAxis;
    const TransformAxisSpace effectiveSpace = GetEffectiveAxisSpace(m_AxisSpace);
    if (effectiveSpace != TransformAxisSpace::World)
    {
        GameEngine::ECS::World* world = &GetWorld();
        if (!world->IsValid(m_TargetEntity))
        {
            return;
        }
        // Local axis space: rotate around the entity's local axes, expressed in
        // world space via the current world rotation.
        const Quaternion worldRot = ComputeWorldTRSForEntity(world, m_TargetEntity).rotation;
        const Vector3 worldAxis = worldRot.Rotate(worldIndexAxis);
        const float lenSq = worldAxis.LengthSquared();
        if (lenSq > 1.0e-8f)
        {
            axis = worldAxis * (1.0f / std::sqrt(lenSq));
        }
    }

    ApplyRotationDeltaWorldAxis(axis, deltaRadians);
}

void TransformTool::ApplyRotationDeltaWorldAxis(const Vector3& worldAxis, float deltaRadians)
{
    GE_CPU_PROFILE_SCOPE("TransformTool.ApplyRotationDelta");

    if (!m_TargetEntity.IsValid())
    {
        return;
    }

    if (std::fabs(deltaRadians) < 1.0e-5f)
    {
        return;
    }

    const float axisLenSq = worldAxis.LengthSquared();
    if (axisLenSq < 1.0e-8f)
    {
        return;
    }
    const Vector3 axis = worldAxis * (1.0f / std::sqrt(axisLenSq));

    GameEngine::ECS::World* world = &GetWorld();
    if (!world->IsValid(m_TargetEntity))
    {
        return;
    }

    auto apply = [&]()
    {
        const std::vector<GameEngine::ECS::EntityHandle> targets = GetTransformEditTargets(world);
        if (targets.empty())
            return;

        TRS worldTrs = ComputeWorldTRSForEntity(world, m_TargetEntity);
        Vector3 pivotWorld = worldTrs.position;
        const bool measurePivot =
            TryComputeMeasureEndpointCenterForEntity(world, m_TargetEntity, pivotWorld);

        Quaternion deltaRot = Quaternion::FromAxisAngle(axis, deltaRadians);

        for (const auto& entity : targets)
        {
            TRS entityWorldTrs = ComputeWorldTRSForEntity(world, entity);
            Vector3 entityWorldPos = entityWorldTrs.position;
            Quaternion entityWorldRot = entityWorldTrs.rotation;
            Quaternion newWorldRot = (deltaRot * entityWorldRot).Normalized();

            Vector3 parentPos;
            Quaternion parentRot;
            Vector3 parentScale{1.0f, 1.0f, 1.0f};
            GetParentWorldTRS(world, entity, parentPos, parentRot, parentScale);

            Vector3 newLocalPos = ComputeLocalPositionFromWorld(entityWorldPos,
                                                                 parentPos,
                                                                 parentRot,
                                                                 parentScale);

            if (auto* t = world->GetComponent<Transform>(entity))
            {
                Vector3 localScale = t->GetScale();
                Quaternion parentInvRot = parentRot.Conjugated();
                Quaternion localRot = (parentInvRot * newWorldRot).Normalized();

                Transform updated = Transform::FromTRS(newLocalPos, localRot, localScale);
                {
                    GE_CPU_PROFILE_SCOPE("ECS.AddComponentImmediate.Transform");
                    world->AddComponentImmediate(entity, updated);
                }
            }

            (void)TrySetPhysicsBodyWorldPose(world, entity, entityWorldPos, newWorldRot);
        }

        if (measurePivot)
        {
            Vector3 newMeasureCenter{};
            if (TryComputeMeasureEndpointCenterForEntity(world, m_TargetEntity, newMeasureCenter))
            {
                const Vector3 offset = pivotWorld - newMeasureCenter;
                if (offset.x != 0.0f || offset.y != 0.0f || offset.z != 0.0f)
                {
                    const TRS updatedTargetTrs = ComputeWorldTRSForEntity(world, m_TargetEntity);
                    (void)MoveEntityWorldPositionPreservingRotationScale(
                        world, m_TargetEntity, updatedTargetTrs.position + offset);
                }
            }
        }

        Vector3 updatedPivot = pivotWorld;
        (void)TryComputeEditorPivotForEntity(world, m_TargetEntity, updatedPivot);
        m_TranslateGizmo.SetPivot(updatedPivot);
        m_RotateGizmo.SetPivot(updatedPivot);
        m_ScaleGizmo.SetPivot(updatedPivot);
        m_HasPivot = true;
    };

    if (m_ActiveEdit)
    {
        m_ActiveEdit.Preview(apply);
    }
    else
    {
        apply();
    }
}

void TransformTool::ApplyScaleDelta(const Vector3& deltaScale)
{
    GE_CPU_PROFILE_SCOPE("TransformTool.ApplyScaleDelta");

    if (!m_TargetEntity.IsValid())
    {
        return;
    }

    GameEngine::ECS::World* world = &GetWorld();
    if (!world->IsValid(m_TargetEntity))
    {
        return;
    }

    auto apply = [&]()
    {
        const std::vector<GameEngine::ECS::EntityHandle> targets = GetTransformEditTargets(world);
        if (targets.empty())
            return;

        Vector3 pivotWorld{};
        const bool measurePivot =
            TryComputeMeasureEndpointCenterForEntity(world, m_TargetEntity, pivotWorld);

        for (const auto& entity : targets)
        {
            auto* t = world->GetComponent<Transform>(entity);
            if (!t)
                continue;

            Vector3 localPos = t->GetPosition();
            Quaternion localRot = t->GetRotation();
            Vector3 localScale = t->GetScale();

            Vector3 newScale(
                localScale.x * deltaScale.x,
                localScale.y * deltaScale.y,
                localScale.z * deltaScale.z);

            Transform updated = Transform::FromTRS(localPos, localRot, newScale);
            {
                GE_CPU_PROFILE_SCOPE("ECS.AddComponentImmediate.Transform");
                world->AddComponentImmediate(entity, updated);
            }

            if (auto* body = world->GetComponentForWrite<PhysicsBody>(entity))
            {
                body->initialized = false;
                body->hasPrevPhysicsTransform = false;
            }
        }

        if (measurePivot)
        {
            Vector3 newMeasureCenter{};
            if (TryComputeMeasureEndpointCenterForEntity(world, m_TargetEntity, newMeasureCenter))
            {
                const Vector3 offset = pivotWorld - newMeasureCenter;
                if (offset.x != 0.0f || offset.y != 0.0f || offset.z != 0.0f)
                {
                    const TRS updatedTargetTrs = ComputeWorldTRSForEntity(world, m_TargetEntity);
                    (void)MoveEntityWorldPositionPreservingRotationScale(
                        world, m_TargetEntity, updatedTargetTrs.position + offset);
                }
            }
        }

        Vector3 updatedPivot{};
        if (!TryComputeEditorPivotForEntity(world, m_TargetEntity, updatedPivot))
            updatedPivot = ComputeWorldTRSForEntity(world, m_TargetEntity).position;
        m_TranslateGizmo.SetPivot(updatedPivot);
        m_RotateGizmo.SetPivot(updatedPivot);
        m_ScaleGizmo.SetPivot(updatedPivot);
        m_HasPivot = true;
    };

    if (m_ActiveEdit)
    {
        m_ActiveEdit.Preview(apply);
    }
    else
    {
        apply();
    }
}

void TransformTool::PerformEntityPick(const ScenePointerEvent& event)
{
    if (m_ScenePick)
        m_ScenePick(event, /*clearOnMiss=*/true);
}

void TransformTool::UpdateGizmoSizeForCamera(const Vector3& cameraPos, float baseSizeMultiplier)
{
    if (!m_ConstantScreenSize || !m_HasPivot)
        return;

    // Distance from camera to the current pivot.
    const float distance = (cameraPos - m_TranslateGizmo.GetPivot()).Length();

    // Scale gizmo size based on distance (so it appears the same screen size)
    float scaledSize = distance * baseSizeMultiplier;
    
    // Clamp to reasonable range
    scaledSize = std::max(0.1f, std::min(scaledSize, 10.0f));
    
    m_TranslateGizmo.SetAxisLength(scaledSize);
    m_RotateGizmo.SetRadius(scaledSize * 1.5f);
    m_ScaleGizmo.SetAxisLength(scaledSize);
}

void TransformTool::BeginDirectEntityDragArm(const ScenePointerEvent& event)
{
    RefreshPivotFromTargetEntity();
    if (!m_HasPivot)
        return;

    m_DirectDragPlaneOrigin = m_TranslateGizmo.GetPivot();

    Vector3 normal = event.cameraForward;
    float normalLenSq = Vector3::Dot(normal, normal);
    if (normalLenSq < 1.0e-8f)
    {
        normal = Vector3(-event.ray.direction.x, -event.ray.direction.y, -event.ray.direction.z);
        normalLenSq = Vector3::Dot(normal, normal);
    }
    if (normalLenSq < 1.0e-8f)
        normal = Vector3(0.0f, 0.0f, 1.0f);
    else
        normal = normal * (1.0f / std::sqrt(normalLenSq));
    m_DirectDragPlaneNormal = normal;

    float rayT = 0.0f;
    Vector3 hit;
    const bool hitInFront =
        GameEngine::Mathematics::IntersectRayPlane(event.ray, m_DirectDragPlaneOrigin, m_DirectDragPlaneNormal,
                                                   rayT, hit) &&
        rayT >= 0.0f;
    m_DirectDragLastPoint = hitInFront ? hit : m_DirectDragPlaneOrigin;

    m_DirectDragArmed = true;
    m_DirectDragActive = false;
    m_DirectDragDownViewX = event.viewX;
    m_DirectDragDownViewY = event.viewY;
}

void TransformTool::UpdateDirectEntityDrag(const ScenePointerEvent& event)
{
    if (!m_DirectDragArmed)
        return;

    float rayT = 0.0f;
    Vector3 hit;
    if (!GameEngine::Mathematics::IntersectRayPlane(event.ray, m_DirectDragPlaneOrigin, m_DirectDragPlaneNormal,
                                                    rayT, hit) ||
        rayT < 0.0f)
        return;

    if (!m_DirectDragActive)
    {
        const float dx = event.viewX - m_DirectDragDownViewX;
        const float dy = event.viewY - m_DirectDragDownViewY;
        constexpr float kThresholdPx = 4.0f;
        if ((dx * dx + dy * dy) < (kThresholdPx * kThresholdPx))
            return;

        BeginTransformEdit("Transform Translate");
        if (!IsInteractiveEditActive())
        {
            CancelDirectEntityDrag();
            return;
        }
        m_DirectDragActive = true;
    }

    const Vector3 deltaWorld = hit - m_DirectDragLastPoint;
    m_DirectDragLastPoint = hit;

    if (deltaWorld.x == 0.0f && deltaWorld.y == 0.0f && deltaWorld.z == 0.0f)
        return;

    ApplyTranslationDelta(deltaWorld);
}

void TransformTool::FinishDirectEntityDrag()
{
    if (m_DirectDragActive)
        CommitTransformEdit();
    m_DirectDragArmed = false;
    m_DirectDragActive = false;
}

void TransformTool::CancelDirectEntityDrag()
{
    if (m_DirectDragActive)
        CommitTransformEdit();
    m_DirectDragArmed = false;
    m_DirectDragActive = false;
}

void TransformTool::BeginMarqueeArm(const ScenePointerEvent& event)
{
    m_MarqueeArmed = true;
    m_MarqueeActive = false;
    m_MarqueeDownViewX = event.viewX;
    m_MarqueeDownViewY = event.viewY;
    m_MarqueeDownEvent = event;

    m_MarqueeScreenStartX = event.viewX;
    m_MarqueeScreenStartY = event.viewY;
    m_MarqueeScreenCurX   = event.viewX;
    m_MarqueeScreenCurY   = event.viewY;
    m_LassoScreenPoints.clear();

    // Pull current user appearance settings onto the gizmo.
    {
        const auto& settings = Editor::SceneViewSettings::Get();
        m_MarqueeGizmo.SetColor(ColorUtils::UnpackArgb(settings.GetMarqueeColor()));
        m_MarqueeGizmo.SetThickness(settings.GetMarqueeThickness());
        m_MarqueeShape = (settings.GetMarqueeShape() == Editor::MarqueeShape::Lasso) ? 1 : 0;
    }
    m_LassoPoints.clear();

    // Build a click plane perpendicular to the camera view forward so a
    // rectangle drawn in this plane's u/v basis projects as a screen-aligned
    // rectangle regardless of which off-center pixel the pointer sits on.
    const Vector3& ro = event.ray.origin;

    Vector3 fwd = event.cameraForward;
    const float fLenSq = Vector3::Dot(fwd, fwd);
    if (fLenSq < 1.0e-8f)
    {
        m_MarqueeArmed = false;
        return;
    }
    fwd = fwd * (1.0f / std::sqrt(fLenSq));
    Vector3 n = fwd * -1.0f;

    // Plane point: a fixed view-space depth in front of the camera.
    const Vector3 planePoint = ro + fwd * 50.0f;

    // Project the down pointer ray onto this plane to anchor the start.
    float rayT = 0.0f;
    if (!Mathematics::IntersectRayPlane(event.ray, planePoint, n, rayT, m_MarqueeStart) || rayT < 0.0f)
    {
        m_MarqueeArmed = false;
        return;
    }
    m_MarqueeCurrent = m_MarqueeStart;

    BuildPlaneBasis(n, m_MarqueePlaneU, m_MarqueePlaneV);
    m_MarqueePlaneNormal = n;
}

void TransformTool::UpdateMarquee(const ScenePointerEvent& event)
{
    if (!m_MarqueeArmed)
        return;

    // Promote from armed to active once the cursor moves past a small pixel
    // threshold — distinguishes a click-pick from a drag-select.
    if (!m_MarqueeActive)
    {
        const float dx = event.viewX - m_MarqueeDownViewX;
        const float dy = event.viewY - m_MarqueeDownViewY;
        constexpr float kThresholdPx = 4.0f;
        if ((dx * dx + dy * dy) < (kThresholdPx * kThresholdPx))
            return;
        m_MarqueeActive = true;
        m_MarqueeGizmo.SetActive(true);
        m_MarqueeGizmo.SetShape(m_MarqueeShape);
        if (m_MarqueeShape == 1)
        {
            m_LassoPoints.clear();
            m_LassoPoints.push_back(m_MarqueeStart);
            m_LassoScreenPoints.clear();
            m_LassoScreenPoints.emplace_back(m_MarqueeScreenStartX, m_MarqueeScreenStartY);
        }
    }

    m_MarqueeScreenCurX = event.viewX;
    m_MarqueeScreenCurY = event.viewY;

    // Project the current ray onto the click plane. Update the current marquee
    // corner in world space.
    float rayT = 0.0f;
    Vector3 hit;
    if (!GameEngine::Mathematics::IntersectRayPlane(event.ray, m_MarqueeStart, m_MarqueePlaneNormal, rayT, hit) ||
        rayT < 0.0f)
        return;
    m_MarqueeCurrent = hit;

    if (m_MarqueeShape == 0)
    {
        m_MarqueeGizmo.SetRect(m_MarqueeStart, m_MarqueeCurrent, m_MarqueePlaneU, m_MarqueePlaneV);
    }
    else
    {
        // Append the new sample only if it has moved a reasonable screen-
        // space distance from the previous sample.
        bool append = true;
        if (!m_LassoScreenPoints.empty())
        {
            const auto& last = m_LassoScreenPoints.back();
            const float dx = event.viewX - last.x;
            const float dy = event.viewY - last.y;
            if ((dx*dx + dy*dy) < 9.0f) // ~3 px min spacing
                append = false;
        }
        if (append)
        {
            m_LassoPoints.push_back(m_MarqueeCurrent);
            m_LassoScreenPoints.emplace_back(event.viewX, event.viewY);
        }
        m_MarqueeGizmo.SetLasso(m_LassoPoints);
    }
}

void TransformTool::CollectMarqueePicks(std::vector<GameEngine::ECS::EntityHandle>& picked) const
{
    picked.clear();

    const ScenePointerEvent& ev = m_MarqueeDownEvent;
    const float viewW = ev.viewW;
    const float viewH = ev.viewH;
    if (viewW <= 0.0f || viewH <= 0.0f)
        return;

    // Screen-space marquee rect, normalised so xMin <= xMax / yMin <= yMax.
    const float xMin = std::min(m_MarqueeScreenStartX, m_MarqueeScreenCurX);
    const float xMax = std::max(m_MarqueeScreenStartX, m_MarqueeScreenCurX);
    const float yMin = std::min(m_MarqueeScreenStartY, m_MarqueeScreenCurY);
    const float yMax = std::max(m_MarqueeScreenStartY, m_MarqueeScreenCurY);

    // Lasso polygon in screen space.
    const std::vector<Mathematics::Vector2>& lassoPoly = m_LassoScreenPoints;

    GameEngine::ECS::World* world = &GetWorld();

    auto query = world->Query<
        GameEngine::ECS::Read<WorldTransform>,
        GameEngine::ECS::Read<MeshRenderer>,
        GameEngine::ECS::Optional<LocalBounds>>();

    query.Each([&, this](GameEngine::ECS::EntityHandle e,
                         const WorldTransform& worldXf,
                         const MeshRenderer&,
                         const LocalBounds* bounds)
    {
        if (m_IsEntityPickable && !m_IsEntityPickable(e))
            return;

        const GameEngine::Mathematics::BoundingBox kDefault{};
        const auto box = (bounds ? bounds->Box : kDefault).TransformToAABB(worldXf.matrix);

        Mathematics::Vector2 corners[8];
        int   cornerCount = 0;
        float entXMin =  std::numeric_limits<float>::infinity();
        float entXMax = -std::numeric_limits<float>::infinity();
        float entYMin =  std::numeric_limits<float>::infinity();
        float entYMax = -std::numeric_limits<float>::infinity();

        for (const Vector3& corner : box.Corners())
        {
            Mathematics::Vector2 screen;
            if (!ProjectWorldToView(ev, corner, screen))
                continue;
            corners[cornerCount] = screen;
            ++cornerCount;
            if (screen.x < entXMin) entXMin = screen.x;
            if (screen.x > entXMax) entXMax = screen.x;
            if (screen.y < entYMin) entYMin = screen.y;
            if (screen.y > entYMax) entYMax = screen.y;
        }

        if (cornerCount == 0)
            return;

        if (m_MarqueeShape == 0)
        {
            if (entXMax < xMin || entXMin > xMax) return;
            if (entYMax < yMin || entYMin > yMax) return;
            picked.push_back(e);
        }
        else
        {
            bool hit = false;

            for (size_t pi = 0; pi < lassoPoly.size() && !hit; ++pi)
            {
                const Mathematics::Vector2& p = lassoPoly[pi];
                if (p.x >= entXMin && p.x <= entXMax &&
                    p.y >= entYMin && p.y <= entYMax)
                {
                    hit = true;
                }
            }

            for (int ci = 0; ci < cornerCount && !hit; ++ci)
            {
                if (Mathematics::PointInPolygon(corners[ci], lassoPoly))
                    hit = true;
            }

            if (!hit && !lassoPoly.empty())
            {
                float cu = 0.0f, cv = 0.0f;
                const size_t n = lassoPoly.size();
                for (const Mathematics::Vector2& p : lassoPoly)
                {
                    cu += p.x;
                    cv += p.y;
                }
                cu /= static_cast<float>(n);
                cv /= static_cast<float>(n);
                if (cu >= entXMin && cu <= entXMax &&
                    cv >= entYMin && cv <= entYMax)
                {
                    hit = true;
                }
            }

            if (hit)
                picked.push_back(e);
        }
    });
}

void TransformTool::FinishMarquee(const ScenePointerEvent& event)
{
    if (!m_MarqueeArmed)
        return;

    const bool wasActive = m_MarqueeActive;
    m_MarqueeArmed = false;
    m_MarqueeActive = false;
    m_MarqueeGizmo.SetActive(false);

    if (!wasActive)
    {
        m_LassoPoints.clear();
        m_LassoScreenPoints.clear();
        // No drag — fall through to a click-pick using the original down event.
        PerformEntityPick(m_MarqueeDownEvent);
        return;
    }

    std::vector<GameEngine::ECS::EntityHandle> picked;
    CollectMarqueePicks(picked);

    // Modifier mapping mirrors pro DCC apps: plain replaces, Shift adds,
    // Ctrl/Cmd removes. With no picks and no modifiers, the selection is
    // cleared; with modifiers and no picks, the current selection is kept.
    const bool additive = event.shift || event.ctrl || event.alt;
    if (m_OnEntitiesMarqueeCallback)
    {
        m_OnEntitiesMarqueeCallback(picked, additive);
    }
    m_LassoPoints.clear();
    m_LassoScreenPoints.clear();
}

void TransformTool::CancelMarquee()
{
    m_MarqueeArmed = false;
    m_MarqueeActive = false;
    m_MarqueeGizmo.SetActive(false);
    m_LassoPoints.clear();
    m_LassoScreenPoints.clear();
}


GameEngine::ECS::World& TransformTool::GetWorld() const
{
    return *m_World;
}

} // namespace SceneTools
} // namespace Editor
} // namespace GameEngine
