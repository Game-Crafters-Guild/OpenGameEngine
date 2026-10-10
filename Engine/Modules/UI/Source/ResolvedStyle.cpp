#include "UI/ResolvedStyle.h"

#include "Parsers/CSSParserDetail.h"

#include <Types/ParseNumber.h>
#include <string>
#include <unordered_map>
#include <vector>

namespace GameEngine
{

namespace
{

using Entry = CustomPropertyScope::Entry;

// A custom property's value as a reader sees it: the declared text, with any var() substituted
// against the reader's scope. That is how the cascade resolves a real property that uses the
// variable, so C++ reading a variable and CSS using it agree. A typed value cached on the entry is
// reused when it was computed for every reader (no var() in the text) or for this reader's scope;
// scopes are immutable once built and their ids never repeat, so that cache cannot go stale.
class ComputedCustomValue
{
public:
    ComputedCustomValue(const CustomPropertyScope* scope, StringId name)
    {
        if (!scope || scope->Lookup(name, m_Entry) != CustomPropertyScope::LookupStatus::Found)
            m_Entry = nullptr;
        m_ReaderScopeId = scope ? scope->UniqueId : 0;
    }

    bool Found() const { return m_Entry != nullptr; }

    bool HasCached(Entry::Kind kind) const
    {
        return m_Entry->CachedType == kind &&
               (m_Entry->CachedScopeId == 0 || m_Entry->CachedScopeId == m_ReaderScopeId);
    }

    const Entry& CachedEntry() const { return *m_Entry; }

    // Null when a var() reference in the value does not resolve and has no fallback.
    const std::string* Text(const CustomPropertyScope* scope)
    {
        m_DependsOnReader = UIParsing::CSSDetail::ContainsVarCall(m_Entry->Value);
        if (!m_DependsOnReader)
            return &m_Entry->Value;
        static thread_local std::string tl_resolved;
        static thread_local std::vector<std::string> tl_stack;
        static const std::unordered_map<StringId, std::string> kNoLocalVars;
        tl_resolved.clear();
        tl_stack.clear();
        if (!UIParsing::CSSDetail::ResolveVarFunctions(kNoLocalVars, scope, m_Entry->Value, tl_resolved,
                                                       /*depth=*/0, tl_stack))
            return nullptr;
        return &tl_resolved;
    }

    // Whether the value Text() computed may be cached on the entry: a reader-dependent value
    // needs the reader's scope id to key it.
    bool CanCache() const { return !m_DependsOnReader || m_ReaderScopeId != 0; }

    // Records that the entry's typed cache now holds a value computed by Text(). Only after
    // CanCache().
    void MarkCached(Entry::Kind kind) const
    {
        m_Entry->CachedType = kind;
        m_Entry->CachedScopeId = m_DependsOnReader ? m_ReaderScopeId : 0;
    }

private:
    const Entry* m_Entry = nullptr;
    uint64_t m_ReaderScopeId = 0;
    bool m_DependsOnReader = false;
};

std::optional<StyleLength> ParseLength(const std::string& s)
{
    if (s.empty())
        return std::nullopt;
    if (s == "auto")
        return StyleLength::Auto();

    size_t consumed = 0;
    auto numOpt = GameEngine::ParseFloat(s, &consumed);
    if (!numOpt)
        return std::nullopt;

    std::string_view suffix(s.data() + consumed, s.size() - consumed);
    if (suffix == "%" || suffix == "pct")
        return StyleLength::Percent(*numOpt);
    return StyleLength::Px(*numOpt);
}

} // namespace

std::optional<float> ResolvedStyle::GetCustomNumber(StringId name) const
{
    ComputedCustomValue value(CustomScope.get(), name);
    if (!value.Found())
        return std::nullopt;
    if (value.HasCached(Entry::Kind::Float))
        return value.CachedEntry().CachedFloat;

    const std::string* text = value.Text(CustomScope.get());
    if (!text)
        return std::nullopt;
    auto parsed = GameEngine::ParseFloat(*text);
    if (parsed && value.CanCache())
    {
        value.CachedEntry().CachedFloat = *parsed;
        value.MarkCached(Entry::Kind::Float);
    }
    return parsed;
}

std::optional<std::string> ResolvedStyle::GetCustomString(StringId name) const
{
    ComputedCustomValue value(CustomScope.get(), name);
    if (!value.Found())
        return std::nullopt;
    const std::string* text = value.Text(CustomScope.get());
    if (!text)
        return std::nullopt;
    return *text;
}

std::optional<uint32_t> ResolvedStyle::GetCustomColor(StringId name) const
{
    ComputedCustomValue value(CustomScope.get(), name);
    if (!value.Found())
        return std::nullopt;
    if (value.HasCached(Entry::Kind::Color))
        return value.CachedEntry().CachedColor;

    const std::string* text = value.Text(CustomScope.get());
    if (!text)
        return std::nullopt;
    auto parsed = UIParsing::CSSDetail::TryParseColor(*text);
    if (parsed && value.CanCache())
    {
        value.CachedEntry().CachedColor = *parsed;
        value.MarkCached(Entry::Kind::Color);
    }
    return parsed;
}

std::optional<StyleLength> ResolvedStyle::GetCustomLength(StringId name) const
{
    ComputedCustomValue value(CustomScope.get(), name);
    if (!value.Found())
        return std::nullopt;
    if (value.HasCached(Entry::Kind::Length))
        return value.CachedEntry().CachedLength;

    const std::string* text = value.Text(CustomScope.get());
    if (!text)
        return std::nullopt;
    auto parsed = ParseLength(*text);
    if (parsed && value.CanCache())
    {
        value.CachedEntry().CachedLength = *parsed;
        value.MarkCached(Entry::Kind::Length);
    }
    return parsed;
}

} // namespace GameEngine
