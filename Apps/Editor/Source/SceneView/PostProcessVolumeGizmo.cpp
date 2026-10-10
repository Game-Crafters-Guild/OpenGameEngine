#include "SceneView/PostProcessVolumeGizmo.h"

#include "Components/Rendering/PostProcessVolume.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "SceneView/ComponentGizmoRegistry.h"
#include "SceneView/LocalVolumeGizmo.h"

namespace GameEngine::Editor::SceneTools
{

using GameEngine::Components::PostProcessVolume;
using GameEngine::Components::WorldTransform;

namespace
{

VolumeShape ToVolumeShape(GameEngine::Components::PostProcessVolumeShape shape)
{
    switch (shape)
    {
    case GameEngine::Components::PostProcessVolumeShape::Sphere: return VolumeShape::Sphere;
    case GameEngine::Components::PostProcessVolumeShape::Capsule: return VolumeShape::Capsule;
    case GameEngine::Components::PostProcessVolumeShape::Cylinder: return VolumeShape::Cylinder;
    case GameEngine::Components::PostProcessVolumeShape::Box: break;
    }
    return VolumeShape::Box;
}

void DrawPostProcessVolume(GizmoRenderContext& context, const GameEngine::ECS::World& world,
                           GameEngine::ECS::EntityHandle entity)
{
    const Color kInnerColor(1.0f, 0.55f, 0.15f, 0.95f);
    const Color kBlendColor(1.0f, 0.55f, 0.15f, 0.35f);

    const auto* volume = world.GetComponent<PostProcessVolume>(entity);
    const auto* xf = world.GetComponent<WorldTransform>(entity);
    if (!volume || !xf || volume->IsGlobal)
        return;
    DrawLocalVolume(context, *xf, ToVolumeShape(volume->Shape), volume->BlendDistance, kInnerColor, kBlendColor);
}

} // namespace

void RegisterPostProcessVolumeGizmo()
{
    ComponentGizmoRegistry::Get().Register(GameEngine::ECS::GetComponentTypeId<PostProcessVolume>(), DrawPostProcessVolume);
}

} // namespace GameEngine::Editor::SceneTools
