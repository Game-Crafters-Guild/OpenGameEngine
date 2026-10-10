#include "Inspectors/FastBlurEffectInspector.h"

#include "InspectorRegistry.h"

#include "Components/Rendering/PostProcessEffects/FastBlurEffect.h"
#include "Editor/Entities/EditorECSHelpers.h"

#include "Inspectors/InspectorComponentRowHelpers.h"
#include "Inspectors/InspectorDragHelpers.h"

#include <algorithm>

namespace GameEngine
{

void RegisterFastBlurEffectInspector()
{
    using namespace InspectorDrag;
    using FB = Components::FastBlurEffect;

    InspectorFn fn = [](const InspectorContext& ctx)
    {
        if (!ctx.Parent || !ctx.World || !ctx.Entity.IsValid())
            return;

        auto* effect = ctx.World->GetComponent<FB>(ctx.Entity);
        if (!effect)
            return;

        ECS::World* w = ctx.World;
        ECS::EntityHandle e = ctx.Entity;
        Editor::EditorChangeNotifications* n = ctx.ChangeNotifications;
        Editor::UndoRedoService* undo = ctx.Undo;

        AddComponentFloatRowWithDrag<FB>(ctx.Parent, "Intensity", effect->Intensity, w, e, n,
            undo, "Change Fast Blur Intensity",
            [](FB& u, float v) { u.Intensity = std::clamp(v, 0.0f, 1.0f); },
            0.0f,
            "0 = off, 1 = full depth-of-field blur.",
            {},
            0.0f,
            1.0f);

        AddComponentFloatRowWithDrag<FB>(ctx.Parent, "Focus Distance", effect->FocusDistance, w, e, n,
            undo, "Change Fast Blur Focus Distance",
            [](FB& u, float v) { u.FocusDistance = std::max(v, 0.01f); },
            12.0f,
            "View-space distance that remains sharp.",
            {},
            0.01f,
            10000.0f);

        AddComponentFloatRowWithDrag<FB>(ctx.Parent, "Focus Range", effect->FocusRange, w, e, n,
            undo, "Change Fast Blur Focus Range",
            [](FB& u, float v) { u.FocusRange = std::max(v, 0.01f); },
            6.0f,
            "Width of the sharp band around focus distance.",
            {},
            0.01f,
            10000.0f);

        AddComponentFloatRowWithDrag<FB>(ctx.Parent, "Max Radius", effect->MaxRadius, w, e, n,
            undo, "Change Fast Blur Max Radius",
            [](FB& u, float v) { u.MaxRadius = std::clamp(v, 0.0f, 24.0f); },
            6.0f,
            "Maximum blur radius in pixels. Small values keep the pass cheap.",
            {},
            0.0f,
            24.0f);

        AddToggleRow(ctx.Parent, "Near Blur", effect->NearBlur,
            [w, e, n, undo](bool v) {
                CommitComponentWithUndo<FB>(w, e, n, undo, "Change Fast Blur Near Blur",
                    [v](FB& u) { u.NearBlur = v; });
            },
            "Blur foreground objects closer than the focus distance.");
    };

    InspectorRegistry::Get().RegisterComponentInspector<FB>(std::move(fn));
}

} // namespace GameEngine
