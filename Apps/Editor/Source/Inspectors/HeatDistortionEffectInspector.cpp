#include "Inspectors/HeatDistortionEffectInspector.h"

#include "InspectorRegistry.h"

#include "Components/Rendering/PostProcessEffects/HeatDistortionEffect.h"
#include "Editor/Entities/EditorECSHelpers.h"

#include "Inspectors/InspectorComponentRowHelpers.h"
#include "Inspectors/InspectorDragHelpers.h"

#include <algorithm>

namespace GameEngine
{

void RegisterHeatDistortionEffectInspector()
{
    using namespace InspectorDrag;
    using HD = Components::HeatDistortionEffect;

    InspectorFn fn = [](const InspectorContext& ctx)
    {
        if (!ctx.Parent || !ctx.World || !ctx.Entity.IsValid())
            return;

        auto* effect = ctx.World->GetComponent<HD>(ctx.Entity);
        if (!effect)
            return;

        ECS::World* w = ctx.World;
        ECS::EntityHandle e = ctx.Entity;
        Editor::EditorChangeNotifications* n = ctx.ChangeNotifications;
        Editor::UndoRedoService* undo = ctx.Undo;

        AddComponentFloatRowWithDrag<HD>(ctx.Parent, "Strength", effect->Strength, w, e, n,
            undo, "Change Heat Distortion Strength",
            [](HD& u, float v) { u.Strength = std::max(v, 0.0f); },
            8.0f,
            "Maximum screen-space distortion in pixels.",
            {},
            0.0f,
            32.0f);

        AddComponentFloatRowWithDrag<HD>(ctx.Parent, "Speed", effect->Speed, w, e, n,
            undo, "Change Heat Distortion Speed",
            [](HD& u, float v) { u.Speed = v; },
            1.15f,
            "Animation speed for vertically rising mirage shimmer.");

        AddComponentFloatRowWithDrag<HD>(ctx.Parent, "Scale", effect->Scale, w, e, n,
            undo, "Change Heat Distortion Scale",
            [](HD& u, float v) { u.Scale = std::max(v, 0.01f); },
            11.0f,
            "Screen-space tiling scale for the shimmer.",
            {},
            0.01f,
            80.0f);

        AddComponentFloatRowWithDrag<HD>(ctx.Parent, "Mask Strength", effect->MaskStrength, w, e, n,
            undo, "Change Heat Distortion Mask",
            [](HD& u, float v) { u.MaskStrength = std::max(v, 0.0f); },
            1.0f,
            "Multiplier for depth and view-angle masking.",
            {},
            0.0f,
            4.0f);

        AddComponentFloatRowWithDrag<HD>(ctx.Parent, "Distance Start", effect->DistanceStart, w, e, n,
            undo, "Change Heat Distortion Distance Start",
            [](HD& u, float v) { u.DistanceStart = std::max(v, 0.0f); },
            0.02f,
            "Normalized depth where distortion starts.",
            {},
            0.0f,
            1.0f);

        AddComponentFloatRowWithDrag<HD>(ctx.Parent, "Distance End", effect->DistanceEnd, w, e, n,
            undo, "Change Heat Distortion Distance End",
            [](HD& u, float v) { u.DistanceEnd = std::max(v, 0.001f); },
            0.14f,
            "Normalized depth where distortion reaches full strength.",
            {},
            0.0f,
            1.0f);

        AddComponentFloatRowWithDrag<HD>(ctx.Parent, "View Falloff", effect->DirectionalFalloff, w, e, n,
            undo, "Change Heat Distortion View Falloff",
            [](HD& u, float v) { u.DirectionalFalloff = std::max(v, 0.0f); },
            4.0f,
            "Suppresses distortion on steep view angles.",
            {},
            0.0f,
            12.0f);

        AddToggleRow(ctx.Parent, "Absolute Y", effect->UseAbsoluteY,
            [w, e, n, undo](bool v) {
                CommitComponentWithUndo<HD>(w, e, n, undo, "Change Heat Distortion View Mode",
                    [v](HD& u) { u.UseAbsoluteY = v; });
            });

        AddComponentFloatRowWithDrag<HD>(ctx.Parent, "Softness", effect->Softness, w, e, n,
            undo, "Change Heat Distortion Softness",
            [](HD& u, float v) { u.Softness = std::clamp(v, 0.0f, 3.0f); },
            1.0f,
            "Blends in a soft local blur to hide blocky shimmer edges.",
            {},
            0.0f,
            3.0f);
    };

    InspectorRegistry::Get().RegisterComponentInspector<HD>(std::move(fn));
}

} // namespace GameEngine
