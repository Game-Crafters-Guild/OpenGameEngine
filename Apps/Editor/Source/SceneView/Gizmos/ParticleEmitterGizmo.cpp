#include "SceneView/Gizmos/ParticleEmitterGizmo.h"

#include "Assets/AssetManager.h"
#include "Components/Rendering/Particles.h"
#include "Components/Transform.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "Mathematics/MatrixOps.h"
#include "Particles/Assets/ParticleStackAsset.h"
#include "Particles/Processors/ParticleCollisionProcessor.h"
#include "Particles/Processors/ParticleShapeProcessor.h"
#include "Particles/Processors/ParticleVelocityConeProcessor.h"
#include "SceneView/ComponentGizmoRegistry.h"
#include "Types/Color.h"

#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace GameEngine::Editor::SceneTools
{
namespace
{
using Mathematics::Matrix4x4;
using Mathematics::Vector3;
using namespace Particles;

const Color kEmissionColor(0.35f, 0.85f, 1.0f, 0.9f);
const Color kCollisionColor(1.0f, 0.55f, 0.2f, 0.75f);
constexpr float kEmissionThickness = 1.5f;
constexpr float kMinimumAxisLength = 1e-6f;
constexpr float kPi = 3.14159265358979323846f;
constexpr float kPointHalfExtent = 0.1f;
// Mesh shapes draw at most this many triangle outlines.
constexpr size_t kMaxMeshTriangles = 4096;

struct EmitterSpace
{
    Matrix4x4 World;
    bool Planar = false;
};

void DrawCircle(GizmoRenderContext& context, const Matrix4x4& transform, const Vector3& center, const Vector3& axisA,
                const Vector3& axisB, float radius)
{
    if (radius <= 0.0f)
        return;
    const auto worldCenter = transform.TransformPoint(center);
    const auto radiusA = transform.TransformPoint(center + axisA * radius) - worldCenter;
    const auto radiusB = transform.TransformPoint(center + axisB * radius) - worldCenter;
    context.DrawWireEllipse(worldCenter, radiusA, radiusB, kEmissionColor, kEmissionThickness);
}

void DrawLine(GizmoRenderContext& context, const Matrix4x4& transform, const Vector3& from, const Vector3& to)
{
    const auto start = transform.TransformPoint(from);
    const auto end = transform.TransformPoint(to);
    context.DrawColoredLine(start, end, kEmissionColor, kEmissionThickness);
}

void DrawMesh(GizmoRenderContext& context, const Matrix4x4& transform, const ParticleProcessorGeometry& geometry,
              bool surface)
{
    GizmoLineBatch batch(context, kEmissionColor, 1.0f);
    if (!surface)
    {
        for (const auto& vertex : geometry.Vertices)
            for (int axis = 0; axis < 3; ++axis)
            {
                Vector3 extent;
                extent[axis] = kPointHalfExtent * 0.5f;
                const auto start = transform.TransformPoint(vertex - extent);
                const auto end = transform.TransformPoint(vertex + extent);
                batch.AddLine(start, end);
            }
        return;
    }
    const size_t triangles = std::min(geometry.Indices.size() / 3, kMaxMeshTriangles);
    for (size_t triangle = 0; triangle < triangles; ++triangle)
        for (size_t corner = 0; corner < 3; ++corner)
        {
            const uint32 a = geometry.Indices[triangle * 3 + corner];
            const uint32 b = geometry.Indices[triangle * 3 + (corner + 1) % 3];
            if (a >= geometry.Vertices.size() || b >= geometry.Vertices.size())
                continue;
            const auto start = transform.TransformPoint(geometry.Vertices[a]);
            const auto end = transform.TransformPoint(geometry.Vertices[b]);
            batch.AddLine(start, end);
        }
}

// The shape of a Shape processor in the emitter's space, where the simulation samples it.
void DrawShape(GizmoRenderContext& context, const EmitterSpace& space, const ParticleProcessorInstance& processor)
{
    const auto& shape = processor.Params<ParticleShapeParameters>();
    const auto transform = space.World * Mathematics::MakeTranslation(shape.Offset);
    const Vector3 axisX{1, 0, 0}, axisY{0, 1, 0}, axisZ{0, 0, 1};
    switch (shape.Shape)
    {
    case ParticleShapeKind::Sphere:
    case ParticleShapeKind::Hemisphere:
        DrawCircle(context, transform, {}, axisX, axisY, shape.Radius);
        if (!space.Planar)
        {
            DrawCircle(context, transform, {}, axisX, axisZ, shape.Radius);
            DrawCircle(context, transform, {}, axisY, axisZ, shape.Radius);
        }
        break;
    case ParticleShapeKind::Box:
    {
        const Vector3 extents{shape.Extents.x, shape.Extents.y, space.Planar ? 0.0f : shape.Extents.z};
        const auto minimum = extents * -1.0f;
        context.DrawTransformedWireBox(minimum, extents, transform, kEmissionColor, kEmissionThickness);
        break;
    }
    case ParticleShapeKind::Circle:
        DrawCircle(context, transform, {}, axisX, axisZ, shape.Radius);
        DrawCircle(context, transform, {}, axisX, axisZ, std::min(shape.InnerRadius, shape.Radius));
        break;
    case ParticleShapeKind::Cylinder:
        for (const float side : {-0.5f, 0.5f})
        {
            DrawCircle(context, transform, axisY * (side * shape.Length), axisX, axisZ, shape.Radius);
            DrawCircle(context, transform, axisY * (side * shape.Length), axisX, axisZ,
                       std::min(shape.InnerRadius, shape.Radius));
        }
        break;
    case ParticleShapeKind::Cone:
    {
        // Apex at the origin, opening along +Y.
        const float radius = shape.Length * std::tan(std::clamp(shape.Angle, 0.0f, 89.9f) * kPi / 180.0f);
        const Vector3 rim = axisY * shape.Length;
        DrawCircle(context, transform, rim, axisX, axisZ, radius);
        for (const Vector3& side : {axisX, axisX * -1.0f, axisZ, axisZ * -1.0f})
            DrawLine(context, transform, {}, rim + side * radius);
        break;
    }
    case ParticleShapeKind::Line:
        DrawLine(context, transform, axisY * (-0.5f * shape.Length), axisY * (0.5f * shape.Length));
        break;
    case ParticleShapeKind::MeshVertices:
    case ParticleShapeKind::MeshSurface:
        DrawMesh(context, transform, processor.Geometry, shape.Shape == ParticleShapeKind::MeshSurface);
        break;
    }
}

// The launch direction and, for a narrow cone, its rim one unit out.
void DrawVelocityCone(GizmoRenderContext& context, const EmitterSpace& space, const ParticleProcessorInstance& processor)
{
    const auto& cone = processor.Params<ParticleVelocityConeParameters>();
    Vector3 direction = cone.Direction;
    if (space.Planar)
        direction.z = 0.0f;
    const float length = direction.Length();
    if (length < kMinimumAxisLength)
        return;
    direction = direction / length;
    // World cones ignore the emitter's rotation and scale.
    const Matrix4x4 transform = cone.Space == ParticleSpace::World
                                    ? Mathematics::MakeTranslation(space.World.TransformPoint({}))
                                    : space.World;
    DrawLine(context, transform, {}, direction);
    const float spread = std::clamp(cone.Spread, 0.0f, 180.0f) * kPi / 180.0f;
    if (spread <= 0.0f || spread >= kPi * 0.5f || space.Planar)
        return;
    const Vector3 helper = std::fabs(direction.y) < 0.99f ? Vector3{0, 1, 0} : Vector3{1, 0, 0};
    const Vector3 across = Vector3::Cross(helper, direction).Normalize();
    const Vector3 up = Vector3::Cross(direction, across);
    DrawCircle(context, transform, direction * std::cos(spread), across, up, std::sin(spread));
}

void DrawCollisionPlane(GizmoRenderContext& context, const EmitterSpace& space, const ParticleProcessorInstance& processor)
{
    const auto& collision = processor.Params<ParticleCollisionParameters>();
    if (collision.Source != ParticleCollisionSource::Plane)
        return;
    Vector3 normal = collision.PlaneNormal;
    normal = normal.Length() < kMinimumAxisLength ? Vector3{0, 1, 0} : normal.Normalize();
    const auto origin = space.World.TransformPoint({});
    const auto center = origin - normal * (Vector3::Dot(origin, normal) - collision.PlaneOffset);
    const auto tip = center + normal;
    context.DrawColoredLine(center, tip, kCollisionColor, 2.0f);
    Vector3 tangent{normal.y, -normal.x, 0};
    tangent = tangent.Length() < kMinimumAxisLength ? Vector3{1, 0, 0} : tangent.Normalize();
    const auto bitangent = Vector3::Cross(normal, tangent);
    constexpr int kGridHalfExtent = 2;
    GizmoLineBatch batch(context, kCollisionColor, 1.0f);
    for (int line = -kGridHalfExtent; line <= kGridHalfExtent; ++line)
    {
        const auto firstCenter = center + tangent * static_cast<float>(line);
        const auto secondCenter = center + bitangent * static_cast<float>(line);
        const auto firstStart = firstCenter - bitangent * kGridHalfExtent;
        const auto firstEnd = firstCenter + bitangent * kGridHalfExtent;
        const auto secondStart = secondCenter - tangent * kGridHalfExtent;
        const auto secondEnd = secondCenter + tangent * kGridHalfExtent;
        batch.AddLine(firstStart, firstEnd);
        batch.AddLine(secondStart, secondEnd);
    }
}

// The stack the simulation runs for the emitter: its stack asset once loaded, the default stack
// without one. Null while the asset is not loaded or holds no valid stack, as the simulation then
// runs none.
const StackDocument* SimulatedStack(const Components::ParticleEmitter3D& emitter,
                                    std::shared_ptr<const CompiledParticleStack>& keepAlive)
{
    static const StackDocument kDefault = MakeDefaultStack();
    const GUID guid = emitter.Stack.ToGuid();
    if (guid.IsNull())
        return &kDefault;
    auto* assets = EngineCore::GetInstance().TryGetAssetManager();
    const auto asset = assets ? std::dynamic_pointer_cast<ParticleStackAsset>(assets->GetAsset(guid)) : nullptr;
    keepAlive = asset ? asset->Compiled() : nullptr;
    return keepAlive ? keepAlive->Document.get() : nullptr;
}

} // namespace

void DrawParticleStackGizmo(GizmoRenderContext& context, const Components::ParticleEmitter3D& emitter,
                            const Components::WorldTransform& worldTransform, const StackDocument& document)
{
    const EmitterSpace space{Matrix4x4{glm::make_mat4(worldTransform.matrix)},
                             emitter.Dimension == Components::ParticleEmitterDimension::World2D};
    bool placed = false;
    for (const auto& phase : document.Phases)
        for (const auto& processor : phase.Processors)
        {
            if (!processor.Enabled)
                continue;
            if (processor.Descriptor == &ParticleShapeProcessor())
            {
                DrawShape(context, space, processor);
                placed = true;
            }
            else if (processor.Descriptor == &ParticleVelocityConeProcessor())
                DrawVelocityCone(context, space, processor);
            else if (processor.Descriptor == &ParticleCollisionProcessor())
                DrawCollisionPlane(context, space, processor);
        }
    if (placed)
        return;
    // Without a shape every particle starts at the emitter's origin.
    for (int axis = 0; axis < (space.Planar ? 2 : 3); ++axis)
    {
        Vector3 extent;
        extent[axis] = kPointHalfExtent;
        DrawLine(context, space.World, extent * -1.0f, extent);
    }
}

void DrawParticleEmitterGizmo(GizmoRenderContext& context, const ECS::World& world, ECS::EntityHandle entity)
{
    // An emitter switched off, itself or through an ancestor, runs no simulation and draws no gizmo.
    if (!world.IsValid(entity) || world.HasComponent<ECS::Disabled>(entity) ||
        world.HasComponent<ECS::DisabledInHierarchy>(entity))
        return;
    const auto* emitter = world.GetComponent<Components::ParticleEmitter3D>(entity);
    const auto* transform = world.GetComponent<Components::WorldTransform>(entity);
    if (!emitter || !transform)
        return;
    std::shared_ptr<const CompiledParticleStack> keepAlive;
    if (const auto* document = SimulatedStack(*emitter, keepAlive))
        DrawParticleStackGizmo(context, *emitter, *transform, *document);
}

void RegisterParticleEmitterGizmo()
{
    ComponentGizmoRegistry::Get().Register(ECS::GetComponentTypeId<Components::ParticleEmitter3D>(), DrawParticleEmitterGizmo);
}
} // namespace GameEngine::Editor::SceneTools
