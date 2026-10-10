#pragma once

#include "CliSessionList.h"

#include <filesystem>
#include <string>

namespace GameEngine
{
class CancelToken;

/// Reads the session files the local CLIs keep, read-only: nothing here writes to,
/// creates or locks anything under a CLI's folder. A file the reader cannot make
/// sense of is skipped and counted, and a line that is not JSON is skipped, so one
/// odd file never fails the list. The reads walk directories and parse every line
/// of each session file: call them off the UI thread. Each returns early, with what
/// it read so far, once `cancel` fires.
namespace CliSessionFiles
{
/// The folder under `<Claude config>/projects/` that holds the sessions Claude Code
/// ran in `workingDirectory`: the directory's path with every UTF-16 code unit that
/// is not an ASCII letter or digit replaced by '-' ("C:\Dev\My Game" is
/// "C--Dev-My-Game").
std::string ClaudeProjectFolderName(const std::filesystem::path& workingDirectory);

/// Claude Code's sessions for `workingDirectory`: one `<session id>.jsonl` per
/// session in `<claudeConfigDirectory>/projects/<ClaudeProjectFolderName>/`. A
/// prompt is a `user` record whose content is text, not a tool result, and not
/// marked isMeta or isSidechain. A file with no prompt is listed by its id and time.
CliSessionList ReadClaudeSessions(const std::filesystem::path& claudeConfigDirectory,
                                  const std::filesystem::path& workingDirectory, const CancelToken& cancel);

/// Codex's sessions for `workingDirectory`: the `rollout-*.jsonl` files under
/// `<codexHome>/sessions/` (one folder per year, month and day, every project's
/// sessions together) whose first record, `session_meta`, names `workingDirectory`
/// as its cwd (compared as normalized paths, case-insensitively on Windows). A
/// prompt is an `event_msg` whose `item_completed` item is a `UserMessage`. Only the
/// first line of another project's file is read.
CliSessionList ReadCodexSessions(const std::filesystem::path& codexHome,
                                 const std::filesystem::path& workingDirectory, const CancelToken& cancel);
} // namespace CliSessionFiles
} // namespace GameEngine
