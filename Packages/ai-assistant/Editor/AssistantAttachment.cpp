#include "AssistantAttachment.h"

#include "AssistantTools.h"

#include "Core/Application.h"
#include "Editor/Registries/DebugRequestGateRegistry.h"
#include "Platform/Process.h"

#include <fstream>
#include <system_error>

#include <nlohmann/json.hpp>

namespace GameEngine
{
namespace
{
// A request may wait on the editor (a capture, later a confirmation) for up to ten
// minutes; the server's own time limit sits above that.
constexpr const char* kServerTimeoutMs = "660000";

std::filesystem::path ConfigFile(uint64_t conversation)
{
    std::error_code error;
    std::filesystem::path base = std::filesystem::temp_directory_path(error);
    if (error || base.empty())
        base = PathUtils::GetExecutableDirectory();
    return base / "gameengine_mcp" / ("pid-" + std::to_string(Platform::GetCurrentProcessId())) /
           ("assistant-" + std::to_string(conversation) + ".json");
}

std::string JoinNames(const std::vector<std::string>& names)
{
    std::string joined;
    for (const std::string& name : names)
        joined += (joined.empty() ? "" : ",") + name;
    return joined;
}
} // namespace

std::filesystem::path AssistantAttachment::StagedServerScript()
{
    return PathUtils::GetExecutableDirectory() / "mcp" / "dist" / "index.js";
}

std::string AssistantAttachment::Unavailable()
{
    if (Editor::EditorDebugPort() == 0)
        return "The editor's debug server is not running, so the assistant cannot reach the editor: restart the "
               "editor, or start it with a free --debug-port.";
    std::error_code error;
    if (!std::filesystem::exists(StagedServerScript(), error))
        return "The editor's MCP server is not staged beside the editor: build the McpServer target.";
    return {};
}

std::optional<AgentToolAttachment> AssistantAttachment::Write(std::string node, uint16_t port, std::string_view token,
                                                              const std::filesystem::path& projectRoot,
                                                              uint64_t conversation)
{
    AgentToolAttachment attachment;
    attachment.ServerName = std::string(AssistantTools::kServerName);
    attachment.Node = std::move(node);
    attachment.ServerScript = StagedServerScript().string();
    attachment.Port = port;
    attachment.ToolNames = AssistantTools::AttachedNames();
    attachment.Environment = {
        {"GE_EDITOR_DEBUG_PORT", std::to_string(port)},
        {"GE_MCP_TOOLS", JoinNames(attachment.ToolNames)},
        {"GE_ASSISTANT_TOKEN", std::string(token)},
        {"GE_PROJECT_ROOT", projectRoot.string()},
        {"GE_MCP_IPC_TIMEOUT_MS", kServerTimeoutMs},
    };
    attachment.ConfigFile = ConfigFile(conversation);

    nlohmann::json environment = nlohmann::json::object();
    for (const auto& [name, value] : attachment.Environment)
        environment[name] = value;
    const nlohmann::json config = {
        {"mcpServers",
         {{attachment.ServerName,
           {{"type", "stdio"},
            {"command", attachment.Node},
            {"args", nlohmann::json::array({attachment.ServerScript})},
            {"env", environment}}}}}};

    std::error_code error;
    std::filesystem::create_directories(attachment.ConfigFile.parent_path(), error);
    std::ofstream file(attachment.ConfigFile, std::ios::binary | std::ios::trunc);
    file << config.dump(2);
    if (!file.good())
        return std::nullopt;
    return attachment;
}

void AssistantAttachment::Remove(uint64_t conversation)
{
    std::error_code error;
    std::filesystem::remove(ConfigFile(conversation), error);
}
} // namespace GameEngine
