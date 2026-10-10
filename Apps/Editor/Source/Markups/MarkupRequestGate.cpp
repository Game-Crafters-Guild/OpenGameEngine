#include "Markups/MarkupRequestGate.h"

#include "Editor/Registries/DebugRequestGateRegistry.h"
#include "Markups/MarkupEditorBridge.h"

#include <algorithm>
#include <array>
#include <string_view>

namespace GameEngine::Editor
{

namespace
{

constexpr std::array<std::string_view, 8> kInputInjectionMethods = {
    "send_key",     "input_text",   "send_text",           "click_element",
    "move_pointer", "perform_drop", "simulate_mouse_drag", "simulate_double_click_drag",
};

// The bridge whose Agent scope a request opens, or null for an input-injection method or
// an editor with no bridge. Before and Handled ask the same question, so they pair.
MarkupEditorBridge* AttributedBridge(const DebugRequestGateContext& context)
{
    const bool injectsInput = std::find(kInputInjectionMethods.begin(), kInputInjectionMethods.end(),
                                        context.Method) != kInputInjectionMethods.end();
    return injectsInput ? nullptr : MarkupEditorBridge::TryGet();
}

DebugRequestVerdict OpenAgentScope(const DebugRequestGateContext& context)
{
    if (MarkupEditorBridge* bridge = AttributedBridge(context))
        bridge->BeginAgentRequest();
    return {};
}

void CloseAgentScope(const DebugRequestGateContext& context)
{
    if (MarkupEditorBridge* bridge = AttributedBridge(context))
        bridge->EndAgentRequest();
}

} // namespace

void RegisterMarkupRequestGate(DebugRequestGateRegistry& registry)
{
    registry.Register({"markups", &OpenAgentScope, &CloseAgentScope});
}

} // namespace GameEngine::Editor
