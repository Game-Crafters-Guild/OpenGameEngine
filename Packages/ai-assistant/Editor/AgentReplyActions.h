#pragma once

#include <functional>
#include <span>
#include <string>

namespace GameEngine
{
struct ConversationMessage;

/// A button on every finished assistant reply in the AI Assistant panel.
struct AgentReplyAction
{
    /// Stable identifier; the button's element id is built from it.
    std::string Id;
    /// The button text.
    std::string Label;
    /// The button tooltip.
    std::string Tooltip;
    /// Runs on the UI thread with the reply the button sits on, and returns what the
    /// panel's status line reads after it ("Reply copied"); empty leaves the line as is.
    std::function<std::string(const ConversationMessage&)> Run;
};

/// The per-reply actions, registered by the module that owns each one (the package
/// registers Copy), shown in registration order. Register at module load, from the
/// UI thread; rows read the list when they are built.
class AgentReplyActions
{
public:
    static void Register(AgentReplyAction action);
    static std::span<const AgentReplyAction> All();
};
} // namespace GameEngine
