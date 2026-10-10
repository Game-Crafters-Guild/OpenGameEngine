#include "Inspectors/AmbientOcclusionEffectInspector.h"

#include "InspectorRegistry.h"

#include "Components/Rendering/PostProcessEffects/AmbientOcclusionEffect.h"
#include "Editor/Entities/EditorECSHelpers.h"

#include "Inspectors/InspectorComponentRowHelpers.h"
#include "Inspectors/InspectorDragHelpers.h"

#include <algorithm>

namespace GameEngine
{

void RegisterAmbientOcclusionEffectInspector()
{
    using namespace InspectorDrag;
    using AmbientOcclusion = Components::AmbientOcclusionEffect;

    InspectorFn fn = [](const InspectorContext& ctx)
    {
        if (!ctx.Parent || !ctx.World || !ctx.Entity.IsValid())
            return;

        auto* effect = ctx.World->GetComponent<AmbientOcclusion>(ctx.Entity);
        if (!effect)
            return;

        ECS::World* w = ctx.World;
        ECS::EntityHandle e = ctx.Entity;
        Editor::EditorChangeNotifications* n = ctx.ChangeNotifications;
        Editor::UndoRedoService* undo = ctx.Undo;

        AddComponentFloatRowWithDrag<AmbientOcclusion>(ctx.Parent, "Intensity", effect->Intensity, w, e, n,
            undo, "Change Ambient Occlusion Intensity",
            [](AmbientOcclusion& u, float v) { u.Intensity = std::max(0.0f, v); },
            1.0f, "AO strength (pow on visibility); 1 = physical, >1 darker, 0 = off");

        AddComponentFloatRowWithDrag<AmbientOcclusion>(ctx.Parent, "Radius", effect->Radius, w, e, n,
            undo, "Change Ambient Occlusion Radius",
            [](AmbientOcclusion& u, float v) { u.Radius = std::max(0.0f, v); },
            1.5f, "Sampling radius in world units (absolute - does not scale with the object; smaller hugs contacts)");

        AddComponentFloatRowWithDrag<AmbientOcclusion>(ctx.Parent, "Thickness", effect->Thickness, w, e, n,
            undo, "Change Ambient Occlusion Thickness",
            [](AmbientOcclusion& u, float v) { u.Thickness = std::max(0.0f, v); },
            1.0f, "Occluder thickness for the visibility bitmask");
    };

    InspectorRegistry::Get().RegisterComponentInspector<AmbientOcclusion>(std::move(fn));
}

} // namespace GameEngine
