#include "Providers/CancelToken.h"
#include "Providers/ClaudeSessionProvider.h"
#include "Providers/CodexSessionProvider.h"
#include "RecordingTurnEvents.h"
#include "ScopedEnvironmentVariable.h"

#include "Platform/Shell.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace GameEngine
{
namespace
{
using namespace std::chrono_literals;

// The redacted recordings (one real stdout stream of each CLI, session ids replaced,
// paths and account data removed) and the fake CLI that replays them are staged
// beside this executable.
std::filesystem::path TestDirectory()
{
    return Platform::GetExecutablePath().parent_path();
}

std::string FakeCli()
{
#if defined(_WIN32)
    return (TestDirectory() / "FakeAgentCli.exe").string();
#else
    return (TestDirectory() / "FakeAgentCli").string();
#endif
}

std::string Fixture(const char* name)
{
    return (TestDirectory() / "AgentCliFixtures" / name).string();
}

AgentTurnRequest Request(std::string userText)
{
    AgentTurnRequest request;
    request.UserText = std::move(userText);
    return request;
}

constexpr const char* kFixtureSession = "00000000-0000-4000-8000-000000000001";
} // namespace

TEST(CliSessionProviderTests, ClaudeTurnArgumentsAreTheDesignCommandLine)
{
    const ClaudeSessionProvider provider("claude", {});
    AgentTurnRequest request = Request("hello");
    request.SessionId = "session-1";
    request.Model = "opus";
    request.Effort = "high";
    request.SystemPrompt = "Be brief.";

    const std::vector<std::string> expected = {
        "-p", "--output-format", "stream-json", "--verbose", "--include-partial-messages", "--resume", "session-1",
        "--permission-prompts", "none", "--tools", "", "--strict-mcp-config", "--system-prompt-snapshot", "off",
        "--model", "opus", "--effort", "high", "--append-system-prompt", "Be brief."};
    EXPECT_EQ(provider.TurnArguments(request), expected);
}

TEST(CliSessionProviderTests, CodexTurnArgumentsPutTheExecOptionsBeforeResume)
{
    const CodexSessionProvider provider("codex", "C:/Project");
    AgentTurnRequest request = Request("hello");
    request.SessionId = "thread-1";

    const std::vector<std::string> expected = {
        "exec", "--json", "--skip-git-repo-check", "-C", std::filesystem::path("C:/Project").string(),
        "-s", "read-only", "resume", "thread-1", "-"};
    EXPECT_EQ(provider.TurnArguments(request), expected);
}

TEST(CliSessionProviderTests, RecordedClaudeTurnYieldsTheSessionIdTheDeltasInOrderAndTheAccounting)
{
    ScopedEnvironmentVariable mode("FAKE_AGENT_CLI_MODE", "replay");
    const std::string fixture = Fixture("claude-turn.stdout");
    ScopedEnvironmentVariable replay("FAKE_AGENT_CLI_REPLAY", fixture.c_str());
    ClaudeSessionProvider provider(FakeCli(), {});
    RecordingTurnEvents events;
    CancelToken cancel;

    provider.RunTurn(Request("Describe blue in two sentences."), events, cancel);

    const std::string reply = "Blue is the color of a clear daytime sky and deep ocean water. It is often "
                              "associated with calm, trust, and stability.";
    const std::vector<std::string> expected = {
        std::string("session:") + kFixtureSession, "delta:Blue is the color of a", "delta: clear da",
        "delta:ytime sky and de", "delta:ep ocean water", "delta:. It is", "delta: often associ",
        "delta:ated with calm, tr", "delta:ust, and stability.", "finished:" + reply};
    EXPECT_EQ(events.Log, expected);
    EXPECT_EQ(events.Result.Outcome, TurnOutcome::Succeeded);
    EXPECT_EQ(events.Result.Model, "claude-fable-5-1") << "the model the init record names";
    EXPECT_DOUBLE_EQ(events.Result.CostUsd, 0.08598599999999999);
    EXPECT_EQ(events.Result.ModelRoundTrips, 1u);
    EXPECT_EQ(events.Result.InputTokens, 2u + 4189u + 544u);
    EXPECT_EQ(events.Result.OutputTokens, 41u);
    EXPECT_EQ(events.Result.PermissionDenials, 0u);
}

TEST(CliSessionProviderTests, RecordedCodexTurnYieldsTheThreadIdTheMessageAndTheUsageAndSkipsWarnings)
{
    ScopedEnvironmentVariable mode("FAKE_AGENT_CLI_MODE", "replay");
    const std::string fixture = Fixture("codex-turn.stdout");
    ScopedEnvironmentVariable replay("FAKE_AGENT_CLI_REPLAY", fixture.c_str());
    CodexSessionProvider provider(FakeCli(), {});
    RecordingTurnEvents events;
    CancelToken cancel;

    provider.RunTurn(Request("Describe blue in two sentences."), events, cancel);

    const std::string reply = "Blue is the color of a clear sky. It often feels calm and peaceful.";
    const std::vector<std::string> expected = {std::string("session:") + kFixtureSession, "delta:" + reply,
                                               "finished:" + reply};
    EXPECT_EQ(events.Log, expected);
    EXPECT_EQ(events.Result.Outcome, TurnOutcome::Succeeded);
    EXPECT_EQ(events.Result.InputTokens, 15868u);
    EXPECT_EQ(events.Result.OutputTokens, 63u);
}

// The recorded answer of `claude -p --resume <unknown id>`: no init, a result with no
// text and the message in `errors`.
TEST(CliSessionProviderTests, AnUnknownClaudeSessionFailsTheTurnWithTheClisMessage)
{
    ScopedEnvironmentVariable mode("FAKE_AGENT_CLI_MODE", "replay");
    const std::string fixture = Fixture("claude-dead-session.stdout");
    ScopedEnvironmentVariable replay("FAKE_AGENT_CLI_REPLAY", fixture.c_str());
    ClaudeSessionProvider provider(FakeCli(), {});
    RecordingTurnEvents events;
    CancelToken cancel;

    provider.RunTurn(Request("hello"), events, cancel);

    EXPECT_EQ(events.Log, std::vector<std::string>{"finished:"}) << "no session is reported";
    EXPECT_EQ(events.Result.Outcome, TurnOutcome::Failed);
    EXPECT_EQ(events.Result.Error, "Claude Code reported an error: No conversation found with session ID: "
                                   "00000000-0000-4000-8000-000000000009");
    EXPECT_TRUE(events.Result.SessionNotFound);
}

// The recorded answer of `claude -p --model not-a-model` (claude 2.1.296): an init, the
// error as a synthetic assistant message and a failed result carrying the same text.
TEST(CliSessionProviderTests, AnUnknownModelFailsTheTurnWithTheClisMessage)
{
    ScopedEnvironmentVariable mode("FAKE_AGENT_CLI_MODE", "replay");
    const std::string fixture = Fixture("claude-unknown-model.stdout");
    ScopedEnvironmentVariable replay("FAKE_AGENT_CLI_REPLAY", fixture.c_str());
    ClaudeSessionProvider provider(FakeCli(), {});
    RecordingTurnEvents events;
    CancelToken cancel;

    AgentTurnRequest request = Request("hello");
    request.Model = "not-a-model";
    provider.RunTurn(request, events, cancel);

    EXPECT_EQ(events.Result.Outcome, TurnOutcome::Failed);
    EXPECT_EQ(events.Result.Error, "Claude Code reported an error: There's an issue with the selected model "
                                   "(not-a-model). It may not exist or you may not have access to it. Run "
                                   "--model to pick a different model.");
    EXPECT_FALSE(events.Result.SessionNotFound);
}

// `codex exec resume <unknown id>` prints no record; its stderr says the thread is unknown.
TEST(CliSessionProviderTests, AnUnknownCodexThreadFailsTheTurnAsSessionNotFound)
{
    ScopedEnvironmentVariable mode("FAKE_AGENT_CLI_MODE", "unknown-thread");
    CodexSessionProvider provider(FakeCli(), {});
    RecordingTurnEvents events;
    CancelToken cancel;
    AgentTurnRequest request = Request("hello");
    request.SessionId = "00000000-0000-4000-8000-000000000009";

    provider.RunTurn(request, events, cancel);

    EXPECT_EQ(events.Result.Outcome, TurnOutcome::Failed);
    EXPECT_NE(events.Result.Error.find("no rollout found for thread id"), std::string::npos) << events.Result.Error;
    EXPECT_TRUE(events.Result.SessionNotFound);
}

TEST(CliSessionProviderTests, ThePromptGoesOnStdin)
{
    ScopedEnvironmentVariable mode("FAKE_AGENT_CLI_MODE", "echo");
    ClaudeSessionProvider provider(FakeCli(), {});
    RecordingTurnEvents events;
    CancelToken cancel;

    provider.RunTurn(Request("a prompt with spaces"), events, cancel);

    EXPECT_EQ(events.Result.Outcome, TurnOutcome::Succeeded);
    EXPECT_EQ(events.Result.Text, "stdin=a prompt with spaces");
}

TEST(CliSessionProviderTests, TheChildEnvironmentHoldsNoKeyVariableButKeepsTheRest)
{
    ScopedEnvironmentVariable mode("FAKE_AGENT_CLI_MODE", "env");
    ScopedEnvironmentVariable anthropicKey("ANTHROPIC_API_KEY", "fixture-anthropic-key");
    ScopedEnvironmentVariable anthropicToken("ANTHROPIC_AUTH_TOKEN", "fixture-anthropic-token");
    ScopedEnvironmentVariable codexKey("CODEX_API_KEY", "fixture-codex-key");
    ScopedEnvironmentVariable openAiKey("OPENAI_API_KEY", "fixture-openai-key");
    ScopedEnvironmentVariable unrelated("FAKE_AGENT_CLI_UNRELATED", "kept");
    ClaudeSessionProvider provider(FakeCli(), {});
    RecordingTurnEvents events;
    CancelToken cancel;

    provider.RunTurn(Request("hello"), events, cancel);

    EXPECT_EQ(events.Result.Outcome, TurnOutcome::Succeeded) << events.Result.Error;
    EXPECT_EQ(events.Result.Text, "ANTHROPIC_API_KEY unset; ANTHROPIC_AUTH_TOKEN unset; CODEX_API_KEY unset; "
                                  "OPENAI_API_KEY unset; FAKE_AGENT_CLI_UNRELATED=kept");
}

TEST(CliSessionProviderTests, CancellingMidStreamStopsTheTurnAndKeepsTheText)
{
    ScopedEnvironmentVariable mode("FAKE_AGENT_CLI_MODE", "stall");
    ClaudeSessionProvider provider(FakeCli(), {});
    CancelToken cancel;

    // Cancels as soon as the first delta arrives; the fake then sleeps 30 s.
    class CancellingEvents final : public RecordingTurnEvents
    {
    public:
        explicit CancellingEvents(CancelToken& token) : m_Token(token) {}
        void OnTextDelta(std::string_view text) override
        {
            RecordingTurnEvents::OnTextDelta(text);
            m_Token.Cancel();
        }

    private:
        CancelToken& m_Token;
    } events(cancel);

    const auto start = std::chrono::steady_clock::now();
    provider.RunTurn(Request("hello"), events, cancel);

    EXPECT_LT(std::chrono::steady_clock::now() - start, 20s);
    EXPECT_EQ(events.FinishedCount, 1);
    EXPECT_EQ(events.Result.Outcome, TurnOutcome::Stopped);
    EXPECT_EQ(events.Result.Text, "first");
}

TEST(CliSessionProviderTests, StoppingDuringTheVersionCheckStopsTheTurn)
{
    ScopedEnvironmentVariable mode("FAKE_AGENT_CLI_MODE", "slow-version");
    ClaudeSessionProvider provider(FakeCli(), {});
    RecordingTurnEvents events;
    CancelToken cancel;

    // The fake sleeps 60 s before answering `--version`.
    std::thread stopper([&cancel]
    {
        std::this_thread::sleep_for(200ms);
        cancel.Cancel();
    });
    const auto start = std::chrono::steady_clock::now();
    provider.RunTurn(Request("hello"), events, cancel);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    stopper.join();

    EXPECT_LT(elapsed, 10s);
    EXPECT_EQ(events.FinishedCount, 1);
    EXPECT_EQ(events.Result.Outcome, TurnOutcome::Stopped) << events.Result.Error;
}

TEST(CliSessionProviderTests, AVersionCheckThatDoesNotAnswerFailsTheTurnWithTheFix)
{
    ScopedEnvironmentVariable mode("FAKE_AGENT_CLI_MODE", "slow-version");
    ClaudeSessionProvider provider(FakeCli(), {});
    RecordingTurnEvents events;
    CancelToken cancel;

    const auto start = std::chrono::steady_clock::now();
    provider.RunTurn(Request("hello"), events, cancel);

    EXPECT_LT(std::chrono::steady_clock::now() - start, 40s);
    EXPECT_EQ(events.Result.Outcome, TurnOutcome::Failed);
    EXPECT_EQ(events.Result.Error,
              "Claude Code did not answer `--version` within 20 s: check its executable in Settings > AI Assistant.");
}

TEST(CliSessionProviderTests, ACliThatExitsBeforeItsResultFailsTheTurnWithItsMessageAndKeepsTheText)
{
    ScopedEnvironmentVariable mode("FAKE_AGENT_CLI_MODE", "exit-early");
    ClaudeSessionProvider provider(FakeCli(), {});
    RecordingTurnEvents events;
    CancelToken cancel;

    provider.RunTurn(Request("hello"), events, cancel);

    EXPECT_EQ(events.Result.Outcome, TurnOutcome::Failed);
    EXPECT_EQ(events.Result.Error, "Claude Code exited with code 3 before finishing the reply: fixture failure");
    EXPECT_EQ(events.Result.Text, "partial");
}

TEST(CliSessionProviderTests, AnOverlongLineFailsTheTurnWithItsMessage)
{
    ScopedEnvironmentVariable mode("FAKE_AGENT_CLI_MODE", "overlong");
    ClaudeSessionProvider provider(FakeCli(), {});
    RecordingTurnEvents events;
    CancelToken cancel;

    provider.RunTurn(Request("hello"), events, cancel);

    EXPECT_EQ(events.Result.Outcome, TurnOutcome::Failed);
    EXPECT_EQ(events.Result.Error, "The reply exceeded 1 MiB in one line, so it could not be read whole.");
}

TEST(CliSessionProviderTests, AClaudeOlderThanTheFloorIsRefusedWithTheFix)
{
    ScopedEnvironmentVariable mode("FAKE_AGENT_CLI_MODE", "old-version");
    ClaudeSessionProvider provider(FakeCli(), {});
    RecordingTurnEvents events;
    CancelToken cancel;

    provider.RunTurn(Request("hello"), events, cancel);

    const std::vector<std::string> expected = {"finished:"};
    EXPECT_EQ(events.Log, expected);
    EXPECT_EQ(events.Result.Outcome, TurnOutcome::Failed);
    EXPECT_EQ(events.Result.Error,
              "Claude Code 2.1.259 or later is required (found 2.1.200): run `claude update`.");
}

TEST(CliSessionProviderTests, AMissingExecutableIsRefusedWithTheFix)
{
    ClaudeSessionProvider provider((TestDirectory() / "NoSuchAgentCli.exe").string(), {});
    RecordingTurnEvents events;
    CancelToken cancel;

    provider.RunTurn(Request("hello"), events, cancel);

    EXPECT_EQ(events.Result.Outcome, TurnOutcome::Failed);
    EXPECT_NE(events.Result.Error.find("set its path in Settings > AI Assistant"), std::string::npos)
        << events.Result.Error;
}

TEST(CliSessionProviderTests, ACommandScriptIsRefusedWithTheFix)
{
    const std::string script = (TestDirectory() / "claude.CMD").string();
    ClaudeSessionProvider provider(script, {});
    RecordingTurnEvents events;
    CancelToken cancel;

    provider.RunTurn(Request("hello"), events, cancel);

    EXPECT_EQ(events.Result.Outcome, TurnOutcome::Failed);
    EXPECT_EQ(events.Result.Error, "Claude Code executable " + script +
                                       " is a command script, which is not supported: set it to the native "
                                       "program in Settings > AI Assistant.");
}

TEST(CliSessionProviderTests, LoginRowsShowTheMethodAndNeverTheAccountOrAKey)
{
    const CancelToken cancel;
    {
        ScopedEnvironmentVariable mode("FAKE_AGENT_CLI_MODE", "");
        const CliSessionProvider::LoginStatus claude = ClaudeSessionProvider(FakeCli(), {}).CheckLogin(cancel);
        EXPECT_TRUE(claude.LoggedIn);
        EXPECT_EQ(claude.Text, "Logged in (claude.ai)");

        const CliSessionProvider::LoginStatus codex = CodexSessionProvider(FakeCli(), {}).CheckLogin(cancel);
        EXPECT_TRUE(codex.LoggedIn);
        EXPECT_EQ(codex.Text, "Logged in using an API key");
    }
    {
        ScopedEnvironmentVariable mode("FAKE_AGENT_CLI_MODE", "logged-out");
        const CliSessionProvider::LoginStatus claude = ClaudeSessionProvider(FakeCli(), {}).CheckLogin(cancel);
        EXPECT_FALSE(claude.LoggedIn);
        EXPECT_EQ(claude.Text, "Not logged in: run `claude auth login`.");

        const CliSessionProvider::LoginStatus codex = CodexSessionProvider(FakeCli(), {}).CheckLogin(cancel);
        EXPECT_FALSE(codex.LoggedIn);
        EXPECT_EQ(codex.Text, "Not logged in: run `codex login`.");
    }
}

TEST(CliSessionProviderTests, TheLoginCheckRunsWithoutTheKeySoItReportsTheLogin)
{
    ScopedEnvironmentVariable mode("FAKE_AGENT_CLI_MODE", "");
    ScopedEnvironmentVariable key("ANTHROPIC_API_KEY", "fixture-anthropic-key");
    const CancelToken cancel;

    const CliSessionProvider::LoginStatus claude = ClaudeSessionProvider(FakeCli(), {}).CheckLogin(cancel);

    EXPECT_TRUE(claude.LoggedIn);
    EXPECT_EQ(claude.Text, "Logged in (claude.ai)");
}

TEST(CliSessionProviderTests, ALoggedOutSessionRefusesTheTurnWithTheFix)
{
    ScopedEnvironmentVariable mode("FAKE_AGENT_CLI_MODE", "logged-out");
    CodexSessionProvider provider(FakeCli(), {});
    RecordingTurnEvents events;
    CancelToken cancel;

    provider.RunTurn(Request("hello"), events, cancel);

    EXPECT_EQ(events.Result.Outcome, TurnOutcome::Failed);
    EXPECT_EQ(events.Result.Error, "Not logged in: run `codex login`.");
}
} // namespace GameEngine
