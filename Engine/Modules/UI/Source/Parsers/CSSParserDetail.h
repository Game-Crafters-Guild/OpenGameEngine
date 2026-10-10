#pragma once

// Shared internal declarations for the CSS parser translation units. The hot
// cascade/runtime lives in CSSParser.cpp; cold parsing is split across
// CSSValueParsers.cpp, CSSVarResolver.cpp and CSSStylesheetParser.cpp. Helpers
// referenced by more than one of those TUs live here so there is exactly one
// definition (out-of-line in one TU, or inline/template in this header).

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#include "UI/UIStyle.h"

namespace GameEngine
{
namespace UIParsing
{
namespace CSSDetail
{

inline std::string Trim(const std::string& s)
{
    size_t a = 0;
    while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a])))
        ++a;
    size_t b = s.size();
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])))
        --b;
    return s.substr(a, b - a);
}

inline std::string_view TrimView(std::string_view s)
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.remove_suffix(1);
    return s;
}

inline bool Ieq(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
    {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

template <typename T>
const T& GetValue(const StyleProperty& prop)
{
    return std::get<T>(prop.Value);
}

// Defined in CSSValueParsers.cpp
StyleKeyword ParseStyleKeyword(const std::string& rawValue);
// A CSS colour value in any form the cascade reads (hex, named, rgb()/rgba(), hsl()/hsla(),
// 0xAARRGGBB, decimal ARGB) as ARGB; null when the text is not a colour.
std::optional<uint32_t> TryParseColor(const std::string& value);
void AppendDeclarationProperties(const std::string& name, const std::string& value, std::vector<StyleProperty>& outProps);

// Defined in CSSVarResolver.cpp
bool ContainsVarCall(std::string_view s);
bool ResolveVarFunctions(const std::unordered_map<StringId, std::string>& customVars,
                                const CustomPropertyScope* parentScope,
                                std::string_view input,
                                std::string& out,
                                int depth,
                                std::vector<std::string>& stack);

} // namespace CSSDetail
} // namespace UIParsing
} // namespace GameEngine
