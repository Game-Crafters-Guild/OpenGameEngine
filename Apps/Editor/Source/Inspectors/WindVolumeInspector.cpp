#include "Inspectors/WindVolumeInspector.h"

#include "Components/Rendering/WindVolume.h"
#include "Editor/Entities/EditorECSHelpers.h"
#include "InspectorRegistry.h"
#include "Inspectors/InspectorComponentRowHelpers.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "SceneView/WindVolumeGizmo.h"

#include <algorithm>

namespace GameEngine
{
namespace
{

using Components::WindVolumeBlendMode;
using Components::WindVolumeShape;

static constexpr EnumEntry<WindVolumeShape> kWindVolumeShapes[] = {
    {WindVolumeShape::Box, "Box"},
    {WindVolumeShape::Sphere, "Sphere"},
    {WindVolumeShape::Capsule, "Capsule"},
    {WindVolumeShape::Cylinder, "Cylinder"},
};

static constexpr EnumEntry<WindVolumeBlendMode> kWindVolumeBlendModes[] = {
    {WindVolumeBlendMode::Additive, "Additive"},
    {WindVolumeBlendMode::Override, "Override"},
};

} // namespace

void RegisterWindVolumeInspector()
{
    using namespace InspectorDrag;
    using Wind = Components::WindVolume;

    Editor::SceneTools::RegisterWindVolumeGizmo();

    InspectorRegistry::Get().RegisterComponentInspector<Wind>(
        [](const InspectorContext& ctx)
        {
            if (!ctx.Parent || !ctx.World || !ctx.Entity.IsValid())
                return;

            auto* wind = ctx.World->GetComponent<Wind>(ctx.Entity);
            if (!wind)
            {
                InspectorUI::AddLine(ctx.Parent, "(WindVolume missing)");
                return;
            }

            ECS::World* w = ctx.World;
            ECS::EntityHandle e = ctx.Entity;
            Editor::EditorChangeNotifications* n = ctx.ChangeNotifications;
            Editor::UndoRedoService* undo = ctx.Undo;

            AddToggleRow(ctx.Parent, "Global", wind->IsGlobal,
                [w, e, n, undo](bool v)
                {
                    CommitComponentWithUndo<Wind>(w, e, n, undo, "Change Wind Volume Global",
                        [v](Wind& u) { u.IsGlobal = v; });
                },
                "When enabled, this wind affects the whole world. When disabled, Transform controls the volume bounds.");

            auto* blendMode = InspectorUI::AddEnumRow(ctx.Parent, "Blend Mode", kWindVolumeBlendModes, wind->BlendMode,
                "Additive adds this wind to lower-priority wind. Override blends toward this wind.");
            blendMode->SetOnValueChanged([w, e, n, undo](WindVolumeBlendMode v)
            {
                CommitComponentWithUndo<Wind>(w, e, n, undo, "Change Wind Volume Blend Mode",
                    [v](Wind& u) { u.BlendMode = v; });
            });

            AddComponentIntRowWithDrag<Wind>(ctx.Parent, "Priority", wind->Priority, w, e, n, undo,
                "Change Wind Volume Priority",
                [](Wind& u, int32_t v) { u.Priority = v; },
                0, "Higher-priority override volumes are applied after lower-priority volumes.");

            AddComponentFloatRowWithDrag<Wind>(ctx.Parent, "Weight", wind->Weight, w, e, n, undo,
                "Change Wind Volume Weight",
                [](Wind& u, float v) { u.Weight = std::clamp(v, 0.0f, 1.0f); },
                1.0f, "Volume influence [0..1].");

            AddComponentIntRowWithDrag<Wind>(ctx.Parent, "Layer Mask", static_cast<int>(wind->LayerMask), w, e, n, undo,
                "Change Wind Volume Layer Mask",
                [](Wind& u, int v) { u.LayerMask = static_cast<uint32>(v); },
                static_cast<int>(0xFFFFFFFFu), "Wind consumer mask. -1 affects all consumers.");

            InspectorUI::AddTextBlock(ctx.Parent, "Bounds", "inspector-section-subheader");
            auto* shape = InspectorUI::AddEnumRow(ctx.Parent, "Shape", kWindVolumeShapes, wind->Shape,
                "Bounding shape for local wind volumes. Transform controls orientation and scale.");
            shape->SetOnValueChanged([w, e, n, undo](WindVolumeShape v)
            {
                CommitComponentWithUndo<Wind>(w, e, n, undo, "Change Wind Volume Shape",
                    [v](Wind& u) { u.Shape = v; });
            });

            AddComponentFloatRowWithDrag<Wind>(ctx.Parent, "Blend Distance", wind->BlendDistance, w, e, n, undo,
                "Change Wind Volume Blend Distance",
                [](Wind& u, float v) { u.BlendDistance = std::max(0.0f, v); },
                2.0f, "World-space fade region outside the local volume.");

            InspectorUI::AddTextBlock(ctx.Parent, "Wind", "inspector-section-subheader");
            AddComponentFloatRowWithDrag<Wind>(ctx.Parent, "Direction X", wind->DirectionX, w, e, n, undo,
                "Change Wind Volume Direction X",
                [](Wind& u, float v) { u.DirectionX = v; });
            AddComponentFloatRowWithDrag<Wind>(ctx.Parent, "Direction Y", wind->DirectionY, w, e, n, undo,
                "Change Wind Volume Direction Y",
                [](Wind& u, float v) { u.DirectionY = v; });
            AddComponentFloatRowWithDrag<Wind>(ctx.Parent, "Direction Z", wind->DirectionZ, w, e, n, undo,
                "Change Wind Volume Direction Z",
                [](Wind& u, float v) { u.DirectionZ = v; });
            AddComponentFloatRowWithDrag<Wind>(ctx.Parent, "Speed", wind->Speed, w, e, n, undo,
                "Change Wind Volume Speed",
                [](Wind& u, float v) { u.Speed = std::max(0.0f, v); },
                1.0f);
            AddComponentFloatRowWithDrag<Wind>(ctx.Parent, "Turbulence", wind->Turbulence, w, e, n, undo,
                "Change Wind Volume Turbulence",
                [](Wind& u, float v) { u.Turbulence = std::max(0.0f, v); },
                0.35f);
            AddComponentFloatRowWithDrag<Wind>(ctx.Parent, "Gust Frequency", wind->GustFrequency, w, e, n, undo,
                "Change Wind Volume Gust Frequency",
                [](Wind& u, float v) { u.GustFrequency = std::max(0.0f, v); },
                0.5f);
            AddComponentFloatRowWithDrag<Wind>(ctx.Parent, "Gust Scale", wind->GustScale, w, e, n, undo,
                "Change Wind Volume Gust Scale",
                [](Wind& u, float v) { u.GustScale = std::max(0.001f, v); },
                70.0f);
        });
}

} // namespace GameEngine
