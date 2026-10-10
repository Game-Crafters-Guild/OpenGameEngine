// New implementation with expanded selector support

#include "UI/Parsers/CSSParser.h"
#include "CSSParserDetail.h"
#include "UI/ResolvedStyle.h"
#include "UI/StyleProperties.h"
#include "UI/UIElement.h"
#include "../UIAttributeAccess.h"
#include "UI/Registration/ElementRegistration.h"

#include "Logger/Logger.h"
#include "Types/StringUtils.h"
#include <atomic>
#include <bitset>
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <memory>
#include <sstream>
#include <string_view>
#include <list>
#include <unordered_map>

namespace GameEngine
{
namespace UIParsing
{
using namespace CSSDetail;

namespace
{

// Unique id generator for CustomPropertyScope instances. Pointer addresses can be
// reused after scopes are destroyed (especially in unit tests), so caches must
// not rely solely on pointer identity.
static std::atomic<uint64_t> s_CustomScopeUniqueId{1};

#if GE_DEBUG_INSTRUMENTATION
// Cache-vs-live divergence counter for the GE_UI_RULE_CACHE_VALIDATE
// path. Bumped from ValidateRuleCacheAgainstLive when a rule the live
// matcher accepts is missing from the cached match set, or vice versa.
// Tests assert against this; release builds compile the validator out
// entirely.
static std::atomic<uint64_t> s_RuleCacheValidationDivergences{0};

// Override for the env-var gate. -1 = not overridden, 0 = forced off,
// 1 = forced on. Used by tests to drive the validator without mutating
// process env.
static std::atomic<int> s_RuleCacheValidationOverride{-1};
#endif

// Intern font-family lists to avoid per-cascade-application heap allocations.
// Most UIs use 1-3 distinct font families, so a small cache has near-100% hit rate.
static constexpr size_t kFontFamilyCacheSize = 4;
struct FontFamilyCacheEntry
{
    std::shared_ptr<const std::vector<std::string>> Ptr;
};
static thread_local FontFamilyCacheEntry s_FontFamilyCache[kFontFamilyCacheSize];
static thread_local size_t s_FontFamilyCacheNext = 0;

static std::shared_ptr<const std::vector<std::string>>
InternFontFamily(const std::vector<std::string>& families)
{
    for (auto& entry : s_FontFamilyCache)
    {
        if (entry.Ptr && *entry.Ptr == families)
            return entry.Ptr;
    }
    auto ptr = std::make_shared<const std::vector<std::string>>(families);
    s_FontFamilyCache[s_FontFamilyCacheNext % kFontFamilyCacheSize].Ptr = ptr;
    ++s_FontFamilyCacheNext;
    return ptr;
}

static void ApplyProperty(ResolvedStyle& rs,
                          std::unordered_map<StringId, std::string>& customVars,
                          const StyleProperty& p)
{
    // Padding has no `auto` in CSS, so an Auto unit arriving here is the
    // parser's "unset" and collapses to zero.
    auto applyBoxEdge = [](float& outValue, bool& outIsPercent, const StyleLength& len)
    {
        if (len.IsAuto())
        {
            outValue = 0.0f;
            outIsPercent = false;
            return;
        }
        outValue = len.Value;
        outIsPercent = len.IsPercent();
    };

    switch (p.PropertyId)
    {
    case StylePropertyId::Display:
        rs.Layout.DisplayMode = GetValue<DisplayMode>(p);
        break;
    case StylePropertyId::FlexDir:
        rs.Layout.FlexDirection = GetValue<FlexDirection>(p);
        rs.Layout.HasFlexDirection = true;
        break;
    case StylePropertyId::FlexWrap:
        rs.Layout.FlexWrap = GetValue<bool>(p);
        break;
    case StylePropertyId::AlignItems:
        rs.Layout.AlignItems = GetValue<AlignItems>(p);
        break;
    case StylePropertyId::JustifyContent:
        rs.Layout.JustifyContent = GetValue<JustifyContent>(p);
        break;
    case StylePropertyId::AlignContent:
        rs.Layout.AlignContent = GetValue<AlignContent>(p);
        break;
    case StylePropertyId::Direction:
        rs.Layout.Direction = GetValue<Direction>(p);
        break;
    case StylePropertyId::Position:
        rs.Layout.PositionType = GetValue<PositionType>(p);
        break;
    case StylePropertyId::PositionLeft:
        rs.Layout.PositionLeft = GetValue<StyleLength>(p);
        break;
    case StylePropertyId::PositionTop:
        rs.Layout.PositionTop = GetValue<StyleLength>(p);
        break;
    case StylePropertyId::PositionRight:
        rs.Layout.PositionRight = GetValue<StyleLength>(p);
        break;
    case StylePropertyId::PositionBottom:
        rs.Layout.PositionBottom = GetValue<StyleLength>(p);
        break;
    case StylePropertyId::Order:
        rs.Layout.Order = GetValue<int>(p);
        break;
    case StylePropertyId::ZIndex:
        rs.Layout.ZIndex = GetValue<int>(p);
        break;
    case StylePropertyId::AlignSelf:
        rs.Layout.AlignSelf = GetValue<AlignItems>(p);
        break;
    case StylePropertyId::FlexGrow:
        rs.Layout.FlexGrow = GetValue<float>(p);
        break;
    case StylePropertyId::FlexShrink:
        rs.Layout.FlexShrink = GetValue<float>(p);
        break;
    case StylePropertyId::FlexBasis:
        rs.Layout.FlexBasis = GetValue<StyleLength>(p);
        break;
    case StylePropertyId::Visibility:
        rs.Visual.Visible = GetValue<bool>(p);
        rs.Visual.HasVisibility = true;
        break;
    case StylePropertyId::PointerEvents:
        rs.Visual.PointerEvents = GetValue<bool>(p);
        rs.Visual.HasPointerEvents = true;
        break;
    case StylePropertyId::Opacity:
        rs.Visual.LocalOpacity = GetValue<float>(p);
        rs.Visual.Opacity = rs.Visual.LocalOpacity;
        break;
    case StylePropertyId::Overflow:
        rs.Layout.Overflow = GetValue<Overflow>(p);
        break;
    case StylePropertyId::OverflowX:
        rs.Layout.OverflowX = GetValue<Overflow>(p);
        break;
    case StylePropertyId::OverflowY:
        rs.Layout.OverflowY = GetValue<Overflow>(p);
        break;
    case StylePropertyId::Cursor:
        rs.Visual.Cursor = GetValue<CursorStyle>(p);
        rs.Visual.HasCursor = true;
        break;
    case StylePropertyId::TextAlign:
        rs.Visual.TextAlign = GetValue<TextAlign>(p);
        rs.Visual.HasTextAlign = true;
        break;
    case StylePropertyId::WordBreak:
        rs.Visual.WordBreak = GetValue<WordBreak>(p);
        rs.Visual.HasWordBreak = true;
        break;
    case StylePropertyId::OverflowWrap:
        rs.Visual.OverflowWrap = GetValue<OverflowWrap>(p);
        rs.Visual.HasOverflowWrap = true;
        break;
    case StylePropertyId::WhiteSpace:
        rs.Visual.WhiteSpace = GetValue<WhiteSpace>(p);
        rs.Visual.HasWhiteSpace = true;
        break;
    case StylePropertyId::TextOverflow:
        rs.Visual.TextOverflow = GetValue<TextOverflowMode>(p);
        break;
    case StylePropertyId::FontSize:
        SetFontSize(rs.Visual, GetValue<StyleLength>(p));
        break;
    case StylePropertyId::LineHeight:
        rs.Visual.LineHeight = GetValue<float>(p);
        rs.Visual.HasLineHeight = true;
        break;
    case StylePropertyId::LetterSpacing:
        rs.Visual.LetterSpacing = GetValue<float>(p);
        rs.Visual.HasLetterSpacing = true;
        break;
    case StylePropertyId::FontFamily: {
        const auto& families = GetValue<std::vector<std::string>>(p);
        if (!rs.Visual.FontFamily || *rs.Visual.FontFamily != families)
            rs.Visual.FontFamily = InternFontFamily(families);
        rs.Visual.HasFontFamily = true;
        break;
    }
    case StylePropertyId::FontWeight:
        rs.Visual.FontWeight = GetValue<int>(p);
        rs.Visual.HasFontWeight = true;
        break;
    case StylePropertyId::FontStyle:
        rs.Visual.FontStyle = GetValue<FontStyle>(p);
        rs.Visual.HasFontStyle = true;
        break;
    case StylePropertyId::FontVariant:
        rs.Visual.FontVariant = GetValue<FontVariant>(p);
        rs.Visual.HasFontVariant = true;
        break;
    case StylePropertyId::Color:
        rs.Visual.Color = GetValue<uint32_t>(p);
        rs.Visual.HasColor = true;
        break;
    case StylePropertyId::BackgroundColor:
        rs.Visual.BackgroundColor = GetValue<uint32_t>(p);
        break;
    case StylePropertyId::BackgroundImage:
        rs.Visual.BackgroundImage.Source = GetValue<BackgroundImageSource>(p);
        rs.Visual.BackgroundImage.HasImage = rs.Visual.BackgroundImage.Source.HasImage();
        break;
    case StylePropertyId::BackgroundRepeat:
        rs.Visual.BackgroundImage.Repeat = GetValue<BackgroundRepeat>(p);
        break;
    case StylePropertyId::BackgroundSize:
    {
        const auto& size = GetValue<BackgroundSizeValue>(p);
        rs.Visual.BackgroundImage.SizeMode = size.Mode;
        rs.Visual.BackgroundImage.SizeX = size.SizeX;
        rs.Visual.BackgroundImage.SizeXIsPercent = size.SizeXIsPercent;
        rs.Visual.BackgroundImage.SizeY = size.SizeY;
        rs.Visual.BackgroundImage.SizeYIsPercent = size.SizeYIsPercent;
    }
        break;
    case StylePropertyId::BorderImageSlice:
    {
        const auto& s = GetValue<BorderImageSliceValue>(p);
        rs.Visual.BackgroundImage.HasBorderImage = true;
        rs.Visual.BackgroundImage.BiSlice[0] = s.Top;
        rs.Visual.BackgroundImage.BiSlice[1] = s.Right;
        rs.Visual.BackgroundImage.BiSlice[2] = s.Bottom;
        rs.Visual.BackgroundImage.BiSlice[3] = s.Left;
        rs.Visual.BackgroundImage.BiSliceFill = s.Fill;
    }
        break;
    case StylePropertyId::BorderImageRepeat:
    {
        // repeat alone is a no-op (only border-image-slice enables the override);
        // it refines a slice when one is also authored.
        const auto& r = GetValue<BorderImageRepeatValue>(p);
        rs.Visual.BackgroundImage.BiRepeatX = r.X;
        rs.Visual.BackgroundImage.BiRepeatY = r.Y;
    }
        break;
    case StylePropertyId::BackgroundPosition:
    {
        const auto& pos = GetValue<BackgroundPositionValue>(p);
        rs.Visual.BackgroundImage.PosX = pos.X;
        rs.Visual.BackgroundImage.PosXIsPercent = pos.XIsPercent;
        rs.Visual.BackgroundImage.PosY = pos.Y;
        rs.Visual.BackgroundImage.PosYIsPercent = pos.YIsPercent;
    }
        break;
    case StylePropertyId::BackgroundTint:
        rs.Visual.BackgroundImage.Tint = GetValue<uint32_t>(p);
        rs.Visual.BackgroundImage.HasTint = true;
        break;
    case StylePropertyId::BackgroundImageSaturation:
        rs.Visual.BackgroundImage.Saturation = std::clamp(GetValue<float>(p), 0.0f, 1.0f);
        break;
    case StylePropertyId::BorderColor:
    {
        uint32_t color = GetValue<uint32_t>(p);
        rs.Visual.BorderColor.Top = color;
        rs.Visual.BorderColor.Right = color;
        rs.Visual.BorderColor.Bottom = color;
        rs.Visual.BorderColor.Left = color;
    }
        break;
    case StylePropertyId::BorderTopColor:
        rs.Visual.BorderColor.Top = GetValue<uint32_t>(p);
        break;
    case StylePropertyId::BorderRightColor:
        rs.Visual.BorderColor.Right = GetValue<uint32_t>(p);
        break;
    case StylePropertyId::BorderBottomColor:
        rs.Visual.BorderColor.Bottom = GetValue<uint32_t>(p);
        break;
    case StylePropertyId::BorderLeftColor:
        rs.Visual.BorderColor.Left = GetValue<uint32_t>(p);
        break;
    case StylePropertyId::CustomVar:
    {
        const auto& custom = GetValue<CustomVarDecl>(p);
        customVars[HashStringId(custom.Name)] = custom.Value;
    }
        break;

    case StylePropertyId::BorderWidth:
    {
        float bw = std::max(0.0f, GetValue<float>(p));
        rs.Layout.BorderWidth.Top = bw;
        rs.Layout.BorderWidth.Right = bw;
        rs.Layout.BorderWidth.Bottom = bw;
        rs.Layout.BorderWidth.Left = bw;
    }
    break;
    case StylePropertyId::BorderTopWidth:
        rs.Layout.BorderWidth.Top = std::max(0.0f, GetValue<float>(p));
        break;
    case StylePropertyId::BorderRightWidth:
        rs.Layout.BorderWidth.Right = std::max(0.0f, GetValue<float>(p));
        break;
    case StylePropertyId::BorderBottomWidth:
        rs.Layout.BorderWidth.Bottom = std::max(0.0f, GetValue<float>(p));
        break;
    case StylePropertyId::BorderLeftWidth:
        rs.Layout.BorderWidth.Left = std::max(0.0f, GetValue<float>(p));
        break;
    case StylePropertyId::BorderStyle:
        rs.Visual.BorderStyle = GetValue<BorderStyle>(p);
        break;
    case StylePropertyId::OutlineWidth:
        rs.Visual.OutlineWidth = std::max(0.0f, GetValue<float>(p));
        break;
    case StylePropertyId::OutlineStyle:
        rs.Visual.OutlineStyle = GetValue<BorderStyle>(p);
        break;
    case StylePropertyId::OutlineColor:
        rs.Visual.OutlineColor = GetValue<uint32_t>(p);
        rs.Visual.HasOutlineColor = true;
        break;
    case StylePropertyId::OutlineOffset:
        rs.Visual.OutlineOffset = GetValue<float>(p);
        break;
    case StylePropertyId::BorderRadius:
    {
        // Pixels-only shorthand, so it also clears the per-corner percentage
        // flags: a stale flag would reinterpret this value as a percentage.
        float r = std::max(0.0f, GetValue<float>(p));
        rs.Visual.BorderRadius = CornerRadiiTLTRBRBL{r, r, r, r};
        rs.Visual.BorderRadiusIsPercent = {};
    }
    break;
    case StylePropertyId::BorderTopLeftRadius:
        SetBorderRadiusCorner(rs.Visual.BorderRadius.TopLeft,
                              rs.Visual.BorderRadiusIsPercent.TopLeft, GetValue<CornerRadiusValue>(p));
        break;
    case StylePropertyId::BorderTopRightRadius:
        SetBorderRadiusCorner(rs.Visual.BorderRadius.TopRight,
                              rs.Visual.BorderRadiusIsPercent.TopRight, GetValue<CornerRadiusValue>(p));
        break;
    case StylePropertyId::BorderBottomRightRadius:
        SetBorderRadiusCorner(rs.Visual.BorderRadius.BottomRight,
                              rs.Visual.BorderRadiusIsPercent.BottomRight, GetValue<CornerRadiusValue>(p));
        break;
    case StylePropertyId::BorderBottomLeftRadius:
        SetBorderRadiusCorner(rs.Visual.BorderRadius.BottomLeft,
                              rs.Visual.BorderRadiusIsPercent.BottomLeft, GetValue<CornerRadiusValue>(p));
        break;
    case StylePropertyId::Margin:
    {
        const auto& box = GetValue<StyleBox>(p);
        rs.Layout.Margin = box.Values;
        rs.Layout.MarginIsPercent = box.IsPercent;
        rs.Layout.MarginIsAuto = box.IsAuto;
    }
        break;
    case StylePropertyId::MarginTop:
    {
        const auto& len = GetValue<StyleLength>(p);
        SetMarginEdge(rs.Layout.Margin.Top, rs.Layout.MarginIsPercent.Top,
                        rs.Layout.MarginIsAuto.Top, len);
    }
        break;
    case StylePropertyId::MarginRight:
    {
        const auto& len = GetValue<StyleLength>(p);
        SetMarginEdge(rs.Layout.Margin.Right, rs.Layout.MarginIsPercent.Right,
                        rs.Layout.MarginIsAuto.Right, len);
    }
        break;
    case StylePropertyId::MarginBottom:
    {
        const auto& len = GetValue<StyleLength>(p);
        SetMarginEdge(rs.Layout.Margin.Bottom, rs.Layout.MarginIsPercent.Bottom,
                        rs.Layout.MarginIsAuto.Bottom, len);
    }
        break;
    case StylePropertyId::MarginLeft:
    {
        const auto& len = GetValue<StyleLength>(p);
        SetMarginEdge(rs.Layout.Margin.Left, rs.Layout.MarginIsPercent.Left,
                        rs.Layout.MarginIsAuto.Left, len);
    }
        break;
    case StylePropertyId::Gap:
    {
        const auto& len = GetValue<StyleLength>(p);
        SetGapGutter(rs.Layout.Gap, rs.Layout.GapIsPercent, len);
        SetGapGutter(rs.Layout.RowGap, rs.Layout.RowGapIsPercent, len);
        SetGapGutter(rs.Layout.ColumnGap, rs.Layout.ColumnGapIsPercent, len);
    }
        break;
    case StylePropertyId::RowGap:
        SetGapGutter(rs.Layout.RowGap, rs.Layout.RowGapIsPercent, GetValue<StyleLength>(p));
        break;
    case StylePropertyId::ColumnGap:
        SetGapGutter(rs.Layout.ColumnGap, rs.Layout.ColumnGapIsPercent, GetValue<StyleLength>(p));
        break;

    case StylePropertyId::Padding:
    {
        const auto& box = GetValue<StyleBox>(p);
        rs.Layout.Padding = box.Values;
        rs.Layout.PaddingIsPercent = box.IsPercent;
    }
        break;
    case StylePropertyId::PaddingTop:
    {
        const auto& len = GetValue<StyleLength>(p);
        applyBoxEdge(rs.Layout.Padding.Top, rs.Layout.PaddingIsPercent.Top, len);
    }
        break;
    case StylePropertyId::PaddingRight:
    {
        const auto& len = GetValue<StyleLength>(p);
        applyBoxEdge(rs.Layout.Padding.Right, rs.Layout.PaddingIsPercent.Right, len);
    }
        break;
    case StylePropertyId::PaddingBottom:
    {
        const auto& len = GetValue<StyleLength>(p);
        applyBoxEdge(rs.Layout.Padding.Bottom, rs.Layout.PaddingIsPercent.Bottom, len);
    }
        break;
    case StylePropertyId::PaddingLeft:
    {
        const auto& len = GetValue<StyleLength>(p);
        applyBoxEdge(rs.Layout.Padding.Left, rs.Layout.PaddingIsPercent.Left, len);
    }
        break;
    case StylePropertyId::Width:
        rs.Layout.Width = GetValue<StyleLength>(p);
        break;
    case StylePropertyId::Height:
        rs.Layout.Height = GetValue<StyleLength>(p);
        break;
    case StylePropertyId::MinWidth:
        rs.Layout.MinWidth = GetValue<StyleLength>(p);
        break;
    case StylePropertyId::MinHeight:
        rs.Layout.MinHeight = GetValue<StyleLength>(p);
        break;
    case StylePropertyId::MaxWidth:
        rs.Layout.MaxWidth = GetValue<StyleLength>(p);
        break;
    case StylePropertyId::MaxHeight:
        rs.Layout.MaxHeight = GetValue<StyleLength>(p);
        break;
    case StylePropertyId::AspectRatio:
        rs.Layout.AspectRatio = GetValue<float>(p);
        break;
    case StylePropertyId::BoxShadow:
    {
        const auto& sv = GetValue<BoxShadowValue>(p);
        rs.Visual.ShadowOffsetX = sv.OffsetX;
        rs.Visual.ShadowOffsetY = sv.OffsetY;
        rs.Visual.ShadowSoftness = sv.Blur;
        rs.Visual.ShadowColor = sv.Color;
        rs.Visual.ShadowInset = sv.Inset;
        break;
    }
    case StylePropertyId::TextShadowOffsetX: rs.Visual.TextEffects.ShadowOffsetX = GetValue<float>(p); break;
    case StylePropertyId::TextShadowOffsetY: rs.Visual.TextEffects.ShadowOffsetY = GetValue<float>(p); break;
    case StylePropertyId::TextShadowBlur: rs.Visual.TextEffects.ShadowBlur = GetValue<float>(p); break;
    case StylePropertyId::TextShadowColor: rs.Visual.TextEffects.ShadowColor = GetValue<uint32_t>(p); break;
    case StylePropertyId::TextGlowRadius: rs.Visual.TextEffects.GlowRadius = GetValue<float>(p); break;
    case StylePropertyId::TextGlowColor: rs.Visual.TextEffects.GlowColor = GetValue<uint32_t>(p); break;
    case StylePropertyId::TextOutlineWidth: rs.Visual.TextEffects.OutlineWidth = GetValue<float>(p); break;
    case StylePropertyId::TextOutlineColor: rs.Visual.TextEffects.OutlineColor = GetValue<uint32_t>(p); break;
    case StylePropertyId::SelectionColor: rs.Visual.SelectionColor = GetValue<uint32_t>(p); break;
    case StylePropertyId::Glow:
    {
        const auto& gv = GetValue<GlowValue>(p);
        rs.Visual.GlowRadius = gv.Radius;
        rs.Visual.GlowColor = gv.Color;
        break;
    }
    case StylePropertyId::Transition:
    {
        const auto& entries = GetValue<std::vector<TransitionEntry>>(p);
        rs.Transitions.Clear();
        for (const auto& e : entries)
            rs.Transitions.Add(e);
        break;
    }
    default:
        break;
    }
}

static bool IsInheritedByDefault(StylePropertyId kind)
{
    switch (kind)
    {
    case StylePropertyId::TextShadowOffsetX:
    case StylePropertyId::TextShadowOffsetY:
    case StylePropertyId::TextShadowBlur:
    case StylePropertyId::TextShadowColor:
    case StylePropertyId::TextGlowRadius:
    case StylePropertyId::TextGlowColor:
    case StylePropertyId::TextOutlineWidth:
    case StylePropertyId::TextOutlineColor:
    case StylePropertyId::SelectionColor:
    case StylePropertyId::Color:
    case StylePropertyId::FontFamily:
    case StylePropertyId::FontSize:
    case StylePropertyId::LineHeight:
    case StylePropertyId::LetterSpacing:
    case StylePropertyId::FontWeight:
    case StylePropertyId::FontStyle:
    case StylePropertyId::FontVariant:
    case StylePropertyId::TextAlign:
    case StylePropertyId::WordBreak:
    case StylePropertyId::OverflowWrap:
    case StylePropertyId::WhiteSpace:
    case StylePropertyId::Cursor:
    case StylePropertyId::Direction:
    case StylePropertyId::Visibility:
    case StylePropertyId::PointerEvents:
        return true;
    default:
        return false;
    }
}

static void CopyPropertyFromStyle(ResolvedStyle& dst,
                                  const ResolvedStyle& src,
                                  StylePropertyId kind,
                                  bool markSpecified)
{
    switch (kind)
    {
    case StylePropertyId::TextShadowOffsetX: dst.Visual.TextEffects.ShadowOffsetX = src.Visual.TextEffects.ShadowOffsetX; break;
    case StylePropertyId::TextShadowOffsetY: dst.Visual.TextEffects.ShadowOffsetY = src.Visual.TextEffects.ShadowOffsetY; break;
    case StylePropertyId::TextShadowBlur: dst.Visual.TextEffects.ShadowBlur = src.Visual.TextEffects.ShadowBlur; break;
    case StylePropertyId::TextShadowColor: dst.Visual.TextEffects.ShadowColor = src.Visual.TextEffects.ShadowColor; break;
    case StylePropertyId::TextGlowRadius: dst.Visual.TextEffects.GlowRadius = src.Visual.TextEffects.GlowRadius; break;
    case StylePropertyId::TextGlowColor: dst.Visual.TextEffects.GlowColor = src.Visual.TextEffects.GlowColor; break;
    case StylePropertyId::TextOutlineWidth: dst.Visual.TextEffects.OutlineWidth = src.Visual.TextEffects.OutlineWidth; break;
    case StylePropertyId::TextOutlineColor: dst.Visual.TextEffects.OutlineColor = src.Visual.TextEffects.OutlineColor; break;
    case StylePropertyId::SelectionColor: dst.Visual.SelectionColor = src.Visual.SelectionColor; break;
    case StylePropertyId::Display:
        dst.Layout.DisplayMode = src.Layout.DisplayMode;
        break;
    case StylePropertyId::FlexDir:
        dst.Layout.FlexDirection = src.Layout.FlexDirection;
        if (markSpecified)
            dst.Layout.HasFlexDirection = true;
        break;
    case StylePropertyId::FlexWrap:
        dst.Layout.FlexWrap = src.Layout.FlexWrap;
        break;
    case StylePropertyId::AlignItems:
        dst.Layout.AlignItems = src.Layout.AlignItems;
        break;
    case StylePropertyId::JustifyContent:
        dst.Layout.JustifyContent = src.Layout.JustifyContent;
        break;
    case StylePropertyId::AlignSelf:
        dst.Layout.AlignSelf = src.Layout.AlignSelf;
        break;
    case StylePropertyId::AlignContent:
        dst.Layout.AlignContent = src.Layout.AlignContent;
        break;
    case StylePropertyId::Direction:
        dst.Layout.Direction = src.Layout.Direction;
        break;
    case StylePropertyId::Position:
        dst.Layout.PositionType = src.Layout.PositionType;
        break;
    case StylePropertyId::PositionLeft:
        dst.Layout.PositionLeft = src.Layout.PositionLeft;
        break;
    case StylePropertyId::PositionTop:
        dst.Layout.PositionTop = src.Layout.PositionTop;
        break;
    case StylePropertyId::PositionRight:
        dst.Layout.PositionRight = src.Layout.PositionRight;
        break;
    case StylePropertyId::PositionBottom:
        dst.Layout.PositionBottom = src.Layout.PositionBottom;
        break;
    case StylePropertyId::Order:
        dst.Layout.Order = src.Layout.Order;
        break;
    case StylePropertyId::ZIndex:
        dst.Layout.ZIndex = src.Layout.ZIndex;
        break;
    case StylePropertyId::FlexGrow:
        dst.Layout.FlexGrow = src.Layout.FlexGrow;
        break;
    case StylePropertyId::FlexShrink:
        // `initial`/`unset` copy from a pristine style, and `inherit` from a
        // parent that may never have declared the property — both leave the
        // optional empty. The keyword is still a declaration, so land the CSS
        // initial value; leaving it undeclared would read as 0 under block flow.
        dst.Layout.FlexShrink = markSpecified
                                    ? src.Layout.FlexShrink.value_or(kCssInitialFlexShrink)
                                    : src.Layout.FlexShrink;
        break;
    case StylePropertyId::FlexBasis:
        dst.Layout.FlexBasis = src.Layout.FlexBasis;
        break;
    case StylePropertyId::Visibility:
        dst.Visual.Visible = src.Visual.Visible;
        if (markSpecified)
            dst.Visual.HasVisibility = true;
        break;
    case StylePropertyId::PointerEvents:
        dst.Visual.PointerEvents = src.Visual.PointerEvents;
        if (markSpecified)
            dst.Visual.HasPointerEvents = true;
        break;
    case StylePropertyId::Opacity:
        dst.Visual.LocalOpacity = src.Visual.LocalOpacity;
        dst.Visual.Opacity = src.Visual.LocalOpacity;
        break;
    case StylePropertyId::Overflow:
        dst.Layout.Overflow = src.Layout.Overflow;
        break;
    case StylePropertyId::OverflowX:
        dst.Layout.OverflowX = src.Layout.OverflowX;
        break;
    case StylePropertyId::OverflowY:
        dst.Layout.OverflowY = src.Layout.OverflowY;
        break;
    case StylePropertyId::Cursor:
        dst.Visual.Cursor = src.Visual.Cursor;
        if (markSpecified)
            dst.Visual.HasCursor = true;
        break;
    case StylePropertyId::TextAlign:
        dst.Visual.TextAlign = src.Visual.TextAlign;
        if (markSpecified)
            dst.Visual.HasTextAlign = true;
        break;
    case StylePropertyId::WordBreak:
        dst.Visual.WordBreak = src.Visual.WordBreak;
        if (markSpecified)
            dst.Visual.HasWordBreak = true;
        break;
    case StylePropertyId::OverflowWrap:
        dst.Visual.OverflowWrap = src.Visual.OverflowWrap;
        if (markSpecified)
            dst.Visual.HasOverflowWrap = true;
        break;
    case StylePropertyId::WhiteSpace:
        dst.Visual.WhiteSpace = src.Visual.WhiteSpace;
        if (markSpecified)
            dst.Visual.HasWhiteSpace = true;
        break;
    case StylePropertyId::TextOverflow:
        dst.Visual.TextOverflow = src.Visual.TextOverflow;
        break;
    case StylePropertyId::FontSize:
        // What inherits is the COMPUTED size, an absolute length — a percentage
        // is a specified value and never crosses the boundary, so the source's
        // percentage must not come with it (css-fonts-4 §3.5).
        dst.Visual.FontSize = src.Visual.FontSize;
        dst.Visual.FontSizePercent.reset();
        if (markSpecified)
            dst.Visual.HasFontSize = true;
        break;
    case StylePropertyId::LineHeight:
        dst.Visual.LineHeight = src.Visual.LineHeight;
        if (markSpecified)
            dst.Visual.HasLineHeight = true;
        break;
    case StylePropertyId::LetterSpacing:
        dst.Visual.LetterSpacing = src.Visual.LetterSpacing;
        if (markSpecified)
            dst.Visual.HasLetterSpacing = true;
        break;
    case StylePropertyId::FontFamily:
        dst.Visual.FontFamily = src.Visual.FontFamily;
        if (markSpecified)
            dst.Visual.HasFontFamily = true;
        break;
    case StylePropertyId::FontWeight:
        dst.Visual.FontWeight = src.Visual.FontWeight;
        if (markSpecified)
            dst.Visual.HasFontWeight = true;
        break;
    case StylePropertyId::FontStyle:
        dst.Visual.FontStyle = src.Visual.FontStyle;
        if (markSpecified)
            dst.Visual.HasFontStyle = true;
        break;
    case StylePropertyId::FontVariant:
        dst.Visual.FontVariant = src.Visual.FontVariant;
        if (markSpecified)
            dst.Visual.HasFontVariant = true;
        break;
    case StylePropertyId::Color:
        dst.Visual.Color = src.Visual.Color;
        if (markSpecified)
            dst.Visual.HasColor = true;
        break;
    case StylePropertyId::BackgroundColor:
        dst.Visual.BackgroundColor = src.Visual.BackgroundColor;
        break;
    case StylePropertyId::BackgroundImage:
        dst.Visual.BackgroundImage.Source = src.Visual.BackgroundImage.Source;
        dst.Visual.BackgroundImage.HasImage = src.Visual.BackgroundImage.HasImage;
        break;
    case StylePropertyId::BackgroundRepeat:
        dst.Visual.BackgroundImage.Repeat = src.Visual.BackgroundImage.Repeat;
        break;
    case StylePropertyId::BackgroundSize:
        dst.Visual.BackgroundImage.SizeMode = src.Visual.BackgroundImage.SizeMode;
        dst.Visual.BackgroundImage.SizeX = src.Visual.BackgroundImage.SizeX;
        dst.Visual.BackgroundImage.SizeXIsPercent = src.Visual.BackgroundImage.SizeXIsPercent;
        dst.Visual.BackgroundImage.SizeY = src.Visual.BackgroundImage.SizeY;
        dst.Visual.BackgroundImage.SizeYIsPercent = src.Visual.BackgroundImage.SizeYIsPercent;
        break;
    case StylePropertyId::BorderImageSlice:
        dst.Visual.BackgroundImage.HasBorderImage = true;
        dst.Visual.BackgroundImage.BiSlice[0] = src.Visual.BackgroundImage.BiSlice[0];
        dst.Visual.BackgroundImage.BiSlice[1] = src.Visual.BackgroundImage.BiSlice[1];
        dst.Visual.BackgroundImage.BiSlice[2] = src.Visual.BackgroundImage.BiSlice[2];
        dst.Visual.BackgroundImage.BiSlice[3] = src.Visual.BackgroundImage.BiSlice[3];
        dst.Visual.BackgroundImage.BiSliceFill = src.Visual.BackgroundImage.BiSliceFill;
        break;
    case StylePropertyId::BorderImageRepeat:
        dst.Visual.BackgroundImage.BiRepeatX = src.Visual.BackgroundImage.BiRepeatX;
        dst.Visual.BackgroundImage.BiRepeatY = src.Visual.BackgroundImage.BiRepeatY;
        break;
    case StylePropertyId::BackgroundPosition:
        dst.Visual.BackgroundImage.PosX = src.Visual.BackgroundImage.PosX;
        dst.Visual.BackgroundImage.PosXIsPercent = src.Visual.BackgroundImage.PosXIsPercent;
        dst.Visual.BackgroundImage.PosY = src.Visual.BackgroundImage.PosY;
        dst.Visual.BackgroundImage.PosYIsPercent = src.Visual.BackgroundImage.PosYIsPercent;
        break;
    case StylePropertyId::BackgroundTint:
        dst.Visual.BackgroundImage.Tint = src.Visual.BackgroundImage.Tint;
        dst.Visual.BackgroundImage.HasTint = src.Visual.BackgroundImage.HasTint;
        break;
    case StylePropertyId::BackgroundImageSaturation:
        dst.Visual.BackgroundImage.Saturation = src.Visual.BackgroundImage.Saturation;
        break;
    case StylePropertyId::BorderColor:
        dst.Visual.BorderColor = src.Visual.BorderColor;
        break;
    case StylePropertyId::BorderTopColor:
        dst.Visual.BorderColor.Top = src.Visual.BorderColor.Top;
        break;
    case StylePropertyId::BorderRightColor:
        dst.Visual.BorderColor.Right = src.Visual.BorderColor.Right;
        break;
    case StylePropertyId::BorderBottomColor:
        dst.Visual.BorderColor.Bottom = src.Visual.BorderColor.Bottom;
        break;
    case StylePropertyId::BorderLeftColor:
        dst.Visual.BorderColor.Left = src.Visual.BorderColor.Left;
        break;
    case StylePropertyId::BorderWidth:
        dst.Layout.BorderWidth = src.Layout.BorderWidth;
        break;
    case StylePropertyId::BorderTopWidth:
        dst.Layout.BorderWidth.Top = src.Layout.BorderWidth.Top;
        break;
    case StylePropertyId::BorderRightWidth:
        dst.Layout.BorderWidth.Right = src.Layout.BorderWidth.Right;
        break;
    case StylePropertyId::BorderBottomWidth:
        dst.Layout.BorderWidth.Bottom = src.Layout.BorderWidth.Bottom;
        break;
    case StylePropertyId::BorderLeftWidth:
        dst.Layout.BorderWidth.Left = src.Layout.BorderWidth.Left;
        break;
    case StylePropertyId::BorderStyle:
        dst.Visual.BorderStyle = src.Visual.BorderStyle;
        break;
    case StylePropertyId::OutlineWidth:
        dst.Visual.OutlineWidth = src.Visual.OutlineWidth;
        break;
    case StylePropertyId::OutlineStyle:
        dst.Visual.OutlineStyle = src.Visual.OutlineStyle;
        break;
    case StylePropertyId::OutlineColor:
        dst.Visual.OutlineColor = src.Visual.OutlineColor;
        dst.Visual.HasOutlineColor = src.Visual.HasOutlineColor;
        break;
    case StylePropertyId::OutlineOffset:
        dst.Visual.OutlineOffset = src.Visual.OutlineOffset;
        break;
    // A radius and its percentage flag are one value in two fields; copying the
    // number without the flag reinterprets a percentage as pixels.
    case StylePropertyId::BorderRadius:
        dst.Visual.BorderRadius = src.Visual.BorderRadius;
        dst.Visual.BorderRadiusIsPercent = src.Visual.BorderRadiusIsPercent;
        break;
    case StylePropertyId::BorderTopLeftRadius:
        dst.Visual.BorderRadius.TopLeft = src.Visual.BorderRadius.TopLeft;
        dst.Visual.BorderRadiusIsPercent.TopLeft = src.Visual.BorderRadiusIsPercent.TopLeft;
        break;
    case StylePropertyId::BorderTopRightRadius:
        dst.Visual.BorderRadius.TopRight = src.Visual.BorderRadius.TopRight;
        dst.Visual.BorderRadiusIsPercent.TopRight = src.Visual.BorderRadiusIsPercent.TopRight;
        break;
    case StylePropertyId::BorderBottomRightRadius:
        dst.Visual.BorderRadius.BottomRight = src.Visual.BorderRadius.BottomRight;
        dst.Visual.BorderRadiusIsPercent.BottomRight = src.Visual.BorderRadiusIsPercent.BottomRight;
        break;
    case StylePropertyId::BorderBottomLeftRadius:
        dst.Visual.BorderRadius.BottomLeft = src.Visual.BorderRadius.BottomLeft;
        dst.Visual.BorderRadiusIsPercent.BottomLeft = src.Visual.BorderRadiusIsPercent.BottomLeft;
        break;
    case StylePropertyId::Margin:
        dst.Layout.Margin = src.Layout.Margin;
        dst.Layout.MarginIsPercent = src.Layout.MarginIsPercent;
        dst.Layout.MarginIsAuto = src.Layout.MarginIsAuto;
        break;
    case StylePropertyId::MarginTop:
        dst.Layout.Margin.Top = src.Layout.Margin.Top;
        dst.Layout.MarginIsPercent.Top = src.Layout.MarginIsPercent.Top;
        dst.Layout.MarginIsAuto.Top = src.Layout.MarginIsAuto.Top;
        break;
    case StylePropertyId::MarginRight:
        dst.Layout.Margin.Right = src.Layout.Margin.Right;
        dst.Layout.MarginIsPercent.Right = src.Layout.MarginIsPercent.Right;
        dst.Layout.MarginIsAuto.Right = src.Layout.MarginIsAuto.Right;
        break;
    case StylePropertyId::MarginBottom:
        dst.Layout.Margin.Bottom = src.Layout.Margin.Bottom;
        dst.Layout.MarginIsPercent.Bottom = src.Layout.MarginIsPercent.Bottom;
        dst.Layout.MarginIsAuto.Bottom = src.Layout.MarginIsAuto.Bottom;
        break;
    case StylePropertyId::MarginLeft:
        dst.Layout.Margin.Left = src.Layout.Margin.Left;
        dst.Layout.MarginIsPercent.Left = src.Layout.MarginIsPercent.Left;
        dst.Layout.MarginIsAuto.Left = src.Layout.MarginIsAuto.Left;
        break;
    case StylePropertyId::Padding:
        dst.Layout.Padding = src.Layout.Padding;
        dst.Layout.PaddingIsPercent = src.Layout.PaddingIsPercent;
        break;
    case StylePropertyId::PaddingTop:
        dst.Layout.Padding.Top = src.Layout.Padding.Top;
        dst.Layout.PaddingIsPercent.Top = src.Layout.PaddingIsPercent.Top;
        break;
    case StylePropertyId::PaddingRight:
        dst.Layout.Padding.Right = src.Layout.Padding.Right;
        dst.Layout.PaddingIsPercent.Right = src.Layout.PaddingIsPercent.Right;
        break;
    case StylePropertyId::PaddingBottom:
        dst.Layout.Padding.Bottom = src.Layout.Padding.Bottom;
        dst.Layout.PaddingIsPercent.Bottom = src.Layout.PaddingIsPercent.Bottom;
        break;
    case StylePropertyId::PaddingLeft:
        dst.Layout.Padding.Left = src.Layout.Padding.Left;
        dst.Layout.PaddingIsPercent.Left = src.Layout.PaddingIsPercent.Left;
        break;
    case StylePropertyId::Gap:
        dst.Layout.Gap = src.Layout.Gap;
        dst.Layout.GapIsPercent = src.Layout.GapIsPercent;
        dst.Layout.RowGap = src.Layout.RowGap;
        dst.Layout.RowGapIsPercent = src.Layout.RowGapIsPercent;
        dst.Layout.ColumnGap = src.Layout.ColumnGap;
        dst.Layout.ColumnGapIsPercent = src.Layout.ColumnGapIsPercent;
        break;
    case StylePropertyId::RowGap:
        dst.Layout.RowGap = src.Layout.RowGap;
        dst.Layout.RowGapIsPercent = src.Layout.RowGapIsPercent;
        break;
    case StylePropertyId::ColumnGap:
        dst.Layout.ColumnGap = src.Layout.ColumnGap;
        dst.Layout.ColumnGapIsPercent = src.Layout.ColumnGapIsPercent;
        break;
    case StylePropertyId::Width:
        dst.Layout.Width = src.Layout.Width;
        break;
    case StylePropertyId::Height:
        dst.Layout.Height = src.Layout.Height;
        break;
    case StylePropertyId::MinWidth:
        dst.Layout.MinWidth = src.Layout.MinWidth;
        break;
    case StylePropertyId::MinHeight:
        dst.Layout.MinHeight = src.Layout.MinHeight;
        break;
    case StylePropertyId::MaxWidth:
        dst.Layout.MaxWidth = src.Layout.MaxWidth;
        break;
    case StylePropertyId::MaxHeight:
        dst.Layout.MaxHeight = src.Layout.MaxHeight;
        break;
    case StylePropertyId::AspectRatio:
        dst.Layout.AspectRatio = src.Layout.AspectRatio;
        break;
    default:
        break;
    }
}

static void ApplyStyleKeyword(ResolvedStyle& rs,
                              StylePropertyId kind,
                              StyleKeyword kw,
                              const ResolvedStyle& initial,
                              const ResolvedStyle* parentStyle)
{
    if (kw == StyleKeyword::None)
        return;

    const ResolvedStyle& parentOrInitial = parentStyle ? *parentStyle : initial;
    switch (kw)
    {
    case StyleKeyword::Inherit:
        CopyPropertyFromStyle(rs, parentOrInitial, kind, true);
        break;
    case StyleKeyword::Initial:
        CopyPropertyFromStyle(rs, initial, kind, true);
        break;
    case StyleKeyword::Unset:
        if (IsInheritedByDefault(kind))
            CopyPropertyFromStyle(rs, parentOrInitial, kind, true);
        else
            CopyPropertyFromStyle(rs, initial, kind, true);
        break;
    default:
        break;
    }
}

// Matching functions
static bool EqualsCase(const std::string& a, const std::string& b, bool caseInsensitive)
{
    return caseInsensitive ? Ieq(a, b) : (a == b);
}

static const std::string* FindSelectorAttribute(const UIElement& el, const std::string& name)
{
    if (Ieq(name, "id") || Ieq(name, "name"))
    {
        const std::string& id = el.GetId();
        return id.empty() ? nullptr : &id;
    }
    return UIAttributeAccess::FindAuthoredAttribute(el, name);
}

static bool MatchesAttributeSelector(const UIElement& el, const AttributeSelector& attr)
{
    const std::string* valuePtr = FindSelectorAttribute(el, attr.Name);
    if (!valuePtr)
    {
        return false;
    }
    const std::string& val = *valuePtr;

    switch (attr.Match)
    {
    case AttributeSelector::MatchType::Exists:
        return true;
    case AttributeSelector::MatchType::Equals:
        return EqualsCase(val, attr.Value, attr.CaseInsensitive);
    case AttributeSelector::MatchType::Includes:
    {
        std::istringstream ss(val);
        std::string token;
        while (ss >> token)
        {
            if (EqualsCase(token, attr.Value, attr.CaseInsensitive))
                return true;
        }
        return false;
    }
    case AttributeSelector::MatchType::DashMatch:
        return EqualsCase(val, attr.Value, attr.CaseInsensitive) ||
               (val.size() > attr.Value.size() && EqualsCase(val.substr(0, attr.Value.size()), attr.Value, attr.CaseInsensitive) && val[attr.Value.size()] == '-');
    case AttributeSelector::MatchType::Prefix:
        if (attr.Value.size() > val.size())
            return false;
        return EqualsCase(val.substr(0, attr.Value.size()), attr.Value, attr.CaseInsensitive);
    case AttributeSelector::MatchType::Suffix:
        if (attr.Value.size() > val.size())
            return false;
        return EqualsCase(val.substr(val.size() - attr.Value.size()), attr.Value, attr.CaseInsensitive);
    case AttributeSelector::MatchType::Substring:
        if (attr.CaseInsensitive)
        {
            std::string hay = ToLowerAscii(val);
            std::string needle = ToLowerAscii(attr.Value);
            return hay.find(needle) != std::string::npos;
        }
        return val.find(attr.Value) != std::string::npos;
    }
    return false;
}

static const UIElement* GetPreviousSibling(const UIElement& el)
{
    const UIElement* parent = el.GetParent();
    if (!parent)
        return nullptr;
    const auto& children = parent->GetChildren();
    const UIElement* prev = nullptr;
    for (const auto& child : children)
    {
        if (child.get() == &el)
            return prev;
        prev = child.get();
    }
    return nullptr;
}

static int ComputeSiblingIndex(const UIElement& el, bool ofType, bool fromEnd)
{
    const UIElement* parent = el.GetParent();
    if (!parent)
        return 1;
    const auto& children = parent->GetChildren();
    if (children.empty())
        return -1;
    int index = 0;
    const std::type_info& elType = typeid(el);

    if (fromEnd)
    {
        for (int i = static_cast<int>(children.size()) - 1; i >= 0; --i)
        {
            const UIElement* child = children[i].get();
            if (ofType && typeid(*child) != elType)
                continue;
            ++index;
            if (child == &el)
                return index;
        }
    }
    else
    {
        for (size_t i = 0; i < children.size(); ++i)
        {
            const UIElement* child = children[i].get();
            if (ofType && typeid(*child) != elType)
                continue;
            ++index;
            if (child == &el)
                return index;
        }
    }
    return -1;
}

static int CountSiblingsOfType(const UIElement& el, bool ofType)
{
    const UIElement* parent = el.GetParent();
    if (!parent)
        return 1;
    const auto& children = parent->GetChildren();
    if (!ofType)
        return static_cast<int>(children.size());
    int count = 0;
    const std::type_info& elType = typeid(el);
    for (const auto& child : children)
    {
        if (typeid(*child) == elType)
            ++count;
    }
    return count;
}

static bool MatchesNthIndex(int index, const NthPattern& pattern)
{
    if (index <= 0)
        return false;
    int a = pattern.A;
    int b = pattern.B;
    if (a == 0)
        return index == b;
    int diff = index - b;
    if (a > 0)
    {
        if (diff < 0)
            return false;
        return diff % a == 0;
    }
    else
    {
        if (diff > 0)
            return false;
        a = -a;
        diff = -diff;
        return diff % a == 0;
    }
}

static bool MatchesSelectorChain(const UIElement& el,
                                 const SelectorChain& chain,
                                 const ElementState& state,
                                 const UIElement* stateTarget,
                                 const UIElement* hoverTarget);

// Helper: determine whether an element should match :hover given the currently
// hovered element (if any). We want standard CSS semantics where a rule like
// `.parent:hover .child` matches when the mouse is over either the parent or
// any of its descendants.
static bool ElementMatchesHover(const UIElement& el, const UIElement* hoverTarget)
{
    if (!hoverTarget)
        return false;

    const UIElement* cur = hoverTarget;
    while (cur)
    {
        if (cur == &el)
            return true;
        cur = cur->GetParent();
    }
    return false;
}

static bool MatchesPseudo(const UIElement& el,
                          const PseudoClass& pseudo,
                          const ElementState& state,
                          const UIElement* stateTarget,
                          const UIElement* hoverTarget)
{
    const bool isTarget = (&el == stateTarget);
    switch (pseudo.PseudoKind)
    {
    case PseudoClass::Kind::Hover:
        // When a global hoverTarget is provided (e.g., from UIManager), use
        // it to implement ancestor :hover so selectors like
        // `.parent:hover .child` work. Fall back to the previous
        // per-element semantics if no hoverTarget is supplied (tests or
        // callers that only care about direct :hover on the element).
        if (hoverTarget)
            return ElementMatchesHover(el, hoverTarget);
        return isTarget && state.Hover;
    case PseudoClass::Kind::Active:
        return isTarget && state.Active;
    // Focus family: the target compound reads the passed ElementState (so
    // direct ComputeStyleFor callers / tests that set state.Focus keep working);
    // a NON-target compound (ancestor via a descendant/child combinator, e.g.
    // `.search-bar:focus-within .icon` or `#panel:focus .child`) reads the
    // compound element's own per-element predicate, maintained by the manager in
    // RebuildFocusWithinChain. Mirrors the :disabled/:checked split below.
    case PseudoClass::Kind::Focus:
        return isTarget ? state.Focus : el.IsPseudoFocused();
    case PseudoClass::Kind::FocusVisible:
        return isTarget ? state.FocusVisible : el.IsPseudoFocusVisible();
    case PseudoClass::Kind::FocusWithin:
        return isTarget ? state.FocusWithin : el.IsInFocusChain();
    case PseudoClass::Kind::Disabled:
        return isTarget ? state.Disabled : !el.IsEnabled();
    case PseudoClass::Kind::Enabled:
        return isTarget ? state.Enabled : el.IsEnabled();
    case PseudoClass::Kind::Checked:
        return isTarget ? state.Checked : el.IsPseudoChecked();
    case PseudoClass::Kind::Root:
        return el.GetParent() == nullptr;
    case PseudoClass::Kind::Empty:
        return el.GetChildren().empty();
    case PseudoClass::Kind::FirstChild:
        return ComputeSiblingIndex(el, false, false) == 1;
    case PseudoClass::Kind::LastChild:
        return ComputeSiblingIndex(el, false, true) == 1;
    case PseudoClass::Kind::OnlyChild:
        return CountSiblingsOfType(el, false) == 1;
    case PseudoClass::Kind::FirstOfType:
        return ComputeSiblingIndex(el, true, false) == 1;
    case PseudoClass::Kind::LastOfType:
        return ComputeSiblingIndex(el, true, true) == 1;
    case PseudoClass::Kind::OnlyOfType:
        return CountSiblingsOfType(el, true) == 1;
    case PseudoClass::Kind::NthChild:
    case PseudoClass::Kind::NthLastChild:
    case PseudoClass::Kind::NthOfType:
    case PseudoClass::Kind::NthLastOfType:
    {
        int idx = ComputeSiblingIndex(el, pseudo.Nth.OfType, pseudo.Nth.FromEnd);
        if (idx < 0)
            return false;
        return MatchesNthIndex(idx, pseudo.Nth);
    }
    case PseudoClass::Kind::Is:
    case PseudoClass::Kind::Where:
    {
        for (const auto& nested : pseudo.SelectorList)
        {
            if (nested && MatchesSelectorChain(el, *nested, state, stateTarget, hoverTarget))
                return true;
        }
        return false;
    }
    case PseudoClass::Kind::Not:
    {
        for (const auto& nested : pseudo.SelectorList)
        {
            if (nested && MatchesSelectorChain(el, *nested, state, stateTarget, hoverTarget))
                return false;
        }
        return true;
    }
    case PseudoClass::Kind::Custom:
        // Target compound reads the cascade's ElementState set (so direct
        // ComputeStyleFor callers / tests keep working); a non-target compound
        // (ancestor via a descendant/child combinator, e.g. `#panel:loading
        // .spinner`) reads the element's own custom-state set. Mirrors the
        // :focus/:checked target/ancestor split above.
        return isTarget
                   ? state.CustomStates.find(pseudo.ResolvedCustomNameId) != state.CustomStates.end()
                   : el.HasCustomState(pseudo.ResolvedCustomNameId);
    }
    return false;
}

static bool MatchesCompound(const UIElement& el,
                            const CompoundSelector& compound,
                            const ElementState& state,
                            const UIElement* stateTarget,
                            const UIElement* hoverTarget)
{
    if (compound.ResolvedTagId)
    {
        StringId elTagId = UIRegistration::ElementFactoryRegistry::Instance().GetElementTagId(el);
        if (elTagId != compound.ResolvedTagId)
            return false;
    }
    else if (!compound.Tag.empty() && !compound.Universal)
    {
        return false; // unresolved tag -- can never match
    }
    if (compound.ResolvedId != 0 && el.GetIdHash() != compound.ResolvedId)
        return false;
    for (StringId clsId : compound.ResolvedClassIds)
    {
        if (!el.HasClass(clsId))
            return false;
    }
    for (const auto& attr : compound.Attributes)
    {
        if (!MatchesAttributeSelector(el, attr))
            return false;
    }
    for (const auto& pseudo : compound.Pseudos)
    {
        if (!MatchesPseudo(el, pseudo, state, stateTarget, hoverTarget))
            return false;
    }
    return true;
}

static bool MatchesChainRecursive(const UIElement& el,
                                  const SelectorChain& chain,
                                  size_t termIndex,
                                  const ElementState& state,
                                  const UIElement* stateTarget,
                                  const UIElement* hoverTarget)
{
    const SelectorTerm& term = chain.Terms[termIndex];
    if (!MatchesCompound(el, term.Selector, state, stateTarget, hoverTarget))
        return false;
    if (termIndex == 0)
        return true;

    switch (term.Comb)
    {
    case SelectorTerm::Combinator::None:
    case SelectorTerm::Combinator::Descendant:
    {
        const UIElement* parent = el.GetParent();
        while (parent)
        {
            if (MatchesChainRecursive(*parent, chain, termIndex - 1, state, stateTarget, hoverTarget))
                return true;
            parent = parent->GetParent();
        }
        return false;
    }
    case SelectorTerm::Combinator::Child:
    {
        const UIElement* parent = el.GetParent();
        if (!parent)
            return false;
        return MatchesChainRecursive(*parent, chain, termIndex - 1, state, stateTarget, hoverTarget);
    }
    case SelectorTerm::Combinator::AdjacentSibling:
    {
        const UIElement* prev = GetPreviousSibling(el);
        if (!prev)
            return false;
        return MatchesChainRecursive(*prev, chain, termIndex - 1, state, stateTarget, hoverTarget);
    }
    case SelectorTerm::Combinator::GeneralSibling:
    {
        const UIElement* prev = GetPreviousSibling(el);
        while (prev)
        {
            if (MatchesChainRecursive(*prev, chain, termIndex - 1, state, stateTarget, hoverTarget))
                return true;
            prev = GetPreviousSibling(*prev);
        }
        return false;
    }
    }
    return false;
}

static bool MatchesSelectorChain(const UIElement& el,
                                 const SelectorChain& chain,
                                 const ElementState& state,
                                 const UIElement* stateTarget,
                                 const UIElement* hoverTarget)
{
    if (chain.Terms.empty())
        return false;
    return MatchesChainRecursive(el, chain, chain.Terms.size() - 1, state, stateTarget, hoverTarget);
}

// ---------------------------------------------------------------------------
// Cascade-matcher memoization Phase 3: structural matcher + cache builder.
//
// Structural variant of the matcher above. Treats dynamic pseudo classes
// (hover/active/focus/focus-visible/focus-within/disabled/enabled/checked/
// custom) as `true`, evaluating only structural pseudos (root/empty/nth-*/
// first/last/only-*) plus the recursive ones (:is/:where/:not, which
// recurse with the same dynamic-pseudo skip semantics).
//
// The cache stores the result so apply-time can re-check only the dynamic
// pseudos against current ElementState — bit comparisons, no class/id/parent
// walks. Cache invalidation (Phase 2) handles structural mutations.
// ---------------------------------------------------------------------------

static bool MatchesSelectorChainStructural(const UIElement& el,
                                           const SelectorChain& chain);

static bool MatchesPseudoStructural(const UIElement& el,
                                    const PseudoClass& pseudo)
{
    switch (pseudo.PseudoKind)
    {
    // Dynamic pseudos: skip at structural-match time. The cache stores a
    // `hasDynamicPseudo` flag so apply-time knows to re-check these against
    // current ElementState. Filtered to `true` here so the structural walk
    // doesn't reject rules that would match given the right runtime state.
    case PseudoClass::Kind::Hover:
    case PseudoClass::Kind::Active:
    case PseudoClass::Kind::Focus:
    case PseudoClass::Kind::FocusVisible:
    case PseudoClass::Kind::FocusWithin:
    case PseudoClass::Kind::Disabled:
    case PseudoClass::Kind::Enabled:
    case PseudoClass::Kind::Checked:
    case PseudoClass::Kind::Custom:
        return true;

    // Structural pseudos: depend only on tree shape; cache invalidation
    // handles tree mutations so we can evaluate them at structural-match
    // time.
    case PseudoClass::Kind::Root:
        return el.GetParent() == nullptr;
    case PseudoClass::Kind::Empty:
        return el.GetChildren().empty();
    case PseudoClass::Kind::FirstChild:
        return ComputeSiblingIndex(el, false, false) == 1;
    case PseudoClass::Kind::LastChild:
        return ComputeSiblingIndex(el, false, true) == 1;
    case PseudoClass::Kind::OnlyChild:
        return CountSiblingsOfType(el, false) == 1;
    case PseudoClass::Kind::FirstOfType:
        return ComputeSiblingIndex(el, true, false) == 1;
    case PseudoClass::Kind::LastOfType:
        return ComputeSiblingIndex(el, true, true) == 1;
    case PseudoClass::Kind::OnlyOfType:
        return CountSiblingsOfType(el, true) == 1;
    case PseudoClass::Kind::NthChild:
    case PseudoClass::Kind::NthLastChild:
    case PseudoClass::Kind::NthOfType:
    case PseudoClass::Kind::NthLastOfType:
    {
        int idx = ComputeSiblingIndex(el, pseudo.Nth.OfType, pseudo.Nth.FromEnd);
        if (idx < 0)
            return false;
        return MatchesNthIndex(idx, pseudo.Nth);
    }

    // Recursive pseudos: recurse with structural semantics so nested
    // dynamic pseudos (`:is(.foo:hover)`) also resolve to true at
    // structural-match time.
    case PseudoClass::Kind::Is:
    case PseudoClass::Kind::Where:
    {
        for (const auto& nested : pseudo.SelectorList)
        {
            if (nested && MatchesSelectorChainStructural(el, *nested))
                return true;
        }
        return false;
    }
    case PseudoClass::Kind::Not:
    {
        for (const auto& nested : pseudo.SelectorList)
        {
            if (nested && MatchesSelectorChainStructural(el, *nested))
                return false;
        }
        return true;
    }
    }
    return false;
}

static bool MatchesCompoundStructural(const UIElement& el,
                                      const CompoundSelector& compound)
{
    if (compound.ResolvedTagId)
    {
        StringId elTagId = UIRegistration::ElementFactoryRegistry::Instance().GetElementTagId(el);
        if (elTagId != compound.ResolvedTagId)
            return false;
    }
    else if (!compound.Tag.empty() && !compound.Universal)
    {
        return false;
    }
    if (compound.ResolvedId != 0 && el.GetIdHash() != compound.ResolvedId)
        return false;
    for (StringId clsId : compound.ResolvedClassIds)
    {
        if (!el.HasClass(clsId))
            return false;
    }
    for (const auto& attr : compound.Attributes)
    {
        if (!MatchesAttributeSelector(el, attr))
            return false;
    }
    for (const auto& pseudo : compound.Pseudos)
    {
        if (!MatchesPseudoStructural(el, pseudo))
            return false;
    }
    return true;
}

static bool MatchesChainRecursiveStructural(const UIElement& el,
                                            const SelectorChain& chain,
                                            size_t termIndex)
{
    const SelectorTerm& term = chain.Terms[termIndex];
    if (!MatchesCompoundStructural(el, term.Selector))
        return false;
    if (termIndex == 0)
        return true;

    switch (term.Comb)
    {
    case SelectorTerm::Combinator::None:
    case SelectorTerm::Combinator::Descendant:
    {
        const UIElement* parent = el.GetParent();
        while (parent)
        {
            if (MatchesChainRecursiveStructural(*parent, chain, termIndex - 1))
                return true;
            parent = parent->GetParent();
        }
        return false;
    }
    case SelectorTerm::Combinator::Child:
    {
        const UIElement* parent = el.GetParent();
        if (!parent)
            return false;
        return MatchesChainRecursiveStructural(*parent, chain, termIndex - 1);
    }
    case SelectorTerm::Combinator::AdjacentSibling:
    {
        const UIElement* prev = GetPreviousSibling(el);
        if (!prev)
            return false;
        return MatchesChainRecursiveStructural(*prev, chain, termIndex - 1);
    }
    case SelectorTerm::Combinator::GeneralSibling:
    {
        const UIElement* prev = GetPreviousSibling(el);
        while (prev)
        {
            if (MatchesChainRecursiveStructural(*prev, chain, termIndex - 1))
                return true;
            prev = GetPreviousSibling(*prev);
        }
        return false;
    }
    }
    return false;
}

static bool MatchesSelectorChainStructural(const UIElement& el,
                                           const SelectorChain& chain)
{
    if (chain.Terms.empty())
        return false;
    return MatchesChainRecursiveStructural(el, chain, chain.Terms.size() - 1);
}

// Returns true if `chain` references any dynamic pseudo class (anywhere —
// including nested :is/:where/:not). Used by BuildRuleCache to flag rules
// that need apply-time pseudo filtering. Rules with no dynamic pseudo
// always-apply on cache hit (cheaper).
static bool HasDynamicPseudoInChain(const SelectorChain& chain)
{
    for (const SelectorTerm& term : chain.Terms)
    {
        for (const PseudoClass& pseudo : term.Selector.Pseudos)
        {
            switch (pseudo.PseudoKind)
            {
            case PseudoClass::Kind::Hover:
            case PseudoClass::Kind::Active:
            case PseudoClass::Kind::Focus:
            case PseudoClass::Kind::FocusVisible:
            case PseudoClass::Kind::FocusWithin:
            case PseudoClass::Kind::Disabled:
            case PseudoClass::Kind::Enabled:
            case PseudoClass::Kind::Checked:
            case PseudoClass::Kind::Custom:
                return true;
            case PseudoClass::Kind::Is:
            case PseudoClass::Kind::Where:
            case PseudoClass::Kind::Not:
                for (const auto& nested : pseudo.SelectorList)
                    if (nested && HasDynamicPseudoInChain(*nested))
                        return true;
                break;
            default:
                break;
            }
        }
    }
    return false;
}

} // namespace

ResolvedStyle CSSParser::ComputeStyleFor(const UIElement& el,
                                         const Stylesheet& sheet,
                                         const ElementState& state,
                                         const UIElement* hoverTarget)
{
    const Stylesheet* s = &sheet;
    return ComputeStyleFor(
        el,
        std::span<const Stylesheet* const>(&s, 1),
        std::span<const StylesheetRuleIndex* const>{},
        state,
        (const ResolvedStyle*)nullptr,
        hoverTarget);
}

ResolvedStyle CSSParser::ComputeStyleFor(const UIElement& el,
                                         const std::vector<const Stylesheet*>& sheets,
                                         const ElementState& state,
                                         const UIElement* hoverTarget)
{
    return ComputeStyleFor(el, sheets, state, (const ResolvedStyle*)nullptr, hoverTarget);
}

ResolvedStyle CSSParser::ComputeStyleFor(const UIElement& el,
                                         const std::vector<const Stylesheet*>& sheets,
                                         const ElementState& state,
                                         const ResolvedStyle* parentStyle,
                                         const UIElement* hoverTarget)
{
    static const std::vector<const StylesheetRuleIndex*> kNoIndices;
    return ComputeStyleFor(el, sheets, kNoIndices, state, parentStyle, hoverTarget);
}

// Cascade-memoization Phase 4 row record. Hoisted to namespace scope so
// the GE_UI_RULE_CACHE_VALIDATE validator can take a span of these
// records without depending on a function-local type.
namespace
{
struct MatchRec
{
    const CSSRule* Rule;
    StyleOrigin Origin;
    Specificity Spec;
    uint32_t SheetIndex;
};

MatchRec MakeMatch(const CSSRule& rule, const Stylesheet& sheet, uint32_t sheetIndex)
{
    return MatchRec{&rule, sheet.Origin, rule.Selector.Spec, sheetIndex};
}

#if GE_DEBUG_INSTRUMENTATION
// Walks every stylesheet's candidate rules with the *live* matcher
// (dynamic pseudos resolved against ElementState), mirroring the
// fallback path in ComputeStyleInto when the rule cache is disabled.
// Used only by ValidateRuleCacheAgainstLive.
void CollectLiveMatchesForValidator(const UIElement& el,
                                    std::span<const Stylesheet* const> sheets,
                                    std::span<const StylesheetRuleIndex* const> sheetIndices,
                                    const ElementState& state,
                                    const UIElement* hoverTarget,
                                    std::vector<MatchRec>& out)
{
    if (!sheetIndices.empty() && sheetIndices.size() != sheets.size())
        sheetIndices = {};

    for (uint32_t si = 0; si < sheets.size(); ++si)
    {
        const Stylesheet* s = sheets[si];
        if (!s)
            continue;

        const StylesheetRuleIndex* idx = nullptr;
        if (!sheetIndices.empty() && si < sheetIndices.size())
            idx = sheetIndices[si];

        if (idx)
        {
            static thread_local std::vector<uint32_t> tl_validatorCandidates;
            tl_validatorCandidates.clear();
            auto& candidates = tl_validatorCandidates;
            candidates.reserve(idx->Universal.size() + 8);

            const StringId idHash = el.GetIdHash();
            if (idHash != 0)
            {
                auto it = idx->ById.find(idHash);
                if (it != idx->ById.end())
                    candidates.insert(candidates.end(), it->second.begin(), it->second.end());
            }
            for (StringId clsId : el.GetClassIds())
            {
                auto it = idx->ByClass.find(clsId);
                if (it != idx->ByClass.end())
                    candidates.insert(candidates.end(), it->second.begin(), it->second.end());
            }
            {
                StringId elTagId = UIRegistration::ElementFactoryRegistry::Instance().GetElementTagId(el);
                if (elTagId)
                {
                    auto it = idx->ByTag.find(elTagId);
                    if (it != idx->ByTag.end())
                        candidates.insert(candidates.end(), it->second.begin(), it->second.end());
                }
            }
            candidates.insert(candidates.end(), idx->Universal.begin(), idx->Universal.end());
            std::sort(candidates.begin(), candidates.end());
            candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());

            for (uint32_t ri : candidates)
            {
                if (ri >= s->Rules.size())
                    continue;
                const auto& rule = s->Rules[ri];
                if (MatchesSelectorChain(el, rule.Selector, state, &el, hoverTarget))
                    out.push_back(MakeMatch(rule, *s, si));
            }
        }
        else
        {
            for (const auto& rule : s->Rules)
            {
                if (MatchesSelectorChain(el, rule.Selector, state, &el, hoverTarget))
                    out.push_back(MakeMatch(rule, *s, si));
            }
        }
    }
}

// Compares the cached cascade's match set against a fresh live walk and
// logs each rule that appears in one but not the other. Increments the
// global divergence counter once per divergent rule. Called from
// ComputeStyleInto when GE_UI_RULE_CACHE_VALIDATE is on.
//
// MISSING-FROM-CACHE: live matched a rule the cache didn't surface →
// stale cache, invalidation didn't fire on a structural change that
// added a match. This is the high-priority bug class — the cascade is
// silently dropping styles that should apply.
//
// EXTRA-IN-CACHE: cache surfaced a rule the live matcher rejects →
// stale cache, invalidation didn't fire on a structural change that
// removed a match. Manifests as "old" styles persisting after a
// mutation.
void ValidateRuleCacheAgainstLive(const UIElement& el,
                                  std::span<const Stylesheet* const> sheets,
                                  std::span<const StylesheetRuleIndex* const> sheetIndices,
                                  const ElementState& state,
                                  const UIElement* hoverTarget,
                                  std::span<const MatchRec> cachedMatches)
{
    static thread_local std::vector<MatchRec> tl_liveMatches;
    tl_liveMatches.clear();
    CollectLiveMatchesForValidator(el, sheets, sheetIndices, state, hoverTarget, tl_liveMatches);

    auto ruleIndexInSheet = [&](const CSSRule* r, uint32_t si) -> int {
        if (si >= sheets.size() || !sheets[si])
            return -1;
        const auto& rules = sheets[si]->Rules;
        if (rules.empty())
            return -1;
        const auto* base = rules.data();
        if (r < base || r >= base + rules.size())
            return -1;
        return static_cast<int>(r - base);
    };

    for (const MatchRec& live : tl_liveMatches)
    {
        bool foundInCache = false;
        for (const MatchRec& cached : cachedMatches)
        {
            if (cached.Rule == live.Rule)
            {
                foundInCache = true;
                break;
            }
        }
        if (!foundInCache)
        {
            s_RuleCacheValidationDivergences.fetch_add(1, std::memory_order_relaxed);
            const int ri = ruleIndexInSheet(live.Rule, live.SheetIndex);
            Logger::Log::Warning(
                "UI rule cache validator MISSING-FROM-CACHE: element id='{}' instance={} sheet={} rule_index={} (live matched, cache did not — invalidation gap)",
                el.GetId(), (uint64_t)el.GetInstanceId(), live.SheetIndex, ri);
        }
    }

    for (const MatchRec& cached : cachedMatches)
    {
        bool foundInLive = false;
        for (const MatchRec& live : tl_liveMatches)
        {
            if (live.Rule == cached.Rule)
            {
                foundInLive = true;
                break;
            }
        }
        if (!foundInLive)
        {
            s_RuleCacheValidationDivergences.fetch_add(1, std::memory_order_relaxed);
            const int ri = ruleIndexInSheet(cached.Rule, cached.SheetIndex);
            Logger::Log::Warning(
                "UI rule cache validator EXTRA-IN-CACHE: element id='{}' instance={} sheet={} rule_index={} (cache applied a rule the live matcher rejects — stale structural state)",
                el.GetId(), (uint64_t)el.GetInstanceId(), cached.SheetIndex, ri);
        }
    }
}
#endif // GE_DEBUG_INSTRUMENTATION
} // namespace

void CSSParser::ComputeStyleInto(ResolvedStyle& out,
                                 const UIElement& el,
                                 std::span<const Stylesheet* const> sheets,
                                 std::span<const StylesheetRuleIndex* const> sheetIndices,
                                 const ElementState& state,
                                 const ResolvedStyle* parentStyle,
                                 const UIElement* hoverTarget,
                                 uint16_t* outMatchedPseudoStates)
{
    // Stage 5 Block B: pseudo-state predicate accumulator. Walked alongside
    // the matched-rule application loop below so we observe every pseudo
    // referenced by every matched rule's selector chain. Reset at function
    // entry so callers always get a fresh result; bits are only set when
    // outMatchedPseudoStates != nullptr to keep the OR cheap when unused.
    uint16_t matchedPseudoBits = 0;
    if (!sheetIndices.empty() && sheetIndices.size() != sheets.size())
    {
        sheetIndices = {};
    }

    static const ResolvedStyle kInitial{};
    out = kInitial;

    constexpr size_t kKindCount = (size_t)StylePropertyId::DeferredDecl + 1;
    std::bitset<kKindCount> specified;

    std::shared_ptr<const CustomPropertyScope> parentScope = parentStyle ? parentStyle->CustomScope : nullptr;
    static thread_local std::unordered_map<StringId, std::string> tl_customVars;
    tl_customVars.clear();
    auto& customVars = tl_customVars;
    if (parentScope && !parentScope->Local.Empty())
        customVars.reserve(parentScope->Local.Size());
    std::shared_ptr<CustomPropertyScope> localScope;

    auto ensureLocalScope = [&]()
    {
        if (!localScope)
        {
            localScope = std::make_shared<CustomPropertyScope>();
            localScope->Parent = parentScope;
            localScope->UniqueId = s_CustomScopeUniqueId.fetch_add(1, std::memory_order_relaxed);
            if (parentScope && !parentScope->Local.Empty())
                localScope->Local.Reserve(parentScope->Local.Size());
        }
    };

    // !important lock set for custom-property declarations. Because custom
    // vars are applied eagerly during the matches walk (so subsequent
    // DeferredDecl resolutions see them), we cannot defer them like
    // phase2Refs. Instead, when an `!important` --foo declaration applies,
    // we lock the varId; subsequent non-important declarations of the same
    // var are skipped. Within the important tier, last-wins still applies
    // because the matches walk is already in (spec, order) ascending order.
    static thread_local std::unordered_set<StringId> tl_importantVarIds;
    tl_importantVarIds.clear();
    auto& importantVarIds = tl_importantVarIds;

    auto applyCustomVar = [&](const StyleProperty& prop)
    {
        if (prop.PropertyId != StylePropertyId::CustomVar)
            return;
        const auto& custom = GetValue<CustomVarDecl>(prop);
        if (custom.Name.empty())
            return;

        const std::string varName = custom.Name;
        const StringId varId = HashStringId(varName);

        if (!prop.Important && importantVarIds.count(varId))
            return;

        const std::string rawValue = custom.Value;
        const StyleKeyword kw = ParseStyleKeyword(rawValue);

        ensureLocalScope();
        auto& slot = localScope->Local.GetOrInsert(varId);

        if (prop.Important)
            importantVarIds.insert(varId);

        if (kw == StyleKeyword::Initial)
        {
            slot.Invalid = true;
            slot.Value.clear();
            customVars.erase(varId);
            return;
        }

        if (kw == StyleKeyword::Inherit || kw == StyleKeyword::Unset)
        {
            const CustomPropertyScope::Entry* parentVal = nullptr;
            CustomPropertyScope::LookupStatus st = CustomPropertyScope::LookupStatus::Missing;
            if (parentScope)
            {
                st = parentScope->Lookup(varId, parentVal);
            }
            if (st == CustomPropertyScope::LookupStatus::Found && parentVal)
            {
                slot.Invalid = false;
                slot.Value = parentVal->Value;
                customVars[varId] = slot.Value;
            }
            else
            {
                slot.Invalid = true;
                slot.Value.clear();
                customVars.erase(varId);
            }
            return;
        }

        slot.Invalid = false;
        slot.Value = rawValue;
        customVars[varId] = rawValue;
    };

    static thread_local std::vector<const StyleProperty*> tl_phase2Refs;
    tl_phase2Refs.clear();
    auto& phase2Refs = tl_phase2Refs;

    static thread_local std::string tl_resolvedValue;
    auto& resolvedValue = tl_resolvedValue;

    static thread_local std::vector<std::string> tl_varStack;
    auto& varStack = tl_varStack;

    static thread_local std::vector<StyleProperty> tl_tmpProps;
    auto& tmpProps = tl_tmpProps;

    struct DeferredMemoEntry
    {
        const CustomPropertyScope* Scope = nullptr;
        uint64_t ScopeId = 0;
        std::string PropName;
        std::string RawValue;
        bool Valid = false;
        std::vector<StyleProperty> Props;
    };

    // Deferred-decl resolution memo.
    //
    // Previously: a plain unordered_map<key, bucket> with a "clear everything
    // when entry count >= 4096" policy. That meant every ~4096 cache misses
    // would throw away the whole cache and rebuild from scratch — a heap-churn
    // pattern that showed up in VS Diagnostic Tools as ~100 KB of allocations
    // per snapshot. Worse, it lost ALL hot entries on each clear.
    //
    // Now: a proper LRU. A std::list orders buckets by use (front = most
    // recently used). The map indexes by key into a list iterator. On
    // overflow we evict the LRU bucket (back of list), one bucket at a time,
    // until total entry count is under the cap. Hot entries survive churn.
    struct DeferredMemoBucket
    {
        uint64_t                        Key;
        std::vector<DeferredMemoEntry>  Entries;
    };
    static thread_local std::list<DeferredMemoBucket> tl_deferredMemoLru;
    static thread_local std::unordered_map<uint64_t, std::list<DeferredMemoBucket>::iterator> tl_deferredMemoIndex;
    static thread_local size_t tl_deferredMemoEntryCount = 0;
    constexpr size_t kMaxDeferredMemoEntries = 4096;

    auto deferredMemoEvictLru = [&]()
    {
        while (tl_deferredMemoEntryCount > kMaxDeferredMemoEntries
               && !tl_deferredMemoLru.empty())
        {
            auto& victim = tl_deferredMemoLru.back();
            tl_deferredMemoEntryCount -= victim.Entries.size();
            tl_deferredMemoIndex.erase(victim.Key);
            tl_deferredMemoLru.pop_back();
        }
    };

    auto hashCombine64 = [](uint64_t& h, uint64_t v)
    {
        h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    };
    auto hashString64 = [](std::string_view s) -> uint64_t
    {
        uint64_t h = 1469598103934665603ULL;
        for (unsigned char c : s)
        {
            h ^= (uint64_t)c;
            h *= 1099511628211ULL;
        }
        return h;
    };
    auto deferredMemoKey = [&](const CustomPropertyScope* scope,
                               const std::string& propName,
                               const std::string& rawValue) -> uint64_t
    {
        uint64_t h = 0;
        const uint64_t scopeId = scope ? scope->UniqueId : 0;
        hashCombine64(h, scopeId);
        hashCombine64(h, hashString64(propName));
        hashCombine64(h, hashString64(rawValue));
        return h;
    };

    auto applyOneResolved = [&](const StyleProperty& prop)
    {
        if (prop.PropertyId == StylePropertyId::CustomVar)
            return;

        if (prop.PropertyId == StylePropertyId::DeferredDecl)
        {
            const auto& deferred = GetValue<DeferredDeclValue>(prop);
            const CustomPropertyScope* scopePtr = out.CustomScope.get();
            const uint64_t scopeId = scopePtr ? scopePtr->UniqueId : 0;
            const uint64_t key = deferredMemoKey(scopePtr, deferred.Name, deferred.Value);

            // Note: entries in `propsToApply` always have `important=false`
            // even when the parent DeferredDecl was `!important`. The memo
            // is keyed by (scope, propName, rawValue) and shared across
            // important / non-important callers; the parent's importance
            // is already honored at the phase2Refs partition level, which
            // determines apply order. Downstream Apply* helpers do not
            // read `p2.important`, so no flag propagation is needed here.
            auto applyParsedList = [&](const std::vector<StyleProperty>& propsToApply)
            {
                for (const auto& p2 : propsToApply)
                {
                    if (p2.PropertyId == StylePropertyId::CustomVar)
                        continue;
                    const size_t kindIdx2 = (size_t)p2.PropertyId;
                    if (kindIdx2 < kKindCount)
                        specified.set(kindIdx2);
                    if (p2.Keyword != StyleKeyword::None)
                        ApplyStyleKeyword(out, p2.PropertyId, p2.Keyword, kInitial, parentStyle);
                    else
                        ApplyProperty(out, customVars, p2);
                }
            };

            // Cache lookup: find the bucket for this key, then linearly check
            // entries inside (rare hash collisions). On hit, promote the bucket
            // to MRU.
            if (auto idxIt = tl_deferredMemoIndex.find(key); idxIt != tl_deferredMemoIndex.end())
            {
                auto bucketIt = idxIt->second;
                for (const auto& e : bucketIt->Entries)
                {
                    if (e.Scope == scopePtr && e.ScopeId == scopeId && e.PropName == deferred.Name && e.RawValue == deferred.Value)
                    {
                        // Cache hit: mark bucket as MRU and return.
                        tl_deferredMemoLru.splice(tl_deferredMemoLru.begin(), tl_deferredMemoLru, bucketIt);
                        if (!e.Valid)
                            return;
                        applyParsedList(e.Props);
                        return;
                    }
                }
                // Bucket exists but no matching entry — fall through to insert
                // a new entry into the existing bucket. Still bump to MRU.
                tl_deferredMemoLru.splice(tl_deferredMemoLru.begin(), tl_deferredMemoLru, bucketIt);
            }

            resolvedValue.clear();
            varStack.clear();
            DeferredMemoEntry entry{};
            entry.Scope = scopePtr;
            entry.ScopeId = scopeId;
            entry.PropName = deferred.Name;
            entry.RawValue = deferred.Value;

            if (!ResolveVarFunctions(customVars, parentScope.get(), deferred.Value, resolvedValue, /*depth=*/0, varStack))
            {
                entry.Valid = false;
            }
            else
            {
                tmpProps.clear();
                AppendDeclarationProperties(deferred.Name, resolvedValue, tmpProps);
                entry.Valid = true;
                entry.Props = tmpProps;
            }

            // Locate or create bucket for this key. The lookup above may have
            // already promoted an existing bucket to MRU; if it didn't exist
            // we create at front of LRU.
            std::list<DeferredMemoBucket>::iterator bucketIt;
            if (auto idxIt = tl_deferredMemoIndex.find(key); idxIt != tl_deferredMemoIndex.end())
            {
                bucketIt = idxIt->second;
            }
            else
            {
                tl_deferredMemoLru.push_front(DeferredMemoBucket{key, {}});
                bucketIt = tl_deferredMemoLru.begin();
                tl_deferredMemoIndex.emplace(key, bucketIt);
            }

            bucketIt->Entries.push_back(std::move(entry));
            ++tl_deferredMemoEntryCount;

            // Evict LRU buckets until under cap. Preserves hot entries that
            // the previous clear-everything approach would have lost.
            deferredMemoEvictLru();

            if (!bucketIt->Entries.empty())
            {
                const auto& back = bucketIt->Entries.back();
                if (!back.Valid)
                    return;
                applyParsedList(back.Props);
            }
            return;
        }

        const size_t kindIdx = (size_t)prop.PropertyId;
        if (kindIdx < kKindCount)
            specified.set(kindIdx);
        if (prop.Keyword != StyleKeyword::None)
            ApplyStyleKeyword(out, prop.PropertyId, prop.Keyword, kInitial, parentStyle);
        else
            ApplyProperty(out, customVars, prop);
    };

    static thread_local std::vector<MatchRec> tl_matches;
    tl_matches.clear();
    auto& matches = tl_matches;
    size_t totalRuleCount = 0;
    for (const Stylesheet* s : sheets)
    {
        if (s)
            totalRuleCount += s->Rules.size();
    }
    matches.reserve(totalRuleCount);

    // Cascade-memoization Phase 4: gated on GE_UI_RULE_CACHE env var
    // (read once at process start via IsRuleCacheEnabled). When enabled,
    // populate the per-element structural rule cache on demand, then
    // iterate cached rules instead of walking every stylesheet's
    // candidates. For rules that contain no dynamic pseudos (purely
    // structural), the cache hit applies the rule unconditionally —
    // saving the entire selector chain walk. For rules that DO reference
    // dynamic pseudos, we re-run the live matcher against the current
    // ElementState (handles `.parent:hover .child`, `.foo:focus`, etc.
    // correctly) — which is still cheaper than walking every candidate,
    // because the cache has already filtered down to structural-match
    // candidates.
    //
    // Cache invalidation (Phase 2) handles every mutation that affects
    // the structural match: class/id changes, tree mutations, stylesheet
    // attach/detach, Mount target swaps. Cache miss falls back to the
    // legacy walk + builds the cache as a side effect.
    if (CSSParser::IsRuleCacheEnabled())
    {
        if (!el.IsRuleCacheValid())
            BuildRuleCache(el, sheets, sheetIndices);

        // Cascade-memoization Phase 5: accumulate the dynamic-pseudo
        // predicate from EVERY cached rule (not just filtered matches).
        // The cache is structural — rules with `:hover` are present
        // whether or not the element is currently hovered. This makes
        // m_MatchedPseudoStates sound for push-time narrowing: an element
        // with no `HoverRules` bit literally has no rule that could
        // change style on hover, so the dirty-mark is safe to skip.
        //
        // The matches-application loop below adds the same bits a second
        // time (a no-op OR), but only for filtered matches. The cached-
        // rule walk here is the load-bearing one for narrowing.
        const bool wantPredicate = (outMatchedPseudoStates != nullptr);

        for (const UIElement::CachedMatchedRule& cm : el.GetCachedMatchedRules())
        {
            if (!cm.rule)
                continue;
            if (wantPredicate && cm.hasDynamicPseudo)
            {
                for (const SelectorTerm& term : cm.rule->Selector.Terms)
                {
                    for (const PseudoClass& pseudo : term.Selector.Pseudos)
                    {
                        switch (pseudo.PseudoKind)
                        {
                            case PseudoClass::Kind::Hover:        matchedPseudoBits |= UIElement::HoverRules;        break;
                            case PseudoClass::Kind::Active:       matchedPseudoBits |= UIElement::ActiveRules;       break;
                            case PseudoClass::Kind::Focus:        matchedPseudoBits |= UIElement::FocusRules;        break;
                            case PseudoClass::Kind::FocusVisible: matchedPseudoBits |= UIElement::FocusVisibleRules; break;
                            case PseudoClass::Kind::FocusWithin:  matchedPseudoBits |= UIElement::FocusWithinRules;  break;
                            case PseudoClass::Kind::Disabled:     matchedPseudoBits |= UIElement::DisabledRules;     break;
                            case PseudoClass::Kind::Enabled:      matchedPseudoBits |= UIElement::EnabledRules;      break;
                            case PseudoClass::Kind::Checked:      matchedPseudoBits |= UIElement::CheckedRules;      break;
                            case PseudoClass::Kind::Custom:       matchedPseudoBits |= UIElement::CustomStateRules;  break;
                            default: break;
                        }
                    }
                }
            }
            if (cm.hasDynamicPseudo)
            {
                if (!MatchesSelectorChain(el, cm.rule->Selector, state, &el, hoverTarget))
                    continue;
            }
            matches.push_back(MatchRec{cm.rule, cm.origin, cm.rule->Selector.Spec, cm.sheetIndex});
        }

#if GE_DEBUG_INSTRUMENTATION
        // Cache-correctness validator (debug-only, opt-in via
        // GE_UI_RULE_CACHE_VALIDATE). Walks the live matcher in parallel
        // and logs any rule the two paths disagree on. Adds substantial
        // runtime cost — disabled in release and off by default.
        if (CSSParser::IsRuleCacheValidationEnabled())
        {
            ValidateRuleCacheAgainstLive(
                el, sheets, sheetIndices, state, hoverTarget,
                std::span<const MatchRec>(matches.data(), matches.size()));
        }
#endif
    }
    else
    for (uint32_t si = 0; si < sheets.size(); ++si)
    {
        const Stylesheet* s = sheets[si];
        if (!s)
            continue;

        const StylesheetRuleIndex* idx = nullptr;
        if (!sheetIndices.empty() && si < sheetIndices.size())
        {
            idx = sheetIndices[si];
        }

        if (idx)
        {
            static thread_local std::vector<uint32_t> tl_candidates;
            tl_candidates.clear();
            auto& candidates = tl_candidates;
            candidates.reserve(idx->Universal.size() + 8);

            const StringId idHash = el.GetIdHash();
            if (idHash != 0)
            {
                auto it = idx->ById.find(idHash);
                if (it != idx->ById.end())
                {
                    candidates.insert(candidates.end(), it->second.begin(), it->second.end());
                }
            }

            for (StringId clsId : el.GetClassIds())
            {
                auto it = idx->ByClass.find(clsId);
                if (it != idx->ByClass.end())
                {
                    candidates.insert(candidates.end(), it->second.begin(), it->second.end());
                }
            }

            {
                StringId elTagId = UIRegistration::ElementFactoryRegistry::Instance().GetElementTagId(el);
                if (elTagId)
                {
                    auto it = idx->ByTag.find(elTagId);
                    if (it != idx->ByTag.end())
                    {
                        candidates.insert(candidates.end(), it->second.begin(), it->second.end());
                    }
                }
            }

            candidates.insert(candidates.end(), idx->Universal.begin(), idx->Universal.end());

            std::sort(candidates.begin(), candidates.end());
            candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());

            for (uint32_t ri : candidates)
            {
                if (ri >= s->Rules.size())
                    continue;
                const auto& rule = s->Rules[ri];
                if (MatchesSelectorChain(el, rule.Selector, state, &el, hoverTarget))
                {
                    matches.push_back(MakeMatch(rule, *s, si));
                }
            }
        }
        else
        {
            for (const auto& rule : s->Rules)
            {
                if (MatchesSelectorChain(el, rule.Selector, state, &el, hoverTarget))
                {
                    matches.push_back(MakeMatch(rule, *s, si));
                }
            }
        }
    }

    // Cascade order, lowest first: origin (user-agent under author), then specificity, then sheet
    // order, then source order within a sheet.
    std::stable_sort(matches.begin(), matches.end(), [](const MatchRec& a, const MatchRec& b)
                     {
	        if (a.Origin != b.Origin)
	            return a.Origin < b.Origin;
	        if (auto c = a.Spec <=> b.Spec; c != 0)
	            return c < 0;
	        if (a.SheetIndex != b.SheetIndex)
	            return a.SheetIndex < b.SheetIndex;
	        return a.Rule->Order < b.Rule->Order; });

    for (const MatchRec& m : matches)
    {
        for (const auto& prop : m.Rule->Properties)
        {
            if (prop.PropertyId == StylePropertyId::CustomVar)
                applyCustomVar(prop);
            else
                phase2Refs.push_back(&prop);
        }

        // Stage 5 Block B: accumulate pseudo-state predicate bits. Every
        // pseudo-class referenced anywhere in this matched rule's selector
        // chain contributes a bit. The whole walk is bounded by the rule's
        // complexity (typically 1-3 terms × 0-2 pseudos) and skipped if
        // the caller didn't ask for the output.
        if (outMatchedPseudoStates)
        {
            for (const SelectorTerm& term : m.Rule->Selector.Terms)
            {
                for (const PseudoClass& pseudo : term.Selector.Pseudos)
                {
                    switch (pseudo.PseudoKind)
                    {
                        case PseudoClass::Kind::Hover:        matchedPseudoBits |= UIElement::HoverRules;        break;
                        case PseudoClass::Kind::Active:       matchedPseudoBits |= UIElement::ActiveRules;       break;
                        case PseudoClass::Kind::Focus:        matchedPseudoBits |= UIElement::FocusRules;        break;
                        case PseudoClass::Kind::FocusVisible: matchedPseudoBits |= UIElement::FocusVisibleRules; break;
                        case PseudoClass::Kind::FocusWithin:  matchedPseudoBits |= UIElement::FocusWithinRules;  break;
                        case PseudoClass::Kind::Disabled:     matchedPseudoBits |= UIElement::DisabledRules;     break;
                        case PseudoClass::Kind::Enabled:      matchedPseudoBits |= UIElement::EnabledRules;      break;
                        case PseudoClass::Kind::Checked:      matchedPseudoBits |= UIElement::CheckedRules;      break;
                        case PseudoClass::Kind::Custom:       matchedPseudoBits |= UIElement::CustomStateRules;  break;
                        default: break; // structural / logical pseudos: not predicate-relevant
                    }
                }
            }
        }
    }

    el.InlineOverrides().ForEachCustom([&](StringId varId, const std::string& value)
    {
        ensureLocalScope();
        auto& slot = localScope->Local.GetOrInsert(varId);
        slot.Invalid = false;
        slot.Value = value;
        customVars[varId] = value;
    });

    el.Overrides().ForEachCustom([&](StringId varId, const std::string& value)
    {
        ensureLocalScope();
        auto& slot = localScope->Local.GetOrInsert(varId);
        slot.Invalid = false;
        slot.Value = value;
        customVars[varId] = value;
    });

    if (localScope)
        out.CustomScope = localScope;
    else
        out.CustomScope = parentScope;

    // !important cascade tier: normal-priority declarations run first, then
    // important-priority declarations run last so they overwrite. Within each
    // tier, the walk order is already (origin, specificity, sheetIndex, source order)
    // ascending from the stable_sort on `matches` above, so last-wins
    // semantics within a tier are preserved. stable_partition keeps that
    // intra-tier order while moving !important refs to the tail. CSS reverses
    // origin order for important declarations; nothing here needs to, because
    // the only user-agent sheet has none (UI/Source/DefaultStylesheet.cpp).
    //
    // Fast path: most elements match only normal-priority declarations.
    // Scan once to detect any !important ref; if there are none, skip the
    // partition entirely (saves the predicate scan + any STL bookkeeping).
    bool anyImportant = false;
    for (const StyleProperty* p : phase2Refs)
    {
        if (p && p->Important) { anyImportant = true; break; }
    }
    if (anyImportant)
    {
        std::stable_partition(phase2Refs.begin(), phase2Refs.end(),
            [](const StyleProperty* p) { return p && !p->Important; });
    }

    for (const StyleProperty* p : phase2Refs)
    {
        if (p)
            applyOneResolved(*p);
    }

    if (parentStyle)
    {
        for (size_t ki = 0; ki < kKindCount; ++ki)
        {
            if (specified.test(ki))
                continue;
            const auto kind = static_cast<StylePropertyId>(ki);
            if (IsInheritedByDefault(kind))
                CopyPropertyFromStyle(out, *parentStyle, kind, false);
        }
    }

    // A percentage font-size computes against the parent's computed size, which
    // is only settled now that every declaration and the default-inherit pass
    // have run. The root has no parent and takes the initial value as its base.
    // Undeclared font-size is already handled above by the inherit pass, so this
    // only finishes the percentage case.
    ResolveFontSize(out.Visual, parentStyle ? parentStyle->Visual.FontSize : kInitialFontSizePx);

    out.Visual.Opacity = out.Visual.LocalOpacity;

    // Stage 5 Block B: emit the accumulated pseudo-state predicate. The
    // caller (ResolveCascadeForElement) writes this onto the element so
    // mark-dirty sites can skip pushes for elements unaffected by a given
    // pseudo-state flip.
    if (outMatchedPseudoStates)
        *outMatchedPseudoStates = matchedPseudoBits;
}

ResolvedStyle CSSParser::ComputeStyleFor(const UIElement& el,
                                         std::span<const Stylesheet* const> sheets,
                                         std::span<const StylesheetRuleIndex* const> sheetIndices,
                                         const ElementState& state,
                                         const ResolvedStyle* parentStyle,
                                         const UIElement* hoverTarget)
{
    ResolvedStyle rs;
    ComputeStyleInto(rs, el, sheets, sheetIndices, state, parentStyle, hoverTarget);
    return rs;
}

bool CSSParser::IsRuleCacheEnabled()
{
    // Cascade-memoization Phase 7 finale: cache + Phase 5 narrowing are
    // the production cascade path. Default ON; emergency kill-switch via
    // GE_UI_RULE_CACHE=0 (or "false"/"off"). Read once at process start
    // because the gate threads through ComputeStyleInto, the narrowing
    // call sites in DispatchEvents/RebuildFocusWithinChain/AddCustomState/
    // MarkHoverTransitionDirty, and the cache-validity invariant holds
    // for the lifetime of the process. Toggling mid-run would leave
    // m_MatchedPseudoStates populated from one path while the other
    // expects the other path's semantics — not worth supporting.
    static const bool s_Enabled = []() {
        if (const char* e = std::getenv("GE_UI_RULE_CACHE"))
        {
            std::string v(e);
            std::transform(v.begin(), v.end(), v.begin(),
                           [](unsigned char c) { return (char)std::tolower(c); });
            if (v == "0" || v == "false" || v == "off")
                return false;
        }
        return true;
    }();
    return s_Enabled;
}

#if GE_DEBUG_INSTRUMENTATION
bool CSSParser::IsRuleCacheValidationEnabled()
{
    const int forced = s_RuleCacheValidationOverride.load(std::memory_order_relaxed);
    if (forced >= 0)
        return forced != 0;
    static const bool s_FromEnv = []() {
        if (const char* e = std::getenv("GE_UI_RULE_CACHE_VALIDATE"))
        {
            std::string v(e);
            std::transform(v.begin(), v.end(), v.begin(),
                           [](unsigned char c) { return (char)std::tolower(c); });
            if (v.empty() || v == "0" || v == "false" || v == "off")
                return false;
            return true;
        }
        return false;
    }();
    return s_FromEnv;
}

void CSSParser::SetRuleCacheValidationEnabledForTest(bool enabled)
{
    s_RuleCacheValidationOverride.store(enabled ? 1 : 0, std::memory_order_relaxed);
}

void CSSParser::ClearRuleCacheValidationOverrideForTest()
{
    s_RuleCacheValidationOverride.store(-1, std::memory_order_relaxed);
}

uint64_t CSSParser::GetRuleCacheValidationDivergenceCount()
{
    return s_RuleCacheValidationDivergences.load(std::memory_order_relaxed);
}

void CSSParser::ResetRuleCacheValidationDivergenceCount()
{
    s_RuleCacheValidationDivergences.store(0, std::memory_order_relaxed);
}
#endif

void CSSParser::BuildRuleCache(const UIElement& el,
                               std::span<const Stylesheet* const> sheets,
                               std::span<const StylesheetRuleIndex* const> sheetIndices)
{
    auto& cache = el.m_CachedMatchedRules;
    cache.clear();

    // An empty sheet pool cannot prove that no rule matches this element; it
    // only says the caller had no sheets to offer. A valid "nothing matches"
    // cache would be trusted by every later cascade, including the ones that
    // carry the real sheets, until the manager's rule-cache epoch advances.
    if (sheets.empty())
    {
        el.m_RuleCacheValid = false;
        return;
    }

    el.m_RuleCacheValid = true;
    // Stamp the manager epoch so a sheet-content change while this element
    // is detached (outside the invalidation walk) marks the cache stale on
    // the next IsRuleCacheValid check instead of dereferencing freed rules.
    el.m_RuleCacheEpoch = UIManagerRuleCacheEpoch(el.GetOwnerManager());

    if (!sheetIndices.empty() && sheetIndices.size() != sheets.size())
        sheetIndices = {};

    // Mirror the candidate-collection logic in ComputeStyleInto: when a
    // rule index is available, narrow by id/class/tag/universal first,
    // then dedup. Otherwise iterate every rule. Then run the structural
    // matcher (no live ElementState — dynamic pseudos resolve to true).
    for (uint32_t si = 0; si < sheets.size(); ++si)
    {
        const Stylesheet* s = sheets[si];
        if (!s)
            continue;

        const StylesheetRuleIndex* idx = nullptr;
        if (!sheetIndices.empty() && si < sheetIndices.size())
            idx = sheetIndices[si];

        if (idx)
        {
            static thread_local std::vector<uint32_t> tl_candidates;
            tl_candidates.clear();
            auto& candidates = tl_candidates;
            candidates.reserve(idx->Universal.size() + 8);

            const StringId idHash = el.GetIdHash();
            if (idHash != 0)
            {
                auto it = idx->ById.find(idHash);
                if (it != idx->ById.end())
                    candidates.insert(candidates.end(), it->second.begin(), it->second.end());
            }
            for (StringId clsId : el.GetClassIds())
            {
                auto it = idx->ByClass.find(clsId);
                if (it != idx->ByClass.end())
                    candidates.insert(candidates.end(), it->second.begin(), it->second.end());
            }
            {
                StringId elTagId = UIRegistration::ElementFactoryRegistry::Instance().GetElementTagId(el);
                if (elTagId)
                {
                    auto it = idx->ByTag.find(elTagId);
                    if (it != idx->ByTag.end())
                        candidates.insert(candidates.end(), it->second.begin(), it->second.end());
                }
            }
            candidates.insert(candidates.end(), idx->Universal.begin(), idx->Universal.end());
            std::sort(candidates.begin(), candidates.end());
            candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());

            for (uint32_t ri : candidates)
            {
                if (ri >= s->Rules.size())
                    continue;
                const auto& rule = s->Rules[ri];
                if (MatchesSelectorChainStructural(el, rule.Selector))
                {
                    cache.push_back(UIElement::CachedMatchedRule{
                        &rule,
                        static_cast<uint16_t>(si),
                        s->Origin,
                        HasDynamicPseudoInChain(rule.Selector),
                    });
                }
            }
        }
        else
        {
            for (const auto& rule : s->Rules)
            {
                if (MatchesSelectorChainStructural(el, rule.Selector))
                {
                    cache.push_back(UIElement::CachedMatchedRule{
                        &rule,
                        static_cast<uint16_t>(si),
                        s->Origin,
                        HasDynamicPseudoInChain(rule.Selector),
                    });
                }
            }
        }
    }
}

ResolvedStyle CSSParser::ComputeStyleFor(const UIElement& el,
                                         const std::vector<const Stylesheet*>& sheets,
                                         const std::vector<const StylesheetRuleIndex*>& sheetIndices,
                                         const ElementState& state,
                                         const ResolvedStyle* parentStyle,
                                         const UIElement* hoverTarget)
{
    return ComputeStyleFor(
        el,
        std::span<const Stylesheet* const>(sheets.data(), sheets.size()),
        std::span<const StylesheetRuleIndex* const>(sheetIndices.data(), sheetIndices.size()),
        state,
        parentStyle,
        hoverTarget);
}

} // namespace UIParsing
} // namespace GameEngine
