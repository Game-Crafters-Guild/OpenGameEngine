#include "Startup/EditorSessionDescriptor.h"

#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace GameEngine::Editor::Startup
{
namespace fs = std::filesystem;

namespace
{
// No repository sits this far below its own root. The bound keeps a malformed or
// cyclic path from turning provenance capture into an unbounded walk at startup.
constexpr int kMaxGitRootWalkDepth = 64;

std::string_view TrimAscii(std::string_view text)
{
    auto isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!text.empty() && isSpace(text.front()))
        text.remove_prefix(1);
    while (!text.empty() && isSpace(text.back()))
        text.remove_suffix(1);
    return text;
}

// First line of a small text file, or empty on any failure. Git metadata files are
// one short line each, so there is no partial-read case worth handling.
std::string ReadFirstLine(const fs::path& file)
{
    std::ifstream in(file);
    if (!in)
        return {};

    std::string line;
    std::getline(in, line);
    return line;
}

// Directory holding this tree's git metadata. `.git` is a directory in a primary
// clone but a FILE containing "gitdir: <path>" in a linked worktree, and this repo
// is worked mostly through linked worktrees, so both forms are load-bearing.
std::optional<fs::path> ResolveGitDir(const fs::path& gitEntry)
{
    std::error_code ec;
    if (fs::is_directory(gitEntry, ec))
        return gitEntry;

    ec.clear();
    if (!fs::is_regular_file(gitEntry, ec) || ec)
        return std::nullopt;

    constexpr std::string_view kGitDirPrefix = "gitdir:";
    const std::string firstLine = ReadFirstLine(gitEntry);
    const std::string_view trimmed = TrimAscii(firstLine);
    if (trimmed.rfind(kGitDirPrefix, 0) != 0)
        return std::nullopt;

    const std::string_view target = TrimAscii(trimmed.substr(kGitDirPrefix.size()));
    if (target.empty())
        return std::nullopt;

    fs::path gitDir{std::string(target)};
    if (gitDir.is_relative())
        gitDir = gitEntry.parent_path() / gitDir;
    return gitDir.lexically_normal();
}

// Branch name from a HEAD file, or absent when HEAD is detached (a raw commit id).
// Branch names may contain '/', so the name is everything after the ref prefix.
std::optional<std::string> ReadBranchFromHead(const fs::path& gitDir)
{
    constexpr std::string_view kSymbolicHeadPrefix = "ref: refs/heads/";
    const std::string head = ReadFirstLine(gitDir / "HEAD");
    const std::string_view trimmed = TrimAscii(head);
    if (trimmed.rfind(kSymbolicHeadPrefix, 0) != 0)
        return std::nullopt;

    const std::string_view branch = trimmed.substr(kSymbolicHeadPrefix.size());
    if (branch.empty())
        return std::nullopt;
    return std::string(branch);
}
} // namespace

EditorSessionDescriptor DeriveEditorSessionDescriptor(const fs::path& exeDir,
                                                      std::optional<std::string> sessionLabel)
{
    EditorSessionDescriptor out{};

    if (sessionLabel.has_value() && !sessionLabel->empty())
        out.SessionLabel = std::move(sessionLabel);

#ifdef GE_BUILD_CONFIG
    out.BuildConfig = std::string(GE_BUILD_CONFIG);
#endif

    // Walking up out of the staged output is legal here because this reports provenance
    // rather than resolving a runtime asset: nothing is loaded from what is found, and
    // finding nothing is the normal case for a shipped build.
    fs::path dir = exeDir.lexically_normal();
    for (int depth = 0; depth < kMaxGitRootWalkDepth && !dir.empty(); ++depth)
    {
        std::error_code ec;
        const fs::path gitEntry = dir / ".git";
        if (fs::exists(gitEntry, ec) && !ec)
        {
            // A `.git` that exists but cannot be parsed leaves every field absent: an
            // unreadable repository is unknown provenance, not a reason to guess.
            if (const std::optional<fs::path> gitDir = ResolveGitDir(gitEntry))
            {
                out.WorktreePath = dir;

                std::string name = dir.filename().string();
                if (name.empty())
                    name = dir.string();
                if (!name.empty())
                    out.WorktreeName = std::move(name);

                out.BranchAtLaunch = ReadBranchFromHead(*gitDir);
            }
            break;
        }

        const fs::path parent = dir.parent_path();
        if (parent == dir)
            break;
        dir = parent;
    }

    return out;
}

std::optional<std::string> FormatSessionChromeSuffix(const EditorSessionDescriptor& descriptor)
{
    if (descriptor.SessionLabel.has_value())
        return descriptor.SessionLabel;

    if (descriptor.WorktreeName.has_value())
    {
        std::string suffix = *descriptor.WorktreeName;
        if (descriptor.BranchAtLaunch.has_value())
        {
            suffix += '@';
            suffix += *descriptor.BranchAtLaunch;
        }
        return suffix;
    }

    return std::nullopt;
}
} // namespace GameEngine::Editor::Startup
