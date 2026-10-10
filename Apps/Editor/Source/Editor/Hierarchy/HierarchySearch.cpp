#include "Editor/Hierarchy/HierarchySearch.h"

#include <algorithm>
#include <cctype>
#include <utility>

namespace GameEngine::Editor {
namespace {

std::string Lower(std::string_view value)
{
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c)
                   { return static_cast<char>(std::tolower(c)); });
    return result;
}

std::vector<std::string> Tokenize(std::string_view text)
{
    std::vector<std::string> tokens;
    std::string token;
    bool quoted = false;
    bool escaped = false;
    for (char c : text)
    {
        if (escaped)
        {
            token.push_back(c);
            escaped = false;
            continue;
        }
        if (c == '\\' && quoted)
        {
            escaped = true;
            continue;
        }
        if (c == '"')
        {
            quoted = !quoted;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(c)) && !quoted)
        {
            if (!token.empty())
            {
                tokens.push_back(std::move(token));
                token.clear();
            }
            continue;
        }
        token.push_back(c);
    }
    if (escaped)
        token.push_back('\\');
    if (!token.empty())
        tokens.push_back(std::move(token));
    return tokens;
}

bool TextMatches(std::string_view candidate, std::string_view value, bool exact)
{
    const std::string lowerCandidate = Lower(candidate);
    return exact ? lowerCandidate == value : lowerCandidate.find(value) != std::string::npos;
}

} // namespace

HierarchySearchQuery HierarchySearchQuery::Parse(std::string_view text)
{
    HierarchySearchQuery query;
    for (std::string token : Tokenize(text))
    {
        Term term;
        if (!token.empty() && (token.front() == '-' || token.front() == '+'))
        {
            term.Negated = token.front() == '-';
            term.Exact = token.front() == '+';
            token.erase(token.begin());
        }
        if (token.empty())
            continue;

        const size_t colon = token.find(':');
        const size_t equals = token.find('=');
        size_t separator = std::string::npos;
        if (colon != std::string::npos && equals != std::string::npos)
            separator = std::min(colon, equals);
        else
            separator = colon != std::string::npos ? colon : equals;

        if (separator != std::string::npos)
        {
            const std::string field = Lower(std::string_view(token).substr(0, separator));
            Field parsedField = Field::Any;
            bool recognized = true;
            if (field == "n" || field == "name")
                parsedField = Field::Name;
            else if (field == "id")
                parsedField = Field::Id;
            else if (field == "p" || field == "path")
                parsedField = Field::Path;
            else if (field == "t" || field == "type")
                parsedField = Field::Type;
            else
                recognized = false;

            if (recognized)
            {
                term.SearchField = parsedField;
                term.Exact = term.Exact || token[separator] == '=';
                token.erase(0, separator + 1);
            }
        }

        term.Value = Lower(token);
        if (!term.Value.empty())
            query.m_Terms.push_back(std::move(term));
    }
    return query;
}

bool HierarchySearchQuery::Matches(const Candidate& candidate, std::string_view scope) const
{
    for (const Term& term : m_Terms)
    {
        bool matched = false;
        switch (term.SearchField)
        {
            case Field::Name:
                matched = TextMatches(candidate.Name, term.Value, term.Exact);
                break;
            case Field::Id:
                matched = TextMatches(candidate.Id, term.Value, term.Exact);
                break;
            case Field::Path:
                matched = TextMatches(candidate.Path, term.Value, term.Exact);
                break;
            case Field::Type:
                matched = candidate.HasType && candidate.HasType(term.Value, term.Exact);
                break;
            case Field::Any:
                if (scope == "name")
                    matched = TextMatches(candidate.Name, term.Value, term.Exact);
                else if (scope == "id")
                    matched = TextMatches(candidate.Id, term.Value, term.Exact);
                else if (scope == "path")
                    matched = TextMatches(candidate.Path, term.Value, term.Exact);
                else if (scope == "type")
                    matched = candidate.HasType && candidate.HasType(term.Value, term.Exact);
                else
                    matched = TextMatches(candidate.Name, term.Value, term.Exact) ||
                              TextMatches(candidate.Id, term.Value, term.Exact);
                break;
        }
        if (term.Negated ? matched : !matched)
            return false;
    }
    return true;
}

} // namespace GameEngine::Editor
