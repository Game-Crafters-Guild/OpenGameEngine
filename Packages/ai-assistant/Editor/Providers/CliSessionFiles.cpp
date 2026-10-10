#include "CliSessionFiles.h"

#include "CancelToken.h"
#include "JsonFields.h"

#include "Types/StringUtils.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <optional>
#include <string_view>
#include <system_error>

#include <nlohmann/json.hpp>

namespace GameEngine::CliSessionFiles
{
namespace
{
namespace fs = std::filesystem;

// Every Claude prompt record holds this text, however its JSON is spaced; parsing
// only those lines skips most of the large attachment and reply records.
constexpr std::string_view kClaudeUserMarker = R"("user")";
// Every Codex prompt record holds this text.
constexpr std::string_view kCodexUserMarker = R"("UserMessage")";
// A read checks for cancellation once per this many lines of a file, so a session file
// of many megabytes never holds up the panel's close until it ends.
constexpr uint32_t kLinesPerCancelCheck = 256;

// One line of a session file without its line ending; false at the end of the file.
bool ReadLine(std::ifstream& file, std::string& line)
{
    if (!std::getline(file, line))
        return false;
    if (!line.empty() && line.back() == '\r')
        line.pop_back();
    return true;
}

// The first non-empty line of `text`, trimmed and cut at kMaxFirstPromptBytes on a
// character boundary.
std::string PromptLine(std::string_view text)
{
    while (!text.empty())
    {
        const size_t end = text.find_first_of("\r\n");
        const std::string_view line = TrimWhitespaceView(text.substr(0, end));
        if (!line.empty())
        {
            size_t cut = std::min(line.size(), CliSessionSummary::kMaxFirstPromptBytes);
            // Back off UTF-8 continuation bytes so the cut keeps whole characters.
            while (cut < line.size() && cut > 0 && (static_cast<unsigned char>(line[cut]) & 0xC0) == 0x80)
                --cut;
            return std::string(line.substr(0, cut));
        }
        if (end == std::string_view::npos)
            break;
        text.remove_prefix(end + 1);
    }
    return {};
}

std::chrono::system_clock::time_point FileTime(const fs::path& path)
{
    std::error_code error;
    const fs::file_time_type written = fs::last_write_time(path, error);
    if (error)
        return {};
    // clock_cast is not available in every deployed libc++.
    return std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        written - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
}

bool IsTrue(const nlohmann::json& object, const char* name)
{
    const auto it = object.find(name);
    return it != object.end() && it->is_boolean() && it->get<bool>();
}

// The text of a Claude prompt record; nullopt for any other record.
std::optional<std::string> ClaudePromptText(const nlohmann::json& record)
{
    if (JsonFields::String(record, "type") != "user" || IsTrue(record, "isMeta") || IsTrue(record, "isSidechain"))
        return std::nullopt;
    const nlohmann::json& message = JsonFields::Object(record, "message");
    const auto content = message.find("content");
    if (content == message.end())
        return std::nullopt;
    if (content->is_string())
        return content->get<std::string>();
    if (!content->is_array())
        return std::nullopt;
    std::optional<std::string> text;
    for (const nlohmann::json& block : *content)
    {
        if (!block.is_object())
            continue;
        const std::string type = JsonFields::String(block, "type");
        if (type == "tool_result")
            return std::nullopt;
        if (type == "text" && !text)
            text = JsonFields::String(block, "text");
    }
    return text;
}

// The text of a Codex prompt record; nullopt for any other record.
std::optional<std::string> CodexPromptText(const nlohmann::json& record)
{
    if (JsonFields::String(record, "type") != "event_msg")
        return std::nullopt;
    const nlohmann::json& payload = JsonFields::Object(record, "payload");
    if (JsonFields::String(payload, "type") != "item_completed")
        return std::nullopt;
    const nlohmann::json& item = JsonFields::Object(payload, "item");
    if (JsonFields::String(item, "type") != "UserMessage")
        return std::nullopt;
    std::string text;
    if (const auto content = item.find("content"); content != item.end() && content->is_array())
        for (const nlohmann::json& block : *content)
            if (block.is_object() && JsonFields::String(block, "type") == "text")
                text += JsonFields::String(block, "text");
    return text;
}

// Counts a prompt into `session`, keeping the first one's line.
void AddPrompt(CliSessionSummary& session, const std::string& text)
{
    if (session.PromptCount++ == 0)
        session.FirstPrompt = PromptLine(text);
}

// True when `cancel` fired; checked once every kLinesPerCancelCheck calls.
bool CancelledAtLine(uint32_t& line, const CancelToken& cancel)
{
    return ++line % kLinesPerCancelCheck == 0 && cancel.IsCancelled();
}

// Reads one Claude session file; nullopt when no line of it is a JSON object or the
// read was cancelled.
std::optional<CliSessionSummary> ReadClaudeFile(const fs::path& path, const CancelToken& cancel)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return std::nullopt;
    CliSessionSummary session;
    session.Id = path.stem().string();
    bool understood = false;
    std::string line;
    uint32_t lineNumber = 0;
    while (ReadLine(file, line))
    {
        if (CancelledAtLine(lineNumber, cancel))
            return std::nullopt;
        if (understood && line.find(kClaudeUserMarker) == std::string::npos)
            continue;
        const nlohmann::json record = nlohmann::json::parse(line, nullptr, false);
        if (!record.is_object())
            continue;
        understood = true;
        if (const std::optional<std::string> text = ClaudePromptText(record))
            AddPrompt(session, *text);
    }
    if (!understood)
        return std::nullopt;
    session.Time = FileTime(path);
    return session;
}

// `path` spelled so two spellings of one directory compare equal: normalized, native
// separators, no trailing separator, and ASCII-lowercased where paths ignore case.
std::string ComparablePath(const fs::path& path)
{
    std::string text = fs::path(path).make_preferred().lexically_normal().string();
    while (text.size() > 1 && (text.back() == '/' || text.back() == '\\'))
        text.pop_back();
#if defined(_WIN32)
    return ToLowerAscii(text);
#else
    return text;
#endif
}

void SortNewestFirst(CliSessionList& list)
{
    std::stable_sort(list.Sessions.begin(), list.Sessions.end(),
                     [](const CliSessionSummary& a, const CliSessionSummary& b) { return a.Time > b.Time; });
}

bool IsCodexSessionFile(const fs::path& path)
{
    const std::string name = path.filename().string();
    return name.starts_with("rollout-") && path.extension() == ".jsonl";
}
} // namespace

std::string ClaudeProjectFolderName(const fs::path& workingDirectory)
{
    // Per UTF-16 code unit, as Claude Code (a JavaScript program) replaces them.
    const std::u16string units = workingDirectory.u16string();
    std::string name(units.size(), '-');
    for (size_t index = 0; index < units.size(); ++index)
        if (units[index] < 0x80 && std::isalnum(static_cast<unsigned char>(units[index])))
            name[index] = static_cast<char>(units[index]);
    return name;
}

CliSessionList ReadClaudeSessions(const fs::path& claudeConfigDirectory, const fs::path& workingDirectory,
                                  const CancelToken& cancel)
{
    CliSessionList list;
    if (workingDirectory.empty())
        return list;
    const fs::path folder = claudeConfigDirectory / "projects" / ClaudeProjectFolderName(workingDirectory);
    std::error_code error;
    for (fs::directory_iterator it(folder, error), end; !error && it != end; it.increment(error))
    {
        if (cancel.IsCancelled())
            break;
        if (!it->is_regular_file(error) || it->path().extension() != ".jsonl")
            continue;
        std::optional<CliSessionSummary> session = ReadClaudeFile(it->path(), cancel);
        if (session)
            list.Sessions.push_back(std::move(*session));
        else if (!cancel.IsCancelled())
            ++list.SkippedFiles;
    }
    SortNewestFirst(list);
    return list;
}

CliSessionList ReadCodexSessions(const fs::path& codexHome, const fs::path& workingDirectory,
                                 const CancelToken& cancel)
{
    CliSessionList list;
    if (workingDirectory.empty())
        return list;
    const std::string project = ComparablePath(workingDirectory);
    std::error_code error;
    for (fs::recursive_directory_iterator it(codexHome / "sessions", fs::directory_options::skip_permission_denied,
                                             error),
         end;
         !error && it != end; it.increment(error))
    {
        if (cancel.IsCancelled())
            break;
        if (!it->is_regular_file(error) || !IsCodexSessionFile(it->path()))
            continue;
        std::ifstream file(it->path(), std::ios::binary);
        std::string line;
        if (!file || !ReadLine(file, line))
        {
            ++list.SkippedFiles;
            continue;
        }
        const nlohmann::json meta = nlohmann::json::parse(line, nullptr, false);
        if (!meta.is_object() || JsonFields::String(meta, "type") != "session_meta")
        {
            ++list.SkippedFiles;
            continue;
        }
        const nlohmann::json& payload = JsonFields::Object(meta, "payload");
        CliSessionSummary session;
        session.Id = JsonFields::String(payload, "id");
        if (session.Id.empty())
        {
            ++list.SkippedFiles;
            continue;
        }
        const std::string cwd = JsonFields::String(payload, "cwd");
        if (cwd.empty() || ComparablePath(cwd) != project)
            continue;
        uint32_t lineNumber = 0;
        while (ReadLine(file, line) && !CancelledAtLine(lineNumber, cancel))
        {
            if (line.find(kCodexUserMarker) == std::string::npos)
                continue;
            const nlohmann::json record = nlohmann::json::parse(line, nullptr, false);
            if (!record.is_object())
                continue;
            if (const std::optional<std::string> text = CodexPromptText(record))
                AddPrompt(session, *text);
        }
        session.Time = FileTime(it->path());
        list.Sessions.push_back(std::move(session));
    }
    SortNewestFirst(list);
    return list;
}
} // namespace GameEngine::CliSessionFiles
