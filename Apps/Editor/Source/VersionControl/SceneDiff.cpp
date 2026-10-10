#include "VersionControl/SceneDiff.h"

#include <algorithm>
#include <cctype>
#include <unordered_map>

namespace GameEngine::Editor
{
namespace
{
struct ParsedObject
{
    std::string StableKey;
    std::string Label;
    std::string Parent;
    bool IsEntity = false;
    std::vector<std::string> PropertyOrder;
    std::unordered_map<std::string, std::string> Properties;
};

struct ParsedScene
{
    std::vector<std::string> Order;
    std::unordered_map<std::string, ParsedObject> Objects;
};

std::string_view Trim(std::string_view value)
{
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
        value.remove_prefix(1);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
        value.remove_suffix(1);
    return value;
}

std::string Unquote(std::string_view value)
{
    value = Trim(value);
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
        value = value.substr(1, value.size() - 2);
    return std::string(value);
}

std::string Attribute(std::string_view header, std::string_view name)
{
    const std::string needle = std::string(name) + "=";
    size_t pos = 0;
    do
    {
        pos = header.find(needle, pos);
        if (pos == std::string_view::npos)
            return {};
        const bool attributeBoundary =
            pos == 0 || std::isspace(static_cast<unsigned char>(header[pos - 1])) || header[pos - 1] == '[';
        if (attributeBoundary)
            break;
        pos += needle.size();
    } while (pos < header.size());
    pos += needle.size();
    if (pos >= header.size())
        return {};
    if (header[pos] == '"')
    {
        const size_t end = header.find('"', pos + 1);
        return std::string(header.substr(pos + 1, end == std::string_view::npos ? header.size() - pos - 1
                                                                               : end - pos - 1));
    }
    const size_t end = header.find_first_of(" \t]", pos);
    return std::string(header.substr(pos, end == std::string_view::npos ? header.size() - pos : end - pos));
}

ParsedScene Parse(std::string_view content)
{
    ParsedScene result;
    ParsedObject* current = nullptr;
    std::unordered_map<std::string, size_t> anonymousCounts;

    size_t cursor = 0;
    while (cursor <= content.size())
    {
        size_t end = content.find('\n', cursor);
        if (end == std::string_view::npos)
            end = content.size();
        // Trim already drops the CR of a CRLF line ending.
        std::string_view line = Trim(content.substr(cursor, end - cursor));

        // A leading '[' is unambiguous: property lines always start with their
        // key. A truncated header still opens its section — folding it into the
        // previous object instead would drop it from the diff entirely and
        // report it as added against an intact baseline.
        if (!line.empty() && line.front() == '[')
        {
            const size_t typeEnd = line.find_first_of(" \t]", 1);
            const std::string type(
                line.substr(1, typeEnd == std::string_view::npos ? std::string_view::npos
                                                                 : typeEnd - 1));
            const std::string id = Attribute(line, "id");
            const std::string path = Attribute(line, "path");
            const std::string parent = Attribute(line, "parent");
            const bool isEntity = type == "entity" || type == "blueprint" || type == "subscene";

            // The scene header is metadata rather than an inspectable object.
            if (type == "scene")
            {
                current = nullptr;
            }
            else
            {
                std::string identity = !id.empty() ? id : path;
                // Sections with neither id nor path fall back to their ordinal
                // among same-type sections. Stable for the singleton sections
                // the scene writer emits ([editor_camera], [hierarchy_ui]);
                // inserting one of a repeated anonymous type would renumber the
                // rest and diff them as changed.
                if (identity.empty())
                    identity = std::to_string(anonymousCounts[type]++);
                const std::string stableKey = type + ":" + identity;
                auto [it, inserted] = result.Objects.try_emplace(stableKey);
                current = &it->second;
                if (inserted)
                {
                    current->StableKey = stableKey;
                    current->IsEntity = isEntity;
                    current->Label = !id.empty() ? id : (!path.empty() ? path : type);
                    current->Parent = parent;
                    result.Order.push_back(stableKey);
                }
            }
        }
        else if (current && !line.empty() && line.front() != ';' && line.front() != '#')
        {
            const size_t equals = line.find('=');
            if (equals != std::string_view::npos)
            {
                const std::string key(Trim(line.substr(0, equals)));
                const std::string value(Trim(line.substr(equals + 1)));
                if (!current->Properties.contains(key))
                    current->PropertyOrder.push_back(key);
                current->Properties[key] = value;
                if (key == "Name.value")
                    current->Label = Unquote(value);
            }
        }

        if (end == content.size())
            break;
        cursor = end + 1;
    }
    return result;
}

template <typename T>
std::vector<T> OrderedUnion(const std::vector<T>& first, const std::vector<T>& second)
{
    std::vector<T> result = first;
    for (const T& value : second)
        if (std::find(result.begin(), result.end(), value) == result.end())
            result.push_back(value);
    return result;
}
} // namespace

uint32_t SceneDiffStateColorArgb(SceneDiffState state)
{
    switch (state)
    {
        case SceneDiffState::Added: return 0xFF9DFF00u;
        case SceneDiffState::Removed: return 0xFFFF9500u;
        case SceneDiffState::Modified: return 0xFF549BFFu;
        case SceneDiffState::Unchanged: break;
    }
    return 0xFF888888u;
}

std::string SceneSchemaFieldName(std::string_view field)
{
    std::string folded(field);
    for (char& ch : folded)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return folded;
}

const char* SceneDiffStateLabel(SceneDiffState state)
{
    switch (state)
    {
        case SceneDiffState::Added: return "Added";
        case SceneDiffState::Removed: return "Removed";
        case SceneDiffState::Modified: return "Modified";
        case SceneDiffState::Unchanged: break;
    }
    return "Unchanged";
}

std::vector<SceneObjectDiff> BuildSceneDiff(std::string_view originalContent,
                                            std::string_view currentContent)
{
    const ParsedScene original = Parse(originalContent);
    const ParsedScene current = Parse(currentContent);
    std::vector<SceneObjectDiff> result;

    for (const std::string& objectKey : OrderedUnion(original.Order, current.Order))
    {
        const auto oldIt = original.Objects.find(objectKey);
        const auto newIt = current.Objects.find(objectKey);
        const bool hasOld = oldIt != original.Objects.end();
        const bool hasNew = newIt != current.Objects.end();

        SceneObjectDiff object;
        object.StableKey = objectKey;
        object.OriginalLabel = hasOld ? oldIt->second.Label : std::string{};
        object.CurrentLabel = hasNew ? newIt->second.Label : std::string{};
        object.IsEntity = hasOld ? oldIt->second.IsEntity : newIt->second.IsEntity;
        object.OriginalParent = hasOld ? oldIt->second.Parent : std::string{};
        object.CurrentParent = hasNew ? newIt->second.Parent : std::string{};
        object.State = !hasOld ? SceneDiffState::Added
                               : (!hasNew ? SceneDiffState::Removed : SceneDiffState::Unchanged);

        const std::vector<std::string> empty;
        const auto& oldOrder = hasOld ? oldIt->second.PropertyOrder : empty;
        const auto& newOrder = hasNew ? newIt->second.PropertyOrder : empty;
        for (const std::string& propertyKey : OrderedUnion(oldOrder, newOrder))
        {
            const auto oldProperty = hasOld ? oldIt->second.Properties.find(propertyKey)
                                            : std::unordered_map<std::string, std::string>::const_iterator{};
            const auto newProperty = hasNew ? newIt->second.Properties.find(propertyKey)
                                            : std::unordered_map<std::string, std::string>::const_iterator{};
            const bool hasOldProperty = hasOld && oldProperty != oldIt->second.Properties.end();
            const bool hasNewProperty = hasNew && newProperty != newIt->second.Properties.end();

            ScenePropertyDiff property;
            property.Key = propertyKey;
            if (hasOldProperty)
                property.OriginalValue = oldProperty->second;
            if (hasNewProperty)
                property.CurrentValue = newProperty->second;
            property.State = !hasOldProperty ? SceneDiffState::Added
                                             : (!hasNewProperty ? SceneDiffState::Removed
                                                                : (property.OriginalValue == property.CurrentValue
                                                                       ? SceneDiffState::Unchanged
                                                                       : SceneDiffState::Modified));
            if (property.State != SceneDiffState::Unchanged && object.State == SceneDiffState::Unchanged)
                object.State = SceneDiffState::Modified;
            object.Properties.push_back(std::move(property));
        }

        if (hasOld && hasNew && object.State == SceneDiffState::Unchanged &&
            (object.OriginalLabel != object.CurrentLabel || object.WasReparented()))
            object.State = SceneDiffState::Modified;
        result.push_back(std::move(object));
    }
    return result;
}

} // namespace GameEngine::Editor
