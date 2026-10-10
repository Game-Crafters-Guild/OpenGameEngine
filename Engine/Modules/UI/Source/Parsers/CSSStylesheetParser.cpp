// CSS stylesheet + inline-style parsing (lexbor-backed). Split out of
// CSSParser.cpp (P4f). Lexbor selector conversion is folded in here because it
// is only ever called from stylesheet parsing and keeps lexbor types out of
// the shared detail header and the hot cascade TU.

#include "UI/Parsers/CSSParser.h"
#include "CSSParserDetail.h"
#include "UI/StyleProperties.h"
#include "UI/UIStyle.h"
#include "UI/Registration/ElementRegistration.h"

#include "Logger/Logger.h"
#include "Types/StringUtils.h"
#include <algorithm>
#include <cctype>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <lexbor/core/serialize.h>
#include <lexbor/css/parser.h>
#include <lexbor/css/property.h>
#include <lexbor/css/rule.h>
#include <lexbor/css/selectors/pseudo_const.h>
#include <lexbor/css/selectors/selector.h>
#include <lexbor/css/selectors/selectors.h>
#include <lexbor/css/stylesheet.h>
#include <lexbor/css/syntax/anb.h>
#include <lexbor/css/syntax/tokenizer.h>
#include <lexbor/css/syntax/tokenizer/error.h>

namespace GameEngine
{
namespace UIParsing
{
using namespace CSSDetail;

namespace
{

// Compute CSS selector specificity from our SelectorChain representation.
// This mirrors the standard (a,b,c) definition:
//   a = number of ID selectors
//   b = number of class selectors, attributes, and pseudo-classes
//   c = number of type selectors and pseudo-elements (we currently have no
//       pseudo-elements in our selector model).
static Specificity ComputeSpecificityFromChain(const SelectorChain& chain)
{
    Specificity sp{};
    for (const SelectorTerm& term : chain.Terms)
    {
        const CompoundSelector& sel = term.Selector;

        if (!sel.Id.empty())
        {
            sp.A += 1;
        }
        if (!sel.Tag.empty() && !sel.Universal)
        {
            sp.C += 1;
        }

        sp.B += static_cast<int>(sel.Classes.size());
        sp.B += static_cast<int>(sel.Attributes.size());

        for (const PseudoClass& pseudo : sel.Pseudos)
        {
            using Kind = PseudoClass::Kind;
            switch (pseudo.PseudoKind)
            {
            case Kind::Where:
                // CSS: :where() always has zero specificity, regardless of its
                // argument selector list.
                break;
            case Kind::Is:
            case Kind::Not:
            {
                // CSS Selectors Level 4:
                // - :is() / :not() have the specificity of their most specific argument.
                // We model the argument selector list explicitly in SelectorList.
                if (pseudo.SelectorList.empty())
                {
                    // Defensive fallback: if we failed to parse the argument list,
                    // treat it like a normal pseudo-class.
                    sp.B += 1;
                    break;
                }

                Specificity best{};
                for (const auto& nested : pseudo.SelectorList)
                {
                    if (!nested)
                        continue;
                    Specificity s = ComputeSpecificityFromChain(*nested);
                    if (s > best)
                        best = s;
                }
                sp.A += best.A;
                sp.B += best.B;
                sp.C += best.C;
                break;
            }
            case Kind::NthChild:
            case Kind::NthLastChild:
            case Kind::NthOfType:
            case Kind::NthLastOfType:
            {
                // CSS: :nth-*(An+B) counts as one pseudo-class.
                // CSS: :nth-*(An+B of S) adds the max specificity of S as well.
                sp.B += 1;
                if (!pseudo.SelectorList.empty())
                {
                    Specificity best{};
                    for (const auto& nested : pseudo.SelectorList)
                    {
                        if (!nested)
                            continue;
                        Specificity s = ComputeSpecificityFromChain(*nested);
                        if (s > best)
                            best = s;
                    }
                    sp.A += best.A;
                    sp.B += best.B;
                    sp.C += best.C;
                }
                break;
            }
            default:
                // All other pseudo-classes, including :root, count as class-level.
                sp.B += 1;
                break;
            }
        }
    }
    return sp;
}

static void ApplyPropertyToOverrides(StyleOverrides& overrides, const StyleProperty& prop)
{
    // Canonical classification (GetStylePropertyImpact): an override-local
    // layout list here had drifted both ways — z-index/overflow relayouted
    // needlessly while line-height, the text-shaping properties, and border
    // widths under-invalidated (no relayout when set as overrides).
    const StyleImpact impact = GetStylePropertyImpact(prop.PropertyId).Layout
                                   ? StyleImpact::Layout
                                   : StyleImpact::Visual;
    if (prop.Keyword != StyleKeyword::None)
    {
        overrides.SetKeywordById(prop.PropertyId, prop.Keyword, impact);
        return;
    }

    switch (prop.PropertyId)
    {
    case StylePropertyId::Display:         overrides.SetById(prop.PropertyId, StyleValue{GetValue<DisplayMode>(prop)}, impact); break;
    case StylePropertyId::FlexDir:         overrides.SetById(prop.PropertyId, StyleValue{GetValue<FlexDirection>(prop)}, impact); break;
    case StylePropertyId::FlexWrap:        overrides.SetById(prop.PropertyId, StyleValue{GetValue<bool>(prop)}, impact); break;
    case StylePropertyId::AlignItems:      overrides.SetById(prop.PropertyId, StyleValue{GetValue<AlignItems>(prop)}, impact); break;
    case StylePropertyId::JustifyContent:  overrides.SetById(prop.PropertyId, StyleValue{GetValue<JustifyContent>(prop)}, impact); break;
    case StylePropertyId::AlignContent:    overrides.SetById(prop.PropertyId, StyleValue{GetValue<AlignContent>(prop)}, impact); break;
    case StylePropertyId::AlignSelf:       overrides.SetById(prop.PropertyId, StyleValue{GetValue<AlignItems>(prop)}, impact); break;
    case StylePropertyId::Direction:       overrides.SetById(prop.PropertyId, StyleValue{GetValue<Direction>(prop)}, impact); break;
    case StylePropertyId::Position:        overrides.SetById(prop.PropertyId, StyleValue{GetValue<PositionType>(prop)}, impact); break;
    case StylePropertyId::PositionLeft:    overrides.SetById(prop.PropertyId, StyleValue{GetValue<StyleLength>(prop)}, impact); break;
    case StylePropertyId::PositionTop:     overrides.SetById(prop.PropertyId, StyleValue{GetValue<StyleLength>(prop)}, impact); break;
    case StylePropertyId::PositionRight:   overrides.SetById(prop.PropertyId, StyleValue{GetValue<StyleLength>(prop)}, impact); break;
    case StylePropertyId::PositionBottom:  overrides.SetById(prop.PropertyId, StyleValue{GetValue<StyleLength>(prop)}, impact); break;
    case StylePropertyId::Order:           overrides.SetById(prop.PropertyId, StyleValue{GetValue<int>(prop)}, impact); break;
    case StylePropertyId::ZIndex:          overrides.SetById(prop.PropertyId, StyleValue{GetValue<int>(prop)}, impact); break;
    case StylePropertyId::FlexGrow:        overrides.SetById(prop.PropertyId, StyleValue{GetValue<float>(prop)}, impact); break;
    case StylePropertyId::FlexShrink:      overrides.SetById(prop.PropertyId, StyleValue{GetValue<float>(prop)}, impact); break;
    case StylePropertyId::FlexBasis:       overrides.SetById(prop.PropertyId, StyleValue{GetValue<StyleLength>(prop)}, impact); break;
    case StylePropertyId::Visibility:      overrides.SetById(prop.PropertyId, StyleValue{GetValue<bool>(prop)}, impact); break;
    case StylePropertyId::PointerEvents:   overrides.SetById(prop.PropertyId, StyleValue{GetValue<bool>(prop)}, impact); break;
    case StylePropertyId::Opacity:         overrides.SetById(prop.PropertyId, StyleValue{GetValue<float>(prop)}, impact); break;
    case StylePropertyId::Overflow:        overrides.SetById(prop.PropertyId, StyleValue{GetValue<Overflow>(prop)}, impact); break;
    case StylePropertyId::OverflowX:       overrides.SetById(prop.PropertyId, StyleValue{GetValue<Overflow>(prop)}, impact); break;
    case StylePropertyId::OverflowY:       overrides.SetById(prop.PropertyId, StyleValue{GetValue<Overflow>(prop)}, impact); break;
    case StylePropertyId::Cursor:          overrides.SetById(prop.PropertyId, StyleValue{GetValue<CursorStyle>(prop)}, impact); break;
    case StylePropertyId::TextAlign:       overrides.SetById(prop.PropertyId, StyleValue{GetValue<TextAlign>(prop)}, impact); break;
    case StylePropertyId::WordBreak:       overrides.SetById(prop.PropertyId, StyleValue{GetValue<WordBreak>(prop)}, impact); break;
    case StylePropertyId::OverflowWrap:    overrides.SetById(prop.PropertyId, StyleValue{GetValue<OverflowWrap>(prop)}, impact); break;
    case StylePropertyId::FontSize:        overrides.SetById(prop.PropertyId, StyleValue{GetValue<StyleLength>(prop)}, impact); break;
    case StylePropertyId::LineHeight:      overrides.SetById(prop.PropertyId, StyleValue{GetValue<float>(prop)}, impact); break;
    case StylePropertyId::LetterSpacing:   overrides.SetById(prop.PropertyId, StyleValue{GetValue<float>(prop)}, impact); break;
    case StylePropertyId::FontFamily:      overrides.SetById(prop.PropertyId, StyleValue{GetValue<std::vector<std::string>>(prop)}, impact); break;
    case StylePropertyId::FontWeight:      overrides.SetById(prop.PropertyId, StyleValue{GetValue<int>(prop)}, impact); break;
    case StylePropertyId::FontStyle:       overrides.SetById(prop.PropertyId, StyleValue{GetValue<FontStyle>(prop)}, impact); break;
    case StylePropertyId::FontVariant:     overrides.SetById(prop.PropertyId, StyleValue{GetValue<FontVariant>(prop)}, impact); break;
    case StylePropertyId::Color:           overrides.SetById(prop.PropertyId, StyleValue{GetValue<uint32_t>(prop)}, impact); break;
    case StylePropertyId::BackgroundColor: overrides.SetById(prop.PropertyId, StyleValue{GetValue<uint32_t>(prop)}, impact); break;
    case StylePropertyId::BackgroundImage: overrides.SetById(prop.PropertyId, StyleValue{GetValue<BackgroundImageSource>(prop)}, impact); break;
    case StylePropertyId::BackgroundSize:  overrides.SetById(prop.PropertyId, StyleValue{GetValue<BackgroundSizeValue>(prop)}, impact); break;
    case StylePropertyId::BorderImageSlice:  overrides.SetById(prop.PropertyId, StyleValue{GetValue<BorderImageSliceValue>(prop)}, impact); break;
    case StylePropertyId::BorderImageRepeat: overrides.SetById(prop.PropertyId, StyleValue{GetValue<BorderImageRepeatValue>(prop)}, impact); break;
    case StylePropertyId::BackgroundPosition: overrides.SetById(prop.PropertyId, StyleValue{GetValue<BackgroundPositionValue>(prop)}, impact); break;
    case StylePropertyId::BackgroundRepeat: overrides.SetById(prop.PropertyId, StyleValue{GetValue<BackgroundRepeat>(prop)}, impact); break;
    case StylePropertyId::BackgroundTint:  overrides.SetById(prop.PropertyId, StyleValue{GetValue<uint32_t>(prop)}, impact); break;
    case StylePropertyId::BackgroundImageSaturation: overrides.SetById(prop.PropertyId, StyleValue{GetValue<float>(prop)}, impact); break;
    case StylePropertyId::BorderColor:     overrides.SetById(prop.PropertyId, StyleValue{GetValue<BorderColorsTRBL>(prop)}, impact); break;
    case StylePropertyId::BorderTopColor:  overrides.SetById(prop.PropertyId, StyleValue{GetValue<uint32_t>(prop)}, impact); break;
    case StylePropertyId::BorderRightColor: overrides.SetById(prop.PropertyId, StyleValue{GetValue<uint32_t>(prop)}, impact); break;
    case StylePropertyId::BorderBottomColor: overrides.SetById(prop.PropertyId, StyleValue{GetValue<uint32_t>(prop)}, impact); break;
    case StylePropertyId::BorderLeftColor: overrides.SetById(prop.PropertyId, StyleValue{GetValue<uint32_t>(prop)}, impact); break;
    case StylePropertyId::BorderWidth:     overrides.SetById(prop.PropertyId, StyleValue{GetValue<Box4>(prop)}, impact); break;
    case StylePropertyId::BorderTopWidth:  overrides.SetById(prop.PropertyId, StyleValue{GetValue<float>(prop)}, impact); break;
    case StylePropertyId::BorderRightWidth: overrides.SetById(prop.PropertyId, StyleValue{GetValue<float>(prop)}, impact); break;
    case StylePropertyId::BorderBottomWidth: overrides.SetById(prop.PropertyId, StyleValue{GetValue<float>(prop)}, impact); break;
    case StylePropertyId::BorderLeftWidth: overrides.SetById(prop.PropertyId, StyleValue{GetValue<float>(prop)}, impact); break;
    case StylePropertyId::BorderStyle:     overrides.SetById(prop.PropertyId, StyleValue{GetValue<BorderStyle>(prop)}, impact); break;
    case StylePropertyId::OutlineWidth:    overrides.SetById(prop.PropertyId, StyleValue{GetValue<float>(prop)}, impact); break;
    case StylePropertyId::OutlineStyle:    overrides.SetById(prop.PropertyId, StyleValue{GetValue<BorderStyle>(prop)}, impact); break;
    case StylePropertyId::OutlineColor:    overrides.SetById(prop.PropertyId, StyleValue{GetValue<uint32_t>(prop)}, impact); break;
    case StylePropertyId::OutlineOffset:   overrides.SetById(prop.PropertyId, StyleValue{GetValue<float>(prop)}, impact); break;
    case StylePropertyId::BorderRadius:    overrides.SetById(prop.PropertyId, StyleValue{GetValue<CornerRadiiTLTRBRBL>(prop)}, impact); break;
    case StylePropertyId::BorderTopLeftRadius: overrides.SetById(prop.PropertyId, StyleValue{GetValue<CornerRadiusValue>(prop)}, impact); break;
    case StylePropertyId::BorderTopRightRadius: overrides.SetById(prop.PropertyId, StyleValue{GetValue<CornerRadiusValue>(prop)}, impact); break;
    case StylePropertyId::BorderBottomRightRadius: overrides.SetById(prop.PropertyId, StyleValue{GetValue<CornerRadiusValue>(prop)}, impact); break;
    case StylePropertyId::BorderBottomLeftRadius: overrides.SetById(prop.PropertyId, StyleValue{GetValue<CornerRadiusValue>(prop)}, impact); break;
    case StylePropertyId::Margin:          overrides.SetById(prop.PropertyId, StyleValue{GetValue<StyleBox>(prop)}, impact); break;
    case StylePropertyId::MarginTop:       overrides.SetById(prop.PropertyId, StyleValue{GetValue<StyleLength>(prop)}, impact); break;
    case StylePropertyId::MarginRight:     overrides.SetById(prop.PropertyId, StyleValue{GetValue<StyleLength>(prop)}, impact); break;
    case StylePropertyId::MarginBottom:    overrides.SetById(prop.PropertyId, StyleValue{GetValue<StyleLength>(prop)}, impact); break;
    case StylePropertyId::MarginLeft:      overrides.SetById(prop.PropertyId, StyleValue{GetValue<StyleLength>(prop)}, impact); break;
    case StylePropertyId::Padding:         overrides.SetById(prop.PropertyId, StyleValue{GetValue<StyleBox>(prop)}, impact); break;
    case StylePropertyId::PaddingTop:      overrides.SetById(prop.PropertyId, StyleValue{GetValue<StyleLength>(prop)}, impact); break;
    case StylePropertyId::PaddingRight:    overrides.SetById(prop.PropertyId, StyleValue{GetValue<StyleLength>(prop)}, impact); break;
    case StylePropertyId::PaddingBottom:   overrides.SetById(prop.PropertyId, StyleValue{GetValue<StyleLength>(prop)}, impact); break;
    case StylePropertyId::PaddingLeft:     overrides.SetById(prop.PropertyId, StyleValue{GetValue<StyleLength>(prop)}, impact); break;
    case StylePropertyId::Gap:             overrides.SetById(prop.PropertyId, StyleValue{GetValue<StyleLength>(prop)}, impact); break;
    case StylePropertyId::RowGap:          overrides.SetById(prop.PropertyId, StyleValue{GetValue<StyleLength>(prop)}, impact); break;
    case StylePropertyId::ColumnGap:       overrides.SetById(prop.PropertyId, StyleValue{GetValue<StyleLength>(prop)}, impact); break;
    case StylePropertyId::Width:           overrides.SetById(prop.PropertyId, StyleValue{GetValue<StyleLength>(prop)}, impact); break;
    case StylePropertyId::Height:          overrides.SetById(prop.PropertyId, StyleValue{GetValue<StyleLength>(prop)}, impact); break;
    case StylePropertyId::MinWidth:        overrides.SetById(prop.PropertyId, StyleValue{GetValue<StyleLength>(prop)}, impact); break;
    case StylePropertyId::MinHeight:       overrides.SetById(prop.PropertyId, StyleValue{GetValue<StyleLength>(prop)}, impact); break;
    case StylePropertyId::MaxWidth:        overrides.SetById(prop.PropertyId, StyleValue{GetValue<StyleLength>(prop)}, impact); break;
    case StylePropertyId::MaxHeight:       overrides.SetById(prop.PropertyId, StyleValue{GetValue<StyleLength>(prop)}, impact); break;
    case StylePropertyId::AspectRatio:     overrides.SetById(prop.PropertyId, StyleValue{GetValue<float>(prop)}, impact); break;
    case StylePropertyId::BoxShadow:     overrides.SetById(prop.PropertyId, StyleValue{GetValue<BoxShadowValue>(prop)}, impact); break;
    case StylePropertyId::TextShadowOffsetX: overrides.SetById(prop.PropertyId, StyleValue{GetValue<float>(prop)}, impact); break;
    case StylePropertyId::TextShadowOffsetY: overrides.SetById(prop.PropertyId, StyleValue{GetValue<float>(prop)}, impact); break;
    case StylePropertyId::TextShadowBlur: overrides.SetById(prop.PropertyId, StyleValue{GetValue<float>(prop)}, impact); break;
    case StylePropertyId::TextShadowColor: overrides.SetById(prop.PropertyId, StyleValue{GetValue<uint32_t>(prop)}, impact); break;
    case StylePropertyId::TextGlowRadius: overrides.SetById(prop.PropertyId, StyleValue{GetValue<float>(prop)}, impact); break;
    case StylePropertyId::TextGlowColor: overrides.SetById(prop.PropertyId, StyleValue{GetValue<uint32_t>(prop)}, impact); break;
    case StylePropertyId::TextOutlineWidth: overrides.SetById(prop.PropertyId, StyleValue{GetValue<float>(prop)}, impact); break;
    case StylePropertyId::TextOutlineColor: overrides.SetById(prop.PropertyId, StyleValue{GetValue<uint32_t>(prop)}, impact); break;
    case StylePropertyId::SelectionColor: overrides.SetById(prop.PropertyId, StyleValue{GetValue<uint32_t>(prop)}, impact); break;
    case StylePropertyId::Glow:          overrides.SetById(prop.PropertyId, StyleValue{GetValue<GlowValue>(prop)}, impact); break;
    case StylePropertyId::Transition:
    case StylePropertyId::CustomVar:
    case StylePropertyId::DeferredDecl:
    default:
        break;
    }
}

// Lexbor parsing functions
static lxb_status_t AppendToString(const lxb_char_t* data, size_t length, void* ctx)
{
    auto* out = static_cast<std::string*>(ctx);
    out->append(reinterpret_cast<const char*>(data), length);
    return LXB_STATUS_OK;
}

static std::string LexborString(const lexbor_str_t& str)
{
    if (str.data == nullptr || str.length == 0)
    {
        return {};
    }
    return std::string(reinterpret_cast<const char*>(str.data), str.length);
}

static SelectorTerm::Combinator ConvertCombinator(lxb_css_selector_combinator_t comb)
{
    switch (comb)
    {
    case LXB_CSS_SELECTOR_COMBINATOR_DESCENDANT:
        return SelectorTerm::Combinator::Descendant;
    case LXB_CSS_SELECTOR_COMBINATOR_CHILD:
        return SelectorTerm::Combinator::Child;
    case LXB_CSS_SELECTOR_COMBINATOR_SIBLING:
        return SelectorTerm::Combinator::AdjacentSibling;
    case LXB_CSS_SELECTOR_COMBINATOR_FOLLOWING:
        return SelectorTerm::Combinator::GeneralSibling;
    default:
        return SelectorTerm::Combinator::None;
    }
}

static AttributeSelector::MatchType ConvertMatchType(lxb_css_selector_match_t match)
{
    switch (match)
    {
    case LXB_CSS_SELECTOR_MATCH_EQUAL:
        return AttributeSelector::MatchType::Equals;
    case LXB_CSS_SELECTOR_MATCH_INCLUDE:
        return AttributeSelector::MatchType::Includes;
    case LXB_CSS_SELECTOR_MATCH_DASH:
        return AttributeSelector::MatchType::DashMatch;
    case LXB_CSS_SELECTOR_MATCH_PREFIX:
        return AttributeSelector::MatchType::Prefix;
    case LXB_CSS_SELECTOR_MATCH_SUFFIX:
        return AttributeSelector::MatchType::Suffix;
    case LXB_CSS_SELECTOR_MATCH_SUBSTRING:
        return AttributeSelector::MatchType::Substring;
    default:
        return AttributeSelector::MatchType::Exists;
    }
}

static bool ConvertSelectorList(const lxb_css_selector_list_t* list, SelectorChain& outChain);
static void ConvertSelectorListCollection(const lxb_css_selector_list_t* list, std::vector<std::shared_ptr<SelectorChain>>& outList);

static bool ConvertPseudoSimple(const lxb_css_selector_t& node, PseudoClass& outPseudo)
{
    auto pseudoId = static_cast<lxb_css_selector_pseudo_class_id_t>(node.u.pseudo.type);
    switch (pseudoId)
    {
    case LXB_CSS_SELECTOR_PSEUDO_CLASS_HOVER:
        outPseudo.PseudoKind = PseudoClass::Kind::Hover;
        return true;
    case LXB_CSS_SELECTOR_PSEUDO_CLASS_ACTIVE:
        outPseudo.PseudoKind = PseudoClass::Kind::Active;
        return true;
    case LXB_CSS_SELECTOR_PSEUDO_CLASS_FOCUS:
        outPseudo.PseudoKind = PseudoClass::Kind::Focus;
        return true;
    case LXB_CSS_SELECTOR_PSEUDO_CLASS_FOCUS_VISIBLE:
        outPseudo.PseudoKind = PseudoClass::Kind::FocusVisible;
        return true;
    case LXB_CSS_SELECTOR_PSEUDO_CLASS_FOCUS_WITHIN:
        outPseudo.PseudoKind = PseudoClass::Kind::FocusWithin;
        return true;
    case LXB_CSS_SELECTOR_PSEUDO_CLASS_DISABLED:
        outPseudo.PseudoKind = PseudoClass::Kind::Disabled;
        return true;
    case LXB_CSS_SELECTOR_PSEUDO_CLASS_ENABLED:
        outPseudo.PseudoKind = PseudoClass::Kind::Enabled;
        return true;
    case LXB_CSS_SELECTOR_PSEUDO_CLASS_CHECKED:
        outPseudo.PseudoKind = PseudoClass::Kind::Checked;
        return true;
    case LXB_CSS_SELECTOR_PSEUDO_CLASS_ROOT:
        outPseudo.PseudoKind = PseudoClass::Kind::Root;
        return true;
    case LXB_CSS_SELECTOR_PSEUDO_CLASS_EMPTY:
        outPseudo.PseudoKind = PseudoClass::Kind::Empty;
        return true;
    case LXB_CSS_SELECTOR_PSEUDO_CLASS_FIRST_CHILD:
        outPseudo.PseudoKind = PseudoClass::Kind::FirstChild;
        return true;
    case LXB_CSS_SELECTOR_PSEUDO_CLASS_LAST_CHILD:
        outPseudo.PseudoKind = PseudoClass::Kind::LastChild;
        return true;
    case LXB_CSS_SELECTOR_PSEUDO_CLASS_ONLY_CHILD:
        outPseudo.PseudoKind = PseudoClass::Kind::OnlyChild;
        return true;
    case LXB_CSS_SELECTOR_PSEUDO_CLASS_FIRST_OF_TYPE:
        outPseudo.PseudoKind = PseudoClass::Kind::FirstOfType;
        return true;
    case LXB_CSS_SELECTOR_PSEUDO_CLASS_LAST_OF_TYPE:
        outPseudo.PseudoKind = PseudoClass::Kind::LastOfType;
        return true;
    case LXB_CSS_SELECTOR_PSEUDO_CLASS_ONLY_OF_TYPE:
        outPseudo.PseudoKind = PseudoClass::Kind::OnlyOfType;
        return true;
    default:
        outPseudo.PseudoKind = PseudoClass::Kind::Custom;
        outPseudo.CustomName = ToLowerAscii(LexborString(node.name));
        outPseudo.ResolvedCustomNameId = HashStringId(outPseudo.CustomName);
        return true;
    }
}

static bool ConvertPseudoFunction(const lxb_css_selector_t& node, PseudoClass& outPseudo)
{
    auto funcId = static_cast<lxb_css_selector_pseudo_class_function_id_t>(node.u.pseudo.type);
    switch (funcId)
    {
    case LXB_CSS_SELECTOR_PSEUDO_CLASS_FUNCTION_IS:
        outPseudo.PseudoKind = PseudoClass::Kind::Is;
        ConvertSelectorListCollection(static_cast<lxb_css_selector_list_t*>(node.u.pseudo.data), outPseudo.SelectorList);
        return true;
    case LXB_CSS_SELECTOR_PSEUDO_CLASS_FUNCTION_NOT:
        outPseudo.PseudoKind = PseudoClass::Kind::Not;
        ConvertSelectorListCollection(static_cast<lxb_css_selector_list_t*>(node.u.pseudo.data), outPseudo.SelectorList);
        return true;
    case LXB_CSS_SELECTOR_PSEUDO_CLASS_FUNCTION_WHERE:
        outPseudo.PseudoKind = PseudoClass::Kind::Where;
        ConvertSelectorListCollection(static_cast<lxb_css_selector_list_t*>(node.u.pseudo.data), outPseudo.SelectorList);
        return true;
    case LXB_CSS_SELECTOR_PSEUDO_CLASS_FUNCTION_NTH_CHILD:
    case LXB_CSS_SELECTOR_PSEUDO_CLASS_FUNCTION_NTH_LAST_CHILD:
    case LXB_CSS_SELECTOR_PSEUDO_CLASS_FUNCTION_NTH_OF_TYPE:
    case LXB_CSS_SELECTOR_PSEUDO_CLASS_FUNCTION_NTH_LAST_OF_TYPE:
    {
        auto* data = static_cast<lxb_css_selector_anb_of_t*>(node.u.pseudo.data);
        if (data)
        {
            outPseudo.Nth.A = static_cast<int>(data->anb.a);
            outPseudo.Nth.B = static_cast<int>(data->anb.b);
            ConvertSelectorListCollection(data->of, outPseudo.SelectorList);
        }
        switch (funcId)
        {
        case LXB_CSS_SELECTOR_PSEUDO_CLASS_FUNCTION_NTH_CHILD:
            outPseudo.PseudoKind = PseudoClass::Kind::NthChild;
            outPseudo.Nth.OfType = false;
            outPseudo.Nth.FromEnd = false;
            break;
        case LXB_CSS_SELECTOR_PSEUDO_CLASS_FUNCTION_NTH_LAST_CHILD:
            outPseudo.PseudoKind = PseudoClass::Kind::NthLastChild;
            outPseudo.Nth.OfType = false;
            outPseudo.Nth.FromEnd = true;
            break;
        case LXB_CSS_SELECTOR_PSEUDO_CLASS_FUNCTION_NTH_OF_TYPE:
            outPseudo.PseudoKind = PseudoClass::Kind::NthOfType;
            outPseudo.Nth.OfType = true;
            outPseudo.Nth.FromEnd = false;
            break;
        case LXB_CSS_SELECTOR_PSEUDO_CLASS_FUNCTION_NTH_LAST_OF_TYPE:
            outPseudo.PseudoKind = PseudoClass::Kind::NthLastOfType;
            outPseudo.Nth.OfType = true;
            outPseudo.Nth.FromEnd = true;
            break;
        default:
            break;
        }
        return true;
    }
    default:
        outPseudo.PseudoKind = PseudoClass::Kind::Custom;
        outPseudo.CustomName = ToLowerAscii(LexborString(node.name));
        outPseudo.ResolvedCustomNameId = HashStringId(outPseudo.CustomName);
        return true;
    }
}

static bool AppendSimpleSelector(const lxb_css_selector_t& node, CompoundSelector& compound)
{
    switch (node.type)
    {
    case LXB_CSS_SELECTOR_TYPE_ANY:
        compound.Universal = true;
        return true;
    case LXB_CSS_SELECTOR_TYPE_ELEMENT:
        if (node.name.length > 0)
        {
            compound.Tag = LexborString(node.name);
            compound.ResolvedTagId = UIRegistration::ElementFactoryRegistry::Instance().GetTagId(ToLowerAscii(compound.Tag));
        }
        else
        {
            compound.Universal = true;
        }
        return true;
    case LXB_CSS_SELECTOR_TYPE_ID:
        compound.Id = LexborString(node.name);
        compound.ResolvedId = HashStringId(compound.Id);
        return true;
    case LXB_CSS_SELECTOR_TYPE_CLASS:
    {
        std::string clsName = LexborString(node.name);
        compound.ResolvedClassIds.push_back(HashStringId(clsName));
        compound.Classes.push_back(std::move(clsName));
        return true;
    }
    case LXB_CSS_SELECTOR_TYPE_ATTRIBUTE:
    {
        AttributeSelector attr{};
        attr.Name = LexborString(node.name);
        attr.Value = LexborString(node.u.attribute.value);
        attr.Match = ConvertMatchType(node.u.attribute.match);
        attr.CaseInsensitive = (node.u.attribute.modifier == LXB_CSS_SELECTOR_MODIFIER_I);
        compound.Attributes.push_back(std::move(attr));
        return true;
    }
    case LXB_CSS_SELECTOR_TYPE_PSEUDO_CLASS:
    {
        PseudoClass pseudo{};
        ConvertPseudoSimple(node, pseudo);
        compound.Pseudos.push_back(std::move(pseudo));
        return true;
    }
    case LXB_CSS_SELECTOR_TYPE_PSEUDO_CLASS_FUNCTION:
    {
        PseudoClass pseudo{};
        ConvertPseudoFunction(node, pseudo);
        compound.Pseudos.push_back(std::move(pseudo));
        return true;
    }
    case LXB_CSS_SELECTOR_TYPE_PSEUDO_ELEMENT:
    case LXB_CSS_SELECTOR_TYPE_PSEUDO_ELEMENT_FUNCTION:
    {
        PseudoClass pseudo{};
        pseudo.PseudoKind = PseudoClass::Kind::Custom;
        pseudo.CustomName = ToLowerAscii(LexborString(node.name));
        pseudo.ResolvedCustomNameId = HashStringId(pseudo.CustomName);
        compound.Pseudos.push_back(std::move(pseudo));
        return true;
    }
    default:
        return false;
    }
}

static void ConvertSelectorListCollection(const lxb_css_selector_list_t* list,
                                          std::vector<std::shared_ptr<SelectorChain>>& outList)
{
    for (auto* item = list; item != nullptr; item = item->next)
    {
        auto chain = std::make_shared<SelectorChain>();
        if (ConvertSelectorList(item, *chain))
        {
            outList.push_back(std::move(chain));
        }
    }
}

static bool ConvertSelectorList(const lxb_css_selector_list_t* list, SelectorChain& outChain)
{
    if (list == nullptr)
    {
        return false;
    }

    outChain.Terms.clear();
    outChain.Spec = Specificity{};

    SelectorTerm current{};
    bool hasCurrent = false;

    for (const lxb_css_selector_t* node = list->first; node != nullptr; node = node->next)
    {
        bool structural = (node->combinator == LXB_CSS_SELECTOR_COMBINATOR_DESCENDANT ||
                           node->combinator == LXB_CSS_SELECTOR_COMBINATOR_CHILD ||
                           node->combinator == LXB_CSS_SELECTOR_COMBINATOR_SIBLING ||
                           node->combinator == LXB_CSS_SELECTOR_COMBINATOR_FOLLOWING);

        if (!hasCurrent)
        {
            current = SelectorTerm{};
            current.Comb = SelectorTerm::Combinator::None;
            hasCurrent = true;
        }
        else if (structural)
        {
            outChain.Terms.push_back(current);
            current = SelectorTerm{};
            current.Comb = ConvertCombinator(node->combinator);
        }

        AppendSimpleSelector(*node, current.Selector);
    }

    if (hasCurrent)
    {
        outChain.Terms.push_back(current);
    }

    if (!outChain.Terms.empty())
    {
        outChain.Terms.front().Comb = SelectorTerm::Combinator::None;
        outChain.Spec = ComputeSpecificityFromChain(outChain);
    }

    return !outChain.Terms.empty();
}

// Detect and strip a trailing `!important` from a CSS declaration value.
// Per CSS Syntax 3, the marker is matched case-insensitively and may have
// whitespace between `!` and `important`. Returns true if `!important` was
// stripped from `value` (in which case the caller should set the declaration's
// `important` flag).
static bool StripImportantSuffix(std::string& value)
{
    // Find the last '!' that isn't inside a function/string. The common case
    // (no functions wrapping the marker) is simple: scan from the right past
    // the trailing `important` token.
    size_t end = value.size();
    while (end > 0 && std::isspace(static_cast<unsigned char>(value[end - 1])))
        --end;
    if (end < 9) // "important" is 9 chars; need at least "!important"
        return false;

    // Match the identifier "important" case-insensitively at the end.
    static const char kTok[] = "important";
    constexpr size_t kTokLen = sizeof(kTok) - 1;
    if (end < kTokLen)
        return false;
    for (size_t i = 0; i < kTokLen; ++i)
    {
        if (std::tolower(static_cast<unsigned char>(value[end - kTokLen + i])) != kTok[i])
            return false;
    }

    // Walk back past optional whitespace to find the '!'.
    size_t bang = end - kTokLen;
    while (bang > 0 && std::isspace(static_cast<unsigned char>(value[bang - 1])))
        --bang;
    if (bang == 0 || value[bang - 1] != '!')
        return false;

    // Strip everything from the '!' onward.
    value.erase(bang - 1);
    // Trim trailing whitespace that preceded the '!'.
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
        value.pop_back();
    return true;
}

// Records `name` as unregistered, once per sheet. Runs at sheet parse time —
// once per load or hot-reload, never per element — so the linear scan over a
// list that is empty for a clean sheet costs nothing measurable.
static void NoteUnknownProperty(const std::string& name, std::vector<std::string>& outUnknown)
{
    if (std::find(outUnknown.begin(), outUnknown.end(), name) == outUnknown.end())
        outUnknown.push_back(name);
}

static void CollectProperties(const lxb_css_rule_declaration_list_t* list,
                              std::vector<StyleProperty>& outProps,
                              std::vector<std::string>& outUnknown)
{
    if (list == nullptr)
        return;

    for (const lxb_css_rule_t* entry = list->first; entry != nullptr; entry = entry->next)
    {
        if (entry->type != LXB_CSS_RULE_DECLARATION)
            continue;
        const auto* decl = lxb_css_rule_declaration(entry);
        std::string serialized;
        if (lxb_css_rule_declaration_serialize(decl, AppendToString, &serialized) != LXB_STATUS_OK)
        {
            continue;
        }
        size_t colon = serialized.find(':');
        if (colon == std::string::npos)
            continue;
        std::string name = Trim(serialized.substr(0, colon));
        std::string value = Trim(serialized.substr(colon + 1));
        if (!value.empty() && value.back() == ';')
        {
            value.pop_back();
            value = Trim(value);
        }
        if (name.empty() || value.empty())
            continue;

        // Prefer lexbor's parsed flag (authoritative — set whenever the
        // declaration ends with `! important`). Some serializations also
        // include the literal `!important` in the value text, so strip it
        // either way to keep the value parsable.
        const bool importantFromLexbor = decl->important;
        const bool importantFromSuffix = StripImportantSuffix(value);
        const bool important = importantFromLexbor || importantFromSuffix;
        if (value.empty())
            continue;

        // Both branches below take the name on trust — the var() branch defers
        // by name without ever looking it up — so the registration check has to
        // sit above them or every var()-holding typo escapes it. Custom
        // properties are declarations too and are never "unknown".
        const std::string loweredName = ToLowerAscii(name);
        if (!(loweredName.size() >= 2 && loweredName[0] == '-' && loweredName[1] == '-') &&
            !CSSParser::IsKnownPropertyName(loweredName))
        {
            NoteUnknownProperty(loweredName, outUnknown);
        }

        const size_t startSize = outProps.size();

        // Defer parsing for declarations that contain var() so we can substitute
        // variables at compute time (including inside shorthands).
        if (ContainsVarCall(value) && !(name.size() >= 2 && name[0] == '-' && name[1] == '-'))
        {
            StyleProperty p{};
            p.PropertyId = StylePropertyId::DeferredDecl;
            p.Value = DeferredDeclValue{ToLowerAscii(name), value};
            p.Important = important;
            outProps.push_back(std::move(p));
        }
        else
        {
            AppendDeclarationProperties(name, value, outProps);
            if (important)
            {
                for (size_t i = startSize; i < outProps.size(); ++i)
                    outProps[i].Important = true;
            }
        }
    }
}

// First bytes of a stylesheet, for diagnostics that need to identify the text
// without a source name.
static std::string PreviewOf(std::string_view cssText)
{
    constexpr size_t kPreviewBytes = 120;
    const size_t length = std::min<size_t>(cssText.size(), kPreviewBytes);
    std::string preview(cssText.substr(0, length));
    if (length < cssText.size())
        preview += "...";
    return preview;
}

// Describe the construct a stylesheet ends inside, or return an empty string
// when it ends cleanly.
//
// A CSS tokenizer closes an open string, comment, block or function at EOF and
// the syntax parser recovers around the truncation, so half a stylesheet parses
// into a smaller, perfectly valid one. Nothing downstream can tell that apart
// from an author deleting rules, so truncation is decided here, from the token
// stream, before any rule reaches `outSheet`.
//
// Only truncation. Content that is malformed but all there keeps the CSS error
// recovery a browser gives it: the unreadable declaration or rule is dropped
// and the rest of the file applies.
static std::string DescribeIncompleteStylesheet(std::string_view cssText)
{
    struct OpenConstruct
    {
        lxb_css_syntax_token_type_t Closer;
        const char* Description;
    };

    std::unique_ptr<lxb_css_syntax_tokenizer_t, decltype(&lxb_css_syntax_tokenizer_destroy)> tokenizer(
        lxb_css_syntax_tokenizer_create(), &lxb_css_syntax_tokenizer_destroy);
    if (!tokenizer || lxb_css_syntax_tokenizer_init(tokenizer.get()) != LXB_STATUS_OK)
        return "could not be tokenized";

    lxb_css_syntax_tokenizer_buffer_set(
        tokenizer.get(), reinterpret_cast<const lxb_char_t*>(cssText.data()), cssText.size());

    std::vector<OpenConstruct> open;
    // A construct at the top level that no `;` terminated and no `{ }` block
    // completed — a selector the writer had not finished typing.
    bool unfinishedRule = false;

    for (;;)
    {
        const lxb_css_syntax_token_t* token = lxb_css_syntax_token(tokenizer.get());
        if (!token)
            return "could not be tokenized";

        if (token->type == LXB_CSS_SYNTAX_TOKEN__EOF)
        {
            if (!open.empty())
                return open.back().Description;
            if (unfinishedRule)
                return "ends with an unfinished rule";

            // Truncation inside a string, comment, url() or escape leaves no
            // unbalanced token behind; the tokenizer records it instead. Only
            // the end-of-file errors mean "incomplete" — a newline in a string
            // or a bad code point is malformed content the parser handles.
            const size_t errorCount = lexbor_array_obj_length(tokenizer->parse_errors);
            for (size_t i = 0; i < errorCount; ++i)
            {
                const auto* error = static_cast<const lxb_css_syntax_tokenizer_error_t*>(
                    lexbor_array_obj_get(tokenizer->parse_errors, i));
                if (!error)
                    continue;
                switch (error->id)
                {
                case LXB_CSS_SYNTAX_TOKENIZER_ERROR_EOINCO:
                    return "ends inside an unterminated comment";
                case LXB_CSS_SYNTAX_TOKENIZER_ERROR_EOINST:
                    return "ends inside an unterminated string";
                case LXB_CSS_SYNTAX_TOKENIZER_ERROR_EOINUR:
                    return "ends inside an unterminated url()";
                case LXB_CSS_SYNTAX_TOKENIZER_ERROR_EOINES:
                    return "ends with an unfinished escape sequence";
                case LXB_CSS_SYNTAX_TOKENIZER_ERROR_UNEOF:
                    return "ends unexpectedly";
                default:
                    break;
                }
            }
            return {};
        }

        switch (token->type)
        {
        case LXB_CSS_SYNTAX_TOKEN_WHITESPACE:
        case LXB_CSS_SYNTAX_TOKEN_COMMENT:
        case LXB_CSS_SYNTAX_TOKEN_CDO:
        case LXB_CSS_SYNTAX_TOKEN_CDC:
            break;

        case LXB_CSS_SYNTAX_TOKEN_LC_BRACKET:
            open.push_back({LXB_CSS_SYNTAX_TOKEN_RC_BRACKET, "ends inside an unclosed '{' block"});
            break;
        case LXB_CSS_SYNTAX_TOKEN_LS_BRACKET:
            open.push_back({LXB_CSS_SYNTAX_TOKEN_RS_BRACKET, "ends inside an unclosed '['"});
            break;
        case LXB_CSS_SYNTAX_TOKEN_L_PARENTHESIS:
        case LXB_CSS_SYNTAX_TOKEN_FUNCTION:
            open.push_back({LXB_CSS_SYNTAX_TOKEN_R_PARENTHESIS, "ends inside an unclosed '('"});
            break;

        case LXB_CSS_SYNTAX_TOKEN_RC_BRACKET:
        case LXB_CSS_SYNTAX_TOKEN_RS_BRACKET:
        case LXB_CSS_SYNTAX_TOKEN_R_PARENTHESIS:
            // A closer with nothing to close is malformed, not incomplete: the
            // file is all there and CSS error recovery drops what it cannot
            // read, exactly as a browser does. Only truncation is rejected here.
            if (open.empty() || open.back().Closer != token->type)
                break;
            open.pop_back();
            if (open.empty() && token->type == LXB_CSS_SYNTAX_TOKEN_RC_BRACKET)
                unfinishedRule = false;
            break;

        case LXB_CSS_SYNTAX_TOKEN_SEMICOLON:
            if (open.empty())
                unfinishedRule = false;
            break;

        default:
            unfinishedRule = true;
            break;
        }

        lxb_css_syntax_token_consume(tokenizer.get());
    }
}

// A UTF-8 byte-order mark is an encoding mark, not content: the CSS tokenizer
// reads U+FEFF as an identifier, which reads as an unfinished rule in a file
// with no rules and joins the first selector in a file that has them.
constexpr std::string_view kUtf8Bom = "\xEF\xBB\xBF";

static bool ParseStylesLexbor(const std::string& cssText, Stylesheet& outSheet)
{
    outSheet.Rules.clear();
    outSheet.UnknownProperties.clear();

    std::string_view css(cssText);
    if (css.starts_with(kUtf8Bom))
        css.remove_prefix(kUtf8Bom.size());

    if (const std::string incomplete = DescribeIncompleteStylesheet(css); !incomplete.empty())
    {
        Logger::Log::Error(
            "UI CSS: the stylesheet {} — none of its rules were applied. Save a complete, "
            "balanced stylesheet (bytes={}, preview='{}')",
            incomplete, (uint32_t)css.size(), PreviewOf(css));
        return false;
    }

    lxb_css_parser_t* parser = lxb_css_parser_create();
    if (!parser)
    {
        return false;
    }

    lxb_status_t status = lxb_css_parser_init(parser, nullptr);
    if (status != LXB_STATUS_OK)
    {
        lxb_css_parser_destroy(parser, true);
        return false;
    }

    // Use the stylesheet parse API instead of process, which hangs in some builds.
#if defined(LEXBOR_VERSION_MAJOR) && LEXBOR_VERSION_MAJOR >= 3
    lxb_css_stylesheet_t* sheet = lxb_css_stylesheet_create(nullptr);
    if (sheet != nullptr)
    {
        status = lxb_css_stylesheet_parse(
            sheet, parser, reinterpret_cast<const lxb_char_t*>(css.data()), css.size());
    }
    if (sheet == nullptr || status != LXB_STATUS_OK || sheet->root == nullptr)
#else
    lxb_css_stylesheet_t* sheet = lxb_css_stylesheet_parse(
        parser, reinterpret_cast<const lxb_char_t*>(css.data()), css.size());
    if (sheet == nullptr || sheet->root == nullptr)
#endif
    {
        // Provide a concise diagnostic to help triage CSS parse failures
        Logger::Log::Warning("UI: Lexbor stylesheet parse returned null (bytes={}, preview='{}')",
                             (uint32_t)css.size(), PreviewOf(css));
        if (sheet)
        {
            lxb_css_stylesheet_destroy(sheet, true);
        }
#if !defined(LEXBOR_VERSION_MAJOR) || LEXBOR_VERSION_MAJOR < 3
        lxb_css_stylesheet_finish(parser);
#endif
        lxb_css_parser_destroy(parser, true);
        return false;
    }

    int order = 0;
    const lxb_css_rule_list_t* rootList = lxb_css_rule_list(sheet->root);
    for (const lxb_css_rule_t* rule = rootList->first; rule != nullptr; rule = rule->next)
    {
        if (rule->type != LXB_CSS_RULE_STYLE)
            continue;
        const auto* style = lxb_css_rule_style(rule);
        if (style->selector == nullptr || style->declarations == nullptr)
            continue;

        std::vector<StyleProperty> properties;
        CollectProperties(style->declarations, properties, outSheet.UnknownProperties);

        for (const lxb_css_selector_list_t* sel = style->selector; sel != nullptr; sel = sel->next)
        {
            SelectorChain chain;
            if (!ConvertSelectorList(sel, chain))
                continue;

            CSSRule cssRule{};
            cssRule.Selector = chain;
            cssRule.Properties = properties;
            cssRule.Order = order++;
            cssRule.Selector.Spec = chain.Spec;
            outSheet.Rules.push_back(std::move(cssRule));
        }
    }

    lxb_css_stylesheet_destroy(sheet, true);
#if !defined(LEXBOR_VERSION_MAJOR) || LEXBOR_VERSION_MAJOR < 3
    lxb_css_stylesheet_finish(parser);
#endif
    lxb_css_parser_destroy(parser, true);
    return true;
}

} // namespace

bool CSSParser::ParseStylesFromString(const std::string& cssText, Stylesheet& outSheet)
{
    // Keep logs quiet by default
    bool ok = ParseStylesLexbor(cssText, outSheet);
    return ok;
}

// CSS properties this engine has decided not to implement. They are still
// recorded in Stylesheet::UnknownProperties — a declaration using one really is
// dropped — but they are not worth a warning on every load, because the answer
// is "known, by design" and never "fix your stylesheet".
//
// Keeping them out of the log is what lets the warning stay a warning: before
// this filter the shipped editor emitted one per sheet across 25 sheets, 60% of
// all warning-level output, so a genuine typo would have landed in a stream
// nobody reads. Anything NOT listed here still warns.
//
// `border-*-style` is the per-side half of a whole-box property (see
// ExpandBorderSideValue in CSSValueParsers.cpp); the rest are ordinary CSS
// properties with no implementation here. Add a name only when the decision is
// deliberate — a typo must never end up on this list.
constexpr std::string_view kKnownUnsupportedProperties[] = {
    "-webkit-appearance",
    "appearance",
    "border-bottom-style",
    "border-left-style",
    "border-right-style",
    "border-top-style",
    "box-sizing",
    "isolation",
    "text-decoration",
    "text-transform",
    "transform",
    "user-select",
    "vertical-align",
};

static bool IsKnownUnsupportedProperty(const std::string& name)
{
    return std::find(std::begin(kKnownUnsupportedProperties),
                     std::end(kKnownUnsupportedProperties), name) !=
           std::end(kKnownUnsupportedProperties);
}

size_t WarnUnknownStylesheetProperties(const Stylesheet& sheet)
{
    std::string names;
    size_t count = 0;
    for (const std::string& name : sheet.UnknownProperties)
    {
        if (IsKnownUnsupportedProperty(name))
            continue;
        if (!names.empty())
            names += ", ";
        names += name;
        ++count;
    }
    if (count == 0)
        return 0;

    Logger::Log::Warning(
        "UI CSS: '{}' declares {} unregistered propert{} — every declaration using {} is dropped: {}",
        sheet.SourceName.empty() ? std::string("<inline stylesheet>") : sheet.SourceName, count,
        count == 1 ? "y" : "ies", count == 1 ? "it" : "them", names);
    return count;
}

void CSSParser::ParseInlineStyleToOverrides(std::string_view inlineStyle, StyleOverrides& out)
{
    out.Clear();

    std::vector<StyleProperty> tmpProps;
    tmpProps.reserve(8);

    size_t pos = 0;
    while (pos < inlineStyle.size())
    {
        size_t semi = inlineStyle.find(';', pos);
        if (semi == std::string_view::npos)
            semi = inlineStyle.size();

        std::string_view decl = TrimView(inlineStyle.substr(pos, semi - pos));
        if (!decl.empty())
        {
            size_t colon = decl.find(':');
            if (colon != std::string_view::npos)
            {
                std::string_view nameView = TrimView(decl.substr(0, colon));
                std::string_view valueView = TrimView(decl.substr(colon + 1));
                if (!nameView.empty())
                {
                    if (nameView.size() >= 2 && nameView[0] == '-' && nameView[1] == '-')
                    {
                        // Strip a trailing !important from the value; inline custom
                        // properties don't track importance, but we should not pollute
                        // the stored value with the marker.
                        std::string customValue(valueView);
                        StripImportantSuffix(customValue);
                        out.SetCustom(HashStringId(nameView), std::move(customValue));
                    }
                    else
                    {
                        // Strip !important up front so the var()-skip path below
                        // doesn't ambiguously bail on a marker-laden value, and so
                        // a value of just "!important" doesn't pass the empty check.
                        // Inline overrides currently always win over author CSS, so
                        // the importance bit itself is irrelevant; the strip is purely
                        // to keep the residual value parsable.
                        std::string value(valueView);
                        StripImportantSuffix(value);
                        if (value.empty())
                        {
                            pos = semi + 1;
                            continue;
                        }
                        if (ContainsVarCall(value))
                        {
                            // var() in inline styles is a pre-existing limitation:
                            // resolution would need the cascade scope, which is not
                            // available here. Drop the declaration silently. (Holds
                            // whether or not the marker was present.)
                        }
                        else
                        {
                            const std::string name = ToLowerAscii(std::string(nameView));
                            tmpProps.clear();
                            AppendDeclarationProperties(name, value, tmpProps);
                            for (const auto& prop : tmpProps)
                            {
                                if (prop.PropertyId == StylePropertyId::CustomVar)
                                {
                                    const auto& cv = GetValue<CustomVarDecl>(prop);
                                    out.SetCustom(HashStringId(cv.Name), cv.Value);
                                }
                                else if (prop.PropertyId != StylePropertyId::DeferredDecl)
                                {
                                    ApplyPropertyToOverrides(out, prop);
                                }
                            }
                        }
                    }
                }
            }
        }

        pos = semi + 1;
    }
}

} // namespace UIParsing
} // namespace GameEngine
