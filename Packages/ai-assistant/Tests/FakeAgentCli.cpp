// Stands in for the `claude` and `codex` executables in the CLI session provider
// tests. The test picks the behavior through FAKE_AGENT_CLI_MODE, which the provider
// passes through (it removes only the key variables):
//
//   `--version`        "2.1.290 (Claude Code)", or "2.1.200 (Claude Code)" in mode old-version;
//                      in mode slow-version a 60 s sleep first
//   `auth status`      Claude's login JSON (with an email and organization the row must not
//                      show); authMethod "api_key" when ANTHROPIC_API_KEY is set; in mode
//                      logged-out {"loggedIn":false}, exit 1
//   `login status`     on stderr, as Codex does: "Logged in using an API key - sk-proj-***FIXTURE";
//                      in mode logged-out "Not logged in", exit 1
//   `-p process.versions.node`
//                      stands in for Node.js: "22.11.0", or "18.20.0" in mode old-node
//   `sandbox ...`      stands in for Codex's sandbox probe: "CONNECTED" in mode sandbox-open
//                      (the sandboxed command reached the port); in mode sandbox-broken Codex's
//                      own start failure on stderr and exit code 1; otherwise "REFUSED EACCES"
//   anything else is a turn: stdin is read to its end, then by mode
//     replay           prints the file named by FAKE_AGENT_CLI_REPLAY, line by line
//     env              a Claude stream whose reply reports each variable in kReportedVariables
//                      as "NAME=value" or "NAME unset", separated by "; "
//     echo             a Claude stream whose reply is "stdin=<what stdin held>"
//     args             a Claude stream whose reply is "args=" and the arguments, each followed by ' '
//     stall            a Claude stream's init and first delta, then a 30 s sleep
//     slow-init        a 2 s sleep, then the reply of mode args
//     exit-early       a Claude stream's init and first delta, "fixture failure" on stderr,
//                      then exit code 3 without a result
//     overlong         a Claude init, a stdout line of 1 MiB + 1 bytes, then a successful result
//     unknown-thread   what codex 0.162.0-alpha.2 does for `exec resume <unknown id>`: nothing on
//                      stdout, "no rollout found" on stderr, exit code 1

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <thread>

#if defined(_WIN32)
#  include <fcntl.h>
#  include <io.h>
#endif

namespace
{
constexpr const char* kReportedVariables[] = {"ANTHROPIC_API_KEY", "ANTHROPIC_AUTH_TOKEN", "CODEX_API_KEY",
                                              "OPENAI_API_KEY", "FAKE_AGENT_CLI_UNRELATED"};
constexpr std::size_t kOverlongLineBytes = 1024u * 1024u + 1u;
constexpr const char* kInit =
    R"({"type":"system","subtype":"init","session_id":"00000000-0000-4000-8000-000000000002"})";

std::string Mode()
{
    const char* mode = std::getenv("FAKE_AGENT_CLI_MODE");
    return mode ? mode : "";
}

void PrintLine(std::string_view line)
{
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

// `text` must hold no control character; quotes and backslashes (a Windows path among
// the arguments) are escaped.
std::string DeltaLine(std::string_view text)
{
    std::string escaped;
    for (const char c : text)
    {
        if (c == '"' || c == '\\')
            escaped += '\\';
        escaped += c;
    }
    return R"({"type":"stream_event","event":{"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":")" +
           escaped + R"("}}})";
}

std::string ResultLine()
{
    return R"({"type":"result","subtype":"success","is_error":false,"num_turns":1,"total_cost_usd":0.5,)"
           R"("usage":{"input_tokens":1,"output_tokens":2},"permission_denials":[]})";
}

void PrintClaudeReply(std::string_view text)
{
    PrintLine(kInit);
    PrintLine(DeltaLine(text));
    PrintLine(ResultLine());
}

int Replay()
{
    const char* path = std::getenv("FAKE_AGENT_CLI_REPLAY");
    std::ifstream file(path ? path : "", std::ios::binary);
    if (!file)
    {
        std::fputs("fake: no replay file\n", stderr);
        return 2;
    }
    std::string line;
    while (std::getline(file, line))
        PrintLine(line);
    return 0;
}

int ReportEnvironment()
{
    std::string report;
    for (const char* name : kReportedVariables)
    {
        if (!report.empty())
            report += "; ";
        const char* value = std::getenv(name);
        report += value ? std::string(name) + "=" + value : std::string(name) + " unset";
    }
    PrintClaudeReply(report);
    return 0;
}

int Turn(const std::string& stdinText, const std::string& arguments)
{
    const std::string mode = Mode();
    if (mode == "replay")
        return Replay();
    if (mode == "env")
        return ReportEnvironment();
    if (mode == "echo")
    {
        PrintClaudeReply("stdin=" + stdinText);
        return 0;
    }
    if (mode == "args")
    {
        PrintClaudeReply("args=" + arguments);
        return 0;
    }
    if (mode == "slow-init")
    {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        PrintClaudeReply("args=" + arguments);
        return 0;
    }
    if (mode == "stall")
    {
        PrintLine(kInit);
        PrintLine(DeltaLine("first"));
        std::this_thread::sleep_for(std::chrono::seconds(30));
        return 0;
    }
    if (mode == "exit-early")
    {
        PrintLine(kInit);
        PrintLine(DeltaLine("partial"));
        std::fputs("fixture failure\n", stderr);
        return 3;
    }
    if (mode == "unknown-thread")
    {
        std::fputs("Error: thread/resume: thread/resume failed: no rollout found for thread id "
                   "00000000-0000-4000-8000-000000000009 (code -32600)\n",
                   stderr);
        return 1;
    }
    if (mode == "overlong")
    {
        PrintLine(kInit);
        PrintLine(std::string(kOverlongLineBytes, 'x'));
        PrintLine(ResultLine());
        return 0;
    }
    std::fputs("fake: unknown mode\n", stderr);
    return 2;
}
} // namespace

int main(int argc, char** argv)
{
#if defined(_WIN32)
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    const std::string first = argc > 1 ? argv[1] : "";
    const std::string second = argc > 2 ? argv[2] : "";
    const bool loggedOut = Mode() == "logged-out";

    if (first == "--version")
    {
        if (Mode() == "slow-version")
            std::this_thread::sleep_for(std::chrono::seconds(60));
        PrintLine(Mode() == "old-version" ? "2.1.200 (Claude Code)" : "2.1.290 (Claude Code)");
        return 0;
    }
    if (first == "-p" && second == "process.versions.node")
    {
        PrintLine(Mode() == "old-node" ? "18.20.0" : "22.11.0");
        return 0;
    }
    if (first == "sandbox")
    {
        if (Mode() == "sandbox-broken")
        {
            std::fputs("Error: CODEX_HOME points to \"C:\\missing\", but that path does not exist\n", stderr);
            return 1;
        }
        PrintLine(Mode() == "sandbox-open" ? "CONNECTED" : "REFUSED EACCES");
        return 0;
    }
    if (first == "auth" && second == "status")
    {
        if (loggedOut)
        {
            PrintLine(R"({"loggedIn":false,"authMethod":"none"})");
            return 1;
        }
        // As the real CLI does, a key in the environment wins over the login.
        if (std::getenv("ANTHROPIC_API_KEY"))
        {
            PrintLine(R"({"loggedIn":true,"authMethod":"api_key","apiProvider":"firstParty",)"
                      R"("apiKeySource":"ANTHROPIC_API_KEY"})");
            return 0;
        }
        PrintLine(R"({"loggedIn":true,"authMethod":"claude.ai","apiProvider":"firstParty",)"
                  R"("email":"user@example.com","orgId":"org-fixture","orgName":"Example Org","subscriptionType":"max"})");
        return 0;
    }
    if (first == "login" && second == "status")
    {
        std::fputs(loggedOut ? "Not logged in\n" : "Logged in using an API key - sk-proj-***FIXTURE\n", stderr);
        return loggedOut ? 1 : 0;
    }

    std::string arguments;
    for (int index = 1; index < argc; ++index)
        arguments += std::string(argv[index]) + ' ';
    const std::string stdinText(std::istreambuf_iterator<char>(std::cin), {});
    return Turn(stdinText, arguments);
}
