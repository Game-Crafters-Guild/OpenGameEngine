#include "Inspectors/ShadowSettingsEffectInspector.h"

#include "InspectorRegistry.h"
#include "Core/Engine.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/ShadowMapRenderFeature.h"

#include "Components/Rendering/PostProcessEffects/ShadowSettingsEffect.h"
#include "Editor/Entities/EditorECSHelpers.h"

#include "Inspectors/InspectorComponentRowHelpers.h"
#include "Inspectors/InspectorDragHelpers.h"

#include <algorithm>
#include <charconv>
#include <vector>

namespace GameEngine
{

void RegisterShadowSettingsEffectInspector()
{
    using namespace InspectorDrag;
    using Shadow = Components::ShadowSettingsEffect;

    InspectorFn fn = [](const InspectorContext& ctx)
    {
        if (!ctx.Parent || !ctx.World || !ctx.Entity.IsValid())
            return;

        auto* effect = ctx.World->GetComponent<Shadow>(ctx.Entity);
        if (!effect)
            return;

        ECS::World* w = ctx.World;
        ECS::EntityHandle e = ctx.Entity;
        Editor::EditorChangeNotifications* n = ctx.ChangeNotifications;
        Editor::UndoRedoService* undo = ctx.Undo;

        auto* rs = EngineCore::GetInstance().GetRenderServices();
        auto* feature = rs ? rs->GetFeature<Engine::Renderer::ShadowMapRenderFeature>() : nullptr;
        if (feature)
        {
            using DebugMode = Engine::Renderer::ShadowDebugMode;
            static const std::vector<Dropdown::Option> kDebugOptions = {
                {"0", "Off"}, {"1", "Cascade Colors"},
                {"2", "Combined Shadow Mask"}, {"3", "PCSS Branches"},
                {"4", "Normal Shadow Mask"}, {"5", "Contact Shadow Mask"},
                {"6", "Contact Only (Lit)"}, {"7", "Normal Only (Lit)"},
            };
            auto* debug = InspectorUI::AddDropdownRow(ctx.Parent, "Shadow Debug", kDebugOptions,
                static_cast<int>(feature->GetDebugMode()),
                "Temporary view for all volumes. Masks: black = shadow, white = lit. "
                "Normal means the selected Cascades or Ray Traced source. Contact requires "
                "Screen-space Shadows enabled. Not saved with the scene.");
            debug->SetOnValueChanged([](const std::string& value)
            {
                int selected = 0;
                const auto parsed = std::from_chars(value.data(), value.data() + value.size(), selected);
                if (parsed.ec != std::errc{}) return;
                auto* services = EngineCore::GetInstance().GetRenderServices();
                if (auto* f = services ? services->GetFeature<Engine::Renderer::ShadowMapRenderFeature>() : nullptr)
                    f->SetDebugMode(static_cast<DebugMode>(std::clamp(selected, 0,
                        static_cast<int>(DebugMode::Count) - 1)));
            });
        }

        // Enabled is the section header's dot (ECS::ComponentFlags::KeepsOwnEnabledField),
        // so it has no row here.
        static const std::vector<Dropdown::Option> kModeOptions = {
            {"0", "Cascades"},
            {"1", "Ray Traced (Experimental)"},
        };
        auto* mode = InspectorUI::AddDropdownRow(
            ctx.Parent, "Mode", kModeOptions, static_cast<int>(effect->Mode),
            "Directional shadow sampling for opaque receivers. Cascades = shadow maps "
            "(default). Ray Traced = per-pixel ray-query mask: hard edges, no "
            "peter-panning; applies live, no restart. Experimental gaps: skinned and "
            "terrain casters don't cast, cutout foliage casts solid, and transparent "
            "receivers keep cascades. While casters are moving the mask rebuilds and "
            "those frames render cascades. Falls back to Cascades on GPUs without "
            "ray query.");
        mode->SetOnValueChanged([w, e, n, undo](const std::string& value)
            {
                int selected = 0;
                const auto parsed =
                    std::from_chars(value.data(), value.data() + value.size(), selected);
                if (parsed.ec != std::errc{})
                    return;
                CommitComponentWithUndo<Shadow>(w, e, n, undo, "Change Shadow Mode",
                    [selected](Shadow& u)
                    {
                        u.Mode = static_cast<Components::DirectionalShadowMode>(std::clamp(
                            selected, 0, static_cast<int>(Shadow::kDirectionalShadowModeLast)));
                    });
                // Rebuild: the rows below are gated on the mode.
                if (n)
                {
                    Editor::EditorChangeNotifications::ComponentChangedEvent ev{};
                    ev.world = w;
                    ev.entity = e;
                    ev.componentType = ECS::GetComponentTypeId<Shadow>();
                    ev.kind = Editor::EditorChangeNotifications::ChangeKind::InspectorRebuild;
                    n->NotifyComponentChanged(ev);
                }
            });

        // Cascades still render under RayTraced (transmissive receivers sample
        // them), so every cascade row stays visible in both modes. Only the ray
        // budget has no effect under Cascades and is hidden there.
        const bool rayTraced = effect->Mode == Components::DirectionalShadowMode::RayTraced;

        static const std::vector<Dropdown::Option> kFilterOptions = {
            {"0", "Grid 5x5 PCF"},
            {"1", "Grid 3x3 PCF"},
            {"2", "Poisson PCF"},
            {"3", "PCSS"},
            {"4", "MSM4"},
            {"5", "Dilated PCF"},
        };
        auto* filter = InspectorUI::AddDropdownRow(
            ctx.Parent, "Filter", kFilterOptions, static_cast<int>(effect->Filter),
            "Cascade shadow filter (Cascades mode). PCF grids and Poisson give a fixed "
            "penumbra; PCSS derives a contact-hardening penumbra from the light's angular "
            "diameter; MSM4 is a moment-based prefiltered soft shadow; Dilated PCF "
            "approximates contact hardening from one tap set at a lower cost than PCSS. "
            "PCSS and MSM4 fall back automatically on GPUs that lack their prerequisites.");
        filter->SetOnValueChanged([w, e, n, undo](const std::string& value)
            {
                int selected = 0;
                const auto parsed =
                    std::from_chars(value.data(), value.data() + value.size(), selected);
                if (parsed.ec != std::errc{})
                    return;
                CommitComponentWithUndo<Shadow>(w, e, n, undo, "Change Shadow Filter",
                    [selected](Shadow& u)
                    {
                        u.Filter = static_cast<Components::DirectionalShadowFilter>(std::clamp(
                            selected, 0, static_cast<int>(Shadow::kDirectionalShadowFilterLast)));
                    });
            });

        if (rayTraced)
        {
            static const std::vector<Dropdown::Option> kQualityOptions = {
                {"0", "Performance"},
                {"1", "Quality"},
            };
            auto* quality = InspectorUI::AddDropdownRow(
                ctx.Parent, "Ray Traced Quality", kQualityOptions,
                static_cast<int>(effect->RayTracedQuality),
                "Ray budget of the Ray Traced mask. Performance = 1 ray per pixel, penumbra "
                "resolved by a wide spatial denoise (and TAA where the view has it). Quality = "
                "4 rays per pixel and half the denoise footprint: the physical 0.53 deg sun and "
                "thin contact shadows resolve in place. Ignored under Cascades.");
            quality->SetOnValueChanged([w, e, n, undo](const std::string& value)
                {
                    int selected = 0;
                    const auto parsed =
                        std::from_chars(value.data(), value.data() + value.size(), selected);
                    if (parsed.ec != std::errc{})
                        return;
                    CommitComponentWithUndo<Shadow>(w, e, n, undo, "Change Ray Traced Quality",
                        [selected](Shadow& u)
                        {
                            u.RayTracedQuality = static_cast<Components::RayTracedShadowQuality>(
                                std::clamp(selected, 0,
                                           static_cast<int>(Shadow::kRayTracedShadowQualityLast)));
                        });
                });
        }

        AddToggleRow(ctx.Parent, "Screen-space Shadows", effect->ScreenSpaceShadows,
            [w, e, n, undo](bool enabled)
            {
                CommitComponentWithUndo<Shadow>(w, e, n, undo, "Toggle Screen-space Shadows",
                    [enabled](Shadow& shadow) { shadow.ScreenSpaceShadows = enabled; });
            },
            "Add sun contact and detail shadows from visible depth using compute. "
            "Offscreen occluders still need shadow maps. Perspective views only.");
        AddComponentFloatRowWithDrag<Shadow>(ctx.Parent, "Screen-space Thickness",
            effect->ScreenSpaceShadowThickness, w, e, n, undo, "Change Screen-space Shadow Thickness",
            [](Shadow& u, float v) { u.ScreenSpaceShadowThickness = std::clamp(v, 0.0001f, 0.05f); },
            0.005f, "Assumed thickness of each traced surface as a fraction of its distance "
            "from the camera: 0.005 is about 5 cm at 10 m. Higher values fill gaps but thicken silhouettes.",
            {}, 0.0001f, 0.05f);

        AddComponentFloatRowWithDrag<Shadow>(ctx.Parent, "Max Shadow Distance",
            effect->MaxShadowDistance, w, e, n, undo, "Change Max Shadow Distance",
            [](Shadow& u, float v) { u.MaxShadowDistance = std::max(0.01f, v); },
            200.0f,
            "Far distance (world units) at which directional shadows fade out. Shorter = sharper "
            "near shadows and a large GPU saving (tighter cascades).",
            {}, 0.01f);

        AddComponentFloatRowWithDrag<Shadow>(ctx.Parent, "Distance Fade Fraction",
            std::clamp(effect->DistanceFadeFraction, 0.0f, Shadow::kDistanceFadeFractionMax), w, e, n,
            undo, "Change Shadow Distance Fade Fraction",
            [](Shadow& u, float v)
            { u.DistanceFadeFraction = std::clamp(v, 0.0f, Shadow::kDistanceFadeFractionMax); },
            0.1f,
            "Fraction of Max Shadow Distance over which directional shadows fade out: 0.1 fades "
            "over the last tenth, 0 ends them on a hard edge, 0.5 (the most) over the far half.",
            {}, 0.0f, Shadow::kDistanceFadeFractionMax);

        AddComponentFloatRowWithDrag<Shadow>(ctx.Parent, "Split Lambda",
            std::clamp(effect->SplitLambda, 0.0f, 1.0f), w, e, n, undo, "Change Shadow Split Lambda",
            [](Shadow& u, float v) { u.SplitLambda = std::clamp(v, 0.0f, 1.0f); },
            0.75f,
            "Cascade split blend: 0 = uniform, 1 = logarithmic (packs resolution nearer the camera).",
            {}, 0.0f, 1.0f);

        AddComponentFloatRowWithDrag<Shadow>(ctx.Parent, "Depth Bias",
            effect->DepthBias, w, e, n, undo, "Change Shadow Depth Bias",
            [](Shadow& u, float v) { u.DepthBias = std::clamp(v, 0.0f, 0.01f); },
            0.0001f,
            "NDC receiver depth bias added before the reverse-Z shadow comparison (anti-acne). "
            "Sane values are tiny (shipped: 0.0001); large values detach shadows.",
            {}, 0.0f, 0.01f);

        AddComponentFloatRowWithDrag<Shadow>(ctx.Parent, "Normal Bias",
            effect->NormalBias, w, e, n, undo, "Change Shadow Normal Bias",
            [](Shadow& u, float v) { u.NormalBias = std::clamp(v, 0.0f, 5.0f); },
            0.5f,
            "Maximum world-space normal offset used to prevent speckling on slopes. "
            "The renderer reduces it for finer shadows to preserve contact detail.",
            {}, 0.0f, 5.0f);
    };

    InspectorRegistry::Get().RegisterComponentInspector<Shadow>(std::move(fn));
}

} // namespace GameEngine
