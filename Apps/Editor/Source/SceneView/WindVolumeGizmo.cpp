#include "SceneView/WindVolumeGizmo.h"

#include "Components/Rendering/WindVolume.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "SceneView/ComponentGizmoRegistry.h"
#include "SceneView/LocalVolumeGizmo.h"

namespace GameEngine::Editor::SceneTools
{

using GameEngine::Components::WindVolume;
using GameEngine::Components::WorldTransform;

namespace
{

VolumeShape ToVolumeShape(GameEngine::Components::WindVolumeShape shape)
{
    switch (shape)
    {
    case GameEngine::Components::WindVolumeShape::Sphere: return VolumeShape::Sphere;
    case GameEngine::Components::WindVolumeShape::Capsule: return VolumeShape::Capsule;
    case GameEngine::Components::WindVolumeShape::Cylinder: return VolumeShape::Cylinder;
    case GameEngine::Components::WindVolumeShape::Box: break;
    }
    return VolumeShape::Box;
}

void DrawWindVolume(GizmoRenderContext& context, const GameEngine::ECS::World& world,
                    GameEngine::ECS::EntityHandle entity)
{
    const Color kInnerColor(0.25f, 0.72f, 1.0f, 0.95f);
    const Color kBlendColor(0.25f, 0.72f, 1.0f, 0.32f);

    const auto* volume = world.GetComponent<WindVolume>(entity);
    const auto* xf = world.GetComponent<WorldTransform>(entity);
    if (!volume || !xf || volume->IsGlobal)
        return;
    DrawLocalVolume(context, *xf, ToVolumeShape(volume->Shape), volume->BlendDistance, kInnerColor, kBlendColor);
}

} // namespace

void RegisterWindVolumeGizmo()
{
    ComponentGizmoRegistry::Get().Register(GameEngine::ECS::GetComponentTypeId<WindVolume>(), DrawWindVolume);
}

} // namespace GameEngine::Editor::SceneTools
