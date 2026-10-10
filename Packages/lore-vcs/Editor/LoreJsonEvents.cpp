#include "LoreJsonEvents.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <ctime>

namespace GameEngine
{
namespace
{

constexpr const char* kTagName = "tagName";
constexpr const char* kData = "data";

// Metadata keys the CLI stamps on every commit (lore-revision metadata.rs).
constexpr const char* kMetadataMessage = "message";
constexpr const char* kMetadataTimestamp = "timestamp";
constexpr const char* kMetadataCreatedBy = "created-by";

std::string TagOf(const nlohmann::json& event)
{
    if (!event.is_object())
        return {};
    const auto it = event.find(kTagName);
    return (it != event.end() && it->is_string()) ? it->get<std::string>() : std::string{};
}

const nlohmann::json* DataOf(const nlohmann::json& event)
{
    if (!event.is_object())
        return nullptr;
    const auto it = event.find(kData);
    return it != event.end() ? &*it : nullptr;
}

std::string StringField(const nlohmann::json& object, const char* key)
{
    const auto it = object.find(key);
    return (it != object.end() && it->is_string()) ? it->get<std::string>() : std::string{};
}

bool BoolField(const nlohmann::json& object, const char* key)
{
    const auto it = object.find(key);
    if (it == object.end())
        return false;
    if (it->is_boolean())
        return it->get<bool>();
    if (it->is_number_integer())
        return it->get<int64_t>() != 0;
    return false;
}

// serde_json writes u64 fields as bare digits; a parsed value is unsigned, a
// programmatically built one may be signed. Negative values are rejected.
bool UnsignedField(const nlohmann::json& object, const char* key, uint64_t& outValue)
{
    const auto it = object.find(key);
    if (it == object.end() || !it->is_number_integer())
        return false;
    if (!it->is_number_unsigned() && it->get<int64_t>() < 0)
        return false;
    outValue = it->get<uint64_t>();
    return true;
}

void NormalizeRelPath(std::string& path)
{
    std::replace(path.begin(), path.end(), '\\', '/');
    while (!path.empty() && path.front() == '/')
        path.erase(path.begin());
}

// Commit timestamps are Unix epoch milliseconds.
std::string FormatTimestampMs(uint64_t millis)
{
    const std::time_t seconds = static_cast<std::time_t>(millis / 1000);
    std::tm utc{};
#ifdef _WIN32
    if (gmtime_s(&utc, &seconds) != 0)
        return {};
#else
    if (gmtime_r(&seconds, &utc) == nullptr)
        return {};
#endif
    char buffer[32];
    if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &utc) == 0)
        return {};
    return buffer;
}

// A metadata value is itself tagged: {"tagName":"string","data":"..."} or
// {"tagName":"numeric","data":123}.
void ApplyMetadata(const nlohmann::json& data, VCSLogEntry& entry)
{
    const std::string key = StringField(data, "key");
    const auto valueIt = data.find("value");
    if (key.empty() || valueIt == data.end() || !valueIt->is_object())
        return;
    const nlohmann::json* value = DataOf(*valueIt);
    if (value == nullptr)
        return;

    if (key == kMetadataMessage && value->is_string())
        entry.message = value->get<std::string>();
    else if (key == kMetadataCreatedBy && value->is_string())
        entry.author = value->get<std::string>();
    else if (key == kMetadataTimestamp && value->is_number_integer() &&
             (value->is_number_unsigned() || value->get<int64_t>() >= 0))
        entry.date = FormatTimestampMs(value->get<uint64_t>());
}

} // namespace

std::vector<nlohmann::json> ParseLoreJsonEvents(std::string_view output)
{
    std::vector<nlohmann::json> events;
    size_t start = 0;
    while (start < output.size())
    {
        size_t end = output.find('\n', start);
        if (end == std::string_view::npos)
            end = output.size();
        std::string_view line = output.substr(start, end - start);
        start = end + 1;

        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.remove_suffix(1);
        if (line.empty() || line.front() != '{')
            continue;

        nlohmann::json event = nlohmann::json::parse(line, nullptr, false);
        if (event.is_discarded())
            continue;
        events.push_back(std::move(event));
    }
    return events;
}

bool ParseLoreStatusFileEvent(const nlohmann::json& data, LoreFileStatusEntry& outEntry)
{
    if (!data.is_object())
        return false;

    std::string path = StringField(data, "path");
    NormalizeRelPath(path);
    if (path.empty())
        return false;

    const std::string action = StringField(data, "action");
    const bool staged = BoolField(data, "flagStaged");
    const bool unresolvedConflict =
        BoolField(data, "flagConflict") && BoolField(data, "flagConflictUnresolved");

    VCSFileStatus status = VCSFileStatus::Modified;
    if (action == "add")
        status = staged ? VCSFileStatus::Added : VCSFileStatus::Unversioned;
    else if (action == "delete")
        status = VCSFileStatus::Deleted;
    if (unresolvedConflict)
        status = VCSFileStatus::Conflict;

    outEntry.Path = std::move(path);
    outEntry.Status = status;
    return true;
}

bool ParseLoreRevisionHeader(const nlohmann::json& data, LoreRevisionHeader& outHeader)
{
    if (!data.is_object())
        return false;
    const std::string branchName = StringField(data, "branchName");
    if (branchName.empty())
        return false;

    outHeader.BranchName = branchName;
    outHeader.Revision = StringField(data, "revision");
    outHeader.RevisionNumber = 0;
    UnsignedField(data, "revisionNumber", outHeader.RevisionNumber);
    return true;
}

std::vector<VCSLogEntry> ParseLoreHistoryEvents(const std::vector<nlohmann::json>& events)
{
    std::vector<VCSLogEntry> entries;
    for (const auto& event : events)
    {
        const std::string tag = TagOf(event);
        const nlohmann::json* data = DataOf(event);
        if (data == nullptr || !data->is_object())
            continue;

        if (tag == "revisionHistoryEntry")
        {
            VCSLogEntry entry;
            uint64_t revisionNumber = 0;
            if (UnsignedField(*data, "revisionNumber", revisionNumber))
                entry.revision = std::to_string(revisionNumber);
            else
                entry.revision = StringField(*data, "revision");
            entries.push_back(std::move(entry));
        }
        else if (tag == "metadata" && !entries.empty())
        {
            ApplyMetadata(*data, entries.back());
        }
    }
    return entries;
}

} // namespace GameEngine
