#include "Providers/CancelToken.h"
#include "Providers/CliSessionFiles.h"

#include "Platform/Shell.h"

#include <gtest/gtest.h>

#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace GameEngine
{
namespace
{
namespace fs = std::filesystem;
using namespace std::chrono_literals;

// Reduced from what claude 2.1.290 and codex 0.162.0-alpha.2 wrote for a probe turn
// (ids replaced, account data removed); the Codex file's cwd is {{CWD}}.
fs::path Fixture(const char* name)
{
    return Platform::GetExecutablePath().parent_path() / "AgentCliFixtures" / name;
}

std::string ReadText(const fs::path& path)
{
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), {});
}

void WriteText(const fs::path& path, const std::string& text)
{
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

// `text` with every {{CWD}} replaced by `directory` as a JSON string's content.
std::string WithCwd(std::string text, const fs::path& directory)
{
    std::string escaped;
    for (const char character : directory.string())
        escaped += character == '\\' ? std::string("\\\\") : std::string(1, character);
    for (size_t at = text.find("{{CWD}}"); at != std::string::npos; at = text.find("{{CWD}}", at + escaped.size()))
        text.replace(at, 7, escaped);
    return text;
}

// A folder of its own under the temp directory, removed with the test.
class ScratchFolder
{
public:
    explicit ScratchFolder(const char* name)
        : m_Path(fs::temp_directory_path() / name)
    {
        fs::remove_all(m_Path);
        fs::create_directories(m_Path);
    }
    ~ScratchFolder()
    {
        std::error_code error;
        fs::remove_all(m_Path, error);
    }
    const fs::path& Path() const { return m_Path; }

private:
    fs::path m_Path;
};
} // namespace

TEST(CliSessionFilesTests, TheClaudeFolderNameReplacesEveryCharacterThatIsNotALetterOrDigit)
{
    // The folder claude 2.1.290 created for a probe run in this directory.
    EXPECT_EQ(CliSessionFiles::ClaudeProjectFolderName(R"(C:\Dev\_reports\cli-probe\proj-dir\My Proj.v2)"),
              "C--Dev--reports-cli-probe-proj-dir-My-Proj-v2");
    EXPECT_EQ(CliSessionFiles::ClaudeProjectFolderName("C:/Dev/GameEngine"), "C--Dev-GameEngine");
}

// Two prompts (one written with spaces in its JSON) among meta, tool-result, sidechain
// and reply records and one cut line;
// a session with no prompt is listed by its id; a file with no JSON is skipped and
// counted; another project's folder and a non-session file are not read.
TEST(CliSessionFilesTests, ClaudeSessionsAreListedNewestFirstByTheirFirstPromptAndCount)
{
    ScratchFolder scratch("CliSessionFilesTests-claude");
    const fs::path project = scratch.Path() / "My Game";
    const fs::path folder =
        scratch.Path() / "claude" / "projects" / CliSessionFiles::ClaudeProjectFolderName(project);
    const fs::path older = folder / "00000000-0000-4000-8000-000000000011.jsonl";
    const fs::path newer = folder / "00000000-0000-4000-8000-000000000012.jsonl";
    WriteText(older, ReadText(Fixture("claude-session.records")));
    WriteText(newer, R"({"type":"attachment","attachment":{"type":"date"}})" "\n");
    WriteText(folder / "not-a-session.jsonl", "not json\n");
    WriteText(folder / "notes.txt", "ignored\n");
    WriteText(scratch.Path() / "claude" / "projects" / "C--Other" / "other.jsonl",
              ReadText(Fixture("claude-session.records")));
    const auto now = fs::file_time_type::clock::now();
    fs::last_write_time(older, now - 2h);
    fs::last_write_time(newer, now - 1h);

    CancelToken cancel;
    const CliSessionList list = CliSessionFiles::ReadClaudeSessions(scratch.Path() / "claude", project, cancel);

    ASSERT_EQ(list.Sessions.size(), 2u);
    EXPECT_EQ(list.SkippedFiles, 1u);
    EXPECT_EQ(list.Sessions[0].Id, "00000000-0000-4000-8000-000000000012");
    EXPECT_EQ(list.Sessions[0].FirstPrompt, "");
    EXPECT_EQ(list.Sessions[0].PromptCount, 0u);
    EXPECT_EQ(list.Sessions[1].Id, "00000000-0000-4000-8000-000000000011");
    EXPECT_EQ(list.Sessions[1].FirstPrompt, "Why does the terrain flicker") << "its first line, trimmed";
    EXPECT_EQ(list.Sessions[1].PromptCount, 2u) << "the meta, tool-result, sidechain and cut lines are not prompts";
    EXPECT_GT(list.Sessions[0].Time, list.Sessions[1].Time);
}

// Codex keeps every project's sessions together: the reader keeps those whose
// session record names the project's directory, however it is spelled.
TEST(CliSessionFilesTests, CodexSessionsAreListedForTheProjectsDirectoryOnly)
{
    ScratchFolder scratch("CliSessionFilesTests-codex");
    const fs::path project = scratch.Path() / "Projects" / "My Game";
    const fs::path day = scratch.Path() / "codex" / "sessions" / "2026" / "10" / "08";
    const std::string fixture = ReadText(Fixture("codex-session.records"));
    WriteText(day / "rollout-2026-10-08T10-52-43-00000000-0000-4000-8000-000000000021.jsonl",
              WithCwd(fixture, project));
    // The same directory with a trailing separator, as another tool may record it.
    std::string trailing = WithCwd(fixture, project / "");
    trailing.replace(trailing.find("000000000021"), 12, "000000000022");
    WriteText(day / "rollout-2026-10-08T11-00-00-00000000-0000-4000-8000-000000000022.jsonl", trailing);
    WriteText(day / "rollout-2026-10-08T12-00-00-00000000-0000-4000-8000-000000000023.jsonl",
              WithCwd(fixture, scratch.Path() / "Projects" / "Other"));
    WriteText(day / "rollout-2026-10-08T13-00-00-00000000-0000-4000-8000-000000000024.jsonl",
              R"({"type":"event_msg","payload":{}})" "\n");

    CancelToken cancel;
    const CliSessionList list = CliSessionFiles::ReadCodexSessions(scratch.Path() / "codex", project, cancel);

    ASSERT_EQ(list.Sessions.size(), 2u) << "the other project's session is not listed";
    EXPECT_EQ(list.SkippedFiles, 1u) << "the file that does not start with its session record";
    for (const CliSessionSummary& session : list.Sessions)
    {
        EXPECT_EQ(session.FirstPrompt, "Name three water shaders") << session.Id;
        EXPECT_EQ(session.PromptCount, 2u) << "the environment message is not a prompt";
    }

#if defined(_WIN32)
    // Windows paths ignore case, and Codex records backslashes.
    std::string upper = project.string();
    for (char& character : upper)
        character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
    const CliSessionList sameDirectory = CliSessionFiles::ReadCodexSessions(scratch.Path() / "codex", upper, cancel);
    EXPECT_EQ(sameDirectory.Sessions.size(), 2u);
#endif
}
} // namespace GameEngine
