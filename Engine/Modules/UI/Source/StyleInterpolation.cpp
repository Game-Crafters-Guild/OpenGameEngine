#include "StyleInterpolation.h"

#include "Mathematics/Interpolation.h"

#include <variant>

namespace GameEngine
{

bool IsInterpolable(StylePropertyId id)
{
    switch (id)
    {
    // Float properties
    case StylePropertyId::Opacity:
    case StylePropertyId::FontSize:
    case StylePropertyId::LineHeight:
    case StylePropertyId::LetterSpacing:
    case StylePropertyId::FlexGrow:
    case StylePropertyId::FlexShrink:
    case StylePropertyId::AspectRatio:
    case StylePropertyId::Gap:
    case StylePropertyId::RowGap:
    case StylePropertyId::ColumnGap:
    // Color properties
    case StylePropertyId::BackgroundColor:
    case StylePropertyId::Color:
    case StylePropertyId::BackgroundTint:
    case StylePropertyId::BorderTopColor:
    case StylePropertyId::BorderRightColor:
    case StylePropertyId::BorderBottomColor:
    case StylePropertyId::BorderLeftColor:
    // Length properties
    case StylePropertyId::Width:
    case StylePropertyId::Height:
    case StylePropertyId::MinWidth:
    case StylePropertyId::MinHeight:
    case StylePropertyId::MaxWidth:
    case StylePropertyId::MaxHeight:
    case StylePropertyId::FlexBasis:
    case StylePropertyId::PositionLeft:
    case StylePropertyId::PositionTop:
    case StylePropertyId::PositionRight:
    case StylePropertyId::PositionBottom:
    case StylePropertyId::MarginTop:
    case StylePropertyId::MarginRight:
    case StylePropertyId::MarginBottom:
    case StylePropertyId::MarginLeft:
    case StylePropertyId::PaddingTop:
    case StylePropertyId::PaddingRight:
    case StylePropertyId::PaddingBottom:
    case StylePropertyId::PaddingLeft:
    // Compound float properties
    case StylePropertyId::BorderTopWidth:
    case StylePropertyId::BorderRightWidth:
    case StylePropertyId::BorderBottomWidth:
    case StylePropertyId::BorderLeftWidth:
    case StylePropertyId::BorderTopLeftRadius:
    case StylePropertyId::BorderTopRightRadius:
    case StylePropertyId::BorderBottomRightRadius:
    case StylePropertyId::BorderBottomLeftRadius:
    // Aggregate compound properties
    case StylePropertyId::BorderWidth:
    case StylePropertyId::BorderRadius:
    case StylePropertyId::BorderColor:
    case StylePropertyId::BoxShadow:
    case StylePropertyId::TextShadowOffsetX:
    case StylePropertyId::TextShadowOffsetY:
    case StylePropertyId::TextShadowBlur:
    case StylePropertyId::TextShadowColor:
    case StylePropertyId::TextGlowRadius:
    case StylePropertyId::TextGlowColor:
    case StylePropertyId::TextOutlineWidth:
    case StylePropertyId::TextOutlineColor:
    case StylePropertyId::Glow:
        return true;
    default:
        return false;
    }
}

bool IsTransitionable(StylePropertyId id)
{
    if (IsInterpolable(id))
        return true;

    switch (id)
    {
    case StylePropertyId::Display:
        return true;
    default:
        return false;
    }
}

// A margin edge round-trips through StyleLength, so `auto` has to survive the
// trip: it is the one unit whose transition is discrete rather than smooth
// (CSS Transitions — auto is not an interpolable length), and the discreteness
// falls out of InterpolateProperty's unit-mismatch branch only if the unit is
// actually there.
static StyleValue ReadMarginEdge(bool isAuto, bool isPercent, float value)
{
    if (isAuto)
        return StyleLength::Auto();
    return StyleLength{isPercent ? StyleLength::UnitType::Percent : StyleLength::UnitType::Px, value};
}

// A stored radius corner or gap gutter rebuilt into the unit-bearing value the
// cascade and the transitions exchange.
StyleLength UnitLength(float value, bool isPercent)
{
    return isPercent ? StyleLength::Percent(value) : StyleLength::Px(value);
}

StyleValue ReadProperty(const ResolvedStyle& rs, StylePropertyId id)
{
    switch (id)
    {
    case StylePropertyId::Display:       return rs.Layout.DisplayMode;

    case StylePropertyId::Opacity:       return rs.Visual.LocalOpacity;
    // The computed value of font-size is always an absolute length: the
    // percentage is a SPECIFIED form, resolved before anything can read this.
    // Transitions interpolate computed values, so they never see the percentage.
    case StylePropertyId::FontSize:      return StyleLength::Px(rs.Visual.FontSize);
    case StylePropertyId::LineHeight:    return rs.Visual.LineHeight;
    case StylePropertyId::LetterSpacing: return rs.Visual.LetterSpacing;
    case StylePropertyId::FlexGrow:      return rs.Layout.FlexGrow;
    // A transition interpolates computed values, and the computed value of an
    // undeclared flex-shrink is the CSS initial 1 whatever the container is.
    case StylePropertyId::FlexShrink:    return rs.Layout.FlexShrink.value_or(kCssInitialFlexShrink);
    case StylePropertyId::AspectRatio:   return rs.Layout.AspectRatio;
    case StylePropertyId::Gap:           return UnitLength(rs.Layout.Gap, rs.Layout.GapIsPercent);
    case StylePropertyId::RowGap:        return UnitLength(rs.Layout.RowGap, rs.Layout.RowGapIsPercent);
    case StylePropertyId::ColumnGap:     return UnitLength(rs.Layout.ColumnGap, rs.Layout.ColumnGapIsPercent);

    case StylePropertyId::BackgroundColor: return rs.Visual.BackgroundColor;
    case StylePropertyId::Color:           return rs.Visual.Color;
    case StylePropertyId::BackgroundTint:  return rs.Visual.BackgroundTint;
    case StylePropertyId::BorderTopColor:    return rs.Visual.BorderColor.Top;
    case StylePropertyId::BorderRightColor:  return rs.Visual.BorderColor.Right;
    case StylePropertyId::BorderBottomColor: return rs.Visual.BorderColor.Bottom;
    case StylePropertyId::BorderLeftColor:   return rs.Visual.BorderColor.Left;

    case StylePropertyId::Width:          return rs.Layout.Width;
    case StylePropertyId::Height:         return rs.Layout.Height;
    case StylePropertyId::MinWidth:       return rs.Layout.MinWidth;
    case StylePropertyId::MinHeight:      return rs.Layout.MinHeight;
    case StylePropertyId::MaxWidth:       return rs.Layout.MaxWidth;
    case StylePropertyId::MaxHeight:      return rs.Layout.MaxHeight;
    case StylePropertyId::FlexBasis:      return rs.Layout.FlexBasis;
    case StylePropertyId::PositionLeft:   return rs.Layout.PositionLeft;
    case StylePropertyId::PositionTop:    return rs.Layout.PositionTop;
    case StylePropertyId::PositionRight:  return rs.Layout.PositionRight;
    case StylePropertyId::PositionBottom: return rs.Layout.PositionBottom;

    case StylePropertyId::MarginTop:
        return ReadMarginEdge(rs.Layout.MarginIsAuto.Top, rs.Layout.MarginIsPercent.Top, rs.Layout.Margin.Top);
    case StylePropertyId::MarginRight:
        return ReadMarginEdge(rs.Layout.MarginIsAuto.Right, rs.Layout.MarginIsPercent.Right, rs.Layout.Margin.Right);
    case StylePropertyId::MarginBottom:
        return ReadMarginEdge(rs.Layout.MarginIsAuto.Bottom, rs.Layout.MarginIsPercent.Bottom, rs.Layout.Margin.Bottom);
    case StylePropertyId::MarginLeft:
        return ReadMarginEdge(rs.Layout.MarginIsAuto.Left, rs.Layout.MarginIsPercent.Left, rs.Layout.Margin.Left);
    case StylePropertyId::PaddingTop:
        return StyleLength{rs.Layout.PaddingIsPercent.Top ? StyleLength::UnitType::Percent : StyleLength::UnitType::Px, rs.Layout.Padding.Top};
    case StylePropertyId::PaddingRight:
        return StyleLength{rs.Layout.PaddingIsPercent.Right ? StyleLength::UnitType::Percent : StyleLength::UnitType::Px, rs.Layout.Padding.Right};
    case StylePropertyId::PaddingBottom:
        return StyleLength{rs.Layout.PaddingIsPercent.Bottom ? StyleLength::UnitType::Percent : StyleLength::UnitType::Px, rs.Layout.Padding.Bottom};
    case StylePropertyId::PaddingLeft:
        return StyleLength{rs.Layout.PaddingIsPercent.Left ? StyleLength::UnitType::Percent : StyleLength::UnitType::Px, rs.Layout.Padding.Left};

    case StylePropertyId::BorderTopWidth:          return rs.Layout.BorderWidth.Top;
    case StylePropertyId::BorderRightWidth:        return rs.Layout.BorderWidth.Right;
    case StylePropertyId::BorderBottomWidth:       return rs.Layout.BorderWidth.Bottom;
    case StylePropertyId::BorderLeftWidth:         return rs.Layout.BorderWidth.Left;
    // The units have to travel with the numbers: InterpolateProperty holds an
    // axis whose units differ instead of lerping a percentage and a pixel
    // count together as if they were the same quantity.
    case StylePropertyId::BorderTopLeftRadius:
        return CornerRadiusValue{
            UnitLength(rs.Visual.BorderRadius.TopLeft.X, rs.Visual.BorderRadiusIsPercent.TopLeft.X),
            UnitLength(rs.Visual.BorderRadius.TopLeft.Y, rs.Visual.BorderRadiusIsPercent.TopLeft.Y)};
    case StylePropertyId::BorderTopRightRadius:
        return CornerRadiusValue{
            UnitLength(rs.Visual.BorderRadius.TopRight.X, rs.Visual.BorderRadiusIsPercent.TopRight.X),
            UnitLength(rs.Visual.BorderRadius.TopRight.Y, rs.Visual.BorderRadiusIsPercent.TopRight.Y)};
    case StylePropertyId::BorderBottomRightRadius:
        return CornerRadiusValue{
            UnitLength(rs.Visual.BorderRadius.BottomRight.X, rs.Visual.BorderRadiusIsPercent.BottomRight.X),
            UnitLength(rs.Visual.BorderRadius.BottomRight.Y, rs.Visual.BorderRadiusIsPercent.BottomRight.Y)};
    case StylePropertyId::BorderBottomLeftRadius:
        return CornerRadiusValue{
            UnitLength(rs.Visual.BorderRadius.BottomLeft.X, rs.Visual.BorderRadiusIsPercent.BottomLeft.X),
            UnitLength(rs.Visual.BorderRadius.BottomLeft.Y, rs.Visual.BorderRadiusIsPercent.BottomLeft.Y)};

    case StylePropertyId::BorderWidth:  return rs.Layout.BorderWidth;
    case StylePropertyId::BorderRadius: return rs.Visual.BorderRadius;
    case StylePropertyId::BorderColor:  return rs.Visual.BorderColor;
    case StylePropertyId::BoxShadow:
        return BoxShadowValue{rs.Visual.ShadowOffsetX, rs.Visual.ShadowOffsetY,
                              rs.Visual.ShadowSoftness, rs.Visual.ShadowColor,
                              rs.Visual.ShadowInset};
    case StylePropertyId::TextShadowOffsetX: return rs.Visual.TextEffects.ShadowOffsetX;
    case StylePropertyId::TextShadowOffsetY: return rs.Visual.TextEffects.ShadowOffsetY;
    case StylePropertyId::TextShadowBlur: return rs.Visual.TextEffects.ShadowBlur;
    case StylePropertyId::TextShadowColor: return rs.Visual.TextEffects.ShadowColor;
    case StylePropertyId::TextGlowRadius: return rs.Visual.TextEffects.GlowRadius;
    case StylePropertyId::TextGlowColor: return rs.Visual.TextEffects.GlowColor;
    case StylePropertyId::TextOutlineWidth: return rs.Visual.TextEffects.OutlineWidth;
    case StylePropertyId::TextOutlineColor: return rs.Visual.TextEffects.OutlineColor;
    case StylePropertyId::Glow:
        return GlowValue{rs.Visual.GlowRadius, rs.Visual.GlowColor};

    default:
        return std::monostate{};
    }
}

void WriteProperty(ResolvedStyle& rs, StylePropertyId id, const StyleValue& v)
{
    switch (id)
    {
    case StylePropertyId::Display:
        rs.Layout.DisplayMode = std::get<DisplayMode>(v);
        break;

    case StylePropertyId::Opacity:
        rs.Visual.LocalOpacity = std::get<float>(v);
        rs.Visual.Opacity = rs.Visual.LocalOpacity;
        break;
    // Write-back bypasses the cascade with a value that is already computed, so
    // it lands as a length and clears any percentage the cascade parked — which
    // ResolveFontSize would otherwise recompute over the animated value.
    case StylePropertyId::FontSize:
        rs.Visual.FontSize = std::get<StyleLength>(v).Value;
        rs.Visual.FontSizePercent.reset();
        break;
    case StylePropertyId::LineHeight:  rs.Visual.LineHeight  = std::get<float>(v); break;
    case StylePropertyId::LetterSpacing: rs.Visual.LetterSpacing = std::get<float>(v); break;
    case StylePropertyId::FlexGrow:    rs.Layout.FlexGrow    = std::get<float>(v); break;
    case StylePropertyId::FlexShrink:  rs.Layout.FlexShrink  = std::get<float>(v); break;
    case StylePropertyId::AspectRatio: rs.Layout.AspectRatio = std::get<float>(v); break;
    case StylePropertyId::Gap:
    {
        const auto& len = std::get<StyleLength>(v);
        SetGapGutter(rs.Layout.Gap, rs.Layout.GapIsPercent, len);
        SetGapGutter(rs.Layout.RowGap, rs.Layout.RowGapIsPercent, len);
        SetGapGutter(rs.Layout.ColumnGap, rs.Layout.ColumnGapIsPercent, len);
        break;
    }
    case StylePropertyId::RowGap:
        SetGapGutter(rs.Layout.RowGap, rs.Layout.RowGapIsPercent, std::get<StyleLength>(v));
        break;
    case StylePropertyId::ColumnGap:
        SetGapGutter(rs.Layout.ColumnGap, rs.Layout.ColumnGapIsPercent, std::get<StyleLength>(v));
        break;

    case StylePropertyId::BackgroundColor: rs.Visual.BackgroundColor = std::get<uint32_t>(v); break;
    case StylePropertyId::Color:           rs.Visual.Color           = std::get<uint32_t>(v); break;
    case StylePropertyId::BackgroundTint:  rs.Visual.BackgroundTint  = std::get<uint32_t>(v); break;
    case StylePropertyId::BorderTopColor:    rs.Visual.BorderColor.Top    = std::get<uint32_t>(v); break;
    case StylePropertyId::BorderRightColor:  rs.Visual.BorderColor.Right  = std::get<uint32_t>(v); break;
    case StylePropertyId::BorderBottomColor: rs.Visual.BorderColor.Bottom = std::get<uint32_t>(v); break;
    case StylePropertyId::BorderLeftColor:   rs.Visual.BorderColor.Left   = std::get<uint32_t>(v); break;

    case StylePropertyId::Width:          rs.Layout.Width          = std::get<StyleLength>(v); break;
    case StylePropertyId::Height:         rs.Layout.Height         = std::get<StyleLength>(v); break;
    case StylePropertyId::MinWidth:       rs.Layout.MinWidth       = std::get<StyleLength>(v); break;
    case StylePropertyId::MinHeight:      rs.Layout.MinHeight      = std::get<StyleLength>(v); break;
    case StylePropertyId::MaxWidth:       rs.Layout.MaxWidth       = std::get<StyleLength>(v); break;
    case StylePropertyId::MaxHeight:      rs.Layout.MaxHeight      = std::get<StyleLength>(v); break;
    case StylePropertyId::FlexBasis:      rs.Layout.FlexBasis      = std::get<StyleLength>(v); break;
    case StylePropertyId::PositionLeft:   rs.Layout.PositionLeft   = std::get<StyleLength>(v); break;
    case StylePropertyId::PositionTop:    rs.Layout.PositionTop    = std::get<StyleLength>(v); break;
    case StylePropertyId::PositionRight:  rs.Layout.PositionRight  = std::get<StyleLength>(v); break;
    case StylePropertyId::PositionBottom: rs.Layout.PositionBottom = std::get<StyleLength>(v); break;

    case StylePropertyId::MarginTop:
        SetMarginEdge(rs.Layout.Margin.Top, rs.Layout.MarginIsPercent.Top,
                        rs.Layout.MarginIsAuto.Top, std::get<StyleLength>(v));
        break;
    case StylePropertyId::MarginRight:
        SetMarginEdge(rs.Layout.Margin.Right, rs.Layout.MarginIsPercent.Right,
                        rs.Layout.MarginIsAuto.Right, std::get<StyleLength>(v));
        break;
    case StylePropertyId::MarginBottom:
        SetMarginEdge(rs.Layout.Margin.Bottom, rs.Layout.MarginIsPercent.Bottom,
                        rs.Layout.MarginIsAuto.Bottom, std::get<StyleLength>(v));
        break;
    case StylePropertyId::MarginLeft:
        SetMarginEdge(rs.Layout.Margin.Left, rs.Layout.MarginIsPercent.Left,
                        rs.Layout.MarginIsAuto.Left, std::get<StyleLength>(v));
        break;
    case StylePropertyId::PaddingTop:
    {
        const auto& len = std::get<StyleLength>(v);
        rs.Layout.Padding.Top = len.Value;
        rs.Layout.PaddingIsPercent.Top = len.IsPercent();
        break;
    }
    case StylePropertyId::PaddingRight:
    {
        const auto& len = std::get<StyleLength>(v);
        rs.Layout.Padding.Right = len.Value;
        rs.Layout.PaddingIsPercent.Right = len.IsPercent();
        break;
    }
    case StylePropertyId::PaddingBottom:
    {
        const auto& len = std::get<StyleLength>(v);
        rs.Layout.Padding.Bottom = len.Value;
        rs.Layout.PaddingIsPercent.Bottom = len.IsPercent();
        break;
    }
    case StylePropertyId::PaddingLeft:
    {
        const auto& len = std::get<StyleLength>(v);
        rs.Layout.Padding.Left = len.Value;
        rs.Layout.PaddingIsPercent.Left = len.IsPercent();
        break;
    }

    case StylePropertyId::BorderTopWidth:          rs.Layout.BorderWidth.Top    = std::get<float>(v); break;
    case StylePropertyId::BorderRightWidth:        rs.Layout.BorderWidth.Right  = std::get<float>(v); break;
    case StylePropertyId::BorderBottomWidth:       rs.Layout.BorderWidth.Bottom = std::get<float>(v); break;
    case StylePropertyId::BorderLeftWidth:         rs.Layout.BorderWidth.Left   = std::get<float>(v); break;
    case StylePropertyId::BorderTopLeftRadius:
        SetBorderRadiusCorner(rs.Visual.BorderRadius.TopLeft,
                              rs.Visual.BorderRadiusIsPercent.TopLeft, std::get<CornerRadiusValue>(v));
        break;
    case StylePropertyId::BorderTopRightRadius:
        SetBorderRadiusCorner(rs.Visual.BorderRadius.TopRight,
                              rs.Visual.BorderRadiusIsPercent.TopRight, std::get<CornerRadiusValue>(v));
        break;
    case StylePropertyId::BorderBottomRightRadius:
        SetBorderRadiusCorner(rs.Visual.BorderRadius.BottomRight,
                              rs.Visual.BorderRadiusIsPercent.BottomRight, std::get<CornerRadiusValue>(v));
        break;
    case StylePropertyId::BorderBottomLeftRadius:
        SetBorderRadiusCorner(rs.Visual.BorderRadius.BottomLeft,
                              rs.Visual.BorderRadiusIsPercent.BottomLeft, std::get<CornerRadiusValue>(v));
        break;

    case StylePropertyId::BorderWidth:  rs.Layout.BorderWidth  = std::get<Box4>(v); break;
    // Pixels-only shorthand; clears the flags for the same reason the appliers do.
    case StylePropertyId::BorderRadius:
        rs.Visual.BorderRadius = std::get<CornerRadiiTLTRBRBL>(v);
        rs.Visual.BorderRadiusIsPercent = {};
        break;
    case StylePropertyId::BorderColor:  rs.Visual.BorderColor  = std::get<BorderColorsTRBL>(v); break;
    case StylePropertyId::BoxShadow:
    {
        const auto& sv = std::get<BoxShadowValue>(v);
        rs.Visual.ShadowOffsetX  = sv.OffsetX;
        rs.Visual.ShadowOffsetY  = sv.OffsetY;
        rs.Visual.ShadowSoftness = sv.Blur;
        rs.Visual.ShadowColor    = sv.Color;
        rs.Visual.ShadowInset    = sv.Inset;
        break;
    }
    case StylePropertyId::TextShadowOffsetX: rs.Visual.TextEffects.ShadowOffsetX = std::get<float>(v); break;
    case StylePropertyId::TextShadowOffsetY: rs.Visual.TextEffects.ShadowOffsetY = std::get<float>(v); break;
    case StylePropertyId::TextShadowBlur: rs.Visual.TextEffects.ShadowBlur = std::get<float>(v); break;
    case StylePropertyId::TextShadowColor: rs.Visual.TextEffects.ShadowColor = std::get<uint32_t>(v); break;
    case StylePropertyId::TextGlowRadius: rs.Visual.TextEffects.GlowRadius = std::get<float>(v); break;
    case StylePropertyId::TextGlowColor: rs.Visual.TextEffects.GlowColor = std::get<uint32_t>(v); break;
    case StylePropertyId::TextOutlineWidth: rs.Visual.TextEffects.OutlineWidth = std::get<float>(v); break;
    case StylePropertyId::TextOutlineColor: rs.Visual.TextEffects.OutlineColor = std::get<uint32_t>(v); break;
    case StylePropertyId::Glow:
    {
        const auto& gv = std::get<GlowValue>(v);
        rs.Visual.GlowRadius = gv.Radius;
        rs.Visual.GlowColor  = gv.Color;
        break;
    }
    default:
        break;
    }
}

StyleValue InterpolateProperty(StylePropertyId /*id*/, const StyleValue& a, const StyleValue& b, float t)
{
    if (std::holds_alternative<float>(a) && std::holds_alternative<float>(b))
    {
        return Math::Lerp(std::get<float>(a), std::get<float>(b), t);
    }

    if (std::holds_alternative<uint32_t>(a) && std::holds_alternative<uint32_t>(b))
    {
        return Math::LerpColorARGB(std::get<uint32_t>(a), std::get<uint32_t>(b), t);
    }

    if (std::holds_alternative<StyleLength>(a) && std::holds_alternative<StyleLength>(b))
    {
        const auto& la = std::get<StyleLength>(a);
        const auto& lb = std::get<StyleLength>(b);
        if (la.Unit != lb.Unit)
            return (t < 0.5f) ? a : b;
        return StyleLength{la.Unit, Math::Lerp(la.Value, lb.Value, t)};
    }

    if (std::holds_alternative<Box4>(a) && std::holds_alternative<Box4>(b))
    {
        const auto& ba = std::get<Box4>(a);
        const auto& bb = std::get<Box4>(b);
        return Box4{
            Math::Lerp(ba.Top, bb.Top, t),
            Math::Lerp(ba.Right, bb.Right, t),
            Math::Lerp(ba.Bottom, bb.Bottom, t),
            Math::Lerp(ba.Left, bb.Left, t)};
    }

    if (std::holds_alternative<CornerRadiiTLTRBRBL>(a) && std::holds_alternative<CornerRadiiTLTRBRBL>(b))
    {
        const auto& ca = std::get<CornerRadiiTLTRBRBL>(a);
        const auto& cb = std::get<CornerRadiiTLTRBRBL>(b);
        const auto corner = [t](const CornerRadius& ra, const CornerRadius& rb) {
            return CornerRadius{Math::Lerp(ra.X, rb.X, t), Math::Lerp(ra.Y, rb.Y, t)};
        };
        CornerRadiiTLTRBRBL out{};
        out.TopLeft = corner(ca.TopLeft, cb.TopLeft);
        out.TopRight = corner(ca.TopRight, cb.TopRight);
        out.BottomRight = corner(ca.BottomRight, cb.BottomRight);
        out.BottomLeft = corner(ca.BottomLeft, cb.BottomLeft);
        return out;
    }

    // A corner's two axes interpolate independently; an axis whose units differ
    // flips discretely at the midpoint, exactly like the StyleLength arm above.
    if (std::holds_alternative<CornerRadiusValue>(a) && std::holds_alternative<CornerRadiusValue>(b))
    {
        const auto& ca = std::get<CornerRadiusValue>(a);
        const auto& cb = std::get<CornerRadiusValue>(b);
        const auto axis = [t](const StyleLength& la, const StyleLength& lb) {
            if (la.Unit != lb.Unit)
                return (t < 0.5f) ? la : lb;
            return StyleLength{la.Unit, Math::Lerp(la.Value, lb.Value, t)};
        };
        return CornerRadiusValue{axis(ca.Horizontal, cb.Horizontal), axis(ca.Vertical, cb.Vertical)};
    }

    if (std::holds_alternative<BorderColorsTRBL>(a) && std::holds_alternative<BorderColorsTRBL>(b))
    {
        const auto& ca = std::get<BorderColorsTRBL>(a);
        const auto& cb = std::get<BorderColorsTRBL>(b);
        return BorderColorsTRBL{
            Math::LerpColorARGB(ca.Top, cb.Top, t),
            Math::LerpColorARGB(ca.Right, cb.Right, t),
            Math::LerpColorARGB(ca.Bottom, cb.Bottom, t),
            Math::LerpColorARGB(ca.Left, cb.Left, t)};
    }

    if (std::holds_alternative<BoxShadowValue>(a) && std::holds_alternative<BoxShadowValue>(b))
    {
        const auto& sa = std::get<BoxShadowValue>(a);
        const auto& sb = std::get<BoxShadowValue>(b);
        return BoxShadowValue{
            Math::Lerp(sa.OffsetX, sb.OffsetX, t),
            Math::Lerp(sa.OffsetY, sb.OffsetY, t),
            Math::Lerp(sa.Blur, sb.Blur, t),
            Math::LerpColorARGB(sa.Color, sb.Color, t),
            (t < 0.5f) ? sa.Inset : sb.Inset};
    }

    if (std::holds_alternative<GlowValue>(a) && std::holds_alternative<GlowValue>(b))
    {
        const auto& ga = std::get<GlowValue>(a);
        const auto& gb = std::get<GlowValue>(b);
        return GlowValue{
            Math::Lerp(ga.Radius, gb.Radius, t),
            Math::LerpColorARGB(ga.Color, gb.Color, t)};
    }

    return (t < 0.5f) ? a : b;
}

} // namespace GameEngine
