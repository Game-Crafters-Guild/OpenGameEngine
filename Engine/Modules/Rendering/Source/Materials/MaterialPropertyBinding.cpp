#include "Rendering/Materials/MaterialPropertyBinding.h"

#include <algorithm>
#include <cctype>

namespace GameEngine::Rendering::MaterialPropertyBinding
{
namespace
{
static bool IsUpper(char c) { return std::isupper((unsigned char)c) != 0; }
static bool IsAlphaNum(char c) { return std::isalnum((unsigned char)c) != 0; }

static std::string StripCommonPrefixes(std::string_view s)
{
    // Strip common coding-style prefixes once (best-effort).
    // Examples: uBaseColor -> BaseColor, m_baseColor -> baseColor, gColor -> Color
    if (s.size() >= 2 && (s[0] == 'u' || s[0] == 'm' || s[0] == 'g' || s[0] == 's') && IsUpper(s[1]))
    {
        return std::string(s.substr(1));
    }
    if (s.size() >= 3 && (s[0] == 'u' || s[0] == 'm' || s[0] == 'g' || s[0] == 's') && s[1] == '_')
    {
        return std::string(s.substr(2));
    }
    return std::string(s);
}

static std::string NormalizeForCompare(std::string_view s)
{
    // Lowercase and strip non-alphanumerics so snake_case, camelCase, etc match.
    std::string out;
    out.reserve(s.size());
    for (char c : s)
    {
        if (!IsAlphaNum(c))
            continue;
        out.push_back((char)std::tolower((unsigned char)c));
    }
    return out;
}

static std::string ToLowerCamel(std::string_view s)
{
    std::string out(s);
    if (!out.empty())
        out[0] = (char)std::tolower((unsigned char)out[0]);
    return out;
}
} // namespace

bool NamesEquivalent(std::string_view a, std::string_view b)
{
    if (a.empty() || b.empty())
        return false;
    if (a == b)
        return true;

    // Compare normalized forms with/without common prefixes.
    const std::string a0 = NormalizeForCompare(a);
    const std::string b0 = NormalizeForCompare(b);
    if (!a0.empty() && a0 == b0)
        return true;

    const std::string as = NormalizeForCompare(StripCommonPrefixes(a));
    const std::string bs = NormalizeForCompare(StripCommonPrefixes(b));
    if (!as.empty() && as == bs)
        return true;

    // Also allow matching base name to a prefixed variant.
    if (!as.empty() && as == b0)
        return true;
    if (!bs.empty() && bs == a0)
        return true;

    return false;
}

std::string SuggestPropertyKeyForUniform(std::string_view uniformName)
{
    if (uniformName.empty())
        return {};

    // Prefer stripping prefixes then lowerCamel.
    const std::string stripped = StripCommonPrefixes(uniformName);
    return ToLowerCamel(stripped);
}

std::string ResolvePropertyKeyForUniform(const MaterialDocument& doc, std::string_view uniformName)
{
    if (uniformName.empty())
        return {};

    // 1) Explicit user mapping (uniform -> propertyKey)
    if (!doc.bindings.empty())
    {
        auto it = doc.bindings.find(std::string(uniformName));
        if (it != doc.bindings.end() && !it->second.empty())
        {
            return it->second;
        }
    }

    // 2) Exact match
    if (doc.properties.find(std::string(uniformName)) != doc.properties.end())
    {
        return std::string(uniformName);
    }

    // 3) Heuristic match against existing property keys
    for (const auto& kv : doc.properties)
    {
        if (NamesEquivalent(kv.first, uniformName))
        {
            return kv.first;
        }
    }

    // 4) Suggested new key
    return SuggestPropertyKeyForUniform(uniformName);
}

bool TryGetFloat4(const MaterialDocument& doc, std::string_view uniformName, float out4[4])
{
    if (!out4)
        return false;
    const std::string key = ResolvePropertyKeyForUniform(doc, uniformName);
    if (key.empty())
        return false;

    auto it = doc.properties.find(key);
    if (it == doc.properties.end())
        return false;

    if (auto* arr = std::get_if<std::vector<float>>(&it->second))
    {
        if (arr->size() < 4)
            return false;
        out4[0] = (*arr)[0];
        out4[1] = (*arr)[1];
        out4[2] = (*arr)[2];
        out4[3] = (*arr)[3];
        return true;
    }
    return false;
}
} // namespace GameEngine::Rendering::MaterialPropertyBinding

