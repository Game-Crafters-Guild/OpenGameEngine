#include "Inspectors/SplineFenceOverrideFields.h"

#include "Inspectors/InspectorDragHelpers.h"

#include "Assets/AssetRegistry.h"
#include "Components/Spline/SplinePoolSelection.h"
#include "Core/Engine.h"

#include <array>

namespace GameEngine::Editor
{
namespace
{

using Components::SplineSpanOverrideKind;

struct KindName
{
    SplineSpanOverrideKind Kind;
    const char* Value;
    const char* Label;
};

// One row per kind, in dropdown order; the None label is the caller's.
constexpr std::array<KindName, 3> kKindNames = {{
    {SplineSpanOverrideKind::None, "None", nullptr},
    {SplineSpanOverrideKind::Gate, "Gate", "Gate"},
    {SplineSpanOverrideKind::ExplicitPiece, "ExplicitPiece", "Explicit Piece"},
}};

std::string MeshName(const Components::ModelRef& model)
{
    AssetMetadata metadata;
    if (EngineCore::GetInstance().GetAssetManager().GetRegistry().TryGetAssetMetadata(model.ToGuid(),
                                                                                      metadata))
    {
        if (!metadata.Name.empty())
            return metadata.Name;
        return metadata.Path.stem().string();
    }
    return model.ToGuid().ToString();
}

} // namespace

std::vector<Dropdown::Option> SpanOverrideKindOptions(const char* noneLabel)
{
    std::vector<Dropdown::Option> options;
    for (const KindName& name : kKindNames)
        options.push_back({name.Value, name.Label ? name.Label : noneLabel});
    return options;
}

int SpanOverrideKindIndex(SplineSpanOverrideKind kind)
{
    for (size_t i = 0; i < kKindNames.size(); ++i)
    {
        if (kKindNames[i].Kind == kind)
            return static_cast<int>(i);
    }
    return 0;
}

std::optional<SplineSpanOverrideKind> SpanOverrideKindOfOption(std::string_view value)
{
    for (const KindName& name : kKindNames)
    {
        if (value == name.Value)
            return name.Kind;
    }
    return std::nullopt;
}

Dropdown* AddPoolPieceRow(UIElement* parent, const std::string& label,
                          const Components::ModelRef (&pool)[Components::kSplineFencePoolCapacity],
                          uint8 slot, const char* poolName, const char* tooltip,
                          std::function<void(uint8)> onPicked)
{
    const uint32 active = Components::ActivePoolCount(pool);
    std::vector<Dropdown::Option> options;
    int selected = -1;
    for (uint32 i = 0; i < active; ++i)
    {
        if (i == slot)
            selected = static_cast<int>(options.size());
        options.push_back({std::to_string(i), std::string(poolName) + " " + std::to_string(i + 1u) + ": " +
                                                  MeshName(pool[i])});
    }
    if (selected < 0)
    {
        selected = static_cast<int>(options.size());
        options.push_back({std::to_string(slot),
                           std::string(poolName) + " " + std::to_string(slot + 1u) + ": empty"});
    }
    Dropdown* field = InspectorUI::AddDropdownRow(parent, label, options, selected, tooltip);
    field->SetOnValueChanged(
        [onPicked = std::move(onPicked)](const std::string& value)
        {
            uint32 picked = 0;
            for (const char c : value)
            {
                if (c < '0' || c > '9')
                    return;
                picked = picked * 10u + static_cast<uint32>(c - '0');
            }
            if (picked < Components::kSplineFencePoolCapacity)
                onPicked(static_cast<uint8>(picked));
        });
    return field;
}

std::vector<std::string> SpanOverrideNotes(const Components::SplineFence& fence,
                                           const Components::SplineSpanOverride& entry, bool spanStands)
{
    std::vector<std::string> notes;
    if (entry.Kind == SplineSpanOverrideKind::Gate)
    {
        const uint32 gates = Components::ActivePoolCount(fence.GatePool);
        if (gates == 0u)
        {
            notes.push_back("The Gate pool is empty, so this span is an opening: nothing stands in it, "
                            "and it cannot be selected in the scene view again. Fill Gate 1 to draw a "
                            "gate, or change it back in the fence's Span Overrides.");
            return notes;
        }
        if (entry.PoolSlot >= gates)
        {
            notes.push_back("Gate " + std::to_string(entry.PoolSlot + 1u) +
                            " is empty, so the span is left open — fill it in the Gate pool or choose "
                            "another gate piece.");
            return notes;
        }
    }
    else if (entry.Kind == SplineSpanOverrideKind::ExplicitPiece &&
             entry.PoolSlot >= Components::ActivePoolCount(fence.SpanPool))
    {
        notes.push_back("Span " + std::to_string(entry.PoolSlot + 1u) +
                        " is empty, so the span keeps the fence's own pick — fill it in the Span pool "
                        "or choose another piece.");
        return notes;
    }
    if (!spanStands && entry.Kind != SplineSpanOverrideKind::None)
    {
        notes.push_back("The fence has no span " + std::to_string(entry.SpanOrdinal) +
                        " on the run from point " + std::to_string(entry.PointIndex) +
                        ", so this override places nothing — change its point or span, or remove it.");
    }
    return notes;
}

} // namespace GameEngine::Editor
