#pragma once

#include "UI/TransitionSpec.h"
#include "UI/UIStyle.h"
#include "UI/TextEffects.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace GameEngine
{

// CSS initial value of flex-shrink (CSS Flexbox 1 §7.2): what an undeclared
// flex-shrink resolves to inside a flex container.
inline constexpr float kCssInitialFlexShrink = 1.0f;

// The engine's initial font-size, in CSS logical px. Doubles as the base a
// percentage font-size resolves against on an element with no parent: CSS
// resolves `font-size: <percentage>` against the PARENT's computed font size
// (css-fonts-4 §3.5), and where there is no parent the initial value stands in
// (css-cascade-4 §7.2).
inline constexpr float kInitialFontSizePx = 18.0f;

// Inputs to Yoga layout. Evolved from LayoutStyle — same fields, with operator==
// for dirty detection. Does NOT contain FontSize/FontWeight (those live in
// VisualStyle; the Yoga measure callback accesses them via the element).
struct LayoutInputs
{
    DisplayMode DisplayMode = GameEngine::DisplayMode::Block;
    FlexDirection FlexDirection = GameEngine::FlexDirection::Row;
    bool HasFlexDirection = false;
    AlignItems AlignItems = GameEngine::AlignItems::Stretch;
    JustifyContent JustifyContent = GameEngine::JustifyContent::FlexStart;
    AlignContent AlignContent = GameEngine::AlignContent::Stretch;
    GameEngine::AlignItems AlignSelf = GameEngine::AlignItems::Auto;
    Direction Direction = GameEngine::Direction::LTR;
    PositionType PositionType = GameEngine::PositionType::Relative;
    StyleLength PositionLeft{};
    StyleLength PositionTop{};
    StyleLength PositionRight{};
    StyleLength PositionBottom{};
    int Order = 0;
    int ZIndex = 0;
    bool FlexWrap = false;
    float FlexGrow = 0.0f;
    // Empty when no declaration reached this element. The distinction is load
    // bearing: a child of a block-flow container defaults to 0 (block layout
    // never shrinks its children) while an authored value still wins, and a
    // plain float cannot tell an authored 1 from the CSS initial 1. Optional
    // rather than a companion `HasFlexShrink` flag so a value can never be set
    // without also marking it declared.
    std::optional<float> FlexShrink{};
    StyleLength FlexBasis{};
    StyleLength Width{};
    StyleLength Height{};
    StyleLength MinWidth{};
    StyleLength MinHeight{};
    StyleLength MaxWidth{};
    StyleLength MaxHeight{};
    float AspectRatio = 0.0f;
    Box4 Margin{};
    Box4b MarginIsPercent{};
    // `margin: auto` is not a length — the edge absorbs the flex line's
    // remaining free space, and on the main axis it pre-empts justify-content
    // entirely (CSS Flexbox 1 §9.5). The Auto unit therefore cannot collapse
    // into Margin: an auto edge and a 0px edge carry the same float and only
    // this flag separates them. Margin holds 0 on an auto edge.
    Box4b MarginIsAuto{};
    Box4 Padding{};
    Box4b PaddingIsPercent{};
    // Part of the box model, so Yoga owns it: the border insets the content box
    // exactly like padding does, and lives here (not in VisualStyle) so a border
    // change trips the LayoutInputs equality that gates the Yoga re-push.
    Box4 BorderWidth{};
    // A gutter whose flag is set holds a bare percentage NUMBER rather than
    // pixels — the style INPUT, exactly as Padding does under PaddingIsPercent.
    // Yoga resolves it against the element's own content box in the gutter's
    // axis (css-align-3 §8.1), so the unit has to reach YGNodeStyleSetGap*.
    // Gap/GapIsPercent mirror the shorthand: CSS fans it out to both gutters,
    // and it exists as its own field so a `transition: gap` has one value to
    // read.
    float Gap = 0.0f;
    float RowGap = 0.0f;
    float ColumnGap = 0.0f;
    bool GapIsPercent = false;
    bool RowGapIsPercent = false;
    bool ColumnGapIsPercent = false;
    Overflow Overflow = GameEngine::Overflow::Visible;
    GameEngine::Overflow OverflowX = GameEngine::Overflow::Visible;
    GameEngine::Overflow OverflowY = GameEngine::Overflow::Visible;

    bool operator==(const LayoutInputs&) const = default;
};

// The one definition of how a parsed length lands on a margin edge, shared by
// the stylesheet cascade, the inline-override applier and the transition
// write-back. All three outputs are written every time: the flags are per-edge
// state that outlives a single declaration, so a later length has to clear
// whichever flag an earlier one raised.
inline void SetMarginEdge(float& outValue, bool& outIsPercent, bool& outIsAuto,
                          const StyleLength& len)
{
    outIsAuto = len.IsAuto();
    outIsPercent = len.IsPercent();
    outValue = outIsAuto ? 0.0f : len.Value;
}

// The one definition of how a parsed corner value lands on a border-radius
// corner, shared by the stylesheet cascade, the inline-override applier and the
// transition write-back. All outputs are written every time: the flags are
// per-axis state that outlives a single declaration, so a later pixel value
// has to clear whichever flag an earlier percentage raised. A negative radius
// is invalid CSS and floors at zero; `auto` is not a radius and lands as 0px.
inline void SetBorderRadiusCorner(CornerRadius& outValue, CornerAxisFlags& outIsPercent,
                                  const CornerRadiusValue& value)
{
    outIsPercent.X = value.Horizontal.IsPercent();
    outIsPercent.Y = value.Vertical.IsPercent();
    outValue.X = value.Horizontal.IsAuto() ? 0.0f : std::max(0.0f, value.Horizontal.Value);
    outValue.Y = value.Vertical.IsAuto() ? 0.0f : std::max(0.0f, value.Vertical.Value);
}

// The one definition of how a parsed length lands on a gap gutter, shared by the
// stylesheet cascade, the inline-override applier and the transition write-back.
// Both outputs are written every time: the flag is per-gutter state that
// outlives a single declaration, so a later pixel value has to clear whichever
// flag an earlier percentage raised. A negative gutter is invalid CSS and floors
// at zero; `auto` is not a gap and lands as 0px.
inline void SetGapGutter(float& outValue, bool& outIsPercent, const StyleLength& len)
{
    outIsPercent = len.IsPercent();
    outValue = len.IsAuto() ? 0.0f : std::max(0.0f, len.Value);
}

// Inputs to the renderer. Merges PaintStyle + TypographyStyle. Also contains
// typography fields that affect text measurement (FontSize, FontWeight, etc.)
// even though those also affect layout — dirty classification handles that
// via the canonical GetStylePropertyImpact table, and the Yoga measure
// callback reads them through the element.
struct VisualStyle
{
    // Background
    uint32_t BackgroundColor = 0x00000000u;
    BackgroundImageStyle BackgroundImage{};
    uint32_t BackgroundTint = 0xFFFFFFFFu;

    // Border — widths live in LayoutInputs (they are box-model, not paint-only).
    BorderStyle BorderStyle = GameEngine::BorderStyle::Solid;
    CornerRadiiTLTRBRBL BorderRadius{};
    // A corner whose flag is set holds a bare percentage NUMBER in BorderRadius
    // rather than pixels — the style INPUT, exactly as Padding does under
    // PaddingIsPercent. UsedBorderRadius resolves it against the border box;
    // every consumer of a radius must go through that, or it contributes "10"
    // where the box asked for 10% of its own width.
    CornerFlagsTLTRBRBL BorderRadiusIsPercent{};
    BorderColorsTRBL BorderColor{};

    // Outline — a ring painted outside the border edge. Not part of the box
    // model: it never reaches Yoga and never displaces a sibling, so the
    // ring can overlap whatever is next to the element. Widths and the
    // offset are CSS logical px, like every other length here.
    // Initial values are CSS's: `medium` width, `none` style (so nothing
    // paints until a stylesheet asks for it), and currentColor — which is
    // what HasOutlineColor == false selects at paint time.
    float OutlineWidth = kMediumLineWidthPx;
    float OutlineOffset = 0.0f;
    ::GameEngine::BorderStyle OutlineStyle = ::GameEngine::BorderStyle::None;
    uint32_t OutlineColor = 0x00000000u;
    bool HasOutlineColor = false;

    // Typography (inherited properties)
    // FontSize is the COMPUTED value and is always an absolute length in CSS
    // logical px (css-fonts-4 §3.5) — every consumer can read it directly.
    // FontSizePercent holds the SPECIFIED percentage when the declaration was
    // one; it is what makes the computation idempotent, because the resolve walk
    // runs many times over the same style and recomputing from the percentage
    // cannot compound the way rewriting FontSize in place would.
    float FontSize = kInitialFontSizePx;
    std::optional<float> FontSizePercent{};
    float LineHeight = 0.0f;
    // CSS px added after every typographic cluster, the run's last one
    // included; 0 is the `normal` keyword. Negative tightens.
    float LetterSpacing = 0.0f;
    std::shared_ptr<const std::vector<std::string>> FontFamily;
    int FontWeight = 400;
    FontStyle FontStyle = GameEngine::FontStyle::Normal;
    FontVariant FontVariant = GameEngine::FontVariant::Normal;
    uint32_t Color = 0xFF000000u;
    TextAlign TextAlign = GameEngine::TextAlign::Left;
    WordBreak WordBreak = GameEngine::WordBreak::Normal;
    OverflowWrap OverflowWrap = GameEngine::OverflowWrap::Normal;
    WhiteSpace WhiteSpace = GameEngine::WhiteSpace::Normal;
    TextOverflowMode TextOverflow = TextOverflowMode::Clip;
    // `selection-color` (ARGB, inherited): the colour every text control fills
    // selected text with, scaled by UI::PackedTextSelectionFill. The initial
    // value is what a UI with no theme selects in; the editor theme sets the
    // accent on :root.
    uint32_t SelectionColor = 0xFF3A8FFFu;

    // Effects (for future SDF renderer — Phase 6 enables via CSS)
    float GlowRadius = 0.0f;
    uint32_t GlowColor = 0x00000000u;
    float ShadowOffsetX = 0.0f;
    float ShadowOffsetY = 0.0f;
    float ShadowSoftness = 0.0f;
    uint32_t ShadowColor = 0x00000000u;
    bool ShadowInset = false;

    UI::TextEffects TextEffects;

    // Interaction / visibility
    float LocalOpacity = 1.0f;
    float Opacity = 1.0f;
    bool Visible = true;
    bool PointerEvents = true;
    CursorStyle Cursor = CursorStyle::Auto;

    // When false, end-of-frame ResolveStyles() may sync visibility/pointer-events from the
    // parent (mirrors CSS inheritance) so descendants stay aligned with runtime overrides.
    bool HasVisibility = false;
    bool HasPointerEvents = false;

    // "has" flags for inheritance tracking — when false, inherit from parent
    bool HasColor = false;
    bool HasFontSize = false;
    bool HasFontFamily = false;
    bool HasFontWeight = false;
    bool HasFontStyle = false;
    bool HasFontVariant = false;
    bool HasTextAlign = false;
    bool HasWordBreak = false;
    bool HasOverflowWrap = false;
    bool HasWhiteSpace = false;
    bool HasLineHeight = false;
    bool HasLetterSpacing = false;
    bool HasCursor = false;
};

// The one definition of how a parsed font-size lands on a style, shared by the
// stylesheet cascade and the inline-override applier. Both outputs are written
// every time: a later pixel value has to clear whichever percentage an earlier
// declaration stored, or the resolve walk would keep recomputing from it.
// A pixel value computes immediately; a percentage cannot, because its base is
// the parent's computed size, so it parks in FontSizePercent and ResolveFontSize
// finishes the job once the parent is known. `auto` is not a font-size.
inline void SetFontSize(VisualStyle& vs, const StyleLength& len)
{
    if (len.IsAuto())
        return;
    vs.HasFontSize = true;
    if (len.IsPercent())
    {
        vs.FontSizePercent = std::max(0.0f, len.Value);
    }
    else
    {
        vs.FontSizePercent.reset();
        vs.FontSize = std::max(0.0f, len.Value);
    }
}

// Turns the specified font-size into the computed one, given the parent's
// computed size — the two cases CSS defines against that base: an undeclared
// font-size inherits it, and a declared percentage scales it (css-fonts-4 §3.5).
// A declared length ignores it. Idempotent, so the resolve walk may run over the
// same element as often as it likes; callers on the root pass kInitialFontSizePx.
inline void ResolveFontSize(VisualStyle& vs, float parentComputedFontSizePx)
{
    if (!vs.HasFontSize)
        vs.FontSize = parentComputedFontSizePx;
    else if (vs.FontSizePercent)
        vs.FontSize = std::max(0.0f, parentComputedFontSizePx * *vs.FontSizePercent * 0.01f);
}

// The USED border radii, in the same CSS-logical px as the border box handed in
// — percentage axes resolved, pixel axes passed through untouched. Every
// consumer of VisualStyle::BorderRadius goes through this; reading the field
// directly gets the style input, which is a bare percentage number on a
// percentage axis.
//
// css-backgrounds-3 §5.1: a percentage horizontal radius resolves against the
// box's width, a vertical one against its height — so `border-radius: 50%` on
// a non-square box names an ELLIPSE, and that ellipse is what comes back here.
//
// §5.2 overlap reduction: when the radii along any side sum past that side's
// length, ALL radii scale by the single factor f that makes the tightest side
// fit — the browser rule, so adjacent corners shrink together and keep their
// shape.
//
// DEVIATION FROM SPEC: each axis then clamps to half the box. The quadrant
// rounded-rect SDF (roundedRectSDF) evaluates one corner per quadrant, so a
// radius past the box midline — legal in CSS when the opposite corner is
// smaller, e.g. `border-radius: 0 100% 0 0` — cannot be painted and lands on
// the half-box arc instead. Uniform radii are unaffected (f already fits them).
inline CornerRadiiTLTRBRBL UsedBorderRadius(const VisualStyle& vs,
                                            float borderBoxWidth, float borderBoxHeight)
{
    const float w = std::max(0.0f, borderBoxWidth);
    const float h = std::max(0.0f, borderBoxHeight);
    const auto used = [](float value, bool isPercent, float basis) {
        return std::max(0.0f, isPercent ? value * 0.01f * basis : value);
    };
    const auto corner = [&](const CornerRadius& r, const CornerAxisFlags& pct) {
        return CornerRadius{used(r.X, pct.X, w), used(r.Y, pct.Y, h)};
    };
    CornerRadiiTLTRBRBL out{};
    out.TopLeft = corner(vs.BorderRadius.TopLeft, vs.BorderRadiusIsPercent.TopLeft);
    out.TopRight = corner(vs.BorderRadius.TopRight, vs.BorderRadiusIsPercent.TopRight);
    out.BottomRight = corner(vs.BorderRadius.BottomRight, vs.BorderRadiusIsPercent.BottomRight);
    out.BottomLeft = corner(vs.BorderRadius.BottomLeft, vs.BorderRadiusIsPercent.BottomLeft);

    float f = 1.0f;
    const auto tighten = [&f](float side, float sum) {
        if (sum > side)
            f = std::min(f, side / sum); // sum > side >= 0, so sum > 0
    };
    tighten(w, out.TopLeft.X + out.TopRight.X);
    tighten(w, out.BottomLeft.X + out.BottomRight.X);
    tighten(h, out.TopLeft.Y + out.BottomLeft.Y);
    tighten(h, out.TopRight.Y + out.BottomRight.Y);

    const float halfW = w * 0.5f;
    const float halfH = h * 0.5f;
    const auto fit = [&](CornerRadius& r) {
        r.X = std::min(r.X * f, halfW);
        r.Y = std::min(r.Y * f, halfH);
    };
    fit(out.TopLeft);
    fit(out.TopRight);
    fit(out.BottomRight);
    fit(out.BottomLeft);
    return out;
}

// Full resolved style for an element — one instance per element per frame.
// Composed of LayoutInputs + VisualStyle (no inheritance diamond).
struct ResolvedStyle
{
    LayoutInputs Layout;
    VisualStyle Visual;
    TransitionSpec Transitions;

    // Resolved custom property scope (inheritance chain for var() lookups)
    std::shared_ptr<const CustomPropertyScope> CustomScope;

    // Typed custom property access (forwarded to CustomScope)
    std::optional<float> GetCustomNumber(StringId name) const;
    std::optional<std::string> GetCustomString(StringId name) const;
    std::optional<uint32_t> GetCustomColor(StringId name) const;
    std::optional<StyleLength> GetCustomLength(StringId name) const;
};

} // namespace GameEngine
