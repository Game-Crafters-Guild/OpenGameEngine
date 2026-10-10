#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine::Editor {

/// Parsed hierarchy search using the conventions shared by Unity's hierarchy
/// search and Unreal's outliner search. Terms are combined with AND.
class HierarchySearchQuery
{
  public:
    enum class Field
    {
        Any,
        Name,
        Id,
        Path,
        Type,
    };

    struct Term
    {
        Field SearchField = Field::Any;
        std::string Value;
        bool Negated = false;
        bool Exact = false;
    };

    struct Candidate
    {
        std::string_view Name;
        std::string_view Id;
        std::string_view Path;
        /// Receives the already-lowercased type search value and exact-match flag.
        std::function<bool(std::string_view, bool)> HasType;
    };

    static HierarchySearchQuery Parse(std::string_view text);

    bool Empty() const { return m_Terms.empty(); }
    const std::vector<Term>& Terms() const { return m_Terms; }

    /// scope is the hierarchy search-bar scope: "all", "name", "type", "path", or "id".
    bool Matches(const Candidate& candidate, std::string_view scope = "all") const;

  private:
    std::vector<Term> m_Terms;
};

} // namespace GameEngine::Editor
