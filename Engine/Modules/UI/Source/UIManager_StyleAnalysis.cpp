#include "UI/UIManager.h"

#include "UI/Parsers/CSSParser.h"
#include "UI/UIStyle.h"

#include <cstdint>
#include <functional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
constexpr uint32_t kSiblingPositionPseudos =
    GameEngine::PseudoKindBit(GameEngine::PseudoClass::Kind::FirstChild) |
    GameEngine::PseudoKindBit(GameEngine::PseudoClass::Kind::LastChild) |
    GameEngine::PseudoKindBit(GameEngine::PseudoClass::Kind::OnlyChild) |
    GameEngine::PseudoKindBit(GameEngine::PseudoClass::Kind::FirstOfType) |
    GameEngine::PseudoKindBit(GameEngine::PseudoClass::Kind::LastOfType) |
    GameEngine::PseudoKindBit(GameEngine::PseudoClass::Kind::OnlyOfType) |
    GameEngine::PseudoKindBit(GameEngine::PseudoClass::Kind::NthChild) |
    GameEngine::PseudoKindBit(GameEngine::PseudoClass::Kind::NthLastChild) |
    GameEngine::PseudoKindBit(GameEngine::PseudoClass::Kind::NthOfType) |
    GameEngine::PseudoKindBit(GameEngine::PseudoClass::Kind::NthLastOfType);

// Records every sibling-position pseudo and :empty that `chain` uses,
// including inside :is / :where / :not, so a selector such as
// `.row:not(:last-child)` counts.
void CollectStructuralPseudos(const GameEngine::SelectorChain& chain, uint32_t& positionMask,
                              bool& usesEmpty)
{
    using GameEngine::PseudoClass;
    for (const auto& term : chain.Terms)
    {
        for (const PseudoClass& pseudo : term.Selector.Pseudos)
        {
            const uint32_t bit = GameEngine::PseudoKindBit(pseudo.PseudoKind);
            if ((bit & kSiblingPositionPseudos) != 0)
                positionMask |= bit;
            else if (pseudo.PseudoKind == PseudoClass::Kind::Empty)
                usesEmpty = true;
            for (const auto& nested : pseudo.SelectorList)
            {
                if (nested)
                    CollectStructuralPseudos(*nested, positionMask, usesEmpty);
            }
        }
    }
}
} // namespace

void GameEngine::UIManager::RefreshDynamicStyleAnalysis(
    const std::unordered_set<const GameEngine::Stylesheet*>& sheets,
    std::uint64_t sheetHash)
{
    using namespace GameEngine;
    DynamicStyleAnalysis next{};

    struct VarUsage
    {
        bool affectsLayout = false;
        bool affectsPaint = false;
    };

    auto isWs = [](char c) -> bool
    {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
    };
    auto toLowerAscii = [](char c) -> char
    {
        if (c >= 'A' && c <= 'Z')
            return (char)(c - 'A' + 'a');
        return c;
    };

    auto extractVarNames = [&](const std::string& value, std::vector<std::string_view>& outVars) // string_view is safe: sheets are alive for this call
    {
        outVars.clear();
        if (value.empty())
            return;

        for (size_t i = 0; i + 3 < value.size(); ++i)
        {
            if (toLowerAscii(value[i + 0]) != 'v' ||
                toLowerAscii(value[i + 1]) != 'a' ||
                toLowerAscii(value[i + 2]) != 'r' ||
                value[i + 3] != '(')
            {
                continue;
            }

            size_t j = i + 4;
            while (j < value.size() && isWs(value[j]))
                ++j;
            if (j + 1 >= value.size())
                continue;
            if (!(value[j] == '-' && value[j + 1] == '-'))
                continue;

            const size_t start = j;
            j += 2;
            while (j < value.size())
            {
                const char c = value[j];
                if (isWs(c) || c == ',' || c == ')')
                    break;
                ++j;
            }
            if (j > start)
            {
                outVars.emplace_back(std::string_view(value.data() + start, j - start));
            }
        }
    };

    // A deferred (var()-holding) declaration is stored unparsed and carries only
    // the property NAME, so it classifies by name — through the parser's table,
    // which knows shorthands and folds each into the longhands it expands to.
    // A name that table does not know at all is a property this build cannot
    // apply; it reports {false,false} and is escalated here to layout+paint,
    // which is the only safe answer when the impact is genuinely unknown.
    auto deferredImpact = [](std::string_view nameLower) -> StylePropertyImpact
    {
        const StylePropertyImpact impact =
            UIParsing::CSSParser::PropertyImpactForName(std::string(nameLower));
        if (!impact.Layout && !impact.Paint)
            return {true, true};
        return impact;
    };

    auto getDeferredDecl = [](const StyleProperty& prop) -> const DeferredDeclValue&
    {
        return std::get<DeferredDeclValue>(prop.Value);
    };
    auto getCustomVarDecl = [](const StyleProperty& prop) -> const CustomVarDecl&
    {
        return std::get<CustomVarDecl>(prop.Value);
    };

    std::unordered_map<std::string_view, VarUsage> varUsage;
    std::vector<std::string_view> tmpVars;

    auto noteVarUsage = [&](std::string_view varName, bool affectsLayout, bool affectsPaint)
    {
        auto& u = varUsage[varName];
        u.affectsLayout = u.affectsLayout || affectsLayout;
        u.affectsPaint = u.affectsPaint || affectsPaint;
    };

    for (const Stylesheet* s : sheets)
    {
        if (!s)
            continue;
        for (const auto& rule : s->Rules)
        {
            for (const auto& prop : rule.Properties)
            {
                if (prop.PropertyId != StylePropertyId::DeferredDecl)
                    continue;
                const auto& deferred = getDeferredDecl(prop);
                const StylePropertyImpact impact = deferredImpact(deferred.Name);
                extractVarNames(deferred.Value, tmpVars);
                for (const auto& vn : tmpVars)
                {
                    noteVarUsage(vn, impact.Layout, impact.Paint);
                }
            }
        }
    }

    auto propAffectsLayout = [&](const StyleProperty& prop) -> bool
    {
        switch (prop.PropertyId)
        {
        case StylePropertyId::DeferredDecl:
            return deferredImpact(getDeferredDecl(prop).Name).Layout;
        case StylePropertyId::CustomVar:
        {
            const auto& custom = getCustomVarDecl(prop);
            auto it = varUsage.find(std::string_view(custom.Name));
            if (it != varUsage.end())
                return it->second.affectsLayout;
            return true;
        }
        default:
            // Every typed property classifies through the canonical table.
            return GetStylePropertyImpact(prop.PropertyId).Layout;
        }
    };

    auto propAffectsPaint = [&](const StyleProperty& prop) -> bool
    {
        switch (prop.PropertyId)
        {
        case StylePropertyId::DeferredDecl:
            return deferredImpact(getDeferredDecl(prop).Name).Paint;
        case StylePropertyId::CustomVar:
        {
            const auto& custom = getCustomVarDecl(prop);
            auto it = varUsage.find(std::string_view(custom.Name));
            if (it != varUsage.end())
                return it->second.affectsPaint;
            return true;
        }
        default:
            // See propAffectsLayout — one canonical table.
            return GetStylePropertyImpact(prop.PropertyId).Paint;
        }
    };

    auto updateInfo = [&](DynamicStyleAnalysis::PseudoInfo& info,
                          bool ruleLayout,
                          bool rulePaint,
                          bool mayDesc,
                          bool maySib)
    {
        info.Any = true;
        info.AffectsLayout = info.AffectsLayout || ruleLayout;
        info.AffectsPaint = info.AffectsPaint || rulePaint;
        info.MayAffectDescendants = info.MayAffectDescendants || mayDesc;
        info.MayAffectSiblings = info.MayAffectSiblings || maySib;
    };

    std::function<void(const SelectorChain&, bool, bool)> scanChain;
    scanChain = [&](const SelectorChain& chain, bool ruleLayout, bool rulePaint)
    {
        if (chain.Terms.empty())
            return;
        const size_t last = chain.Terms.size() - 1;
        for (size_t ti = 0; ti < chain.Terms.size(); ++ti)
        {
            const auto& term = chain.Terms[ti];
            for (const auto& pseudo : term.Selector.Pseudos)
            {
                auto applyForPseudoKind = [&](PseudoClass::Kind kind)
                {
                    bool mayDesc = false;
                    bool maySib = false;
                    if (ti != last)
                    {
                        for (size_t j = ti + 1; j < chain.Terms.size(); ++j)
                        {
                            switch (chain.Terms[j].Comb)
                            {
                            case SelectorTerm::Combinator::Descendant:
                            case SelectorTerm::Combinator::Child:
                                mayDesc = true;
                                break;
                            case SelectorTerm::Combinator::AdjacentSibling:
                            case SelectorTerm::Combinator::GeneralSibling:
                                maySib = true;
                                break;
                            default:
                                break;
                            }
                        }
                    }

                    switch (kind)
                    {
                    case PseudoClass::Kind::Hover:
                        updateInfo(next.Hover, ruleLayout, rulePaint, mayDesc, maySib);
                        break;
                    case PseudoClass::Kind::Active:
                        updateInfo(next.Active, ruleLayout, rulePaint, mayDesc, maySib);
                        break;
                    case PseudoClass::Kind::Focus:
                        updateInfo(next.Focus, ruleLayout, rulePaint, mayDesc, maySib);
                        break;
                    case PseudoClass::Kind::FocusVisible:
                        updateInfo(next.FocusVisible, ruleLayout, rulePaint, mayDesc, maySib);
                        break;
                    case PseudoClass::Kind::FocusWithin:
                        updateInfo(next.FocusWithin, ruleLayout, rulePaint, mayDesc, maySib);
                        break;
                    case PseudoClass::Kind::Checked:
                        updateInfo(next.Checked, ruleLayout, rulePaint, mayDesc, maySib);
                        break;
                    case PseudoClass::Kind::Custom:
                        // Aggregate every custom-state rule into one PseudoInfo
                        // (see DynamicStyleAnalysis::CustomState). A layout- or
                        // descendant-affecting `:foo-state` rule makes every
                        // custom-state flip conservatively raise the same bits.
                        updateInfo(next.CustomState, ruleLayout, rulePaint, mayDesc, maySib);
                        break;
                    default:
                        break;
                    }
                };

                applyForPseudoKind(pseudo.PseudoKind);

                for (const auto& nested : pseudo.SelectorList)
                {
                    if (nested)
                        scanChain(*nested, ruleLayout, rulePaint);
                }
            }
        }
    };

    for (const Stylesheet* s : sheets)
    {
        if (!s)
            continue;
        for (const auto& rule : s->Rules)
        {
            bool ruleLayout = false;
            bool rulePaint = false;
            for (const auto& prop : rule.Properties)
            {
                ruleLayout = ruleLayout || propAffectsLayout(prop);
                rulePaint = rulePaint || propAffectsPaint(prop);
            }
            for (const auto& term : rule.Selector.Terms)
            {
                if (term.Comb == SelectorTerm::Combinator::AdjacentSibling ||
                    term.Comb == SelectorTerm::Combinator::GeneralSibling)
                {
                    next.UsesSiblingCombinators = true;
                }
            }
            // Child-list mutations (UIElement::RestyleSiblingsAfterChildListChange)
            // restyle only the siblings these pseudos make position-dependent.
            CollectStructuralPseudos(rule.Selector, next.StructuralPseudoMask, next.UsesEmptyPseudo);
            scanChain(rule.Selector, ruleLayout, rulePaint);
        }
    }

    m_StyleAnalysis = next;
    m_StyleAnalysisGeneration = m_StylesheetContentGeneration;
    m_StyleAnalysisSheetHash = sheetHash;
}
