#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace GameEngine
{
/// What a tool of the editor's MCP server does, which decides the modes it runs in
/// (AssistantGate).
enum class AssistantToolClass : uint8_t
{
    /// Changes nothing; its result, images included, goes to the model's provider.
    Read,
    /// Changes what the user sees (camera, selection, visibility), not the scene.
    View,
    /// Changes the scene as one step the user can undo.
    UndoableEdit,
    /// Injected keyboard, pointer or drop input; its effects are the editor's own steps.
    Input,
    /// Cannot be undone as one step, or changes something outside the scene's undo
    /// history: a project file, a saved scene, play mode, a project setting, a capture.
    Gated,
    /// Not an action in this editor; refused in every mode and absent from the server.
    Denied,
};

/// One tool of the editor's MCP server as the assistant sees it. The table of these is
/// the one source for the CLI's allowed list, the server's tool set, the gate and the
/// conversation's call summaries.
struct AssistantTool
{
    /// The MCP tool's name (mcp/src/tools/).
    std::string_view Name;
    /// The debug-server method its call sends; empty for a tool that runs in the
    /// server's process (it asks with assistant_authorize when it acts on the machine).
    std::string_view Method;
    AssistantToolClass Class;
    /// The action in words, for the conversation ("Set component").
    std::string_view Action;
    /// The arguments whose values name the call's subject, in order; a summary shows
    /// those present.
    std::array<std::string_view, 3> SubjectArguments;
    /// For a Denied tool: why the assistant may not call it.
    std::string_view DeniedReason = {};
};

/// The table of every MCP server tool, its class and its summary.
class AssistantTools
{
public:
    /// The MCP server's name in the CLI's configuration; Claude Code names its tools
    /// `mcp__<server>__<tool>`.
    static constexpr std::string_view kServerName = "editor_assistant";

    /// Every tool of the server, Denied ones included.
    static std::span<const AssistantTool> All();
    /// The tool called `name` (bare, or as the CLI reports it with the server prefix);
    /// null for a name the table does not have.
    static const AssistantTool* Find(std::string_view name);
    /// The tool whose call sends `method`; null for a method no tool sends.
    static const AssistantTool* FindByMethod(std::string_view method);
    /// The names the assistant is attached with: every tool that is not Denied.
    static std::vector<std::string> AttachedNames();
    /// The subject of a call of the tool `name` with `arguments`: the values of its
    /// subject arguments that are present, joined by " · " ("12 · DirectionalLight ·
    /// Elevation 12"); an object argument reads as "key value" pairs. Empty for a name the
    /// table does not have or a call without them.
    static std::string Subject(std::string_view name, const nlohmann::json& arguments);
};
} // namespace GameEngine
