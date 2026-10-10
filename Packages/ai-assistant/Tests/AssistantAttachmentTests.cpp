// The editor's MCP server attached to a CLI session's turn: what each CLI is given, the
// configuration file's server, and the checks that keep a turn without tools when the
// tools cannot be reached safely.

#include "AssistantAttachment.h"
#include "AssistantTools.h"
#include "Providers/CancelToken.h"
#include "Providers/ClaudeSessionProvider.h"
#include "Providers/CodexSessionProvider.h"
#include "RecordingTurnEvents.h"
#include "ScopedEnvironmentVariable.h"

#include "Platform/Shell.h"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace GameEngine
{
namespace
{
constexpr uint16_t kPort = 9610;
constexpr uint64_t kConversation = 7001;
const std::filesystem::path kProject = "C:/Projects/Lanterns";

std::string FakeCli()
{
#if defined(_WIN32)
    return (Platform::GetExecutablePath().parent_path() / "FakeAgentCli.exe").string();
#else
    return (Platform::GetExecutablePath().parent_path() / "FakeAgentCli").string();
#endif
}

std::string Joined(const std::vector<std::string>& names, std::string_view prefix)
{
    std::string joined;
    for (const std::string& name : names)
        joined += (joined.empty() ? "" : ",") + std::string(prefix) + name;
    return joined;
}

// The value following `flag` in the fake CLI's "args=" reply (arguments separated by
// single spaces); empty when the flag is absent.
std::string ValueAfter(const std::string& reply, const std::string& flag)
{
    const std::size_t at = reply.find(" " + flag + " ");
    if (at == std::string::npos)
        return {};
    const std::size_t start = at + flag.size() + 2;
    return reply.substr(start, reply.find(' ', start) - start);
}

// Removes the attachment's configuration file when the test ends.
struct AttachedTurn
{
    AttachedTurn()
        : Tools(*AssistantAttachment::Write(FakeCli(), kPort, "fixture-token", kProject, kConversation))
    {
    }
    ~AttachedTurn() { AssistantAttachment::Remove(kConversation); }
    AgentToolAttachment Tools;
};
} // namespace

TEST(AssistantAttachmentTests, AClaudeTurnGetsTheEditorsServerAndOnlyTheAssistantsTools)
{
    ScopedEnvironmentVariable mode("FAKE_AGENT_CLI_MODE", "args");
    AttachedTurn attached;
    ClaudeSessionProvider provider(FakeCli(), {});
    AgentTurnRequest request;
    request.UserText = "hello";
    request.Tools = &attached.Tools;
    RecordingTurnEvents events;
    CancelToken cancel;

    provider.RunTurn(request, events, cancel);

    ASSERT_EQ(events.Result.Outcome, TurnOutcome::Succeeded) << events.Result.Error;
    const std::string reply = events.Result.Text;
    EXPECT_NE(reply.find(" --tools  --strict-mcp-config "), std::string::npos) << reply;
    EXPECT_NE(reply.find(" --permission-prompts none "), std::string::npos) << reply;
    const std::vector<std::string> attachedNames = AssistantTools::AttachedNames();
    EXPECT_EQ(ValueAfter(reply, "--allowedTools"), Joined(attachedNames, "mcp__editor_assistant__"));
    const std::string configPath = ValueAfter(reply, "--mcp-config");
    ASSERT_EQ(std::filesystem::path(configPath), attached.Tools.ConfigFile);

    std::ifstream file(configPath);
    const nlohmann::json config = nlohmann::json::parse(file, nullptr, false);
    const nlohmann::json& server = config["mcpServers"]["editor_assistant"];
    EXPECT_EQ(server["type"], "stdio");
    EXPECT_EQ(server["command"], FakeCli());
    EXPECT_EQ(std::filesystem::path(server["args"][0].get<std::string>()), AssistantAttachment::StagedServerScript());
    EXPECT_EQ(server["env"]["GE_EDITOR_DEBUG_PORT"], std::to_string(kPort));
    EXPECT_EQ(server["env"]["GE_MCP_TOOLS"], Joined(attachedNames, ""));
    EXPECT_EQ(server["env"]["GE_ASSISTANT_TOKEN"], "fixture-token");
    EXPECT_EQ(server["env"]["GE_MCP_IPC_TIMEOUT_MS"], "660000");
    EXPECT_EQ(server["env"]["GE_PROJECT_ROOT"], kProject.string());
    EXPECT_EQ(attachedNames.size(), AssistantTools::All().size() - 11u) << "every tool but the eleven denied ones";
}

TEST(AssistantAttachmentTests, ACodexTurnIgnoresTheUsersConfigurationAndDefinesTheServerInline)
{
    AttachedTurn attached;
    const CodexSessionProvider provider("codex", {});
    AgentTurnRequest request;
    request.Tools = &attached.Tools;

    const std::vector<std::string> arguments = provider.TurnArguments(request);

    const std::string key = "mcp_servers.editor_assistant.";
    std::string enabled;
    for (const std::string& name : AssistantTools::AttachedNames())
        enabled += (enabled.empty() ? "\"" : ", \"") + name + "\"";
    const std::vector<std::string> expected = {
        "exec", "--json", "--skip-git-repo-check", "-s", "read-only", "--ignore-user-config",
        "-c", key + "command=\"" + attached.Tools.Node + "\"",
        "-c", key + "args=[\"" + attached.Tools.ServerScript + "\"]",
        "-c", key + "env={GE_EDITOR_DEBUG_PORT = \"9610\", GE_MCP_TOOLS = \"" +
                  Joined(AssistantTools::AttachedNames(), "") +
                  "\", GE_ASSISTANT_TOKEN = \"fixture-token\", GE_PROJECT_ROOT = \"" + kProject.string() +
                  "\", GE_MCP_IPC_TIMEOUT_MS = \"660000\"}",
        "-c", key + "enabled_tools=[" + enabled + "]",
        "-c", key + "default_tools_approval_mode=\"approve\"",
        "-c", key + "tool_timeout_sec=660",
        "-"};
    // TOML basic strings: a Windows path's backslashes are escaped.
    std::vector<std::string> escaped = expected;
    for (std::string& argument : escaped)
        for (std::size_t at = 0; (at = argument.find('\\', at)) != std::string::npos; at += 2)
            argument.insert(at, 1, '\\');
    EXPECT_EQ(arguments, escaped);
}

TEST(AssistantAttachmentTests, CodexActsOnlyWhenItsSandboxedShellCannotReachThePort)
{
    CancelToken cancel;
    {
        ScopedEnvironmentVariable mode("FAKE_AGENT_CLI_MODE", "sandbox-open");
        CodexSessionProvider provider(FakeCli(), {});
        EXPECT_EQ(provider.ToolsRefusal(FakeCli(), kPort, cancel),
                  "Codex's shell can reach the editor's port outside the assistant's checks, so Codex runs as a "
                  "conversation only. Use Claude (local session) for the editor's tools.");
    }
    {
        ScopedEnvironmentVariable mode("FAKE_AGENT_CLI_MODE", "sandbox-closed");
        CodexSessionProvider provider(FakeCli(), {});
        EXPECT_EQ(provider.ToolsRefusal(FakeCli(), kPort, cancel), "");
    }
    {
        ScopedEnvironmentVariable mode("FAKE_AGENT_CLI_MODE", "sandbox-broken");
        CodexSessionProvider provider(FakeCli(), {});
        const std::string refusal = provider.ToolsRefusal(FakeCli(), kPort, cancel);
        EXPECT_NE(refusal.find("CODEX_HOME points to"), std::string::npos) << refusal;
        EXPECT_NE(refusal.find("Settings > AI Assistant > Codex executable"), std::string::npos) << refusal;
    }
    {
        ScopedEnvironmentVariable mode("FAKE_AGENT_CLI_MODE", "old-node");
        ClaudeSessionProvider provider(FakeCli(), {});
        EXPECT_EQ(provider.ToolsRefusal(FakeCli(), kPort, cancel),
                  "Node.js 18.20.0 is too old for the editor's tools: install Node 20 or later.");
    }
}

// Claude's `user` record carries each tool call's result; an error result is reported
// against the call's id with the first line of its text, and a call the result record lists
// as a permission denial is reported again as refused.
TEST(AssistantAttachmentTests, AClaudeToolCallThatEndsInAnErrorIsReported)
{
    const std::filesystem::path replay = std::filesystem::temp_directory_path() / "assistant-tool-results.stdout";
    std::ofstream(replay) << R"({"type":"system","subtype":"init","session_id":"00000000-0000-4000-8000-000000000003"})" "\n"
                          << R"({"type":"assistant","message":{"content":[{"type":"tool_use","id":"toolu_1","name":"mcp__editor_assistant__save_scene","input":{}},{"type":"tool_use","id":"toolu_2","name":"mcp__editor_assistant__get_log","input":{}},{"type":"tool_use","id":"toolu_3","name":"mcp__editor_assistant__undo","input":{}}]}})" "\n"
                          << R"({"type":"user","message":{"content":[{"type":"tool_result","tool_use_id":"toolu_1","is_error":true,"content":[{"type":"text","text":"Invalid entity\nat save_scene"}]},{"type":"tool_result","tool_use_id":"toolu_2","content":"[]"},{"type":"tool_result","tool_use_id":"toolu_3","is_error":true,"content":"Claude requested permissions to use undo"}]}})" "\n"
                          << R"({"type":"result","subtype":"success","is_error":false,"num_turns":2,"usage":{},"permission_denials":[{"tool_name":"mcp__editor_assistant__undo","tool_use_id":"toolu_3"}]})" "\n";
    ScopedEnvironmentVariable mode("FAKE_AGENT_CLI_MODE", "replay");
    ScopedEnvironmentVariable replayFile("FAKE_AGENT_CLI_REPLAY", replay.string().c_str());
    ClaudeSessionProvider provider(FakeCli(), {});
    AgentTurnRequest request;
    request.UserText = "save";
    RecordingTurnEvents events;
    CancelToken cancel;

    provider.RunTurn(request, events, cancel);
    std::filesystem::remove(replay);

    const std::vector<std::string> expected = {"session:00000000-0000-4000-8000-000000000003",
                                               "tool:mcp__editor_assistant__save_scene",
                                               "tool:mcp__editor_assistant__get_log",
                                               "tool:mcp__editor_assistant__undo",
                                               "failed:toolu_1:Invalid entity",
                                               "failed:toolu_3:Claude requested permissions to use undo",
                                               "refused:toolu_3:",
                                               "finished:"};
    EXPECT_EQ(events.Log, expected);
}

TEST(AssistantAttachmentTests, ATurnWhoseToolsAreRefusedRunsWithoutThemAndSaysWhy)
{
    ScopedEnvironmentVariable mode("FAKE_AGENT_CLI_MODE", "args");
    AttachedTurn attached;
    attached.Tools.Node = (std::filesystem::temp_directory_path() / "no-node-here.exe").string();
    ClaudeSessionProvider provider(FakeCli(), {});
    AgentTurnRequest request;
    request.UserText = "hello";
    request.Tools = &attached.Tools;
    RecordingTurnEvents events;
    CancelToken cancel;

    provider.RunTurn(request, events, cancel);

    ASSERT_EQ(events.Result.Outcome, TurnOutcome::Succeeded) << events.Result.Error;
    EXPECT_EQ(events.Result.Text.find("--mcp-config"), std::string::npos) << events.Result.Text;
    EXPECT_TRUE(events.Result.Note.starts_with("This reply ran without the editor's tools. Node.js did not run"))
        << events.Result.Note;
}
} // namespace GameEngine
