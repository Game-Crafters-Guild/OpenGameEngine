#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace GameEngine
{
/// One session a local CLI keeps on disk for the project, as the session list shows it.
struct CliSessionSummary
{
    /// The id the CLI resumes the session by (AgentTurnRequest::SessionId).
    std::string Id;
    /// The first line of the session's first prompt, trimmed and cut at
    /// kMaxFirstPromptBytes; empty when the file holds no prompt the reader recognizes.
    std::string FirstPrompt;
    /// When the session's file was last written; zero for a session the list did not
    /// hold (a stored id whose file was not found).
    std::chrono::system_clock::time_point Time;
    /// The prompts the user sent in the session.
    uint32_t PromptCount = 0;

    /// The most of a first prompt a summary keeps, in bytes, cut back to a whole
    /// UTF-8 character.
    static constexpr size_t kMaxFirstPromptBytes = 200;
};

/// A CLI's sessions for one project, read from the CLI's own files.
struct CliSessionList
{
    /// Newest first.
    std::vector<CliSessionSummary> Sessions;
    /// Session files of the project the reader could not make sense of (no line of
    /// the file is a JSON object, or a Codex file does not start with its session
    /// record); they are left out of Sessions.
    uint32_t SkippedFiles = 0;
};
} // namespace GameEngine
