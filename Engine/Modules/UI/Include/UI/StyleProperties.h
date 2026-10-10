#pragma once

#include "UI/StyleProp.h"
#include "UI/UIStyle.h"

namespace GameEngine::Style
{

// Layout properties (affect Yoga)
constexpr StyleProp<DisplayMode> Display{StylePropertyId::Display};
constexpr StyleProp<FlexDirection> FlexDir{StylePropertyId::FlexDir};
constexpr StyleProp<bool> FlexWrap{StylePropertyId::FlexWrap};
constexpr StyleProp<::GameEngine::AlignItems> AlignItems{StylePropertyId::AlignItems};
constexpr StyleProp<::GameEngine::JustifyContent> JustifyContent{StylePropertyId::JustifyContent};
constexpr StyleProp<::GameEngine::AlignContent> AlignContent{StylePropertyId::AlignContent};
constexpr StyleProp<::GameEngine::AlignItems> AlignSelf{StylePropertyId::AlignSelf};
constexpr StyleProp<::GameEngine::Direction> Direction{StylePropertyId::Direction};
constexpr StyleProp<PositionType> Position{StylePropertyId::Position};
constexpr StyleProp<StyleLength> PositionLeft{StylePropertyId::PositionLeft};
constexpr StyleProp<StyleLength> PositionTop{StylePropertyId::PositionTop};
constexpr StyleProp<StyleLength> PositionRight{StylePropertyId::PositionRight};
constexpr StyleProp<StyleLength> PositionBottom{StylePropertyId::PositionBottom};
constexpr StyleProp<int> Order{StylePropertyId::Order};
constexpr StyleProp<int> ZIndex{StylePropertyId::ZIndex};
constexpr StyleProp<float> FlexGrow{StylePropertyId::FlexGrow};
constexpr StyleProp<float> FlexShrink{StylePropertyId::FlexShrink};
constexpr StyleProp<StyleLength> FlexBasis{StylePropertyId::FlexBasis};
constexpr StyleProp<StyleLength> Width{StylePropertyId::Width};
constexpr StyleProp<StyleLength> Height{StylePropertyId::Height};
constexpr StyleProp<StyleLength> MinWidth{StylePropertyId::MinWidth};
constexpr StyleProp<StyleLength> MinHeight{StylePropertyId::MinHeight};
constexpr StyleProp<StyleLength> MaxWidth{StylePropertyId::MaxWidth};
constexpr StyleProp<StyleLength> MaxHeight{StylePropertyId::MaxHeight};
constexpr StyleProp<float> AspectRatio{StylePropertyId::AspectRatio};
constexpr StyleProp<StyleBox> Margin{StylePropertyId::Margin};
constexpr StyleProp<StyleLength> MarginTop{StylePropertyId::MarginTop};
constexpr StyleProp<StyleLength> MarginRight{StylePropertyId::MarginRight};
constexpr StyleProp<StyleLength> MarginBottom{StylePropertyId::MarginBottom};
constexpr StyleProp<StyleLength> MarginLeft{StylePropertyId::MarginLeft};
constexpr StyleProp<StyleBox> Padding{StylePropertyId::Padding};
constexpr StyleProp<StyleLength> PaddingTop{StylePropertyId::PaddingTop};
constexpr StyleProp<StyleLength> PaddingRight{StylePropertyId::PaddingRight};
constexpr StyleProp<StyleLength> PaddingBottom{StylePropertyId::PaddingBottom};
constexpr StyleProp<StyleLength> PaddingLeft{StylePropertyId::PaddingLeft};
// The gutters take a length OR a percentage of the element's own content box
// (css-align-3 §8.1), and font-size a length OR a percentage of the PARENT's
// computed size (css-fonts-4 §3.5), so all four carry a unit.
constexpr StyleProp<StyleLength> Gap{StylePropertyId::Gap};
constexpr StyleProp<StyleLength> RowGap{StylePropertyId::RowGap};
constexpr StyleProp<StyleLength> ColumnGap{StylePropertyId::ColumnGap};
constexpr StyleProp<StyleLength> FontSize{StylePropertyId::FontSize};
constexpr StyleProp<float> LetterSpacing{StylePropertyId::LetterSpacing};
constexpr StyleProp<int> FontWeight{StylePropertyId::FontWeight};

// Visual properties (affect paint only)
constexpr StyleProp<bool> Visibility{StylePropertyId::Visibility};
constexpr StyleProp<bool> PointerEvents{StylePropertyId::PointerEvents};
constexpr StyleProp<float> Opacity{StylePropertyId::Opacity};
constexpr StyleProp<Overflow> OverflowProp{StylePropertyId::Overflow};
constexpr StyleProp<Overflow> OverflowXProp{StylePropertyId::OverflowX};
constexpr StyleProp<Overflow> OverflowYProp{StylePropertyId::OverflowY};
constexpr StyleProp<CursorStyle> Cursor{StylePropertyId::Cursor};
constexpr StyleProp<TextAlign> TextAlignProp{StylePropertyId::TextAlign};
constexpr StyleProp<WordBreak> WordBreakProp{StylePropertyId::WordBreak};
constexpr StyleProp<OverflowWrap> OverflowWrapProp{StylePropertyId::OverflowWrap};
constexpr StyleProp<WhiteSpace> WhiteSpaceProp{StylePropertyId::WhiteSpace};
constexpr StyleProp<TextOverflowMode> TextOverflowProp{StylePropertyId::TextOverflow};
constexpr StyleProp<float> LineHeight{StylePropertyId::LineHeight};
constexpr StyleProp<std::vector<std::string>> FontFamily{StylePropertyId::FontFamily};
constexpr StyleProp<FontStyle> FontStyleProp{StylePropertyId::FontStyle};
constexpr StyleProp<FontVariant> FontVariantProp{StylePropertyId::FontVariant};
constexpr StyleProp<uint32_t> Color{StylePropertyId::Color};
constexpr StyleProp<uint32_t> BackgroundColor{StylePropertyId::BackgroundColor};
constexpr StyleProp<BackgroundImageSource> BackgroundImage{StylePropertyId::BackgroundImage};
constexpr StyleProp<BackgroundSizeValue> BackgroundSize{StylePropertyId::BackgroundSize};
constexpr StyleProp<BackgroundPositionValue> BackgroundPosition{StylePropertyId::BackgroundPosition};
constexpr StyleProp<BackgroundRepeat> BackgroundRepeatProp{StylePropertyId::BackgroundRepeat};
constexpr StyleProp<uint32_t> BackgroundTint{StylePropertyId::BackgroundTint};
constexpr StyleProp<float> BackgroundImageSaturation{StylePropertyId::BackgroundImageSaturation};
constexpr StyleProp<BorderImageSliceValue> BorderImageSlice{StylePropertyId::BorderImageSlice};
constexpr StyleProp<BorderColorsTRBL> BorderColor{StylePropertyId::BorderColor};
constexpr StyleProp<uint32_t> BorderTopColor{StylePropertyId::BorderTopColor};
constexpr StyleProp<uint32_t> BorderRightColor{StylePropertyId::BorderRightColor};
constexpr StyleProp<uint32_t> BorderBottomColor{StylePropertyId::BorderBottomColor};
constexpr StyleProp<uint32_t> BorderLeftColor{StylePropertyId::BorderLeftColor};
constexpr StyleProp<Box4> BorderWidth{StylePropertyId::BorderWidth};
constexpr StyleProp<float> BorderTopWidth{StylePropertyId::BorderTopWidth};
constexpr StyleProp<float> BorderRightWidth{StylePropertyId::BorderRightWidth};
constexpr StyleProp<float> BorderBottomWidth{StylePropertyId::BorderBottomWidth};
constexpr StyleProp<float> BorderLeftWidth{StylePropertyId::BorderLeftWidth};
constexpr StyleProp<::GameEngine::BorderStyle> BorderStyleProp{StylePropertyId::BorderStyle};
constexpr StyleProp<CornerRadiiTLTRBRBL> BorderRadius{StylePropertyId::BorderRadius};
// The corner longhands carry a horizontal/vertical pair of unit-bearing
// components (css-backgrounds-3 §5.1 — each a length or a percentage). The
// shorthand above stays pixels-only and circular: CSS never reaches it
// (`border-radius` expands to these four) and its C++ callers author pixels.
constexpr StyleProp<CornerRadiusValue> BorderTopLeftRadius{StylePropertyId::BorderTopLeftRadius};
constexpr StyleProp<CornerRadiusValue> BorderTopRightRadius{StylePropertyId::BorderTopRightRadius};
constexpr StyleProp<CornerRadiusValue> BorderBottomRightRadius{StylePropertyId::BorderBottomRightRadius};
constexpr StyleProp<CornerRadiusValue> BorderBottomLeftRadius{StylePropertyId::BorderBottomLeftRadius};
constexpr StyleProp<float> OutlineWidth{StylePropertyId::OutlineWidth};
constexpr StyleProp<::GameEngine::BorderStyle> OutlineStyleProp{StylePropertyId::OutlineStyle};
constexpr StyleProp<uint32_t> OutlineColor{StylePropertyId::OutlineColor};
constexpr StyleProp<float> OutlineOffset{StylePropertyId::OutlineOffset};
constexpr StyleProp<BoxShadowValue> BoxShadowProp{StylePropertyId::BoxShadow};
constexpr StyleProp<float> TextShadowOffsetX{StylePropertyId::TextShadowOffsetX};
constexpr StyleProp<float> TextShadowOffsetY{StylePropertyId::TextShadowOffsetY};
constexpr StyleProp<float> TextShadowBlur{StylePropertyId::TextShadowBlur};
constexpr StyleProp<uint32_t> TextShadowColor{StylePropertyId::TextShadowColor};
constexpr StyleProp<float> TextGlowRadius{StylePropertyId::TextGlowRadius};
constexpr StyleProp<uint32_t> TextGlowColor{StylePropertyId::TextGlowColor};
constexpr StyleProp<float> TextOutlineWidth{StylePropertyId::TextOutlineWidth};
constexpr StyleProp<uint32_t> TextOutlineColor{StylePropertyId::TextOutlineColor};
constexpr StyleProp<GlowValue> GlowProp{StylePropertyId::Glow};

} // namespace GameEngine::Style
