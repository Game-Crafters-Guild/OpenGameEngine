#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace GameEngine::Editor::Startup
{
/**
 * @brief Launch-time provenance for one editor process: which tree it was built from,
 *        which branch that tree was on when it started, and what it was launched for.
 *
 * Captured once at startup and immutable for the process lifetime. Every field is a
 * fact. An absent field means "not known" and is never filled with a guess: an
 * inferred label would mislabel exactly the ambiguous sessions this exists to
 * disambiguate.
 */
struct EditorSessionDescriptor
{
    /// Free text from --session-label. Absent when the flag was not passed.
    std::optional<std::string> SessionLabel;

    /// Git root enclosing the executable, found by walking up from the exe directory.
    /// Absent for a shipped or relocated build with no repository above it, which is
    /// the normal case rather than an error.
    std::optional<std::filesystem::path> WorktreePath;

    /// Directory name of WorktreePath. The durable half of the identity: unlike the
    /// branch it cannot drift, because the binary lives in that directory.
    std::optional<std::string> WorktreeName;

    /// Branch WorktreePath was on when this process started. Stamped once and never
    /// re-read: a `git switch` after launch leaves the running binary built from the
    /// old branch, so a live read would name a branch this build did not come from.
    /// Present only for a symbolic HEAD; a detached HEAD has no branch and reports
    /// absent. Present it as "launched from", never as "current branch".
    std::optional<std::string> BranchAtLaunch;

    /// Build configuration name ("Debug", "DebugFast", "Release"), taken from the
    /// build's own GE_BUILD_CONFIG definition. Absent when the build did not define
    /// one — it is not reconstructed from NDEBUG, which cannot tell DebugFast from
    /// Debug because DebugFast seeds its flags from Debug.
    std::optional<std::string> BuildConfig;
};

/**
 * @brief Derives the descriptor from an executable directory.
 *
 * The git walk is best-effort and silent: it never throws, never logs, and no engine
 * behaviour depends on finding a root. Failure to find one costs a single bounded
 * directory walk and yields absent worktree/branch fields.
 *
 * @param exeDir      Directory holding the running executable.
 * @param sessionLabel Verbatim --session-label text, if the flag was passed.
 */
EditorSessionDescriptor DeriveEditorSessionDescriptor(const std::filesystem::path& exeDir,
                                                      std::optional<std::string> sessionLabel);

/**
 * @brief Compact provenance suffix for the editor chrome, or absent when nothing is known.
 *
 * The fallback chain is facts-only: an explicit label wins; otherwise the worktree name
 * (with the launch branch when HEAD was symbolic); otherwise nothing at all.
 */
std::optional<std::string> FormatSessionChromeSuffix(const EditorSessionDescriptor& descriptor);
} // namespace GameEngine::Editor::Startup
