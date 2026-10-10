#include "Inspectors/VolumetricCloudsInspector.h"

#include "InspectorRegistry.h"

#include "Components/Rendering/PostProcessEffects/VolumetricClouds.h"
#include "Components/Rendering/PostProcessEffects/VolumetricCloudsPresets.h"
#include "Editor/Entities/EditorECSHelpers.h"

#include "Inspectors/InspectorComponentRowHelpers.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "UndoRedo/UndoRedoService.h"

#include <algorithm>

namespace GameEngine
{

using Components::VolumetricCloudsPreset;

static constexpr EnumEntry<VolumetricCloudsPreset> kCloudPresets[] = {
    {VolumetricCloudsPreset::Custom, "Custom"},
    {VolumetricCloudsPreset::FairWeatherCumulus, "Fair Weather Cumulus"},
    {VolumetricCloudsPreset::Altocumulus, "Altocumulus (Mackerel)"},
    {VolumetricCloudsPreset::StratusOvercast, "Stratus Overcast"},
    {VolumetricCloudsPreset::Thunderhead, "Thunderhead"},
    {VolumetricCloudsPreset::CirrusVeil, "Cirrus Veil"},
    {VolumetricCloudsPreset::BrokenCeiling, "Broken Ceiling"},
};

void RegisterVolumetricCloudsInspector()
{
    using namespace InspectorDrag;
    using Clouds = Components::VolumetricClouds;

    InspectorFn fn = [](const InspectorContext& ctx)
    {
        if (!ctx.Parent || !ctx.World || !ctx.Entity.IsValid())
            return;

        auto* clouds = ctx.World->GetComponent<Clouds>(ctx.Entity);
        if (!clouds)
        {
            InspectorUI::AddLine(ctx.Parent, "(VolumetricClouds missing)");
            return;
        }

        ECS::World* w = ctx.World;
        ECS::EntityHandle e = ctx.Entity;
        Editor::EditorChangeNotifications* n = ctx.ChangeNotifications;
        Editor::UndoRedoService* undo = ctx.Undo;

        auto* presetField = InspectorUI::AddEnumRow(ctx.Parent, "Preset", kCloudPresets, clouds->Preset,
            "Start from a named sky, then tune.");
        if (presetField)
        {
            presetField->SetOnValueChanged([w, e, n, undo](VolumetricCloudsPreset preset) {
                CommitComponentWithUndo<Clouds>(w, e, n, undo, "Change Clouds Preset",
                    [preset](Clouds& u) {
                        if (preset == VolumetricCloudsPreset::Custom)
                        {
                            u.Preset = VolumetricCloudsPreset::Custom;
                            return;
                        }
                        Components::ApplyVolumetricCloudsPreset(u, preset);
                    });
            });
        }

        // Volume (viewer-centered disc)
        AddComponentFloatRowWithDrag<Clouds>(ctx.Parent, "Radius", clouds->Radius, w, e, n,
            undo, "Change Clouds Radius",
            [](Clouds& u, float v) { u.Radius = std::max(1.0f, v); },
            9000.0f, "How far the cloud disc extends around the viewer", {}, 100.0f, 50000.0f);

        AddComponentFloatRowWithDrag<Clouds>(ctx.Parent, "Altitude", clouds->Altitude, w, e, n,
            undo, "Change Clouds Altitude",
            [](Clouds& u, float v) { u.Altitude = v; },
            900.0f, "World height of the underside of the layer", {}, -1000.0f, 20000.0f);

        AddComponentFloatRowWithDrag<Clouds>(ctx.Parent, "Thickness", clouds->Thickness, w, e, n,
            undo, "Change Clouds Thickness",
            [](Clouds& u, float v) { u.Thickness = std::max(1.0f, v); },
            1400.0f, "Vertical depth of the layer", {}, 1.0f, 20000.0f);

        // March
        AddComponentIntRowWithDrag<Clouds>(ctx.Parent, "NumStepsLight", clouds->NumStepsLight, w, e, n,
            undo, "Change Clouds Light Steps",
            [](Clouds& u, int v) { u.NumStepsLight = std::clamp(v, 1, 64); },
            8, "Samples along the sun ray per march step");

        AddComponentFloatRowWithDrag<Clouds>(ctx.Parent, "StepSize", clouds->StepSize, w, e, n,
            undo, "Change Clouds Step Size",
            [](Clouds& u, float v) { u.StepSize = std::clamp(v, 0.5f, 1000.0f); },
            11.0f, "World-space march step length", {}, 0.5f, 200.0f);

        AddComponentFloatRowWithDrag<Clouds>(ctx.Parent, "RayOffsetStrength", clouds->RayOffsetStrength, w, e, n,
            undo, "Change Clouds Ray Offset",
            [](Clouds& u, float v) { u.RayOffsetStrength = std::max(0.0f, v); },
            10.0f, "Jitter on the march start; hidden by the temporal blend", {}, 0.0f, 50.0f);

        // Base shape
        AddComponentFloatRowWithDrag<Clouds>(ctx.Parent, "CloudScale", clouds->CloudScale, w, e, n,
            undo, "Change Cloud Scale",
            [](Clouds& u, float v) { u.CloudScale = std::max(0.01f, v); },
            0.18f, "Cloud size: lower = larger, fewer cloud forms", {}, 0.01f, 5.0f);

        AddComponentFloatRowWithDrag<Clouds>(ctx.Parent, "DensityMultiplier", clouds->DensityMultiplier, w, e, n,
            undo, "Change Clouds Density Multiplier",
            [](Clouds& u, float v) { u.DensityMultiplier = std::max(0.0f, v); },
            1.0f, "Overall density scale; 0 disables the effect", {}, 0.0f, 10.0f);

        AddComponentFloatRowWithDrag<Clouds>(ctx.Parent, "DensityOffset", clouds->DensityOffset, w, e, n,
            undo, "Change Clouds Density Offset",
            [](Clouds& u, float v) { u.DensityOffset = v; },
            -5.2f, "Coverage: more negative = fewer, more separated clouds", {}, -10.0f, 10.0f);

        const char* axisNames[3] = {"X", "Y", "Z"};
        for (int i = 0; i < 3; ++i)
        {
            AddComponentFloatRowWithDrag<Clouds>(ctx.Parent, std::string("ShapeOffset") + axisNames[i],
                clouds->ShapeOffset[i], w, e, n,
                undo, "Change Clouds Shape Offset",
                [i](Clouds& u, float v) { u.ShapeOffset[i] = v; },
                0.0f, "Manual scroll offset of the shape noise");
        }

        const char* channelNames[4] = {"R", "G", "B", "A"};
        for (int i = 0; i < 4; ++i)
        {
            AddComponentFloatRowWithDrag<Clouds>(ctx.Parent, std::string("ShapeNoiseWeight") + channelNames[i],
                clouds->ShapeNoiseWeights[i], w, e, n,
                undo, "Change Clouds Shape Weights",
                [i](Clouds& u, float v) { u.ShapeNoiseWeights[i] = std::max(0.0f, v); },
                i == 0 ? 1.0f : 0.0f, "Blend weight of this shape noise octave channel", {}, 0.0f, 4.0f);
        }

        // Detail
        AddComponentFloatRowWithDrag<Clouds>(ctx.Parent, "DetailNoiseScale", clouds->DetailNoiseScale, w, e, n,
            undo, "Change Clouds Detail Scale",
            [](Clouds& u, float v) { u.DetailNoiseScale = std::max(0.01f, v); },
            10.0f, "Detail noise tiling relative to the shape noise", {}, 0.01f, 100.0f);

        AddComponentFloatRowWithDrag<Clouds>(ctx.Parent, "DetailNoiseWeight", clouds->DetailNoiseWeight, w, e, n,
            undo, "Change Clouds Detail Weight",
            [](Clouds& u, float v) { u.DetailNoiseWeight = std::max(0.0f, v); },
            0.1f, "Strength of edge erosion by the detail noise", {}, 0.0f, 5.0f);

        for (int i = 0; i < 3; ++i)
        {
            AddComponentFloatRowWithDrag<Clouds>(ctx.Parent, std::string("DetailNoiseWeight") + channelNames[i],
                clouds->DetailNoiseWeights[i], w, e, n,
                undo, "Change Clouds Detail Weights",
                [i](Clouds& u, float v) { u.DetailNoiseWeights[i] = std::max(0.0f, v); },
                i == 0 ? 1.0f : 0.5f, "Blend weight of this detail noise channel", {}, 0.0f, 4.0f);
        }

        for (int i = 0; i < 3; ++i)
        {
            AddComponentFloatRowWithDrag<Clouds>(ctx.Parent, std::string("DetailOffset") + axisNames[i],
                clouds->DetailOffset[i], w, e, n,
                undo, "Change Clouds Detail Offset",
                [i](Clouds& u, float v) { u.DetailOffset[i] = v; },
                0.0f, "Manual scroll offset of the detail noise");
        }

        // Lighting
        AddComponentFloatRowWithDrag<Clouds>(ctx.Parent, "AbsorptionThroughCloud", clouds->LightAbsorptionThroughCloud, w, e, n,
            undo, "Change Clouds View Absorption",
            [](Clouds& u, float v) { u.LightAbsorptionThroughCloud = std::max(0.0f, v); },
            1.0f, "Extinction along the view ray (Beer's law)", {}, 0.0f, 5.0f);

        AddComponentFloatRowWithDrag<Clouds>(ctx.Parent, "AbsorptionTowardSun", clouds->LightAbsorptionTowardSun, w, e, n,
            undo, "Change Clouds Sun Absorption",
            [](Clouds& u, float v) { u.LightAbsorptionTowardSun = std::max(0.0f, v); },
            1.0f, "Extinction along the light march toward the sun", {}, 0.0f, 5.0f);

        AddComponentFloatRowWithDrag<Clouds>(ctx.Parent, "DarknessThreshold", clouds->DarknessThreshold, w, e, n,
            undo, "Change Clouds Darkness Threshold",
            [](Clouds& u, float v) { u.DarknessThreshold = std::clamp(v, 0.0f, 1.0f); },
            0.2f, "Floor on shadowed cloud brightness", {}, 0.0f, 1.0f);

        AddComponentFloatRowWithDrag<Clouds>(ctx.Parent, "ForwardScattering", clouds->ForwardScattering, w, e, n,
            undo, "Change Clouds Forward Scattering",
            [](Clouds& u, float v) { u.ForwardScattering = std::clamp(v, 0.0f, 1.0f); },
            0.83f, "Henyey-Greenstein forward lobe (silver lining toward the sun)", {}, 0.0f, 1.0f);

        AddComponentFloatRowWithDrag<Clouds>(ctx.Parent, "BackScattering", clouds->BackScattering, w, e, n,
            undo, "Change Clouds Back Scattering",
            [](Clouds& u, float v) { u.BackScattering = std::clamp(v, 0.0f, 1.0f); },
            0.3f, "Henyey-Greenstein backward lobe", {}, 0.0f, 1.0f);

        AddComponentFloatRowWithDrag<Clouds>(ctx.Parent, "BaseBrightness", clouds->BaseBrightness, w, e, n,
            undo, "Change Clouds Base Brightness",
            [](Clouds& u, float v) { u.BaseBrightness = std::clamp(v, 0.0f, 1.0f); },
            0.8f, "Phase-independent brightness floor", {}, 0.0f, 1.0f);

        AddComponentFloatRowWithDrag<Clouds>(ctx.Parent, "PhaseFactor", clouds->PhaseFactor, w, e, n,
            undo, "Change Clouds Phase Factor",
            [](Clouds& u, float v) { u.PhaseFactor = std::clamp(v, 0.0f, 1.0f); },
            0.15f, "How strongly the phase function modulates brightness", {}, 0.0f, 1.0f);

        // Animation
        AddComponentFloatRowWithDrag<Clouds>(ctx.Parent, "TimeScale", clouds->TimeScale, w, e, n,
            undo, "Change Clouds Time Scale",
            [](Clouds& u, float v) { u.TimeScale = v; },
            1.0f, "Global animation speed (0 freezes the clouds)");

        AddComponentFloatRowWithDrag<Clouds>(ctx.Parent, "BaseSpeed", clouds->BaseSpeed, w, e, n,
            undo, "Change Clouds Base Speed",
            [](Clouds& u, float v) { u.BaseSpeed = v; },
            1.0f, "Shape noise scroll speed");

        AddComponentFloatRowWithDrag<Clouds>(ctx.Parent, "DetailSpeed", clouds->DetailSpeed, w, e, n,
            undo, "Change Clouds Detail Speed",
            [](Clouds& u, float v) { u.DetailSpeed = v; },
            2.0f, "Detail noise scroll speed");

        AddComponentFloatRowWithDrag<Clouds>(ctx.Parent, "HistoryWeight", clouds->HistoryWeight, w, e, n,
            undo, "Change Clouds History Weight",
            [](Clouds& u, float v) { u.HistoryWeight = std::clamp(v, 0.0f, 0.98f); },
            0.85f, "Temporal accumulation strength (0 = no history reuse)", {}, 0.0f, 0.98f);
    };

    InspectorRegistry::Get().RegisterComponentInspector<Clouds>(std::move(fn));
}

} // namespace GameEngine
