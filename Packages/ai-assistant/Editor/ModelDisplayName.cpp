#include "ModelDisplayName.h"

#include <algorithm>

namespace GameEngine
{
std::string ModelDisplayName(std::string_view model)
{
    model = model.substr(0, model.find('['));
    constexpr std::string_view kPrefix = "claude-";
    const bool prefixed = model.starts_with(kPrefix);
    const std::string_view rest = prefixed ? model.substr(kPrefix.size()) : model;
    const size_t dash = rest.find('-');
    const std::string_view family = rest.substr(0, dash);
    if (family.empty() ||
        !std::all_of(family.begin(), family.end(), [](char c) { return c >= 'a' && c <= 'z'; }) ||
        (!prefixed && dash != std::string_view::npos))
        return std::string(model);

    std::string name(family);
    name[0] = static_cast<char>(name[0] - 'a' + 'A');
    // The version: the short numeric parts after the family, joined with dots.
    std::string version;
    std::string_view parts = dash == std::string_view::npos ? std::string_view() : rest.substr(dash + 1);
    while (!parts.empty())
    {
        const std::string_view part = parts.substr(0, parts.find('-'));
        if (part.empty() || part.size() > 2 ||
            !std::all_of(part.begin(), part.end(), [](char c) { return c >= '0' && c <= '9'; }))
            break;
        version += (version.empty() ? "" : ".") + std::string(part);
        parts = part.size() < parts.size() ? parts.substr(part.size() + 1) : std::string_view();
    }
    if (prefixed && version.empty())
        return std::string(model);
    return version.empty() ? name : name + " " + version;
}
} // namespace GameEngine
