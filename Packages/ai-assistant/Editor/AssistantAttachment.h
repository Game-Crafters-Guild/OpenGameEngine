#pragma once

#include "Providers/IAgentProvider.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace GameEngine
{
/// Attaches the editor's MCP server to a conversation's turns: the staged server, the
/// editor's port, the assistant's tool set and the session token, written as the
/// configuration file the CLI reads.
class AssistantAttachment
{
public:
    /// The server script staged beside the editor by the McpServer target.
    static std::filesystem::path StagedServerScript();
    /// Empty when this editor can attach its tools to a conversation; otherwise why
    /// not, with the fix (no debug server running, no staged server). Cheap: reads no
    /// process and starts none.
    static std::string Unavailable();

    /// The attachment for one turn of conversation `conversation` in the project at
    /// `projectRoot`, its configuration file written under this editor's per-process
    /// temporary directory (`gameengine_mcp/pid-<pid>/assistant-<conversation>.json`);
    /// nullopt when the file could not be written. The server's tools that read or
    /// write files resolve their paths against `projectRoot` and refuse any outside it.
    static std::optional<AgentToolAttachment> Write(std::string node, uint16_t port, std::string_view token,
                                                    const std::filesystem::path& projectRoot, uint64_t conversation);
    /// Removes the configuration file of conversation `conversation`.
    static void Remove(uint64_t conversation);
};
} // namespace GameEngine
