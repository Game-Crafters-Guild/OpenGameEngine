#include "Projects/ProjectCatalog.h"

#include "Logger/Logger.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <unordered_set>

namespace GameEngine::Editor
{

namespace
{

bool IsHttpRef(const std::string& ref)
{
    return ref.rfind("http://", 0) == 0 || ref.rfind("https://", 0) == 0;
}

// Manifest ids name cache files and staging directories, so they must be
// plain slugs — anything else could escape those directories.
bool IsValidCatalogId(const std::string& id)
{
    if (id.empty() || id.size() > 64)
        return false;
    for (const char c : id)
    {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok)
            return false;
    }
    return true;
}

// Source paths and relative thumbnail refs are joined under directories we
// own; reject traversal, absolute forms, and option-lookalikes ('-' prefix
// would be parsed as a git flag).
bool IsSafeRelativePath(const std::string& path)
{
    if (path.empty() || path.front() == '-' ||
        path.find('\\') != std::string::npos || path.find(':') != std::string::npos)
        return false;
    if (std::filesystem::path(path).is_absolute())
        return false;
    size_t start = 0;
    while (start <= path.size())
    {
        const size_t end = path.find('/', start);
        const std::string segment =
            path.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (segment.empty() || segment == "." || segment == "..")
            return false;
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return true;
}

std::string ResolveThumbnailRef(const std::string& raw, const std::string& base)
{
    if (raw.empty() || IsHttpRef(raw))
        return raw;
    if (!IsSafeRelativePath(raw) || base.empty())
        return {};
    const char last = base.back();
    const bool needsSeparator = last != '/' && last != '\\';
    return needsSeparator ? base + "/" + raw : base + raw;
}

ProjectSourceType ParseSourceType(const std::string& type)
{
    if (type.empty() || type == "none")
        return ProjectSourceType::None;
    if (type == "local")
        return ProjectSourceType::Local;
    if (type == "git")
        return ProjectSourceType::Git;
    if (type == "zip")
        return ProjectSourceType::Zip;
    return ProjectSourceType::Unsupported;
}

std::string GetString(const nlohmann::json& obj, const char* key)
{
    const auto it = obj.find(key);
    return it != obj.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

uint32_t ParseAccentColor(const std::string& hex)
{
    if (hex.size() != 7 || hex[0] != '#')
        return 0;
    uint32_t rgb = 0;
    for (size_t i = 1; i < hex.size(); ++i)
    {
        const char c = hex[i];
        uint32_t nibble = 0;
        if (c >= '0' && c <= '9')
            nibble = static_cast<uint32_t>(c - '0');
        else if (c >= 'a' && c <= 'f')
            nibble = static_cast<uint32_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            nibble = static_cast<uint32_t>(c - 'A' + 10);
        else
            return 0;
        rgb = (rgb << 4) | nibble;
    }
    return 0xFF000000u | rgb;
}

} // namespace

bool ParseProjectCatalog(const std::string& jsonText,
                         const std::string& thumbnailBase,
                         ProjectCatalog& outCatalog,
                         std::string* outError)
{
    outCatalog = {};

    nlohmann::json root = nlohmann::json::parse(jsonText, nullptr, /*allow_exceptions=*/false);
    if (root.is_discarded() || !root.is_object())
    {
        if (outError)
            *outError = "Manifest is not a valid JSON object.";
        return false;
    }

    const auto versionIt = root.find("schemaVersion");
    outCatalog.SchemaVersion =
        versionIt != root.end() && versionIt->is_number_integer() ? versionIt->get<int>() : 0;
    outCatalog.Name = GetString(root, "name");

    const auto projectsIt = root.find("projects");
    if (projectsIt == root.end() || !projectsIt->is_array())
    {
        if (outError)
            *outError = "Manifest has no \"projects\" array.";
        return false;
    }

    std::unordered_set<std::string> seenIds;
    for (const auto& item : *projectsIt)
    {
        if (!item.is_object())
            continue;

        ProjectCatalogEntry entry;
        entry.Id = GetString(item, "id");
        entry.Name = GetString(item, "name");
        if (!IsValidCatalogId(entry.Id) || entry.Name.empty())
        {
            Logger::Log::Warning("Project manifest entry skipped: missing name or invalid id '{}'",
                                 entry.Id);
            continue;
        }
        if (!seenIds.insert(entry.Id).second)
        {
            Logger::Log::Warning("Project manifest entry skipped: duplicate id '{}'", entry.Id);
            continue;
        }

        entry.Description = GetString(item, "description");
        entry.Author = GetString(item, "author");
        entry.EngineVersion = GetString(item, "engineVersion");
        if (const auto starsIt = item.find("stars");
            starsIt != item.end() && starsIt->is_number_integer())
        {
            entry.Stars = starsIt->get<int64_t>();
        }
        entry.ThumbnailRef = ResolveThumbnailRef(GetString(item, "thumbnail"), thumbnailBase);
        entry.AccentColor = ParseAccentColor(GetString(item, "accentColor"));
        entry.ThumbnailContain = GetString(item, "thumbnailFit") == "contain";

        if (const auto sourceIt = item.find("source"); sourceIt != item.end() && sourceIt->is_object())
        {
            entry.Source.Type = ParseSourceType(GetString(*sourceIt, "type"));
            entry.Source.Url = GetString(*sourceIt, "url");
            entry.Source.Ref = GetString(*sourceIt, "ref");
            entry.Source.Path = GetString(*sourceIt, "path");
            // Downgrade rather than skip so the card still shows with a clear
            // "not supported" error on create. Git/zip URLs are restricted to
            // http(s): file/ssh/ext transports would let a hostile manifest
            // read local paths or execute commands via git remote helpers.
            const bool remoteSource = entry.Source.Type == ProjectSourceType::Git ||
                                      entry.Source.Type == ProjectSourceType::Zip;
            const bool invalidSource =
                (remoteSource &&
                 (!IsHttpRef(entry.Source.Url) ||
                  (!entry.Source.Path.empty() && !IsSafeRelativePath(entry.Source.Path)))) ||
                (entry.Source.Type == ProjectSourceType::Local &&
                 !IsSafeRelativePath(entry.Source.Path));
            if (invalidSource)
                entry.Source.Type = ProjectSourceType::Unsupported;
        }

        outCatalog.Entries.push_back(std::move(entry));
    }

    return true;
}

} // namespace GameEngine::Editor
