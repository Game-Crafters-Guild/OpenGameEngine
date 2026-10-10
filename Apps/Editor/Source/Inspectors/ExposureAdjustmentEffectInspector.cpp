#include "Inspectors/ExposureAdjustmentEffectInspector.h"

#include "InspectorRegistry.h"

#include "Components/Rendering/PostProcessEffects/ExposureAdjustmentEffect.h"
#include "Editor/Entities/EditorECSHelpers.h"

#include "Inspectors/InspectorComponentRowHelpers.h"
#include "Inspectors/InspectorDragHelpers.h"

#include <algorithm>

namespace GameEngine
{

namespace
{
constexpr float kCompensationMin = -5.0f;
constexpr float kCompensationMax = 5.0f;
constexpr float kEvMin = -5.0f;
constexpr float kEvMax = 20.0f;
// Row reset values come straight from the component's member defaults.
constexpr Components::ExposureAdjustmentEffect kDefaults{};
} // namespace

void RegisterExposureAdjustmentEffectInspector()
{
    using namespace InspectorDrag;
    using EA = Components::ExposureAdjustmentEffect;

    InspectorFn fn = [](const InspectorContext& ctx)
    {
        if (!ctx.Parent || !ctx.World || !ctx.Entity.IsValid())
            return;

        auto* effect = ctx.World->GetComponent<EA>(ctx.Entity);
        if (!effect)
            return;

        ECS::World* w = ctx.World;
        ECS::EntityHandle e = ctx.Entity;
        Editor::EditorChangeNotifications* n = ctx.ChangeNotifications;
        Editor::UndoRedoService* undo = ctx.Undo;

        AddComponentFloatRowWithDrag<EA>(ctx.Parent, "Compensation",
            std::clamp(effect->Compensation, kCompensationMin, kCompensationMax), w, e, n,
            undo, "Change Exposure Compensation",
            [](EA& u, float v) { u.Compensation = std::clamp(v, kCompensationMin, kCompensationMax); },
            kDefaults.Compensation,
            "Exposure compensation in EV stops (positive = brighter).",
            {},
            kCompensationMin,
            kCompensationMax);

        AddToggleRow(ctx.Parent, "Clamp Min EV", effect->ClampMin,
            [w, e, n, undo](bool v) {
                CommitComponentWithUndo<EA>(w, e, n, undo, "Change Exposure Clamp Min EV",
                    [v](EA& u) { u.ClampMin = v; });
            },
            "Clamp auto exposure so it never goes below Min EV100.");

        AddComponentFloatRowWithDrag<EA>(ctx.Parent, "Min EV100",
            std::clamp(effect->MinEv, kEvMin, kEvMax), w, e, n,
            undo, "Change Exposure Min EV100",
            [](EA& u, float v) { u.MinEv = std::clamp(v, kEvMin, kEvMax); },
            kDefaults.MinEv,
            "Lower EV100 bound applied when Clamp Min EV is on.",
            {},
            kEvMin,
            kEvMax);

        AddToggleRow(ctx.Parent, "Clamp Max EV", effect->ClampMax,
            [w, e, n, undo](bool v) {
                CommitComponentWithUndo<EA>(w, e, n, undo, "Change Exposure Clamp Max EV",
                    [v](EA& u) { u.ClampMax = v; });
            },
            "Clamp auto exposure so it never goes above Max EV100.");

        AddComponentFloatRowWithDrag<EA>(ctx.Parent, "Max EV100",
            std::clamp(effect->MaxEv, kEvMin, kEvMax), w, e, n,
            undo, "Change Exposure Max EV100",
            [](EA& u, float v) { u.MaxEv = std::clamp(v, kEvMin, kEvMax); },
            kDefaults.MaxEv,
            "Upper EV100 bound applied when Clamp Max EV is on.",
            {},
            kEvMin,
            kEvMax);
    };

    InspectorRegistry::Get().RegisterComponentInspector<EA>(std::move(fn));
}

} // namespace GameEngine
