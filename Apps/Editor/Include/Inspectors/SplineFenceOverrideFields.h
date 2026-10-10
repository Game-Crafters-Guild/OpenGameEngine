#pragma once

#include "Components/Spline/SplineFence.h"
#include "UI/Controls/Dropdown.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine
{
class UIElement;
}

namespace GameEngine::Editor
{

// The fields both fence inspectors show for one span override: the scene
// view's span section and the fence's own override rows name the kinds, the
// pool pieces and the problems the same way.

// The override kinds as a dropdown offers them. `noneLabel` names the None
// entry: "Span Pool" where it means "the fence's own pick", "Remove" where it
// empties an override row.
std::vector<Dropdown::Option> SpanOverrideKindOptions(const char* noneLabel);
// The option index of a kind, and the kind an option value names.
int SpanOverrideKindIndex(Components::SplineSpanOverrideKind kind);
std::optional<Components::SplineSpanOverrideKind> SpanOverrideKindOfOption(std::string_view value);

// A dropdown row offering the filled slots of one pool by mesh name, the
// selected slot first shown. An empty slot the override still names is offered
// too, labelled as empty, so the row never shows a slot it does not hold.
// `onPicked` receives the chosen slot.
Dropdown* AddPoolPieceRow(UIElement* parent, const std::string& label,
                          const Components::ModelRef (&pool)[Components::kSplineFencePoolCapacity],
                          uint8 slot, const char* poolName, const char* tooltip,
                          std::function<void(uint8)> onPicked);

// What the author needs to know about one override that the layout would only
// log: a gate over an empty Gate pool is an opening that cannot be picked again,
// a slot its pool does not hold, and — when `spanStands` is false for an
// override that should place a piece — a span the fence does not have.
std::vector<std::string> SpanOverrideNotes(const Components::SplineFence& fence,
                                           const Components::SplineSpanOverride& entry, bool spanStands);

} // namespace GameEngine::Editor
