#pragma once

#include "Types/StringId.h"
#include <compare>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace GameEngine {

struct SelectorChain;

struct AttributeSelector {
    enum class MatchType { Exists, Equals, Includes, DashMatch, Prefix, Suffix, Substring };
    std::string Name;
    std::string Value;
    MatchType Match = MatchType::Exists;
    bool CaseInsensitive = false;
};

struct NthPattern {
    int A = 0;
    int B = 0;
    bool FromEnd = false;
    bool OfType = false;
};

struct PseudoClass {
    enum class Kind {
        Hover,
        Active,
        Focus,
        FocusVisible,
        FocusWithin,
        Disabled,
        Enabled,
        Checked,
        Root,
        Empty,
        FirstChild,
        LastChild,
        OnlyChild,
        FirstOfType,
        LastOfType,
        OnlyOfType,
        NthChild,
        NthLastChild,
        NthOfType,
        NthLastOfType,
        Is,
        Not,
        Where,
        Custom
    };

    Kind PseudoKind = Kind::Hover;
    NthPattern Nth{};
    std::vector<std::shared_ptr<SelectorChain>> SelectorList;
    std::string CustomName;
    // Stage 5 Block C: hashed lowercase form of CustomName, populated by
    // the CSS parser when Kind == Custom. Lets MatchesSelectorChain compare
    // by StringId against UIElement::m_CustomStateIds without per-match
    // string hashing or allocation.
    StringId ResolvedCustomNameId = 0;
};

// One bit per PseudoClass::Kind, for the set of pseudo kinds a stylesheet uses.
constexpr uint32_t PseudoKindBit(PseudoClass::Kind kind)
{
    return 1u << static_cast<uint32_t>(kind);
}
static_assert(static_cast<uint32_t>(PseudoClass::Kind::Custom) < 32u,
              "PseudoKindBit needs a wider mask type");

struct CompoundSelector {
    bool Universal = false;
    std::string Tag;
    StringId ResolvedTagId = 0;  // Deterministic hash of canonical lowercase tag; 0 = unresolved
    std::string Id;
    StringId ResolvedId = 0;     // HashStringId(Id); 0 when Id is empty
    std::vector<std::string> Classes;
    std::vector<StringId> ResolvedClassIds; // Parallel to Classes; HashStringId of each entry
    std::vector<AttributeSelector> Attributes;
    std::vector<PseudoClass> Pseudos;
};

struct SelectorTerm {
    enum class Combinator { None, Descendant, Child, AdjacentSibling, GeneralSibling };
    Combinator Comb = Combinator::None;
    CompoundSelector Selector;
};

// CSS specificity triple. Compared lexicographically (A, then B, then C) —
// never flattened into one integer: a packed encoding gives each tier a
// finite radix, so e.g. 100 class selectors would overflow into the id tier
// and out-rank a genuine #id rule (C-10).
struct Specificity {
    int A = 0;
    int B = 0;
    int C = 0;

    auto operator<=>(const Specificity&) const = default;
};

struct SelectorChain {
    std::vector<SelectorTerm> Terms;
    Specificity Spec{};
};

} // namespace GameEngine



