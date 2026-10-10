#pragma once

#include <cctype>
#include <string>
#include <string_view>

namespace GameEngine
{

// A SearchDialog filter chip encodes its category as a "category:<name>\n"
// prefix on the query; the rest is the typed text.
struct PickerQueryFilter
{
    std::string Category;
    std::string LowerQuery;

    static std::string ToLower(std::string_view text)
    {
        std::string out;
        out.reserve(text.size());
        for (char c : text)
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        return out;
    }

    static PickerQueryFilter Parse(const std::string& query)
    {
        PickerQueryFilter filter;
        std::string_view rest = query;
        constexpr std::string_view categoryPrefix = "category:";
        if (rest.rfind(categoryPrefix, 0) == 0)
        {
            const size_t separator = rest.find('\n', categoryPrefix.size());
            if (separator != std::string_view::npos)
            {
                filter.Category.assign(
                    rest.substr(categoryPrefix.size(), separator - categoryPrefix.size()));
                rest.remove_prefix(separator + 1);
            }
        }
        filter.LowerQuery = ToLower(rest);
        return filter;
    }

    bool Matches(const std::string& label, const std::string& category) const
    {
        if (!Category.empty() && category != Category)
            return false;
        if (LowerQuery.empty())
            return true;
        return ToLower(label).find(LowerQuery) != std::string::npos ||
               ToLower(category).find(LowerQuery) != std::string::npos;
    }
};

} // namespace GameEngine
