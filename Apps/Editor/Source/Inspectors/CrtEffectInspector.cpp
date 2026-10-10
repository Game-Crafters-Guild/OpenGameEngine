#include "Inspectors/CrtEffectInspector.h"

#include "InspectorRegistry.h"

#include "Components/Rendering/PostProcessEffects/CrtEffect.h"
#include "Editor/Entities/EditorECSHelpers.h"

#include "Inspectors/InspectorComponentRowHelpers.h"
#include "Inspectors/InspectorDragHelpers.h"

#include <algorithm>
#include <cmath>

namespace GameEngine
{

void RegisterCrtEffectInspector()
{
    using namespace InspectorDrag;
    using CRT = Components::CrtEffect;

    InspectorFn fn = [](const InspectorContext& ctx)
    {
        if (!ctx.Parent || !ctx.World || !ctx.Entity.IsValid())
            return;

        auto* effect = ctx.World->GetComponent<CRT>(ctx.Entity);
        if (!effect)
            return;

        ECS::World* w = ctx.World;
        ECS::EntityHandle e = ctx.Entity;
        Editor::EditorChangeNotifications* n = ctx.ChangeNotifications;
        Editor::UndoRedoService* undo = ctx.Undo;

        AddComponentFloatRowWithDrag<CRT>(ctx.Parent, "Intensity", effect->Intensity, w, e, n,
            undo, "Change CRT Intensity",
            [](CRT& u, float v) { u.Intensity = std::clamp(v, 0.0f, 1.0f); },
            0.05f, "Overall CRT blend (0 = off)",
            {},
            0.0f,
            1.0f);

        AddComponentFloatRowWithDrag<CRT>(ctx.Parent, "Curvature", effect->Curvature, w, e, n,
            undo, "Change CRT Curvature",
            [](CRT& u, float v) { u.Curvature = std::clamp(v, 0.0f, 1.5f); },
            0.02f, "Screen barrel warp strength",
            {},
            0.0f,
            1.5f);

        AddComponentFloatRowWithDrag<CRT>(ctx.Parent, "Scanlines", effect->Scanlines, w, e, n,
            undo, "Change CRT Scanlines",
            [](CRT& u, float v) { u.Scanlines = std::clamp(v, 0.0f, 1.0f); },
            0.05f, "Horizontal scanline visibility",
            {},
            0.0f,
            1.0f);

        AddComponentFloatRowWithDrag<CRT>(ctx.Parent, "Vignette", effect->Vignette, w, e, n,
            undo, "Change CRT Vignette",
            [](CRT& u, float v) { u.Vignette = std::clamp(v, 0.0f, 2.0f); },
            0.05f, "Edge darkening toward tube corners",
            {},
            0.0f,
            2.0f);

        AddComponentFloatRowWithDrag<CRT>(ctx.Parent, "Aberration", effect->Aberration, w, e, n,
            undo, "Change CRT Chromatic Aberration",
            [](CRT& u, float v) { u.Aberration = std::clamp(v, 0.0f, 0.05f); },
            0.00025f,
            "Red/blue channel separation in UV (small values)",
            {},
            0.0f,
            0.05f);

        AddComponentFloatRowWithDrag<CRT>(ctx.Parent, "Softness",
            std::clamp(std::isfinite(effect->Softness) ? effect->Softness : 0.0f, 0.0f, 1.0f),
            w, e, n,
            undo, "Change CRT Softness",
            [](CRT& u, float v) {
                u.Softness = std::clamp(std::isfinite(v) ? v : 0.0f, 0.0f, 1.0f);
            },
            0.05f,
            "Phosphor blend softness (0 = sharp, 1 = soft)",
            {},
            0.0f,
            1.0f);

        AddToggleRow(ctx.Parent, "ExposureCompensation", effect->ExposureCompensation,
            [w, e, n, undo](bool v) {
                CommitComponentWithUndo<CRT>(w, e, n, undo, "Change CRT Exposure Compensation",
                    [v](CRT& u) { u.ExposureCompensation = v; });
            },
            "Auto-raises CRT brightness from scanline intensity to preserve perceived exposure.");

        AddComponentFloatRowWithDrag<CRT>(
            ctx.Parent,
            "EmulatedResolutionDiv",
            effect->EmulatedResolutionDiv,
            w,
            e,
            n,
            undo,
            "Change CRT emulated-resolution divisor",
            [](CRT& u, float v) { u.EmulatedResolutionDiv = std::max(0.0f, std::min(v, 4096.0f)); },
            0.5f,
            "0 = Lottes default (6×). Larger matches integer-scaled artwork; set equal to Scene View "
            "pixel-perfect scale when not using automatic 2D override.",
            {},
            0.0f,
            4096.0f);
    };

    InspectorRegistry::Get().RegisterComponentInspector<CRT>(std::move(fn));
}

} // namespace GameEngine
