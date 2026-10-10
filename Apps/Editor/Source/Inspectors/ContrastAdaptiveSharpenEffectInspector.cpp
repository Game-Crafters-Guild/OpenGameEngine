#include "Inspectors/ContrastAdaptiveSharpenEffectInspector.h"

#include "InspectorRegistry.h"

#include "Components/Rendering/PostProcessEffects/ContrastAdaptiveSharpenEffect.h"
#include "Editor/Entities/EditorECSHelpers.h"

#include "Inspectors/InspectorComponentRowHelpers.h"
#include "Inspectors/InspectorDragHelpers.h"

#include <algorithm>

namespace GameEngine
{

void RegisterContrastAdaptiveSharpenEffectInspector()
{
    using namespace InspectorDrag;
    using CAS = Components::ContrastAdaptiveSharpenEffect;
    constexpr float kCasMin = 0.0f;
    constexpr float kCasMax = 1.0f;
    constexpr float kCasDefault = 0.35f;

    InspectorFn fn = [](const InspectorContext& ctx)
    {
        if (!ctx.Parent || !ctx.World || !ctx.Entity.IsValid())
            return;

        auto* effect = ctx.World->GetComponent<CAS>(ctx.Entity);
        if (!effect)
            return;

        ECS::World* w = ctx.World;
        ECS::EntityHandle e = ctx.Entity;
        Editor::EditorChangeNotifications* n = ctx.ChangeNotifications;
        Editor::UndoRedoService* undo = ctx.Undo;

        AddComponentFloatRowWithDrag<CAS>(ctx.Parent, "Strength",
            std::clamp(effect->Strength, 0.0f, 1.0f), w, e, n,
            undo, "Change CAS Strength",
            [](CAS& u, float v) { u.Strength = std::clamp(v, 0.0f, 1.0f); },
            kCasDefault,
            "Contrast Adaptive Sharpening amount (recommended range 0.2-0.6)",
            {},
            kCasMin,
            kCasMax);
    };

    InspectorRegistry::Get().RegisterComponentInspector<CAS>(std::move(fn));
}

} // namespace GameEngine
