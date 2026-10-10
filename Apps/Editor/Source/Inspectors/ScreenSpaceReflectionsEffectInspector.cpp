#include "Inspectors/ScreenSpaceReflectionsEffectInspector.h"

#include "InspectorRegistry.h"
#include "Components/Rendering/PostProcessEffects/ScreenSpaceReflectionsEffect.h"
#include "Editor/Entities/EditorECSHelpers.h"
#include "Inspectors/InspectorComponentRowHelpers.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "UI/Controls/EnumField.h"

#include <algorithm>
#include <limits>

namespace GameEngine
{
namespace
{
constexpr EnumEntry<Components::SssrSampleQuality> kSampleQualities[] = {
    {Components::SssrSampleQuality::Low, "Low"},
    {Components::SssrSampleQuality::Medium, "Medium"},
    {Components::SssrSampleQuality::High, "High"},
};
} // namespace

void RegisterScreenSpaceReflectionsEffectInspector()
{
    using namespace InspectorDrag;
    using SSSR = Components::ScreenSpaceReflectionsEffect;
    constexpr float kNoUpperBound = std::numeric_limits<float>::infinity();

    InspectorRegistry::Get().RegisterComponentInspector<SSSR>(
        [](const InspectorContext& ctx)
        {
            if (!ctx.Parent || !ctx.World || !ctx.Entity.IsValid())
                return;
            const auto* effect = ctx.World->GetComponent<SSSR>(ctx.Entity);
            if (!effect)
                return;
            auto* w = ctx.World;
            const auto e = ctx.Entity;
            auto* n = ctx.ChangeNotifications;
            auto* undo = ctx.Undo;

            AddComponentFloatRowWithDrag<SSSR>(
                ctx.Parent, "Intensity", effect->Intensity, w, e, n, undo, "Change SSSR Intensity",
                [](SSSR& s, float v) { s.Intensity = std::clamp(v, 0.0f, SSSR::kIntensityMax); },
                0.75f, "Strength of the confidence-weighted reflection composite. 0 disables the pass.",
                {}, 0.0f, SSSR::kIntensityMax);
            AddComponentFloatRowWithDrag<SSSR>(
                ctx.Parent, "Max Distance", effect->MaxDistance, w, e, n, undo,
                "Change SSSR Max Distance",
                [](SSSR& s, float v) { s.MaxDistance = std::max(v, 0.0f); }, 100.0f,
                "Longest reflection ray, in world units.", {}, 0.0f, kNoUpperBound);
            AddComponentFloatRowWithDrag<SSSR>(
                ctx.Parent, "Thickness", effect->Thickness, w, e, n, undo, "Change SSSR Thickness",
                [](SSSR& s, float v) { s.Thickness = std::max(v, SSSR::kMinThickness); }, 0.2f,
                "Depth tolerance for accepting a hit. Raise it for thin or distant geometry.", {},
                SSSR::kMinThickness, kNoUpperBound);
            AddComponentFloatRowWithDrag<SSSR>(
                ctx.Parent, "Edge Fade", effect->EdgeFade, w, e, n, undo, "Change SSSR Edge Fade",
                [](SSSR& s, float v)
                { s.EdgeFade = std::clamp(v, SSSR::kMinEdgeFade, SSSR::kMaxEdgeFade); },
                0.08f, "Fades reflection confidence toward the screen boundary, where rays run out of scene.",
                {}, SSSR::kMinEdgeFade, SSSR::kMaxEdgeFade);
            AddComponentIntRowWithDrag<SSSR>(
                ctx.Parent, "Max Steps", effect->MaxSteps, w, e, n, undo, "Change SSSR Max Steps",
                [](SSSR& s, int v) { s.MaxSteps = std::clamp(v, SSSR::kMinSteps, SSSR::kMaxSteps); },
                48, "Hierarchical traversal budget per reflected pixel.");

            auto* qualityField =
                InspectorUI::AddEnumRow(ctx.Parent, "Sample Quality", kSampleQualities, effect->SampleQuality,
                           "Stochastic ray density. Low quarters the trace rate on rough surfaces; "
                           "High traces near full rate for less noise ahead of the denoiser.");
            qualityField->SetOnValueChanged(
                [w, e, n, undo](Components::SssrSampleQuality v)
                {
                    CommitComponentWithUndo<SSSR>(w, e, n, undo, "Change SSSR Sample Quality",
                                                  [v](SSSR& s) { s.SampleQuality = v; });
                });

            AddToggleRow(
                ctx.Parent, "Multi Bounce", effect->MultiBounce,
                [w, e, n, undo](bool v)
                {
                    CommitComponentWithUndo<SSSR>(w, e, n, undo, "Change SSSR Multi Bounce",
                                                  [v](SSSR& s) { s.MultiBounce = v; });
                },
                "Reflections within reflections, read from the previous frame's composite. Off keeps "
                "the crisp direct mirror; curved surfaces can read soft and bright with it on.");
        });
}

} // namespace GameEngine
