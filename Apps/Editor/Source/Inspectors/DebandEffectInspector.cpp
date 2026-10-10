#include "Inspectors/DebandEffectInspector.h"

#include "InspectorRegistry.h"

#include "Components/Rendering/PostProcessEffects/DebandEffect.h"
#include "Editor/Entities/EditorECSHelpers.h"

#include "Inspectors/InspectorComponentRowHelpers.h"

#include <algorithm>

namespace GameEngine
{

void RegisterDebandEffectInspector()
{
    using namespace InspectorDrag;
    using Deband = Components::DebandEffect;

    InspectorFn fn = [](const InspectorContext& ctx)
    {
        if (!ctx.Parent || !ctx.World || !ctx.Entity.IsValid())
            return;

        auto* effect = ctx.World->GetComponent<Deband>(ctx.Entity);
        if (!effect)
            return;

        ECS::World* w = ctx.World;
        ECS::EntityHandle e = ctx.Entity;
        Editor::EditorChangeNotifications* n = ctx.ChangeNotifications;
        Editor::UndoRedoService* undo = ctx.Undo;

        // Enabled is the section header's dot (ECS::ComponentFlags::KeepsOwnEnabledField),
        // so it has no row here.
        AddComponentFloatRowWithDrag<Deband>(ctx.Parent, "Threshold (LSB)",
            std::clamp(effect->ThresholdLsb, 0.0f, 16.0f), w, e, n, undo,
            "Change Deband Threshold",
            [](Deband& u, float v) { u.ThresholdLsb = std::clamp(v, 0.0f, 16.0f); },
            6.0f,
            "Gate in output LSBs: banding steps up to roughly this size smooth out; "
            "anything larger is treated as a real edge and left untouched. 6 covers "
            "8-bit-authored gradients under exposure gain; higher gates risk eating "
            "texture detail. 0 = off.",
            {}, 0.0f, 16.0f);
    };

    InspectorRegistry::Get().RegisterComponentInspector<Deband>(std::move(fn));
}

} // namespace GameEngine
